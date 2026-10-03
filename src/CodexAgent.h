#pragma once

#include "AgentBackend.h"

#include <QJsonArray>
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
    bool supportsFastMode() const override { return true; }
    bool isFastMode() const override { return fastMode_; }
    void setFastMode(bool fast) override;
    bool supportsReadOnly() const override { return true; }
    bool isReadOnly() const override { return readOnly_; }
    bool readOnlyIsEnforced() const override { return true; }
    QStringList writableDirectories() const override;
    void setReadOnly(bool readOnly) override;
    // Read-only, never asking for approval, with a thread Codex does not keep; for one-off questions.
    void makeEphemeral() { ephemeral_ = true; setReadOnly(true); }
    bool isEphemeral() const { return ephemeral_; }
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    bool supportsSteering() const override { return true; }
    bool canSteer() const override;
    bool isSteering() const override { return steeringInFlight_; }
    bool steer(const QString &text) override;
    bool supportsCompaction() const override { return true; }
    bool canCompact() const override;
    bool isCompacting() const override { return manualCompaction_; }
    bool compact() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override;
    void answerApproval(int id, ApprovalDecision decision) override;
    QStringList trustedSessionCommands() const override;
    bool removeTrustedSessionCommand(const QString &rule) override;
    void answerQuestions(int id, const QHash<QString, QStringList> &answers) override;

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
    void showOutputOf(const QString &itemId);
    void refreshAfterCompaction();
    QJsonObject sandboxPolicy() const;

    QPointer<CodexConnection> connection_;
    QString workingDirectory_;
    QString threadId_;
    // The thread to reopen when the App Server has been restarted.
    QString reopenThreadId_;
    QString activeTurnId_;
    QString model_;
    QString effort_;
    // Set once the user picks a model, so the thread's reported settings no longer replace it.
    bool modelChosen_ = false;
    bool fastMode_ = false;
    // The thread's token total before the running turn, found from the first report in the turn.
    TokenUsage turnBaseline_;
    bool turnBaselineKnown_ = false;
    // The thread's own sandbox policy, restored when read-only mode is switched off again.
    QJsonObject threadSandbox_;
    bool readOnly_ = false;
    bool sandboxChosen_ = false;
    // A one-off chat, such as one asked for suggestions, whose thread Codex does not keep.
    bool ephemeral_ = false;
    QStringList queuedPrompts_;
    QSet<QString> streamedMessages_;
    QSet<QString> streamedCommands_;
    // Commands can run in parallel; each keeps its command, and output goes under the block of the
    // command it belongs to, which is reopened when another command wrote in between.
    QHash<QString, QString> runningCommands_;
    QString outputCommand_;
    QHash<QString, QJsonObject> reasoningItems_;
    QHash<int, QJsonValue> serverRequests_;
    QHash<int, QList<AgentQuestion>> pendingQuestions_;
    // Command prefixes Codex proposed as lasting rules, by approval request.
    QHash<int, QJsonArray> proposedRules_;
    QSet<QString> trustedSessionCommands_;
    // The changes of file change items in the running turn, shown in their approvals.
    QHash<QString, QJsonArray> fileChanges_;
    QString trustedConversationId_;
    QHash<int, QStringList> requestSessionRules_;
    // Additional permissions Codex asked for, by approval request.
    QHash<int, QJsonObject> permissionRequests_;
    // Directories Codex may also write to after a permission request, for this turn or this thread.
    QStringList turnWriteRoots_;
    QStringList sessionWriteRoots_;
    int nextServerRequest_ = 1;
    QString historyThreadId_;
    QString historyCursor_;
    QList<ChatEntry> historyEntries_;
    quint64 historyGeneration_ = 0;
    bool historyPending_ = false;
    bool refreshingHistory_ = false;
    bool compactedHistoryPending_ = false;
    bool threadOpening_ = false;
    bool steeringInFlight_ = false;
    QString steeringText_;
    bool busy_ = false;
    bool manualCompaction_ = false;
    bool stopRequested_ = false;
    bool stopSent_ = false;
};
