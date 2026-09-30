#include "AntigravityAgent.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

AntigravityAgent::AntigravityAgent(const QString &program, const QString &workingDirectory, QObject *parent)
    : AgentBackend(parent), program_(program), workingDirectory_(workingDirectory), process_(new QProcess(this))
{
    connect(process_, &QProcess::readyReadStandardOutput, this, &AntigravityAgent::drainOutput);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QString details = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!details.isEmpty()) {
            diagnostics_ = details;
            emit message("[Antigravity] " + details);
        }
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (interrupted_ && processError == QProcess::Crashed) return;
        errorDetails_ = process_->errorString();
        emit message("[Antigravity] Antigravity CLI process error: " + errorDetails_ + " (executable: " + program_ + ")");
        if (processError == QProcess::FailedToStart) finish(-1);
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { finish(code); });
}

AntigravityAgent::~AntigravityAgent()
{
    disconnect(process_, nullptr, this, nullptr);
    if (isRunning()) {
        process_->terminate();
        if (!process_->waitForFinished(1000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
}

bool AntigravityAgent::isRunning() const { return process_->state() != QProcess::NotRunning; }

QString AntigravityAgent::statusText() const
{
    if (busy_) return "Antigravity is responding…";
    if (QStandardPaths::findExecutable(program_).isEmpty()) return "Antigravity CLI is not installed or is not in PATH";
    return "Antigravity ready";
}

bool AntigravityAgent::newConversation(const QString &workingDirectory)
{
    if (busy_) {
        emit message("[Wait for Antigravity to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    conversationId_.clear();
    queuedPrompts_.clear();
    return true;
}

bool AntigravityAgent::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (id.isEmpty() || !QFileInfo(workingDirectory).isDir()) return false;
    if (busy_) {
        emit message("[Wait for Antigravity to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    conversationId_ = id;
    queuedPrompts_.clear();
    emit message("[Resuming Antigravity conversation: " + id + "]");
    emit stateChanged();
    return true;
}

bool AntigravityAgent::prompt(const QString &text)
{
    queuedPrompts_.append(text);
    sendNextPrompt();
    return true;
}

void AntigravityAgent::interrupt()
{
    if (!busy_ || stopRequested_ || !isRunning()) return;
    stopRequested_ = true;
    interrupted_ = true;
    process_->terminate();
    QTimer::singleShot(1000, process_, [this] { if (isRunning()) process_->kill(); });
    emit stateChanged();
}

void AntigravityAgent::loadHistory(const QString &id, const QString &, bool)
{
    emit historyLoaded(id, {}, false, "History preview is not available for Antigravity conversations.");
}

void AntigravityAgent::sendNextPrompt()
{
    if (busy_ || isRunning() || queuedPrompts_.isEmpty()) return;
    busy_ = true;
    stopRequested_ = false;
    buffer_.clear();
    errorDetails_.clear();
    diagnostics_.clear();
    interrupted_ = false;
    resultSeen_ = false;
    textSeen_ = false;
    completionSent_ = false;
    QStringList arguments{"--output-format", "stream-json"};
    if (!conversationId_.isEmpty()) arguments << "--conversation" << conversationId_;
    arguments << "--prompt" << queuedPrompts_.takeFirst();
    process_->setWorkingDirectory(workingDirectory_);
    process_->start(program_, arguments);
    emit stateChanged();
}

void AntigravityAgent::handleLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        emit message("[Antigravity] Invalid Antigravity CLI response: " + parseError.errorString());
        return;
    }
    const auto text = [this](const QString &delta) {
        if (!textSeen_) {
            textSeen_ = true;
            emit messageStarted();
        }
        emit messageDelta(delta);
    };
    const QJsonObject event = document.object();
    const QString type = event.value("event").toString();
    if (type == "init") {
        const QString id = event.value("conversation_id").toString();
        if (!id.isEmpty()) {
            conversationId_ = id;
            emit conversationOpened(id, false);
        }
    } else if (type == "step_update") {
        const QJsonObject step = event.value("step_update").toObject();
        if (step.value("step_type").toString() == "agent_response") {
            const QString delta = step.value("text_delta").toString();
            if (!delta.isEmpty()) text(delta);
        } else if (step.value("step_type").toString() == "tool" && step.value("state").toString() == "ACTIVE") {
            const QJsonObject tool = step.value("tool_info").toObject();
            emit toolStarted(step.value("tool_name").toString(tool.value("name").toString()),
                             QString::fromUtf8(QJsonDocument(tool.value("parameters").toObject()).toJson(QJsonDocument::Compact)));
        }
    } else if (type == "result") {
        resultSeen_ = true;
        const QJsonObject result = event.value("result").toObject();
        const QString id = result.value("conversation_id").toString();
        if (!id.isEmpty() && id != conversationId_) {
            conversationId_ = id;
            emit conversationOpened(id, false);
        }
        if (!textSeen_ && !result.value("response").toString().isEmpty()) text(result.value("response").toString());
        if (result.value("status").toString() != "SUCCESS") {
            errorDetails_ = result.value("error").toString(result.value("status").toString("Unknown error"));
        }
    }
}

void AntigravityAgent::drainOutput()
{
    buffer_ += process_->readAllStandardOutput();
    qsizetype newline;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (!line.isEmpty()) handleLine(line);
    }
}

void AntigravityAgent::finish(int code)
{
    if (completionSent_) return;
    completionSent_ = true;
    drainOutput();
    if (!buffer_.trimmed().isEmpty()) handleLine(buffer_.trimmed());
    buffer_.clear();
    if (code != 0 && errorDetails_.isEmpty()) errorDetails_ = diagnostics_;
    if (textSeen_) emit messageFinished();
    busy_ = false;
    stopRequested_ = false;
    emit turnCompleted(interrupted_ ? "interrupted" : (code == 0 && resultSeen_ && errorDetails_.isEmpty() ? "completed" : "failed"),
                       errorDetails_.isEmpty() && code != 0 && !interrupted_
                           ? QString("Antigravity CLI exited with code %1").arg(code) : errorDetails_);
    emit stateChanged();
    sendNextPrompt();
}
