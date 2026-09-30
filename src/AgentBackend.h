#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

struct ChatEntry
{
    QString role;
    QString text;
};

// A conversation read in full once and revealed page by page from its end, for agents whose
// history source cannot be paged.
struct HistoryPages
{
    QString id;
    QList<ChatEntry> entries;
    qsizetype shown = 0;

    void reset(const QString &conversation, const QList<ChatEntry> &all, qsizetype page)
    {
        id = conversation;
        entries = all;
        shown = qMin(page, all.size());
    }
    void showMore(qsizetype page) { shown = qMin(shown + page, entries.size()); }
    QList<ChatEntry> visible() const { return entries.mid(entries.size() - shown); }
    bool hasMore() const { return shown < entries.size(); }
};

struct AgentQuestion
{
    QString id;
    QString header;
    QString text;
    QStringList options;
    QStringList optionDescriptions;
    bool multiSelect = false;
    // The agent also accepts an answer in the user's own words.
    bool allowOther = false;
    // The answer is a secret and is not shown while typed.
    bool secret = false;
};

enum class ApprovalDecision {
    Accept,
    // Also allow the same kind of action for the rest of the session.
    AcceptForSession,
    // The agent continues without the action.
    Decline,
    // Decline and stop the agent's turn.
    Cancel,
};

// Help shown for the selected agent: introductory lines, then the output of program with arguments.
struct AgentHelp
{
    QStringList lines;
    QString name;
    QString program;
    QStringList arguments;
};

// A model a chat can use, with the reasoning efforts it supports.
struct AgentModel
{
    QString id;
    QString displayName;
    QString description;
    QStringList efforts;
    QStringList effortDescriptions;
    QString defaultEffort;
    bool isDefault = false;
};

// One usage limit window of the account, such as five hours or a week.
struct UsageLimit
{
    QString id;
    // Qualifies the window when an account has several of the same length, such as a model family.
    QString name;
    qint64 windowMinutes = 0;
    // Negative when the agent reported the window without a percentage.
    double usedPercent = -1;
    qint64 resetsAt = 0;
    QString status;
};

class AgentBackend;

// One kind of agent: it lists its conversations for the tree and creates a chat session per tab.
class AgentProvider : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString name() const = 0;
    virtual AgentHelp help() const = 0;
    // Describes another tool that holds the conversation open, or returns an empty string.
    virtual QString externalLock(const QString &id) const
    {
        Q_UNUSED(id);
        return {};
    }

    // Conversations shown in the tree. Each entry has id, cwd, title, tooltip and createdAt, and
    // optionally archived. loadConversations() reads saved data at startup; refreshConversations()
    // discovers current ones and reports through conversationsChanged().
    virtual void loadConversations() = 0;
    virtual void refreshConversations() = 0;
    virtual QList<QJsonObject> conversations() const = 0;
    // Models chats can switch to; empty when the agent offers no choice.
    virtual QList<AgentModel> models() const { return {}; }
    // Account usage limits as far as the agent reports them.
    virtual QList<UsageLimit> usageLimits() const { return {}; }
    // Model and effort that new chats start with, from the application options; empty means the
    // agent's own default.
    QString defaultModel() const { return defaultModel_; }
    QString defaultEffort() const { return defaultEffort_; }
    void setDefaults(const QString &model, const QString &effort)
    {
        defaultModel_ = model;
        defaultEffort_ = effort;
    }

    virtual AgentBackend *createChat(const QString &workingDirectory, QObject *parent) = 0;

signals:
    void message(const QString &text);
    void stateChanged();
    void conversationsChanged();
    void modelsChanged();
    void usageChanged();

private:
    QString defaultModel_;
    QString defaultEffort_;
};

// One chat session with an agent. It owns its protocol state, prompt queue and turn state; a
// chat tab drives it through these calls and renders its signals.
class AgentBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString name() const = 0;
    virtual QString workingDirectory() const = 0;
    virtual QString sessionId() const = 0;
    virtual QString statusText() const = 0;
    virtual bool isResponding() const = 0;
    virtual bool canInterrupt() const = 0;

    // Each call reports through message() and returns false when the request cannot be started.
    virtual bool newConversation(const QString &workingDirectory) = 0;
    virtual bool resumeConversation(const QString &id, const QString &workingDirectory) = 0;
    // Model and reasoning effort for the next turns; empty means the agent's or the conversation's default.
    virtual QString model() const { return {}; }
    virtual QString effort() const { return {}; }
    virtual void setModel(const QString &model, const QString &effort)
    {
        Q_UNUSED(model);
        Q_UNUSED(effort);
    }

    // Read-only turns: the agent may read files but not change them. Only agents that enforce this
    // themselves offer it; the setting applies from the next message on.
    virtual bool supportsReadOnly() const { return false; }
    virtual bool isReadOnly() const { return false; }
    virtual void setReadOnly(bool readOnly) { Q_UNUSED(readOnly); }

    // Queues a message for the current conversation and sends it when the agent is free.
    virtual bool prompt(const QString &text) = 0;
    virtual void interrupt() = 0;

    // Loads the latest history of a conversation, or older entries when older is true.
    virtual void loadHistory(const QString &id, const QString &workingDirectory, bool older) = 0;
    virtual void cancelHistory() = 0;

    virtual void answerApproval(int id, ApprovalDecision decision) = 0;
    // Answers are keyed by AgentQuestion::id; a missing key means the question was not answered.
    // A question with multiSelect may have several values.
    virtual void answerQuestions(int id, const QHash<QString, QStringList> &answers) = 0;

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
    void approvalRequested(int id, const QString &title, const QString &description, bool canAcceptForSession);
    void questionsRequested(int id, const QList<AgentQuestion> &questions);
    void conversationOpened(const QString &id, bool resumed);
    void conversationOpenFailed(const QString &id, const QString &reason);
    // Entries are the complete list to show for the conversation, oldest first.
    void historyLoaded(const QString &id, const QList<ChatEntry> &entries, bool hasMore, const QString &notice);
};
