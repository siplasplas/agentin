#include "CodexAgent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>

#include <algorithm>

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

void limitPreview(QJsonObject &thread)
{
    if (!thread.value("preview").isString()) return;
    QString preview = thread.value("preview").toString().simplified();
    constexpr int limit = 200;
    if (preview.size() > limit) preview = preview.left(limit - 1) + QChar(0x2026);
    thread.insert("preview", preview);
}
}

CodexAgent::CodexAgent(const QString &program, const QString &workingDirectory, const QString &indexPath,
                       QObject *parent)
    : AgentBackend(parent), program_(program), workingDirectory_(workingDirectory), indexPath_(indexPath),
      server_(new QProcess(this))
{
    connect(server_, &QProcess::started, this, [this] {
        sendRequest("initialize", {{"clientInfo", QJsonObject{
            {"name", "agentdeskt"}, {"title", "agentdeskt Qt"}, {"version", "0.1.0"}}}});
    });
    connect(server_, &QProcess::readyReadStandardOutput, this, [this] {
        readBuffer_ += server_->readAllStandardOutput();
        qsizetype newline;
        while ((newline = readBuffer_.indexOf('\n')) >= 0) {
            const QByteArray line = readBuffer_.left(newline).trimmed();
            readBuffer_.remove(0, newline + 1);
            if (!line.isEmpty()) handleLine(line);
        }
    });
    connect(server_, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(server_->readAllStandardError()).trimmed();
        if (!text.isEmpty()) emit message("[Server] " + text);
    });
    connect(server_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit message("[Server startup error] " + server_->errorString());
        emit message("Executable: " + program_);
        emit message("Working directory: " + server_->workingDirectory());
        emit message("Try --codex /absolute/path/to/codex if the executable was not found.");
        emit stateChanged();
    });
    connect(server_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        if (syncing_) {
            syncing_ = false;
            stagedConversations_.clear();
            emit message("[Codex conversation sync stopped: server disconnected]");
        }
        initialized_ = false;
        busy_ = false;
        threadOpening_ = false;
        stopRequested_ = false;
        stopSent_ = false;
        threadId_.clear();
        activeTurnId_.clear();
        emit disconnected();
        emit message(QString("[Server exited with code %1]").arg(code));
        emit stateChanged();
    });
}

CodexAgent::~CodexAgent()
{
    disconnect(server_, nullptr, this, nullptr);
    if (server_->state() != QProcess::NotRunning) {
        server_->terminate();
        if (!server_->waitForFinished(1000)) {
            server_->kill();
            server_->waitForFinished(1000);
        }
    }
}

void CodexAgent::start()
{
    server_->setWorkingDirectory(workingDirectory_);
    server_->start(program_, {"app-server", "--stdio"});
}

bool CodexAgent::isRunning() const
{
    return server_->state() != QProcess::NotRunning;
}

QString CodexAgent::statusText() const
{
    if (!isRunning()) return "Server: not running";
    if (busy_) return "Codex is responding…";
    if (threadOpening_) return "Opening Codex conversation…";
    if (initialized_) return "Codex ready";
    return "Connecting to Codex App Server…";
}

bool CodexAgent::newConversation(const QString &workingDirectory)
{
    if (!initialized_) {
        emit message("[Codex App Server is not connected.]");
        return false;
    }
    if (busy_ || threadOpening_) {
        emit message("[Wait for the current Codex response to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    queuedPrompts_.clear();
    threadId_.clear();
    activeTurnId_.clear();
    startThread();
    return true;
}

bool CodexAgent::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (id.isEmpty() || !initialized_) return false;
    if (busy_ || threadOpening_) {
        emit message("[Wait for the current Codex response to finish.]");
        return false;
    }
    if (!workingDirectory.isEmpty()) workingDirectory_ = workingDirectory;
    queuedPrompts_.clear();
    activeTurnId_.clear();
    threadId_.clear();
    threadOpening_ = true;
    resumingThreadId_ = id;
    emit message("[Resuming Codex conversation: " + id + "]");
    sendRequest("thread/resume", {{"threadId", id}});
    emit stateChanged();
    return true;
}

bool CodexAgent::prompt(const QString &text)
{
    if (!isRunning()) {
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
    historyRequest_ = 0;
    historyPending_ = false;
    if (!older || id != historyThreadId_) {
        historyThreadId_ = id;
        historyCursor_.clear();
        historyEntries_.clear();
    }
    if (!initialized_) {
        historyPending_ = true;
        emit historyLoaded(id, historyEntries_, false, "Waiting for the Codex App Server to load this conversation.");
        return;
    }
    QJsonObject params{{"threadId", id}, {"limit", kHistoryPageSize}, {"sortDirection", "desc"}};
    if (older && !historyCursor_.isEmpty()) params.insert("cursor", historyCursor_);
    historyRequest_ = sendRequest("thread/items/list", params);
}

void CodexAgent::cancelHistory()
{
    historyRequest_ = 0;
    historyPending_ = false;
}

void CodexAgent::answerApproval(int id, bool allow)
{
    const QJsonValue requestId = serverRequests_.take(id);
    if (requestId.isUndefined()) return;
    const QString decision = allow ? "accept" : "decline";
    sendJson({{"id", requestId}, {"result", QJsonObject{{"decision", decision}}}});
    emit message("[Approval: " + decision + "]");
}

void CodexAgent::answerQuestions(int id, const QHash<QString, QString> &answers)
{
    const QJsonValue requestId = serverRequests_.take(id);
    const QList<AgentQuestion> questions = pendingQuestions_.take(id);
    if (requestId.isUndefined()) return;
    QJsonObject result;
    for (const AgentQuestion &question : questions) {
        const auto answer = answers.constFind(question.id);
        result.insert(question.id, QJsonObject{
            {"answers", answer != answers.constEnd() ? QJsonArray{*answer} : QJsonArray{}}});
    }
    sendJson({{"id", requestId}, {"result", QJsonObject{{"answers", result}}}});
}

bool CodexAgent::loadConversationIndex()
{
    QFile file(indexPath_);
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) {
        emit message("[Could not read Codex conversation index: " + file.errorString() + "]");
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (!document.isObject() || document.object().value("version").toInt() != 1
        || !document.object().value("threads").isArray()) {
        emit message("[Invalid Codex conversation index: " + error.errorString() + "]");
        return false;
    }
    for (const QJsonValue &value : document.object().value("threads").toArray()) {
        QJsonObject thread = value.toObject();
        limitPreview(thread);
        const QString id = thread.value("id").toString();
        if (!id.isEmpty()) cachedConversations_.insert(id, thread);
    }
    return true;
}

bool CodexAgent::saveConversationIndex()
{
    if (!QDir().mkpath(QFileInfo(indexPath_).absolutePath())) {
        emit message("[Could not create directory for Codex conversation index]");
        return false;
    }
    QList<QJsonObject> threads = cachedConversations_.values();
    std::sort(threads.begin(), threads.end(), [](const QJsonObject &left, const QJsonObject &right) {
        const qint64 leftDate = left.value("createdAt").toInteger();
        const qint64 rightDate = right.value("createdAt").toInteger();
        return leftDate == rightDate ? left.value("id").toString() < right.value("id").toString()
                                     : leftDate > rightDate;
    });
    QJsonArray data;
    for (const QJsonObject &thread : threads) data.append(thread);
    const QJsonObject root{{"version", 1},
                           {"syncedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {"threads", data}};
    QSaveFile file(indexPath_);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        emit message("[Could not save Codex conversation index: " + file.errorString() + "]");
        return false;
    }
    return true;
}

// Active threads are read newest first until they reach the newest cached one; archived threads
// are always read in full.
void CodexAgent::syncConversations()
{
    if (syncing_) return;
    if (!initialized_) {
        emit message("[Codex App Server is not connected.]");
        return;
    }
    stagedConversations_ = cachedConversations_;
    newConversationIds_.clear();
    activeConversationWatermark_ = 0;
    for (const QJsonObject &thread : cachedConversations_) {
        if (!thread.value("archived").toBool()) {
            activeConversationWatermark_ = qMax(activeConversationWatermark_, thread.value("createdAt").toInteger());
        }
    }
    syncCursor_.clear();
    syncPages_ = 0;
    syncingArchived_ = false;
    syncing_ = true;
    emit message("[Syncing Codex conversations…]");
    emit stateChanged();
    requestConversationPage();
}

void CodexAgent::requestConversationPage()
{
    QJsonArray sources{"cli", "vscode", "exec", "appServer", "subAgent", "subAgentReview",
                       "subAgentCompact", "subAgentThreadSpawn", "subAgentOther", "unknown"};
    QJsonObject params{{"limit", 100}, {"sortKey", "created_at"}, {"sortDirection", "desc"},
                       {"sourceKinds", sources}, {"archived", syncingArchived_}};
    if (!syncCursor_.isEmpty()) params.insert("cursor", syncCursor_);
    sendRequest("thread/list", params);
}

void CodexAgent::handleConversationPage(const QJsonObject &result)
{
    if (!syncing_) return;
    if (!result.value("data").isArray()) {
        emit message("[Codex conversation sync failed: invalid thread/list response]");
        syncing_ = false;
        stagedConversations_.clear();
        emit stateChanged();
        return;
    }
    ++syncPages_;
    bool olderThanCached = false;
    for (const QJsonValue &value : result.value("data").toArray()) {
        QJsonObject thread = value.toObject();
        limitPreview(thread);
        const QString id = thread.value("id").toString();
        if (id.isEmpty()) continue;
        if (!syncingArchived_ && activeConversationWatermark_ > 0
            && thread.value("createdAt").toInteger() < activeConversationWatermark_) {
            olderThanCached = true;
        }
        if (!cachedConversations_.contains(id)) newConversationIds_.insert(id);
        thread.insert("archived", syncingArchived_);
        stagedConversations_.insert(id, thread);
    }
    const QString nextCursor = result.value("nextCursor").toString();
    if (!nextCursor.isEmpty() && nextCursor == syncCursor_) {
        emit message("[Codex conversation sync failed: repeated pagination cursor]");
        syncing_ = false;
        stagedConversations_.clear();
        emit stateChanged();
        return;
    }
    if (!nextCursor.isEmpty() && !(olderThanCached && !syncingArchived_)) {
        syncCursor_ = nextCursor;
        requestConversationPage();
        return;
    }
    if (!syncingArchived_) {
        syncingArchived_ = true;
        syncCursor_.clear();
        requestConversationPage();
    } else {
        finishConversationSync();
    }
}

void CodexAgent::finishConversationSync()
{
    cachedConversations_ = std::move(stagedConversations_);
    stagedConversations_.clear();
    syncing_ = false;
    const bool saved = saveConversationIndex();
    emit message(QString("[Codex conversations: %1 total, %2 new; fetched %3 pages%4]")
                     .arg(cachedConversations_.size()).arg(newConversationIds_.size()).arg(syncPages_)
                     .arg(saved ? "" : "; index not saved"));
    if (saved) emit message("[Index: " + indexPath_ + "]");
    emit conversationsChanged();
    emit stateChanged();
}

void CodexAgent::startThread()
{
    if (!initialized_ || threadOpening_) return;
    threadOpening_ = true;
    emit message("[Starting a new conversation]");
    sendRequest("thread/start", {{"cwd", workingDirectory_}, {"serviceName", "agentdeskt"}});
    emit stateChanged();
}

void CodexAgent::sendNextPrompt()
{
    if (!initialized_ || threadId_.isEmpty() || busy_ || queuedPrompts_.isEmpty()) return;
    const QString text = queuedPrompts_.takeFirst();
    busy_ = true;
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
    QJsonObject params{{"threadId", threadId_},
                       {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
    sendRequest("turn/start", params);
    emit stateChanged();
}

// turn/interrupt needs the turn ID, which may arrive after the user asked to stop.
void CodexAgent::sendStopIfPossible()
{
    if (!busy_ || !stopRequested_ || stopSent_ || threadId_.isEmpty() || activeTurnId_.isEmpty()) return;
    stopSent_ = true;
    sendRequest("turn/interrupt", {{"threadId", threadId_}, {"turnId", activeTurnId_}});
}

qint64 CodexAgent::sendRequest(const QString &method, const QJsonObject &params)
{
    const qint64 id = nextRequestId_++;
    pendingRequests_.insert(id, method);
    sendJson({{"id", id}, {"method", method}, {"params", params}});
    return id;
}

void CodexAgent::sendNotification(const QString &method, const QJsonObject &params)
{
    sendJson({{"method", method}, {"params", params}});
}

void CodexAgent::sendJson(const QJsonObject &message)
{
    if (server_->state() == QProcess::NotRunning) return;
    server_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void CodexAgent::handleLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit message("[Invalid server response: " + error.errorString() + "]");
        return;
    }
    const QJsonObject object = document.object();
    if (object.contains("method")) {
        const QString method = object.value("method").toString();
        if (object.contains("id")) {
            handleServerRequest(method, object.value("id"), object.value("params").toObject());
        } else {
            handleNotification(method, object.value("params").toObject());
        }
    } else if (object.contains("id")) {
        handleResponse(object);
    }
}

void CodexAgent::handleResponse(const QJsonObject &response)
{
    const qint64 id = response.value("id").toInteger();
    const QString method = pendingRequests_.take(id);
    const QJsonObject error = response.value("error").toObject();
    if (!error.isEmpty()) {
        const QString errorText = error.value("message").toString();
        emit message("[Error in " + method + "] " + errorText);
        if (method == "thread/items/list" && id == historyRequest_) {
            historyRequest_ = 0;
            emit historyLoaded(historyThreadId_, historyEntries_, false, "Could not load this conversation: " + errorText);
        } else if (method == "thread/list") {
            syncing_ = false;
            stagedConversations_.clear();
        } else if (method == "thread/start" || method == "thread/resume") {
            threadOpening_ = false;
            if (method == "thread/resume") {
                const QString failedId = resumingThreadId_;
                resumingThreadId_.clear();
                emit conversationOpenFailed(failedId, "the Codex App Server refused to open it: " + errorText);
            }
        } else if (method == "turn/start") {
            busy_ = false;
            activeTurnId_.clear();
            stopRequested_ = false;
            stopSent_ = false;
            sendNextPrompt();
        } else if (method == "turn/interrupt") {
            stopRequested_ = false;
            stopSent_ = false;
        }
        emit stateChanged();
        return;
    }
    const QJsonObject result = response.value("result").toObject();
    if (method == "initialize") {
        initialized_ = true;
        sendNotification("initialized", {});
        emit connected();
        if (historyPending_) loadHistory(historyThreadId_, {}, false);
    } else if (method == "thread/items/list") {
        if (id != historyRequest_) return;
        historyRequest_ = 0;
        // Items arrive newest first; older pages are prepended to what is already loaded.
        QList<ChatEntry> page;
        const QJsonArray items = result.value("data").toArray();
        for (qsizetype i = items.size() - 1; i >= 0; --i)
            page.append(historyEntries(items.at(i).toObject().value("item").toObject()));
        historyEntries_ = page + historyEntries_;
        historyCursor_ = result.value("nextCursor").toString();
        emit historyLoaded(historyThreadId_, historyEntries_, !historyCursor_.isEmpty(), {});
    } else if (method == "thread/list") {
        handleConversationPage(result);
    } else if (method == "thread/start" || method == "thread/resume") {
        threadOpening_ = false;
        resumingThreadId_.clear();
        threadId_ = result.value("thread").toObject().value("id").toString();
        if (threadId_.isEmpty()) emit message("[Server did not return a conversation ID.]");
        else emit message(method == "thread/start" ? "[Connected to Codex]" : "[Resumed Codex conversation: " + threadId_ + "]");
        emit conversationOpened(threadId_, method == "thread/resume");
        sendNextPrompt();
    } else if (method == "turn/start") {
        if (busy_) {
            activeTurnId_ = result.value("turn").toObject().value("id").toString();
            sendStopIfPossible();
        }
    }
    emit stateChanged();
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
        busy_ = false;
        activeTurnId_.clear();
        stopRequested_ = false;
        stopSent_ = false;
        emit turnCompleted(turn.value("status").toString(), turn.value("error").toObject().value("message").toString());
        emit stateChanged();
        sendNextPrompt();
    } else if (method == "error") {
        emit message("[Error] " + params.value("error").toObject().value("message").toString());
    } else if (method == "warning" || method == "configWarning") {
        emit message("[Warning] " + params.value("message").toString(params.value("summary").toString()));
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
        sendJson({{"id", id}, {"error", QJsonObject{{"code", -32601}, {"message", "Unsupported request"}}}});
    }
}
