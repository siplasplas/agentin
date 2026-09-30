#include "AntigravityAgent.h"
#include "ClaudeAgent.h"
#include "CodexAgent.h"
#include "CodexConnection.h"
#include "GeminiAgent.h"
#include "MainWindow.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
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
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <qxfiledialog.h>

#include <algorithm>

namespace {
QString shortPreview(const QString &value)
{
    const QString singleLine = value.simplified();
    constexpr int limit = 72;
    return singleLine.size() > limit ? singleLine.left(limit - 1) + QChar(0x2026) : singleLine;
}

QString codexIndexFile(const QString &path)
{
    return path.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("codex-conversations.json")
        : path;
}
}

MainWindow::MainWindow(const QString &codexProgram, const QString &workingDirectory,
                       const QString &claudePython, const QString &claudeScript,
                       const QString &geminiProgram, QWidget *parent, const QString &codexIndexPath,
                       const QString &antigravityProgram, const QString &geminiDataDirectory)
    : QMainWindow(parent), workingDirectory_(workingDirectory),
      dataDirectory_(QFileInfo(codexIndexFile(codexIndexPath)).absolutePath()),
      codexConnection_(new CodexConnection(codexProgram, workingDirectory, codexIndexFile(codexIndexPath), this)),
      codex_(new CodexAgent(codexConnection_, workingDirectory, this)),
      claude_(new ClaudeAgent(claudePython, claudeScript, workingDirectory, "claude",
                              QDir(dataDirectory_).filePath("claude-conversations.json"), this)),
      glm_(new ClaudeAgent(claudePython, claudeScript, workingDirectory, "glm",
                           QDir(dataDirectory_).filePath("glm-conversations.json"), this)),
      gemini_(new GeminiAgent(geminiProgram, workingDirectory, geminiDataDirectory.isEmpty()
          ? QDir(QDir::homePath()).filePath(".gemini") : geminiDataDirectory,
          QDir(dataDirectory_).filePath("gemini-conversations.json"), this)),
      antigravity_(new AntigravityAgent(antigravityProgram, workingDirectory,
                                        QDir(dataDirectory_).filePath("antigravity-conversations.json"), this)),
      agents_{codex_, claude_, gemini_, glm_, antigravity_}
{
    claude_->excludeSessionsOf(glm_);
    setWindowTitle("agentdeskt — Codex, Claude, GLM, Gemini and Antigravity");
    resize(900, 700);
    auto *conversationMenu = menuBar()->addMenu("Conversations");
    auto *newConversationAction = conversationMenu->addAction("New conversation in directory…");
    connect(newConversationAction, &QAction::triggered, this, &MainWindow::showNewConversationDialog);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    newChatButton_ = new QPushButton("New chat…", central);
    newChatButton_->setObjectName("newChatButton");
    status_ = new QLabel(central);
    chatHeader_ = new QLabel(central);
    chatHeader_->setObjectName("chatHeader");
    chatHeader_->setWordWrap(true);
    loadEarlierButton_ = new QPushButton("Show earlier messages", central);
    loadEarlierButton_->setObjectName("loadEarlierButton");
    loadEarlierButton_->setVisible(false);
    chatView_ = new QPlainTextEdit(central);
    chatView_->setObjectName("chatView");
    chatView_->setReadOnly(true);
    chatView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    chatView_->setPlaceholderText("Select a chat in the tree or start a new one.");
    log_ = new QPlainTextEdit(this);
    log_->setObjectName("log");
    log_->setReadOnly(true);
    log_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    log_->setPlaceholderText("Application messages will appear here.");
    input_ = new QLineEdit(central);
    input_->setObjectName("commandInput");
    sendButton_ = new QPushButton("Send", central);
    sendButton_->setObjectName("sendButton");
    stopButton_ = new QPushButton("Stop", central);
    stopButton_->setObjectName("stopButton");
    stopButton_->setEnabled(false);

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(sendButton_);
    inputRow->addWidget(stopButton_);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(newChatButton_);
    layout->addLayout(statusRow);
    layout->addWidget(chatHeader_);
    layout->addWidget(loadEarlierButton_);
    layout->addWidget(chatView_, 1);
    layout->addLayout(inputRow);
    auto *chatSplitter = new QSplitter(Qt::Horizontal, this);
    conversationTree_ = new QTreeWidget(chatSplitter);
    conversationTree_->setObjectName("conversationTree");
    conversationTree_->setHeaderHidden(true);
    conversationTree_->setMinimumWidth(180);
    chatSplitter->addWidget(conversationTree_);
    chatSplitter->addWidget(central);
    chatSplitter->setStretchFactor(1, 1);
    chatSplitter->setSizes({260, 590});
    auto *logSplitter = new QSplitter(Qt::Vertical, this);
    logSplitter->addWidget(chatSplitter);
    logSplitter->addWidget(log_);
    logSplitter->setStretchFactor(0, 1);
    logSplitter->setSizes({450, 150});
    setCentralWidget(logSplitter);
    // Selecting a chat shows a read-only preview of its latest messages; double-clicking continues it.
    connect(conversationTree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item) showChatPreview(item);
    });
    connect(conversationTree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        attachChat(item);
    });
    connect(loadEarlierButton_, &QPushButton::clicked, this, [this] {
        loadHistory(false);
    });
    connect(conversationTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toString() != "provider") return;
        // Rebuilding the tree deletes item, so read its provider first.
        const QString provider = item->text(0);
        expandedProviders_.insert(provider);
        refreshConversationTree();
        if (AgentBackend *expanded = agent(providerIndex(provider))) expanded->refreshConversations();
    });
    connect(conversationTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toString() != "provider") return;
        expandedProviders_.remove(item->text(0));
        refreshConversationTree();
    });

    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::submitCommand);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::submitCommand);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::requestStop);
    connect(newChatButton_, &QPushButton::clicked, this, &MainWindow::showNewConversationDialog);
    for (AgentBackend *agent : agents_) connectAgent(agent);

    appendLine("Directory: " + workingDirectory_);
    appendLine("Type help to see the available commands.\n");
    for (AgentBackend *agent : agents_) agent->loadConversations();
    loadRecentDirectories();
    refreshConversationTree();
    showLiveChat(codex_->name(), workingDirectory_);
    if (!QDir(workingDirectory_).exists()) {
        appendLine("[Working directory does not exist: " + workingDirectory_ + "]");
        return;
    }
    codexConnection_->start();
    input_->setFocus();
}

MainWindow::~MainWindow()
{
    for (AgentBackend *agent : agents_) disconnect(agent, nullptr, this, nullptr);
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
        log_->clear();
    } else if (local == "new" || local == "/new") {
        showNewConversationDialog();
    } else if (local == "stop" || local == "/stop") {
        requestStop();
    } else if (local == "quit" || local == "/quit" || local == "exit") {
        close();
    } else {
        if (!viewLive_) {
            appendLine("[The displayed chat is a read-only preview. Double-click it in the tree to continue it.]");
            return;
        }
        AgentBackend *current = currentAgent();
        const QString name = current->name();
        if (current->prompt(command)) appendChatText(name, "\nYou: " + command + '\n');
    }
}

void MainWindow::showHelp()
{
    appendLine("Commands:");
    appendLine("  help       show this help and installed agent options");
    appendLine("  new        start a new conversation");
    appendLine("  clear      clear the log pane");
    appendLine("  stop       interrupt the current response");
    appendLine("  quit       close the application");
    appendLine("All other text is sent to the selected agent as a message.\n");
    const AgentHelp help = currentAgent()->help();
    for (const QString &line : help.lines) appendLine(line);
    if (help.program.isEmpty()) {
        appendLine("[" + help.name + " is not installed or not in PATH.]\n");
        return;
    }

    const QString helpName = help.name;
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
    helpProcess->start(help.program, help.arguments);
}

QString MainWindow::providerName(int index) const
{
    const AgentBackend *indexed = agent(index);
    return indexed ? indexed->name() : QString();
}


int MainWindow::providerIndex(const QString &name) const
{
    for (int i = 0; i < agents_.size(); ++i) {
        if (agents_.at(i)->name() == name) return i;
    }
    return -1;
}


void MainWindow::selectProvider(int index)
{
    if (index == currentProvider_) return;
    currentProvider_ = index;
    appendLine("[" + providerName(index) + " selected]");
    updateStatus();
}

void MainWindow::refreshConversationTree()
{
    const QSignalBlocker blocker(conversationTree_);
    conversationTree_->clear();
    for (AgentBackend *listed : agents_) {
        const QString provider = listed->name();
        auto *root = new QTreeWidgetItem(conversationTree_, {provider});
        root->setData(0, Qt::UserRole, "provider");
        root->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if (!expandedProviders_.contains(provider)) continue;
        root->setExpanded(true);
        QList<QJsonObject> chats = listed->conversations();
        std::stable_sort(chats.begin(), chats.end(), [](const QJsonObject &left, const QJsonObject &right) {
            return left.value("createdAt").toInteger() > right.value("createdAt").toInteger();
        });
        QHash<QString, QTreeWidgetItem *> directories;
        for (const QJsonObject &chat : chats) {
            const QString path = chat.value("cwd").toString();
            QTreeWidgetItem *directory = directories.value(path);
            if (!directory) {
                directory = new QTreeWidgetItem(root, {path.isEmpty() ? "(unknown directory)" : QDir::toNativeSeparators(path)});
                directory->setToolTip(0, path);
                directory->setExpanded(true);
                directories.insert(path, directory);
            }
            QString title = shortPreview(chat.value("title").toString());
            if (chat.value("archived").toBool()) title += " (archived)";
            auto *item = new QTreeWidgetItem(directory, {title});
            item->setData(0, Qt::UserRole, provider);
            item->setData(0, Qt::UserRole + 1, chat.value("id").toString());
            item->setData(0, Qt::UserRole + 2, path);
            item->setToolTip(0, chat.value("tooltip").toString());
        }
    }
}

// Recently used working directories are shown in the directory chooser of the New chat dialog.
void MainWindow::loadRecentDirectories()
{
    QFile file(QDir(dataDirectory_).filePath("recent-directories.json"));
    if (!file.open(QIODevice::ReadOnly)) return;
    recentDirectories_.clear();
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value("directories").toArray()) {
        if (!value.toString().isEmpty()) recentDirectories_.append(value.toString());
    }
}

void MainWindow::saveRecentDirectories()
{
    if (!QDir().mkpath(dataDirectory_)) return;
    QSaveFile file(QDir(dataDirectory_).filePath("recent-directories.json"));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(QJsonObject{{"version", 1}, {"directories", QJsonArray::fromStringList(recentDirectories_)}})
                          .toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        appendLine("[Could not save recent directories: " + file.errorString() + "]");
    }
}

void MainWindow::rememberRecentDirectory(const QString &path)
{
    constexpr int limit = 15;
    recentDirectories_.removeAll(path);
    recentDirectories_.prepend(path);
    while (recentDirectories_.size() > limit) recentDirectories_.removeLast();
    saveRecentDirectories();
}

void MainWindow::newProviderConversation(int providerIndex, const QString &path)
{
    const QFileInfo directory(path);
    if (!directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    const QString canonicalPath = directory.canonicalFilePath();
    AgentBackend *selected = agent(providerIndex);
    if (!selected->newConversation(canonicalPath)) return;
    selectProvider(providerIndex);
    showLiveChat(selected->name(), canonicalPath);
    appendLine("[Starting a new " + selected->name() + " conversation in " + canonicalPath + "]");
    refreshConversationTree();
    updateStatus();
}

void MainWindow::showNewConversationDialog()
{
    const QString currentPath = currentAgent()->workingDirectory();
    QDialog dialog(this);
    dialog.setWindowTitle("New conversation");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Agent:", &dialog));
    auto *providerInput = new QComboBox(&dialog);
    providerInput->setObjectName("newConversationProvider");
    for (const AgentBackend *listed : agents_) providerInput->addItem(listed->name());
    providerInput->setCurrentIndex(currentProvider_);
    layout->addWidget(providerInput);
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
        const QString path = QxFileDialog::getExistingDirectory(this, "Choose working directory", pathInput->text(),
                                                                recentDirectories_);
        if (!path.isEmpty()) pathInput->setText(path);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validatePath(currentPath);
    dialog.resize(560, dialog.sizeHint().height());
    if (dialog.exec() == QDialog::Accepted) {
        const QString path = QDir::cleanPath(pathInput->text().trimmed());
        rememberRecentDirectory(path);
        newProviderConversation(providerInput->currentIndex(), path);
    }
}

void MainWindow::requestStop()
{
    AgentBackend *current = currentAgent();
    if (!current->isResponding()) {
        appendLine("[No active " + current->name() + " response.]");
        return;
    }
    if (!current->canInterrupt()) return;
    appendLine("[Interrupting " + current->name() + " response]");
    current->interrupt();
}

void MainWindow::connectAgent(AgentBackend *agent)
{
    const QString name = agent->name();
    connect(agent, &AgentBackend::message, this, &MainWindow::appendLine);
    connect(agent, &AgentBackend::stateChanged, this, &MainWindow::updateStatus);
    connect(agent, &AgentBackend::conversationsChanged, this, &MainWindow::refreshConversationTree);
    connect(agent, &AgentBackend::messageStarted, this, [this, name] { appendChatText(name, "\n" + name + ": "); });
    connect(agent, &AgentBackend::messageDelta, this, [this, name](const QString &text) { appendChatText(name, text); });
    connect(agent, &AgentBackend::messageFinished, this, [this, name] { appendChatText(name, "\n"); });
    connect(agent, &AgentBackend::toolStarted, this, [this, name](const QString &tool, const QString &details) {
        appendChatText(name, "\n[" + name + " tool: " + tool + "] " + details + '\n');
    });
    connect(agent, &AgentBackend::toolOutput, this, [this, name](const QString &text) { appendChatText(name, text); });
    connect(agent, &AgentBackend::toolFinished, this, [this, name](const QString &tool, const QString &status) {
        appendChatText(name, "\n[" + tool + ": " + status + "]\n");
    });
    connect(agent, &AgentBackend::turnCompleted, this, [this, name](const QString &status, const QString &details) {
        if (status != "completed")
            appendLine("[" + name + " response: " + status + (details.isEmpty() ? "" : ": " + details) + "]");
    });
    connect(agent, &AgentBackend::approvalRequested, this,
            [this, agent](int id, const QString &title, const QString &description) {
        const auto answer = QMessageBox::question(this, title, description + "\n\nAllow this action?",
                                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        agent->answerApproval(id, answer == QMessageBox::Yes);
    });
    connect(agent, &AgentBackend::questionsRequested, this, [this, agent](int id, const QList<AgentQuestion> &questions) {
        agent->answerQuestions(id, askQuestions(questions));
    });
    // A resumed conversation becomes live only if the user is still looking at the chat they attached.
    connect(agent, &AgentBackend::conversationOpened, this, [this, name](const QString &id, bool resumed) {
        if (resumed && !pendingAttachId_.isEmpty()) {
            if (id == pendingAttachId_ && viewProvider_ == name && viewId_ == id) {
                viewLive_ = true;
                lockNotice_.clear();
                updateChatHeader();
            }
            pendingAttachId_.clear();
        } else if (!resumed && viewLive_ && viewProvider_ == name) {
            viewId_ = id;
        }
    });
    connect(agent, &AgentBackend::conversationOpenFailed, this, [this, name](const QString &id, const QString &reason) {
        if (id.isEmpty() || id != pendingAttachId_) return;
        if (viewProvider_ == name && viewId_ == id) {
            lockNotice_ = reason;
            updateChatHeader();
        }
        pendingAttachId_.clear();
    });
    connect(agent, &AgentBackend::historyLoaded, this,
            [this, name](const QString &id, const QList<ChatEntry> &entries, bool hasMore, const QString &notice) {
        if (viewProvider_ != name || viewId_ != id) return;
        showHistory(entries, hasMore, notice);
    });
}

// Asks each question in a dialog and stops at the first one the user cancels.
QHash<QString, QString> MainWindow::askQuestions(const QList<AgentQuestion> &questions)
{
    QHash<QString, QString> answers;
    for (const AgentQuestion &question : questions) {
        bool accepted = false;
        const QString reply = question.multiSelect || question.options.isEmpty()
            ? QInputDialog::getText(this, question.header, question.text, QLineEdit::Normal, {}, &accepted)
            : QInputDialog::getItem(this, question.header, question.text, question.options, 0, false, &accepted);
        if (!accepted) break;
        answers.insert(question.id, reply);
    }
    return answers;
}

void MainWindow::appendText(const QString &text)
{
    QTextCursor cursor = log_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    log_->setTextCursor(cursor);
    log_->ensureCursorVisible();
}

void MainWindow::appendLine(const QString &text)
{
    appendText(text + '\n');
}

void MainWindow::appendChatText(const QString &provider, const QString &text)
{
    // Live output is shown only while its conversation is displayed; saved history covers the rest.
    if (!viewLive_ || viewProvider_ != provider) return;
    liveTranscript_ += text;
    QTextCursor cursor = chatView_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    chatView_->setTextCursor(cursor);
    chatView_->ensureCursorVisible();
}

void MainWindow::showLiveChat(const QString &provider, const QString &path)
{
    for (AgentBackend *agent : agents_) agent->cancelHistory();
    viewProvider_ = provider;
    viewId_.clear();
    viewPath_ = path;
    viewTitle_ = "New chat";
    viewLive_ = true;
    lockNotice_.clear();
    liveTranscript_.clear();
    chatView_->clear();
    loadEarlierButton_->setVisible(false);
    updateChatHeader();
    updateStatus();
}

void MainWindow::showChatPreview(QTreeWidgetItem *item)
{
    const QString provider = item->data(0, Qt::UserRole).toString();
    if (provider.isEmpty() || provider == "provider") return;
    const QString id = item->data(0, Qt::UserRole + 1).toString();
    if (viewProvider_ == provider && viewId_ == id) return;
    openChat(provider, id, item->data(0, Qt::UserRole + 2).toString(), item->text(0));
}

void MainWindow::attachChat(QTreeWidgetItem *item)
{
    const QString provider = item->data(0, Qt::UserRole).toString();
    if (provider.isEmpty() || provider == "provider") return;
    const QString id = item->data(0, Qt::UserRole + 1).toString();
    const QString path = item->data(0, Qt::UserRole + 2).toString();
    const int index = providerIndex(provider);
    if (id.isEmpty() || index < 0 || (viewLive_ && viewProvider_ == provider && viewId_ == id)) return;
    if (viewProvider_ != provider || viewId_ != id) openChat(provider, id, path, item->text(0));
    AgentBackend *selected = agent(index);
    if (selected->sessionId() != id) {
        const QString lock = selected->externalLock(id);
        if (!lock.isEmpty()) {
            lockNotice_ = "open in " + lock;
            updateChatHeader();
            appendLine("[" + provider + " conversation is open in another tool and stays read-only: " + lock + "]");
            return;
        }
        lockNotice_.clear();
        updateChatHeader();
        pendingAttachId_ = id;
        if (!selected->resumeConversation(id, path)) {
            pendingAttachId_.clear();
            return;
        }
        selectProvider(index);
        // Codex confirms asynchronously through conversationOpened; the other agents switch at once.
        if (selected->sessionId() != id) return;
        pendingAttachId_.clear();
    }
    selectProvider(index);
    viewLive_ = true;
    updateChatHeader();
    updateStatus();
}

void MainWindow::openChat(const QString &provider, const QString &id, const QString &path, const QString &title)
{
    viewProvider_ = provider;
    viewId_ = id;
    viewPath_ = path;
    viewTitle_ = title;
    viewLive_ = false;
    lockNotice_.clear();
    liveTranscript_.clear();
    chatView_->setPlainText("Loading the latest messages…");
    loadEarlierButton_->setVisible(false);
    updateChatHeader();
    updateStatus();
    loadHistory(true);
}

void MainWindow::loadHistory(bool reset)
{
    for (AgentBackend *agent : agents_) agent->cancelHistory();
    if (AgentBackend *viewed = agent(providerIndex(viewProvider_))) viewed->loadHistory(viewId_, viewPath_, !reset);
}

void MainWindow::showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice)
{
    QStringList blocks;
    for (const ChatEntry &entry : entries) {
        if (entry.role == "user") blocks.append("You: " + entry.text);
        else if (entry.role == "tool") blocks.append("[" + viewProvider_ + " tool: " + entry.text + "]");
        else blocks.append(viewProvider_ + ": " + entry.text);
    }
    if (!notice.isEmpty()) blocks.append("[" + notice + "]");
    else if (blocks.isEmpty()) blocks.append("[This conversation has no messages to show.]");
    chatView_->setPlainText(blocks.join("\n\n") + '\n' + (viewLive_ ? liveTranscript_ : QString()));
    chatView_->moveCursor(QTextCursor::End);
    chatView_->ensureCursorVisible();
    loadEarlierButton_->setVisible(hasMore);
}

void MainWindow::updateChatHeader()
{
    QStringList parts{viewProvider_, viewTitle_, QDir::toNativeSeparators(viewPath_)};
    if (!lockNotice_.isEmpty()) parts.append("locked: " + lockNotice_);
    else if (!viewLive_) parts.append("read-only preview");
    chatHeader_->setText(parts.join("  •  "));
    chatHeader_->setToolTip(viewId_);
}

void MainWindow::updateStatus()
{
    AgentBackend *current = currentAgent();
    status_->setText(current->statusText() + "  •  " + QDir::toNativeSeparators(current->workingDirectory()));
    stopButton_->setEnabled(current->canInterrupt());
    sendButton_->setText("Send to " + current->name());
    sendButton_->setEnabled(viewLive_);
    // Reloading a longer tail while a live response streams would drop the partial answer.
    loadEarlierButton_->setEnabled(!(viewLive_ && current->isResponding()));
    input_->setPlaceholderText(viewLive_ ? "Enter a message or help, then press Enter"
                                         : "Read-only preview. Type help, new or clear, then press Enter");
    newChatButton_->setEnabled(std::none_of(agents_.begin(), agents_.end(),
                                            [](AgentBackend *agent) { return agent->isResponding(); }));
}
