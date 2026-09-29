#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QProcess;

class ClaudeBridge : public QObject
{
    Q_OBJECT

public:
    ClaudeBridge(const QString &pythonProgram, const QString &scriptPath,
                 const QString &workingDirectory, QObject *parent = nullptr);
    ~ClaudeBridge() override;
    bool isRunning() const;
    void start();
    void prompt(const QString &text);
    void interrupt();
    void resetConversation();
    void answerApproval(int id, bool allow);
    void answerQuestions(int id, const QJsonObject &answers, bool accepted);

signals:
    void ready();
    void textDelta(const QString &text);
    void toolStarted(const QString &name, const QJsonObject &input);
    void completed(const QString &status, const QString &details);
    void approvalRequested(int id, const QString &tool, const QJsonObject &input);
    void questionsRequested(int id, const QJsonArray &questions);
    void error(const QString &message);
    void disconnected();

private:
    void send(const QJsonObject &message);
    void handleLine(const QByteArray &line);

    QString pythonProgram_;
    QString scriptPath_;
    QString workingDirectory_;
    QProcess *process_;
    QByteArray buffer_;
};
