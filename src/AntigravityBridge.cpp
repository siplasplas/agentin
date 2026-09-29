#include "AntigravityBridge.h"

#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

AntigravityBridge::AntigravityBridge(const QString &program, const QString &workingDirectory, QObject *parent)
    : QObject(parent), program_(program), workingDirectory_(workingDirectory), process_(new QProcess(this))
{
    connect(process_, &QProcess::readyReadStandardOutput, this, &AntigravityBridge::drainOutput);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QString details = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!details.isEmpty()) {
            diagnostics_ = details;
            emit error(details);
        }
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (interrupted_ && processError == QProcess::Crashed) return;
        errorDetails_ = process_->errorString();
        emit error("Antigravity CLI process error: " + errorDetails_ + " (executable: " + program_ + ")");
        if (processError == QProcess::FailedToStart) finish(-1);
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { finish(code); });
}

AntigravityBridge::~AntigravityBridge()
{
    if (isRunning()) {
        process_->terminate();
        if (!process_->waitForFinished(1000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
}

QString AntigravityBridge::program() const { return program_; }
QString AntigravityBridge::conversationId() const { return conversationId_; }
bool AntigravityBridge::isRunning() const { return process_->state() != QProcess::NotRunning; }

void AntigravityBridge::prompt(const QString &text)
{
    if (isRunning()) return;
    buffer_.clear();
    errorDetails_.clear();
    diagnostics_.clear();
    interrupted_ = false;
    resultSeen_ = false;
    textSeen_ = false;
    completionSent_ = false;
    QStringList arguments{"--output-format", "stream-json"};
    if (!conversationId_.isEmpty()) arguments << "--conversation" << conversationId_;
    arguments << "--prompt" << text;
    process_->setWorkingDirectory(workingDirectory_);
    process_->start(program_, arguments);
}

void AntigravityBridge::interrupt()
{
    if (!isRunning()) return;
    interrupted_ = true;
    process_->terminate();
    QTimer::singleShot(1000, process_, [this] { if (isRunning()) process_->kill(); });
}

void AntigravityBridge::resetConversation(const QString &workingDirectory)
{
    if (isRunning()) return;
    workingDirectory_ = workingDirectory;
    conversationId_.clear();
}

void AntigravityBridge::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (isRunning()) return;
    conversationId_ = id;
    workingDirectory_ = workingDirectory;
}

void AntigravityBridge::handleLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        emit error("Invalid Antigravity CLI response: " + parseError.errorString());
        return;
    }
    const QJsonObject event = document.object();
    const QString type = event.value("event").toString();
    if (type == "init") {
        const QString id = event.value("conversation_id").toString();
        if (!id.isEmpty()) {
            conversationId_ = id;
            emit conversationChanged(id);
        }
    } else if (type == "step_update") {
        const QJsonObject step = event.value("step_update").toObject();
        if (step.value("step_type").toString() == "agent_response") {
            const QString delta = step.value("text_delta").toString();
            if (!delta.isEmpty()) {
                textSeen_ = true;
                emit textDelta(delta);
            }
        } else if (step.value("step_type").toString() == "tool" && step.value("state").toString() == "ACTIVE") {
            const QJsonObject tool = step.value("tool_info").toObject();
            emit toolStarted(step.value("tool_name").toString(tool.value("name").toString()),
                             tool.value("parameters").toObject());
        }
    } else if (type == "result") {
        resultSeen_ = true;
        const QJsonObject result = event.value("result").toObject();
        const QString id = result.value("conversation_id").toString();
        if (!id.isEmpty() && id != conversationId_) {
            conversationId_ = id;
            emit conversationChanged(id);
        }
        if (!textSeen_ && !result.value("response").toString().isEmpty()) {
            emit textDelta(result.value("response").toString());
        }
        if (result.value("status").toString() != "SUCCESS") {
            errorDetails_ = result.value("error").toString(result.value("status").toString("Unknown error"));
        }
    }
}

void AntigravityBridge::drainOutput()
{
    buffer_ += process_->readAllStandardOutput();
    qsizetype newline;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (!line.isEmpty()) handleLine(line);
    }
}

void AntigravityBridge::finish(int code)
{
    if (completionSent_) return;
    completionSent_ = true;
    drainOutput();
    if (!buffer_.trimmed().isEmpty()) handleLine(buffer_.trimmed());
    buffer_.clear();
    if (code != 0 && errorDetails_.isEmpty()) errorDetails_ = diagnostics_;
    emit completed(interrupted_ ? "interrupted" : (code == 0 && resultSeen_ && errorDetails_.isEmpty() ? "completed" : "failed"),
                   errorDetails_.isEmpty() && code != 0 && !interrupted_
                       ? QString("Antigravity CLI exited with code %1").arg(code) : errorDetails_);
}
