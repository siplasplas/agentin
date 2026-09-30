#pragma once

#include "AgentBackend.h"

#include <QHash>
#include <QObject>

class QTextDocument;
class QTimer;
class TurnLocks;

// An approval or a set of questions from the agent, waiting for the user in the tab.
struct PendingRequest
{
    int id = 0;
    bool approval = false;
    QString title;
    QString description;
    bool canAcceptForSession = false;
    QString alwaysRule;
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
    QString conversationId() const { return id_; }
    // Identifies the conversation for MruTabWidget::findTab(); empty until it has an ID.
    QString key() const;
    static QString key(const QString &provider, const QString &id);
    QString title() const { return title_; }
    QString workingDirectory() const { return path_; }
    QString headerText() const;
    bool isLive() const { return live_; }
    bool hasMoreHistory() const { return hasMore_; }
    // The user's messages in this conversation as far as loaded, oldest first, for recalling them.
    QStringList userMessages() const { return historyMessages_ + sentMessages_; }

    // Live chat in the agent's directory; the conversation starts with the first message.
    void startDraft();
    bool startNew(const QString &workingDirectory);
    // Shows a saved conversation read-only; the provider may differ from the current one.
    void showPreview(AgentProvider *provider, const QString &id, const QString &workingDirectory,
                     const QString &title);
    // Makes the displayed conversation live unless another tool holds it.
    void continueChat();
    bool send(const QString &text);
    void loadEarlier();

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
    void requestsChanged();

private:
    void setAgent(AgentBackend *agent);
    void appendText(const QString &text);
    void showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice);
    void finishQuestions();
    // Hands the next message to the agent once the previous turn has ended and the directory is free.
    // Returns false when the agent refused the message.
    bool dispatch();
    void releaseDirectory();

    AgentProvider *provider_;
    AgentBackend *agent_ = nullptr;
    QTextDocument *document_;
    QString id_;
    QString path_;
    QString title_;
    QString liveTranscript_;
    QStringList historyMessages_;
    QStringList sentMessages_;
    QString lockNotice_;
    QList<PendingRequest> requests_;
    QStringList sessionApprovals_;
    TurnLocks *locks_ = nullptr;
    QTimer *retry_;
    QStringList outgoing_;
    QString waitingFor_;
    bool inTurn_ = false;
    bool holdsDirectory_ = false;
    bool live_ = false;
    bool hasMore_ = false;
    bool pendingAttach_ = false;
};
