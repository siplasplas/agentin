#include "CodexAgent.h"

#include "CodexConnection.h"

#include <QJsonArray>

namespace {
constexpr int kHistoryPageSize = 40;

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

CodexAgent::CodexAgent(CodexConnection *connection, const QString &workingDirectory, QObject *parent)
    : AgentBackend(parent), connection_(connection), workingDirectory_(workingDirectory)
{
    connect(connection, &CodexConnection::stateChanged, this, &AgentBackend::stateChanged);
    connect(connection, &CodexConnection::connected, this, [this] {
        if (historyPending_) loadHistory(historyThreadId_, {}, false);
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
        threadId_.clear();
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
    if (busy_ || threadOpening_) {
        emit message("[Wait for the current Codex response to finish.]");
        return false;
    }
    closeThread();
    workingDirectory_ = workingDirectory;
    queuedPrompts_.clear();
    startThread();
    return true;
}

bool CodexAgent::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (id.isEmpty() || !connection_ || !connection_->isConnected()) return false;
    if (busy_ || threadOpening_) {
        emit message("[Wait for the current Codex response to finish.]");
        return false;
    }
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
    if (threadId_.isEmpty()) startThread();
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

void CodexAgent::refreshAfterCompaction()
{
    if (refreshingHistory_ || threadId_.isEmpty() || !connection_ || !connection_->isConnected()) return;
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
            sendNextPrompt();
            return;
        }
        if (!error.isEmpty()) {
            refreshingHistory_ = false;
            emit message("[Could not refresh the conversation after compaction: " + error + "]");
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
        sendNextPrompt();
    });
}

void CodexAgent::cancelHistory()
{
    ++historyGeneration_;
    historyPending_ = false;
}

void CodexAgent::answerApproval(int id, ApprovalDecision decision)
{
    const QJsonValue requestId = serverRequests_.take(id);
    const QJsonArray rule = proposedRules_.take(id);
    if (requestId.isUndefined() || !connection_) return;
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
    QJsonObject params{{"cwd", workingDirectory_}, {"serviceName", "agentdeskt"}};
    const QString startModel = modelChosen_ ? model_ : connection_->defaultModel();
    if (!startModel.isEmpty()) params.insert("model", startModel);
    if (sandboxChosen_ && readOnly_) params.insert("sandbox", "read-only");
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

void CodexAgent::sendNextPrompt()
{
    if (!connection_ || !connection_->isConnected() || threadId_.isEmpty() || busy_ || refreshingHistory_ || queuedPrompts_.isEmpty()) return;
    const QString text = queuedPrompts_.takeFirst();
    busy_ = true;
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
    turnBaselineKnown_ = false;
    QJsonObject params{{"threadId", threadId_},
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

void CodexAgent::resetTurn()
{
    busy_ = false;
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
}

void CodexAgent::handleNotification(const QString &method, const QJsonObject &params)
{
    const QJsonObject item = params.value("item").toObject();
    const QString itemId = params.value("itemId").toString(item.value("id").toString());
    if (method == "turn/started") {
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
    } else if (method == "item/commandExecution/outputDelta") {
        streamedCommands_.insert(itemId);
        emit toolOutput(params.value("delta").toString());
    } else if (method == "item/started" && item.value("type") == "commandExecution") {
        emit toolStarted("shell", item.value("command").toString());
    } else if (method == "item/completed") {
        const QString type = item.value("type").toString();
        const QString completedId = item.value("id").toString();
        if (type == "agentMessage") {
            if (!streamedMessages_.remove(completedId)) {
                emit messageStarted();
                emit messageDelta(item.value("text").toString());
            }
            emit messageFinished();
        } else if (type == "commandExecution") {
            if (!streamedCommands_.remove(completedId)) {
                const QString output = item.value("aggregatedOutput").toString();
                if (!output.isEmpty()) emit toolOutput(output);
            }
            emit toolFinished("shell", item.value("status").toString());
        } else if (type == "fileChange") {
            emit toolFinished("file changes", item.value("status").toString());
        } else if (type == "contextCompaction") {
            compactedHistoryPending_ = true;
            emit contextCompacted();
            if (!busy_) refreshAfterCompaction();
        }
    } else if (method == "thread/compacted") {
        // Older App Servers report the same event without a contextCompaction item.
        compactedHistoryPending_ = true;
        emit contextCompacted();
        if (!busy_) refreshAfterCompaction();
    } else if (method == "turn/completed") {
        const QJsonObject turn = params.value("turn").toObject();
        resetTurn();
        emit turnCompleted(turn.value("status").toString(), turn.value("error").toObject().value("message").toString());
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
        QString description = params.value("reason").toString();
        if (method == "item/commandExecution/requestApproval") {
            const QJsonObject network = params.value("networkApprovalContext").toObject();
            if (!network.isEmpty()) {
                description += "\nNetwork access: " + network.value("protocol").toString()
                    + "://" + network.value("host").toString();
            } else {
                description += "\n" + params.value("command").toString();
                description += "\nDirectory: " + params.value("cwd").toString();
            }
        } else {
            description += "\n" + params.value("grantRoot").toString();
        }
        const int requestId = nextServerRequest_++;
        serverRequests_.insert(requestId, id);
        const QJsonArray rule = params.value("proposedExecpolicyAmendment").toArray();
        QString alwaysRule;
        if (!rule.isEmpty()) {
            proposedRules_.insert(requestId, rule);
            QStringList words;
            for (const QJsonValue &word : rule) words.append(word.toString());
            alwaysRule = "Allow commands starting with \"" + words.join(' ') + "\" without asking";
        }
        emit approvalRequested(requestId, "Approve Codex action", description.trimmed(), true, alwaysRule);
    } else if (method == "item/tool/requestUserInput") {
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
        emit questionsRequested(requestId, questions);
    } else {
        emit message("[Unsupported server request: " + method + "]");
        connection_->respondError(id, -32601, "Unsupported request");
    }
}
