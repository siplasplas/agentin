#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>

class QProcess;

class GeminiBridge : public QObject
{
    Q_OBJECT
public:
    GeminiBridge(const QString &program, const QString &workingDirectory, QObject *parent = nullptr);
    ~GeminiBridge() override;
    bool isRunning() const;
    QString program() const;
    void prompt(const QString &text);
    void interrupt();
    void resetConversation();
    void resetConversation(const QString &workingDirectory);
    void resumeConversation(const QString &sessionId, const QString &workingDirectory);
    QString sessionId() const;
    bool addDirectory(const QString &path);
    QStringList directories() const;

signals:
    void textDelta(const QString &text);
    void toolStarted(const QString &name, const QJsonObject &input);
    void completed(const QString &status, const QString &details);
    void error(const QString &message);
    void sessionChanged(const QString &sessionId);

private:
    void handleLine(const QByteArray &line);
    void finish(int code);

    QString program_;
    QString workingDirectory_;
    QProcess *process_;
    QByteArray buffer_;
    QString sessionId_;
    QStringList directories_;
    QString errorDetails_;
    bool interrupted_ = false;
    bool resultSeen_ = false;
};
