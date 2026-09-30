#include "CodexConnection.h"

#include "CodexAgent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

namespace {
void limitPreview(QJsonObject &thread)
{
    if (!thread.value("preview").isString()) return;
    QString preview = thread.value("preview").toString().simplified();
    constexpr int limit = 200;
    if (preview.size() > limit) preview = preview.left(limit - 1) + QChar(0x2026);
    thread.insert("preview", preview);
}
}

CodexConnection::CodexConnection(const QString &program, const QString &workingDirectory, const QString &indexPath,
                                 QObject *parent)
    : AgentProvider(parent), program_(program), workingDirectory_(workingDirectory), indexPath_(indexPath),
      server_(new QProcess(this))
{
    connect(server_, &QProcess::started, this, [this] {
        request("initialize", {{"clientInfo", QJsonObject{
                    {"name", "agentdeskt"}, {"title", "agentdeskt Qt"}, {"version", "0.1.0"}}}},
                this, [this](const QJsonObject &, const QString &error) {
            if (!error.isEmpty()) return;
            initialized_ = true;
            sendJson({{"method", "initialized"}, {"params", QJsonObject{}}});
            emit connected();
            requestModels({});
            requestRateLimits();
            if (syncWhenConnected_) {
                syncWhenConnected_ = false;
                syncConversations();
            }
            emit stateChanged();
        });
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
        const bool wasConnected = initialized_;
        initialized_ = false;
        pendingRequests_.clear();
        threads_.clear();
        liveThreadDirectories_.clear();
        emit disconnected();
        emit conversationsChanged();
        emit message(QString("[Server exited with code %1]").arg(code));
        // A server that was working is started again after a growing delay; chats reopen their
        // threads once it is connected. One that keeps failing is left stopped.
        if (wasConnected) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            crashTimes_.append(now);
            while (!crashTimes_.isEmpty() && now - crashTimes_.first() > 5 * 60 * 1000) crashTimes_.removeFirst();
            if (crashTimes_.size() > 5) {
                emit message("[The Codex App Server keeps exiting, so it is not restarted. Restart agentdeskt to try again.]");
            } else {
                const int delay = qMin(30, 1 << (crashTimes_.size() - 1));
                emit message(QString("[Restarting the Codex App Server in %1 s]").arg(delay));
                QTimer::singleShot(delay * 1000, this, &CodexConnection::start);
            }
        }
        emit stateChanged();
    });
}

CodexConnection::~CodexConnection()
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

AgentHelp CodexConnection::help() const
{
    return {{"Codex connection: codex app-server --stdio (direct JSONL).",
             "Installed Codex App Server commands and options (reference; CLI subcommands are not chat messages):"},
            "App Server", program_, {"app-server", "--help"}};
}

// model/list is paged; the list is replaced once all pages have arrived.
void CodexConnection::requestModels(const QString &cursor)
{
    QJsonObject params;
    if (cursor.isEmpty()) stagedModels_.clear();
    else params.insert("cursor", cursor);
    request("model/list", params, this, [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) return;
        for (const QJsonValue &value : result.value("data").toArray()) {
            const QJsonObject object = value.toObject();
            AgentModel model;
            model.id = object.value("model").toString(object.value("id").toString());
            model.displayName = object.value("displayName").toString(model.id);
            model.description = object.value("description").toString();
            model.defaultEffort = object.value("defaultReasoningEffort").toString();
            model.isDefault = object.value("isDefault").toBool();
            for (const QJsonValue &option : object.value("supportedReasoningEfforts").toArray()) {
                model.efforts.append(option.toObject().value("reasoningEffort").toString());
                model.effortDescriptions.append(option.toObject().value("description").toString());
            }
            if (!model.id.isEmpty()) stagedModels_.append(model);
        }
        const QString next = result.value("nextCursor").toString();
        if (!next.isEmpty()) {
            requestModels(next);
            return;
        }
        models_ = stagedModels_;
        emit modelsChanged();
    });
}

void CodexConnection::requestRateLimits()
{
    request("account/rateLimits/read", {{"excludeResetCreditDetails", true}}, this,
            [this](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) return;
        rateLimits_.clear();
        const QJsonObject byLimit = result.value("rateLimitsByLimitId").toObject();
        for (const QJsonValue &snapshot : byLimit) updateRateLimits(snapshot.toObject());
        if (byLimit.isEmpty()) updateRateLimits(result.value("rateLimits").toObject());
        emit usageChanged();
    });
}

// A snapshot describes one limit with up to two windows; which windows exist depends on the plan.
void CodexConnection::updateRateLimits(const QJsonObject &snapshot)
{
    if (snapshot.isEmpty()) return;
    const QString limitId = snapshot.value("limitId").toString("codex");
    QList<UsageLimit> windows;
    for (const QString &key : {QStringLiteral("primary"), QStringLiteral("secondary")}) {
        const QJsonObject window = snapshot.value(key).toObject();
        if (window.isEmpty()) continue;
        UsageLimit limit;
        limit.id = limitId + '/' + key;
        limit.name = snapshot.value("limitName").toString();
        limit.windowMinutes = window.value("windowDurationMins").toInteger();
        limit.usedPercent = window.value("usedPercent").toDouble();
        limit.resetsAt = window.value("resetsAt").toInteger();
        windows.append(limit);
    }
    rateLimits_.insert(limitId, windows);
}

QList<UsageLimit> CodexConnection::usageLimits() const
{
    QList<UsageLimit> result;
    for (const QList<UsageLimit> &windows : rateLimits_) result += windows;
    return result;
}

AgentBackend *CodexConnection::createChat(const QString &workingDirectory, QObject *parent)
{
    return new CodexAgent(this, workingDirectory, parent);
}

void CodexConnection::start()
{
    server_->setWorkingDirectory(workingDirectory_);
    server_->start(program_, {"app-server", "--stdio"});
}

bool CodexConnection::isRunning() const
{
    return server_->state() != QProcess::NotRunning;
}

qint64 CodexConnection::request(const QString &method, const QJsonObject &params, QObject *context,
                                ResponseHandler handler)
{
    const qint64 id = nextRequestId_++;
    pendingRequests_.insert(id, {method, context, context != nullptr, std::move(handler)});
    sendJson({{"id", id}, {"method", method}, {"params", params}});
    return id;
}

void CodexConnection::respond(const QJsonValue &id, const QJsonObject &result)
{
    sendJson({{"id", id}, {"result", result}});
}

void CodexConnection::respondError(const QJsonValue &id, int code, const QString &text)
{
    sendJson({{"id", id}, {"error", QJsonObject{{"code", code}, {"message", text}}}});
}

void CodexConnection::registerThread(const QString &threadId, CodexAgent *chat, const QString &workingDirectory)
{
    if (threadId.isEmpty()) return;
    threads_.insert(threadId, chat);
    liveThreadDirectories_.insert(threadId, workingDirectory);
    emit conversationsChanged();
}

void CodexConnection::unregisterThread(const QString &threadId)
{
    if (threadId.isEmpty()) return;
    threads_.remove(threadId);
    if (liveThreadDirectories_.remove(threadId)) emit conversationsChanged();
}

void CodexConnection::loadConversations()
{
    if (loadConversationIndex())
        emit message(QString("[Cached Codex conversations: %1]").arg(cachedConversations_.size()));
}

void CodexConnection::refreshConversations()
{
    if (!initialized_) {
        syncWhenConnected_ = true;
        return;
    }
    syncConversations();
}

QList<QJsonObject> CodexConnection::conversations() const
{
    QList<QJsonObject> result;
    for (const QJsonObject &thread : cachedConversations_) {
        const QString id = thread.value("id").toString();
        QString title = thread.value("name").toString();
        if (title.isEmpty()) title = thread.value("preview").toString();
        if (title.isEmpty()) title = id;
        result.append({{"id", id}, {"cwd", thread.value("cwd").toString()}, {"title", title},
                       {"tooltip", title + "\n\n" + id}, {"createdAt", thread.value("createdAt").toInteger()},
                       {"archived", thread.value("archived").toBool()}});
    }
    // A chat started here appears in thread/list only after the next sync.
    for (auto it = liveThreadDirectories_.begin(); it != liveThreadDirectories_.end(); ++it) {
        if (cachedConversations_.contains(it.key())) continue;
        result.append({{"id", it.key()}, {"cwd", it.value()}, {"title", "Current chat"},
                       {"tooltip", it.key()}, {"createdAt", 0}});
    }
    return result;
}

bool CodexConnection::loadConversationIndex()
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

bool CodexConnection::saveConversationIndex()
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
void CodexConnection::syncConversations()
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

void CodexConnection::requestConversationPage()
{
    QJsonArray sources{"cli", "vscode", "exec", "appServer", "subAgent", "subAgentReview",
                       "subAgentCompact", "subAgentThreadSpawn", "subAgentOther", "unknown"};
    QJsonObject params{{"limit", 100}, {"sortKey", "created_at"}, {"sortDirection", "desc"},
                       {"sourceKinds", sources}, {"archived", syncingArchived_}};
    if (!syncCursor_.isEmpty()) params.insert("cursor", syncCursor_);
    request("thread/list", params, this, [this](const QJsonObject &result, const QString &error) {
        if (error.isEmpty()) {
            handleConversationPage(result);
            return;
        }
        syncing_ = false;
        stagedConversations_.clear();
        emit stateChanged();
    });
}

void CodexConnection::handleConversationPage(const QJsonObject &result)
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

void CodexConnection::finishConversationSync()
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

void CodexConnection::sendJson(const QJsonObject &message)
{
    if (server_->state() == QProcess::NotRunning) return;
    server_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void CodexConnection::handleLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit message("[Invalid server response: " + error.errorString() + "]");
        return;
    }
    const QJsonObject object = document.object();
    if (!object.contains("method")) {
        if (object.contains("id")) handleResponse(object);
        return;
    }
    const QString method = object.value("method").toString();
    const QJsonObject params = object.value("params").toObject();
    const QPointer<CodexAgent> chat = threads_.value(params.value("threadId").toString());
    if (object.contains("id")) {
        if (chat) {
            chat->handleServerRequest(method, object.value("id"), params);
        } else {
            emit message("[Unsupported server request: " + method + "]");
            respondError(object.value("id"), -32601, "Unsupported request");
        }
    } else if (chat) {
        chat->handleNotification(method, params);
    } else if (method == "account/rateLimits/updated") {
        updateRateLimits(params.value("rateLimits").toObject());
        emit usageChanged();
    } else if (method == "warning" || method == "configWarning") {
        emit message("[Warning] " + params.value("message").toString(params.value("summary").toString()));
    }
}

void CodexConnection::handleResponse(const QJsonObject &response)
{
    const PendingRequest pending = pendingRequests_.take(response.value("id").toInteger());
    const QString error = response.value("error").toObject().value("message").toString();
    if (response.contains("error")) emit message("[Error in " + pending.method + "] " + error);
    if (!pending.handler || (pending.hasContext && !pending.context)) return;
    pending.handler(response.value("result").toObject(), response.contains("error") && error.isEmpty() ? "Unknown error" : error);
}
