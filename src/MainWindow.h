#pragma once

#include "FileOpener.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>

class AgentProvider;
class AntigravityProvider;
class ChatTab;
class ChatView;
class UsageLimitsPanel;
class QAction;
class ClaudeProvider;
class CodexConnection;
class GeminiProvider;
class MessageInput;
class Notifier;
class QToolButton;
class QSplitter;
class MruTabWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTextDocument;
class QTreeWidget;
class QTreeWidgetItem;
class TurnLocks;

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

    // Reopens the tabs saved when the window was last closed, and saves them on close from now on.
    // The chat that opens at start stays when keepStartChat is true, otherwise it is replaced.
    void restoreSession(bool keepStartChat);
    // Claude and GLM run their bridge in a virtual environment in the application data directory,
    // created with claude-agent-sdk when one of them is first used.
    void useManagedClaudeEnvironment();
    // GLM, Gemini and Antigravity are offered only when experimental agents are enabled in the options.
    void setExperimentalAgentsEnabled(bool enabled);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void saveSession();
    void submitCommand();
    void submitSteer();
    void submitFromKeyboard();
    bool canSteerNow() const;
    void updateSteerButton();
    void suggestMessage();
    void showSuggestions(const QStringList &items);
    void updateSuggestButton();
    void showHelp();
    void requestStop();
    AgentProvider *provider(const QString &name) const;
    QList<AgentProvider *> visibleProviders() const;
    void loadSettings();
    void saveSettings();
    void showOptionsDialog();
    void applyChatFont(const QFont &font);
    void showApprovalsDialog();
    void showNotificationsDialog();
    void showMuteState();
    void loadRecentDirectories();
    void saveRecentDirectories();
    void rememberRecentDirectory(const QString &path);
    void showNewConversationDialog();
    void newConversation(AgentProvider *provider, const QString &path);
    void refreshConversationTree();
    QList<QJsonObject> treeChats(AgentProvider *provider) const;
    // Adds directory rows with their chats under root, or as top-level rows when root is null.
    void addDirectoryItems(QTreeWidgetItem *root, QList<QJsonObject> chats);
    bool isDirectoryExpanded(const QString &key) const;
    void setDirectoryExpanded(const QString &key, bool expanded);
    void applyTreeOptions();
    void updateTreeControls();
    // Level 1 shows the top rows only, level 2 one level deeper and level 3 everything.
    void setTreeLevel(int level);
    void discoverConversations();
    void revealCurrentConversation(bool expandBranch, bool focusTree = false);
    void openConversation(QTreeWidgetItem *item, bool continueChat);
    QWidget *addChatTab(AgentProvider *provider, const QString &workingDirectory);
    ChatTab *chatTab(QWidget *page) const;
    ChatTab *currentTab() const;
    void showCurrentTab();
    void updateTab(QWidget *page);
    void appendText(const QString &text);
    void appendLine(const QString &text);
    void updateStatus();
    void showChangesWindow();
    void showOpenWithDialog();
    void rememberFast(const QString &conversationId, bool fast);
    void updateOperationTime();
    void updateModelControls();
    void updateUsage();
    void logUsageLimits(AgentProvider *provider);
    void showEnterAction(bool sends);
    void updateRequestPanel();
    void chooseModel();

    QString workingDirectory_;
    QString dataDirectory_;
    QString lastAudioDirectory_;
    CodexConnection *codex_;
    ClaudeProvider *claude_;
    ClaudeProvider *glm_;
    GeminiProvider *gemini_;
    AntigravityProvider *antigravity_;
    // Provider order in the tree and the New chat dialog.
    QList<AgentProvider *> providers_;
    TurnLocks *turnLocks_;
    Notifier *notifier_;
    QToolButton *muteSounds_;
    MruTabWidget *tabs_;
    // One chat view shared by all tabs; it moves into the current tab's page and shows its document.
    QWidget *chatPanel_;
    QLabel *chatHeader_;
    // Shows the latest turn's changes summary and opens the changes window.
    QLabel *changesLink_;
    QAction *changesAction_;
    QPushButton *loadEarlierButton_;
    ChatView *chatView_;
    QTextDocument *emptyDocument_;
    // Shows the current tab's waiting approval or question below the chat.
    QWidget *requestPanel_;
    // Which request the panel shows; rebuilding it would drop the options the user ticked.
    QString requestPanelState_;
    QPlainTextEdit *log_;
    QPlainTextEdit *reasoning_;
    QComboBox *modelInput_;
    QComboBox *effortInput_;
    QCheckBox *fastInput_;
    QCheckBox *readOnlyInput_;
    QLabel *tokens_;
    QWidget *compactionPanel_;
    QPushButton *compactButton_;
    QLineEdit *contextTokens_;
    QLabel *operationTime_;
    // What the model controls last showed; rebuilding them would close an open drop-down list.
    QString modelControlsState_;
    bool chatRefreshPending_ = false;
    MessageInput *input_;
    QPushButton *sendButton_;
    // Whom Send sends to and what Enter does now; both are shown in Send's tooltip.
    QString sendTarget_;
    bool enterSends_ = true;
    void updateSendToolTip();
    QPushButton *steerButton_;
    QPushButton *suggestButton_;
    QPushButton *stopButton_;
    QPushButton *newChatButton_;
    QLabel *status_;
    UsageLimitsPanel *usage_;
    QSplitter *usageSplitter_;
    int usagePanelHeight_ = 0;
    // The limits last written to the log, by provider.
    QHash<QString, QString> loggedUsage_;
    QAction *usageVisibleAction_;
    QAction *reasoningVisibleAction_;
    QAction *toolsVisibleAction_;
    QTreeWidget *conversationTree_;
    QSet<QString> expandedProviders_;
    // The tree groups chats by agent and then directory, or by directory with the agent in each chat row.
    bool treeByDirectory_ = false;
    bool chatsByModified_ = true;
    bool directoriesByName_ = false;
    // Directory rows follow directoriesCollapsed_ except those the user toggled since the last level choice.
    bool directoriesCollapsed_ = false;
    QSet<QString> toggledDirectories_;
    QAction *groupByAgentAction_;
    QAction *groupByDirectoryAction_;
    QAction *chatsByCreatedAction_;
    QAction *chatsByModifiedAction_;
    QAction *directoriesByNameAction_;
    QAction *directoriesByRecentAction_;
    QAction *treeLevelActions_[3];
    QToolButton *treeGroupButton_;
    QToolButton *treeLevelButtons_[3];
    QStringList recentDirectories_;
    // Ctrl+Z in the message field can bring back the message just sent.
    bool undoAfterSend_ = true;
    bool experimentalAgents_ = false;
    // Which program opens changed files, by file name pattern.
    QList<OpenRule> openRules_;
    // Conversations whose Fast mode was last switched on, so that it is on again when they reopen.
    QStringList fastChats_;
    // Commands that run without asking, as their leading words; unchecked ones stay listed but do not apply.
    struct TrustedCommand
    {
        QString command;
        bool enabled = true;
    };
    QList<TrustedCommand> trustedCommands_;
    void applyTrustedCommands();
    bool sessionEnabled_ = false;
    // Restored Codex chats that continue once the App Server is connected.
    QList<QPointer<QWidget>> continueWhenConnected_;
};
