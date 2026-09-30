#pragma once

#include "AgentBackend.h"
#include "ConversationIndex.h"

#include <QPointer>

class QProcess;

// Antigravity CLI. The CLI cannot list its conversations, so the provider shows only chats recorded
// here; each chat runs one headless `agy --output-format stream-json` process per turn.
class AntigravityProvider : public AgentProvider
{
    Q_OBJECT
public:
    AntigravityProvider(const QString &program, const QString &indexPath, QObject *parent = nullptr);

    QString name() const override { return "Antigravity"; }
    AgentHelp help() const override;
    QString externalLock(const QString &id) const override;
    void loadConversations() override;
    void refreshConversations() override;
    QList<QJsonObject> conversations() const override { return index_.treeEntries(); }
    AgentBackend *createChat(const QString &workingDirectory, QObject *parent) override;

    QString program() const { return program_; }
    void rememberConversation(const QString &id, const QString &workingDirectory, const QString &firstPrompt);

private:
    void reportIndexError(const QString &error);

    QString program_;
    ConversationIndex index_;
};

class AntigravityAgent : public AgentBackend
{
    Q_OBJECT
public:
    AntigravityAgent(AntigravityProvider *provider, const QString &workingDirectory, QObject *parent = nullptr);
    ~AntigravityAgent() override;

    QString name() const override { return "Antigravity"; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return conversationId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override {}
    void answerApproval(int, ApprovalDecision) override {}
    void answerQuestions(int, const QHash<QString, QStringList> &) override {}

private:
    bool isRunning() const;
    void sendNextPrompt();
    void rememberConversation();
    void handleLine(const QByteArray &line);
    void drainOutput();
    void finish(int code);

    QPointer<AntigravityProvider> provider_;
    QString program_;
    QString workingDirectory_;
    QString conversationId_;
    QString firstPrompt_;
    QString errorDetails_;
    QString diagnostics_;
    QByteArray buffer_;
    QProcess *process_;
    QStringList queuedPrompts_;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool interrupted_ = false;
    bool resultSeen_ = false;
    bool textSeen_ = false;
    bool completionSent_ = false;
};
