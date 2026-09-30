#pragma once

#include "AgentBackend.h"
#include "ConversationIndex.h"

#include <QJsonObject>

#include <functional>

class QProcess;

// Claude or GLM backend that talks JSONL to claude/bridge.py, which runs the Claude Agent SDK.
class ClaudeAgent : public AgentBackend
{
    Q_OBJECT

public:
    ClaudeAgent(const QString &pythonProgram, const QString &scriptPath,
                const QString &workingDirectory, const QString &provider, const QString &indexPath,
                QObject *parent = nullptr);
    ~ClaudeAgent() override;

    QString name() const override;
    QString program() const override { return pythonProgram_; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return sessionId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    AgentHelp help() const override;
    QString externalLock(const QString &id) const override;
    void loadConversations() override;
    void refreshConversations() override;
    QList<QJsonObject> conversations() const override { return index_.treeEntries(); }
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override;
    void answerApproval(int id, bool allow) override;
    void answerQuestions(int id, const QHash<QString, QString> &answers) override;

    bool isRunning() const;
    // Claude and GLM share the SDK transcript store; sessions recorded by the other agent are not listed here.
    void excludeSessionsOf(const ClaudeAgent *other) { excluded_ = other; }

private:
    void start(const QString &workingDirectory);
    void sendNextPrompt();
    void send(const QJsonObject &message);
    void reportIndexError(const QString &error);
    void runHelper(const QStringList &arguments, const QString &workingDirectory,
                   const std::function<void(QProcess *process, bool started)> &done);
    void handleLine(const QByteArray &line);

    QString pythonProgram_;
    QString scriptPath_;
    QString workingDirectory_;
    QString provider_;
    QProcess *process_;
    QByteArray buffer_;
    QString sessionId_;
    QString pendingResumeId_;
    QString firstPrompt_;
    ConversationIndex index_;
    const ClaudeAgent *excluded_ = nullptr;
    QStringList queuedPrompts_;
    QHash<int, int> pendingQuestionCounts_;
    QList<QProcess *> helperProcesses_;
    quint64 historyGeneration_ = 0;
    int historyLimit_ = 0;
    bool ready_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool textStarted_ = false;
};
