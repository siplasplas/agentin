#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class ClaudeBridge;
class GeminiBridge;
class AntigravityBridge;
class QTreeWidget;
class QTreeWidgetItem;

struct ChatEntry
{
    QString role;
    QString text;
};

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
    void startThread();
    void sendNextPrompt();
    void sendNextClaudePrompt();
    void sendNextGlmPrompt();
    void sendNextGeminiPrompt();
    void sendNextAntigravityPrompt();
    Q_INVOKABLE void syncCodexConversations();
    void requestCodexConversationPage();
    void handleCodexConversationPage(const QJsonObject &result);
    void finishCodexConversationSync();
    bool loadCodexConversationIndex();
    bool saveCodexConversationIndex();
    bool loadLocalConversations();
    bool saveLocalConversations();
    void rememberLocalConversation(const QString &provider, const QString &id);
    void newProviderConversation(int providerIndex, const QString &path);
    void showNewConversationDialog();
    void resumeProviderConversation(const QString &provider, const QString &id, const QString &path);
    void fetchClaudeSessions();
    void fetchGeminiSessions();
    void refreshConversationTree();
    Q_INVOKABLE void newCodexConversation(const QString &path);
    Q_INVOKABLE void resumeCodexConversation(const QString &id, const QString &path);
    static QString providerName(int index);
    QString providerWorkingDirectory(int index) const;
    void selectProvider(int index);
    void requestStop();
    void sendStopIfPossible();
    qint64 sendRequest(const QString &method, const QJsonObject &params);
    void sendNotification(const QString &method, const QJsonObject &params);
    void sendJson(const QJsonObject &message);
    void handleLine(const QByteArray &line);
    void handleResponse(const QJsonObject &message);
    void handleNotification(const QString &method, const QJsonObject &params);
    void handleServerRequest(const QString &method, const QJsonValue &id, const QJsonObject &params);
    void appendText(const QString &text);
    void appendLine(const QString &text);
    void appendChatText(const QString &provider, const QString &text);
    void showChatPreview(QTreeWidgetItem *item);
    void showLiveChat(const QString &provider, const QString &path);
    void loadHistory(bool reset);
    void showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice = {});
    void updateChatHeader();
    void updateStatus();

    QString codexProgram_;
    QString workingDirectory_;
    QString codexIndexPath_;
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
    QProcess *server_;
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
    QByteArray readBuffer_;
    QHash<qint64, QString> pendingRequests_;
    QSet<QString> streamedMessages_;
    QSet<QString> streamedCommands_;
    QStringList queuedPrompts_;
    QStringList claudeQueuedPrompts_;
    QStringList glmQueuedPrompts_;
    QStringList geminiQueuedPrompts_;
    QStringList antigravityQueuedPrompts_;
    QString threadId_;
    QString activeTurnId_;
    QString codexWorkingDirectory_;
    QString syncCursor_;
    QHash<QString, QJsonObject> cachedCodexConversations_;
    QHash<QString, QJsonObject> localConversations_;
    QList<QProcess *> historyProcesses_;
    QHash<QString, QJsonObject> stagedCodexConversations_;
    QSet<QString> newCodexConversationIds_;
    QSet<QString> expandedProviders_;
    qint64 activeConversationWatermark_ = 0;
    int currentProvider_ = 0;
    QString viewProvider_;
    QString viewId_;
    QString viewPath_;
    QString viewTitle_;
    QList<ChatEntry> historyEntries_;
    QString codexHistoryCursor_;
    qint64 codexHistoryRequest_ = 0;
    quint64 historyGeneration_ = 0;
    int historyLimit_ = 0;
    int historyTotal_ = 0;
    bool viewLive_ = false;
    bool pendingCodexHistory_ = false;
    int syncPages_ = 0;
    qint64 nextRequestId_ = 1;
    bool initialized_ = false;
    bool codexThreadOpening_ = false;
    bool syncingCodexConversations_ = false;
    bool syncingArchivedCodexConversations_ = false;
    bool geminiExecutableChecked_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool stopSent_ = false;
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
