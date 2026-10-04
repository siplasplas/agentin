#include "ClaudeAgent.h"
#include "CommandApproval.h"

#include "ProcessLocks.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

#include <utility>

namespace {
constexpr int kHistoryPageSize = 20;

QString lastLines(const QString &text, int count)
{
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    return lines.mid(qMax(0, lines.size() - count)).join('\n');
}
}

ClaudeEnvironment::ClaudeEnvironment(const QString &directory, QObject *parent)
    : QObject(parent), directory_(directory)
{
}

ClaudeEnvironment::~ClaudeEnvironment()
{
    if (!process_) return;
    disconnect(process_, nullptr, this, nullptr);
    process_->kill();
    process_->waitForFinished(1000);
}

QString ClaudeEnvironment::pythonProgram() const
{
#ifdef Q_OS_WIN
    return QDir(directory_).filePath("Scripts/python.exe");
#else
    return QDir(directory_).filePath("bin/python");
#endif
}

void ClaudeEnvironment::prepare(QObject *context, const std::function<void()> &done)
{
    if (ready_) {
        done();
        return;
    }
    waiters_.append({context, done});
    if (process_) return;
    if (!QFileInfo(pythonProgram()).isExecutable()) {
        const QString python = QStandardPaths::findExecutable("python3");
        if (python.isEmpty()) {
            emit message("[Claude Agent SDK: python3 was not found, so its environment cannot be created]");
            finish(false);
            return;
        }
        emit message("[Claude Agent SDK: creating a Python environment in " + directory_ + "]");
        run(python, {"-m", "venv", directory_}, [this](bool ok, const QString &output) {
            if (ok) {
                install();
                return;
            }
            emit message("[Claude Agent SDK: the Python environment could not be created (on Debian and Ubuntu "
                         "install python3-venv)]\n" + lastLines(output, 5));
            finish(false);
        });
        return;
    }
    run(pythonProgram(), {"-c", "import claude_agent_sdk"}, [this](bool ok, const QString &) {
        if (ok) finish(true);
        else install();
    });
}

void ClaudeEnvironment::install()
{
    emit message("[Claude Agent SDK: installing claude-agent-sdk into " + directory_ + ", this can take a minute]");
    run(pythonProgram(), {"-m", "pip", "install", "--upgrade", "claude-agent-sdk"},
        [this](bool ok, const QString &output) {
        emit message(ok ? QString("[Claude Agent SDK: installed]")
                        : "[Claude Agent SDK: installation failed]\n" + lastLines(output, 10));
        finish(ok);
    });
}

void ClaudeEnvironment::finish(bool ready)
{
    // A failed attempt is repeated the next time a bridge starts, for example after installing python3-venv.
    ready_ = ready;
    const auto waiters = std::exchange(waiters_, {});
    for (const auto &[context, done] : waiters)
        if (context) done();
}

void ClaudeEnvironment::run(const QString &program, const QStringList &arguments,
                            const std::function<void(bool ok, const QString &output)> &next)
{
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, next](int code, QProcess::ExitStatus status) {
        QProcess *process = std::exchange(process_, nullptr);
        process->deleteLater();
        next(status == QProcess::NormalExit && code == 0, QString::fromUtf8(process->readAll()));
    });
    connect(process_, &QProcess::errorOccurred, this, [this, next](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        QProcess *process = std::exchange(process_, nullptr);
        process->deleteLater();
        next(false, process->errorString());
    });
    process_->start(program, arguments);
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
    if (kind_ == "glm") {
        QList<AgentModel> result{{{}, "Default model", "GLM_MODEL, or glm-5.3 when it is not set", {}, {}, {}, true}};
        for (const QString &model : extraModels_) result.append({model, model, "Z.AI model", {}, {}, {}, false});
        return result;
    }
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

// Chat SDK events update the shared account snapshot, including any reported per-window data.
void ClaudeProvider::updateUsage(const QJsonObject &event)
{
    const QString type = event.value("limit").toString();
    if (type.isEmpty() || type == "overage") return;
    UsageLimit limit = usage_.value(type);
    limit.id = type;
    limit.windowMinutes = type == "five_hour" ? 5 * 60 : 7 * 24 * 60;
    limit.name = type == "seven_day_opus" ? "Opus" : (type == "seven_day_sonnet" ? "Sonnet" : QString());
    if (event.value("resetsAt").isDouble() && event.value("resetsAt").toInteger() != limit.resetsAt)
        limit.usedPercent = -1; // An old percentage must not be carried into a new window.
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

void ClaudeProvider::recordActivity(const QString &id)
{
    if (!index_.touch(id)) return;
    reportIndexError(index_.save());
    emit conversationsChanged();
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

// The bridge asks a light model once, in a temporary directory whose session it removes afterwards.
void ClaudeProvider::suggest(const QString &workingDirectory, const QString &prompt, QObject *context,
                             const std::function<void(const QString &text, const QString &error)> &done)
{
    runHelper({"--provider", kind_, "--suggest", prompt}, workingDirectory, context,
              [done](QProcess *process, bool started) {
        if (!started) {
            done({}, process->errorString());
            return;
        }
        const QJsonObject result = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
        if (result.value("type") == "suggestions") done(result.value("text").toString(), {});
        else done({}, result.value("message").toString(QString::fromUtf8(process->readAllStandardError()).trimmed()));
    });
}

namespace {
// The bridge's JSON answer of a one-shot helper, or its error message.
QJsonObject helperResult(QProcess *process, bool started, const QString &type, QString &error)
{
    if (!started) {
        error = process->errorString();
        return {};
    }
    const QJsonObject result = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
    if (process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0 && result.value("type") == type)
        return result;
    error = result.value("message").toString();
    if (error.isEmpty()) error = QString::fromUtf8(process->readAllStandardError()).trimmed();
    if (error.isEmpty()) error = "the Claude Agent SDK bridge failed";
    return {};
}
}

// The SDK records the title in the session, where Claude Code's own list finds it; the index keeps it for the tree.
void ClaudeProvider::renameConversation(const QString &id, const QString &workingDirectory, const QString &title,
                                        QObject *context, const std::function<void(const QString &error)> &done)
{
    const QPointer<QObject> guard(context);
    runHelper({"--provider", kind_, "--rename", id, "--title", title}, workingDirectory, this,
              [this, id, guard, done](QProcess *process, bool started) {
        QString error;
        const QJsonObject result = helperResult(process, started, "renamed", error);
        if (error.isEmpty() && index_.contains(id)) {
            QJsonObject entry = index_.value(id);
            entry.insert("title", result.value("title").toString());
            entry.insert("customTitle", result.value("title").toString());
            if (index_.insert(entry)) reportIndexError(index_.save());
            emit conversationsChanged();
        }
        if (guard) done(error);
    });
}

void ClaudeProvider::readConversationStart(const QString &id, const QString &workingDirectory, QObject *context,
                                           const std::function<void(const QList<ChatEntry> &entries,
                                                                    const QString &error)> &done)
{
    runHelper({"--provider", kind_, "--read-session", id, "--limit", "60", "--head"}, workingDirectory, context,
              [done](QProcess *process, bool started) {
        QString error;
        const QJsonObject result = helperResult(process, started, "history", error);
        QList<ChatEntry> entries;
        for (const QJsonValue &value : result.value("entries").toArray()) {
            const QJsonObject entry = value.toObject();
            entries.append({entry.value("role").toString(), entry.value("text").toString(), entry.value("id").toString()});
        }
        done(entries, error);
    });
}

void ClaudeProvider::setEnvironment(ClaudeEnvironment *environment)
{
    environment_ = environment;
    pythonProgram_ = environment->pythonProgram();
}

void ClaudeProvider::prepareEnvironment(QObject *context, const std::function<void()> &done)
{
    if (environment_) environment_->prepare(context, done);
    else done();
}

void ClaudeProvider::runHelper(const QStringList &arguments, const QString &workingDirectory, QObject *context,
                               const std::function<void(QProcess *process, bool started)> &done)
{
    const QPointer<QObject> guard(context);
    prepareEnvironment(this, [this, arguments, workingDirectory, guard, done] {
        if (guard) startHelper(arguments, workingDirectory, guard, done);
    });
}

void ClaudeProvider::startHelper(const QStringList &arguments, const QString &workingDirectory,
                                 const QPointer<QObject> &guard,
                                 const std::function<void(QProcess *process, bool started)> &done)
{
    auto *process = new QProcess(this);
    helperProcesses_.append(process);
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
      effort_(provider->models().isEmpty() || provider->kind() == "glm" ? QString() : provider->defaultEffort())
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
        if (!steeringText_.isEmpty()) emit steerFailed(std::exchange(steeringText_, {}), "the bridge did not start");
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
        if (!steeringText_.isEmpty()) emit steerFailed(std::exchange(steeringText_, {}), "the bridge exited");
        // A compaction is not a turn: it ends without one.
        const bool working = (busy_ && !compacting_) || !queuedPrompts_.isEmpty();
        if (compacting_) {
            compacting_ = false;
            emit compactionFinished();
        }
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
    if (busy_) return retry_.isEmpty() ? name_ + " is responding…" : name_ + ": " + retry_;
    if (ready_) return name_ + " ready";
    if (preparing_) return "Preparing the Claude Agent SDK environment…";
    if (isRunning()) return kind_ == "glm" ? "Connecting to GLM via Claude Agent SDK…" : "Connecting to Claude Agent SDK…";
    return name_ + " bridge is not running";
}

bool ClaudeAgent::isRunning() const
{
    return process_->state() != QProcess::NotRunning;
}

void ClaudeAgent::start(const QString &workingDirectory)
{
    if (isRunning() || preparing_) return;
    if (!workingDirectory.isEmpty()) workingDirectory_ = workingDirectory;
    if (!provider_) return;
    preparing_ = true;
    provider_->prepareEnvironment(this, [this] {
        preparing_ = false;
        startBridge();
        emit stateChanged();
    });
    if (preparing_) emit stateChanged();
}

// The working directory, model and options are read when the bridge starts, since they may change while
// its environment is being prepared.
void ClaudeAgent::startBridge()
{
    if (!provider_) return;
    process_->setWorkingDirectory(workingDirectory_);
    QStringList arguments{"-u", provider_->scriptPath(), "--cwd", workingDirectory_, "--provider", kind_};
    if (!model_.isEmpty()) arguments << "--model" << model_;
    if (!effort_.isEmpty()) arguments << "--effort" << effort_;
    if (readOnly_) arguments << "--read-only";
    appliedReadOnly_ = readOnly_;
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
    trustedSessionCommands_.clear();
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
    // Chat trust belongs to one conversation.
    if (id != sessionId_) trustedSessionCommands_.clear();
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

bool ClaudeAgent::steer(const QString &text)
{
    if (text.trimmed().isEmpty() || !canSteer() || !isRunning()) return false;
    steeringText_ = text;
    send({{"type", "steer"}, {"text", text}});
    emit stateChanged();
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
            entries.append({entry.value("role").toString(), entry.value("text").toString(), entry.value("id").toString()});
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
    const CommandVerdict verdict = approvalVerdicts_.take(id);
    if (askedCommandChecks_.remove(id)) {
        if (decision == ApprovalDecision::AcceptForSession) {
            for (const QString &rule : verdict.sessionRules()) trustedSessionCommands_.insert(rule);
            emit message("[Trusted for this chat: " + verdict.sessionRules().join(", ") + "]");
        }
        send({{"type", "command_check_response"}, {"id", id}, {"decision", allow ? "allow" : "deny"},
              {"reason", allow ? "approved in agentin" : "declined in agentin"}});
        if (decision == ApprovalDecision::Cancel) interrupt();
    } else {
        send({{"type", "approval_response"}, {"id", id}, {"allow", allow}, {"decision", value}});
    }
}

bool ClaudeAgent::canCompact() const
{
    return isRunning() && ready_ && !busy_ && !compacting_ && !sessionId_.isEmpty() && queuedPrompts_.isEmpty();
}

bool ClaudeAgent::compact()
{
    if (!canCompact()) return false;
    busy_ = true;
    compacting_ = true;
    emit compactionStarted();
    emit message("[Compacting " + name_ + " context]");
    send({{"type", "compact"}});
    emit stateChanged();
    return true;
}

QStringList ClaudeAgent::trustedSessionCommands() const
{
    QStringList rules = trustedSessionCommands_.values();
    rules.sort();
    return rules;
}

void ClaudeAgent::trustSessionCommands(const QStringList &rules)
{
    for (const QString &rule : rules) trustedSessionCommands_.insert(rule);
    emit message("[Trusted for this chat: " + rules.join(", ") + "]");
}

bool ClaudeAgent::removeTrustedSessionCommand(const QString &rule)
{
    if (!trustedSessionCommands_.remove(rule)) return false;
    emit message("[Removed chat trust: " + rule + "]");
    return true;
}

CommandContext ClaudeAgent::commandContext(const QJsonObject &event) const
{
    CommandContext context;
    context.directory = event.value("cwd").toString(workingDirectory_);
    for (const QJsonValue &directory : event.value("writable").toArray()) context.writable.append(directory.toString());
    // An older bridge does not say where writing is allowed.
    if (context.writable.isEmpty()) context.writable = {workingDirectory_, "/tmp", QDir::tempPath()};
    context.trusted = trustedSessionCommands_.values();
    // Without a timeout Claude Code gives a command two minutes.
    if (event.value("background").toBool()) context.timeoutSeconds = -1;
    else if (event.contains("command")) context.timeoutSeconds = event.value("timeout").isDouble() ? event.value("timeout").toInt() / 1000 : 120;
    return context;
}

// Session rules live in the Claude Code process, so the bridge reconnects and resumes the session.
void ClaudeAgent::resetSessionApprovals()
{
    if (busy_) {
        emit message("[Wait for " + name_ + " to finish before withdrawing its session approvals.]");
        return;
    }
    trustedSessionCommands_.clear();
    if (!isRunning()) return;
    ready_ = false;
    send({{"type", "reset_permissions"}});
    emit message("[" + name_ + " session approvals withdrawn; reconnecting]");
    emit stateChanged();
}

void ClaudeAgent::answerQuestions(int id, const QHash<QString, QStringList> &answers)
{
    const int count = pendingQuestionCounts_.take(id);
    QJsonObject result;
    // Claude Code expects the answers of a multi-select question as one comma-separated string.
    for (auto it = answers.begin(); it != answers.end(); ++it) result.insert(it.key(), it.value().join(", "));
    send({{"type", "question_response"}, {"id", id}, {"accepted", answers.size() == count}, {"answers", result}});
}

void ClaudeAgent::setReadOnly(bool readOnly)
{
    readOnly_ = readOnly;
    emit stateChanged();
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
    if (model_ == appliedModel_ && effort_ == appliedEffort_ && readOnly_ == appliedReadOnly_) return false;
    // GLM sets its model in the connection's environment, so a new GLM model also reconnects.
    const bool reconnect = effort_ != appliedEffort_ || (kind_ == "glm" && model_ != appliedModel_);
    send({{"type", "settings"}, {"model", model_}, {"effort", effort_}, {"readOnly", readOnly_}});
    appliedReadOnly_ = readOnly_;
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
    retry_.clear();
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
    if (!retry_.isEmpty() && (type == "delta" || type == "reasoning" || type == "tool" || type == "complete")) {
        retry_.clear();
        emit stateChanged();
    }
    if (type == "retry") {
        const QJsonValue status = event.value("status");
        QString cause = status.isDouble() ? QString("API error %1").arg(status.toInt()) : QString("API connection error");
        if (!event.value("error").toString().isEmpty()) cause += " (" + event.value("error").toString() + ")";
        retry_ = QString("%1, retry %2 of %3 in %4 s").arg(cause).arg(event.value("attempt").toInt())
                     .arg(event.value("maxRetries").toInt()).arg(event.value("delayMs").toDouble() / 1000, 0, 'f', 1);
        emit message("[" + name_ + "] " + retry_);
        emit stateChanged();
    } else if (type == "ready") {
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
    } else if (type == "reasoning") {
        emit reasoningUpdated(event.value("id").toString(), event.value("text").toString());
    } else if (type == "tool") {
        // A tool ends the text before it: the bridge streams a whole turn as one text, and a tool line waits for
        // the end of a message, so without this every tool of the turn would show after all of its text.
        if (textStarted_) {
            textStarted_ = false;
            emit messageFinished();
        }
        emit toolStarted(event.value("name").toString(),
                         QString::fromUtf8(QJsonDocument(event.value("input").toObject()).toJson(QJsonDocument::Compact)));
    } else if (type == "complete") {
        if (textStarted_) emit messageFinished();
        busy_ = false;
        stopRequested_ = false;
        textStarted_ = false;
        // A steering message the bridge never answered must not block steering in later turns.
        if (!steeringText_.isEmpty()) emit steerFailed(std::exchange(steeringText_, {}), "the turn ended before it was confirmed");
        emit turnCompleted(event.value("status").toString(), event.value("details").toString());
        emit stateChanged();
        sendNextPrompt();
    } else if (type == "command_check") {
        // The bridge's hook asks before Claude Code applies its own permission rules, so agentin's rules win.
        const int id = event.value("id").toInt();
        const QString command = event.value("command").toString();
        const CommandVerdict verdict = commandRuleVerdict(command, commandContext(event));
        // A read-only chat is left to Claude Code's plan mode, which a hook decision would override.
        if (verdict.decision == CommandDecision::Ask && !readOnly_) {
            // Claude Code may run a command it deems read-only even when a hook asks, so agentin asks itself and
            // answers the hook with the user's decision.
            askedCommandChecks_.insert(id);
            approvalVerdicts_.insert(id, verdict);
            const QStringList trusted = verdict.sessionRules();
            const QString details = "Bash\n\n" + command + "\n\n" + verdict.explanation();
            emit message("[Asking, as agentin's rules say: " + verdict.reason + "]");
            emit approvalRequested(id, "Approve " + name_ + " action", details, !trusted.isEmpty(), QString(),
                                   trusted.join(", "), verdict.choices());
            return;
        }
        QString decision;
        if (verdict.decision == CommandDecision::Deny) decision = "deny";
        else if (verdict.decision == CommandDecision::Allow && !readOnly_) decision = "allow";
        // What the rules allow runs sed with --sandbox, as a second guard behind agentin's reading of its script.
        send({{"type", "command_check_response"}, {"id", id}, {"decision", decision},
              {"reason", decision.isEmpty() ? QString() : "agentin's rules: " + verdict.reason}, {"sandboxSed", decision == "allow"}});
        if (!decision.isEmpty())
            emit message(QString(decision == "deny" ? "[Declined by agentin's rules: " : "[Allowed by agentin's rules: ")
                         + verdict.reason + "]");
    } else if (type == "context") {
        emit contextUsage(event.value("used").toInteger(-1), event.value("window").toInteger(-1));
    } else if (type == "compacted") {
        busy_ = false;
        compacting_ = false;
        emit compactionFinished();
        if (event.value("ok").toBool()) emit contextCompacted();
        else emit message("[Could not compact " + name_ + " context: " + event.value("details").toString() + "]");
        emit stateChanged();
        sendNextPrompt();
    } else if (type == "writable") {
        QStringList directories;
        for (const QJsonValue &directory : event.value("directories").toArray())
            if (!directory.toString().isEmpty()) directories.append(directory.toString());
        if (directories != writableDirectories_) {
            writableDirectories_ = directories;
            emit writableDirectoriesChanged();
        }
    } else if (type == "read_check") {
        // Reading tools run without a question anywhere, except on files that may hold secrets, which agentin asks
        // about itself. Glob only lists names.
        const int id = event.value("id").toInt();
        const QString tool = event.value("tool").toString();
        const QJsonObject input = event.value("input").toObject();
        QStringList paths;
        if (tool == "Read") {
            paths.append(input.value("file_path").toString());
        } else if (tool == "NotebookRead") {
            paths.append(input.value("notebook_path").toString());
        } else if (tool == "Grep") {
            const QString base = input.value("path").toString(".");
            const QString glob = input.value("glob").toString();
            paths.append(glob.isEmpty() ? base : base + (glob.contains('/') ? "/" : "/**/") + glob);
        }
        paths.removeAll(QString());
        const CommandVerdict verdict = readVerdict(tool, paths, commandContext(event));
        if (verdict.decision != CommandDecision::Ask) {
            send({{"type", "command_check_response"}, {"id", id}, {"decision", "allow"}, {"reason", "agentin: " + verdict.reason}});
            return;
        }
        askedCommandChecks_.insert(id);
        approvalVerdicts_.insert(id, verdict);
        const QString details = tool + "\n\n" + paths.join('\n') + "\n\n" + verdict.explanation();
        emit message("[Asking, as the file may hold secrets: " + verdict.reason + "]");
        emit approvalRequested(id, "Approve " + name_ + " action", details, false, QString());
    } else if (type == "approval") {
        // Without the hook, as with an older SDK, the same rules answer the approval.
        QList<ApprovalChoice> choices;
        if (event.value("tool").toString() == "Bash") {
            const CommandVerdict verdict = commandRuleVerdict(event.value("input").toObject().value("command").toString(),
                                                              commandContext({}));
            choices = verdict.choices();
            const bool allow = verdict.decision == CommandDecision::Allow && !readOnly_;
            if (verdict.decision == CommandDecision::Deny || allow) {
                send({{"type", "approval_response"}, {"id", event.value("id").toInt()}, {"allow", allow},
                      {"decision", allow ? "accept" : "decline"}});
                emit message(QString(allow ? "[Allowed by agentin's rules: " : "[Declined by agentin's rules: ")
                             + verdict.reason + "]");
                return;
            }
        }
        const QString tool = event.value("tool").toString();
        const QJsonObject input = event.value("input").toObject();
        QString details = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Indented)).trimmed();
        QString title = "Approve " + name_ + " action";
        // An edit reads as the file and the change, and says plainly when the file is outside the chat's
        // directories, as such an edit may change another project.
        const QString path = input.value(tool == "NotebookEdit" ? "notebook_path" : "file_path").toString();
        if (QStringList{"Edit", "MultiEdit", "Write", "NotebookEdit"}.contains(tool) && !path.isEmpty()) {
            const auto shortened = [](const QString &text) {
                QStringList lines = text.split('\n');
                const qsizetype count = lines.size();
                if (count > 30) lines = lines.mid(0, 30) << QString("… %1 more lines").arg(count - 30);
                return lines.join('\n');
            };
            details = tool + " " + path;
            if (event.value("outsideWritable").toBool()) {
                title = name_ + " wants to change a file outside this chat's directories";
                QStringList writable;
                for (const QJsonValue &directory : event.value("writable").toArray()) writable.append(directory.toString());
                details += "\n\nThis file is outside the directories of this chat"
                           + (writable.isEmpty() ? QString() : " (" + writable.join(", ") + ")")
                           + ". Allowing it lets " + name_ + " change it, for example in another project.";
            }
            if (tool == "Write") {
                details += "\n\nNew content:\n" + shortened(input.value("content").toString());
            } else if (tool == "Edit") {
                details += "\n\nReplace:\n" + shortened(input.value("old_string").toString()) + "\n\nWith:\n"
                         + shortened(input.value("new_string").toString());
            } else if (tool == "MultiEdit") {
                details += QString("\n\n%1 changes").arg(input.value("edits").toArray().size());
            }
        } else {
            details = tool + "\n\n" + details;
        }
        emit approvalRequested(event.value("id").toInt(), title, details,
                               event.value("canRemember").toBool(), event.value("alwaysRule").toString(), QString(), choices);
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
    } else if (type == "usage") {
        // Claude counts cache reads and cache writes apart from the other input tokens.
        const QJsonObject usage = event.value("usage").toObject();
        TokenUsage tokens;
        const qint64 fresh = usage.value("input_tokens").toInteger(-1);
        const qint64 cacheRead = usage.value("cache_read_input_tokens").toInteger(0);
        const qint64 cacheWrite = usage.value("cache_creation_input_tokens").toInteger(0);
        tokens.input = fresh < 0 ? -1 : fresh + cacheRead + cacheWrite;
        tokens.cached = usage.contains("cache_read_input_tokens") ? cacheRead : -1;
        tokens.output = usage.value("output_tokens").toInteger(-1);
        tokens.total = tokens.input < 0 || tokens.output < 0 ? -1 : tokens.input + tokens.output;
        tokens.costUsd = event.value("costUsd").isDouble() ? event.value("costUsd").toDouble() : -1;
        emit turnUsage(tokens);
    } else if (type == "steer_accepted" || type == "steer_failed") {
        const QString text = std::exchange(steeringText_, {});
        if (type == "steer_accepted") emit steerAccepted(text);
        else emit steerFailed(text, event.value("message").toString());
        emit stateChanged();
        sendNextPrompt();
    } else if (type == "rate_limit") {
        if (provider_) provider_->updateUsage(event);
    } else if (type == "error") {
        emit message("[" + name_ + "] " + event.value("message").toString());
    }
}
