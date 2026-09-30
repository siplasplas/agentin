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

ClaudeProvider::ClaudeProvider(const QString &pythonProgram, const QString &scriptPath, const QString &workingDirectory,
                               const QString &kind, const QString &indexPath, QObject *parent)
    : AgentProvider(parent), pythonProgram_(pythonProgram), scriptPath_(scriptPath),
      workingDirectory_(workingDirectory), kind_(kind), index_(kind == "glm" ? "GLM" : "Claude", indexPath)
{
}

ClaudeProvider::~ClaudeProvider()
{
    for (QProcess *process : helperProcesses_) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

QString ClaudeProvider::name() const
{
    return kind_ == "glm" ? "GLM" : "Claude";
}

AgentHelp ClaudeProvider::help() const
{
    AgentHelp help;
    if (kind_ == "glm") {
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

QString ClaudeProvider::externalLock(const QString &id) const
{
    return claudeSessionLock(qEnvironmentVariable("CLAUDE_CONFIG_DIR", QDir::home().filePath(".claude")), id);
}

void ClaudeProvider::loadConversations()
{
    reportIndexError(index_.load());
}

void ClaudeProvider::refreshConversations()
{
    if (kind_ == "glm") {
        reportIndexError(index_.load());
        emit conversationsChanged();
        return;
    }
    if (pythonProgram_.isEmpty() || scriptPath_.isEmpty()) return;
    runHelper({"--list-sessions", "--directories", "[]"}, workingDirectory_, this, [this](QProcess *process, bool started) {
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

// The Agent SDK cannot list models, so Claude offers Claude Code's aliases for the latest models.
QList<AgentModel> ClaudeProvider::models() const
{
    if (kind_ == "glm") return {};
    const QStringList efforts{"low", "medium", "high", "xhigh", "max"};
    const QStringList effortDescriptions{
        "Minimal thinking, fastest responses", "Moderate thinking", "Deep reasoning",
        "Extended reasoning depth; models without it use high", "Maximum effort"};
    QList<AgentModel> result;
    const auto add = [&](const QString &id, const QString &displayName, const QString &description) {
        result.append({id, displayName, description, efforts, effortDescriptions, "high", id.isEmpty()});
    };
    add({}, "Default model", "The model Claude Code uses by default for this account");
    add("fable", "Fable", "Latest Fable model");
    add("opus", "Opus", "Latest Opus model");
    add("sonnet", "Sonnet", "Latest Sonnet model");
    add("haiku", "Haiku", "Latest Haiku model");
    return result;
}

// The SDK reports a window only when its status changes, so windows appear after some turns.
void ClaudeProvider::updateUsage(const QJsonObject &event)
{
    const QString type = event.value("limit").toString();
    if (type.isEmpty() || type == "overage") return;
    UsageLimit limit = usage_.value(type);
    limit.id = type;
    limit.windowMinutes = type == "five_hour" ? 5 * 60 : 7 * 24 * 60;
    limit.name = type == "seven_day_opus" ? "Opus" : (type == "seven_day_sonnet" ? "Sonnet" : QString());
    if (event.value("utilization").isDouble()) limit.usedPercent = event.value("utilization").toDouble() * 100;
    if (event.value("resetsAt").isDouble()) limit.resetsAt = event.value("resetsAt").toInteger();
    limit.status = event.value("status").toString();
    usage_.insert(type, limit);
    emit usageChanged();
}

AgentBackend *ClaudeProvider::createChat(const QString &workingDirectory, QObject *parent)
{
    return new ClaudeAgent(this, workingDirectory, parent);
}

void ClaudeProvider::rememberConversation(const QString &id, const QString &workingDirectory, const QString &firstPrompt)
{
    if (!index_.remember(id, workingDirectory, firstPrompt)) return;
    reportIndexError(index_.save());
    emit conversationsChanged();
}

void ClaudeProvider::reportIndexError(const QString &error)
{
    if (!error.isEmpty()) emit message("[" + error + "]");
}

void ClaudeProvider::runHelper(const QStringList &arguments, const QString &workingDirectory, QObject *context,
                               const std::function<void(QProcess *process, bool started)> &done)
{
    auto *process = new QProcess(this);
    helperProcesses_.append(process);
    const QPointer<QObject> guard(context);
    const auto finish = [this, process, done, guard](bool started) {
        helperProcesses_.removeAll(process);
        process->deleteLater();
        if (guard) done(process, started);
    };
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [finish] { finish(true); });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    if (QFileInfo(workingDirectory).isDir()) process->setWorkingDirectory(workingDirectory);
    process->start(pythonProgram_, QStringList{"-u", scriptPath_, "--cwd", workingDirectory} + arguments);
}

ClaudeAgent::ClaudeAgent(ClaudeProvider *provider, const QString &workingDirectory, QObject *parent)
    : AgentBackend(parent), provider_(provider), name_(provider->name()), kind_(provider->kind()),
      workingDirectory_(workingDirectory), process_(new QProcess(this)),
      model_(provider->models().isEmpty() ? QString() : provider->defaultModel()),
      effort_(provider->models().isEmpty() ? QString() : provider->defaultEffort())
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
        if (!details.isEmpty()) emit message("[" + name_ + "] " + details);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        emit message("[" + name_ + "] " + kind_.toUpper() + " bridge process error: " + process_->errorString()
                     + " (Python: " + process_->program() + ")");
        if (processError != QProcess::FailedToStart) return;
        const bool working = busy_ || !queuedPrompts_.isEmpty();
        ready_ = false;
        busy_ = false;
        stopRequested_ = false;
        queuedPrompts_.clear();
        if (working) emit turnCompleted("failed", "the bridge did not start");
        emit stateChanged();
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        emit message(QString("[%1] %2 bridge exited with code %3").arg(name_, kind_.toUpper()).arg(code));
        const bool working = busy_ || !queuedPrompts_.isEmpty();
        ready_ = false;
        busy_ = false;
        stopRequested_ = false;
        queuedPrompts_.clear();
        // Every message sent ends with turnCompleted, so its tab can release the directory it holds.
        if (working) emit turnCompleted("failed", QString("the bridge exited with code %1").arg(code));
        emit stateChanged();
    });
}

ClaudeAgent::~ClaudeAgent()
{
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

QString ClaudeAgent::statusText() const
{
    if (busy_) return name_ + " is responding…";
    if (ready_) return name_ + " ready";
    if (isRunning()) return kind_ == "glm" ? "Connecting to GLM via Claude Agent SDK…" : "Connecting to Claude Agent SDK…";
    return name_ + " bridge is not running";
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
    if (!provider_) return;
    QStringList arguments{"-u", provider_->scriptPath(), "--cwd", workingDirectory_, "--provider", kind_};
    if (!model_.isEmpty()) arguments << "--model" << model_;
    if (!effort_.isEmpty()) arguments << "--effort" << effort_;
    appliedModel_ = model_;
    appliedEffort_ = effort_;
    process_->start(provider_->pythonProgram(), arguments);
}

bool ClaudeAgent::newConversation(const QString &workingDirectory)
{
    if (busy_) {
        emit message("[Wait for " + name_ + " to finish.]");
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
        emit message("[Wait for " + name_ + " to finish.]");
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
    emit message("[Resuming " + name_ + " conversation: " + id + "]");
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
    if (!provider_ || provider_->pythonProgram().isEmpty() || provider_->scriptPath().isEmpty()) {
        emit historyLoaded(id, {}, false, "The Claude Agent SDK bridge is not configured.");
        return;
    }
    provider_->runHelper({"--read-session", id, "--limit", "0"}, workingDirectory, this,
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
            emit message("[" + name_ + " history: " + details + "]");
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

void ClaudeAgent::answerApproval(int id, ApprovalDecision decision)
{
    const QString value = decision == ApprovalDecision::Accept ? "accept"
        : decision == ApprovalDecision::AcceptForSession ? "acceptForSession"
        : decision == ApprovalDecision::AcceptAlways ? "acceptAlways"
        : decision == ApprovalDecision::Cancel ? "cancel" : "decline";
    const bool allow = decision == ApprovalDecision::Accept || decision == ApprovalDecision::AcceptForSession
        || decision == ApprovalDecision::AcceptAlways;
    send({{"type", "approval_response"}, {"id", id}, {"allow", allow}, {"decision", value}});
}

void ClaudeAgent::answerQuestions(int id, const QHash<QString, QStringList> &answers)
{
    const int count = pendingQuestionCounts_.take(id);
    QJsonObject result;
    // Claude Code expects the answers of a multi-select question as one comma-separated string.
    for (auto it = answers.begin(); it != answers.end(); ++it) result.insert(it.key(), it.value().join(", "));
    send({{"type", "question_response"}, {"id", id}, {"accepted", answers.size() == count}, {"answers", result}});
}

void ClaudeAgent::setModel(const QString &model, const QString &effort)
{
    model_ = model;
    effort_ = effort;
    emit stateChanged();
}

// Sends a changed model or effort to the bridge. Returns true while the bridge reconnects for a new
// effort; it reports ready again when done.
bool ClaudeAgent::applySettings()
{
    if (model_ == appliedModel_ && effort_ == appliedEffort_) return false;
    const bool reconnect = effort_ != appliedEffort_;
    send({{"type", "settings"}, {"model", model_}, {"effort", effort_}});
    appliedModel_ = model_;
    appliedEffort_ = effort_;
    if (reconnect) {
        ready_ = false;
        emit stateChanged();
    }
    return reconnect;
}

void ClaudeAgent::sendNextPrompt()
{
    if (!ready_ || busy_ || queuedPrompts_.isEmpty()) return;
    if (applySettings()) return;
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
        emit message("[" + name_ + "] Invalid " + kind_.toUpper() + " bridge message: " + error.errorString());
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
        emit message(kind_ == "glm" ? "[Connected to GLM via Claude Agent SDK]" : "[Connected to Claude Agent SDK]");
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
        emit approvalRequested(event.value("id").toInt(), "Approve " + name_ + " action",
                               event.value("tool").toString() + "\n\n" + details.trimmed(),
                               event.value("canRemember").toBool(), event.value("alwaysRule").toString());
    } else if (type == "question") {
        // The SDK keys answers by question text.
        QList<AgentQuestion> questions;
        for (const QJsonValue &value : event.value("questions").toArray()) {
            const QJsonObject object = value.toObject();
            AgentQuestion question;
            question.text = object.value("question").toString();
            question.id = question.text;
            question.header = object.value("header").toString(name_ + " question");
            question.multiSelect = object.value("multiSelect").toBool();
            // Claude Code's questions always accept an answer in the user's own words.
            question.allowOther = true;
            for (const QJsonValue &option : object.value("options").toArray()) {
                question.options.append(option.toObject().value("label").toString());
                question.optionDescriptions.append(option.toObject().value("description").toString());
            }
            questions.append(question);
        }
        const int id = event.value("id").toInt();
        pendingQuestionCounts_.insert(id, questions.size());
        emit questionsRequested(id, questions);
    } else if (type == "session") {
        sessionId_ = event.value("id").toString();
        emit conversationOpened(sessionId_, false);
        if (provider_) provider_->rememberConversation(sessionId_, workingDirectory_, firstPrompt_);
    } else if (type == "rate_limit") {
        if (provider_) provider_->updateUsage(event);
    } else if (type == "error") {
        emit message("[" + name_ + "] " + event.value("message").toString());
    }
}
