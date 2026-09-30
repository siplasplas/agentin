#pragma once

#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

class AgentProvider;
class AntigravityProvider;
class ChatTab;
class ClaudeProvider;
class CodexConnection;
class GeminiProvider;
class MessageInput;
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

private:
    void submitCommand();
    void showHelp();
    void requestStop();
    AgentProvider *provider(const QString &name) const;
    void loadSettings();
    void saveSettings();
    void showOptionsDialog();
    void showApprovalsDialog();
    void loadRecentDirectories();
    void saveRecentDirectories();
    void rememberRecentDirectory(const QString &path);
    void showNewConversationDialog();
    void newConversation(AgentProvider *provider, const QString &path);
    void refreshConversationTree();
    void openConversation(QTreeWidgetItem *item, bool continueChat);
    QWidget *addChatTab(AgentProvider *provider, const QString &workingDirectory);
    ChatTab *chatTab(QWidget *page) const;
    ChatTab *currentTab() const;
    void showCurrentTab();
    void updateTab(QWidget *page);
    void appendText(const QString &text);
    void appendLine(const QString &text);
    void updateStatus();
    void updateModelControls();
    void updateUsage();
    void showEnterAction(bool sends);
    void updateRequestPanel();
    void chooseModel();

    QString workingDirectory_;
    QString dataDirectory_;
    CodexConnection *codex_;
    ClaudeProvider *claude_;
    ClaudeProvider *glm_;
    GeminiProvider *gemini_;
    AntigravityProvider *antigravity_;
    // Provider order in the tree and the New chat dialog.
    QList<AgentProvider *> providers_;
    TurnLocks *turnLocks_;
    MruTabWidget *tabs_;
    // One chat view shared by all tabs; it moves into the current tab's page and shows its document.
    QWidget *chatPanel_;
    QLabel *chatHeader_;
    QPushButton *loadEarlierButton_;
    QPlainTextEdit *chatView_;
    QTextDocument *emptyDocument_;
    // Shows the current tab's waiting approval or question below the chat.
    QWidget *requestPanel_;
    // Which request the panel shows; rebuilding it would drop the options the user ticked.
    QString requestPanelState_;
    QPlainTextEdit *log_;
    QComboBox *modelInput_;
    QComboBox *effortInput_;
    QCheckBox *readOnlyInput_;
    // What the model controls last showed; rebuilding them would close an open drop-down list.
    QString modelControlsState_;
    MessageInput *input_;
    QLabel *enterIndicator_;
    QPushButton *sendButton_;
    QPushButton *stopButton_;
    QPushButton *newChatButton_;
    QLabel *status_;
    QLabel *usage_;
    QTreeWidget *conversationTree_;
    QSet<QString> expandedProviders_;
    QStringList recentDirectories_;
    // Ctrl+Z in the message field can bring back the message just sent.
    bool undoAfterSend_ = true;
};
