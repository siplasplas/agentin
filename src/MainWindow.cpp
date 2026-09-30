#include "AntigravityAgent.h"
#include "ApprovalRules.h"
#include "ChatTab.h"
#include "ChatView.h"
#include "ClaudeAgent.h"
#include "CodexConnection.h"
#include "GeminiAgent.h"
#include "MainWindow.h"
#include "MessageInput.h"
#include "Notifier.h"
#include "TurnLocks.h"

#include <QCheckBox>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QRadioButton>
#include <QRegularExpression>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextDocumentLayout>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
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
QString groupedTokens(qint64 count)
{
    if (count < 0) return QString(QChar(0x2014));
    QString text = QString::number(count);
    for (int position = text.size() - 3; position > 0; position -= 3) text.insert(position, '\'');
    return text;
}

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
      providers_{codex_, claude_, gemini_, glm_, antigravity_},
      turnLocks_(new TurnLocks(dataDirectory_, this)), notifier_(new Notifier(this))
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
    auto *fontAction = settingsMenu->addAction("Chat font…");
    connect(fontAction, &QAction::triggered, this, [this] {
        bool accepted = false;
        const QFont chosen = QFontDialog::getFont(&accepted, chatView_->font(), this, "Chat font");
        if (accepted) {
            applyChatFont(chosen);
            saveSettings();
        }
    });
    auto *fontSizeAction = settingsMenu->addAction("Chat font size…");
    connect(fontSizeAction, &QAction::triggered, this, [this] {
        QInputDialog dialog(this);
        dialog.setWindowTitle("Chat font size");
        dialog.setLabelText("Size in points:");
        dialog.setInputMode(QInputDialog::DoubleInput);
        dialog.setDoubleRange(6.0, 72.0);
        dialog.setDoubleDecimals(1);
        dialog.setDoubleStep(0.5);
        dialog.setDoubleValue(chatView_->font().pointSizeF());
        if (dialog.exec() == QDialog::Accepted) {
            QFont font = chatView_->font();
            font.setPointSizeF(dialog.doubleValue());
            applyChatFont(font);
            saveSettings();
        }
    });
    auto *notificationsAction = settingsMenu->addAction("Notifications…");
    connect(notificationsAction, &QAction::triggered, this, &MainWindow::showNotificationsDialog);
    auto *approvalsAction = settingsMenu->addAction("Approvals…");
    connect(approvalsAction, &QAction::triggered, this, &MainWindow::showApprovalsDialog);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    newChatButton_ = new QPushButton("New chat…", central);
    newChatButton_->setObjectName("newChatButton");
    // Long status texts are cut off instead of widening the window; the tooltip keeps the full text.
    status_ = new QLabel(central);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->setMinimumWidth(0);
    usage_ = new QLabel(central);
    usage_->setObjectName("usageLabel");
    usage_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    usage_->setMinimumWidth(0);
    tabs_ = new MruTabWidget(central);
    tabs_->setObjectName("chatTabs");
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setUsesScrollButtons(true);
    modelInput_ = new QComboBox(this);
    modelInput_->setObjectName("modelSelect");
    modelInput_->setToolTip("Model for the next messages in this chat");
    effortInput_ = new QComboBox(this);
    effortInput_->setObjectName("effortSelect");
    effortInput_->setToolTip("Reasoning effort for the next messages in this chat");
    readOnlyInput_ = new QCheckBox("Read-only", this);
    readOnlyInput_->setObjectName("readOnlyToggle");
    for (QComboBox *combo : {modelInput_, effortInput_}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(6);
    }
    input_ = new MessageInput(central);
    input_->setObjectName("commandInput");
    enterIndicator_ = new QLabel(central);
    enterIndicator_->setObjectName("enterIndicator");
    sendButton_ = new QPushButton("Send", central);
    sendButton_->setObjectName("sendButton");
    steerButton_ = new QPushButton("Steer", central);
    steerButton_->setObjectName("steerButton");
    steerButton_->setToolTip("Send this message to the running turn; Send queues it for the next turn");
    steerButton_->setEnabled(false);
    steerButton_->hide();
    stopButton_ = new QPushButton("Stop", central);
    stopButton_->setObjectName("stopButton");
    stopButton_->setEnabled(false);

    chatPanel_ = new QWidget(this);
    chatPanel_->hide();
    auto *panelLayout = new QVBoxLayout(chatPanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    chatHeader_ = new QLabel(chatPanel_);
    chatHeader_->setObjectName("chatHeader");
    chatHeader_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    chatHeader_->setMinimumWidth(0);
    loadEarlierButton_ = new QPushButton("Show earlier messages", chatPanel_);
    loadEarlierButton_->setObjectName("loadEarlierButton");
    loadEarlierButton_->setVisible(false);
    chatView_ = new ChatView(chatPanel_);
    chatView_->setObjectName("chatView");
    chatView_->setReadOnly(true);
    chatView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    chatView_->setPlaceholderText("Select a chat in the tree or start a new one.");
    emptyDocument_ = new QTextDocument(chatView_);
    emptyDocument_->setDocumentLayout(new QPlainTextDocumentLayout(emptyDocument_));
    chatView_->setDocument(emptyDocument_);
    // The model and effort belong to the chat, so they sit in its header.
    tokens_ = new QLabel(chatPanel_);
    tokens_->setObjectName("tokenUsage");
    auto *headerRow = new QHBoxLayout;
    headerRow->addWidget(chatHeader_, 1);
    operationTime_ = new QLabel("Time 00:00", chatPanel_);
    operationTime_->setObjectName("operationTime");
    operationTime_->setToolTip("Elapsed time of the current task or compaction; keeps the final duration when it ends");
    headerRow->addWidget(operationTime_);
    auto *operationTimer = new QTimer(this);
    operationTimer->setInterval(1000);
    connect(operationTimer, &QTimer::timeout, this, &MainWindow::updateOperationTime);
    operationTimer->start();
    headerRow->addWidget(tokens_);
    headerRow->addWidget(modelInput_);
    headerRow->addWidget(effortInput_);
    headerRow->addWidget(readOnlyInput_);
    panelLayout->addLayout(headerRow);
    compactionPanel_ = new QWidget(chatPanel_);
    auto *compactionRow = new QHBoxLayout(compactionPanel_);
    compactionRow->setContentsMargins(0, 0, 0, 0);
    compactButton_ = new QPushButton("Compact", compactionPanel_);
    compactButton_->setObjectName("compactButton");
    compactButton_->setToolTip("Compact this chat's context when Codex is idle");
    contextTokens_ = new QLineEdit(compactionPanel_);
    contextTokens_->setObjectName("contextTokens");
    contextTokens_->setReadOnly(true);
    contextTokens_->setAlignment(Qt::AlignRight);
    contextTokens_->setFixedWidth(contextTokens_->fontMetrics().horizontalAdvance("999'999'999") + 24);
    contextTokens_->setToolTip("Current context tokens reported by App Server; updated after compaction");
    compactionRow->addWidget(compactButton_);
    compactionRow->addWidget(contextTokens_);
    compactionRow->addWidget(new QLabel("tokens", compactionPanel_));
    compactionRow->addStretch(1);
    compactionPanel_->hide();
    connect(compactButton_, &QPushButton::clicked, this, [this] {
        if (ChatTab *tab = currentTab()) {
            if (tab->isLive() && !tab->isWaiting()) tab->agent()->compact();
        }
        updateStatus();
    });
    panelLayout->addWidget(compactionPanel_);
    panelLayout->addWidget(loadEarlierButton_);
    panelLayout->addWidget(chatView_, 1);
    requestPanel_ = new QFrame(chatPanel_);
    requestPanel_->setObjectName("requestPanel");
    static_cast<QFrame *>(requestPanel_)->setFrameShape(QFrame::StyledPanel);
    new QVBoxLayout(requestPanel_);
    requestPanel_->hide();
    panelLayout->addWidget(requestPanel_);

    log_ = new QPlainTextEdit(this);
    log_->setObjectName("log");
    log_->setReadOnly(true);
    log_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    log_->setPlaceholderText("Application messages will appear here.");

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(enterIndicator_);
    inputRow->addWidget(sendButton_);
    inputRow->addWidget(steerButton_);
    inputRow->addWidget(stopButton_);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(usage_);
    muteSounds_ = new QToolButton(central);
    muteSounds_->setObjectName("muteSounds");
    muteSounds_->setCheckable(true);
    muteSounds_->setAutoRaise(true);
    statusRow->addWidget(muteSounds_);
    statusRow->addWidget(newChatButton_);
    layout->addLayout(statusRow);
    layout->addWidget(tabs_, 1);
    layout->addLayout(inputRow);
    auto *chatSplitter = new QSplitter(Qt::Horizontal, this);
    conversationTree_ = new QTreeWidget(chatSplitter);
    conversationTree_->setObjectName("conversationTree");
    conversationTree_->setHeaderHidden(true);
    conversationTree_->setMinimumWidth(0);
    chatSplitter->addWidget(conversationTree_);
    chatSplitter->addWidget(central);
    // The tree can be dragged down to nothing and widened to read long directories; the chat stays.
    chatSplitter->setCollapsible(0, true);
    chatSplitter->setCollapsible(1, false);
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
        notifier_->waitingEnded(QString::number(quintptr(page)));
        if (chatPanel_->parentWidget() == page) {
            chatPanel_->hide();
            chatPanel_->setParent(this);
        }
        ChatTab *tab = chatTab(page);
        if (tab && chatView_->document() == tab->document()) chatView_->setDocument(emptyDocument_);
    });

    connect(modelInput_, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::chooseModel);
    connect(effortInput_, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::chooseModel);
    connect(readOnlyInput_, &QCheckBox::clicked, this, [this](bool checked) {
        if (ChatTab *tab = currentTab()) tab->agent()->setReadOnly(checked);
    });
    connect(input_, &MessageInput::submitted, this, &MainWindow::submitCommand);
    connect(muteSounds_, &QToolButton::toggled, this, [this](bool muted) {
        notifier_->setMuted(muted);
        showMuteState();
        saveSettings();
    });
    showMuteState();
    connect(input_, &MessageInput::enterActionChanged, this, &MainWindow::showEnterAction);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::submitCommand);
    connect(steerButton_, &QPushButton::clicked, this, &MainWindow::submitSteer);
    connect(input_, &QPlainTextEdit::textChanged, this, &MainWindow::updateSteerButton);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::requestStop);
    connect(newChatButton_, &QPushButton::clicked, this, &MainWindow::showNewConversationDialog);
    auto *newChatShortcut = new QShortcut(QKeySequence("Ctrl+T"), this);
    connect(newChatShortcut, &QShortcut::activated, this, &MainWindow::showNewConversationDialog);
    // Closing asks first when the agent is still responding, as the tab's close button does.
    auto *closeTabShortcut = new QShortcut(QKeySequence("Ctrl+W"), this);
    connect(closeTabShortcut, &QShortcut::activated, this, [this] {
        if (QWidget *page = tabs_->currentWidget()) tabs_->requestCloseTab(page);
    });
    newChatButton_->setToolTip("New chat (Ctrl+T)");
    connect(codex_, &CodexConnection::connected, this, [this] {
        const QList<QPointer<QWidget>> pages = continueWhenConnected_;
        continueWhenConnected_.clear();
        for (const QPointer<QWidget> &page : pages) {
            if (ChatTab *tab = page ? chatTab(page) : nullptr) tab->continueChat();
        }
    });
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
    showEnterAction(input_->enterSends());
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

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (sessionEnabled_) saveSession();
    QMainWindow::closeEvent(event);
}

// open-tabs.json keeps the tabs except the preview tab, with their model, effort and read-only mode.
void MainWindow::saveSession()
{
    QJsonArray saved;
    int current = -1;
    for (int i = 0; i < tabs_->count(); ++i) {
        QWidget *page = tabs_->widget(i);
        const ChatTab *tab = chatTab(page);
        if (!tab || page == tabs_->previewTab()) continue;
        if (page == tabs_->currentWidget()) current = saved.size();
        const AgentBackend *agent = tab->agent();
        saved.append(QJsonObject{{"provider", tab->provider()->name()}, {"id", tab->conversationId()},
                                 {"directory", tab->workingDirectory()}, {"title", tab->title()},
                                 {"live", tab->isLive()}, {"model", agent->model()}, {"effort", agent->effort()},
                                 {"readOnly", agent->isReadOnly()}});
    }
    if (!QDir().mkpath(dataDirectory_)) return;
    QSaveFile file(QDir(dataDirectory_).filePath("open-tabs.json"));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(QJsonObject{{"version", 1}, {"current", current}, {"tabs", saved}})
                          .toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        appendLine("[Could not save the open tabs: " + file.errorString() + "]");
    }
}

void MainWindow::restoreSession(bool keepStartChat)
{
    sessionEnabled_ = true;
    QFile file(QDir(dataDirectory_).filePath("open-tabs.json"));
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject session = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray saved = session.value("tabs").toArray();
    if (saved.isEmpty()) return;
    QWidget *startChat = tabs_->widget(0);
    QWidget *current = nullptr;
    for (int i = 0; i < saved.size(); ++i) {
        const QJsonObject entry = saved.at(i).toObject();
        AgentProvider *selected = provider(entry.value("provider").toString());
        const QString directory = entry.value("directory").toString();
        if (!selected || !QFileInfo(directory).isDir()) continue;
        QWidget *page = addChatTab(selected, directory);
        ChatTab *tab = chatTab(page);
        AgentBackend *agent = tab->agent();
        if (!entry.value("model").toString().isEmpty() || !entry.value("effort").toString().isEmpty())
            agent->setModel(entry.value("model").toString(), entry.value("effort").toString());
        if (entry.value("readOnly").toBool() && agent->supportsReadOnly()) agent->setReadOnly(true);
        const QString id = entry.value("id").toString();
        if (id.isEmpty()) {
            tab->startDraft();
        } else {
            tab->showPreview(selected, id, directory, entry.value("title").toString());
            if (entry.value("live").toBool()) {
                // Codex resumes only once its App Server is connected.
                if (selected == codex_ && !codex_->isConnected()) continueWhenConnected_.append(page);
                else tab->continueChat();
            }
        }
        if (i == session.value("current").toInt(-1)) current = page;
    }
    if (!keepStartChat && startChat && tabs_->count() > 1) tabs_->requestCloseTab(startChat);
    if (current) tabs_->setCurrentWidget(current);
    revealCurrentConversation(true);
    QTimer::singleShot(0, this, [this] { revealCurrentConversation(true, true); });
    appendLine(QString("[Reopened %1 tabs from the last session]").arg(saved.size()));
}

void MainWindow::submitCommand()
{
    const QString command = input_->toPlainText().trimmed();
    if (command.isEmpty()) return;
    // An undoable clear lets Ctrl+Z bring a sent message back for editing; clear() also drops the
    // undo history when the options turn that off.
    if (undoAfterSend_) input_->replaceText({});
    else input_->clear();
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
        if (!tab->answerWithText(command)) tab->send(command);
    } else {
        appendLine("[No chat is open. Use New chat… to start one.]");
    }
}

void MainWindow::submitSteer()
{
    ChatTab *tab = currentTab();
    const QString text = input_->toPlainText().trimmed();
    if (!tab || text.isEmpty() || !tab->steer(text)) return;
    if (undoAfterSend_) input_->replaceText({});
    else input_->clear();
    input_->setFocus();
}

void MainWindow::updateSteerButton()
{
    const ChatTab *tab = currentTab();
    steerButton_->setVisible(tab && tab->agent()->supportsSteering());
    steerButton_->setEnabled(tab && tab->isLive() && !tab->pendingRequest()
                            && tab->agent()->canSteer() && !input_->toPlainText().trimmed().isEmpty());
}

void MainWindow::showHelp()
{
    appendLine("Commands:");
    appendLine("  help       show this help and installed agent options");
    appendLine("  new        start a new conversation");
    appendLine("  clear      clear the log pane");
    appendLine("  stop       interrupt the current response");
    appendLine("  quit       close the application");
    appendLine("All other text is sent to the chat in the current tab as a message.");
    appendLine("During a Codex turn, Steer sends the message to that turn; Send queues it for the next turn.\n");
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
    if (tab->isWaiting()) {
        tab->cancelWaiting();
        return;
    }
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
    const QJsonObject settings = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                                                : QJsonObject();
    QFont chatFont("Monospace", 10);
    chatFont.setStyleHint(QFont::TypeWriter);
    if (settings.value("chatFont").isString()) chatFont.fromString(settings.value("chatFont").toString());
    applyChatFont(chatFont);
    const QJsonObject agents = settings.value("agents").toObject();
    for (AgentProvider *listed : providers_) {
        const QJsonObject options = agents.value(listed->name()).toObject();
        listed->setDefaults(options.value("model").toString(), options.value("effort").toString("medium"));
    }
    const QString enterKey = settings.value("enterKey").toString();
    input_->setShortMessageLength(settings.value("enterSendsUpTo").toInt(60));
    undoAfterSend_ = settings.value("undoAfterSend").toBool(true);
    notifier_->setSettings(Notifier::Settings::fromJson(settings.value("notifications").toObject()));
    {
        // Loading must not save the settings before all of them are read.
        const QSignalBlocker blocker(muteSounds_);
        muteSounds_->setChecked(notifier_->settings().muted);
    }
    showMuteState();
    QStringList glmModels;
    for (const QJsonValue &value : settings.value("glmModels").toArray()) {
        if (!value.toString().isEmpty()) glmModels.append(value.toString());
    }
    glm_->setExtraModels(glmModels);
    input_->setEnterPolicy(enterKey == "send" ? MessageInput::EnterPolicy::Send
                           : (enterKey == "newline" ? MessageInput::EnterPolicy::NewLine
                                                    : MessageInput::EnterPolicy::Smart));
}

void MainWindow::applyChatFont(const QFont &font)
{
    chatView_->setFont(font);
    input_->setFont(font);
    log_->setFont(font);
    emptyDocument_->setDefaultFont(font);
    for (int i = 0; i < tabs_->count(); ++i) {
        if (ChatTab *tab = chatTab(tabs_->widget(i))) tab->document()->setDefaultFont(font);
    }
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
        || file.write(QJsonDocument(QJsonObject{{"version", 1}, {"enterKey", enterKey},
                                                         {"enterSendsUpTo", input_->shortMessageLength()},
                                                         {"undoAfterSend", undoAfterSend_},
                                                         {"chatFont", chatView_->font().toString()},
                                                         {"notifications", notifier_->settings().toJson()},
                                                         {"glmModels", QJsonArray::fromStringList(glm_->extraModels())},
                                                         {"agents", agents}})
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
    enterInput->addItem("Send short messages, start a new line in longer ones",
                        QVariant::fromValue(int(MessageInput::EnterPolicy::Smart)));
    enterInput->addItem("Always send", QVariant::fromValue(int(MessageInput::EnterPolicy::Send)));
    enterInput->addItem("Always start a new line", QVariant::fromValue(int(MessageInput::EnterPolicy::NewLine)));
    enterInput->setCurrentIndex(enterInput->findData(int(input_->enterPolicy())));
    enterForm->addRow("Enter key:", enterInput);
    auto *shortLength = new QSpinBox(&dialog);
    shortLength->setObjectName("enterSendsUpTo");
    shortLength->setRange(0, 10000);
    shortLength->setSuffix(" characters");
    shortLength->setValue(input_->shortMessageLength());
    shortLength->setToolTip("A typed message of one line up to this length is sent with Enter; 0 never sends typed text. "
                            "Recalled and pasted messages are sent with Enter as long as they are unchanged.");
    enterForm->addRow("Short message:", shortLength);
    auto *undoAfterSend = new QCheckBox("Ctrl+Z after sending brings the sent message back", &dialog);
    undoAfterSend->setObjectName("undoAfterSend");
    undoAfterSend->setChecked(undoAfterSend_);
    enterForm->addRow(QString(), undoAfterSend);
    const auto updateShortLength = [enterInput, shortLength] {
        shortLength->setEnabled(enterInput->currentData().toInt() == int(MessageInput::EnterPolicy::Smart));
    };
    connect(enterInput, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, updateShortLength);
    updateShortLength();
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
        // GLM models are entered by name.
        if (listed == glm_) {
            model->setEditable(true);
            model->setToolTip("Choose a model or type the name of a Z.AI model; typed names are added to the GLM chats' list");
        }
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
    input_->setShortMessageLength(shortLength->value());
    undoAfterSend_ = undoAfterSend->isChecked();
    for (const Row &row : rows) {
        QString model = row.model->currentData().toString();
        if (row.model->isEditable()) {
            const int index = row.model->findText(row.model->currentText());
            model = index >= 0 ? row.model->itemData(index).toString() : row.model->currentText().trimmed();
        }
        if (row.provider == glm_ && !model.isEmpty() && !glm_->extraModels().contains(model))
            glm_->setExtraModels(glm_->extraModels() << model);
        row.provider->setDefaults(model, row.effort->currentData().toString());
    }
    saveSettings();
    modelControlsState_.clear();
    updateModelControls();
}

void MainWindow::showMuteState()
{
    const bool muted = muteSounds_->isChecked();
    muteSounds_->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    muteSounds_->setToolTip(muted ? "Sounds are off; click to turn them on" : "Sounds are on; click to turn them off");
}

// Sounds and desktop notifications for long turns and for agents that wait for an answer.
void MainWindow::showNotificationsDialog()
{
    Notifier::Settings settings = notifier_->settings();
    QDialog dialog(this);
    dialog.setWindowTitle("Notifications");
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *popups = new QCheckBox("Show desktop notifications", &dialog);
    popups->setChecked(settings.popups);
    form->addRow(QString(), popups);
    auto *minimum = new QSpinBox(&dialog);
    minimum->setRange(0, 600);
    minimum->setSuffix(" min");
    minimum->setValue(settings.minimumMinutes);
    minimum->setToolTip("Finished and failed turns notify only when they took at least this long; 0 notifies every turn");
    form->addRow("Notify turns longer than:", minimum);
    const auto soundRow = [this, &dialog, form](const QString &label, const QString &file) {
        auto *row = new QHBoxLayout;
        auto *path = new QLineEdit(file, &dialog);
        path->setPlaceholderText("No sound");
        auto *browse = new QPushButton("Browse…", &dialog);
        auto *play = new QPushButton("Play", &dialog);
        row->addWidget(path, 1);
        row->addWidget(browse);
        row->addWidget(play);
        form->addRow(label, row);
        connect(browse, &QPushButton::clicked, &dialog, [this, path] {
            const QString chosen = QxFileDialog::getOpenFileName(this, "Choose a sound", path->text(),
                                                                 "Sounds (*.wav *.mp3 *.ogg *.oga *.flac)");
            if (!chosen.isEmpty()) path->setText(chosen);
        });
        connect(play, &QPushButton::clicked, &dialog, [this, path] {
            if (!Notifier::playSound(path->text()))
                appendLine("[Could not play the sound; install ffplay, mpv, pw-play or paplay, and check the file.]");
        });
        return path;
    };
    QLineEdit *finished = soundRow("Turn finished:", settings.finishedSound);
    QLineEdit *failed = soundRow("Turn failed:", settings.failedSound);
    QLineEdit *waiting = soundRow("Agent waits for you:", settings.waitingSound);
    auto *delay = new QSpinBox(&dialog);
    delay->setRange(0, 3600);
    delay->setSuffix(" s");
    delay->setValue(settings.waitingDelaySeconds);
    delay->setToolTip("An approval or question is announced after this delay, so answering at once stays quiet");
    form->addRow("Announce a waiting agent after:", delay);
    auto *repeat = new QSpinBox(&dialog);
    repeat->setRange(0, 600);
    repeat->setSuffix(" min");
    repeat->setSpecialValueText("Never");
    repeat->setValue(settings.waitingRepeatMinutes);
    form->addRow("Repeat while it waits, every:", repeat);
    auto *voice = new QCheckBox("Announce with a voice instead of the sound files", &dialog);
    voice->setObjectName("announceWithVoice");
    voice->setChecked(settings.voice);
    form->addRow(QString(), voice);
    auto *voiceRow = new QHBoxLayout;
    auto *voiceChoice = new QComboBox(&dialog);
    voiceChoice->setObjectName("voiceChoice");
    voiceChoice->addItem("Automatic (the first voice found)", QString());
    if (!Notifier::findPiper({}).isEmpty()) {
        for (const QString &model : Notifier::piperModels()) {
            voiceChoice->addItem(Notifier::voiceLabel(model), model);
            voiceChoice->setItemData(voiceChoice->count() - 1, model, Qt::ToolTipRole);
        }
    }
    if (!Notifier::findEspeak().isEmpty()) voiceChoice->addItem("English — espeak-ng", QStringLiteral("espeak-ng"));
    const QString chosenVoice = settings.voiceEngine == "espeak-ng" ? QStringLiteral("espeak-ng") : settings.piperModel;
    voiceChoice->setCurrentIndex(qMax(0, voiceChoice->findData(chosenVoice)));
    auto *tryVoice = new QPushButton("Try", &dialog);
    voiceRow->addWidget(voiceChoice, 1);
    voiceRow->addWidget(tryVoice);
    form->addRow("Voice:", voiceRow);
    auto *slowness = new QDoubleSpinBox(&dialog);
    slowness->setObjectName("speechSlowness");
    slowness->setRange(0.5, 3.0);
    slowness->setSingleStep(0.1);
    slowness->setDecimals(1);
    slowness->setValue(settings.speechSlowness);
    slowness->setToolTip("1.0 is the voice's normal pace; higher values speak slower, which is usually clearer");
    form->addRow("Speech slowness:", slowness);
    const QString piperProgram = Notifier::findPiper({});
    auto *voiceStatus = new QLabel(&dialog);
    voiceStatus->setWordWrap(true);
    voiceStatus->setText(piperProgram.isEmpty() && Notifier::findEspeak().isEmpty()
        ? "No voice program found. Install Piper with a voice (a model .onnx with its .onnx.json in ~/piper or "
          "~/.local/share/piper) or espeak-ng; until then the sound files play."
        : "Found: " + QStringList{piperProgram.isEmpty() ? QString() : "Piper (" + piperProgram + ") with "
                                      + QString::number(Notifier::piperModels().size()) + " voices",
                                  Notifier::findEspeak().isEmpty() ? QString() : "espeak-ng"}.filter(QRegularExpression(".")).join(", ")
          + ". A Polish Piper voice (pl_PL-…) speaks Polish sentences, other voices English.");
    form->addRow(QString(), voiceStatus);
    const auto voiceSettings = [voiceChoice, slowness](Notifier::Settings base) {
        base.speechSlowness = slowness->value();
        const QString data = voiceChoice->currentData().toString();
        base.voiceEngine = data.isEmpty() ? QString() : (data == "espeak-ng" ? QStringLiteral("espeak-ng") : QStringLiteral("piper"));
        base.piperModel = data == "espeak-ng" ? QString() : data;
        return base;
    };
    connect(tryVoice, &QPushButton::clicked, &dialog, [this, voiceSettings, settings] {
        const Notifier::Settings chosen = voiceSettings(settings);
        const bool polish = Notifier::voiceEngine(chosen) == "piper"
            && QFileInfo(Notifier::findPiperModel(chosen)).fileName().startsWith("pl");
        if (!Notifier::say(chosen, polish ? "Codex skończył: przykładowa rozmowa" : "Codex finished: an example chat", this))
            appendLine("[No voice program was found for this choice.]");
    });
    auto *note = new QLabel("A waiting agent is announced regardless of how long the turn has run, because its work "
                            "stops until you answer. The speaker button in the status row mutes all sounds.", &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(640, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;
    settings.popups = popups->isChecked();
    settings.minimumMinutes = minimum->value();
    settings.finishedSound = finished->text().trimmed();
    settings.failedSound = failed->text().trimmed();
    settings.waitingSound = waiting->text().trimmed();
    settings.waitingDelaySeconds = delay->value();
    settings.waitingRepeatMinutes = repeat->value();
    settings.voice = voice->isChecked();
    settings = voiceSettings(settings);
    notifier_->setSettings(settings);
    saveSettings();
}

// Lists lasting "always" rules and the approvals given for the session in open chats, and removes
// the selected ones where the agent allows it.
void MainWindow::showApprovalsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Approvals");
    dialog.resize(760, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tree = new QTreeWidget(&dialog);
    tree->setObjectName("approvalsTree");
    tree->setHeaderLabels({"Agent", "Allowed", "Where"});
    tree->setRootIsDecorated(true);
    layout->addWidget(tree);
    auto *note = new QLabel("Chat command trust is withdrawn immediately. Codex may keep using a removed lasting rule "
                            "until its App Server restarts. Codex cannot "
                            "withdraw native App Server session approvals; they end when agentdeskt closes. Withdrawing "
                            "the session approvals of a Claude or GLM chat reconnects it and withdraws all of them.",
                            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton *remove = buttons->addButton("Remove", QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QList<ApprovalRule> rules;
    const auto fill = [this, tree, &rules] {
        tree->clear();
        QStringList projects{workingDirectory_};
        projects += recentDirectories_;
        for (int i = 0; i < tabs_->count(); ++i) {
            if (const ChatTab *tab = chatTab(tabs_->widget(i))) projects.append(tab->workingDirectory());
        }
        rules = codexRules() + claudeRules(projects);
        auto *lasting = new QTreeWidgetItem(tree, {"Always allowed"});
        for (int i = 0; i < rules.size(); ++i) {
            auto *item = new QTreeWidgetItem(lasting, {rules.at(i).agent, rules.at(i).rule, rules.at(i).where});
            item->setData(0, Qt::UserRole, i);
            item->setToolTip(1, rules.at(i).rule);
            item->setToolTip(2, rules.at(i).file);
        }
        auto *session = new QTreeWidgetItem(tree, {"Allowed for this session"});
        for (int i = 0; i < tabs_->count(); ++i) {
            const ChatTab *tab = chatTab(tabs_->widget(i));
            if (!tab) continue;
            for (const QString &rule : tab->agent()->trustedSessionCommands()) {
                auto *item = new QTreeWidgetItem(session, {tab->provider()->name(), rule,
                    "chat \"" + tab->title().left(40) + "\" (managed by agentdeskt)"});
                item->setData(0, Qt::UserRole + 1, QVariant::fromValue<QObject *>(tabs_->widget(i)));
                item->setData(0, Qt::UserRole + 2, rule);
            }
            for (const QString &approval : tab->sessionApprovals()) {
                auto *item = new QTreeWidgetItem(session, {tab->provider()->name(), approval,
                                                           "chat \"" + tab->title().left(40) + "\""});
                item->setData(0, Qt::UserRole + 1, QVariant::fromValue<QObject *>(tabs_->widget(i)));
                item->setToolTip(1, approval);
            }
        }
        for (QTreeWidgetItem *group : {lasting, session}) {
            group->setExpanded(true);
            group->setFirstColumnSpanned(true);
            if (group->childCount() == 0) new QTreeWidgetItem(group, {QString(), "(none)"});
        }
        tree->resizeColumnToContents(0);
        tree->setColumnWidth(1, 380);
    };
    fill();
    const auto updateRemove = [tree, remove] {
        const QTreeWidgetItem *item = tree->currentItem();
        remove->setEnabled(item && (item->data(0, Qt::UserRole).isValid() || item->data(0, Qt::UserRole + 1).isValid()));
    };
    connect(tree, &QTreeWidget::currentItemChanged, &dialog, updateRemove);
    updateRemove();
    connect(remove, &QPushButton::clicked, &dialog, [this, tree, &rules, fill, updateRemove] {
        const QTreeWidgetItem *item = tree->currentItem();
        if (!item) return;
        if (item->data(0, Qt::UserRole).isValid()) {
            const ApprovalRule rule = rules.value(item->data(0, Qt::UserRole).toInt());
            const QString error = removeApprovalRule(rule);
            appendLine(error.isEmpty() ? "[Removed the " + rule.agent + " rule allowing " + rule.rule + "]"
                                       : "[Could not remove the rule: " + error + "]");
        } else if (auto *page = qobject_cast<QWidget *>(item->data(0, Qt::UserRole + 1).value<QObject *>())) {
            ChatTab *tab = tabs_->indexOf(page) >= 0 ? chatTab(page) : nullptr;
            if (tab && item->data(0, Qt::UserRole + 2).isValid()) {
                tab->agent()->removeTrustedSessionCommand(item->data(0, Qt::UserRole + 2).toString());
            } else if (tab && !tab->agent()->canResetSessionApprovals()) {
                appendLine("[" + tab->provider()->name() + " cannot withdraw approvals given for a session.]");
            } else if (tab && tab->agent()->isResponding()) {
                appendLine("[Wait for " + tab->provider()->name() + " to finish before withdrawing its session approvals.]");
            } else if (tab) {
                tab->resetSessionApprovals();
            }
        }
        fill();
        updateRemove();
    });
    dialog.exec();
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
        // Restored chats must be reachable even before the provider finishes its discovery.
        QSet<QString> ids;
        for (const QJsonObject &chat : chats) ids.insert(chat.value("id").toString());
        for (int i = 0; i < tabs_->count(); ++i) {
            const ChatTab *tab = chatTab(tabs_->widget(i));
            if (!tab || tab->provider() != listed || tab->conversationId().isEmpty()
                || ids.contains(tab->conversationId())) continue;
            chats.append({{"id", tab->conversationId()}, {"cwd", tab->workingDirectory()},
                          {"title", tab->title()}, {"tooltip", tab->headerText()}});
            ids.insert(tab->conversationId());
        }
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
    revealCurrentConversation(false);
}

void MainWindow::revealCurrentConversation(bool expandBranch, bool focusTree)
{
    const ChatTab *tab = currentTab();
    if (!tab) return;
    const QString name = tab->provider()->name();
    if (tab->conversationId().isEmpty() && !expandedProviders_.contains(name)) return;
    if (expandBranch && !expandedProviders_.contains(name)) {
        expandedProviders_.insert(name);
        refreshConversationTree();
    }
    // Selecting the matching row must not create a preview tab or reload its history.
    const QSignalBlocker blocker(conversationTree_);
    for (int i = 0; i < conversationTree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *root = conversationTree_->topLevelItem(i);
        if (root->text(0) != name || !root->isExpanded()) continue;
        for (int j = 0; j < root->childCount(); ++j) {
            QTreeWidgetItem *directory = root->child(j);
            QTreeWidgetItem *match = nullptr;
            if (tab->conversationId().isEmpty() && directory->toolTip(0) == tab->workingDirectory())
                match = directory;
            for (int k = 0; k < directory->childCount() && !match; ++k) {
                QTreeWidgetItem *item = directory->child(k);
                if (item->data(0, Qt::UserRole + 1).toString() == tab->conversationId()) match = item;
            }
            if (!match) continue;
            if (expandBranch) directory->setExpanded(true);
            conversationTree_->setCurrentItem(match);
            conversationTree_->scrollToItem(match, QAbstractItemView::EnsureVisible);
            if (focusTree) conversationTree_->setFocus(Qt::OtherFocusReason);
            return;
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
    auto *tab = new ChatTab(selected, workingDirectory, page);
    tab->setTurnLocks(turnLocks_);
    tab->document()->setDefaultFont(chatView_->font());
    connect(tab, &ChatTab::logMessage, this, &MainWindow::appendLine);
    connect(tab, &ChatTab::steeringFailed, this, [this, tab](const QString &text) {
        if (currentTab() == tab && input_->toPlainText().isEmpty()) input_->replaceText(text);
    });
    connect(tab, &ChatTab::changed, this, [this, page] { updateTab(page); });
    connect(tab, &ChatTab::textAppended, this, [this, page] {
        if (tabs_->currentWidget() != page) {
            tabs_->setTabAttention(page, true);
            return;
        }
        chatView_->refreshTools();
        chatView_->moveCursor(QTextCursor::End);
        chatView_->ensureCursorVisible();
    });
    // A tab in the background that waits for an answer is marked until the user switches to it.
    connect(tab, &ChatTab::requestsChanged, this, [this, page, tab] {
        if (tabs_->currentWidget() == page) updateRequestPanel();
        else if (tab->pendingRequest()) tabs_->setTabAttention(page, true);
        // An agent waiting for an answer is announced even at the current tab: the user may be away.
        const QString key = QString::number(quintptr(page));
        const PendingRequest *request = tab->pendingRequest();
        if (!request) {
            notifier_->waitingEnded(key);
            return;
        }
        const QString what = request->approval ? request->description.section('\n', 0, 0).trimmed()
                                               : request->questions.value(request->current).text;
        notifier_->waitingStarted(key, tab->provider()->name(), tab->title().left(60), what);
    });
    connect(tab, &ChatTab::turnEnded, this, [this, tab](bool succeeded, qint64 durationMs) {
        notifier_->turnFinished(tab->provider()->name(), tab->title().left(60), succeeded, durationMs);
    });
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
            tab->document()->setDefaultFont(chatView_->font());
            chatView_->refreshTools();
            chatView_->moveCursor(QTextCursor::End);
            chatView_->ensureCursorVisible();
        }
    }
    revealCurrentConversation(true);
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
    tabs_->setTabBusy(page, tab->agent()->isResponding() || tab->isWaiting());
    if (page == tabs_->currentWidget()) {
        revealCurrentConversation(false);
        updateStatus();
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

void MainWindow::updateOperationTime()
{
    const ChatTab *tab = currentTab();
    operationTime_->setText(tab ? tab->operationTimeText() : "Time 00:00");
}

void MainWindow::updateStatus()
{
    updateOperationTime();
    updateSteerButton();
    const ChatTab *tab = currentTab();
    if (!tab) {
        status_->setText("No chat open");
        status_->setToolTip({});
        chatHeader_->clear();
        tokens_->clear();
        compactionPanel_->hide();
        compactButton_->setEnabled(false);
        contextTokens_->clear();
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
    status_->setToolTip(status_->text());
    chatHeader_->setText(tab->headerText());
    const TokenUsage turn = tab->lastTurnUsage();
    const TokenUsage conversation = tab->conversationUsage();
    compactionPanel_->setVisible(agent->supportsCompaction());
    compactButton_->setEnabled(tab->isLive() && !tab->isWaiting() && agent->canCompact());
    contextTokens_->setText(groupedTokens(conversation.contextUsed));
    QString contextTip = "Current context tokens reported by App Server";
    if (conversation.contextWindow > 0)
        contextTip += "; context window: " + groupedTokens(conversation.contextWindow) + " tokens";
    contextTokens_->setToolTip(contextTip);
    QStringList usage;
    if (!turn.isEmpty()) usage.append("turn " + ChatTab::shortUsage(turn));
    if (!conversation.isEmpty()) usage.append("chat " + ChatTab::shortUsage(conversation));
    tokens_->setText(usage.join("  "));
    QStringList usageTip{"Tokens in" + QString(QChar(0x2192)) + "out"};
    if (!turn.isEmpty()) usageTip.append("Latest turn: " + ChatTab::usageDetails(turn));
    if (!conversation.isEmpty())
        usageTip.append((tab->conversationUsageIsComplete() ? "Conversation: " : "This session in the tab: ")
                        + ChatTab::usageDetails(conversation));
    tokens_->setToolTip(usage.isEmpty() ? QString() : usageTip.join('\n'));
    chatHeader_->setToolTip(tab->conversationId().isEmpty() ? tab->headerText()
                                                             : tab->headerText() + "\n" + tab->conversationId());
    loadEarlierButton_->setVisible(tab->hasMoreHistory());
    // Reloading a longer tail while a live response streams would drop the partial answer.
    loadEarlierButton_->setEnabled(!(tab->isLive() && agent->isResponding()));
    stopButton_->setEnabled(agent->canInterrupt() || tab->isWaiting());
    sendButton_->setText("Send to " + tab->provider()->name());
    sendButton_->setEnabled(tab->isLive() && !agent->isCompacting());
    const PendingRequest *request = tab->pendingRequest();
    if (request && !request->approval)
        input_->setPlaceholderText("Answer the question above: an option number or your own words");
    else input_->setPlaceholderText(tab->isLive() ? "Message or help, then Ctrl+Enter to send"
                                             : "Read-only preview. Type help, new or clear, then press Enter");
    updateModelControls();
    updateUsage();
    updateRequestPanel();
}

// Rebuilds the panel for the current tab's first waiting request. Options can be chosen here or by
// typing their numbers in the message field, which also takes an answer in the user's own words.
void MainWindow::updateRequestPanel()
{
    ChatTab *tab = currentTab();
    const PendingRequest *request = tab ? tab->pendingRequest() : nullptr;
    const QString state = request ? QString("%1/%2/%3/%4").arg(quintptr(tab)).arg(request->id).arg(request->current)
                                        .arg(tab->pendingRequestCount())
                                  : QString();
    if (state == requestPanelState_) return;
    requestPanelState_ = state;
    auto *layout = static_cast<QVBoxLayout *>(requestPanel_->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) widget->deleteLater();
        if (QLayout *child = item->layout()) {
            while (QLayoutItem *inner = child->takeAt(0)) {
                if (inner->widget()) inner->widget()->deleteLater();
                delete inner;
            }
        }
        delete item;
    }
    requestPanel_->setVisible(request);
    if (!request) return;
    auto *title = new QLabel(requestPanel_);
    title->setWordWrap(true);
    auto *text = new QLabel(requestPanel_);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    layout->addWidget(text);
    const QString waiting = tab->pendingRequestCount() > 1
        ? QString("  (%1 more waiting)").arg(tab->pendingRequestCount() - 1) : QString();
    if (request->approval) {
        title->setText("<b>" + request->title.toHtmlEscaped() + "</b>" + waiting.toHtmlEscaped());
        text->setText(request->alwaysRule.isEmpty() ? request->description
                                                    : request->description + "\n\nAlways allow: " + request->alwaysRule);
        // The row joins the panel before its buttons, so they are shown in the already visible panel.
        auto *buttons = new QHBoxLayout;
        layout->addLayout(buttons);
        const auto addButton = [this, buttons, tab](const QString &label, ApprovalDecision decision, const QString &tip) {
            auto *button = new QPushButton(label, requestPanel_);
            button->setToolTip(tip);
            buttons->addWidget(button);
            connect(button, &QPushButton::clicked, this, [tab, decision] { tab->answerApproval(decision); });
        };
        addButton("Allow once", ApprovalDecision::Accept, "Allow only this action");
        if (request->canAcceptForSession)
            addButton(request->sessionRule.isEmpty() ? "Allow for this session"
                      : "Trust " + request->sessionRule + " for this chat", ApprovalDecision::AcceptForSession,
                      request->sessionRule.isEmpty() ? "Also allow the same kind of action for the rest of this session"
                      : "Trust simple " + request->sessionRule + " commands with different arguments; revoke in Settings > Approvals");
        if (!request->alwaysRule.isEmpty())
            addButton("Always allow", ApprovalDecision::AcceptAlways, request->alwaysRule);
        addButton("Decline", ApprovalDecision::Decline, "The agent continues without this action");
        addButton("Decline and stop", ApprovalDecision::Cancel, "Decline and end the agent's turn");
        buttons->addStretch(1);
        return;
    }
    const AgentQuestion &question = request->questions.at(request->current);
    QString heading = question.header.toHtmlEscaped();
    if (request->questions.size() > 1)
        heading += QString(" (question %1 of %2)").arg(request->current + 1).arg(request->questions.size());
    title->setText("<b>" + heading + "</b>" + waiting.toHtmlEscaped());
    text->setText(question.text);
    auto *group = new QButtonGroup(requestPanel_);
    group->setExclusive(!question.multiSelect);
    for (int i = 0; i < question.options.size(); ++i) {
        const QString description = question.optionDescriptions.value(i);
        const QString label = QString("%1. %2").arg(i + 1).arg(question.options.at(i))
            + (description.isEmpty() ? QString() : " — " + description);
        QAbstractButton *option = question.multiSelect ? static_cast<QAbstractButton *>(new QCheckBox(label, requestPanel_))
                                                       : new QRadioButton(label, requestPanel_);
        group->addButton(option, i);
        layout->addWidget(option);
    }
    QLineEdit *secret = nullptr;
    if (question.secret) {
        secret = new QLineEdit(requestPanel_);
        secret->setEchoMode(QLineEdit::Password);
        secret->setPlaceholderText("Hidden answer");
        layout->addWidget(secret);
    } else {
        QString hint;
        if (question.options.isEmpty()) hint = "Type the answer in the message field and send it.";
        else if (question.multiSelect) hint = "Tick the options, or type their numbers in the message field (for example 1, 3).";
        else hint = "Choose an option, or type its number in the message field.";
        if (question.allowOther && !question.options.isEmpty()) hint += " You can also type your own answer there.";
        auto *hintLabel = new QLabel(hint, requestPanel_);
        hintLabel->setWordWrap(true);
        layout->addWidget(hintLabel);
    }
    auto *buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    auto *answer = new QPushButton("Answer", requestPanel_);
    answer->setVisible(secret || !question.options.isEmpty());
    auto *skip = new QPushButton(request->questions.size() > 1 ? "Skip the questions" : "Skip", requestPanel_);
    skip->setToolTip("Leave the question unanswered; the agent continues without the answer");
    buttons->addWidget(answer);
    buttons->addWidget(skip);
    buttons->addStretch(1);
    const QStringList options = question.options;
    connect(answer, &QPushButton::clicked, this, [tab, group, secret, options] {
        QStringList values;
        if (secret) values.append(secret->text());
        for (QAbstractButton *option : group->buttons()) {
            if (option->isChecked()) values.append(options.at(group->id(option)));
        }
        if (!values.isEmpty()) tab->answerQuestion(values);
    });
    if (secret) connect(secret, &QLineEdit::returnPressed, answer, &QPushButton::click);
    connect(skip, &QPushButton::clicked, this, [tab] { tab->skipQuestions(); });
}

// A colored badge next to Send tells what Enter does now; the tooltip names the key for the other action.
void MainWindow::showEnterAction(bool sends)
{
    const int size = fontMetrics().height() + 8;
    const qreal ratio = devicePixelRatioF();
    QPixmap badge(QSize(size, size) * ratio);
    badge.setDevicePixelRatio(ratio);
    badge.fill(Qt::transparent);
    QPainter painter(&badge);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(sends ? QColor(0x2e, 0x7d, 0x32) : QColor(0x54, 0x6e, 0x7a));
    painter.drawRoundedRect(QRectF(0, 0, size, size), 4, 4);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(size * 2 / 3);
    painter.setFont(font);
    painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, sends ? QString(QChar(0x27A4)) : QString(QChar(0x21B5)));
    painter.end();
    enterIndicator_->setPixmap(badge);
    enterIndicator_->setToolTip(sends ? "Enter sends this message; Shift+Enter starts a new line"
                                      : "Enter starts a new line; Ctrl+Enter sends this message");
    enterIndicator_->setAccessibleName(sends ? "Enter sends" : "Enter starts a new line");
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
    const bool canReadOnly = tab && tab->isLive() && tab->agent()->supportsReadOnly();
    readOnlyInput_->setEnabled(canReadOnly);
    readOnlyInput_->setChecked(tab && tab->agent()->isReadOnly());
    readOnlyInput_->setToolTip(!tab || !tab->agent()->supportsReadOnly()
        ? (tab ? tab->provider()->name() + " does not offer a read-only mode" : QString())
        : tab->agent()->readOnlyIsEnforced()
        ? "From the next message on, the agent may read files but not change them. Codex enforces this with its "
          "read-only sandbox, which also covers shell commands, so these turns do not wait for the directory."
        : "From the next message on, the agent works in its plan mode: it reads and plans but is told not to change "
          "files. Nothing outside the agent enforces this, so these turns still wait for the directory.");
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
