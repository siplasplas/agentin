#pragma once

#include "AgentBackend.h"

#include <QHash>
#include <QElapsedTimer>
#include <QObject>

class QTextDocument;
class QTimer;
class ChangeTracker;
class TurnLocks;

// An approval or a set of questions from the agent, waiting for the user in the tab.
struct PendingRequest
{
    int id = 0;
    bool approval = false;
    bool blocking = true;
    QString title;
    QString description;
    bool canAcceptForSession = false;
    QString alwaysRule;
    QString sessionRule;
    QList<AgentQuestion> questions;
    // The question shown now; earlier ones are answered.
    qsizetype current = 0;
    QHash<QString, QStringList> answers;
};

// One chat shown in a tab: its agent session, the text it displays and whether it can be continued.
// A tab is either live (messages go to the agent) or a read-only view of a saved conversation.
class ChatTab : public QObject
{
    Q_OBJECT

public:
    ChatTab(AgentProvider *provider, const QString &workingDirectory, QObject *parent = nullptr);
    ~ChatTab() override;

    // Messages then wait until no other agent works in the same or a nested directory.
    void setTurnLocks(TurnLocks *locks);
    bool isWaiting() const { return !waitingFor_.isEmpty(); }
    // Drops the messages that wait for the directory.
    void cancelWaiting();

    AgentProvider *provider() const { return provider_; }
    AgentBackend *agent() const { return agent_; }
    QTextDocument *document() const { return document_; }
    QTextDocument *reasoningDocument() const { return reasoningDocument_; }
    QString conversationId() const { return id_; }
    // Identifies the conversation for MruTabWidget::findTab(); empty until it has an ID.
    QString key() const;
    static QString key(const QString &provider, const QString &id);
    QString title() const { return title_; }
    QString workingDirectory() const { return path_; }
    // The header without the changes summary is shown beside a button that has it.
    QString headerText(bool withChanges = true) const;
    // Null until the chat's first turn.
    ChangeTracker *changeTracker() const { return changeTracker_; }
    QString operationTimeText() const;
    bool isLive() const { return live_; }
    bool hasMoreHistory() const { return hasMore_; }
    // The user's messages in this conversation as far as loaded, oldest first, for recalling them.
    QStringList userMessages() const { return historyMessages_ + sentMessages_; }
    // The user's messages with the agent's text answers, without tools and reasoning, oldest first.
    struct Exchange
    {
        QString message;
        QString answer;
    };
    QList<Exchange> exchanges() const { return exchanges_; }
    // Ideas for the next message, kept with the exchange they were asked for.
    struct Suggestions
    {
        QString exchange;
        QStringList items;
        bool pending = false;
    };
    Suggestions &suggestions() { return suggestions_; }
    const Suggestions &suggestions() const { return suggestions_; }

    // Live chat in the agent's directory; the conversation starts with the first message.
    void startDraft();
    bool startNew(const QString &workingDirectory);
    // Shows a saved conversation read-only; the provider may differ from the current one.
    void showPreview(AgentProvider *provider, const QString &id, const QString &workingDirectory,
                     const QString &title);
    // Makes the displayed conversation live unless another tool holds it.
    void continueChat();
    bool send(const QString &text);
    bool steer(const QString &text);
    void loadEarlier();

    // Tokens of the latest turn and of the conversation. The conversation total covers the whole
    // conversation when the agent reports it (Codex), otherwise the turns sent from this tab.
    TokenUsage lastTurnUsage() const { return turnUsage_; }
    TokenUsage conversationUsage() const { return conversationUsage_; }
    bool conversationUsageIsComplete() const { return conversationUsageFromAgent_; }
    static QString shortUsage(const TokenUsage &usage);
    static QString usageDetails(const TokenUsage &usage);

    // Approvals and questions are answered in the tab, one request at a time.
    const PendingRequest *pendingRequest() const { return requests_.isEmpty() ? nullptr : &requests_.first(); }
    qsizetype pendingRequestCount() const { return requests_.size(); }
    void answerApproval(ApprovalDecision decision);
    // Approvals given "for this session" in this chat, as the user reads them.
    QStringList sessionApprovals() const { return sessionApprovals_; }
    void resetSessionApprovals();
    void answerQuestion(const QStringList &values);
    // Sends the answers given so far and leaves the remaining questions unanswered.
    void skipQuestions();
    // Uses text from the message field for the current question: option numbers or, where the agent
    // accepts it, the user's own words. Returns false when no question is waiting for text.
    bool answerWithText(const QString &text);

signals:
    void logMessage(const QString &text);
    // Title, header, live state or agent state changed.
    void changed();
    void textAppended();
    void userMessagesChanged();
    // A turn ended on its own, successfully or not; turns the user stopped are not reported.
    void turnEnded(bool succeeded, qint64 durationMs);
    void compactionChanged(bool started);
    void requestsChanged();
    void steeringFailed(const QString &text);
    // The chat's conversation is open with this ID, started here or resumed.
    void conversationOpened(const QString &id);

private:
    void setAgent(AgentBackend *agent);
    void appendText(const QString &text);
    void updateReasoning(const QString &id, const QString &text);
    void rebuildReasoning();
    void clearReasoning();
    void appendToolText(const QString &text);
    int toolGroup_ = 0;
    int nextToolGroup_ = 0;
    void showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice);
    void finishQuestions();
    // Hands the next message to the agent once the previous turn has ended and the directory is free.
    // Returns false when the agent refused the message.
    bool dispatch();
    void releaseDirectory();
    QString lockOwner() const;
    QString lockLabel() const;
    void updateTaskClock();
    void finishCompactionClock();
    QElapsedTimer taskClock_;
    QElapsedTimer compactionClock_;
    bool taskClockRunning_ = false;
    bool compactionClockRunning_ = false;
    qint64 lastOperationDurationMs_ = 0;
    QString lastOperationName_ = "Time";

    AgentProvider *provider_;
    AgentBackend *agent_ = nullptr;
    QTextDocument *document_;
    QTextDocument *reasoningDocument_;
    QStringList reasoningOrder_;
    QHash<QString, QString> reasoningItems_;
    QString id_;
    QString path_;
    QString title_;
    QString liveTranscript_;
    qsizetype historyRefreshCutoff_ = 0;
    QString historyRefreshQueuedText_;
    qsizetype historyRefreshSentCount_ = 0;
    QStringList historyMessages_;
    QStringList sentMessages_;
    QList<Exchange> exchanges_;
    Suggestions suggestions_;
    QString lockNotice_;
    QList<PendingRequest> requests_;
    QStringList sessionApprovals_;
    TokenUsage turnUsage_;
    TokenUsage conversationUsage_;
    bool conversationUsageFromAgent_ = false;
    TurnLocks *locks_ = nullptr;
    QTimer *retry_;
    // The files the latest turn changed; created with the first turn.
    ChangeTracker *changeTracker_ = nullptr;
    QTimer *changeRefresh_ = nullptr;
    QStringList outgoing_;
    QString waitingFor_;
    bool inTurn_ = false;
    qint64 turnStartedAt_ = 0;
    bool holdsDirectory_ = false;
    bool live_ = false;
    bool hasMore_ = false;
    bool pendingAttach_ = false;
};
