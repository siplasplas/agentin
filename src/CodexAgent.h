#pragma once

#include "AgentBackend.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

class QProcess;

// Codex backend that talks JSONL to `codex app-server --stdio`.
class CodexAgent : public AgentBackend
{
    Q_OBJECT

public:
    CodexAgent(const QString &program, const QString &workingDirectory, const QString &indexPath,
               QObject *parent = nullptr);
    ~CodexAgent() override;

    QString name() const override { return "Codex"; }
    QString program() const override { return program_; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return threadId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    AgentHelp help() const override;
    void loadConversations() override;
    void refreshConversations() override;
    QList<QJsonObject> conversations() const override;
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override;
    void answerApproval(int id, bool allow) override;
    void answerQuestions(int id, const QHash<QString, QString> &answers) override;

    void start();
    bool isRunning() const;

private:
    // The local index caches thread/list results so the conversation tree is available before a sync.
    bool loadConversationIndex();
    void syncConversations();
    void startThread();
    void sendNextPrompt();
    void sendStopIfPossible();
    void requestConversationPage();
    void handleConversationPage(const QJsonObject &result);
    void finishConversationSync();
    bool saveConversationIndex();
    qint64 sendRequest(const QString &method, const QJsonObject &params);
    void sendNotification(const QString &method, const QJsonObject &params);
    void sendJson(const QJsonObject &message);
    void handleLine(const QByteArray &line);
    void handleResponse(const QJsonObject &message);
    void handleNotification(const QString &method, const QJsonObject &params);
    void handleServerRequest(const QString &method, const QJsonValue &id, const QJsonObject &params);

    QString program_;
    QString workingDirectory_;
    QString indexPath_;
    QProcess *server_;
    QByteArray readBuffer_;
    QHash<qint64, QString> pendingRequests_;
    QHash<int, QJsonValue> serverRequests_;
    QHash<int, QList<AgentQuestion>> pendingQuestions_;
    int nextServerRequest_ = 1;
    QSet<QString> streamedMessages_;
    QSet<QString> streamedCommands_;
    QStringList queuedPrompts_;
    QString threadId_;
    QString activeTurnId_;
    QString resumingThreadId_;
    QString syncCursor_;
    QHash<QString, QJsonObject> cachedConversations_;
    QHash<QString, QJsonObject> stagedConversations_;
    QSet<QString> newConversationIds_;
    qint64 activeConversationWatermark_ = 0;
    int syncPages_ = 0;
    QString historyThreadId_;
    QString historyCursor_;
    QList<ChatEntry> historyEntries_;
    qint64 historyRequest_ = 0;
    bool historyPending_ = false;
    qint64 nextRequestId_ = 1;
    bool initialized_ = false;
    bool threadOpening_ = false;
    bool syncing_ = false;
    bool syncingArchived_ = false;
    bool syncWhenConnected_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool stopSent_ = false;
};
