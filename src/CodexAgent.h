#pragma once

#include "AgentBackend.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QSet>

class CodexConnection;

// One Codex chat: a thread on the shared App Server connection with its own turn state and queue.
class CodexAgent : public AgentBackend
{
    Q_OBJECT

public:
    CodexAgent(CodexConnection *connection, const QString &workingDirectory, QObject *parent = nullptr);
    ~CodexAgent() override;

    QString name() const override { return "Codex"; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return threadId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    QString model() const override;
    QString effort() const override;
    void setModel(const QString &model, const QString &effort) override;
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override;
    void answerApproval(int id, ApprovalDecision decision) override;
    void answerQuestions(int id, const QHash<QString, QString> &answers) override;

    // Called by CodexConnection for messages that carry this chat's threadId.
    void handleNotification(const QString &method, const QJsonObject &params);
    void handleServerRequest(const QString &method, const QJsonValue &id, const QJsonObject &params);

private:
    void openThread(const QJsonObject &result, bool resumed);
    void closeThread();
    void startThread();
    void sendNextPrompt();
    void sendStopIfPossible();
    void resetTurn();

    QPointer<CodexConnection> connection_;
    QString workingDirectory_;
    QString threadId_;
    QString activeTurnId_;
    QString model_;
    QString effort_;
    // Set once the user picks a model, so the thread's reported settings no longer replace it.
    bool modelChosen_ = false;
    QStringList queuedPrompts_;
    QSet<QString> streamedMessages_;
    QSet<QString> streamedCommands_;
    QHash<int, QJsonValue> serverRequests_;
    QHash<int, QList<AgentQuestion>> pendingQuestions_;
    int nextServerRequest_ = 1;
    QString historyThreadId_;
    QString historyCursor_;
    QList<ChatEntry> historyEntries_;
    quint64 historyGeneration_ = 0;
    bool historyPending_ = false;
    bool threadOpening_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool stopSent_ = false;
};
