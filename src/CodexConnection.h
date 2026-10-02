#pragma once

#include "AgentBackend.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>

class CodexAgent;
class QProcess;

// One `codex app-server --stdio` process shared by all Codex chats. It sends requests, routes
// notifications and server requests to the chat that owns their threadId, and keeps the thread
// list with its local index.
class CodexConnection : public AgentProvider
{
    Q_OBJECT

public:
    // error is empty on success; the connection has already logged it otherwise.
    using ResponseHandler = std::function<void(const QJsonObject &result, const QString &error)>;

    CodexConnection(const QString &program, const QString &workingDirectory, const QString &indexPath,
                    QObject *parent = nullptr);
    ~CodexConnection() override;

    QString name() const override { return "Codex"; }
    AgentHelp help() const override;
    void loadConversations() override;
    void refreshConversations() override;
    QList<QJsonObject> conversations() const override;
    void recordActivity(const QString &id) override;
    QList<AgentModel> models() const override { return models_; }
    bool supportsUsageLimits() const override { return true; }
    bool supportsSuggestions() const override { return true; }
    void suggest(const QString &workingDirectory, const QString &prompt, QObject *context,
                 const std::function<void(const QString &text, const QString &error)> &done) override;
    QList<UsageLimit> usageLimits() const override;
    void setUsageLimitsActive(bool active) override;
    AgentBackend *createChat(const QString &workingDirectory, QObject *parent) override;

    void start();
    bool isRunning() const;
    bool isConnected() const { return initialized_; }

    // The handler is skipped when context has been destroyed before the response arrives.
    qint64 request(const QString &method, const QJsonObject &params, QObject *context = nullptr,
                   ResponseHandler handler = {});
    void respond(const QJsonValue &id, const QJsonObject &result);
    void respondError(const QJsonValue &id, int code, const QString &text);

    // Routes a thread's notifications and server requests to chat; shows it in the tree until listed.
    void registerThread(const QString &threadId, CodexAgent *chat, const QString &workingDirectory);
    void unregisterThread(const QString &threadId);

signals:
    void connected();
    void disconnected();

private:
    struct PendingRequest
    {
        QString method;
        QPointer<QObject> context;
        bool hasContext = false;
        ResponseHandler handler;
    };

    bool loadConversationIndex();
    bool saveConversationIndex();
    void syncConversations();
    void requestModels(const QString &cursor);
    void requestRateLimits();
    void updateRateLimits(const QJsonObject &snapshot);
    void requestConversationPage();
    void handleConversationPage(const QJsonObject &result);
    void finishConversationSync();
    void sendJson(const QJsonObject &message);
    void handleLine(const QByteArray &line);
    void processReadBuffer();
    void handleResponse(const QJsonObject &response);

    QString program_;
    QString workingDirectory_;
    QString indexPath_;
    QProcess *server_;
    QByteArray readBuffer_;
    bool readScheduled_ = false;
    qint64 nextRequestId_ = 1;
    QHash<qint64, PendingRequest> pendingRequests_;
    QHash<QString, QPointer<CodexAgent>> threads_;
    QHash<QString, QString> liveThreadDirectories_;
    QString syncCursor_;
    QList<AgentModel> models_;
    QList<AgentModel> stagedModels_;
    QMap<QString, QList<UsageLimit>> rateLimits_;
    bool usageLimitsActive_ = false;
    QHash<QString, QJsonObject> cachedConversations_;
    QHash<QString, QJsonObject> stagedConversations_;
    QSet<QString> newConversationIds_;
    qint64 activeConversationWatermark_ = 0;
    int syncPages_ = 0;
    bool initialized_ = false;
    bool syncing_ = false;
    bool syncingArchived_ = false;
    bool syncWhenConnected_ = false;
    // Times of unexpected exits, to stop restarting a server that keeps failing.
    QList<qint64> crashTimes_;
};
