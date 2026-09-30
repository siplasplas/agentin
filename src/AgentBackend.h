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

// Token counts of a turn or a conversation; -1 means the agent did not report the value.
struct TokenUsage
{
    // All input tokens, including those read from the cache.
    qint64 input = -1;
    qint64 cached = -1;
    qint64 output = -1;
    qint64 reasoning = -1;
    qint64 total = -1;
    double costUsd = -1;
    // Tokens in the latest request and the model's context window.
    qint64 contextUsed = -1;
    qint64 contextWindow = -1;

    bool isEmpty() const { return input < 0 && output < 0 && total < 0; }
    static qint64 add(qint64 a, qint64 b) { return a < 0 ? b : (b < 0 ? a : a + b); }
    TokenUsage &operator+=(const TokenUsage &other)
    {
        input = add(input, other.input);
        cached = add(cached, other.cached);
        output = add(output, other.output);
        reasoning = add(reasoning, other.reasoning);
        total = add(total, other.total);
        costUsd = costUsd < 0 ? other.costUsd : (other.costUsd < 0 ? costUsd : costUsd + other.costUsd);
        if (other.contextUsed >= 0) contextUsed = other.contextUsed;
        if (other.contextWindow >= 0) contextWindow = other.contextWindow;
        return *this;
    }
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
    // Also add the rule the agent proposed, so similar actions are allowed from now on.
    AcceptAlways,
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
    // True when the operating system enforces read-only turns (a sandbox), rather than the agent
    // following a rule; only such turns may skip the directory lock.
    virtual bool readOnlyIsEnforced() const { return false; }
    // Directories besides the working directory that the agent may write to in the next turn.
    virtual QStringList writableDirectories() const { return {}; }

    // Queues a message for the current conversation and sends it when the agent is free. Every
    // accepted message ends with turnCompleted, also when the turn fails or the agent stops.
    virtual bool prompt(const QString &text) = 0;
    virtual void interrupt() = 0;
    virtual bool supportsCompaction() const { return false; }
    virtual bool canCompact() const { return false; }
    virtual bool isCompacting() const { return false; }
    virtual bool compact() { return false; }

    // Loads the latest history of a conversation, or older entries when older is true.
    virtual void loadHistory(const QString &id, const QString &workingDirectory, bool older) = 0;
    virtual void cancelHistory() = 0;

    virtual void answerApproval(int id, ApprovalDecision decision) = 0;
    // Withdraws everything allowed "for this session", where the agent can do that.
    virtual bool canResetSessionApprovals() const { return false; }
    virtual void resetSessionApprovals() {}
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
    // Tokens of the running or just finished turn; may be reported several times during a turn.
    void turnUsage(const TokenUsage &usage);
    // Tokens of the whole conversation, for agents that keep the total themselves (Codex).
    void conversationUsage(const TokenUsage &usage);
    // alwaysRule describes the lasting rule the agent proposes; empty when it proposes none.
    void approvalRequested(int id, const QString &title, const QString &description, bool canAcceptForSession,
                           const QString &alwaysRule);
    void questionsRequested(int id, const QList<AgentQuestion> &questions);
    void conversationOpened(const QString &id, bool resumed);
    void conversationOpenFailed(const QString &id, const QString &reason);
    // Entries are the complete list to show for the conversation, oldest first.
    void historyLoaded(const QString &id, const QList<ChatEntry> &entries, bool hasMore, const QString &notice);
    void contextCompacted();
    // Refreshes stored history between turns while queued messages stay in the tab.
    void historyRefreshStarted();
    void historyRefreshed(const QString &id, const QList<ChatEntry> &entries, bool hasMore);
};
