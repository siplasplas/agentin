#pragma once

#include <QJsonObject>
#include <QObject>

class QProcess;

class AntigravityBridge : public QObject
{
    Q_OBJECT
public:
    AntigravityBridge(const QString &program, const QString &workingDirectory, QObject *parent = nullptr);
    ~AntigravityBridge() override;
    QString program() const;
    QString conversationId() const;
    bool isRunning() const;
    void prompt(const QString &text);
    void interrupt();
    void resetConversation(const QString &workingDirectory);
    void resumeConversation(const QString &id, const QString &workingDirectory);

signals:
    void textDelta(const QString &text);
    void toolStarted(const QString &name, const QJsonObject &input);
    void completed(const QString &status, const QString &details);
    void error(const QString &message);
    void conversationChanged(const QString &id);

private:
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
    bool interrupted_ = false;
    bool resultSeen_ = false;
    bool textSeen_ = false;
    bool completionSent_ = false;
};
