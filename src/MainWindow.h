#pragma once

#include "AgentBackend.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class AntigravityAgent;
class ClaudeAgent;
class CodexAgent;
class CodexConnection;
class GeminiAgent;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(const QString &codexProgram, const QString &workingDirectory,
               const QString &claudePython = {}, const QString &claudeScript = {},
               const QString &geminiProgram = "gemini", QWidget *parent = nullptr,
               const QString &codexIndexPath = {}, const QString &antigravityProgram = "agy",
               const QString &geminiDataDirectory = {});
    ~MainWindow() override;

private:
    void submitCommand();
    void showHelp();
    void connectAgent(AgentBackend *agent);
    QHash<QString, QString> askQuestions(const QList<AgentQuestion> &questions);
    AgentBackend *agent(int index) const { return agents_.value(index); }
    AgentBackend *currentAgent() const { return agent(currentProvider_); }
    void loadRecentDirectories();
    void saveRecentDirectories();
    void rememberRecentDirectory(const QString &path);
    void newProviderConversation(int providerIndex, const QString &path);
    void showNewConversationDialog();
    void refreshConversationTree();
    QString providerName(int index) const;
    int providerIndex(const QString &name) const;
    void selectProvider(int index);
    void requestStop();
    void appendText(const QString &text);
    void appendLine(const QString &text);
    void appendChatText(const QString &provider, const QString &text);
    void showChatPreview(QTreeWidgetItem *item);
    void attachChat(QTreeWidgetItem *item);
    void openChat(const QString &provider, const QString &id, const QString &path, const QString &title);
    void showLiveChat(const QString &provider, const QString &path);
    void loadHistory(bool reset);
    void showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice = {});
    void updateChatHeader();
    void updateStatus();

    QString workingDirectory_;
    QString dataDirectory_;
    CodexConnection *codexConnection_;
    CodexAgent *codex_;
    ClaudeAgent *claude_;
    ClaudeAgent *glm_;
    GeminiAgent *gemini_;
    AntigravityAgent *antigravity_;
    // Provider order in the tree and the New chat dialog; providerName() and providerIndex() follow it.
    QList<AgentBackend *> agents_;
    QPlainTextEdit *chatView_;
    QPlainTextEdit *log_;
    QLabel *chatHeader_;
    QPushButton *loadEarlierButton_;
    QLineEdit *input_;
    QPushButton *sendButton_;
    QPushButton *stopButton_;
    QPushButton *newChatButton_;
    QLabel *status_;
    QTreeWidget *conversationTree_;
    QSet<QString> expandedProviders_;
    int currentProvider_ = 0;
    QString viewProvider_;
    QString viewId_;
    QString viewPath_;
    QString viewTitle_;
    QString liveTranscript_;
    QStringList recentDirectories_;
    QString lockNotice_;
    QString pendingAttachId_;
    bool viewLive_ = false;
};
