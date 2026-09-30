#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

struct ChatEntry
{
    QString role;
    QString text;
};

struct AgentQuestion
{
    QString id;
    QString header;
    QString text;
    QStringList options;
    bool multiSelect = false;
};

// Common interface of an agent conversation backend. The backend owns its protocol, prompt queue
// and turn state; MainWindow drives it through these calls and renders its signals.
class AgentBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString name() const = 0;
    virtual QString program() const = 0;
    virtual QString workingDirectory() const = 0;
    virtual QString sessionId() const = 0;
    virtual QString statusText() const = 0;
    virtual bool isResponding() const = 0;
    virtual bool canInterrupt() const = 0;

    // Each call reports through message() and returns false when the request cannot be started.
    virtual bool newConversation(const QString &workingDirectory) = 0;
    virtual bool resumeConversation(const QString &id, const QString &workingDirectory) = 0;
    // Queues a message for the current conversation and sends it when the agent is free.
    virtual bool prompt(const QString &text) = 0;
    virtual void interrupt() = 0;

    // Loads the latest history of a conversation, or older entries when older is true.
    virtual void loadHistory(const QString &id, const QString &workingDirectory, bool older) = 0;
    virtual void cancelHistory() = 0;

    virtual void answerApproval(int id, bool allow) = 0;
    // Answers are keyed by AgentQuestion::id; a missing key means the question was not answered.
    virtual void answerQuestions(int id, const QHash<QString, QString> &answers) = 0;

signals:
    void message(const QString &text);
    void stateChanged();
    void messageStarted();
    void messageDelta(const QString &text);
    void messageFinished();
    void toolStarted(const QString &name, const QString &details);
    void toolOutput(const QString &text);
    void toolFinished(const QString &name, const QString &status);
    void turnCompleted(const QString &status, const QString &details);
    void approvalRequested(int id, const QString &title, const QString &description);
    void questionsRequested(int id, const QList<AgentQuestion> &questions);
    void conversationOpened(const QString &id, bool resumed);
    void conversationOpenFailed(const QString &id, const QString &reason);
    // Entries are the complete list to show for the conversation, oldest first.
    void historyLoaded(const QString &id, const QList<ChatEntry> &entries, bool hasMore, const QString &notice);
};
