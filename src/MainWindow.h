#pragma once

#include "AgentBackend.h"

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class AgentBackend;
class ClaudeBridge;
class CodexAgent;
class GeminiBridge;
class AntigravityBridge;
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
    void sendNextClaudePrompt();
    void sendNextGlmPrompt();
    void sendNextGeminiPrompt();
    void sendNextAntigravityPrompt();
    bool loadLocalConversations();
    bool saveLocalConversations();
    void rememberLocalConversation(const QString &provider, const QString &id);
    void loadRecentDirectories();
    void saveRecentDirectories();
    void rememberRecentDirectory(const QString &path);
    void newProviderConversation(int providerIndex, const QString &path);
    void showNewConversationDialog();
    void resumeProviderConversation(const QString &provider, const QString &id, const QString &path);
    void fetchClaudeSessions();
    void fetchGeminiSessions();
    void refreshConversationTree();
    static QString providerName(int index);
    static int providerIndex(const QString &name);
    QString providerWorkingDirectory(int index) const;
    void selectProvider(int index);
    void requestStop();
    void appendText(const QString &text);
    void appendLine(const QString &text);
    void appendChatText(const QString &provider, const QString &text);
    void showChatPreview(QTreeWidgetItem *item);
    void attachChat(QTreeWidgetItem *item);
    void openChat(const QString &provider, const QString &id, const QString &path, const QString &title);
    QString liveSessionId(int index) const;
    QString externalLock(const QString &provider, const QString &id) const;
    void showLiveChat(const QString &provider, const QString &path);
    void loadHistory(bool reset);
    void showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice = {});
    void updateChatHeader();
    void updateStatus();

    QString workingDirectory_;
    QString claudePython_;
    QString claudeScript_;
    QString localIndexPath_;
    QString geminiDataDirectory_;
    QString claudeWorkingDirectory_;
    QString glmWorkingDirectory_;
    QString geminiWorkingDirectory_;
    QString antigravityWorkingDirectory_;
    QString claudeSessionId_;
    QString glmSessionId_;
    QString pendingClaudeResumeId_;
    QString pendingGlmResumeId_;
    QString claudeFirstPrompt_;
    QString glmFirstPrompt_;
    QString geminiFirstPrompt_;
    QString antigravityFirstPrompt_;
    CodexAgent *codex_;
    ClaudeBridge *claude_;
    ClaudeBridge *glm_;
    GeminiBridge *gemini_;
    AntigravityBridge *antigravity_;
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
    QStringList claudeQueuedPrompts_;
    QStringList glmQueuedPrompts_;
    QStringList geminiQueuedPrompts_;
    QStringList antigravityQueuedPrompts_;
    QHash<QString, QJsonObject> localConversations_;
    QList<QProcess *> historyProcesses_;
    QSet<QString> expandedProviders_;
    int currentProvider_ = 0;
    QString viewProvider_;
    QString viewId_;
    QString viewPath_;
    QString viewTitle_;
    QList<ChatEntry> historyEntries_;
    QString liveTranscript_;
    QStringList recentDirectories_;
    QString lockNotice_;
    QString pendingAttachId_;
    quint64 historyGeneration_ = 0;
    int historyLimit_ = 0;
    int historyTotal_ = 0;
    bool viewLive_ = false;
    bool geminiExecutableChecked_ = false;
    bool claudeReady_ = false;
    bool claudeBusy_ = false;
    bool claudeStopRequested_ = false;
    bool claudeTextStarted_ = false;
    bool glmReady_ = false;
    bool glmBusy_ = false;
    bool glmStopRequested_ = false;
    bool glmTextStarted_ = false;
    bool geminiBusy_ = false;
    bool geminiStopRequested_ = false;
    bool geminiTextStarted_ = false;
    bool antigravityBusy_ = false;
    bool antigravityStopRequested_ = false;
    bool antigravityTextStarted_ = false;
};
