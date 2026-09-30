#include "ClaudeAgent.h"

#include "ProcessLocks.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

namespace {
constexpr int kHistoryPageSize = 20;
}

ClaudeAgent::ClaudeAgent(const QString &pythonProgram, const QString &scriptPath,
                         const QString &workingDirectory, const QString &provider, const QString &indexPath,
                         QObject *parent)
    : AgentBackend(parent), pythonProgram_(pythonProgram), scriptPath_(scriptPath),
      workingDirectory_(workingDirectory), provider_(provider), process_(new QProcess(this)),
      index_(name(), indexPath)
{
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
        buffer_ += process_->readAllStandardOutput();
        qsizetype newline;
        while ((newline = buffer_.indexOf('\n')) >= 0) {
            const QByteArray line = buffer_.left(newline).trimmed();
            buffer_.remove(0, newline + 1);
            if (!line.isEmpty()) handleLine(line);
        }
    });
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QString details = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!details.isEmpty()) emit message("[" + name() + "] " + details);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        emit message("[" + name() + "] " + provider_.toUpper() + " bridge process error: " + process_->errorString()
                     + " (Python: " + pythonProgram_ + ")");
        if (processError != QProcess::FailedToStart) return;
        ready_ = false;
        busy_ = false;
        stopRequested_ = false;
        emit stateChanged();
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        emit message(QString("[%1] %2 bridge exited with code %3").arg(name(), provider_.toUpper()).arg(code));
        ready_ = false;
        busy_ = false;
        stopRequested_ = false;
        emit stateChanged();
    });
}

ClaudeAgent::~ClaudeAgent()
{
    for (QProcess *process : helperProcesses_) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    disconnect(process_, nullptr, this, nullptr);
    if (process_->state() != QProcess::NotRunning) {
        send({{"type", "shutdown"}});
        process_->closeWriteChannel();
        if (!process_->waitForFinished(1000)) {
            process_->terminate();
            if (!process_->waitForFinished(1000)) {
                process_->kill();
                process_->waitForFinished(1000);
            }
        }
    }
}

QString ClaudeAgent::name() const
{
    return provider_ == "glm" ? "GLM" : "Claude";
}

QString ClaudeAgent::statusText() const
{
    if (busy_) return name() + " is responding…";
    if (ready_) return name() + " ready";
    if (isRunning()) return provider_ == "glm" ? "Connecting to GLM via Claude Agent SDK…" : "Connecting to Claude Agent SDK…";
    return name() + " bridge is not running";
}

AgentHelp ClaudeAgent::help() const
{
    AgentHelp help;
    if (provider_ == "glm") {
        help.lines = {"GLM via Z.AI and Claude Agent SDK:",
                      "  Send messages with Enter or Send to GLM.",
                      "  Tool approvals and questions appear in dialogs.",
                      "  Requires claude-agent-sdk and ZAI_API_KEY; GLM_MODEL is optional.",
                      "  GLM setup: https://docs.z.ai/devpack/tool/claude",
                      "  Claude Code slash commands: https://code.claude.com/docs/en/commands",
                      "Installed Claude CLI options (reference; GLM uses the SDK):"};
    } else {
        help.lines = {"Claude Agent SDK:",
                      "  Send a message with Enter or the Send to Claude button.",
                      "  Responses are streamed into this window.",
                      "  Tool approvals and questions appear in dialogs.",
                      "  Messages entered during a response are queued.",
                      "  The selected Python environment needs claude-agent-sdk and API credentials.",
                      "  Claude Code slash commands: https://code.claude.com/docs/en/commands",
                      "Installed Claude CLI commands and options (reference; this window uses the SDK):"};
    }
    help.name = "Claude CLI";
    help.program = QStandardPaths::findExecutable("claude");
    help.arguments = {"--help"};
    return help;
}

QString ClaudeAgent::externalLock(const QString &id) const
{
    return claudeSessionLock(qEnvironmentVariable("CLAUDE_CONFIG_DIR", QDir::home().filePath(".claude")), id);
}

void ClaudeAgent::loadConversations()
{
    reportIndexError(index_.load());
}

// GLM sessions are only those recorded here; Claude also lists SDK sessions from all projects.
void ClaudeAgent::refreshConversations()
{
    if (provider_ == "glm") {
        reportIndexError(index_.load());
        emit conversationsChanged();
        return;
    }
    if (pythonProgram_.isEmpty() || scriptPath_.isEmpty()) return;
    runHelper({"--list-sessions", "--directories", "[]"}, workingDirectory_, [this](QProcess *process, bool started) {
        if (!started) {
            emit message("[Claude session discovery: " + process->errorString() + "]");
            return;
        }
        if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0) {
            QString details = QJsonDocument::fromJson(process->readAllStandardOutput()).object().value("message").toString();
            if (details.isEmpty()) details = QString::fromUtf8(process->readAllStandardError()).trimmed();
            if (!details.isEmpty()) emit message("[Claude session discovery: " + details + "]");
            return;
        }
        const QJsonArray sessions = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed())
                                        .object().value("sessions").toArray();
        bool changed = false;
        for (const QJsonValue &value : sessions) {
            const QJsonObject session = value.toObject();
            const QString id = session.value("id").toString();
            if (id.isEmpty() || !QFileInfo(session.value("cwd").toString()).isDir()
                || (excluded_ && excluded_->index_.contains(id))) continue;
            changed = index_.insert(session) || changed;
        }
        if (changed) {
            reportIndexError(index_.save());
            emit conversationsChanged();
        }
        emit message(QString("[Claude sessions discovered: %1]").arg(sessions.size()));
    });
}

void ClaudeAgent::reportIndexError(const QString &error)
{
    if (!error.isEmpty()) emit message("[" + error + "]");
}

// Runs claude/bridge.py for a one-shot query and calls done when it ends or fails to start.
void ClaudeAgent::runHelper(const QStringList &arguments, const QString &workingDirectory,
                            const std::function<void(QProcess *process, bool started)> &done)
{
    auto *process = new QProcess(this);
    helperProcesses_.append(process);
    const auto finish = [this, process, done](bool started) {
        helperProcesses_.removeAll(process);
        process->deleteLater();
        done(process, started);
    };
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [finish] { finish(true); });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    if (QFileInfo(workingDirectory).isDir()) process->setWorkingDirectory(workingDirectory);
    process->start(pythonProgram_, QStringList{"-u", scriptPath_, "--cwd", workingDirectory} + arguments);
}

bool ClaudeAgent::isRunning() const
{
    return process_->state() != QProcess::NotRunning;
}

void ClaudeAgent::start(const QString &workingDirectory)
{
    if (isRunning()) return;
    if (!workingDirectory.isEmpty()) workingDirectory_ = workingDirectory;
    process_->setWorkingDirectory(workingDirectory_);
    process_->start(pythonProgram_, {"-u", scriptPath_, "--cwd", workingDirectory_, "--provider", provider_});
}

bool ClaudeAgent::newConversation(const QString &workingDirectory)
{
    if (busy_) {
        emit message("[Wait for " + name() + " to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    sessionId_.clear();
    firstPrompt_.clear();
    queuedPrompts_.clear();
    pendingResumeId_.clear();
    ready_ = false;
    if (isRunning()) {
        send({{"type", "new"}, {"cwd", workingDirectory}});
    } else {
        start(workingDirectory);
    }
    emit stateChanged();
    return true;
}

bool ClaudeAgent::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (id.isEmpty() || !QFileInfo(workingDirectory).isDir()) return false;
    if (busy_) {
        emit message("[Wait for " + name() + " to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    sessionId_ = id;
    firstPrompt_.clear();
    queuedPrompts_.clear();
    ready_ = false;
    if (isRunning()) {
        send({{"type", "resume"}, {"session_id", id}, {"cwd", workingDirectory}});
    } else {
        // The bridge must report ready before it accepts the resume command.
        pendingResumeId_ = id;
        start(workingDirectory);
    }
    emit message("[Resuming " + name() + " conversation: " + id + "]");
    emit stateChanged();
    return true;
}

bool ClaudeAgent::prompt(const QString &text)
{
    if (sessionId_.isEmpty() && firstPrompt_.isEmpty()) firstPrompt_ = text;
    queuedPrompts_.append(text);
    if (!ready_) start({});
    sendNextPrompt();
    return true;
}

void ClaudeAgent::interrupt()
{
    if (!busy_ || stopRequested_) return;
    stopRequested_ = true;
    send({{"type", "stop"}});
    emit stateChanged();
}

// The SDK always parses the whole transcript, so it is read once and older pages come from memory.
void ClaudeAgent::loadHistory(const QString &id, const QString &workingDirectory, bool older)
{
    const quint64 generation = ++historyGeneration_;
    if (older && history_.id == id) {
        history_.showMore(kHistoryPageSize);
        emit historyLoaded(id, history_.visible(), history_.hasMore(), {});
        return;
    }
    history_ = {};
    if (pythonProgram_.isEmpty() || scriptPath_.isEmpty()) {
        emit historyLoaded(id, {}, false, "The Claude Agent SDK bridge is not configured.");
        return;
    }
    runHelper({"--read-session", id, "--limit", "0"}, workingDirectory,
              [this, generation, id](QProcess *process, bool started) {
        if (generation != historyGeneration_) return;
        if (!started) {
            emit historyLoaded(id, {}, false, "Could not start Python: " + process->errorString());
            return;
        }
        const QJsonObject result = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
        if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0
            || result.value("type") != "history") {
            QString details = result.value("message").toString();
            if (details.isEmpty()) details = QString::fromUtf8(process->readAllStandardError()).trimmed();
            emit message("[" + name() + " history: " + details + "]");
            emit historyLoaded(id, {}, false, "Could not load this conversation: " + details);
            return;
        }
        QList<ChatEntry> entries;
        for (const QJsonValue &value : result.value("entries").toArray()) {
            const QJsonObject entry = value.toObject();
            entries.append({entry.value("role").toString(), entry.value("text").toString()});
        }
        history_.reset(id, entries, kHistoryPageSize);
        emit historyLoaded(id, history_.visible(), history_.hasMore(), {});
    });
}

void ClaudeAgent::cancelHistory()
{
    ++historyGeneration_;
}

void ClaudeAgent::answerApproval(int id, bool allow)
{
    send({{"type", "approval_response"}, {"id", id}, {"allow", allow}});
}

void ClaudeAgent::answerQuestions(int id, const QHash<QString, QString> &answers)
{
    const int count = pendingQuestionCounts_.take(id);
    QJsonObject result;
    for (auto it = answers.begin(); it != answers.end(); ++it) result.insert(it.key(), it.value());
    send({{"type", "question_response"}, {"id", id}, {"accepted", answers.size() == count}, {"answers", result}});
}

void ClaudeAgent::sendNextPrompt()
{
    if (!ready_ || busy_ || queuedPrompts_.isEmpty()) return;
    busy_ = true;
    stopRequested_ = false;
    textStarted_ = false;
    send({{"type", "prompt"}, {"text", queuedPrompts_.takeFirst()}});
    emit stateChanged();
}

void ClaudeAgent::send(const QJsonObject &message)
{
    if (!isRunning()) return;
    process_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void ClaudeAgent::handleLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit message("[" + name() + "] Invalid " + provider_.toUpper() + " bridge message: " + error.errorString());
        return;
    }
    const QJsonObject event = document.object();
    const QString type = event.value("type").toString();
    if (type == "ready") {
        if (!pendingResumeId_.isEmpty()) {
            const QString id = pendingResumeId_;
            pendingResumeId_.clear();
            send({{"type", "resume"}, {"session_id", id}, {"cwd", workingDirectory_}});
            return;
        }
        ready_ = true;
        emit message(provider_ == "glm" ? "[Connected to GLM via Claude Agent SDK]" : "[Connected to Claude Agent SDK]");
        emit stateChanged();
        sendNextPrompt();
    } else if (type == "delta") {
        if (!textStarted_) {
            textStarted_ = true;
            emit messageStarted();
        }
        emit messageDelta(event.value("text").toString());
    } else if (type == "tool") {
        emit toolStarted(event.value("name").toString(),
                         QString::fromUtf8(QJsonDocument(event.value("input").toObject()).toJson(QJsonDocument::Compact)));
    } else if (type == "complete") {
        if (textStarted_) emit messageFinished();
        busy_ = false;
        stopRequested_ = false;
        textStarted_ = false;
        emit turnCompleted(event.value("status").toString(), event.value("details").toString());
        emit stateChanged();
        sendNextPrompt();
    } else if (type == "approval") {
        const QString details = QString::fromUtf8(QJsonDocument(event.value("input").toObject()).toJson(QJsonDocument::Indented));
        emit approvalRequested(event.value("id").toInt(), "Approve " + name() + " action",
                               event.value("tool").toString() + "\n\n" + details.trimmed());
    } else if (type == "question") {
        // The SDK keys answers by question text.
        QList<AgentQuestion> questions;
        for (const QJsonValue &value : event.value("questions").toArray()) {
            const QJsonObject object = value.toObject();
            AgentQuestion question;
            question.text = object.value("question").toString();
            question.id = question.text;
            question.header = object.value("header").toString(name() + " question");
            question.multiSelect = object.value("multiSelect").toBool();
            for (const QJsonValue &option : object.value("options").toArray())
                question.options.append(option.toObject().value("label").toString());
            questions.append(question);
        }
        const int id = event.value("id").toInt();
        pendingQuestionCounts_.insert(id, questions.size());
        emit questionsRequested(id, questions);
    } else if (type == "session") {
        sessionId_ = event.value("id").toString();
        emit conversationOpened(sessionId_, false);
        if (index_.remember(sessionId_, workingDirectory_, firstPrompt_)) {
            reportIndexError(index_.save());
            emit conversationsChanged();
        }
    } else if (type == "error") {
        emit message("[" + name() + "] " + event.value("message").toString());
    }
}
