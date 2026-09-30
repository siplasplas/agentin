#pragma once

#include "AgentBackend.h"
#include "ConversationIndex.h"

#include <QJsonObject>

class QProcess;

// Gemini backend that runs one headless `gemini --output-format stream-json` process per turn.
class GeminiAgent : public AgentBackend
{
    Q_OBJECT
public:
    GeminiAgent(const QString &program, const QString &workingDirectory, const QString &dataDirectory,
                const QString &indexPath, QObject *parent = nullptr);
    ~GeminiAgent() override;

    QString name() const override { return "Gemini"; }
    QString program() const override { return program_; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return sessionId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    AgentHelp help() const override;
    QString externalLock(const QString &id) const override;
    void loadConversations() override;
    // Lists sessions saved by Gemini CLI under its data directory across all projects.
    void refreshConversations() override;
    QList<QJsonObject> conversations() const override { return index_.treeEntries(); }
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override {}
    void answerApproval(int, bool) override {}
    void answerQuestions(int, const QHash<QString, QString> &) override {}

private:
    QList<QJsonObject> discoverSessions();
    void reportIndexError(const QString &error);
    bool isRunning() const;
    void sendNextPrompt();
    void handleLine(const QByteArray &line);
    void finish(int code);

    QString program_;
    QString workingDirectory_;
    QString dataDirectory_;
    QProcess *process_;
    QByteArray buffer_;
    QString sessionId_;
    QString firstPrompt_;
    ConversationIndex index_;
    QString errorDetails_;
    QStringList queuedPrompts_;
    QHash<QString, QString> sessionFiles_;
    int historyLimit_ = 0;
    bool executableChecked_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool textStarted_ = false;
    bool interrupted_ = false;
    bool resultSeen_ = false;
};
