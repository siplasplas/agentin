#include "CodexAgent.h"

#include "CodexConnection.h"
#include "CommandApproval.h"

#include <QDir>
#include <QJsonArray>

#include <utility>

namespace {
constexpr int kHistoryPageSize = 40;

// A path, glob pattern or special location of a Codex permission request, as text.
QString permissionPath(const QJsonObject &path)
{
    const QString type = path.value("type").toString();
    if (type == "path") return path.value("path").toString();
    if (type == "glob_pattern") return path.value("pattern").toString();
    const QJsonObject special = path.value("value").toObject();
    const QString subpath = special.value("subpath").toString();
    return "(" + special.value("kind").toString() + (subpath.isEmpty() ? QString() : ": " + subpath) + ")";
}

// The file system part of a permission profile as pairs of access and path, including the older lists.
QList<QPair<QString, QString>> permissionEntries(const QJsonObject &permissions)
{
    const QJsonObject fileSystem = permissions.value("fileSystem").toObject();
    QList<QPair<QString, QString>> entries;
    for (const QJsonValue &value : fileSystem.value("entries").toArray()) {
        const QJsonObject entry = value.toObject();
        entries.append({entry.value("access").toString(), permissionPath(entry.value("path").toObject())});
    }
    for (const QString &access : {QStringLiteral("read"), QStringLiteral("write")})
        for (const QJsonValue &path : fileSystem.value(access).toArray()) entries.append({access, path.toString()});
    return entries;
}

// Plain directories granted for writing; patterns and special locations cannot be held by the directory lock.
QStringList permissionWriteRoots(const QJsonObject &permissions)
{
    QStringList roots;
    const QJsonObject fileSystem = permissions.value("fileSystem").toObject();
    for (const QJsonValue &value : fileSystem.value("entries").toArray()) {
        const QJsonObject entry = value.toObject();
        const QJsonObject path = entry.value("path").toObject();
        if (entry.value("access").toString() == "write" && path.value("type").toString() == "path")
            roots.append(path.value("path").toString());
    }
    for (const QJsonValue &path : fileSystem.value("write").toArray()) roots.append(path.toString());
    roots.removeAll(QString());
    roots.removeDuplicates();
    return roots;
}

QString reasoningText(const QJsonObject &item)
{
    QStringList parts;
    // Raw content takes precedence when the model supplies it; otherwise show the summary.
    QJsonArray sections = item.value("content").toArray();
    if (sections.isEmpty()) sections = item.value("summary").toArray();
    for (const QJsonValue &section : sections)
        if (!section.toString().isEmpty()) parts.append(section.toString());
    return parts.join("\n\n");
}

// Converts one Codex App Server thread item into chat entries.
QList<ChatEntry> historyEntries(const QJsonObject &item)
{
    const QString type = item.value("type").toString();
    if (type == "userMessage") {
        QStringList parts;
        for (const QJsonValue &input : item.value("content").toArray()) {
            const QString text = input.toObject().value("text").toString();
            if (!text.isEmpty()) parts.append(text);
        }
        if (!parts.isEmpty()) return {{"user", parts.join('\n')}};
    } else if (type == "agentMessage") {
        const QString text = item.value("text").toString().trimmed();
        if (!text.isEmpty()) return {{"assistant", text}};
    } else if (type == "reasoning") {
        return {{"reasoning", reasoningText(item), item.value("id").toString()}};
    } else if (type == "commandExecution") {
        QString text = "$ " + item.value("command").toString();
        const QString output = item.value("aggregatedOutput").toString();
        if (!output.isEmpty()) text += '\n' + output;
        return {{"tool", text}};
    } else if (type == "fileChange") {
        return {{"tool", "file changes"}};
    } else if (type == "contextCompaction") {
        return {{"tool", "Context compacted"}};
    }
    return {};
}
}

QList<ChatEntry> CodexAgent::itemEntries(const QJsonObject &item)
{
    return historyEntries(item);
}

CodexAgent::CodexAgent(CodexConnection *connection, const QString &workingDirectory, QObject *parent)
    : AgentBackend(parent), connection_(connection), workingDirectory_(workingDirectory)
{
    connect(connection, &CodexConnection::stateChanged, this, &AgentBackend::stateChanged);
    connect(this, &AgentBackend::turnCompleted, this, [this] { turnWriteRoots_.clear(); });
    connect(connection, &CodexConnection::connected, this, [this] {
        if (historyPending_) loadHistory(historyThreadId_, {}, false);
        if (!userMessagesPending_.isEmpty()) loadUserMessages(userMessagesPending_, {});
        // After a restart the chat continues its thread; a queued message waits until it is open.
        if (!reopenThreadId_.isEmpty()) {
            const QString id = reopenThreadId_;
            reopenThreadId_.clear();
            const QStringList queued = queuedPrompts_;
            resumeConversation(id, workingDirectory_);
            queuedPrompts_ = queued;
        }
        // A message sent while the server was still starting waits for a thread.
        if (!queuedPrompts_.isEmpty() && threadId_.isEmpty()) startThread();
    });
    connect(connection, &CodexConnection::disconnected, this, [this] {
        const bool working = busy_ || threadOpening_ || !queuedPrompts_.isEmpty();
        if (!threadId_.isEmpty()) reopenThreadId_ = threadId_;
        threadOpening_ = false;
        if (steeringInFlight_) {
            const QString text = steeringText_;
            steeringInFlight_ = false;
            steeringText_.clear();
            emit steerFailed(text, "The Codex App Server disconnected before confirming the message.");
        }
        threadId_.clear();
        for (int id : serverRequests_.keys()) emit requestResolved(id);
        serverRequests_.clear();
        pendingQuestions_.clear();
        proposedRules_.clear();
        requestSessionRules_.clear();
        permissionRequests_.clear();
        sessionWriteRoots_.clear();
        queuedPrompts_.clear();
        if (refreshingHistory_) ++historyGeneration_;
        refreshingHistory_ = false;
        compactedHistoryPending_ = false;
        resetTurn();
        // Every message sent ends with turnCompleted, so its tab can release the directory it holds.
        if (working) emit turnCompleted("failed", "the Codex App Server disconnected");
    });
}

CodexAgent::~CodexAgent()
{
    closeThread();
}

QString CodexAgent::statusText() const
{
    if (!connection_ || !connection_->isRunning()) return "Server: not running";
    if (manualCompaction_) return "Compacting Codex context…";
    if (refreshingHistory_) return "Refreshing Codex history…";
    if (busy_) return "Codex is responding…";
    if (threadOpening_) return "Opening Codex conversation…";
    if (connection_->isConnected()) return "Codex ready";
    return "Connecting to Codex App Server…";
}

bool CodexAgent::newConversation(const QString &workingDirectory)
{
    if (!connection_ || !connection_->isConnected()) {
        emit message("[Codex App Server is not connected.]");
        return false;
    }
    if (busy_ || threadOpening_ || steeringInFlight_) {
        emit message("[Wait for the current Codex response to finish.]");
        return false;
    }
    cancelReopening();
    closeThread();
    trustedSessionCommands_.clear();
    trustedConversationId_.clear();
    workingDirectory_ = workingDirectory;
    queuedPrompts_.clear();
    startThread();
    return true;
}

bool CodexAgent::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (id.isEmpty() || !connection_ || !connection_->isConnected()) return false;
    if (busy_ || threadOpening_ || steeringInFlight_) {
        emit message("[Wait for the current Codex response to finish.]");
        return false;
    }
    if (id != trustedConversationId_) {
        trustedSessionCommands_.clear();
        trustedConversationId_.clear();
    }
    cancelReopening();
    closeThread();
    if (!workingDirectory.isEmpty()) workingDirectory_ = workingDirectory;
    queuedPrompts_.clear();
    threadOpening_ = true;
    emit message("[Resuming Codex conversation: " + id + "]");
    connection_->request("thread/resume", {{"threadId", id}}, this,
                         [this, id](const QJsonObject &result, const QString &error) {
        threadOpening_ = false;
        if (!error.isEmpty()) {
            emit conversationOpenFailed(id, "the Codex App Server refused to open it: " + error);
            emit stateChanged();
            return;
        }
        openThread(result, true);
    });
    emit stateChanged();
    return true;
}

bool CodexAgent::prompt(const QString &text)
{
    if (!connection_ || !connection_->isRunning()) {
        emit message("[Server is not running. Check the codex executable and restart.]");
        return false;
    }
    queuedPrompts_.append(text);
    // A conversation being reopened takes the prompt when it is open again.
    if (threadId_.isEmpty() && reopeningThreadId_.isEmpty()) startThread();
    sendNextPrompt();
    return true;
}

void CodexAgent::interrupt()
{
    if (!busy_ || stopRequested_) return;
    stopRequested_ = true;
    sendStopIfPossible();
    emit stateChanged();
}

bool CodexAgent::canSteer() const
{
    return connection_ && connection_->isConnected() && busy_ && !manualCompaction_ && !stopRequested_
        && !threadId_.isEmpty() && !activeTurnId_.isEmpty() && !steeringInFlight_;
}

bool CodexAgent::steer(const QString &text)
{
    if (text.trimmed().isEmpty() || !canSteer()) return false;
    steeringInFlight_ = true;
    steeringText_ = text;
    const QString id = threadId_;
    const QString turnId = activeTurnId_;
    emit stateChanged();
    connection_->request("turn/steer", {{"threadId", id}, {"expectedTurnId", turnId},
                                      {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}}, this,
                         [this, id, text](const QJsonObject &, const QString &error) {
        if (!steeringInFlight_ || id != threadId_) return;
        steeringInFlight_ = false;
        steeringText_.clear();
        if (error.isEmpty()) emit steerAccepted(text);
        else emit steerFailed(text, error);
        if (!busy_ && compactedHistoryPending_) refreshAfterCompaction();
        emit stateChanged();
        sendNextPrompt();
    });
    return true;
}

bool CodexAgent::canCompact() const
{
    return connection_ && connection_->isConnected() && !threadId_.isEmpty()
        && !busy_ && !threadOpening_ && !refreshingHistory_ && !steeringInFlight_ && queuedPrompts_.isEmpty();
}

bool CodexAgent::compact()
{
    if (!canCompact()) return false;
    busy_ = true;
    manualCompaction_ = true;
    activeTurnId_.clear();
    reasoningItems_.clear();
    stopRequested_ = false;
    stopSent_ = false;
    turnBaselineKnown_ = false;
    const QString id = threadId_;
    emit compactionStarted();
    emit message("[Compacting Codex context]");
    emit stateChanged();
    connection_->request("thread/compact/start", {{"threadId", id}}, this,
                         [this, id](const QJsonObject &, const QString &error) {
        if (id != threadId_ || error.isEmpty() || !manualCompaction_) return;
        resetTurn();
        emit message("[Could not compact Codex context: " + error + "]");
        emit stateChanged();
        sendNextPrompt();
    });
    return true;
}

void CodexAgent::loadHistory(const QString &id, const QString &, bool older)
{
    const quint64 generation = ++historyGeneration_;
    historyPending_ = false;
    if (!older || id != historyThreadId_) {
        historyThreadId_ = id;
        historyCursor_.clear();
        historyEntries_.clear();
    }
    if (!connection_ || !connection_->isConnected()) {
        historyPending_ = true;
        emit historyLoaded(id, historyEntries_, false, "Waiting for the Codex App Server to load this conversation.");
        return;
    }
    QJsonObject params{{"threadId", id}, {"limit", kHistoryPageSize}, {"sortDirection", "desc"}};
    if (older && !historyCursor_.isEmpty()) params.insert("cursor", historyCursor_);
    connection_->request("thread/items/list", params, this,
                         [this, generation, id](const QJsonObject &result, const QString &error) {
        if (generation != historyGeneration_) return;
        if (!error.isEmpty()) {
            emit historyLoaded(id, historyEntries_, false, "Could not load this conversation: " + error);
            return;
        }
        // Items arrive newest first; older pages are prepended to what is already loaded.
        QList<ChatEntry> page;
        const QJsonArray items = result.value("data").toArray();
        for (qsizetype i = items.size() - 1; i >= 0; --i)
            page.append(historyEntries(items.at(i).toObject().value("item").toObject()));
        historyEntries_ = page + historyEntries_;
        historyCursor_ = result.value("nextCursor").toString();
        emit historyLoaded(id, historyEntries_, !historyCursor_.isEmpty(), {});
    });
}

// Turn summaries carry each turn's first user message without the turn's commands and their output, so even a
// long thread is read quickly. Messages sent to a running turn are not in them.
void CodexAgent::loadUserMessages(const QString &id, const QString &)
{
    // Before the App Server is connected, the messages are read once it is.
    userMessagesPending_ = connection_ && !connection_->isConnected() ? id : QString();
    if (id.isEmpty() || !connection_ || !connection_->isConnected()) return;
    requestUserMessages(id, {}, ++userMessagesGeneration_, std::make_shared<QStringList>());
}

void CodexAgent::requestUserMessages(const QString &id, const QString &cursor, quint64 generation,
                                     const std::shared_ptr<QStringList> &messages)
{
    QJsonObject params{{"threadId", id}, {"limit", 100}, {"sortDirection", "asc"}, {"itemsView", "summary"}};
    if (!cursor.isEmpty()) params.insert("cursor", cursor);
    connection_->request("thread/turns/list", params, this,
                         [this, id, generation, messages](const QJsonObject &result, const QString &error) {
        if (generation != userMessagesGeneration_ || !error.isEmpty()) return;
        for (const QJsonValue &turn : result.value("data").toArray())
            for (const QJsonValue &item : turn.toObject().value("items").toArray())
                for (const ChatEntry &entry : historyEntries(item.toObject()))
                    if (entry.role == "user") messages->append(entry.text);
        const QString next = result.value("nextCursor").toString();
        if (!next.isEmpty() && connection_ && connection_->isConnected()) requestUserMessages(id, next, generation, messages);
        else emit userMessagesLoaded(id, *messages);
    });
}

void CodexAgent::refreshAfterCompaction()
{
    if (refreshingHistory_ || steeringInFlight_ || threadId_.isEmpty() || !connection_ || !connection_->isConnected()) return;
    compactedHistoryPending_ = false;
    refreshingHistory_ = true;
    const quint64 generation = ++historyGeneration_;
    historyPending_ = false;
    emit historyRefreshStarted();
    const QString id = threadId_;
    connection_->request("thread/items/list", {{"threadId", id}, {"limit", kHistoryPageSize}, {"sortDirection", "desc"}}, this,
                         [this, generation, id](const QJsonObject &result, const QString &error) {
        if (id != threadId_) return;
        if (generation != historyGeneration_) {
            refreshingHistory_ = false;
            emit stateChanged();
            sendNextPrompt();
            return;
        }
        if (!error.isEmpty()) {
            refreshingHistory_ = false;
            emit message("[Could not refresh the conversation after compaction: " + error + "]");
            emit stateChanged();
            sendNextPrompt();
            return;
        }
        QList<ChatEntry> page;
        const QJsonArray items = result.value("data").toArray();
        for (qsizetype i = items.size() - 1; i >= 0; --i) {
            const QJsonObject entry = items.at(i).toObject();
            page.append(historyEntries(entry.value("item").toObject()));
        }
        historyThreadId_ = id;
        historyEntries_ = page;
        historyCursor_ = result.value("nextCursor").toString();
        refreshingHistory_ = false;
        emit historyRefreshed(id, historyEntries_, !historyCursor_.isEmpty());
        emit stateChanged();
        sendNextPrompt();
    });
}

void CodexAgent::cancelHistory()
{
    ++historyGeneration_;
    historyPending_ = false;
    ++userMessagesGeneration_;
    userMessagesPending_.clear();
}

void CodexAgent::answerApproval(int id, ApprovalDecision decision)
{
    if (!serverRequests_.contains(id)) return;
    const QJsonValue requestId = serverRequests_.take(id);
    const QJsonArray rule = proposedRules_.take(id);
    const QStringList sessionRules = requestSessionRules_.take(id);
    const QString sessionRule = sessionRules.join(", ");
    if (requestId.isUndefined() || !connection_) return;
    if (permissionRequests_.contains(id)) {
        // The grant lasts for this turn or for the thread's session; Codex keeps it, nothing is saved.
        const QJsonObject permissions = permissionRequests_.take(id);
        const bool granted = decision == ApprovalDecision::Accept || decision == ApprovalDecision::AcceptAlways
            || decision == ApprovalDecision::AcceptForSession;
        const bool session = decision == ApprovalDecision::AcceptForSession;
        connection_->respond(requestId, {{"permissions", granted ? permissions : QJsonObject()},
                                         {"scope", session ? "session" : "turn"}});
        if (granted) {
            QStringList &roots = session ? sessionWriteRoots_ : turnWriteRoots_;
            for (const QString &root : permissionWriteRoots(permissions))
                if (!roots.contains(root)) roots.append(root);
        }
        emit message(QString("[Permissions: ") + (granted ? (session ? "granted for the session" : "granted for this turn")
                                                         : "declined") + "]");
        if (decision == ApprovalDecision::Cancel) interrupt();
        return;
    }
    if (!sessionRule.isEmpty() && decision == ApprovalDecision::AcceptForSession) {
        for (const QString &family : sessionRules) trustedSessionCommands_.insert(family);
        trustedConversationId_ = threadId_;
        decision = ApprovalDecision::Accept;
        emit message("[Trusted for this chat: " + sessionRule + "]");
    }
    if (decision == ApprovalDecision::AcceptAlways && !rule.isEmpty()) {
        connection_->respond(requestId, {{"decision", QJsonObject{
            {"acceptWithExecpolicyAmendment", QJsonObject{{"execpolicy_amendment", rule}}}}}});
        QStringList words;
        for (const QJsonValue &word : rule) words.append(word.toString());
        emit message("[Approval: always allow commands starting with " + words.join(' ') + "]");
        return;
    }
    const QString value = decision == ApprovalDecision::Accept || decision == ApprovalDecision::AcceptAlways ? "accept"
        : decision == ApprovalDecision::AcceptForSession ? "acceptForSession"
        : decision == ApprovalDecision::Cancel ? "cancel" : "decline";
    connection_->respond(requestId, {{"decision", value}});
    emit message("[Approval: " + value + "]");
}

// Codex keeps the approvals given for a session in the loaded thread, and has no request to forget them. A
// thread that no client uses is closed after about a minute; opened again from its file, it starts without
// them. The other Codex chats keep running meanwhile.
void CodexAgent::resetSessionApprovals()
{
    if (busy_ || threadOpening_ || steeringInFlight_ || !reopeningThreadId_.isEmpty()) {
        emit message("[Wait for Codex to finish before withdrawing its session approvals.]");
        return;
    }
    if (threadId_.isEmpty() || !connection_) return;
    reopeningThreadId_ = threadId_;
    closeThread();
    emit message("[Withdrawing Codex session approvals: the conversation reopens when Codex has closed it, in about a "
                 "minute]");
    reopenPolls_ = 0;
    if (!reopenTimer_) {
        reopenTimer_ = new QTimer(this);
        reopenTimer_->setInterval(5000);
        connect(reopenTimer_, &QTimer::timeout, this, &CodexAgent::pollReopening);
    }
    reopenTimer_->start();
    QTimer::singleShot(0, this, &CodexAgent::pollReopening);
    emit stateChanged();
}

void CodexAgent::pollReopening()
{
    if (reopeningThreadId_.isEmpty() || !connection_ || !connection_->isConnected()) {
        cancelReopening();
        return;
    }
    // After three minutes the conversation opens again, still loaded, and keeps its approvals.
    if (++reopenPolls_ > 36) {
        emit message("[Codex did not close the conversation; its session approvals stay until the App Server restarts]");
        reopenThread();
        return;
    }
    connection_->request("thread/loaded/list", {}, this, [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty() || reopeningThreadId_.isEmpty()) return;
        for (const QJsonValue &id : result.value("data").toArray())
            if (id.toString() == reopeningThreadId_) return;
        reopenThread();
    });
}

void CodexAgent::reopenThread()
{
    if (reopenTimer_) reopenTimer_->stop();
    const QString id = std::exchange(reopeningThreadId_, {});
    if (id.isEmpty() || !connection_) return;
    threadOpening_ = true;
    connection_->request("thread/resume", {{"threadId", id}}, this, [this, id](const QJsonObject &result, const QString &error) {
        threadOpening_ = false;
        if (!error.isEmpty()) {
            emit message("[Could not reopen the Codex conversation " + id + ": " + error + "]");
            emit stateChanged();
            return;
        }
        openThread(result, true);
        emit message("[Codex session approvals withdrawn]");
    });
}

void CodexAgent::cancelReopening()
{
    reopeningThreadId_.clear();
    if (reopenTimer_) reopenTimer_->stop();
}

QStringList CodexAgent::trustedSessionCommands() const
{
    QStringList rules = trustedSessionCommands_.values();
    rules.sort();
    return rules;
}

void CodexAgent::trustSessionCommands(const QStringList &rules)
{
    for (const QString &rule : rules) trustedSessionCommands_.insert(rule);
    trustedConversationId_ = threadId_;
    emit message("[Trusted for this chat: " + rules.join(", ") + "]");
}

bool CodexAgent::removeTrustedSessionCommand(const QString &rule)
{
    if (!trustedSessionCommands_.remove(rule)) return false;
    emit message("[Removed chat trust: " + rule + "]");
    return true;
}

void CodexAgent::answerQuestions(int id, const QHash<QString, QStringList> &answers)
{
    const QJsonValue requestId = serverRequests_.take(id);
    const QList<AgentQuestion> questions = pendingQuestions_.take(id);
    if (requestId.isUndefined() || !connection_) return;
    QJsonObject result;
    for (const AgentQuestion &question : questions) {
        const auto answer = answers.constFind(question.id);
        result.insert(question.id, QJsonObject{
            {"answers", answer != answers.constEnd() ? QJsonArray::fromStringList(*answer) : QJsonArray{}}});
    }
    connection_->respond(requestId, {{"answers", result}});
}

// Before a thread exists, a new chat shows the defaults it will start with.
QString CodexAgent::model() const
{
    return model_.isEmpty() && threadId_.isEmpty() && !modelChosen_ && connection_ ? connection_->defaultModel() : model_;
}

QString CodexAgent::effort() const
{
    return effort_.isEmpty() && threadId_.isEmpty() && !modelChosen_ && connection_ ? connection_->defaultEffort() : effort_;
}

void CodexAgent::setReadOnly(bool readOnly)
{
    readOnly_ = readOnly;
    sandboxChosen_ = true;
    emit stateChanged();
}

// Read-only turns use Codex's read-only sandbox, which also stops shell commands from writing.
// Switching back restores the thread's own policy, or workspace write when the thread began read-only.
QStringList CodexAgent::writableDirectories() const
{
    if (readOnly_) return {};
    QStringList directories;
    for (const QJsonValue &root : sandboxPolicy().value("writableRoots").toArray()) directories.append(root.toString());
    for (const QString &root : sessionWriteRoots_ + turnWriteRoots_)
        if (!directories.contains(root)) directories.append(root);
    return directories;
}

QJsonObject CodexAgent::sandboxPolicy() const
{
    if (readOnly_) return {{"type", "readOnly"}};
    if (!threadSandbox_.isEmpty() && threadSandbox_.value("type").toString() != "readOnly") return threadSandbox_;
    return {{"type", "workspaceWrite"}};
}

void CodexAgent::setModel(const QString &model, const QString &effort)
{
    model_ = model;
    effort_ = effort;
    modelChosen_ = true;
    emit stateChanged();
}

// Model and effort changes are sent with the next turn/start; the server keeps them for later turns.
void CodexAgent::openThread(const QJsonObject &result, bool resumed)
{
    threadId_ = result.value("thread").toObject().value("id").toString();
    threadSandbox_ = result.value("sandbox").toObject();
    if (!sandboxChosen_) readOnly_ = threadSandbox_.value("type").toString() == "readOnly";
    if (!modelChosen_) {
        model_ = result.value("model").toString();
        effort_ = result.value("reasoningEffort").toString();
        // A thread started here uses the application defaults; turn/start sends them.
        if (!resumed && connection_) {
            if (!connection_->defaultModel().isEmpty()) model_ = connection_->defaultModel();
            if (!connection_->defaultEffort().isEmpty()) effort_ = connection_->defaultEffort();
            modelChosen_ = !connection_->defaultModel().isEmpty() || !connection_->defaultEffort().isEmpty();
        }
    }
    if (threadId_.isEmpty()) {
        emit message("[Server did not return a conversation ID.]");
    } else {
        emit message(resumed ? "[Resumed Codex conversation: " + threadId_ + "]" : "[Connected to Codex]");
        connection_->registerThread(threadId_, this, workingDirectory_);
    }
    emit conversationOpened(threadId_, resumed);
    sendNextPrompt();
    emit stateChanged();
}

// Detaches this chat from its thread so the server can unload it once no client uses it.
void CodexAgent::closeThread()
{
    for (int id : serverRequests_.keys()) emit requestResolved(id);
    serverRequests_.clear();
    pendingQuestions_.clear();
    proposedRules_.clear();
    requestSessionRules_.clear();
    permissionRequests_.clear();
    sessionWriteRoots_.clear();
    if (connection_ && !threadId_.isEmpty()) {
        connection_->unregisterThread(threadId_);
        connection_->request("thread/unsubscribe", {{"threadId", threadId_}});
    }
    threadId_.clear();
    if (refreshingHistory_) ++historyGeneration_;
    refreshingHistory_ = false;
    compactedHistoryPending_ = false;
    resetTurn();
}

void CodexAgent::startThread()
{
    if (!connection_ || !connection_->isConnected() || threadOpening_) return;
    threadOpening_ = true;
    emit message("[Starting a new conversation]");
    QJsonObject params{{"cwd", workingDirectory_}, {"serviceName", "agentin"}};
    const QString startModel = modelChosen_ ? model_ : connection_->defaultModel();
    if (!startModel.isEmpty()) params.insert("model", startModel);
    if (sandboxChosen_) params.insert("sandbox", readOnly_ ? "read-only" : "workspace-write");
    if (ephemeral_) {
        params.insert("ephemeral", true);
        params.insert("approvalPolicy", "never");
    }
    connection_->request("thread/start", params, this,
                         [this](const QJsonObject &result, const QString &error) {
        threadOpening_ = false;
        if (!error.isEmpty()) {
            if (!queuedPrompts_.isEmpty()) {
                queuedPrompts_.clear();
                emit turnCompleted("failed", "the conversation could not be started: " + error);
            }
            emit stateChanged();
            return;
        }
        openThread(result, false);
    });
    emit stateChanged();
}

void CodexAgent::setFastMode(bool fast)
{
    if (fastMode_ == fast) return;
    fastMode_ = fast;
    emit stateChanged();
}

void CodexAgent::sendNextPrompt()
{
    if (!connection_ || !connection_->isConnected() || threadId_.isEmpty() || busy_ || refreshingHistory_ || steeringInFlight_ || queuedPrompts_.isEmpty()) return;
    const QString text = queuedPrompts_.takeFirst();
    busy_ = true;
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
    turnBaselineKnown_ = false;
    QJsonObject params{{"threadId", threadId_}, {"summary", "detailed"},
                       {"serviceTierForTurn", fastMode_ ? "fast" : "default"},
                       {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
    if (modelChosen_ && !model_.isEmpty()) params.insert("model", model_);
    if (modelChosen_ && !effort_.isEmpty()) params.insert("effort", effort_);
    if (sandboxChosen_) params.insert("sandboxPolicy", sandboxPolicy());
    connection_->request("turn/start", params, this, [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            resetTurn();
            emit turnCompleted("failed", error);
            sendNextPrompt();
        } else if (busy_) {
            activeTurnId_ = result.value("turn").toObject().value("id").toString();
            sendStopIfPossible();
        }
        emit stateChanged();
    });
    emit stateChanged();
}

// turn/interrupt needs the turn ID, which may arrive after the user asked to stop.
void CodexAgent::sendStopIfPossible()
{
    if (!busy_ || !stopRequested_ || stopSent_ || threadId_.isEmpty() || activeTurnId_.isEmpty()) return;
    stopSent_ = true;
    connection_->request("turn/interrupt", {{"threadId", threadId_}, {"turnId", activeTurnId_}}, this,
                         [this](const QJsonObject &, const QString &error) {
        if (error.isEmpty()) return;
        stopRequested_ = false;
        stopSent_ = false;
        emit stateChanged();
    });
}

void CodexAgent::showOutputOf(const QString &itemId)
{
    if (itemId == outputCommand_) return;
    outputCommand_ = itemId;
    const QString command = runningCommands_.value(itemId);
    emit toolStarted("shell", (command.isEmpty() ? QString("output") : command) + " (continued)");
}

void CodexAgent::resetTurn()
{
    fileChanges_.clear();
    runningCommands_.clear();
    outputCommand_.clear();
    busy_ = false;
    manualCompaction_ = false;
    emit compactionFinished();
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
}

void CodexAgent::handleNotification(const QString &method, const QJsonObject &params)
{
    const QJsonObject item = params.value("item").toObject();
    const QString itemId = params.value("itemId").toString(item.value("id").toString());
    if (method == "serverRequest/resolved") {
        const QJsonValue resolved = params.value("requestId");
        for (auto it = serverRequests_.begin(); it != serverRequests_.end(); ++it) {
            if (it.value() != resolved) continue;
            const int id = it.key();
            serverRequests_.erase(it);
            pendingQuestions_.remove(id);
            proposedRules_.remove(id);
            requestSessionRules_.remove(id);
            permissionRequests_.remove(id);
            emit requestResolved(id);
            break;
        }
    } else if (method == "turn/started") {
        if (busy_) {
            activeTurnId_ = params.value("turn").toObject().value("id").toString();
            sendStopIfPossible();
        }
        emit stateChanged();
    } else if (method == "item/agentMessage/delta") {
        if (!streamedMessages_.contains(itemId)) {
            streamedMessages_.insert(itemId);
            emit messageStarted();
        }
        emit messageDelta(params.value("delta").toString());
    } else if (method == "item/reasoning/summaryTextDelta" || method == "item/reasoning/textDelta") {
        QJsonObject &snapshot = reasoningItems_[itemId];
        const bool raw = method == "item/reasoning/textDelta";
        const QString field = raw ? "content" : "summary";
        const int index = params.value(raw ? "contentIndex" : "summaryIndex").toInt();
        if (index < 0 || index > 10000) return;
        QJsonArray sections = snapshot.value(field).toArray();
        while (sections.size() <= index) sections.append("");
        sections[index] = sections.at(index).toString() + params.value("delta").toString();
        snapshot.insert(field, sections);
        emit reasoningUpdated(itemId, reasoningText(snapshot));
    } else if (method == "item/commandExecution/outputDelta") {
        streamedCommands_.insert(itemId);
        showOutputOf(itemId);
        emit toolOutput(params.value("delta").toString());
    } else if (method == "item/started" && item.value("type") == "contextCompaction") {
        emit compactionStarted();
    } else if (method == "item/started" && item.value("type") == "commandExecution") {
        runningCommands_.insert(itemId, item.value("command").toString());
        outputCommand_ = itemId;
        emit toolStarted("shell", item.value("command").toString());
    } else if (method == "item/started" && item.value("type") == "fileChange") {
        // An approval for file changes names no files; they come with the item and its patch updates.
        fileChanges_.insert(item.value("id").toString(), item.value("changes").toArray());
    } else if (method == "item/fileChange/patchUpdated") {
        fileChanges_.insert(params.value("itemId").toString(), params.value("changes").toArray());
    } else if (method == "item/completed") {
        const QString type = item.value("type").toString();
        const QString completedId = item.value("id").toString();
        if (type == "agentMessage") {
            if (!streamedMessages_.remove(completedId)) {
                emit messageStarted();
                emit messageDelta(item.value("text").toString());
            }
            emit messageFinished();
        } else if (type == "reasoning") {
            const QString text = reasoningText(item);
            if (!text.isEmpty()) emit reasoningUpdated(completedId, text);
            reasoningItems_.remove(completedId);
        } else if (type == "commandExecution") {
            if (!streamedCommands_.remove(completedId)) {
                const QString output = item.value("aggregatedOutput").toString();
                if (!output.isEmpty()) {
                    showOutputOf(completedId);
                    emit toolOutput(output);
                }
            }
            // With commands running in parallel, the status says which one ended.
            const QString command = runningCommands_.value(completedId, item.value("command").toString());
            const bool parallel = runningCommands_.size() > 1 || outputCommand_ != completedId;
            runningCommands_.remove(completedId);
            outputCommand_.clear();
            emit toolFinished(parallel ? "shell " + command.simplified().left(60) : QString("shell"),
                              item.value("status").toString());
        } else if (type == "fileChange") {
            emit toolFinished("file changes", item.value("status").toString());
        } else if (type == "contextCompaction") {
            compactedHistoryPending_ = true;
            emit compactionFinished();
            emit contextCompacted();
            if (!busy_) refreshAfterCompaction();
        }
    } else if (method == "thread/compacted") {
        // Older App Servers report the same event without a contextCompaction item.
        compactedHistoryPending_ = true;
        emit compactionFinished();
        emit contextCompacted();
        if (!busy_) refreshAfterCompaction();
    } else if (method == "turn/completed") {
        const QJsonObject turn = params.value("turn").toObject();
        const bool wasCompaction = manualCompaction_;
        resetTurn();
        if (wasCompaction) {
            const QString status = turn.value("status").toString();
            if (status != "completed")
                emit message("[Codex compaction: " + status + ": " + turn.value("error").toObject().value("message").toString() + "]");
        } else {
            emit turnCompleted(turn.value("status").toString(), turn.value("error").toObject().value("message").toString());
        }
        if (compactedHistoryPending_) refreshAfterCompaction();
        emit stateChanged();
        sendNextPrompt();
    } else if (method == "thread/tokenUsage/updated") {
        // "total" covers the thread and "last" the latest request; a turn can make several requests,
        // so its usage is the total minus the total before its first request.
        const QJsonObject report = params.value("tokenUsage").toObject();
        const auto read = [&report](const QJsonObject &breakdown) {
            TokenUsage usage;
            usage.input = breakdown.value("inputTokens").toInteger(-1);
            usage.cached = breakdown.value("cachedInputTokens").toInteger(-1);
            usage.output = breakdown.value("outputTokens").toInteger(-1);
            usage.reasoning = breakdown.value("reasoningOutputTokens").toInteger(-1);
            usage.total = breakdown.value("totalTokens").toInteger(-1);
            usage.contextWindow = report.value("modelContextWindow").toInteger(-1);
            return usage;
        };
        TokenUsage total = read(report.value("total").toObject());
        const TokenUsage last = read(report.value("last").toObject());
        total.contextUsed = last.total;
        if (!turnBaselineKnown_) {
            turnBaselineKnown_ = true;
            const auto before = [](qint64 all, qint64 latest) { return all < 0 || latest < 0 ? qint64(-1) : all - latest; };
            turnBaseline_.input = before(total.input, last.input);
            turnBaseline_.cached = before(total.cached, last.cached);
            turnBaseline_.output = before(total.output, last.output);
            turnBaseline_.reasoning = before(total.reasoning, last.reasoning);
            turnBaseline_.total = before(total.total, last.total);
        }
        const auto since = [](qint64 all, qint64 start) { return all < 0 ? qint64(-1) : all - qMax<qint64>(start, 0); };
        TokenUsage turn = total;
        turn.input = since(total.input, turnBaseline_.input);
        turn.cached = since(total.cached, turnBaseline_.cached);
        turn.output = since(total.output, turnBaseline_.output);
        turn.reasoning = since(total.reasoning, turnBaseline_.reasoning);
        turn.total = since(total.total, turnBaseline_.total);
        emit turnUsage(turn);
        emit conversationUsage(total);
    } else if (method == "error") {
        emit message("[Error] " + params.value("error").toObject().value("message").toString());
    }
}

void CodexAgent::handleServerRequest(const QString &method, const QJsonValue &id, const QJsonObject &params)
{
    if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval") {
        QStringList sessionRules;
        CommandVerdict verdict;
        const bool command = method == "item/commandExecution/requestApproval";
        // A request for network access needs its own decision, so neither the rules nor chat trust allow it;
        // nor does input for a command that runs already, which may be a shell reading it as commands.
        const bool network = command && !params.value("networkApprovalContext").toObject().isEmpty();
        const bool input = command && params.value("kind").toString() == "writeStdin";
        if (command) {
            // agentin's rules decide before asking; Codex's own rules acted already.
            CommandContext context;
            context.directory = params.value("cwd").toString(workingDirectory_);
            context.writable = writableDirectories() + QStringList{workingDirectory_, "/tmp", QDir::tempPath()};
            if (!network && !input) context.trusted = trustedSessionCommands_.values();
            verdict = commandRuleVerdict(params.value("command").toString(), context);
            if (verdict.decision == CommandDecision::Deny) {
                connection_->respond(id, {{"decision", "decline"}});
                emit message("[Declined by agentin's rules: " + verdict.reason + "]");
                return;
            }
            if (!network && !input) {
                if (verdict.decision == CommandDecision::Allow && !isReadOnly()) {
                    connection_->respond(id, {{"decision", "accept"}});
                    emit message("[Allowed by agentin's rules: " + verdict.reason + "]");
                    return;
                }
                // Each command that asks can be trusted for the chat on its own.
                sessionRules = verdict.sessionRules();
            }
        }
        const QString sessionRule = sessionRules.join(", ");
        QString description = params.value("reason").toString();
        if (method == "item/commandExecution/requestApproval") {
            const QJsonObject network = params.value("networkApprovalContext").toObject();
            if (!network.isEmpty()) {
                description += "\nNetwork access: " + network.value("protocol").toString()
                    + "://" + network.value("host").toString();
            } else if (input) {
                description += "\nInput for a running command:\n" + params.value("command").toString();
            } else {
                description += "\n" + params.value("command").toString();
                description += "\nDirectory: " + params.value("cwd").toString();
                if (!verdict.findings.isEmpty()) description += "\n\n" + verdict.explanation();
                // Say why agentin did not answer a command that its rules allow.
                else if (verdict.decision == CommandDecision::Allow && isReadOnly())
                    description += "\n\nagentin's rules allow this command, but this chat is read-only, so it asks.";
            }
        } else {
            QStringList files;
            QString diffs;
            for (const QJsonValue &value : fileChanges_.value(params.value("itemId").toString())) {
                const QJsonObject change = value.toObject();
                const QJsonObject kind = change.value("kind").toObject();
                const QString type = kind.value("type").toString();
                const QString path = change.value("path").toString();
                const QString moved = kind.value("move_path").toString();
                files.append((type == "add" ? "add " : type == "delete" ? "delete " : "change ") + path
                             + (moved.isEmpty() ? QString() : " \u2192 " + moved));
                if (!change.value("diff").toString().isEmpty())
                    diffs += "\n\n--- " + path + "\n" + change.value("diff").toString().trimmed();
            }
            description += files.isEmpty() ? QString("\nCodex asks to change files without naming them.")
                                           : "\nFiles:\n" + files.join('\n');
            if (!params.value("grantRoot").toString().isEmpty())
                description += "\nAllow writes under: " + params.value("grantRoot").toString();
            description += diffs;
        }
        const int requestId = nextServerRequest_++;
        serverRequests_.insert(requestId, id);
        QStringList proposed;
        for (const QJsonValue &word : params.value("proposedExecpolicyAmendment").toArray()) proposed.append(word.toString());
        const QJsonArray rule = QJsonArray::fromStringList(lastingRulePrefix(proposed));
        QString alwaysRule;
        if (!sessionRules.isEmpty()) requestSessionRules_.insert(requestId, sessionRules);
        // Always on a command adds agentin's lines only, without a Codex rule, so that Codex keeps asking and
        // agentin's rules, with their denials, keep deciding. Network access is granted by a Codex rule.
        if (network && !rule.isEmpty()) {
            proposedRules_.insert(requestId, rule);
            QStringList words;
            for (const QJsonValue &word : rule) words.append(word.toString());
            alwaysRule = "Allow commands starting with \"" + words.join(' ') + "\" without asking";
        }
        emit approvalRequested(requestId, "Approve Codex action", description.trimmed(), true, alwaysRule, sessionRule,
                               network || input ? QList<ApprovalChoice>() : verdict.choices());
    } else if (method == "item/permissions/requestApproval") {
        const QJsonObject permissions = params.value("permissions").toObject();
        QStringList lines{params.value("reason").toString()};
        for (const auto &[access, path] : permissionEntries(permissions)) {
            const QString label = access == "write" ? "Write" : (access == "read" ? "Read" : "No access");
            lines.append(label + ": " + path);
        }
        if (permissions.value("network").toObject().value("enabled").toBool()) lines.append("Network access");
        lines.append("Directory: " + params.value("cwd").toString());
        lines.removeAll(QString());
        const int requestId = nextServerRequest_++;
        serverRequests_.insert(requestId, id);
        permissionRequests_.insert(requestId, permissions);
        emit approvalRequested(requestId, "Codex asks for additional permissions", lines.join('\n'), true, {});
    } else if (method == "item/tool/requestUserInput" || method == "tool/requestUserInput") {
        QList<AgentQuestion> questions;
        for (const QJsonValue &value : params.value("questions").toArray()) {
            const QJsonObject object = value.toObject();
            AgentQuestion question;
            question.id = object.value("id").toString();
            question.header = object.value("header").toString("Codex question");
            question.text = object.value("question").toString();
            question.allowOther = object.value("isOther").toBool();
            question.secret = object.value("isSecret").toBool();
            for (const QJsonValue &option : object.value("options").toArray()) {
                question.options.append(option.toObject().value("label").toString());
                question.optionDescriptions.append(option.toObject().value("description").toString());
            }
            questions.append(question);
        }
        const int requestId = nextServerRequest_++;
        serverRequests_.insert(requestId, id);
        pendingQuestions_.insert(requestId, questions);
        emit questionsRequested(requestId, questions, params.value("isBlocking").toBool(true));
    } else {
        emit message("[Unsupported server request: " + method + "]");
        connection_->respondError(id, -32601, "Unsupported request");
    }
}
