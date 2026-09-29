#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QMainWindow>
#include <QSet>
#include <QStringList>

class QLabel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class ClaudeBridge;
class GeminiBridge;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(const QString &codexProgram, const QString &workingDirectory,
               const QString &claudePython = {}, const QString &claudeScript = {},
               const QString &geminiProgram = "gemini", QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void submitCommand();
    void showHelp();
    void startThread();
    void sendNextPrompt();
    void sendNextClaudePrompt();
    void sendNextGlmPrompt();
    void sendNextGeminiPrompt();
    Q_INVOKABLE void addClaudeDirectory(const QString &path);
    Q_INVOKABLE void addGlmDirectory(const QString &path);
    Q_INVOKABLE void addCodexDirectory(const QString &path);
    Q_INVOKABLE void addGeminiDirectory(const QString &path);
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
    void updateStatus();

    QString codexProgram_;
    QString workingDirectory_;
    QProcess *server_;
    ClaudeBridge *claude_;
    ClaudeBridge *glm_;
    GeminiBridge *gemini_;
    QComboBox *provider_;
    QPlainTextEdit *output_;
    QLineEdit *input_;
    QPushButton *sendButton_;
    QPushButton *stopButton_;
    QPushButton *addDirButton_;
    QLabel *status_;
    QLabel *claudeDirsLabel_;
    QLabel *glmDirsLabel_;
    QLabel *geminiDirsLabel_;
    QLabel *codexDirsLabel_;
    QByteArray readBuffer_;
    QHash<qint64, QString> pendingRequests_;
    QSet<QString> streamedMessages_;
    QSet<QString> streamedCommands_;
    QStringList queuedPrompts_;
    QStringList claudeQueuedPrompts_;
    QStringList glmQueuedPrompts_;
    QStringList geminiQueuedPrompts_;
    QStringList claudeDirectories_;
    QStringList glmDirectories_;
    QStringList codexDirectories_;
    QString threadId_;
    QString activeTurnId_;
    qint64 nextRequestId_ = 1;
    bool initialized_ = false;
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
};
