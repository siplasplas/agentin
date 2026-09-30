#pragma once

#include "AgentBackend.h"

class QProcess;

// Antigravity backend that runs one headless `agy --output-format stream-json` process per turn.
class AntigravityAgent : public AgentBackend
{
    Q_OBJECT
public:
    AntigravityAgent(const QString &program, const QString &workingDirectory, QObject *parent = nullptr);
    ~AntigravityAgent() override;

    QString name() const override { return "Antigravity"; }
    QString program() const override { return program_; }
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
    void answerApproval(int, bool) override {}
    void answerQuestions(int, const QHash<QString, QString> &) override {}

private:
    bool isRunning() const;
    void sendNextPrompt();
    void handleLine(const QByteArray &line);
    void drainOutput();
    void finish(int code);

    QString program_;
    QString workingDirectory_;
    QString conversationId_;
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
