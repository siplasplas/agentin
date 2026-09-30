#pragma once

#include "AgentBackend.h"

#include <QHash>
#include <QObject>

class QTextDocument;
class QWidget;

// One chat shown in a tab: its agent session, the text it displays and whether it can be continued.
// A tab is either live (messages go to the agent) or a read-only view of a saved conversation.
class ChatTab : public QObject
{
    Q_OBJECT

public:
    ChatTab(AgentProvider *provider, const QString &workingDirectory, QWidget *dialogParent,
            QObject *parent = nullptr);

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

signals:
    void logMessage(const QString &text);
    // Title, header, live state or agent state changed.
    void changed();
    void textAppended();
    void userMessagesChanged();
    // The agent needs an answer in a dialog, so the tab should be shown first.
    void activateRequested();

private:
    void setAgent(AgentBackend *agent);
    void appendText(const QString &text);
    void showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice);
    ApprovalDecision askApproval(const QString &title, const QString &description, bool canAcceptForSession);
    QHash<QString, QString> askQuestions(const QList<AgentQuestion> &questions);

    AgentProvider *provider_;
    AgentBackend *agent_ = nullptr;
    QWidget *dialogParent_;
    QTextDocument *document_;
    QString id_;
    QString path_;
    QString title_;
    QString liveTranscript_;
    QStringList historyMessages_;
    QStringList sentMessages_;
    QString lockNotice_;
    bool live_ = false;
    bool hasMore_ = false;
    bool pendingAttach_ = false;
};
