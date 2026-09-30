#include "AntigravityAgent.h"
#include "ChatTab.h"
#include "ClaudeAgent.h"
#include "CodexConnection.h"
#include "GeminiAgent.h"
#include "MainWindow.h"
#include "MessageInput.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextDocumentLayout>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextDocument>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <mrutabwidget.h>
#include <qxfiledialog.h>

#include <algorithm>

namespace {
QString shortPreview(const QString &value, int limit = 72)
{
    const QString singleLine = value.simplified();
    return singleLine.size() > limit ? singleLine.left(limit - 1) + QChar(0x2026) : singleLine;
}

QString windowName(const UsageLimit &limit)
{
    const qint64 minutes = limit.windowMinutes;
    QString length;
    if (minutes == 7 * 24 * 60) length = "week";
    else if (minutes > 0 && minutes % (24 * 60) == 0) length = QString("%1 days").arg(minutes / (24 * 60));
    else if (minutes > 0 && minutes % 60 == 0) length = QString("%1 h").arg(minutes / 60);
    else if (minutes > 0) length = QString("%1 min").arg(minutes);
    else length = "limit";
    return limit.name.isEmpty() ? length : limit.name + " " + length;
}

// The model list with a "Default model" entry first, which keeps the agent's own default model.
QList<AgentModel> modelChoices(const QList<AgentModel> &models)
{
    QList<AgentModel> choices = models;
    const bool hasDefaultEntry = std::any_of(models.begin(), models.end(),
                                             [](const AgentModel &model) { return model.id.isEmpty(); });
    if (hasDefaultEntry) return choices;
    AgentModel entry{{}, "Default model", "The agent's default model", {}, {}, {}, true};
    for (const AgentModel &model : models) {
        if (!model.isDefault) continue;
        entry.efforts = model.efforts;
        entry.effortDescriptions = model.effortDescriptions;
        entry.defaultEffort = model.defaultEffort;
    }
    choices.prepend(entry);
    return choices;
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
      codex_(new CodexConnection(codexProgram, workingDirectory, codexIndexFile(codexIndexPath), this)),
      claude_(new ClaudeProvider(claudePython, claudeScript, workingDirectory, "claude",
                                 QDir(dataDirectory_).filePath("claude-conversations.json"), this)),
      glm_(new ClaudeProvider(claudePython, claudeScript, workingDirectory, "glm",
                              QDir(dataDirectory_).filePath("glm-conversations.json"), this)),
      gemini_(new GeminiProvider(geminiProgram, geminiDataDirectory.isEmpty()
          ? QDir(QDir::homePath()).filePath(".gemini") : geminiDataDirectory,
          QDir(dataDirectory_).filePath("gemini-conversations.json"), this)),
      antigravity_(new AntigravityProvider(antigravityProgram,
                                           QDir(dataDirectory_).filePath("antigravity-conversations.json"), this)),
      providers_{codex_, claude_, gemini_, glm_, antigravity_}
{
    claude_->excludeSessionsOf(glm_);
    setWindowTitle("agentdeskt — Codex, Claude, GLM, Gemini and Antigravity");
    resize(900, 700);
    auto *conversationMenu = menuBar()->addMenu("Conversations");
    auto *newConversationAction = conversationMenu->addAction("New conversation in directory…");
    connect(newConversationAction, &QAction::triggered, this, &MainWindow::showNewConversationDialog);
    auto *settingsMenu = menuBar()->addMenu("Settings");
    auto *optionsAction = settingsMenu->addAction("Options…");
    connect(optionsAction, &QAction::triggered, this, &MainWindow::showOptionsDialog);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    newChatButton_ = new QPushButton("New chat…", central);
    newChatButton_->setObjectName("newChatButton");
    status_ = new QLabel(central);
    usage_ = new QLabel(central);
    usage_->setObjectName("usageLabel");
    tabs_ = new MruTabWidget(central);
    tabs_->setObjectName("chatTabs");
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setUsesScrollButtons(true);
    modelInput_ = new QComboBox(central);
    modelInput_->setObjectName("modelSelect");
    modelInput_->setToolTip("Model for the next messages in this chat");
    effortInput_ = new QComboBox(central);
    effortInput_->setObjectName("effortSelect");
    effortInput_->setToolTip("Reasoning effort for the next messages in this chat");
    input_ = new MessageInput(central);
    input_->setObjectName("commandInput");
    sendButton_ = new QPushButton("Send", central);
    sendButton_->setObjectName("sendButton");
    stopButton_ = new QPushButton("Stop", central);
    stopButton_->setObjectName("stopButton");
    stopButton_->setEnabled(false);

    chatPanel_ = new QWidget(this);
    chatPanel_->hide();
    auto *panelLayout = new QVBoxLayout(chatPanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    chatHeader_ = new QLabel(chatPanel_);
    chatHeader_->setObjectName("chatHeader");
    chatHeader_->setWordWrap(true);
    loadEarlierButton_ = new QPushButton("Show earlier messages", chatPanel_);
    loadEarlierButton_->setObjectName("loadEarlierButton");
    loadEarlierButton_->setVisible(false);
    chatView_ = new QPlainTextEdit(chatPanel_);
    chatView_->setObjectName("chatView");
    chatView_->setReadOnly(true);
    chatView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    chatView_->setPlaceholderText("Select a chat in the tree or start a new one.");
    emptyDocument_ = new QTextDocument(chatView_);
    emptyDocument_->setDocumentLayout(new QPlainTextDocumentLayout(emptyDocument_));
    chatView_->setDocument(emptyDocument_);
    panelLayout->addWidget(chatHeader_);
    panelLayout->addWidget(loadEarlierButton_);
    panelLayout->addWidget(chatView_, 1);

    log_ = new QPlainTextEdit(this);
    log_->setObjectName("log");
    log_->setReadOnly(true);
    log_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    log_->setPlaceholderText("Application messages will appear here.");

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(modelInput_);
    inputRow->addWidget(effortInput_);
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(sendButton_);
    inputRow->addWidget(stopButton_);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(usage_);
    statusRow->addWidget(newChatButton_);
    layout->addLayout(statusRow);
    layout->addWidget(tabs_, 1);
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

    // Selecting a chat previews it in the preview tab; double-clicking keeps the tab and continues the chat.
    connect(conversationTree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item) openConversation(item, false);
    });
    connect(conversationTree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        openConversation(item, true);
    });
    connect(conversationTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toString() != "provider") return;
        // Rebuilding the tree deletes item, so read its provider first.
        const QString name = item->text(0);
        expandedProviders_.insert(name);
        refreshConversationTree();
        if (AgentProvider *expanded = provider(name)) expanded->refreshConversations();
    });
    connect(conversationTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toString() != "provider") return;
        expandedProviders_.remove(item->text(0));
        refreshConversationTree();
    });
    connect(loadEarlierButton_, &QPushButton::clicked, this, [this] {
        if (ChatTab *tab = currentTab()) tab->loadEarlier();
    });
    connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::showCurrentTab);
    connect(tabs_, &MruTabWidget::tabAboutToClose, this, [this](QWidget *page, bool, bool &allow) {
        ChatTab *tab = chatTab(page);
        if (!allow || !tab || !tab->agent()->isResponding()) return;
        const auto answer = QMessageBox::question(this, "Close chat",
                                                   tab->provider()->name() + " is still responding in this chat. "
                                                   "Stop the response and close the tab?",
                                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) tab->agent()->interrupt();
        else allow = false;
    }, Qt::DirectConnection);
    // The shared chat view must leave a page before the page and its document are deleted.
    connect(tabs_, &MruTabWidget::tabClosing, this, [this](QWidget *page) {
        if (chatPanel_->parentWidget() == page) {
            chatPanel_->hide();
            chatPanel_->setParent(this);
        }
        ChatTab *tab = chatTab(page);
        if (tab && chatView_->document() == tab->document()) chatView_->setDocument(emptyDocument_);
    });

    connect(modelInput_, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::chooseModel);
    connect(effortInput_, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::chooseModel);
    connect(input_, &MessageInput::submitted, this, &MainWindow::submitCommand);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::submitCommand);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::requestStop);
    connect(newChatButton_, &QPushButton::clicked, this, &MainWindow::showNewConversationDialog);
    for (AgentProvider *listed : providers_) {
        connect(listed, &AgentProvider::message, this, &MainWindow::appendLine);
        connect(listed, &AgentProvider::stateChanged, this, &MainWindow::updateStatus);
        connect(listed, &AgentProvider::conversationsChanged, this, &MainWindow::refreshConversationTree);
        connect(listed, &AgentProvider::modelsChanged, this, &MainWindow::updateModelControls);
        connect(listed, &AgentProvider::usageChanged, this, &MainWindow::updateUsage);
    }

    appendLine("Directory: " + workingDirectory_);
    appendLine("Type help to see the available commands.\n");
    for (AgentProvider *listed : providers_) listed->loadConversations();
    loadSettings();
    loadRecentDirectories();
    refreshConversationTree();
    chatTab(addChatTab(codex_, workingDirectory_))->startDraft();
    if (!QDir(workingDirectory_).exists()) {
        appendLine("[Working directory does not exist: " + workingDirectory_ + "]");
        return;
    }
    codex_->start();
    input_->setFocus();
}

// Chats go first, while their providers still exist.
MainWindow::~MainWindow()
{
    disconnect(tabs_, nullptr, this, nullptr);
    for (AgentProvider *listed : providers_) disconnect(listed, nullptr, this, nullptr);
    chatView_->setDocument(emptyDocument_);
    chatPanel_->setParent(this);
    while (tabs_->count() > 0) {
        QWidget *page = tabs_->widget(0);
        disconnect(chatTab(page), nullptr, this, nullptr);
        tabs_->removeTab(0);
        delete page;
    }
}

void MainWindow::submitCommand()
{
    const QString command = input_->toPlainText().trimmed();
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
    } else if (ChatTab *tab = currentTab()) {
        tab->send(command);
    } else {
        appendLine("[No chat is open. Use New chat… to start one.]");
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
    appendLine("All other text is sent to the chat in the current tab as a message.\n");
    const ChatTab *tab = currentTab();
    const AgentHelp help = (tab ? tab->provider() : providers_.first())->help();
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

void MainWindow::requestStop()
{
    ChatTab *tab = currentTab();
    if (!tab) return;
    AgentBackend *agent = tab->agent();
    if (!agent->isResponding()) {
        appendLine("[No active " + agent->name() + " response.]");
        return;
    }
    if (!agent->canInterrupt()) return;
    appendLine("[Interrupting " + agent->name() + " response]");
    agent->interrupt();
}

AgentProvider *MainWindow::provider(const QString &name) const
{
    for (AgentProvider *listed : providers_) {
        if (listed->name() == name) return listed;
    }
    return nullptr;
}

// settings.json keeps each agent's default model and effort for new chats. The effort defaults to
// medium; agents without an effort setting ignore it.
void MainWindow::loadSettings()
{
    QFile file(QDir(dataDirectory_).filePath("settings.json"));
    const QJsonObject agents = file.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(file.readAll()).object().value("agents").toObject() : QJsonObject();
    for (AgentProvider *listed : providers_) {
        const QJsonObject options = agents.value(listed->name()).toObject();
        listed->setDefaults(options.value("model").toString(), options.value("effort").toString("medium"));
    }
    file.seek(0);
    const QString enterKey = QJsonDocument::fromJson(file.readAll()).object().value("enterKey").toString();
    input_->setEnterPolicy(enterKey == "send" ? MessageInput::EnterPolicy::Send
                           : (enterKey == "newline" ? MessageInput::EnterPolicy::NewLine
                                                    : MessageInput::EnterPolicy::Smart));
}

void MainWindow::saveSettings()
{
    const MessageInput::EnterPolicy policy = input_->enterPolicy();
    const QString enterKey = policy == MessageInput::EnterPolicy::Send ? "send"
        : (policy == MessageInput::EnterPolicy::NewLine ? "newline" : "smart");
    QJsonObject agents;
    for (const AgentProvider *listed : providers_)
        agents.insert(listed->name(), QJsonObject{{"model", listed->defaultModel()}, {"effort", listed->defaultEffort()}});
    if (!QDir().mkpath(dataDirectory_)) return;
    QSaveFile file(QDir(dataDirectory_).filePath("settings.json"));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(QJsonObject{{"version", 1}, {"enterKey", enterKey}, {"agents", agents}})
                          .toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        appendLine("[Could not save settings: " + file.errorString() + "]");
    }
}

// Default model and effort per agent for new chats. Agents without a model list are not shown;
// Codex lists its models once the App Server is connected.
void MainWindow::showOptionsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Options");
    auto *layout = new QVBoxLayout(&dialog);
    auto *enterForm = new QFormLayout;
    auto *enterInput = new QComboBox(&dialog);
    enterInput->setObjectName("enterKeyPolicy");
    enterInput->addItem("New line while typing; send a recalled or pasted message",
                        QVariant::fromValue(int(MessageInput::EnterPolicy::Smart)));
    enterInput->addItem("Always send", QVariant::fromValue(int(MessageInput::EnterPolicy::Send)));
    enterInput->addItem("Always start a new line", QVariant::fromValue(int(MessageInput::EnterPolicy::NewLine)));
    enterInput->setCurrentIndex(enterInput->findData(int(input_->enterPolicy())));
    enterForm->addRow("Enter key:", enterInput);
    layout->addLayout(enterForm);
    layout->addWidget(new QLabel("Shift+Enter always starts a new line and Ctrl+Enter always sends.", &dialog));
    layout->addWidget(new QLabel("Model and reasoning effort that new chats start with:", &dialog));
    auto *form = new QFormLayout;
    layout->addLayout(form);
    struct Row
    {
        AgentProvider *provider;
        QComboBox *model;
        QComboBox *effort;
    };
    QList<Row> rows;
    for (AgentProvider *listed : providers_) {
        const QList<AgentModel> choices = modelChoices(listed->models());
        if (listed->models().isEmpty() && listed != codex_) continue;
        auto *model = new QComboBox(&dialog);
        model->setObjectName("defaultModel" + listed->name());
        auto *effort = new QComboBox(&dialog);
        effort->setObjectName("defaultEffort" + listed->name());
        for (const AgentModel &choice : choices) model->addItem(choice.displayName, choice.id);
        if (model->findData(listed->defaultModel()) < 0) model->addItem(listed->defaultModel(), listed->defaultModel());
        model->setCurrentIndex(model->findData(listed->defaultModel()));
        const auto fillEfforts = [choices, model, effort](const QString &preferred) {
            effort->clear();
            QStringList efforts{"low", "medium", "high"};
            for (const AgentModel &choice : choices) {
                if (choice.id == model->currentData().toString()) efforts = choice.efforts;
            }
            if (efforts.isEmpty()) {
                effort->addItem("No effort setting", preferred);
                effort->setEnabled(false);
                return;
            }
            effort->setEnabled(true);
            for (const QString &level : efforts) effort->addItem(level, level);
            const int index = effort->findData(preferred);
            effort->setCurrentIndex(index >= 0 ? index : qMax(0, effort->findData("medium")));
        };
        fillEfforts(listed->defaultEffort());
        connect(model, QOverload<int>::of(&QComboBox::activated), &dialog, [fillEfforts, effort] {
            fillEfforts(effort->currentData().toString());
        });
        auto *row = new QHBoxLayout;
        row->addWidget(model, 1);
        row->addWidget(effort);
        form->addRow(listed->name() + ":", row);
        rows.append({listed, model, effort});
    }
    if (codex_->models().isEmpty())
        layout->addWidget(new QLabel("Codex models appear here once the App Server is connected.", &dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    input_->setEnterPolicy(MessageInput::EnterPolicy(enterInput->currentData().toInt()));
    for (const Row &row : rows) row.provider->setDefaults(row.model->currentData().toString(), row.effort->currentData().toString());
    saveSettings();
    modelControlsState_.clear();
    updateModelControls();
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

void MainWindow::showNewConversationDialog()
{
    const ChatTab *tab = currentTab();
    const QString currentPath = tab ? tab->workingDirectory() : workingDirectory_;
    QDialog dialog(this);
    dialog.setWindowTitle("New conversation");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Agent:", &dialog));
    auto *providerInput = new QComboBox(&dialog);
    providerInput->setObjectName("newConversationProvider");
    for (const AgentProvider *listed : providers_) providerInput->addItem(listed->name());
    if (tab) providerInput->setCurrentText(tab->provider()->name());
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
        newConversation(providers_.value(providerInput->currentIndex()), path);
    }
}

void MainWindow::newConversation(AgentProvider *selected, const QString &path)
{
    const QFileInfo directory(path);
    if (!selected || !directory.isDir()) {
        appendLine("[Directory does not exist: " + directory.absoluteFilePath() + "]");
        return;
    }
    const QString canonicalPath = directory.canonicalFilePath();
    QWidget *page = addChatTab(selected, canonicalPath);
    if (!chatTab(page)->startNew(canonicalPath)) {
        tabs_->requestCloseTab(page);
        return;
    }
    tabs_->setCurrentWidget(page);
    appendLine("[Starting a new " + selected->name() + " conversation in " + canonicalPath + "]");
}

void MainWindow::refreshConversationTree()
{
    const QSignalBlocker blocker(conversationTree_);
    conversationTree_->clear();
    for (AgentProvider *listed : providers_) {
        const QString name = listed->name();
        auto *root = new QTreeWidgetItem(conversationTree_, {name});
        root->setData(0, Qt::UserRole, "provider");
        root->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if (!expandedProviders_.contains(name)) continue;
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
            item->setData(0, Qt::UserRole, name);
            item->setData(0, Qt::UserRole + 1, chat.value("id").toString());
            item->setData(0, Qt::UserRole + 2, path);
            item->setToolTip(0, chat.value("tooltip").toString());
        }
    }
}

// A chat already open in a tab is shown there; otherwise the preview tab shows it read-only.
void MainWindow::openConversation(QTreeWidgetItem *item, bool continueChat)
{
    const QString name = item->data(0, Qt::UserRole).toString();
    const QString id = item->data(0, Qt::UserRole + 1).toString();
    AgentProvider *selected = name == "provider" ? nullptr : provider(name);
    if (!selected || id.isEmpty()) return;
    QWidget *page = tabs_->findTab(ChatTab::key(name, id));
    if (!page) {
        const QString path = item->data(0, Qt::UserRole + 2).toString();
        page = tabs_->previewTab();
        if (!page) {
            page = addChatTab(selected, path);
            tabs_->setTabPreview(page, true);
        }
        chatTab(page)->showPreview(selected, id, path, item->text(0));
    }
    tabs_->setCurrentWidget(page);
    if (!continueChat) return;
    if (page == tabs_->previewTab()) tabs_->promotePreviewTab();
    chatTab(page)->continueChat();
}

QWidget *MainWindow::addChatTab(AgentProvider *selected, const QString &workingDirectory)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *tab = new ChatTab(selected, workingDirectory, this, page);
    connect(tab, &ChatTab::logMessage, this, &MainWindow::appendLine);
    connect(tab, &ChatTab::changed, this, [this, page] { updateTab(page); });
    connect(tab, &ChatTab::textAppended, this, [this, page] {
        if (tabs_->currentWidget() != page) {
            tabs_->setTabAttention(page, true);
            return;
        }
        chatView_->moveCursor(QTextCursor::End);
        chatView_->ensureCursorVisible();
    });
    connect(tab, &ChatTab::activateRequested, this, [this, page] { tabs_->setCurrentWidget(page); });
    connect(tab, &ChatTab::userMessagesChanged, this, [this, page, tab] {
        if (tabs_->currentWidget() == page) input_->setHistory(tab->userMessages());
    });
    tabs_->addTab(page, selected->name());
    updateTab(page);
    return page;
}

ChatTab *MainWindow::chatTab(QWidget *page) const
{
    return page ? page->findChild<ChatTab *>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

ChatTab *MainWindow::currentTab() const
{
    return chatTab(tabs_->currentWidget());
}

void MainWindow::showCurrentTab()
{
    QWidget *page = tabs_->currentWidget();
    ChatTab *tab = chatTab(page);
    input_->setHistory(tab ? tab->userMessages() : QStringList());
    if (!tab) {
        chatPanel_->hide();
        chatPanel_->setParent(this);
        chatView_->setDocument(emptyDocument_);
    } else {
        if (chatPanel_->parentWidget() != page) {
            page->layout()->addWidget(chatPanel_);
            chatPanel_->show();
        }
        if (chatView_->document() != tab->document()) {
            chatView_->setDocument(tab->document());
            chatView_->moveCursor(QTextCursor::End);
            chatView_->ensureCursorVisible();
        }
    }
    updateStatus();
}

void MainWindow::updateTab(QWidget *page)
{
    ChatTab *tab = chatTab(page);
    const int index = tabs_->indexOf(page);
    if (!tab || index < 0) return;
    tabs_->setTabText(index, tab->provider()->name() + ": " + shortPreview(tab->title(), 32));
    tabs_->setTabToolTip(index, tab->headerText());
    tabs_->setTabPopupText(index, tab->headerText());
    tabs_->setTabKey(page, tab->key());
    tabs_->setTabBusy(page, tab->agent()->isResponding());
    if (page == tabs_->currentWidget()) updateStatus();
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

void MainWindow::updateStatus()
{
    const ChatTab *tab = currentTab();
    if (!tab) {
        status_->setText("No chat open");
        chatHeader_->clear();
        loadEarlierButton_->setVisible(false);
        sendButton_->setText("Send");
        sendButton_->setEnabled(false);
        stopButton_->setEnabled(false);
        input_->setPlaceholderText("Type help or new, then press Ctrl+Enter");
        updateModelControls();
        updateUsage();
        return;
    }
    const AgentBackend *agent = tab->agent();
    status_->setText(agent->statusText() + "  •  " + QDir::toNativeSeparators(tab->workingDirectory()));
    chatHeader_->setText(tab->headerText());
    chatHeader_->setToolTip(tab->conversationId());
    loadEarlierButton_->setVisible(tab->hasMoreHistory());
    // Reloading a longer tail while a live response streams would drop the partial answer.
    loadEarlierButton_->setEnabled(!(tab->isLive() && agent->isResponding()));
    stopButton_->setEnabled(agent->canInterrupt());
    sendButton_->setText("Send to " + tab->provider()->name());
    sendButton_->setEnabled(tab->isLive());
    input_->setPlaceholderText(tab->isLive() ? "Message or help, then Ctrl+Enter to send"
                                             : "Read-only preview. Type help, new or clear, then press Enter");
    updateModelControls();
    updateUsage();
}

// Shows how much of each usage limit window is left for the current tab's agent.
void MainWindow::updateUsage()
{
    const ChatTab *tab = currentTab();
    QList<UsageLimit> limits = tab ? tab->provider()->usageLimits() : QList<UsageLimit>{};
    std::sort(limits.begin(), limits.end(), [](const UsageLimit &left, const UsageLimit &right) {
        return left.windowMinutes == right.windowMinutes ? left.name < right.name
                                                         : left.windowMinutes < right.windowMinutes;
    });
    QStringList parts;
    QStringList details;
    for (const UsageLimit &limit : limits) {
        const QString name = windowName(limit);
        const QString left = limit.usedPercent < 0 ? limit.status
                                                   : QString("%1% left").arg(qRound(100 - limit.usedPercent));
        parts.append(name + ": " + left);
        QString detail = name + ": ";
        detail += limit.usedPercent < 0 ? "usage not reported" : QString("%1% used").arg(qRound(limit.usedPercent));
        if (limit.resetsAt > 0)
            detail += ", resets " + QDateTime::fromSecsSinceEpoch(limit.resetsAt).toString("ddd d MMM HH:mm");
        if (!limit.status.isEmpty()) detail += " (" + limit.status + ")";
        details.append(detail);
    }
    usage_->setText(parts.isEmpty() ? QString() : tab->provider()->name() + " limits  " + parts.join("  •  "));
    usage_->setToolTip(details.join('\n'));
}

// Shows the current chat's model and effort. Without a choice yet, the provider's default model and
// that model's default effort are shown.
void MainWindow::updateModelControls()
{
    const ChatTab *tab = currentTab();
    const QList<AgentModel> models = tab ? tab->provider()->models() : QList<AgentModel>{};
    QStringList state{tab ? tab->provider()->name() : QString(), tab && tab->isLive() ? "live" : "read-only",
                      tab ? tab->agent()->model() : QString(), tab ? tab->agent()->effort() : QString()};
    for (const AgentModel &model : models) state.append(model.id + ':' + model.efforts.join(','));
    if (state.join('\n') == modelControlsState_) return;
    modelControlsState_ = state.join('\n');
    const QSignalBlocker modelBlocker(modelInput_);
    const QSignalBlocker effortBlocker(effortInput_);
    modelInput_->clear();
    effortInput_->clear();
    const bool available = !models.isEmpty();
    modelInput_->setEnabled(available && tab->isLive());
    effortInput_->setEnabled(available && tab->isLive());
    if (!available) {
        modelInput_->addItem(tab ? "Default model" : "No chat");
        effortInput_->addItem("Default effort");
        return;
    }
    QString selected = tab->agent()->model();
    const AgentModel *current = nullptr;
    for (const AgentModel &model : models) {
        modelInput_->addItem(model.displayName, model.id);
        modelInput_->setItemData(modelInput_->count() - 1, model.description, Qt::ToolTipRole);
        if (model.id == selected || (selected.isEmpty() && model.isDefault)) current = &model;
    }
    if (!current) {
        // A thread may use a model that the list hides; keep it selectable as it is.
        modelInput_->addItem(selected.isEmpty() ? "Default model" : selected, selected);
        modelInput_->setCurrentIndex(modelInput_->count() - 1);
        effortInput_->addItem(tab->agent()->effort().isEmpty() ? "Default effort" : tab->agent()->effort(),
                              tab->agent()->effort());
        return;
    }
    modelInput_->setCurrentIndex(modelInput_->findData(current->id));
    if (current->efforts.isEmpty()) {
        effortInput_->addItem("No effort setting");
        effortInput_->setEnabled(false);
        return;
    }
    for (int i = 0; i < current->efforts.size(); ++i) {
        effortInput_->addItem(current->efforts.at(i), current->efforts.at(i));
        effortInput_->setItemData(i, current->effortDescriptions.value(i), Qt::ToolTipRole);
    }
    const QString effort = tab->agent()->effort();
    const int effortIndex = effortInput_->findData(effort.isEmpty() ? current->defaultEffort : effort);
    effortInput_->setCurrentIndex(effortIndex >= 0 ? effortIndex : effortInput_->findData(current->defaultEffort));
}

// A new model keeps the chosen effort when it supports it and otherwise uses the model's default.
void MainWindow::chooseModel()
{
    ChatTab *tab = currentTab();
    if (!tab) return;
    const QString modelId = modelInput_->currentData().toString();
    QString effort = effortInput_->currentData().toString();
    for (const AgentModel &model : tab->provider()->models()) {
        if (model.id != modelId) continue;
        if (!model.efforts.contains(effort)) effort = model.defaultEffort;
        break;
    }
    tab->agent()->setModel(modelId, effort);
}
