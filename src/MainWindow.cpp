#include "ClaudeBridge.h"
#include "GeminiBridge.h"
#include "MainWindow.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {
QString shortPreview(const QString &value)
{
    const QString singleLine = value.simplified();
    constexpr int limit = 72;
    return singleLine.size() > limit ? singleLine.left(limit - 1) + QChar(0x2026) : singleLine;
}

void limitCodexPreview(QJsonObject &thread)
{
    if (!thread.value("preview").isString()) return;
    QString preview = thread.value("preview").toString().simplified();
    constexpr int limit = 200;
    if (preview.size() > limit) preview = preview.left(limit - 1) + QChar(0x2026);
    thread.insert("preview", preview);
}
}

MainWindow::MainWindow(const QString &codexProgram, const QString &workingDirectory,
                       const QString &claudePython, const QString &claudeScript,
                       const QString &geminiProgram, QWidget *parent, const QString &codexIndexPath)
    : QMainWindow(parent), codexProgram_(codexProgram), workingDirectory_(workingDirectory),
      codexIndexPath_(codexIndexPath.isEmpty()
          ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("codex-conversations.json")
          : codexIndexPath),
      claudePython_(claudePython), claudeScript_(claudeScript),
      server_(new QProcess(this)),
      claude_(new ClaudeBridge(claudePython, claudeScript, workingDirectory, "claude", this)),
      glm_(new ClaudeBridge(claudePython, claudeScript, workingDirectory, "glm", this)),
      gemini_(new GeminiBridge(geminiProgram, workingDirectory, this))
{
    codexWorkingDirectory_ = workingDirectory_;
    claudeWorkingDirectory_ = workingDirectory_;
    glmWorkingDirectory_ = workingDirectory_;
    geminiWorkingDirectory_ = workingDirectory_;
    localIndexPath_ = QDir(QFileInfo(codexIndexPath_).absolutePath()).filePath("claude-conversations.json");
    setWindowTitle("agentdeskt — Codex, Claude, GLM and Gemini");
    resize(850, 600);
    auto *conversationMenu = menuBar()->addMenu("Conversations");
    auto *newConversationAction = conversationMenu->addAction("New conversation in directory…");
    connect(newConversationAction, &QAction::triggered, this, &MainWindow::showNewConversationDialog);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    provider_ = new QComboBox(central);
    provider_->setObjectName("providerSelect");
    provider_->addItems({"Codex", "Claude", "GLM", "Gemini"});
    addDirButton_ = new QPushButton("Add directory…", central);
    addDirButton_->setObjectName("addDirectoryButton");
    claudeDirsLabel_ = new QLabel("Additional Claude directories: none", central);
    claudeDirsLabel_->setObjectName("claudeDirectories");
    claudeDirsLabel_->setWordWrap(true);
    claudeDirsLabel_->setVisible(false);
    glmDirsLabel_ = new QLabel("Additional GLM directories: none", central);
    glmDirsLabel_->setObjectName("glmDirectories");
    glmDirsLabel_->setWordWrap(true);
    glmDirsLabel_->setVisible(false);
    geminiDirsLabel_ = new QLabel("Additional Gemini directories: none", central);
    geminiDirsLabel_->setObjectName("geminiDirectories");
    geminiDirsLabel_->setWordWrap(true);
    geminiDirsLabel_->setVisible(false);
    codexDirsLabel_ = new QLabel("Additional writable Codex directories: none", central);
    codexDirsLabel_->setObjectName("codexDirectories");
    codexDirsLabel_->setWordWrap(true);
    status_ = new QLabel(central);
    output_ = new QPlainTextEdit(central);
    output_->setObjectName("output");
    output_->setReadOnly(true);
    output_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    output_->setPlaceholderText("Agent responses will appear here.");
    input_ = new QLineEdit(central);
    input_->setObjectName("commandInput");
    input_->setPlaceholderText("Enter a message or help, then press Enter");
    sendButton_ = new QPushButton("Send", central);
    sendButton_->setObjectName("sendButton");
    stopButton_ = new QPushButton("Stop", central);
    stopButton_->setObjectName("stopButton");
    stopButton_->setEnabled(false);

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(sendButton_);
    inputRow->addWidget(stopButton_);
    auto *providerRow = new QHBoxLayout;
    providerRow->addWidget(new QLabel("Agent:", central));
    providerRow->addWidget(provider_);
    providerRow->addWidget(addDirButton_);
    providerRow->addStretch();
    layout->addLayout(providerRow);
    layout->addWidget(claudeDirsLabel_);
    layout->addWidget(glmDirsLabel_);
    layout->addWidget(geminiDirsLabel_);
    layout->addWidget(codexDirsLabel_);
    layout->addWidget(status_);
    layout->addWidget(output_, 1);
    layout->addLayout(inputRow);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    conversationTree_ = new QTreeWidget(splitter);
    conversationTree_->setObjectName("conversationTree");
    conversationTree_->setHeaderHidden(true);
    conversationTree_->setMinimumWidth(180);
    splitter->addWidget(conversationTree_);
    splitter->addWidget(central);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 590});
    setCentralWidget(splitter);
    connect(conversationTree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        const QString kind = item->data(0, Qt::UserRole).toString();
        const QString id = item->data(0, Qt::UserRole + 1).toString();
        const QString path = item->data(0, Qt::UserRole + 2).toString();
        if (kind == "codex-chat") resumeCodexConversation(id, path);
        else if (!kind.isEmpty()) resumeProviderConversation(kind, id, path);
    });
    connect(conversationTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toString() != "provider") return;
        const QString provider = item->text(0);
        expandedProviders_.insert(provider);
        if (provider == "GLM") loadLocalConversations();
        refreshConversationTree();
        if (provider == "Codex" && initialized_) syncCodexConversations();
        else if (provider == "Claude") fetchClaudeSessions();
        else if (provider == "Gemini") fetchGeminiSessions();
    });
    connect(conversationTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toString() != "provider") return;
        expandedProviders_.remove(item->text(0));
        refreshConversationTree();
    });

    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::submitCommand);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::submitCommand);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::requestStop);
    connect(addDirButton_, &QPushButton::clicked, this, [this] {
        const int index = provider_->currentIndex();
        const QString path = QFileDialog::getExistingDirectory(
            this, index == 0 ? "Add writable directory for Codex"
                             : (index == 1 ? "Add directory for Claude" : (index == 2 ? "Add directory for GLM" : "Add directory for Gemini")),
            workingDirectory_);
        if (!path.isEmpty()) {
            if (index == 0) addCodexDirectory(path);
            else if (index == 1) addClaudeDirectory(path);
            else if (index == 2) addGlmDirectory(path);
            else addGeminiDirectory(path);
        }
    });
    connect(provider_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        appendLine("[" + provider_->itemText(index) + " selected]");
        if (index == 1 && !claudeReady_) claude_->start(claudeWorkingDirectory_);
        if (index == 2 && !glmReady_) glm_->start(glmWorkingDirectory_);
        claudeDirsLabel_->setVisible(index == 1);
        glmDirsLabel_->setVisible(index == 2);
        geminiDirsLabel_->setVisible(index == 3);
        codexDirsLabel_->setVisible(index == 0);
        updateStatus();
    });
    connect(claude_, &ClaudeBridge::ready, this, [this] {
        claudeReady_ = true;
        if (!pendingClaudeResumeId_.isEmpty()) {
            const QString id = pendingClaudeResumeId_;
            pendingClaudeResumeId_.clear();
            claudeReady_ = false;
            claude_->resumeConversation(id, claudeWorkingDirectory_);
            return;
        }
        appendLine("[Connected to Claude Agent SDK]");
        updateStatus();
        sendNextClaudePrompt();
    });
    connect(claude_, &ClaudeBridge::textDelta, this, [this](const QString &text) {
        if (!claudeTextStarted_) {
            appendText("\nClaude: ");
            claudeTextStarted_ = true;
        }
        appendText(text);
    });
    connect(claude_, &ClaudeBridge::toolStarted, this, [this](const QString &name, const QJsonObject &input) {
        appendLine("\n[Claude tool: " + name + "] "
                   + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)));
    });
    connect(claude_, &ClaudeBridge::completed, this, [this](const QString &state, const QString &details) {
        if (claudeTextStarted_) appendText("\n");
        if (state != "completed") appendLine("[Claude response: " + state + (details.isEmpty() ? "" : ": " + details) + "]");
        claudeBusy_ = false;
        claudeStopRequested_ = false;
        claudeTextStarted_ = false;
        updateStatus();
        sendNextClaudePrompt();
    });
    connect(claude_, &ClaudeBridge::approvalRequested, this,
            [this](int id, const QString &tool, const QJsonObject &input) {
        const QString details = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Indented));
        const auto answer = QMessageBox::question(this, "Approve Claude action",
                                                   tool + "\n\n" + details + "\nAllow this action?",
                                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        claude_->answerApproval(id, answer == QMessageBox::Yes);
    });
    connect(claude_, &ClaudeBridge::questionsRequested, this, [this](int id, const QJsonArray &questions) {
        QJsonObject answers;
        bool allAccepted = true;
        for (const QJsonValue &value : questions) {
            const QJsonObject question = value.toObject();
            const QString text = question.value("question").toString();
            const QString title = question.value("header").toString("Claude question");
            const QJsonArray options = question.value("options").toArray();
            bool accepted = false;
            QString reply;
            if (question.value("multiSelect").toBool() || options.isEmpty()) {
                reply = QInputDialog::getText(this, title, text, QLineEdit::Normal, {}, &accepted);
            } else {
                QStringList labels;
                for (const QJsonValue &option : options) labels.append(option.toObject().value("label").toString());
                reply = QInputDialog::getItem(this, title, text, labels, 0, false, &accepted);
            }
            if (!accepted) {
                allAccepted = false;
                break;
            }
            answers.insert(text, reply);
        }
        claude_->answerQuestions(id, answers, allAccepted);
    });
    connect(claude_, &ClaudeBridge::directoryAdded, this, [this](const QString &path) {
        if (!claudeDirectories_.contains(path) && QDir::cleanPath(path) != QDir::cleanPath(workingDirectory_)) {
            claudeDirectories_.append(path);
        }
        claudeDirsLabel_->setText("Additional Claude directories: "
                                  + (claudeDirectories_.isEmpty() ? "none" : claudeDirectories_.join(", ")));
        appendLine("[Claude directory available: " + path + "]");
        claudeReady_ = true;
        updateStatus();
    });
    connect(claude_, &ClaudeBridge::error, this, [this](const QString &message) {
        appendLine("[Claude] " + message);
    });
    connect(claude_, &ClaudeBridge::disconnected, this, [this] {
        claudeReady_ = false;
        claudeBusy_ = false;
        claudeStopRequested_ = false;
        claudeDirectories_.clear();
        claudeDirsLabel_->setText("Additional Claude directories: none");
        updateStatus();
    });
    connect(glm_, &ClaudeBridge::ready, this, [this] {
        glmReady_ = true;
        if (!pendingGlmResumeId_.isEmpty()) {
            const QString id = pendingGlmResumeId_;
            pendingGlmResumeId_.clear();
            glmReady_ = false;
            glm_->resumeConversation(id, glmWorkingDirectory_);
            return;
        }
        appendLine("[Connected to GLM via Claude Agent SDK]");
        updateStatus();
        sendNextGlmPrompt();
    });
    connect(glm_, &ClaudeBridge::textDelta, this, [this](const QString &text) {
        if (!glmTextStarted_) {
            appendText("\nGLM: ");
            glmTextStarted_ = true;
        }
        appendText(text);
    });
    connect(glm_, &ClaudeBridge::toolStarted, this, [this](const QString &name, const QJsonObject &input) {
        appendLine("\n[GLM tool: " + name + "] "
                   + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)));
    });
    connect(glm_, &ClaudeBridge::completed, this, [this](const QString &state, const QString &details) {
        if (glmTextStarted_) appendText("\n");
        if (state != "completed") appendLine("[GLM response: " + state + (details.isEmpty() ? "" : ": " + details) + "]");
        glmBusy_ = false;
        glmStopRequested_ = false;
        glmTextStarted_ = false;
        updateStatus();
        sendNextGlmPrompt();
    });
    connect(glm_, &ClaudeBridge::approvalRequested, this,
            [this](int id, const QString &tool, const QJsonObject &input) {
        const QString details = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Indented));
        const auto answer = QMessageBox::question(this, "Approve GLM action",
                                                   tool + "\n\n" + details + "\nAllow this action?",
                                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        glm_->answerApproval(id, answer == QMessageBox::Yes);
    });
    connect(glm_, &ClaudeBridge::questionsRequested, this, [this](int id, const QJsonArray &questions) {
        QJsonObject answers;
        bool allAccepted = true;
        for (const QJsonValue &value : questions) {
            const QJsonObject question = value.toObject();
            const QString text = question.value("question").toString();
            const QString title = question.value("header").toString("GLM question");
            const QJsonArray options = question.value("options").toArray();
            bool accepted = false;
            QString reply;
            if (question.value("multiSelect").toBool() || options.isEmpty()) {
                reply = QInputDialog::getText(this, title, text, QLineEdit::Normal, {}, &accepted);
            } else {
                QStringList labels;
                for (const QJsonValue &option : options) labels.append(option.toObject().value("label").toString());
                reply = QInputDialog::getItem(this, title, text, labels, 0, false, &accepted);
            }
            if (!accepted) { allAccepted = false; break; }
            answers.insert(text, reply);
        }
        glm_->answerQuestions(id, answers, allAccepted);
    });
    connect(glm_, &ClaudeBridge::directoryAdded, this, [this](const QString &path) {
        if (!glmDirectories_.contains(path) && QDir::cleanPath(path) != QDir::cleanPath(workingDirectory_)) {
            glmDirectories_.append(path);
        }
        glmDirsLabel_->setText("Additional GLM directories: "
                               + (glmDirectories_.isEmpty() ? "none" : glmDirectories_.join(", ")));
        appendLine("[GLM directory available: " + path + "]");
        glmReady_ = true;
        updateStatus();
    });
    connect(glm_, &ClaudeBridge::error, this, [this](const QString &message) { appendLine("[GLM] " + message); });
    connect(glm_, &ClaudeBridge::disconnected, this, [this] {
        glmReady_ = false;
        glmBusy_ = false;
        glmStopRequested_ = false;
        glmDirectories_.clear();
        glmDirsLabel_->setText("Additional GLM directories: none");
        updateStatus();
    });
    connect(gemini_, &GeminiBridge::textDelta, this, [this](const QString &text) {
        if (!geminiTextStarted_) {
            appendText("\nGemini: ");
            geminiTextStarted_ = true;
        }
        appendText(text);
    });
    connect(gemini_, &GeminiBridge::toolStarted, this, [this](const QString &name, const QJsonObject &input) {
        appendLine("\n[Gemini tool: " + name + "] "
                   + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)));
    });
    connect(gemini_, &GeminiBridge::error, this, [this](const QString &message) {
        appendLine("[Gemini] " + message);
    });
    connect(gemini_, &GeminiBridge::completed, this, [this](const QString &state, const QString &details) {
        if (geminiTextStarted_) appendText("\n");
        if (state != "completed") appendLine("[Gemini response: " + state + (details.isEmpty() ? "" : ": " + details) + "]");
        geminiBusy_ = false;
        geminiStopRequested_ = false;
        geminiTextStarted_ = false;
        updateStatus();
        sendNextGeminiPrompt();
    });
    connect(claude_, &ClaudeBridge::sessionChanged, this, [this](const QString &id) {
        claudeSessionId_ = id;
        rememberLocalConversation("Claude", id);
    });
    connect(glm_, &ClaudeBridge::sessionChanged, this, [this](const QString &id) {
        glmSessionId_ = id;
        rememberLocalConversation("GLM", id);
    });
    connect(gemini_, &GeminiBridge::sessionChanged, this, [this](const QString &id) {
        rememberLocalConversation("Gemini", id);
    });
    connect(server_, &QProcess::started, this, [this] {
        sendRequest("initialize", {{"clientInfo", QJsonObject{
            {"name", "agentdeskt"}, {"title", "agentdeskt Qt"}, {"version", "0.1.0"}}}});
    });
    connect(server_, &QProcess::readyReadStandardOutput, this, [this] {
        readBuffer_ += server_->readAllStandardOutput();
        qsizetype newline;
        while ((newline = readBuffer_.indexOf('\n')) >= 0) {
            const QByteArray line = readBuffer_.left(newline).trimmed();
            readBuffer_.remove(0, newline + 1);
            if (!line.isEmpty()) handleLine(line);
        }
    });
    connect(server_, &QProcess::readyReadStandardError, this, [this] {
        const QString message = QString::fromUtf8(server_->readAllStandardError()).trimmed();
        if (!message.isEmpty()) appendLine("[Server] " + message);
    });
    connect(server_, &QProcess::errorOccurred, this, [this, codexProgram](QProcess::ProcessError) {
        appendLine("[Server startup error] " + server_->errorString());
        appendLine("Executable: " + codexProgram);
        appendLine("Working directory: " + workingDirectory_);
        appendLine("Try --codex /absolute/path/to/codex if the executable was not found.");
        updateStatus();
    });
    connect(server_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        if (syncingCodexConversations_) {
            syncingCodexConversations_ = false;
            stagedCodexConversations_.clear();
            appendLine("[Codex conversation sync stopped: server disconnected]");
        }
        initialized_ = false;
        busy_ = false;
        codexThreadOpening_ = false;
        stopRequested_ = false;
        stopSent_ = false;
        threadId_.clear();
        activeTurnId_.clear();
        refreshConversationTree();
        appendLine(QString("[Server exited with code %1]").arg(code));
        updateStatus();
    });

    appendLine("Directory: " + workingDirectory_);
    appendLine("Type help to see the available commands.\n");
    if (loadCodexConversationIndex()) {
        appendLine(QString("[Cached Codex conversations: %1]").arg(cachedCodexConversations_.size()));
    }
    loadLocalConversations();
    refreshConversationTree();
    updateStatus();
    if (!QDir(workingDirectory_).exists()) {
        appendLine("[Working directory does not exist: " + workingDirectory_ + "]");
        return;
    }
    server_->setWorkingDirectory(workingDirectory_);
    server_->start(codexProgram, {"app-server", "--stdio"});
    input_->setFocus();
}

MainWindow::~MainWindow()
{
    for (QProcess *process : historyProcesses_) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->terminate();
            if (!process->waitForFinished(1000)) {
                process->kill();
                process->waitForFinished(1000);
            }
        }
    }
    disconnect(claude_, nullptr, this, nullptr);
    disconnect(glm_, nullptr, this, nullptr);
    disconnect(gemini_, nullptr, this, nullptr);
    if (server_->state() != QProcess::NotRunning) {
        server_->terminate();
        if (!server_->waitForFinished(1000)) {
            server_->kill();
            server_->waitForFinished(1000);
        }
    }
}

void MainWindow::submitCommand()
{
    const QString command = input_->text().trimmed();
    if (command.isEmpty()) return;
    input_->clear();
    const QString local = command.toLower();

    if (local == "help" || local == "/help") {
        showHelp();
    } else if (local == "clear" || local == "/clear") {
        output_->clear();
    } else if (local == "new" || local == "/new") {
        showNewConversationDialog();
    } else if (local == "stop" || local == "/stop") {
        requestStop();
    } else if (local == "quit" || local == "/quit" || local == "exit") {
        close();
    } else {
        if (provider_->currentIndex() == 3) {
            if (gemini_->sessionId().isEmpty() && geminiFirstPrompt_.isEmpty()) geminiFirstPrompt_ = command;
            appendLine("You (Gemini): " + command);
            geminiQueuedPrompts_.append(command);
            sendNextGeminiPrompt();
            return;
        }
        if (provider_->currentIndex() == 2) {
            if (glmSessionId_.isEmpty() && glmFirstPrompt_.isEmpty()) glmFirstPrompt_ = command;
            appendLine("You (GLM): " + command);
            glmQueuedPrompts_.append(command);
            if (!glmReady_) glm_->start();
            sendNextGlmPrompt();
            return;
        }
        if (provider_->currentIndex() == 1) {
            if (claudeSessionId_.isEmpty() && claudeFirstPrompt_.isEmpty()) claudeFirstPrompt_ = command;
            appendLine("You (Claude): " + command);
            claudeQueuedPrompts_.append(command);
            if (!claudeReady_) claude_->start();
            sendNextClaudePrompt();
            return;
        }
        if (server_->state() == QProcess::NotRunning) {
            appendLine("[Server is not running. Check the codex executable and restart.]");
            return;
        }
        appendLine("You: " + command);
        queuedPrompts_.append(command);
        if (threadId_.isEmpty()) startThread();
        sendNextPrompt();
    }
}

void MainWindow::showHelp()
{
    const bool claudeSelected = provider_->currentIndex() == 1;
    const bool glmSelected = provider_->currentIndex() == 2;
    const bool geminiSelected = provider_->currentIndex() == 3;
    appendLine("Commands:");
    appendLine("  help       show this help and installed agent options");
    appendLine("  new        start a new conversation");
    appendLine("  clear      clear the output pane");
    appendLine("  stop       interrupt the current response");
    appendLine("  quit       close the application");
    appendLine("All other text is sent to the selected agent as a message.\n");
    QString helpProgram;
    QStringList helpArguments;
    QString helpName;
    if (geminiSelected) {
        appendLine("Gemini CLI headless mode:");
        appendLine("  Send messages with Enter or Send to Gemini.");
        appendLine("  Each response streams JSON events; later messages resume the same session.");
        appendLine("  Use Add directory to include a folder and its subfolders for future turns (maximum five).");
        appendLine("  Authenticate Gemini CLI before using this window.");
        appendLine("  If folder trust is enabled, trust the working folder in Gemini CLI first.");
        appendLine("  Headless mode: https://geminicli.com/docs/cli/headless/");
        appendLine("Installed Gemini CLI commands and options:");
        helpProgram = gemini_->program();
        helpArguments = {"--help"};
        helpName = "Gemini CLI";
    } else if (glmSelected) {
        appendLine("GLM via Z.AI and Claude Agent SDK:");
        appendLine("  Send messages with Enter or Send to GLM.");
        appendLine("  Use Add directory to include a folder and its subfolders for this session.");
        appendLine("  Tool approvals and questions appear in dialogs.");
        appendLine("  Requires claude-agent-sdk and ZAI_API_KEY; GLM_MODEL is optional.");
        appendLine("  GLM setup: https://docs.z.ai/devpack/tool/claude");
        appendLine("  Claude Code slash commands: https://code.claude.com/docs/en/commands");
        appendLine("Installed Claude CLI options (reference; GLM uses the SDK):");
        helpProgram = QStandardPaths::findExecutable("claude");
        helpArguments = {"--help"};
        helpName = "Claude CLI";
        if (helpProgram.isEmpty()) {
            appendLine("[Claude CLI is not installed or not in PATH.]\n");
            return;
        }
    } else if (claudeSelected) {
        appendLine("Claude Agent SDK:");
        appendLine("  Send a message with Enter or the Send to Claude button.");
        appendLine("  Use Add directory to give Claude access to a folder and its subfolders for this session.");
        appendLine("  Responses are streamed into this window.");
        appendLine("  Tool approvals and questions appear in dialogs.");
        appendLine("  Messages entered during a response are queued.");
        appendLine("  The selected Python environment needs claude-agent-sdk and API credentials.");
        appendLine("  Claude Code slash commands: https://code.claude.com/docs/en/commands");
        appendLine("Installed Claude CLI commands and options (reference; this window uses the SDK):");
        helpProgram = QStandardPaths::findExecutable("claude");
        helpArguments = {"--help"};
        helpName = "Claude CLI";
        if (helpProgram.isEmpty()) {
            appendLine("[Claude CLI is not installed or not in PATH.]\n");
            return;
        }
    } else {
        appendLine("Codex connection: codex app-server --stdio (direct JSONL).");
        appendLine("Use Add writable directory to grant Codex write access to a folder and its subfolders for future turns.");
        appendLine("Installed Codex App Server commands and options (reference; CLI subcommands are not chat messages):");
        helpProgram = codexProgram_;
        helpArguments = {"app-server", "--help"};
        helpName = "App Server";
    }

    auto *helpProcess = new QProcess(this);
    helpProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(helpProcess, &QProcess::errorOccurred, this, [this, helpProcess, helpName](QProcess::ProcessError error) {
        appendLine("[Could not load " + helpName + " options] " + helpProcess->errorString());
        if (error == QProcess::FailedToStart) helpProcess->deleteLater();
    });
    connect(helpProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, helpProcess, helpName](int code, QProcess::ExitStatus) {
        appendText(QString::fromUtf8(helpProcess->readAllStandardOutput()));
        if (code != 0) appendLine(QString("[%1 help exited with code %2]").arg(helpName).arg(code));
        appendText("\n");
        helpProcess->deleteLater();
    });
    helpProcess->start(helpProgram, helpArguments);
}

void MainWindow::addClaudeDirectory(const QString &path)
{
    if (path.isEmpty()) return;
    const QFileInfo directory(QDir(claudeWorkingDirectory_).absoluteFilePath(path));
    if (!directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    if (!claudeReady_ || claudeBusy_) {
        appendLine("[Wait for Claude to be ready before adding a directory.]");
        return;
    }
    claudeReady_ = false;
    claude_->addDirectory(directory.canonicalFilePath());
    appendLine("[Adding Claude directory: " + directory.canonicalFilePath() + "]");
    updateStatus();
}

void MainWindow::addGlmDirectory(const QString &path)
{
    if (path.isEmpty()) return;
    const QFileInfo directory(QDir(glmWorkingDirectory_).absoluteFilePath(path));
    if (!directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    if (!glmReady_ || glmBusy_) {
        appendLine("[Wait for GLM to be ready before adding a directory.]");
        return;
    }
    glmReady_ = false;
    glm_->addDirectory(directory.canonicalFilePath());
    appendLine("[Adding GLM directory: " + directory.canonicalFilePath() + "]");
    updateStatus();
}

void MainWindow::addGeminiDirectory(const QString &path)
{
    if (path.isEmpty()) return;
    const QFileInfo directory(QDir(geminiWorkingDirectory_).absoluteFilePath(path));
    if (!directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    const QString canonicalPath = directory.canonicalFilePath();
    if (canonicalPath != QDir(geminiWorkingDirectory_).canonicalPath() && !gemini_->addDirectory(canonicalPath)) {
        appendLine("[Gemini CLI supports up to five additional directories.]");
        return;
    }
    const QStringList directories = gemini_->directories();
    geminiDirsLabel_->setText("Additional Gemini directories: "
                               + (directories.isEmpty() ? "none" : directories.join(", ")));
    appendLine("[Gemini directory available for future turns: " + canonicalPath + "]");
}

void MainWindow::addCodexDirectory(const QString &path)
{
    if (path.isEmpty()) return;
    const QFileInfo directory(QDir(codexWorkingDirectory_).absoluteFilePath(path));
    if (!directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    const QString canonicalPath = directory.canonicalFilePath();
    if (!codexDirectories_.contains(canonicalPath) && canonicalPath != QDir(codexWorkingDirectory_).canonicalPath()) {
        codexDirectories_.append(canonicalPath);
    }
    codexDirsLabel_->setText("Additional writable Codex directories: "
                             + (codexDirectories_.isEmpty() ? "none" : codexDirectories_.join(", ")));
    appendLine("[Codex writable directory available for future turns: " + canonicalPath + "]");
}

void MainWindow::newCodexConversation(const QString &path)
{
    if (!initialized_) {
        appendLine("[Codex App Server is not connected.]");
        return;
    }
    if (busy_ || codexThreadOpening_) {
        appendLine("[Wait for the current Codex response to finish.]");
        return;
    }
    const QFileInfo directory(path);
    if (!directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    codexWorkingDirectory_ = directory.canonicalFilePath();
    queuedPrompts_.clear();
    threadId_.clear();
    activeTurnId_.clear();
    provider_->setCurrentIndex(0);
    startThread();
    refreshConversationTree();
}

void MainWindow::resumeCodexConversation(const QString &id, const QString &path)
{
    if (id.isEmpty() || !initialized_) return;
    if (busy_ || codexThreadOpening_) {
        appendLine("[Wait for the current Codex response to finish.]");
        return;
    }
    if (!path.isEmpty()) codexWorkingDirectory_ = path;
    queuedPrompts_.clear();
    activeTurnId_.clear();
    threadId_.clear();
    codexThreadOpening_ = true;
    provider_->setCurrentIndex(0);
    appendLine("[Resuming Codex conversation: " + id + "]");
    sendRequest("thread/resume", {{"threadId", id}});
    updateStatus();
}

void MainWindow::refreshConversationTree()
{
    const QSignalBlocker blocker(conversationTree_);
    conversationTree_->clear();
    const QStringList providers{"Codex", "Claude", "Gemini", "GLM"};
    QHash<QString, QTreeWidgetItem *> roots;
    QHash<QString, QTreeWidgetItem *> directories;
    for (const QString &provider : providers) {
        auto *root = new QTreeWidgetItem(conversationTree_, {provider});
        roots.insert(provider, root);
        root->setData(0, Qt::UserRole, "provider");
        root->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if (!expandedProviders_.contains(provider)) continue;
        root->setExpanded(true);
    }
    if (expandedProviders_.contains("Codex")) {
    QList<QJsonObject> threads = cachedCodexConversations_.values();
    std::sort(threads.begin(), threads.end(), [](const QJsonObject &left, const QJsonObject &right) {
        return left.value("createdAt").toInteger() > right.value("createdAt").toInteger();
    });
    for (const QJsonObject &thread : threads) {
        const QString id = thread.value("id").toString();
        const QString path = thread.value("cwd").toString();
        const QString directoryName = path.isEmpty() ? "(unknown directory)" : QDir::toNativeSeparators(path);
        const QString key = "Codex\n" + path;
        QTreeWidgetItem *directory = directories.value(key);
        if (!directory) {
            directory = new QTreeWidgetItem(roots.value("Codex"), {directoryName});
            directory->setToolTip(0, path);
            directory->setExpanded(true);
            directories.insert(key, directory);
        }
        QString title = thread.value("name").toString();
        if (title.isEmpty()) title = thread.value("preview").toString();
        if (title.isEmpty()) title = id;
        const QString fullTitle = title;
        title = shortPreview(title);
        if (thread.value("archived").toBool()) title += " (archived)";
        auto *chat = new QTreeWidgetItem(directory, {title});
        chat->setData(0, Qt::UserRole, "codex-chat");
        chat->setData(0, Qt::UserRole + 1, id);
        chat->setData(0, Qt::UserRole + 2, path);
        chat->setToolTip(0, fullTitle + "\n\n" + id);
    }
    if (!threadId_.isEmpty() && !cachedCodexConversations_.contains(threadId_)) {
        const QString key = "Codex\n" + codexWorkingDirectory_;
        QTreeWidgetItem *directory = directories.value(key);
        if (!directory) {
            directory = new QTreeWidgetItem(roots.value("Codex"), {QDir::toNativeSeparators(codexWorkingDirectory_)});
            directory->setExpanded(true);
            directories.insert(key, directory);
        }
        auto *chat = new QTreeWidgetItem(directory, {"Current chat"});
        chat->setData(0, Qt::UserRole, "codex-chat");
        chat->setData(0, Qt::UserRole + 1, threadId_);
        chat->setData(0, Qt::UserRole + 2, codexWorkingDirectory_);
        chat->setToolTip(0, threadId_);
    }
    }
    QList<QJsonObject> localThreads = localConversations_.values();
    std::sort(localThreads.begin(), localThreads.end(), [](const QJsonObject &left, const QJsonObject &right) {
        return left.value("createdAt").toInteger() > right.value("createdAt").toInteger();
    });
    for (const QJsonObject &thread : localThreads) {
        const QString provider = thread.value("provider").toString();
        if (!expandedProviders_.contains(provider)) continue;
        const QString id = thread.value("id").toString();
        const QString path = thread.value("cwd").toString();
        const QString key = provider + '\n' + path;
        QTreeWidgetItem *directory = directories.value(key);
        if (!directory) {
            directory = new QTreeWidgetItem(roots.value(provider), {QDir::toNativeSeparators(path)});
            directory->setToolTip(0, path);
            directory->setExpanded(true);
            directories.insert(key, directory);
        }
        const QString title = thread.value("title").toString(id);
        auto *chat = new QTreeWidgetItem(directory, {shortPreview(title)});
        chat->setData(0, Qt::UserRole, provider);
        chat->setData(0, Qt::UserRole + 1, id);
        chat->setData(0, Qt::UserRole + 2, path);
        chat->setToolTip(0, title + "\n\n" + id);
    }
    for (const QJsonObject &thread : externalGeminiConversations_) {
        if (!expandedProviders_.contains("Gemini")) break;
        const QString id = thread.value("id").toString();
        const QString path = thread.value("cwd").toString();
        const QString key = "Gemini\n" + path;
        QTreeWidgetItem *directory = directories.value(key);
        if (!directory) {
            directory = new QTreeWidgetItem(roots.value("Gemini"), {QDir::toNativeSeparators(path)});
            directory->setToolTip(0, path);
            directory->setExpanded(true);
            directories.insert(key, directory);
        }
        const QString title = thread.value("title").toString(id);
        auto *chat = new QTreeWidgetItem(directory, {shortPreview(title)});
        chat->setData(0, Qt::UserRole, "Gemini");
        chat->setData(0, Qt::UserRole + 1, id);
        chat->setData(0, Qt::UserRole + 2, path);
        chat->setToolTip(0, title + "\n\n" + id);
    }
}

QStringList MainWindow::knownDirectories() const
{
    QSet<QString> paths;
    const QStringList candidates{workingDirectory_, codexWorkingDirectory_, claudeWorkingDirectory_,
                                 glmWorkingDirectory_, geminiWorkingDirectory_};
    for (const QString &path : candidates) {
        const QFileInfo info(path);
        if (!info.isDir()) continue;
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) paths.insert(canonical);
    }
    for (const QJsonObject &thread : cachedCodexConversations_) {
        const QFileInfo info(thread.value("cwd").toString());
        if (!info.isDir()) continue;
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) paths.insert(canonical);
    }
    for (const QJsonObject &thread : localConversations_) {
        const QFileInfo info(thread.value("cwd").toString());
        if (!info.isDir()) continue;
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) paths.insert(canonical);
    }
    QStringList result = paths.values();
    result.sort();
    return result;
}

void MainWindow::fetchClaudeSessions()
{
    if (claudePython_.isEmpty() || claudeScript_.isEmpty()) return;
    auto *process = new QProcess(this);
    historyProcesses_.append(process);
    process->setWorkingDirectory(workingDirectory_);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int exitCode, QProcess::ExitStatus status) {
        historyProcesses_.removeAll(process);
        if (status == QProcess::NormalExit && exitCode == 0) {
            const QJsonDocument document = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed());
            bool changed = false;
            for (const QJsonValue &value : document.object().value("sessions").toArray()) {
                const QJsonObject session = value.toObject();
                const QString id = session.value("id").toString();
                const QString path = session.value("cwd").toString();
                if (id.isEmpty() || !QFileInfo(path).isDir()
                    || localConversations_.contains("GLM\n" + id)) continue;
                const QString key = "Claude\n" + id;
                if (localConversations_.contains(key)) continue;
                QJsonObject entry = session;
                entry.insert("provider", "Claude");
                localConversations_.insert(key, entry);
                changed = true;
            }
            if (changed) {
                saveLocalConversations();
                refreshConversationTree();
            }
        } else {
            const QJsonObject error = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
            QString message = error.value("message").toString();
            if (message.isEmpty()) message = QString::fromUtf8(process->readAllStandardError()).trimmed();
            if (!message.isEmpty()) appendLine("[Claude session discovery: " + message + "]");
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        historyProcesses_.removeAll(process);
        appendLine("[Claude session discovery: " + process->errorString() + "]");
        process->deleteLater();
    });
    process->start(claudePython_, {"-u", claudeScript_, "--cwd", workingDirectory_,
                                   "--list-sessions", "--directories", "[]"});
}

void MainWindow::fetchGeminiSessions()
{
    externalGeminiConversations_.clear();
    for (const QString &directory : knownDirectories()) {
        auto *process = new QProcess(this);
        historyProcesses_.append(process);
        process->setWorkingDirectory(directory);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, process, directory](int exitCode, QProcess::ExitStatus status) {
            historyProcesses_.removeAll(process);
            if (status == QProcess::NormalExit && exitCode == 0) {
                const QString output = QString::fromUtf8(process->readAllStandardOutput());
                const QRegularExpression row(R"(^\s*(\d+)\.\s*(.*?)\s*\[([^\]]+)\]\s*$)");
                for (const QString &line : output.split('\n')) {
                    const auto match = row.match(line);
                    if (!match.hasMatch()) continue;
                    const QString shortId = match.captured(3);
                    bool known = false;
                    for (const QJsonObject &entry : localConversations_) {
                        if (entry.value("provider").toString() == "Gemini"
                            && entry.value("cwd").toString() == directory
                            && entry.value("id").toString().startsWith(shortId)) {
                            known = true;
                            break;
                        }
                    }
                    if (known) continue;
                    const QString id = "index:" + match.captured(1);
                    externalGeminiConversations_.insert(directory + '\n' + id,
                        QJsonObject{{"id", id}, {"cwd", directory}, {"title", match.captured(2)}});
                }
                refreshConversationTree();
            }
            process->deleteLater();
        });
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            historyProcesses_.removeAll(process);
            appendLine("[Gemini session discovery: " + process->errorString() + "]");
            process->deleteLater();
        });
        process->start(gemini_->program(), {"--list-sessions"});
    }
}

bool MainWindow::loadCodexConversationIndex()
{
    QFile file(codexIndexPath_);
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) {
        appendLine("[Could not read Codex conversation index: " + file.errorString() + "]");
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (!document.isObject() || document.object().value("version").toInt() != 1
        || !document.object().value("threads").isArray()) {
        appendLine("[Invalid Codex conversation index: " + error.errorString() + "]");
        return false;
    }
    for (const QJsonValue &value : document.object().value("threads").toArray()) {
        QJsonObject thread = value.toObject();
        limitCodexPreview(thread);
        const QString id = thread.value("id").toString();
        if (!id.isEmpty()) cachedCodexConversations_.insert(id, thread);
    }
    return true;
}

bool MainWindow::saveCodexConversationIndex()
{
    if (!QDir().mkpath(QFileInfo(codexIndexPath_).absolutePath())) {
        appendLine("[Could not create directory for Codex conversation index]");
        return false;
    }
    QList<QJsonObject> threads = cachedCodexConversations_.values();
    std::sort(threads.begin(), threads.end(), [](const QJsonObject &left, const QJsonObject &right) {
        const qint64 leftDate = left.value("createdAt").toInteger();
        const qint64 rightDate = right.value("createdAt").toInteger();
        return leftDate == rightDate ? left.value("id").toString() < right.value("id").toString()
                                     : leftDate > rightDate;
    });
    QJsonArray data;
    for (const QJsonObject &thread : threads) data.append(thread);
    const QJsonObject root{{"version", 1},
                           {"syncedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {"threads", data}};
    QSaveFile file(codexIndexPath_);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        appendLine("[Could not save Codex conversation index: " + file.errorString() + "]");
        return false;
    }
    return true;
}

bool MainWindow::loadLocalConversations()
{
    const QDir directory(QFileInfo(localIndexPath_).absolutePath());
    const QStringList files{"claude-conversations.json", "glm-conversations.json", "gemini-conversations.json"};
    bool loaded = false;
    for (const QString &name : files) {
        const QString path = directory.filePath(name);
        QFile file(path);
        if (!file.exists()) continue;
        if (!file.open(QIODevice::ReadOnly)) {
            appendLine("[Could not read conversation index: " + file.errorString() + "]");
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject() || document.object().value("version").toInt() != 1
            || !document.object().value("threads").isArray()) {
            appendLine("[Invalid conversation index: " + path + "]");
            continue;
        }
        for (const QJsonValue &value : document.object().value("threads").toArray()) {
            const QJsonObject thread = value.toObject();
            const QString provider = thread.value("provider").toString();
            const QString id = thread.value("id").toString();
            if ((provider == "Claude" || provider == "Gemini" || provider == "GLM") && !id.isEmpty()) {
                localConversations_.insert(provider + '\n' + id, thread);
            }
        }
        loaded = true;
    }
    return loaded;
}

bool MainWindow::saveLocalConversations()
{
    if (!QDir().mkpath(QFileInfo(localIndexPath_).absolutePath())) return false;
    const QDir directory(QFileInfo(localIndexPath_).absolutePath());
    const QStringList providers{"Claude", "GLM", "Gemini"};
    for (const QString &provider : providers) {
        QJsonArray threads;
        for (const QJsonObject &thread : localConversations_) {
            if (thread.value("provider").toString() == provider) threads.append(thread);
        }
        QSaveFile file(directory.filePath(provider.toLower() + "-conversations.json"));
        if (!file.open(QIODevice::WriteOnly)
            || file.write(QJsonDocument(QJsonObject{{"version", 1}, {"threads", threads}})
                              .toJson(QJsonDocument::Indented)) < 0
            || !file.commit()) {
            appendLine("[Could not save " + provider + " conversation index: " + file.errorString() + "]");
            return false;
        }
    }
    return true;
}

void MainWindow::rememberLocalConversation(const QString &provider, const QString &id)
{
    if (id.isEmpty()) return;
    const QString key = provider + '\n' + id;
    QJsonObject entry = localConversations_.value(key);
    if (entry.isEmpty()) {
        const QString path = provider == "Claude" ? claudeWorkingDirectory_
            : (provider == "GLM" ? glmWorkingDirectory_ : geminiWorkingDirectory_);
        const QString prompt = provider == "Claude" ? claudeFirstPrompt_
            : (provider == "GLM" ? glmFirstPrompt_ : geminiFirstPrompt_);
        entry = {{"provider", provider}, {"id", id}, {"cwd", path},
                 {"title", prompt.isEmpty() ? id : prompt.left(120)},
                 {"createdAt", QDateTime::currentSecsSinceEpoch()}};
    }
    localConversations_.insert(key, entry);
    saveLocalConversations();
    refreshConversationTree();
}

void MainWindow::newProviderConversation(int providerIndex, const QString &path)
{
    if (providerIndex == 0) { newCodexConversation(path); return; }
    const QFileInfo directory(path);
    if (!directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    const QString canonicalPath = directory.canonicalFilePath();
    if (providerIndex == 1) {
        if (claudeBusy_) { appendLine("[Wait for Claude to finish.]"); return; }
        claudeWorkingDirectory_ = canonicalPath;
        claudeSessionId_.clear();
        claudeFirstPrompt_.clear();
        claudeQueuedPrompts_.clear();
        pendingClaudeResumeId_.clear();
        claudeReady_ = false;
        if (claude_->isRunning()) claude_->resetConversation(canonicalPath);
        else claude_->start(canonicalPath);
    } else if (providerIndex == 2) {
        if (glmBusy_) { appendLine("[Wait for GLM to finish.]"); return; }
        glmWorkingDirectory_ = canonicalPath;
        glmSessionId_.clear();
        glmFirstPrompt_.clear();
        glmQueuedPrompts_.clear();
        pendingGlmResumeId_.clear();
        glmReady_ = false;
        if (glm_->isRunning()) glm_->resetConversation(canonicalPath);
        else glm_->start(canonicalPath);
    } else {
        if (geminiBusy_) { appendLine("[Wait for Gemini to finish.]"); return; }
        geminiWorkingDirectory_ = canonicalPath;
        geminiFirstPrompt_.clear();
        geminiQueuedPrompts_.clear();
        gemini_->resetConversation(canonicalPath);
    }
    provider_->setCurrentIndex(providerIndex);
    appendLine("[Starting a new " + provider_->itemText(providerIndex) + " conversation in " + canonicalPath + "]");
    refreshConversationTree();
    updateStatus();
}

void MainWindow::showNewConversationDialog()
{
    const int index = provider_->currentIndex();
    const QString currentPath = index == 0 ? codexWorkingDirectory_
        : (index == 1 ? claudeWorkingDirectory_ : (index == 2 ? glmWorkingDirectory_ : geminiWorkingDirectory_));
    QDialog dialog(this);
    dialog.setWindowTitle("New " + provider_->itemText(index) + " conversation");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Working directory:", &dialog));
    auto *pathRow = new QHBoxLayout;
    auto *pathInput = new QLineEdit(currentPath, &dialog);
    pathInput->setObjectName("newConversationPath");
    auto *browse = new QPushButton("Browse…", &dialog);
    pathRow->addWidget(pathInput, 1);
    pathRow->addWidget(browse);
    layout->addLayout(pathRow);
    auto *validation = new QLabel(&dialog);
    layout->addWidget(validation);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Create chat");
    layout->addWidget(buttons);
    const auto validatePath = [validation, buttons](const QString &path) {
        const bool valid = !path.trimmed().isEmpty() && QFileInfo(path.trimmed()).isDir();
        validation->setText(valid ? "Directory exists" : "Directory does not exist");
        buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    };
    connect(pathInput, &QLineEdit::textChanged, &dialog, validatePath);
    connect(browse, &QPushButton::clicked, &dialog, [this, pathInput] {
        const QString path = QFileDialog::getExistingDirectory(this, "Choose working directory", pathInput->text());
        if (!path.isEmpty()) pathInput->setText(path);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validatePath(currentPath);
    dialog.resize(560, dialog.sizeHint().height());
    if (dialog.exec() == QDialog::Accepted) newProviderConversation(index, pathInput->text().trimmed());
}

void MainWindow::resumeProviderConversation(const QString &provider, const QString &id, const QString &path)
{
    if (id.isEmpty() || !QFileInfo(path).isDir()) return;
    if (provider == "Claude") {
        if (claudeBusy_) { appendLine("[Wait for Claude to finish.]"); return; }
        claudeWorkingDirectory_ = path;
        claudeSessionId_ = id;
        claudeFirstPrompt_.clear();
        claudeQueuedPrompts_.clear();
        claudeReady_ = false;
        if (claude_->isRunning()) claude_->resumeConversation(id, path);
        else { pendingClaudeResumeId_ = id; claude_->start(path); }
        provider_->setCurrentIndex(1);
    } else if (provider == "GLM") {
        if (glmBusy_) { appendLine("[Wait for GLM to finish.]"); return; }
        glmWorkingDirectory_ = path;
        glmSessionId_ = id;
        glmFirstPrompt_.clear();
        glmQueuedPrompts_.clear();
        glmReady_ = false;
        if (glm_->isRunning()) glm_->resumeConversation(id, path);
        else { pendingGlmResumeId_ = id; glm_->start(path); }
        provider_->setCurrentIndex(2);
    } else if (provider == "Gemini") {
        if (geminiBusy_) { appendLine("[Wait for Gemini to finish.]"); return; }
        geminiWorkingDirectory_ = path;
        geminiFirstPrompt_.clear();
        geminiQueuedPrompts_.clear();
        gemini_->resumeConversation(id, path);
        provider_->setCurrentIndex(3);
    } else return;
    appendLine("[Resuming " + provider + " conversation: " + id + "]");
    updateStatus();
}

void MainWindow::syncCodexConversations()
{
    if (syncingCodexConversations_) return;
    if (!initialized_) {
        appendLine("[Codex App Server is not connected.]");
        return;
    }
    stagedCodexConversations_ = cachedCodexConversations_;
    newCodexConversationIds_.clear();
    activeConversationWatermark_ = 0;
    for (const QJsonObject &thread : cachedCodexConversations_) {
        if (!thread.value("archived").toBool()) {
            activeConversationWatermark_ = qMax(activeConversationWatermark_, thread.value("createdAt").toInteger());
        }
    }
    syncCursor_.clear();
    syncPages_ = 0;
    syncingArchivedCodexConversations_ = false;
    syncingCodexConversations_ = true;
    appendLine("[Syncing Codex conversations…]");
    updateStatus();
    requestCodexConversationPage();
}

void MainWindow::requestCodexConversationPage()
{
    QJsonArray sources{"cli", "vscode", "exec", "appServer", "subAgent", "subAgentReview",
                       "subAgentCompact", "subAgentThreadSpawn", "subAgentOther", "unknown"};
    QJsonObject params{{"limit", 100}, {"sortKey", "created_at"}, {"sortDirection", "desc"},
                       {"sourceKinds", sources}, {"archived", syncingArchivedCodexConversations_}};
    if (!syncCursor_.isEmpty()) params.insert("cursor", syncCursor_);
    sendRequest("thread/list", params);
}

void MainWindow::handleCodexConversationPage(const QJsonObject &result)
{
    if (!syncingCodexConversations_) return;
    if (!result.value("data").isArray()) {
        appendLine("[Codex conversation sync failed: invalid thread/list response]");
        syncingCodexConversations_ = false;
        stagedCodexConversations_.clear();
        updateStatus();
        return;
    }
    ++syncPages_;
    bool olderThanCached = false;
    for (const QJsonValue &value : result.value("data").toArray()) {
        QJsonObject thread = value.toObject();
        limitCodexPreview(thread);
        const QString id = thread.value("id").toString();
        if (id.isEmpty()) continue;
        if (!syncingArchivedCodexConversations_ && activeConversationWatermark_ > 0
            && thread.value("createdAt").toInteger() < activeConversationWatermark_) {
            olderThanCached = true;
        }
        if (!cachedCodexConversations_.contains(id)) newCodexConversationIds_.insert(id);
        thread.insert("archived", syncingArchivedCodexConversations_);
        stagedCodexConversations_.insert(id, thread);
    }
    const QString nextCursor = result.value("nextCursor").toString();
    if (!nextCursor.isEmpty() && nextCursor == syncCursor_) {
        appendLine("[Codex conversation sync failed: repeated pagination cursor]");
        syncingCodexConversations_ = false;
        stagedCodexConversations_.clear();
        updateStatus();
        return;
    }
    if (!nextCursor.isEmpty() && !(olderThanCached && !syncingArchivedCodexConversations_)) {
        syncCursor_ = nextCursor;
        requestCodexConversationPage();
        return;
    }
    if (!syncingArchivedCodexConversations_) {
        syncingArchivedCodexConversations_ = true;
        syncCursor_.clear();
        requestCodexConversationPage();
    } else {
        finishCodexConversationSync();
    }
}

void MainWindow::finishCodexConversationSync()
{
    cachedCodexConversations_ = std::move(stagedCodexConversations_);
    stagedCodexConversations_.clear();
    syncingCodexConversations_ = false;
    const bool saved = saveCodexConversationIndex();
    appendLine(QString("[Codex conversations: %1 total, %2 new; fetched %3 pages%4]")
                   .arg(cachedCodexConversations_.size()).arg(newCodexConversationIds_.size()).arg(syncPages_)
                   .arg(saved ? "" : "; index not saved"));
    if (saved) appendLine("[Index: " + codexIndexPath_ + "]");
    refreshConversationTree();
    updateStatus();
}

void MainWindow::sendNextClaudePrompt()
{
    if (!claudeReady_ || claudeBusy_ || claudeQueuedPrompts_.isEmpty()) return;
    claudeBusy_ = true;
    claudeStopRequested_ = false;
    claudeTextStarted_ = false;
    claude_->prompt(claudeQueuedPrompts_.takeFirst());
    updateStatus();
}

void MainWindow::sendNextGlmPrompt()
{
    if (!glmReady_ || glmBusy_ || glmQueuedPrompts_.isEmpty()) return;
    glmBusy_ = true;
    glmStopRequested_ = false;
    glmTextStarted_ = false;
    glm_->prompt(glmQueuedPrompts_.takeFirst());
    updateStatus();
}

void MainWindow::sendNextGeminiPrompt()
{
    if (geminiBusy_ || geminiQueuedPrompts_.isEmpty()) return;
    geminiBusy_ = true;
    geminiStopRequested_ = false;
    geminiTextStarted_ = false;
    gemini_->prompt(geminiQueuedPrompts_.takeFirst());
    updateStatus();
}

void MainWindow::startThread()
{
    if (!initialized_ || codexThreadOpening_) return;
    codexThreadOpening_ = true;
    appendLine("[Starting a new conversation]");
    sendRequest("thread/start", {{"cwd", codexWorkingDirectory_}, {"serviceName", "agentdeskt"}});
    updateStatus();
}

void MainWindow::sendNextPrompt()
{
    if (!initialized_ || threadId_.isEmpty() || busy_ || queuedPrompts_.isEmpty()) return;
    const QString prompt = queuedPrompts_.takeFirst();
    busy_ = true;
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
    QJsonObject params{{"threadId", threadId_},
                       {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", prompt}}}}};
    if (!codexDirectories_.isEmpty()) {
        QJsonArray writableRoots{codexWorkingDirectory_};
        for (const QString &path : codexDirectories_) writableRoots.append(path);
        params.insert("sandboxPolicy", QJsonObject{{"type", "workspaceWrite"}, {"writableRoots", writableRoots}});
    }
    sendRequest("turn/start", params);
    updateStatus();
}

void MainWindow::requestStop()
{
    if (provider_->currentIndex() == 3) {
        if (!geminiBusy_) appendLine("[No active Gemini response.]");
        else if (!geminiStopRequested_) {
            geminiStopRequested_ = true;
            appendLine("[Interrupting Gemini response]");
            gemini_->interrupt();
            updateStatus();
        }
        return;
    }
    if (provider_->currentIndex() == 2) {
        if (!glmBusy_) appendLine("[No active GLM response.]");
        else if (!glmStopRequested_) {
            glmStopRequested_ = true;
            appendLine("[Interrupting GLM response]");
            glm_->interrupt();
            updateStatus();
        }
        return;
    }
    if (provider_->currentIndex() == 1) {
        if (!claudeBusy_) appendLine("[No active Claude response.]");
        else if (!claudeStopRequested_) {
            claudeStopRequested_ = true;
            appendLine("[Interrupting Claude response]");
            claude_->interrupt();
            updateStatus();
        }
        return;
    }
    if (!busy_) {
        appendLine("[No active response.]");
        return;
    }
    if (stopRequested_) return;
    stopRequested_ = true;
    appendLine("[Interrupting response]");
    sendStopIfPossible();
    updateStatus();
}

void MainWindow::sendStopIfPossible()
{
    if (!busy_ || !stopRequested_ || stopSent_ || threadId_.isEmpty() || activeTurnId_.isEmpty()) return;
    stopSent_ = true;
    sendRequest("turn/interrupt", {{"threadId", threadId_}, {"turnId", activeTurnId_}});
}

qint64 MainWindow::sendRequest(const QString &method, const QJsonObject &params)
{
    const qint64 id = nextRequestId_++;
    pendingRequests_.insert(id, method);
    sendJson({{"id", id}, {"method", method}, {"params", params}});
    return id;
}

void MainWindow::sendNotification(const QString &method, const QJsonObject &params)
{
    sendJson({{"method", method}, {"params", params}});
}

void MainWindow::sendJson(const QJsonObject &message)
{
    if (server_->state() == QProcess::NotRunning) return;
    server_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void MainWindow::handleLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        appendLine("[Invalid server response: " + error.errorString() + "]");
        return;
    }
    const QJsonObject message = document.object();
    if (message.contains("method")) {
        const QString method = message.value("method").toString();
        if (message.contains("id")) {
            handleServerRequest(method, message.value("id"), message.value("params").toObject());
        } else {
            handleNotification(method, message.value("params").toObject());
        }
    } else if (message.contains("id")) {
        handleResponse(message);
    }
}

void MainWindow::handleResponse(const QJsonObject &message)
{
    const qint64 id = message.value("id").toInteger();
    const QString method = pendingRequests_.take(id);
    const QJsonObject error = message.value("error").toObject();
    if (!error.isEmpty()) {
        appendLine("[Error in " + method + "] " + error.value("message").toString());
        if (method == "thread/list") {
            syncingCodexConversations_ = false;
            stagedCodexConversations_.clear();
        } else if (method == "thread/start" || method == "thread/resume") {
            codexThreadOpening_ = false;
        } else if (method == "turn/start") {
            busy_ = false;
            activeTurnId_.clear();
            stopRequested_ = false;
            stopSent_ = false;
            sendNextPrompt();
        } else if (method == "turn/interrupt") {
            stopRequested_ = false;
            stopSent_ = false;
        }
        updateStatus();
        return;
    }
    const QJsonObject result = message.value("result").toObject();
    if (method == "initialize") {
        initialized_ = true;
        sendNotification("initialized", {});
        if (expandedProviders_.contains("Codex")) syncCodexConversations();
    } else if (method == "thread/list") {
        handleCodexConversationPage(result);
    } else if (method == "thread/start" || method == "thread/resume") {
        codexThreadOpening_ = false;
        threadId_ = result.value("thread").toObject().value("id").toString();
        if (threadId_.isEmpty()) appendLine("[Server did not return a conversation ID.]");
        else appendLine(method == "thread/start" ? "[Connected to Codex]" : "[Resumed Codex conversation: " + threadId_ + "]");
        refreshConversationTree();
        sendNextPrompt();
    } else if (method == "turn/start") {
        if (busy_) {
            activeTurnId_ = result.value("turn").toObject().value("id").toString();
            sendStopIfPossible();
        }
    }
    updateStatus();
}

void MainWindow::handleNotification(const QString &method, const QJsonObject &params)
{
    const QJsonObject item = params.value("item").toObject();
    const QString itemId = params.value("itemId").toString(item.value("id").toString());
    if (method == "turn/started") {
        if (busy_) {
            activeTurnId_ = params.value("turn").toObject().value("id").toString();
            sendStopIfPossible();
        }
        updateStatus();
    } else if (method == "item/agentMessage/delta") {
        if (!streamedMessages_.contains(itemId)) {
            streamedMessages_.insert(itemId);
            appendText("\nCodex: ");
        }
        appendText(params.value("delta").toString());
    } else if (method == "item/commandExecution/outputDelta") {
        streamedCommands_.insert(itemId);
        appendText(params.value("delta").toString());
    } else if (method == "item/started" && item.value("type") == "commandExecution") {
        appendLine("\n[Command] " + item.value("command").toString());
    } else if (method == "item/completed") {
        const QString type = item.value("type").toString();
        const QString completedId = item.value("id").toString();
        if (type == "agentMessage") {
            if (streamedMessages_.remove(completedId)) appendText("\n");
            else appendLine("\nCodex: " + item.value("text").toString());
        } else if (type == "commandExecution") {
            if (!streamedCommands_.remove(completedId)) {
                const QString output = item.value("aggregatedOutput").toString();
                if (!output.isEmpty()) appendText(output);
            }
            appendLine(QString("\n[Command: %1]").arg(item.value("status").toString()));
        } else if (type == "fileChange") {
            appendLine(QString("[File changes: %1]").arg(item.value("status").toString()));
        }
    } else if (method == "turn/completed") {
        const QJsonObject turn = params.value("turn").toObject();
        const QString state = turn.value("status").toString();
        if (state != "completed") {
            QString details = turn.value("error").toObject().value("message").toString();
            if (!details.isEmpty()) details.prepend(": ");
            appendLine("[Response: " + state + details + "]");
        }
        busy_ = false;
        activeTurnId_.clear();
        stopRequested_ = false;
        stopSent_ = false;
        updateStatus();
        sendNextPrompt();
    } else if (method == "error") {
        appendLine("[Error] " + params.value("error").toObject().value("message").toString());
    } else if (method == "warning" || method == "configWarning") {
        appendLine("[Warning] " + params.value("message").toString(params.value("summary").toString()));
    }
}

void MainWindow::handleServerRequest(const QString &method, const QJsonValue &id, const QJsonObject &params)
{
    if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval") {
        QString description = params.value("reason").toString();
        if (method == "item/commandExecution/requestApproval") {
            const QJsonObject network = params.value("networkApprovalContext").toObject();
            if (!network.isEmpty()) {
                description += "\nNetwork access: " + network.value("protocol").toString()
                    + "://" + network.value("host").toString();
            } else {
                description += "\n" + params.value("command").toString();
                description += "\nDirectory: " + params.value("cwd").toString();
            }
        } else {
            description += "\n" + params.value("grantRoot").toString();
        }
        const auto answer = QMessageBox::question(this, "Approve Codex action",
                                                   description.trimmed() + "\n\nAllow this action?",
                                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        const QString decision = answer == QMessageBox::Yes ? "accept" : "decline";
        sendJson({{"id", id}, {"result", QJsonObject{{"decision", decision}}}});
        appendLine("[Approval: " + decision + "]");
    } else if (method == "item/tool/requestUserInput") {
        QJsonObject answers;
        for (const QJsonValue &value : params.value("questions").toArray()) {
            const QJsonObject question = value.toObject();
            const QString key = question.value("id").toString();
            const QString title = question.value("header").toString("Codex question");
            const QString prompt = question.value("question").toString();
            const QJsonArray options = question.value("options").toArray();
            bool accepted = false;
            QString reply;
            if (options.isEmpty()) {
                reply = QInputDialog::getText(this, title, prompt, QLineEdit::Normal, {}, &accepted);
            } else {
                QStringList labels;
                for (const QJsonValue &option : options) labels.append(option.toObject().value("label").toString());
                reply = QInputDialog::getItem(this, title, prompt, labels, 0, false, &accepted);
            }
            answers.insert(key, QJsonObject{{"answers", accepted ? QJsonArray{reply} : QJsonArray{}}});
        }
        sendJson({{"id", id}, {"result", QJsonObject{{"answers", answers}}}});
    } else {
        appendLine("[Unsupported server request: " + method + "]");
        sendJson({{"id", id}, {"error", QJsonObject{{"code", -32601}, {"message", "Unsupported request"}}}});
    }
}

void MainWindow::appendText(const QString &text)
{
    QTextCursor cursor = output_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    output_->setTextCursor(cursor);
    output_->ensureCursorVisible();
}

void MainWindow::appendLine(const QString &text)
{
    appendText(text + '\n');
}

void MainWindow::updateStatus()
{
    QString state;
    if (provider_->currentIndex() == 3) {
        state = geminiBusy_ ? "Gemini is responding…" : "Gemini ready";
        stopButton_->setEnabled(geminiBusy_ && !geminiStopRequested_);
    } else if (provider_->currentIndex() == 2) {
        if (glmBusy_) state = "GLM is responding…";
        else if (glmReady_) state = "GLM ready";
        else if (glm_->isRunning()) state = "Connecting to GLM via Claude Agent SDK…";
        else state = "GLM bridge is not running";
        stopButton_->setEnabled(glmBusy_ && !glmStopRequested_);
    } else if (provider_->currentIndex() == 1) {
        if (claudeBusy_) state = "Claude is responding…";
        else if (claudeReady_) state = "Claude ready";
        else if (claude_->isRunning()) state = "Connecting to Claude Agent SDK…";
        else state = "Claude bridge is not running";
        stopButton_->setEnabled(claudeBusy_ && !claudeStopRequested_);
    } else {
        if (server_->state() == QProcess::NotRunning) state = "Server: not running";
        else if (busy_) state = "Codex is responding…";
        else if (codexThreadOpening_) state = "Opening Codex conversation…";
        else if (initialized_) state = "Codex ready";
        else state = "Connecting to Codex App Server…";
        stopButton_->setEnabled(busy_ && !stopRequested_);
    }
    const QString currentDirectory = provider_->currentIndex() == 0 ? codexWorkingDirectory_
        : (provider_->currentIndex() == 1 ? claudeWorkingDirectory_
           : (provider_->currentIndex() == 2 ? glmWorkingDirectory_ : geminiWorkingDirectory_));
    status_->setText(state + "  •  " + QDir::toNativeSeparators(currentDirectory));
    const int index = provider_->currentIndex();
    sendButton_->setText(index == 0 ? "Send to Codex" : (index == 1 ? "Send to Claude" : (index == 2 ? "Send to GLM" : "Send to Gemini")));
    addDirButton_->setText(index == 0 ? "Add writable directory…" : "Add directory…");
    addDirButton_->setToolTip(index == 0
                             ? "Give Codex write access to another directory and its subfolders for future turns."
                             : "Give the selected agent access to another directory and its subfolders for this session.");
    addDirButton_->setEnabled(index == 0 ? !busy_ : (index == 1 ? (claudeReady_ && !claudeBusy_) : (index == 2 ? (glmReady_ && !glmBusy_) : !geminiBusy_)));
    provider_->setEnabled(!busy_ && !claudeBusy_ && !glmBusy_ && !geminiBusy_);
}
