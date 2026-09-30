#pragma once

#include "AgentBackend.h"

#include <QJsonObject>

class QProcess;

// Claude or GLM backend that talks JSONL to claude/bridge.py, which runs the Claude Agent SDK.
class ClaudeAgent : public AgentBackend
{
    Q_OBJECT

public:
    ClaudeAgent(const QString &pythonProgram, const QString &scriptPath,
                const QString &workingDirectory, const QString &provider,
                QObject *parent = nullptr);
    ~ClaudeAgent() override;

    QString name() const override;
    QString program() const override { return pythonProgram_; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return sessionId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override;
    void answerApproval(int id, bool allow) override;
    void answerQuestions(int id, const QHash<QString, QString> &answers) override;

    bool isRunning() const;

private:
    void start(const QString &workingDirectory);
    void sendNextPrompt();
    void send(const QJsonObject &message);
    void handleLine(const QByteArray &line);

    QString pythonProgram_;
    QString scriptPath_;
    QString workingDirectory_;
    QString provider_;
    QProcess *process_;
    QByteArray buffer_;
    QString sessionId_;
    QString pendingResumeId_;
    QStringList queuedPrompts_;
    QHash<int, int> pendingQuestionCounts_;
    QList<QProcess *> historyProcesses_;
    quint64 historyGeneration_ = 0;
    int historyLimit_ = 0;
    bool ready_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool textStarted_ = false;
};
