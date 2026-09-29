#include "ClaudeBridge.h"
#include "GeminiBridge.h"
#include "AntigravityBridge.h"
#include "MainWindow.h"

#include <QComboBox>
#include <QCoreApplication>
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
constexpr int kHistoryPageSize = 20;

QString shortPreview(const QString &value)
{
    const QString singleLine = value.simplified();
    constexpr int limit = 72;
    return singleLine.size() > limit ? singleLine.left(limit - 1) + QChar(0x2026) : singleLine;
}

QString geminiMessageText(const QJsonValue &content)
{
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const QJsonValue &block : content.toArray()) {
        const QString text = block.toObject().value("text").toString();
        if (!text.isEmpty()) parts.append(text);
    }
    return parts.join(' ');
}

QJsonObject readGeminiSession(const QFileInfo &fileInfo, const QString &projectPath)
{
    QFile file(fileInfo.filePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonObject metadata;
    QString firstPrompt;
    if (fileInfo.suffix() == "jsonl") {
        metadata = QJsonDocument::fromJson(file.readLine(256 * 1024)).object();
        for (int line = 0; line < 128 && !file.atEnd() && firstPrompt.isEmpty(); ++line) {
            const QJsonObject event = QJsonDocument::fromJson(file.readLine(256 * 1024)).object();
            if (event.value("type").toString() == "user")
                firstPrompt = geminiMessageText(event.value("content"));
        }
    } else {
        metadata = QJsonDocument::fromJson(file.readAll()).object();
        for (const QJsonValue &value : metadata.value("messages").toArray()) {
            const QJsonObject message = value.toObject();
            if (message.value("type").toString() == "user") {
                firstPrompt = geminiMessageText(message.value("content"));
                break;
            }
        }
    }
    const QString id = metadata.value("sessionId").toString();
    if (id.isEmpty() || metadata.value("kind").toString() == "subagent") return {};
    const qint64 createdAt = QDateTime::fromString(metadata.value("startTime").toString(), Qt::ISODate).toSecsSinceEpoch();
    const qint64 modifiedAt = QDateTime::fromString(metadata.value("lastUpdated").toString(), Qt::ISODate).toSecsSinceEpoch();
    return {{"provider", "Gemini"}, {"id", id}, {"cwd", projectPath},
            {"title", firstPrompt.isEmpty() ? id : firstPrompt.simplified().left(200)},
            {"createdAt", createdAt}, {"lastModified", modifiedAt}, {"file", fileInfo.absoluteFilePath()}};
}

// Replays a Gemini CLI session file, where JSONL records either append/replace a message by id or
// reset the message list with "$set", and converts the result into chat entries.
QList<ChatEntry> readGeminiHistory(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QList<QJsonObject> messages;
    QHash<QString, qsizetype> positions;
    const auto upsert = [&messages, &positions](const QJsonObject &message) {
        const QString id = message.value("id").toString();
        const auto found = positions.constFind(id);
        if (!id.isEmpty() && found != positions.constEnd()) {
            messages[*found] = message;
            return;
        }
        if (!id.isEmpty()) positions.insert(id, messages.size());
        messages.append(message);
    };
    if (QFileInfo(filePath).suffix() == "jsonl") {
        while (!file.atEnd()) {
            const QJsonObject record = QJsonDocument::fromJson(file.readLine()).object();
            const QJsonObject update = record.value("$set").toObject();
            if (update.contains("messages")) {
                messages.clear();
                positions.clear();
                for (const QJsonValue &value : update.value("messages").toArray()) upsert(value.toObject());
            } else if (record.contains("type") && record.contains("id")) {
                upsert(record);
            }
        }
    } else {
        for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value("messages").toArray())
            upsert(value.toObject());
    }
    QList<ChatEntry> entries;
    for (const QJsonObject &message : messages) {
        const QString type = message.value("type").toString();
        const QString text = geminiMessageText(message.value("content")).trimmed();
        if (type == "user") {
            if (!text.isEmpty() && !text.startsWith("<session_context>")) entries.append({"user", text});
        } else if (type == "gemini") {
            if (!text.isEmpty()) entries.append({"assistant", text});
            for (const QJsonValue &call : message.value("toolCalls").toArray())
                entries.append({"tool", call.toObject().value("name").toString("tool")});
        }
    }
    return entries;
}

// Converts one Codex App Server thread item into chat entries.
QList<ChatEntry> codexHistoryEntries(const QJsonObject &item)
{
    const QString type = item.value("type").toString();
    if (type == "userMessage") {
        QStringList parts;
        for (const QJsonValue &input : item.value("content").toArray()) {
            const QString text = input.toObject().value("text").toString();
            if (!text.isEmpty()) parts.append(text);
        }
        if (!parts.isEmpty()) return {{"user", parts.join('\n')}};
    } else if (type == "agentMessage") {
        const QString text = item.value("text").toString().trimmed();
        if (!text.isEmpty()) return {{"assistant", text}};
    } else if (type == "commandExecution") {
        return {{"tool", "$ " + item.value("command").toString()}};
    } else if (type == "fileChange") {
        return {{"tool", "file changes"}};
    }
    return {};
}

// Returns the /proc/<pid>/stat fields that follow the command name (state, ppid, ...).
QStringList processStatFields(qint64 pid)
{
    QFile file(QString("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QString stat = QString::fromUtf8(file.readAll());
    const qsizetype end = stat.lastIndexOf(')');
    return end < 0 ? QStringList{} : stat.mid(end + 2).split(' ', Qt::SkipEmptyParts);
}

bool isOwnDescendant(qint64 pid)
{
    const qint64 self = QCoreApplication::applicationPid();
    for (int depth = 0; depth < 64 && pid > 1; ++depth) {
        if (pid == self) return true;
        const QStringList fields = processStatFields(pid);
        if (fields.size() < 2) return false;
        pid = fields.at(1).toLongLong();
    }
    return false;
}

// Claude Code registers each running session in <config>/sessions/<pid>.json.
QString claudeSessionLock(const QString &configDirectory, const QString &sessionId)
{
    const QDir sessions(QDir(configDirectory).filePath("sessions"));
    for (const QFileInfo &info : sessions.entryInfoList({"*.json"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject entry = QJsonDocument::fromJson(file.readAll()).object();
        if (entry.value("sessionId").toString() != sessionId) continue;
        const qint64 pid = entry.value("pid").toInteger();
        if (pid <= 0 || !QFileInfo::exists(QString("/proc/%1").arg(pid)) || isOwnDescendant(pid)) continue;
        const QString start = entry.value("procStart").toString();
        const QStringList fields = processStatFields(pid);
        if (!start.isEmpty() && (fields.size() <= 19 || fields.at(19) != start)) continue;
        const QString kind = entry.value("kind").toString("session");
        return QString("Claude Code %1 (PID %2)").arg(kind).arg(pid);
    }
    return {};
}

// Heuristic for CLIs without a session registry: another process that names the session on its command line.
QString commandLineLock(const QString &sessionId)
{
    const QDir processes("/proc");
    for (const QString &name : processes.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 pid = name.toLongLong(&numeric);
        if (!numeric || isOwnDescendant(pid)) continue;
        QFile file(processes.filePath(name + "/cmdline"));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QStringList arguments = QString::fromUtf8(file.readAll()).split(QChar('\0'), Qt::SkipEmptyParts);
        for (const QString &argument : arguments) {
            if (!argument.contains(sessionId)) continue;
            return QString("PID %1: %2").arg(pid).arg(arguments.mid(0, 3).join(' '));
        }
    }
    return {};
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
                       const QString &geminiProgram, QWidget *parent, const QString &codexIndexPath,
                       const QString &antigravityProgram, const QString &geminiDataDirectory)
    : QMainWindow(parent), codexProgram_(codexProgram), workingDirectory_(workingDirectory),
      codexIndexPath_(codexIndexPath.isEmpty()
          ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("codex-conversations.json")
          : codexIndexPath),
      geminiDataDirectory_(geminiDataDirectory.isEmpty()
          ? QDir(QDir::homePath()).filePath(".gemini") : geminiDataDirectory),
      claudePython_(claudePython), claudeScript_(claudeScript),
      server_(new QProcess(this)),
      claude_(new ClaudeBridge(claudePython, claudeScript, workingDirectory, "claude", this)),
      glm_(new ClaudeBridge(claudePython, claudeScript, workingDirectory, "glm", this)),
      gemini_(new GeminiBridge(geminiProgram, workingDirectory, this)),
      antigravity_(new AntigravityBridge(antigravityProgram, workingDirectory, this))
{
    codexWorkingDirectory_ = workingDirectory_;
    claudeWorkingDirectory_ = workingDirectory_;
    glmWorkingDirectory_ = workingDirectory_;
    geminiWorkingDirectory_ = workingDirectory_;
    antigravityWorkingDirectory_ = workingDirectory_;
    localIndexPath_ = QDir(QFileInfo(codexIndexPath_).absolutePath()).filePath("claude-conversations.json");
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
        historyLimit_ += kHistoryPageSize;
        loadHistory(false);
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
        else if (provider == "Antigravity") loadLocalConversations();
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
            appendChatText("Claude", "\nClaude: ");
            claudeTextStarted_ = true;
        }
        appendChatText("Claude", text);
    });
    connect(claude_, &ClaudeBridge::toolStarted, this, [this](const QString &name, const QJsonObject &input) {
        appendChatText("Claude", "\n[Claude tool: " + name + "] "
                   + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)) + '\n');
    });
    connect(claude_, &ClaudeBridge::completed, this, [this](const QString &state, const QString &details) {
        if (claudeTextStarted_) appendChatText("Claude", "\n");
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
    connect(claude_, &ClaudeBridge::error, this, [this](const QString &message) {
        appendLine("[Claude] " + message);
    });
    connect(claude_, &ClaudeBridge::disconnected, this, [this] {
        claudeReady_ = false;
        claudeBusy_ = false;
        claudeStopRequested_ = false;
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
            appendChatText("GLM", "\nGLM: ");
            glmTextStarted_ = true;
        }
        appendChatText("GLM", text);
    });
    connect(glm_, &ClaudeBridge::toolStarted, this, [this](const QString &name, const QJsonObject &input) {
        appendChatText("GLM", "\n[GLM tool: " + name + "] "
                   + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)) + '\n');
    });
    connect(glm_, &ClaudeBridge::completed, this, [this](const QString &state, const QString &details) {
        if (glmTextStarted_) appendChatText("GLM", "\n");
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
    connect(glm_, &ClaudeBridge::error, this, [this](const QString &message) { appendLine("[GLM] " + message); });
    connect(glm_, &ClaudeBridge::disconnected, this, [this] {
        glmReady_ = false;
        glmBusy_ = false;
        glmStopRequested_ = false;
        updateStatus();
    });
    connect(gemini_, &GeminiBridge::textDelta, this, [this](const QString &text) {
        if (!geminiTextStarted_) {
            appendChatText("Gemini", "\nGemini: ");
            geminiTextStarted_ = true;
        }
        appendChatText("Gemini", text);
    });
    connect(gemini_, &GeminiBridge::toolStarted, this, [this](const QString &name, const QJsonObject &input) {
        appendChatText("Gemini", "\n[Gemini tool: " + name + "] "
                   + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)) + '\n');
    });
    connect(gemini_, &GeminiBridge::error, this, [this](const QString &message) {
        appendLine("[Gemini] " + message);
    });
    connect(gemini_, &GeminiBridge::completed, this, [this](const QString &state, const QString &details) {
        if (geminiTextStarted_) appendChatText("Gemini", "\n");
        if (state != "completed") appendLine("[Gemini response: " + state + (details.isEmpty() ? "" : ": " + details) + "]");
        geminiBusy_ = false;
        geminiStopRequested_ = false;
        geminiTextStarted_ = false;
        updateStatus();
        sendNextGeminiPrompt();
    });
    connect(claude_, &ClaudeBridge::sessionChanged, this, [this](const QString &id) {
        claudeSessionId_ = id;
        if (viewLive_ && viewProvider_ == "Claude") viewId_ = id;
        rememberLocalConversation("Claude", id);
    });
    connect(glm_, &ClaudeBridge::sessionChanged, this, [this](const QString &id) {
        glmSessionId_ = id;
        if (viewLive_ && viewProvider_ == "GLM") viewId_ = id;
        rememberLocalConversation("GLM", id);
    });
    connect(gemini_, &GeminiBridge::sessionChanged, this, [this](const QString &id) {
        if (viewLive_ && viewProvider_ == "Gemini") viewId_ = id;
        rememberLocalConversation("Gemini", id);
    });
    connect(antigravity_, &AntigravityBridge::textDelta, this, [this](const QString &text) {
        if (!antigravityTextStarted_) {
            appendChatText("Antigravity", "\nAntigravity: ");
            antigravityTextStarted_ = true;
        }
        appendChatText("Antigravity", text);
    });
    connect(antigravity_, &AntigravityBridge::toolStarted, this, [this](const QString &name, const QJsonObject &input) {
        appendChatText("Antigravity", "\n[Antigravity tool: " + name + "] "
                   + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)) + '\n');
    });
    connect(antigravity_, &AntigravityBridge::error, this, [this](const QString &message) {
        appendLine("[Antigravity] " + message);
    });
    connect(antigravity_, &AntigravityBridge::completed, this, [this](const QString &state, const QString &details) {
        if (antigravityTextStarted_) appendChatText("Antigravity", "\n");
        if (state != "completed") appendLine("[Antigravity response: " + state + (details.isEmpty() ? "" : ": " + details) + "]");
        antigravityBusy_ = false;
        antigravityStopRequested_ = false;
        antigravityTextStarted_ = false;
        updateStatus();
        sendNextAntigravityPrompt();
    });
    connect(antigravity_, &AntigravityBridge::conversationChanged, this, [this](const QString &id) {
        if (viewLive_ && viewProvider_ == "Antigravity") viewId_ = id;
        rememberLocalConversation("Antigravity", id);
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
    showLiveChat("Codex", workingDirectory_);
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
    disconnect(antigravity_, nullptr, this, nullptr);
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
        if (currentProvider_ == 4) {
            if (antigravity_->conversationId().isEmpty() && antigravityFirstPrompt_.isEmpty()) antigravityFirstPrompt_ = command;
            appendChatText("Antigravity", "\nYou: " + command + '\n');
            antigravityQueuedPrompts_.append(command);
            sendNextAntigravityPrompt();
            return;
        }
        if (currentProvider_ == 3) {
            if (gemini_->sessionId().isEmpty() && geminiFirstPrompt_.isEmpty()) geminiFirstPrompt_ = command;
            appendChatText("Gemini", "\nYou: " + command + '\n');
            geminiQueuedPrompts_.append(command);
            sendNextGeminiPrompt();
            return;
        }
        if (currentProvider_ == 2) {
            if (glmSessionId_.isEmpty() && glmFirstPrompt_.isEmpty()) glmFirstPrompt_ = command;
            appendChatText("GLM", "\nYou: " + command + '\n');
            glmQueuedPrompts_.append(command);
            if (!glmReady_) glm_->start();
            sendNextGlmPrompt();
            return;
        }
        if (currentProvider_ == 1) {
            if (claudeSessionId_.isEmpty() && claudeFirstPrompt_.isEmpty()) claudeFirstPrompt_ = command;
            appendChatText("Claude", "\nYou: " + command + '\n');
            claudeQueuedPrompts_.append(command);
            if (!claudeReady_) claude_->start();
            sendNextClaudePrompt();
            return;
        }
        if (server_->state() == QProcess::NotRunning) {
            appendLine("[Server is not running. Check the codex executable and restart.]");
            return;
        }
        appendChatText("Codex", "\nYou: " + command + '\n');
        queuedPrompts_.append(command);
        if (threadId_.isEmpty()) startThread();
        sendNextPrompt();
    }
}

void MainWindow::showHelp()
{
    const bool claudeSelected = currentProvider_ == 1;
    const bool glmSelected = currentProvider_ == 2;
    const bool geminiSelected = currentProvider_ == 3;
    const bool antigravitySelected = currentProvider_ == 4;
    appendLine("Commands:");
    appendLine("  help       show this help and installed agent options");
    appendLine("  new        start a new conversation");
    appendLine("  clear      clear the log pane");
    appendLine("  stop       interrupt the current response");
    appendLine("  quit       close the application");
    appendLine("All other text is sent to the selected agent as a message.\n");
    QString helpProgram;
    QStringList helpArguments;
    QString helpName;
    if (antigravitySelected) {
        appendLine("Antigravity CLI headless mode:");
        appendLine("  Send messages with Enter or Send. Later messages resume the same conversation ID.");
        appendLine("  Create a chat to choose its working directory, or double-click a saved chat to resume it.");
        appendLine("  Authenticate once in the interactive agy CLI before using this window.");
        appendLine("  CLI documentation: https://antigravity.google/docs/cli/headless/");
        appendLine("Installed Antigravity CLI commands and options:");
        helpProgram = antigravity_->program();
        helpArguments = {"--help"};
        helpName = "Antigravity CLI";
    } else if (geminiSelected) {
        appendLine("Gemini CLI headless mode:");
        appendLine("  Send messages with Enter or Send to Gemini.");
        appendLine("  Each response streams JSON events; later messages resume the same session.");
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

QString MainWindow::providerName(int index)
{
    static const QStringList names{"Codex", "Claude", "GLM", "Gemini", "Antigravity"};
    return names.value(index);
}

QString MainWindow::providerWorkingDirectory(int index) const
{
    return index == 0 ? codexWorkingDirectory_
        : (index == 1 ? claudeWorkingDirectory_ : (index == 2 ? glmWorkingDirectory_
           : (index == 3 ? geminiWorkingDirectory_ : antigravityWorkingDirectory_)));
}

int MainWindow::providerIndex(const QString &name)
{
    for (int i = 0; i < 5; ++i) {
        if (providerName(i) == name) return i;
    }
    return -1;
}

QString MainWindow::liveSessionId(int index) const
{
    return index == 0 ? threadId_ : (index == 1 ? claudeSessionId_ : (index == 2 ? glmSessionId_
        : (index == 3 ? gemini_->sessionId() : antigravity_->conversationId())));
}

QString MainWindow::externalLock(const QString &provider, const QString &id) const
{
    // Codex is not checked here: the App Server itself refuses to resume a thread it cannot open.
    if (provider == "Claude" || provider == "GLM") {
        return claudeSessionLock(qEnvironmentVariable("CLAUDE_CONFIG_DIR", QDir::home().filePath(".claude")), id);
    }
    if (provider == "Gemini" || provider == "Antigravity") return commandLineLock(id);
    return {};
}

void MainWindow::selectProvider(int index)
{
    if (index == currentProvider_) return;
    currentProvider_ = index;
    appendLine("[" + providerName(index) + " selected]");
    updateStatus();
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
    selectProvider(0);
    showLiveChat("Codex", codexWorkingDirectory_);
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
    selectProvider(0);
    appendLine("[Resuming Codex conversation: " + id + "]");
    sendRequest("thread/resume", {{"threadId", id}});
    updateStatus();
}

void MainWindow::refreshConversationTree()
{
    const QSignalBlocker blocker(conversationTree_);
    conversationTree_->clear();
    const QStringList providers{"Codex", "Claude", "Gemini", "GLM", "Antigravity"};
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
        QStringList details{title, "ID: " + id, "Directory: " + path};
        if (provider == "Claude") {
            const auto addText = [&thread, &details](const QString &key, const QString &label) {
                const QString value = thread.value(key).toString();
                if (!value.isEmpty()) details.append(label + ": " + value);
            };
            const auto addDate = [&thread, &details](const QString &key, const QString &label) {
                const qint64 seconds = thread.value(key).toInteger();
                if (seconds > 0) {
                    details.append(label + ": " + QDateTime::fromSecsSinceEpoch(seconds).toString(Qt::ISODate));
                }
            };
            addDate("createdAt", "Created");
            addDate("lastModified", "Modified");
            if (thread.value("fileSize").isDouble()) {
                details.append("Transcript size: " + QString::number(thread.value("fileSize").toInteger()) + " bytes");
            }
            addText("gitBranch", "Git branch");
            addText("tag", "Tag");
            addText("firstPrompt", "First prompt");
        }
        chat->setToolTip(0, details.join('\n'));
    }
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
                QJsonObject entry = session;
                entry.insert("provider", "Claude");
                if (localConversations_.value(key) == entry) continue;
                localConversations_.insert(key, entry);
                changed = true;
            }
            if (changed) {
                saveLocalConversations();
                refreshConversationTree();
            }
            appendLine(QString("[Claude sessions discovered: %1]")
                           .arg(document.object().value("sessions").toArray().size()));
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
    if (!geminiExecutableChecked_) {
        geminiExecutableChecked_ = true;
        if (QStandardPaths::findExecutable(gemini_->program()).isEmpty()) {
            appendLine("[Gemini CLI is not installed or is not in PATH. Install it or use --gemini /absolute/path/to/gemini.]");
        }
    }
    QHash<QString, QString> projectPaths;
    QFile projects(QDir(geminiDataDirectory_).filePath("projects.json"));
    if (projects.open(QIODevice::ReadOnly)) {
        const QJsonObject entries = QJsonDocument::fromJson(projects.readAll()).object()
                                        .value("projects").toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (QFileInfo(it.key()).isAbsolute() && !it.value().toString().isEmpty())
                projectPaths.insert(it.value().toString(), it.key());
        }
    }
    QSet<QString> discoveredIds;
    bool changed = false;
    const QDir profileRoot(QDir(geminiDataDirectory_).filePath("tmp"));
    for (const QFileInfo &profile : profileRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString path = projectPaths.value(profile.fileName());
        if (path.isEmpty()) {
            QFile root(QDir(profile.filePath()).filePath(".project_root"));
            if (root.open(QIODevice::ReadOnly)) path = QString::fromUtf8(root.readAll()).trimmed();
        }
        if (!QFileInfo(path).isAbsolute()) continue;
        const QDir chats(QDir(profile.filePath()).filePath("chats"));
        const QFileInfoList files = chats.entryInfoList({"session-*.jsonl", "session-*.json"}, QDir::Files,
                                                        QDir::Name);
        for (const QFileInfo &file : files) {
            QJsonObject entry = readGeminiSession(file, path);
            const QString id = entry.value("id").toString();
            if (id.isEmpty()) continue;
            discoveredIds.insert(id);
            const QString key = "Gemini\n" + id;
            const QJsonObject old = localConversations_.value(key);
            if (entry.value("title").toString() == id && !old.value("title").toString().isEmpty())
                entry.insert("title", old.value("title"));
            if (entry.value("createdAt").toInteger() <= 0 && old.value("createdAt").toInteger() > 0)
                entry.insert("createdAt", old.value("createdAt"));
            if (old == entry) continue;
            localConversations_.insert(key, entry);
            changed = true;
        }
    }
    if (changed) saveLocalConversations();
    refreshConversationTree();
    appendLine(QString("[Gemini sessions discovered: %1]").arg(discoveredIds.size()));
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
    const QStringList files{"claude-conversations.json", "glm-conversations.json", "gemini-conversations.json",
                            "antigravity-conversations.json"};
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
            if ((provider == "Claude" || provider == "Gemini" || provider == "GLM"
                 || provider == "Antigravity") && !id.isEmpty()) {
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
    const QStringList providers{"Claude", "GLM", "Gemini", "Antigravity"};
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
            : (provider == "GLM" ? glmWorkingDirectory_
               : (provider == "Gemini" ? geminiWorkingDirectory_ : antigravityWorkingDirectory_));
        const QString prompt = provider == "Claude" ? claudeFirstPrompt_
            : (provider == "GLM" ? glmFirstPrompt_
               : (provider == "Gemini" ? geminiFirstPrompt_ : antigravityFirstPrompt_));
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
    } else if (providerIndex == 3) {
        if (geminiBusy_) { appendLine("[Wait for Gemini to finish.]"); return; }
        geminiWorkingDirectory_ = canonicalPath;
        geminiFirstPrompt_.clear();
        geminiQueuedPrompts_.clear();
        gemini_->resetConversation(canonicalPath);
    } else {
        if (antigravityBusy_) { appendLine("[Wait for Antigravity to finish.]"); return; }
        antigravityWorkingDirectory_ = canonicalPath;
        antigravityFirstPrompt_.clear();
        antigravityQueuedPrompts_.clear();
        antigravity_->resetConversation(canonicalPath);
    }
    selectProvider(providerIndex);
    showLiveChat(providerName(providerIndex), canonicalPath);
    appendLine("[Starting a new " + providerName(providerIndex) + " conversation in " + canonicalPath + "]");
    refreshConversationTree();
    updateStatus();
}

void MainWindow::showNewConversationDialog()
{
    const QString currentPath = providerWorkingDirectory(currentProvider_);
    QDialog dialog(this);
    dialog.setWindowTitle("New conversation");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Agent:", &dialog));
    auto *providerInput = new QComboBox(&dialog);
    providerInput->setObjectName("newConversationProvider");
    for (int i = 0; i < 5; ++i) providerInput->addItem(providerName(i));
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
        const QString path = QFileDialog::getExistingDirectory(this, "Choose working directory", pathInput->text());
        if (!path.isEmpty()) pathInput->setText(path);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validatePath(currentPath);
    dialog.resize(560, dialog.sizeHint().height());
    if (dialog.exec() == QDialog::Accepted) {
        newProviderConversation(providerInput->currentIndex(), pathInput->text().trimmed());
    }
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
        selectProvider(1);
    } else if (provider == "GLM") {
        if (glmBusy_) { appendLine("[Wait for GLM to finish.]"); return; }
        glmWorkingDirectory_ = path;
        glmSessionId_ = id;
        glmFirstPrompt_.clear();
        glmQueuedPrompts_.clear();
        glmReady_ = false;
        if (glm_->isRunning()) glm_->resumeConversation(id, path);
        else { pendingGlmResumeId_ = id; glm_->start(path); }
        selectProvider(2);
    } else if (provider == "Gemini") {
        if (geminiBusy_) { appendLine("[Wait for Gemini to finish.]"); return; }
        geminiWorkingDirectory_ = path;
        geminiFirstPrompt_.clear();
        geminiQueuedPrompts_.clear();
        gemini_->resumeConversation(id, path);
        selectProvider(3);
    } else if (provider == "Antigravity") {
        if (antigravityBusy_) { appendLine("[Wait for Antigravity to finish.]"); return; }
        antigravityWorkingDirectory_ = path;
        antigravityFirstPrompt_.clear();
        antigravityQueuedPrompts_.clear();
        antigravity_->resumeConversation(id, path);
        selectProvider(4);
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

void MainWindow::sendNextAntigravityPrompt()
{
    if (antigravityBusy_ || antigravityQueuedPrompts_.isEmpty()) return;
    antigravityBusy_ = true;
    antigravityStopRequested_ = false;
    antigravityTextStarted_ = false;
    antigravity_->prompt(antigravityQueuedPrompts_.takeFirst());
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
    sendRequest("turn/start", params);
    updateStatus();
}

void MainWindow::requestStop()
{
    if (currentProvider_ == 4) {
        if (!antigravityBusy_) appendLine("[No active Antigravity response.]");
        else if (!antigravityStopRequested_) {
            antigravityStopRequested_ = true;
            appendLine("[Interrupting Antigravity response]");
            antigravity_->interrupt();
            updateStatus();
        }
        return;
    }
    if (currentProvider_ == 3) {
        if (!geminiBusy_) appendLine("[No active Gemini response.]");
        else if (!geminiStopRequested_) {
            geminiStopRequested_ = true;
            appendLine("[Interrupting Gemini response]");
            gemini_->interrupt();
            updateStatus();
        }
        return;
    }
    if (currentProvider_ == 2) {
        if (!glmBusy_) appendLine("[No active GLM response.]");
        else if (!glmStopRequested_) {
            glmStopRequested_ = true;
            appendLine("[Interrupting GLM response]");
            glm_->interrupt();
            updateStatus();
        }
        return;
    }
    if (currentProvider_ == 1) {
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
        if (method == "thread/items/list" && id == codexHistoryRequest_) {
            codexHistoryRequest_ = 0;
            showHistory(historyEntries_, false, "Could not load this conversation: " + error.value("message").toString());
        } else if (method == "thread/list") {
            syncingCodexConversations_ = false;
            stagedCodexConversations_.clear();
        } else if (method == "thread/start" || method == "thread/resume") {
            codexThreadOpening_ = false;
            if (method == "thread/resume" && !pendingCodexAttachId_.isEmpty()) {
                if (viewProvider_ == "Codex" && viewId_ == pendingCodexAttachId_) {
                    lockNotice_ = "the Codex App Server refused to open it: " + error.value("message").toString();
                    updateChatHeader();
                }
                pendingCodexAttachId_.clear();
            }
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
        if (pendingCodexHistory_ && !viewLive_ && viewProvider_ == "Codex") loadHistory(true);
    } else if (method == "thread/items/list") {
        if (id != codexHistoryRequest_) return;
        codexHistoryRequest_ = 0;
        // Items arrive newest first; older pages are prepended to what is already shown.
        QList<ChatEntry> page;
        const QJsonArray items = result.value("data").toArray();
        for (qsizetype i = items.size() - 1; i >= 0; --i)
            page.append(codexHistoryEntries(items.at(i).toObject().value("item").toObject()));
        historyEntries_ = page + historyEntries_;
        codexHistoryCursor_ = result.value("nextCursor").toString();
        showHistory(historyEntries_, !codexHistoryCursor_.isEmpty());
    } else if (method == "thread/list") {
        handleCodexConversationPage(result);
    } else if (method == "thread/start" || method == "thread/resume") {
        codexThreadOpening_ = false;
        threadId_ = result.value("thread").toObject().value("id").toString();
        if (threadId_.isEmpty()) appendLine("[Server did not return a conversation ID.]");
        else appendLine(method == "thread/start" ? "[Connected to Codex]" : "[Resumed Codex conversation: " + threadId_ + "]");
        if (method == "thread/resume" && !pendingCodexAttachId_.isEmpty()) {
            if (threadId_ == pendingCodexAttachId_ && viewProvider_ == "Codex" && viewId_ == threadId_) {
                viewLive_ = true;
                lockNotice_.clear();
                updateChatHeader();
            }
            pendingCodexAttachId_.clear();
        } else if (method == "thread/start" && viewLive_ && viewProvider_ == "Codex") {
            viewId_ = threadId_;
        }
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
            appendChatText("Codex", "\nCodex: ");
        }
        appendChatText("Codex", params.value("delta").toString());
    } else if (method == "item/commandExecution/outputDelta") {
        streamedCommands_.insert(itemId);
        appendChatText("Codex", params.value("delta").toString());
    } else if (method == "item/started" && item.value("type") == "commandExecution") {
        appendChatText("Codex", "\n[Command] " + item.value("command").toString() + '\n');
    } else if (method == "item/completed") {
        const QString type = item.value("type").toString();
        const QString completedId = item.value("id").toString();
        if (type == "agentMessage") {
            if (streamedMessages_.remove(completedId)) appendChatText("Codex", "\n");
            else appendChatText("Codex", "\nCodex: " + item.value("text").toString() + '\n');
        } else if (type == "commandExecution") {
            if (!streamedCommands_.remove(completedId)) {
                const QString output = item.value("aggregatedOutput").toString();
                if (!output.isEmpty()) appendChatText("Codex", output);
            }
            appendChatText("Codex", QString("\n[Command: %1]\n").arg(item.value("status").toString()));
        } else if (type == "fileChange") {
            appendChatText("Codex", QString("[File changes: %1]\n").arg(item.value("status").toString()));
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
    ++historyGeneration_;
    codexHistoryRequest_ = 0;
    pendingCodexHistory_ = false;
    viewProvider_ = provider;
    viewId_.clear();
    viewPath_ = path;
    viewTitle_ = "New chat";
    viewLive_ = true;
    lockNotice_.clear();
    liveTranscript_.clear();
    historyEntries_.clear();
    chatView_->clear();
    loadEarlierButton_->setVisible(false);
    updateChatHeader();
    updateStatus();
}

void MainWindow::showChatPreview(QTreeWidgetItem *item)
{
    const QString kind = item->data(0, Qt::UserRole).toString();
    if (kind.isEmpty() || kind == "provider") return;
    const QString provider = kind == "codex-chat" ? "Codex" : kind;
    const QString id = item->data(0, Qt::UserRole + 1).toString();
    if (viewProvider_ == provider && viewId_ == id) return;
    openChat(provider, id, item->data(0, Qt::UserRole + 2).toString(), item->text(0));
}

void MainWindow::attachChat(QTreeWidgetItem *item)
{
    const QString kind = item->data(0, Qt::UserRole).toString();
    if (kind.isEmpty() || kind == "provider") return;
    const QString provider = kind == "codex-chat" ? "Codex" : kind;
    const QString id = item->data(0, Qt::UserRole + 1).toString();
    const QString path = item->data(0, Qt::UserRole + 2).toString();
    const int index = providerIndex(provider);
    if (id.isEmpty() || index < 0 || (viewLive_ && viewProvider_ == provider && viewId_ == id)) return;
    if (viewProvider_ != provider || viewId_ != id) openChat(provider, id, path, item->text(0));
    const bool alreadyLive = liveSessionId(index) == id;
    if (!alreadyLive) {
        const QString lock = externalLock(provider, id);
        if (!lock.isEmpty()) {
            lockNotice_ = "open in " + lock;
            updateChatHeader();
            appendLine("[" + provider + " conversation is open in another tool and stays read-only: " + lock + "]");
            return;
        }
        lockNotice_.clear();
        updateChatHeader();
        if (provider == "Codex") {
            pendingCodexAttachId_ = id;
            resumeCodexConversation(id, path);
            if (!codexThreadOpening_) pendingCodexAttachId_.clear();
            return;
        }
        resumeProviderConversation(provider, id, path);
        if (liveSessionId(index) != id) return;
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
    historyLimit_ = kHistoryPageSize;
    historyTotal_ = 0;
    historyEntries_.clear();
    codexHistoryCursor_.clear();
    chatView_->setPlainText("Loading the latest messages…");
    loadEarlierButton_->setVisible(false);
    updateChatHeader();
    updateStatus();
    loadHistory(true);
}

void MainWindow::loadHistory(bool reset)
{
    const quint64 generation = ++historyGeneration_;
    codexHistoryRequest_ = 0;
    pendingCodexHistory_ = false;
    if (viewProvider_ == "Codex") {
        if (!initialized_) {
            pendingCodexHistory_ = true;
            showHistory(historyEntries_, false, "Waiting for the Codex App Server to load this conversation.");
            return;
        }
        QJsonObject params{{"threadId", viewId_}, {"limit", 2 * kHistoryPageSize}, {"sortDirection", "desc"}};
        if (!reset && !codexHistoryCursor_.isEmpty()) params.insert("cursor", codexHistoryCursor_);
        codexHistoryRequest_ = sendRequest("thread/items/list", params);
    } else if (viewProvider_ == "Claude" || viewProvider_ == "GLM") {
        if (!reset) liveTranscript_.clear();
        if (claudePython_.isEmpty() || claudeScript_.isEmpty()) {
            showHistory({}, false, "The Claude Agent SDK bridge is not configured.");
            return;
        }
        auto *process = new QProcess(this);
        historyProcesses_.append(process);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, process, generation](int exitCode, QProcess::ExitStatus status) {
            historyProcesses_.removeAll(process);
            process->deleteLater();
            if (generation != historyGeneration_) return;
            const QJsonObject result = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
            if (status != QProcess::NormalExit || exitCode != 0 || result.value("type") != "history") {
                QString message = result.value("message").toString();
                if (message.isEmpty()) message = QString::fromUtf8(process->readAllStandardError()).trimmed();
                appendLine("[" + viewProvider_ + " history: " + message + "]");
                showHistory({}, false, "Could not load this conversation: " + message);
                return;
            }
            QList<ChatEntry> entries;
            for (const QJsonValue &value : result.value("entries").toArray()) {
                const QJsonObject entry = value.toObject();
                entries.append({entry.value("role").toString(), entry.value("text").toString()});
            }
            historyTotal_ = result.value("total").toInt();
            historyEntries_ = entries;
            showHistory(entries, historyTotal_ > entries.size());
        });
        connect(process, &QProcess::errorOccurred, this, [this, process, generation](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            historyProcesses_.removeAll(process);
            process->deleteLater();
            if (generation == historyGeneration_) showHistory({}, false, "Could not start Python: " + process->errorString());
        });
        process->start(claudePython_, {"-u", claudeScript_, "--cwd", viewPath_, "--read-session", viewId_,
                                       "--limit", QString::number(historyLimit_)});
    } else if (viewProvider_ == "Gemini") {
        QString filePath = localConversations_.value("Gemini\n" + viewId_).value("file").toString();
        if (!QFileInfo(filePath).isFile()) {
            filePath.clear();
            const QDir profileRoot(QDir(geminiDataDirectory_).filePath("tmp"));
            for (const QFileInfo &profile : profileRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QDir chats(QDir(profile.filePath()).filePath("chats"));
                for (const QFileInfo &file : chats.entryInfoList({"session-*.jsonl", "session-*.json"}, QDir::Files)) {
                    if (readGeminiSession(file, {}).value("id").toString() == viewId_) filePath = file.filePath();
                }
            }
        }
        if (filePath.isEmpty()) {
            showHistory({}, false, "The Gemini session file was not found.");
            return;
        }
        if (!reset) liveTranscript_.clear();
        const QList<ChatEntry> entries = readGeminiHistory(filePath);
        historyTotal_ = entries.size();
        historyEntries_ = entries.mid(qMax(0, entries.size() - historyLimit_));
        showHistory(historyEntries_, historyTotal_ > historyEntries_.size());
    } else {
        showHistory({}, false, "History preview is not available for " + viewProvider_ + " conversations.");
    }
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
    QString state;
    if (currentProvider_ == 4) {
        if (antigravityBusy_) state = "Antigravity is responding…";
        else if (QStandardPaths::findExecutable(antigravity_->program()).isEmpty())
            state = "Antigravity CLI is not installed or is not in PATH";
        else state = "Antigravity ready";
        stopButton_->setEnabled(antigravityBusy_ && !antigravityStopRequested_);
    } else if (currentProvider_ == 3) {
        state = geminiBusy_ ? "Gemini is responding…" : "Gemini ready";
        stopButton_->setEnabled(geminiBusy_ && !geminiStopRequested_);
    } else if (currentProvider_ == 2) {
        if (glmBusy_) state = "GLM is responding…";
        else if (glmReady_) state = "GLM ready";
        else if (glm_->isRunning()) state = "Connecting to GLM via Claude Agent SDK…";
        else state = "GLM bridge is not running";
        stopButton_->setEnabled(glmBusy_ && !glmStopRequested_);
    } else if (currentProvider_ == 1) {
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
    const QString currentDirectory = providerWorkingDirectory(currentProvider_);
    status_->setText(state + "  •  " + QDir::toNativeSeparators(currentDirectory));
    const int index = currentProvider_;
    sendButton_->setText(index == 0 ? "Send to Codex" : (index == 1 ? "Send to Claude"
                         : (index == 2 ? "Send to GLM" : (index == 3 ? "Send to Gemini" : "Send to Antigravity"))));
    sendButton_->setEnabled(viewLive_);
    // Reloading a longer tail while a live response streams would drop the partial answer.
    const bool viewBusy = viewLive_ && (currentProvider_ == 0 ? busy_ : (currentProvider_ == 1 ? claudeBusy_
        : (currentProvider_ == 2 ? glmBusy_ : (currentProvider_ == 3 ? geminiBusy_ : antigravityBusy_))));
    loadEarlierButton_->setEnabled(!viewBusy);
    input_->setPlaceholderText(viewLive_ ? "Enter a message or help, then press Enter"
                                         : "Read-only preview. Type help, new or clear, then press Enter");
    newChatButton_->setEnabled(!busy_ && !claudeBusy_ && !glmBusy_ && !geminiBusy_ && !antigravityBusy_);
}
