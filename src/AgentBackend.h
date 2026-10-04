#pragma once

#include "CommandApproval.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

struct ChatEntry
{
    QString role;
    QString text;
    QString id;
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
    // optionally modifiedAt and archived. loadConversations() reads saved data at startup;
    // refreshConversations() discovers current ones and reports through conversationsChanged().
    virtual void loadConversations() = 0;
    virtual void refreshConversations() = 0;
    virtual QList<QJsonObject> conversations() const = 0;
    // A turn of the conversation has just ended in this application.
    virtual void recordActivity(const QString &id) { Q_UNUSED(id); }
    // A chat started here was named after its first message, before the agent's own list knows it.
    virtual void setConversationTitle(const QString &id, const QString &title)
    {
        Q_UNUSED(id);
        Q_UNUSED(title);
    }
    // Models chats can switch to; empty when the agent offers no choice.
    virtual QList<AgentModel> models() const { return {}; }
    // Account usage limits as far as the agent reports them.
    virtual QList<UsageLimit> usageLimits() const { return {}; }
    virtual bool supportsUsageLimits() const { return false; }
    // Enable explicit account-limit reads only for the visible provider panel.
    virtual void setUsageLimitsActive(bool active) { Q_UNUSED(active); }
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

    // Short ideas for the user's next message, asked of a light model outside the conversation, so that
    // its history and directory lock stay untouched. done receives the model's text or an error, unless
    // context has been destroyed by then.
    virtual bool supportsSuggestions() const { return false; }
    virtual void suggest(const QString &workingDirectory, const QString &prompt, QObject *context,
                         const std::function<void(const QString &text, const QString &error)> &done)
    {
        Q_UNUSED(workingDirectory);
        Q_UNUSED(prompt);
        Q_UNUSED(context);
        Q_UNUSED(done);
    }

    // Renames a conversation where the agent keeps its own titles, so that its other clients show the
    // name too. done receives an error or an empty string, unless context has been destroyed by then.
    virtual bool supportsRenaming() const { return false; }
    virtual void renameConversation(const QString &id, const QString &workingDirectory, const QString &title,
                                    QObject *context, const std::function<void(const QString &error)> &done)
    {
        Q_UNUSED(id);
        Q_UNUSED(workingDirectory);
        Q_UNUSED(title);
        Q_UNUSED(context);
        Q_UNUSED(done);
    }
    // The first user messages and answers of a conversation, for naming it.
    virtual void readConversationStart(const QString &id, const QString &workingDirectory, QObject *context,
                                       const std::function<void(const QList<ChatEntry> &entries,
                                                                const QString &error)> &done)
    {
        Q_UNUSED(id);
        Q_UNUSED(workingDirectory);
        Q_UNUSED(context);
        Q_UNUSED(done);
    }

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

    // Optional faster service for subsequent turns. This preference is not persisted.
    virtual bool supportsFastMode() const { return false; }
    virtual bool isFastMode() const { return false; }
    virtual void setFastMode(bool fast) { Q_UNUSED(fast); }

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
    virtual bool supportsSteering() const { return false; }
    virtual bool canSteer() const { return false; }
    virtual bool isSteering() const { return false; }
    virtual bool steer(const QString &text) { Q_UNUSED(text); return false; }
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
    // Rules managed by the application; revocation takes effect without reconnecting.
    virtual QStringList trustedSessionCommands() const { return {}; }
    // Trusts command prefixes for the conversation, as chosen for the commands of an approval.
    virtual void trustSessionCommands(const QStringList &) {}
    virtual bool removeTrustedSessionCommand(const QString &) { return false; }
    // Answers are keyed by AgentQuestion::id; a missing key means the question was not answered.
    // A question with multiSelect may have several values.
    virtual void answerQuestions(int id, const QHash<QString, QStringList> &answers) = 0;

signals:
    void message(const QString &text);
    void stateChanged();
    void messageStarted();
    void messageDelta(const QString &text);
    void messageFinished();
    // The complete currently available text of one reasoning item, updated while streaming.
    void reasoningUpdated(const QString &id, const QString &text);
    void toolStarted(const QString &name, const QString &details);
    void toolOutput(const QString &text);
    void toolFinished(const QString &name, const QString &status);
    void turnCompleted(const QString &status, const QString &details);
    // Tokens of the running or just finished turn; may be reported several times during a turn.
    void turnUsage(const TokenUsage &usage);
    // Tokens of the whole conversation, for agents that keep the total themselves (Codex).
    void conversationUsage(const TokenUsage &usage);
    // sessionRule identifies an application-managed command family, if available.
    // alwaysRule describes the lasting rule the agent proposes; empty when it proposes none.
    // choices are what agentin's rules let the user choose for each command that asks: trusting it for the chat
    // or adding a rule. The tab applies them, and the agent only gets the decision.
    void approvalRequested(int id, const QString &title, const QString &description, bool canAcceptForSession,
                           const QString &alwaysRule, const QString &sessionRule = {},
                           const QList<ApprovalChoice> &choices = {});
    void questionsRequested(int id, const QList<AgentQuestion> &questions, bool blocking = true);
    void requestResolved(int id);
    // The directories besides the working directory where the chat may write have changed.
    void writableDirectoriesChanged();
    void conversationOpened(const QString &id, bool resumed);
    void conversationOpenFailed(const QString &id, const QString &reason);
    // Entries are the complete list to show for the conversation, oldest first.
    void historyLoaded(const QString &id, const QList<ChatEntry> &entries, bool hasMore, const QString &notice);
    void compactionStarted();
    void compactionFinished();
    void contextCompacted();
    // How many tokens the conversation's context holds now, and the model's context window (-1 when not known),
    // for agents that report it apart from a turn's usage.
    void contextUsage(qint64 used, qint64 window);
    void steerAccepted(const QString &text);
    void steerFailed(const QString &text, const QString &reason);
    // Refreshes stored history between turns while queued messages stay in the tab.
    void historyRefreshStarted();
    void historyRefreshed(const QString &id, const QList<ChatEntry> &entries, bool hasMore);
};

Q_DECLARE_METATYPE(ApprovalChoice)
