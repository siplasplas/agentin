#include "AntigravityAgent.h"

#include "ProcessLocks.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

AntigravityProvider::AntigravityProvider(const QString &program, const QString &indexPath, QObject *parent)
    : AgentProvider(parent), program_(program), index_("Antigravity", indexPath)
{
}

AgentHelp AntigravityProvider::help() const
{
    return {{"Antigravity CLI headless mode:",
             "  Send messages with Enter or Send. Later messages resume the same conversation ID.",
             "  Create a chat to choose its working directory, or double-click a saved chat to resume it.",
             "  Authenticate once in the interactive agy CLI before using this window.",
             "  CLI documentation: https://antigravity.google/docs/cli/headless/",
             "Installed Antigravity CLI commands and options:"},
            "Antigravity CLI", program_, {"--help"}};
}

QString AntigravityProvider::externalLock(const QString &id) const
{
    return commandLineLock(id);
}

void AntigravityProvider::loadConversations()
{
    reportIndexError(index_.load());
    requestModels();
}

// `agy models` prints one model per line as "<id>\t<name>". The reasoning level is part of the model
// (for example gemini-3.8-flash-high), so Antigravity chats offer no separate effort.
void AntigravityProvider::requestModels()
{
    auto *process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (status != QProcess::NormalExit || code != 0) return;
        QList<AgentModel> models{{{}, "Default model", "The model configured in Antigravity CLI", {}, {}, {}, true}};
        for (const QString &line : QString::fromUtf8(process->readAllStandardOutput()).split('\n')) {
            const QStringList fields = line.split('\t');
            if (fields.size() < 2 || fields.first().trimmed().isEmpty()) continue;
            models.append({fields.first().trimmed(), fields.at(1).trimmed(), fields.at(1).trimmed(), {}, {}, {}, false});
        }
        if (models.size() == 1) return;
        models_ = models;
        emit modelsChanged();
    });
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) process->deleteLater();
    });
    process->start(program_, {"models"});
}

void AntigravityProvider::refreshConversations()
{
    reportIndexError(index_.load());
    emit conversationsChanged();
}

AgentBackend *AntigravityProvider::createChat(const QString &workingDirectory, QObject *parent)
{
    return new AntigravityAgent(this, workingDirectory, parent);
}

void AntigravityProvider::rememberConversation(const QString &id, const QString &workingDirectory,
                                               const QString &firstPrompt)
{
    if (!index_.remember(id, workingDirectory, firstPrompt)) return;
    reportIndexError(index_.save());
    emit conversationsChanged();
}

void AntigravityProvider::reportIndexError(const QString &error)
{
    if (!error.isEmpty()) emit message("[" + error + "]");
}

AntigravityAgent::AntigravityAgent(AntigravityProvider *provider, const QString &workingDirectory, QObject *parent)
    : AgentBackend(parent), provider_(provider), program_(provider->program()), workingDirectory_(workingDirectory),
      model_(provider->defaultModel()), process_(new QProcess(this))
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

void AntigravityAgent::rememberConversation()
{
    emit conversationOpened(conversationId_, false);
    if (provider_) provider_->rememberConversation(conversationId_, workingDirectory_, firstPrompt_);
}

void AntigravityAgent::setModel(const QString &model, const QString &)
{
    model_ = model;
    emit stateChanged();
}

bool AntigravityAgent::newConversation(const QString &workingDirectory)
{
    if (busy_) {
        emit message("[Wait for Antigravity to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    conversationId_.clear();
    firstPrompt_.clear();
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
    firstPrompt_.clear();
    queuedPrompts_.clear();
    emit message("[Resuming Antigravity conversation: " + id + "]");
    emit stateChanged();
    return true;
}

bool AntigravityAgent::prompt(const QString &text)
{
    if (conversationId_.isEmpty() && firstPrompt_.isEmpty()) firstPrompt_ = text;
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
    if (!model_.isEmpty()) arguments << "--model" << model_;
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
            rememberConversation();
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
            rememberConversation();
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
