#pragma once

#include "AgentBackend.h"
#include "ConversationIndex.h"

#include <QJsonObject>
#include <QPointer>

class QProcess;

// Gemini CLI. The provider reads the CLI's saved sessions and keeps the conversation index; each chat
// runs one headless `gemini --output-format stream-json` process per turn.
class GeminiProvider : public AgentProvider
{
    Q_OBJECT
public:
    GeminiProvider(const QString &program, const QString &dataDirectory, const QString &indexPath,
                   QObject *parent = nullptr);

    QString name() const override { return "Gemini"; }
    AgentHelp help() const override;
    QString externalLock(const QString &id) const override;
    void loadConversations() override;
    // Lists sessions saved by Gemini CLI under its data directory across all projects.
    void refreshConversations() override;
    QList<QJsonObject> conversations() const override { return index_.treeEntries(); }
    void recordActivity(const QString &id) override;
    QList<AgentModel> models() const override;
    AgentBackend *createChat(const QString &workingDirectory, QObject *parent) override;

    QString program() const { return program_; }
    QString sessionFile(const QString &id);
    void rememberConversation(const QString &id, const QString &workingDirectory, const QString &firstPrompt);

private:
    QList<QJsonObject> discoverSessions();
    void reportIndexError(const QString &error);

    QString program_;
    QString dataDirectory_;
    ConversationIndex index_;
    QHash<QString, QString> sessionFiles_;
    bool executableChecked_ = false;
};

class GeminiAgent : public AgentBackend
{
    Q_OBJECT
public:
    GeminiAgent(GeminiProvider *provider, const QString &workingDirectory, QObject *parent = nullptr);
    ~GeminiAgent() override;

    QString name() const override { return "Gemini"; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return sessionId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    QString model() const override { return model_; }
    void setModel(const QString &model, const QString &effort) override;
    // The CLI's plan mode: the agent reads and plans without changing files.
    bool supportsReadOnly() const override { return true; }
    bool isReadOnly() const override { return readOnly_; }
    void setReadOnly(bool readOnly) override { readOnly_ = readOnly; emit stateChanged(); }
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
    void handleLine(const QByteArray &line);
    void finish(int code);

    QPointer<GeminiProvider> provider_;
    QString program_;
    QString workingDirectory_;
    QProcess *process_;
    QByteArray buffer_;
    QString sessionId_;
    QString firstPrompt_;
    QString model_;
    bool readOnly_ = false;
    QString errorDetails_;
    QStringList queuedPrompts_;
    HistoryPages history_;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool textStarted_ = false;
    bool interrupted_ = false;
    bool resultSeen_ = false;
};
