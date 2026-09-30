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
        return {{"tool", "$ " + item.value("command").toString()}};
    } else if (type == "fileChange") {
        return {{"tool", "file changes"}};
    }
    return {};
}
}

CodexAgent::CodexAgent(CodexConnection *connection, const QString &workingDirectory, QObject *parent)
    : AgentBackend(parent), connection_(connection), workingDirectory_(workingDirectory)
{
    // Until chats get their own tabs, the only Codex chat also reports the connection's messages and list.
    connect(connection, &CodexConnection::message, this, &AgentBackend::message);
    connect(connection, &CodexConnection::stateChanged, this, &AgentBackend::stateChanged);
    connect(connection, &CodexConnection::conversationsChanged, this, &AgentBackend::conversationsChanged);
    connect(connection, &CodexConnection::connected, this, [this] {
        if (historyPending_) loadHistory(historyThreadId_, {}, false);
    });
    connect(connection, &CodexConnection::disconnected, this, [this] {
        threadOpening_ = false;
        threadId_.clear();
        resetTurn();
    });
}

CodexAgent::~CodexAgent()
{
    closeThread();
}

QString CodexAgent::program() const
{
    return connection_ ? connection_->program() : QString();
}

QString CodexAgent::statusText() const
{
    if (!connection_ || !connection_->isRunning()) return "Server: not running";
    if (busy_) return "Codex is responding…";
    if (threadOpening_) return "Opening Codex conversation…";
    if (connection_->isConnected()) return "Codex ready";
    return "Connecting to Codex App Server…";
}

AgentHelp CodexAgent::help() const
{
    return {{"Codex connection: codex app-server --stdio (direct JSONL).",
             "Installed Codex App Server commands and options (reference; CLI subcommands are not chat messages):"},
            "App Server", program(), {"app-server", "--help"}};
}

void CodexAgent::loadConversations()
{
    if (connection_) connection_->loadConversations();
}

void CodexAgent::refreshConversations()
{
    if (connection_) connection_->refreshConversations();
}

QList<QJsonObject> CodexAgent::conversations() const
{
    return connection_ ? connection_->conversations() : QList<QJsonObject>{};
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
        openThread(result.value("thread").toObject().value("id").toString(), true);
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

void CodexAgent::cancelHistory()
{
    ++historyGeneration_;
    historyPending_ = false;
}

void CodexAgent::answerApproval(int id, bool allow)
{
    const QJsonValue requestId = serverRequests_.take(id);
    if (requestId.isUndefined() || !connection_) return;
    const QString decision = allow ? "accept" : "decline";
    connection_->respond(requestId, {{"decision", decision}});
    emit message("[Approval: " + decision + "]");
}

void CodexAgent::answerQuestions(int id, const QHash<QString, QString> &answers)
{
    const QJsonValue requestId = serverRequests_.take(id);
    const QList<AgentQuestion> questions = pendingQuestions_.take(id);
    if (requestId.isUndefined() || !connection_) return;
    QJsonObject result;
    for (const AgentQuestion &question : questions) {
        const auto answer = answers.constFind(question.id);
        result.insert(question.id, QJsonObject{
            {"answers", answer != answers.constEnd() ? QJsonArray{*answer} : QJsonArray{}}});
    }
    connection_->respond(requestId, {{"answers", result}});
}

void CodexAgent::openThread(const QString &threadId, bool resumed)
{
    threadId_ = threadId;
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

void CodexAgent::closeThread()
{
    if (connection_) connection_->unregisterThread(threadId_);
    threadId_.clear();
    resetTurn();
}

void CodexAgent::startThread()
{
    if (!connection_ || !connection_->isConnected() || threadOpening_) return;
    threadOpening_ = true;
    emit message("[Starting a new conversation]");
    connection_->request("thread/start", {{"cwd", workingDirectory_}, {"serviceName", "agentdeskt"}}, this,
                         [this](const QJsonObject &result, const QString &error) {
        threadOpening_ = false;
        if (!error.isEmpty()) {
            emit stateChanged();
            return;
        }
        openThread(result.value("thread").toObject().value("id").toString(), false);
    });
    emit stateChanged();
}

void CodexAgent::sendNextPrompt()
{
    if (!connection_ || !connection_->isConnected() || threadId_.isEmpty() || busy_ || queuedPrompts_.isEmpty()) return;
    const QString text = queuedPrompts_.takeFirst();
    busy_ = true;
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
    QJsonObject params{{"threadId", threadId_},
                       {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
    connection_->request("turn/start", params, this, [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            resetTurn();
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
        }
    } else if (method == "turn/completed") {
        const QJsonObject turn = params.value("turn").toObject();
        resetTurn();
        emit turnCompleted(turn.value("status").toString(), turn.value("error").toObject().value("message").toString());
        emit stateChanged();
        sendNextPrompt();
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
        emit approvalRequested(requestId, "Approve Codex action", description.trimmed());
    } else if (method == "item/tool/requestUserInput") {
        QList<AgentQuestion> questions;
        for (const QJsonValue &value : params.value("questions").toArray()) {
            const QJsonObject object = value.toObject();
            AgentQuestion question;
            question.id = object.value("id").toString();
            question.header = object.value("header").toString("Codex question");
            question.text = object.value("question").toString();
            for (const QJsonValue &option : object.value("options").toArray())
                question.options.append(option.toObject().value("label").toString());
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
