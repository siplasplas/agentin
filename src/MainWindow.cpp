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
#include "UsageLimitsPanel.h"

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
#include <QActionGroup>
#include <QMenu>
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
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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
    setWindowTitle("agentin — Codex, Claude, GLM, Gemini and Antigravity");
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

    auto *viewMenu = menuBar()->addMenu("View");
    usageVisibleAction_ = viewMenu->addAction("Provider limits");
    usageVisibleAction_->setObjectName("showUsageLimits");
    usageVisibleAction_->setCheckable(true);
    usageVisibleAction_->setChecked(true);
    reasoningVisibleAction_ = viewMenu->addAction("Reasoning");
    reasoningVisibleAction_->setObjectName("showReasoning");
    reasoningVisibleAction_->setCheckable(true);
    toolsVisibleAction_ = viewMenu->addAction("Tool calls");
    toolsVisibleAction_->setObjectName("showTools");
    toolsVisibleAction_->setCheckable(true);
    toolsVisibleAction_->setChecked(true);
    toolsVisibleAction_->setToolTip("Unchecked, the chat shows only your messages and the answers");
    // The same menus open from the buttons above the conversation tree.
    auto *treeMenu = viewMenu->addMenu("Conversation tree");
    const auto choice = [this](QMenu *menu, QActionGroup *group, const QString &text, const QString &name) {
        QAction *action = menu->addAction(text);
        action->setObjectName(name);
        action->setCheckable(true);
        group->addAction(action);
        return action;
    };
    auto *groupMenu = new QMenu("Group", this);
    auto *grouping = new QActionGroup(this);
    groupByAgentAction_ = choice(groupMenu, grouping, "By agent, then directory", "groupByAgent");
    groupByDirectoryAction_ = choice(groupMenu, grouping, "By directory, agent in each chat", "groupByDirectory");
    auto *sortMenu = new QMenu("Sort", this);
    sortMenu->addSection("Chats");
    auto *chatOrder = new QActionGroup(this);
    chatsByCreatedAction_ = choice(sortMenu, chatOrder, "Newest created first", "sortChatsByCreated");
    chatsByModifiedAction_ = choice(sortMenu, chatOrder, "Most recently changed first", "sortChatsByModified");
    sortMenu->addSection("Directories");
    auto *directoryOrder = new QActionGroup(this);
    directoriesByNameAction_ = choice(sortMenu, directoryOrder, "By path", "sortDirectoriesByName");
    directoriesByRecentAction_ = choice(sortMenu, directoryOrder, "By their first chat", "sortDirectoriesByRecent");
    for (QActionGroup *group : {grouping, chatOrder, directoryOrder})
        connect(group, &QActionGroup::triggered, this, &MainWindow::applyTreeOptions);
    treeMenu->addMenu(groupMenu);
    treeMenu->addMenu(sortMenu);
    treeMenu->addSeparator();
    for (int level = 1; level <= 3; ++level) {
        treeLevelActions_[level - 1] = treeMenu->addAction(QString());
        treeLevelActions_[level - 1]->setObjectName(QString("treeLevel%1").arg(level));
        connect(treeLevelActions_[level - 1], &QAction::triggered, this, [this, level] { setTreeLevel(level); });
    }


    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    newChatButton_ = new QPushButton("New chat…", central);
    newChatButton_->setObjectName("newChatButton");
    // Long status texts are cut off instead of widening the window; the tooltip keeps the full text.
    status_ = new QLabel(central);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->setMinimumWidth(0);
    usage_ = new UsageLimitsPanel(central);
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
    fastInput_ = new QCheckBox("Fast", this);
    fastInput_->setObjectName("fastToggle");
    fastInput_->setToolTip("Faster model responses with higher usage of limits, where supported. "
                           "Applies from the next turn; always off after restarting the application.");
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
    sendButton_->setToolTip("Send the message; during a running turn that takes steering, it waits for the next turn, "
                            "while Enter steers the running one");
    steerButton_ = new QPushButton("Steer", central);
    steerButton_->setObjectName("steerButton");
    steerButton_->setToolTip("Send this message to the running turn, as Enter does while it runs; "
                             "Send queues it for the next turn");
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
    new TailFollower(chatView_, "chatNewTextButton");
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
    contextTokens_->setFixedWidth(contextTokens_->fontMetrics().horizontalAdvance("9'999'999") + 20);
    contextTokens_->setToolTip("Current context tokens reported by App Server; updated after compaction");
    compactionRow->addWidget(compactButton_);
    compactionRow->addWidget(contextTokens_);
    compactionRow->addWidget(new QLabel("tokens", compactionPanel_));
    compactionRow->addWidget(fastInput_);
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
    muteSounds_ = new QToolButton(central);
    muteSounds_->setObjectName("muteSounds");
    muteSounds_->setCheckable(true);
    muteSounds_->setAutoRaise(true);
    statusRow->addWidget(muteSounds_);
    statusRow->addWidget(newChatButton_);
    layout->addLayout(statusRow);
    usageSplitter_ = new QSplitter(Qt::Vertical, central);
    usageSplitter_->setObjectName("usageSplitter");
    usageSplitter_->setHandleWidth(6);
    usageSplitter_->addWidget(usage_);
    usageSplitter_->addWidget(tabs_);
    usageSplitter_->setChildrenCollapsible(false);
    usageSplitter_->setStretchFactor(0, 0);
    usageSplitter_->setStretchFactor(1, 1);
    usageSplitter_->setSizes({usage_->sizeHint().height(), 600});
    connect(usageSplitter_, &QSplitter::splitterMoved, this, [this] {
        usagePanelHeight_ = usageSplitter_->sizes().value(0);
        saveSettings();
    });
    connect(usageVisibleAction_, &QAction::toggled, this, [this](bool visible) {
        usage_->setVisible(visible);
        updateUsage();
        saveSettings();
    });
    layout->addWidget(usageSplitter_, 1);
    layout->addLayout(inputRow);
    auto *chatSplitter = new QSplitter(Qt::Horizontal, this);
    auto *treePanel = new QWidget(chatSplitter);
    auto *treeLayout = new QVBoxLayout(treePanel);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(0);
    auto *treeTools = new QHBoxLayout;
    treeTools->setContentsMargins(2, 0, 2, 0);
    treeTools->setSpacing(0);
    const auto toolButton = [treePanel](const QString &text, const QString &name) {
        auto *button = new QToolButton(treePanel);
        button->setText(text);
        button->setObjectName(name);
        button->setAutoRaise(true);
        return button;
    };
    treeGroupButton_ = toolButton(QString(), "treeGroupButton");
    treeGroupButton_->setMenu(groupMenu);
    treeGroupButton_->setPopupMode(QToolButton::InstantPopup);
    treeGroupButton_->setToolTip("How chats are grouped");
    auto *sortButton = toolButton("Sort", "treeSortButton");
    sortButton->setMenu(sortMenu);
    sortButton->setPopupMode(QToolButton::InstantPopup);
    sortButton->setToolTip("Order of chats and directories");
    treeTools->addWidget(treeGroupButton_);
    treeTools->addWidget(sortButton);
    treeTools->addStretch(1);
    for (int level = 1; level <= 3; ++level) {
        treeLevelButtons_[level - 1] = toolButton(QString::number(level), QString("treeLevelButton%1").arg(level));
        connect(treeLevelButtons_[level - 1], &QToolButton::clicked, this, [this, level] { setTreeLevel(level); });
        treeTools->addWidget(treeLevelButtons_[level - 1]);
    }
    treeLayout->addLayout(treeTools);
    conversationTree_ = new QTreeWidget(treePanel);
    conversationTree_->setObjectName("conversationTree");
    conversationTree_->setHeaderHidden(true);
    conversationTree_->setMinimumWidth(0);
    treeLayout->addWidget(conversationTree_, 1);
    treePanel->setMinimumWidth(0);
    auto *leftSplitter = new QSplitter(Qt::Vertical, chatSplitter);
    leftSplitter->setObjectName("conversationReasoningSplitter");
    leftSplitter->setHandleWidth(6);
    leftSplitter->addWidget(treePanel);
    reasoning_ = new QPlainTextEdit(leftSplitter);
    new TailFollower(reasoning_, "reasoningNewTextButton");
    reasoning_->setObjectName("reasoningPanel");
    reasoning_->setReadOnly(true);
    reasoning_->setMinimumSize(0, 0);
    reasoning_->setPlaceholderText("Reasoning — no text reported for this conversation");
    reasoning_->setToolTip("Reasoning text or summaries shared by the selected conversation's provider");
    leftSplitter->addWidget(reasoning_);
    leftSplitter->setStretchFactor(0, 1);
    leftSplitter->setStretchFactor(1, 1);
    leftSplitter->setSizes({300, 300});
    reasoning_->hide();
    connect(toolsVisibleAction_, &QAction::toggled, this, [this](bool visible) {
        chatView_->setToolsShown(visible);
        saveSettings();
    });
    connect(reasoningVisibleAction_, &QAction::toggled, this, [this, leftSplitter](bool visible) {
        reasoning_->setVisible(visible);
        if (visible && !leftSplitter->property("reasoningShown").toBool()) {
            const int half = qMax(1, (leftSplitter->height() - leftSplitter->handleWidth()) / 2);
            leftSplitter->setSizes({half, half});
            leftSplitter->setProperty("reasoningShown", true);
        }
        saveSettings();
    });
    chatSplitter->addWidget(leftSplitter);
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
        if (item->data(0, Qt::UserRole).toString() == "directory")
            setDirectoryExpanded(item->data(0, Qt::UserRole + 1).toString(), true);
        if (item->data(0, Qt::UserRole).toString() != "provider") return;
        // Rebuilding the tree deletes item, so read its provider first.
        const QString name = item->text(0);
        expandedProviders_.insert(name);
        refreshConversationTree();
        if (AgentProvider *expanded = provider(name)) expanded->refreshConversations();
    });
    connect(conversationTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toString() == "directory")
            setDirectoryExpanded(item->data(0, Qt::UserRole + 1).toString(), false);
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
    connect(fastInput_, &QCheckBox::toggled, this, [this](bool checked) {
        if (ChatTab *tab = currentTab()) tab->agent()->setFastMode(checked);
    });
    connect(readOnlyInput_, &QCheckBox::clicked, this, [this](bool checked) {
        if (ChatTab *tab = currentTab()) tab->agent()->setReadOnly(checked);
    });
    connect(input_, &MessageInput::submitted, this, &MainWindow::submitFromKeyboard);
    connect(muteSounds_, &QToolButton::clicked, this, [this](bool muted) {
        if (notifier_->isPlaying()) {
            notifier_->stopPlayback();
            return;
        }
        notifier_->setMuted(muted);
        showMuteState();
        saveSettings();
    });
    connect(notifier_, &Notifier::playbackChanged, this, [this] { showMuteState(); });
    connect(notifier_, &Notifier::playbackFailed, this, [this](const QString &reason) {
        appendLine("[Could not play audio: " + reason + "]");
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
        connect(listed, &AgentProvider::usageChanged, this, [this, listed] { logUsageLimits(listed); });
    }

    appendLine("Directory: " + workingDirectory_);
    appendLine("Type help to see the available commands.\n");
    for (AgentProvider *listed : providers_) listed->loadConversations();
    loadSettings();
    showEnterAction(input_->enterSends());
    loadRecentDirectories();
    refreshConversationTree();
    // Without agent rows to expand, all chats are discovered; this waits until the Claude environment is set.
    if (treeByDirectory_) QTimer::singleShot(0, this, &MainWindow::discoverConversations);
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
    // A help process still running is killed with the window; its error must not reach the deleted log.
    for (QProcess *process : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) disconnect(process, nullptr, this, nullptr);
    chatView_->setDocument(emptyDocument_);
    reasoning_->setDocument(emptyDocument_);
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

void MainWindow::useManagedClaudeEnvironment()
{
    auto *environment = new ClaudeEnvironment(QDir(dataDirectory_).filePath("claude-venv"), this);
    connect(environment, &ClaudeEnvironment::message, this, &MainWindow::appendLine);
    claude_->setEnvironment(environment);
    glm_->setEnvironment(environment);
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
        if (!selected || !visibleProviders().contains(selected) || !QFileInfo(directory).isDir()) continue;
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

bool MainWindow::canSteerNow() const
{
    const ChatTab *tab = currentTab();
    return tab && tab->isLive() && !tab->pendingRequest() && tab->agent()->canSteer()
        && !input_->toPlainText().trimmed().isEmpty();
}

// While the running turn can take it, a message sent from the keyboard steers that turn; the Send
// button still queues it for the next turn. Local commands keep their meaning.
void MainWindow::submitFromKeyboard()
{
    static const QStringList commands{"help", "/help", "clear", "/clear", "new", "/new",
                                      "stop", "/stop", "quit", "/quit", "exit"};
    if (canSteerNow() && !commands.contains(input_->toPlainText().trimmed().toLower())) submitSteer();
    else submitCommand();
}

void MainWindow::updateSteerButton()
{
    const ChatTab *tab = currentTab();
    steerButton_->setVisible(tab && tab->agent()->supportsSteering());
    steerButton_->setEnabled(canSteerNow());
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
    appendLine("During a Codex turn, Enter and Steer send the message to that turn; Send queues it for the next turn.\n");
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

QList<AgentProvider *> MainWindow::visibleProviders() const
{
    if (experimentalAgents_) return providers_;
    return {codex_, claude_};
}

void MainWindow::setExperimentalAgentsEnabled(bool enabled)
{
    if (experimentalAgents_ == enabled) return;
    experimentalAgents_ = enabled;
    refreshConversationTree();
    if (enabled && treeByDirectory_) discoverConversations();
}

void MainWindow::discoverConversations()
{
    for (AgentProvider *listed : visibleProviders()) listed->refreshConversations();
}

void MainWindow::applyTreeOptions()
{
    const bool wasByDirectory = treeByDirectory_;
    treeByDirectory_ = groupByDirectoryAction_->isChecked();
    chatsByModified_ = chatsByModifiedAction_->isChecked();
    directoriesByName_ = directoriesByNameAction_->isChecked();
    updateTreeControls();
    saveSettings();
    refreshConversationTree();
    if (treeByDirectory_ && !wasByDirectory) discoverConversations();
}

void MainWindow::updateTreeControls()
{
    (treeByDirectory_ ? groupByDirectoryAction_ : groupByAgentAction_)->setChecked(true);
    (chatsByModified_ ? chatsByModifiedAction_ : chatsByCreatedAction_)->setChecked(true);
    (directoriesByName_ ? directoriesByNameAction_ : directoriesByRecentAction_)->setChecked(true);
    treeGroupButton_->setText(treeByDirectory_ ? "By directory" : "By agent");
    const QStringList levels = treeByDirectory_
        ? QStringList{"Directories only", "Directories and chats", QString()}
        : QStringList{"Agents only", "Agents and directories", "Agents, directories and chats"};
    for (int i = 0; i < 3; ++i) {
        treeLevelActions_[i]->setText(levels.at(i).isEmpty() ? QString("Level %1").arg(i + 1) : "Show " + levels.at(i));
        treeLevelActions_[i]->setEnabled(!levels.at(i).isEmpty());
        treeLevelButtons_[i]->setEnabled(!levels.at(i).isEmpty());
        treeLevelButtons_[i]->setToolTip(levels.at(i).isEmpty() ? QString() : "Show " + levels.at(i));
    }
}

void MainWindow::setTreeLevel(int level)
{
    directoriesCollapsed_ = treeByDirectory_ ? level == 1 : level == 2;
    toggledDirectories_.clear();
    if (treeByDirectory_) {
        refreshConversationTree();
        return;
    }
    const QSet<QString> expandedBefore = expandedProviders_;
    expandedProviders_.clear();
    if (level > 1) {
        for (const AgentProvider *listed : visibleProviders()) expandedProviders_.insert(listed->name());
    }
    refreshConversationTree();
    for (AgentProvider *listed : visibleProviders()) {
        if (expandedProviders_.contains(listed->name()) && !expandedBefore.contains(listed->name()))
            listed->refreshConversations();
    }
}

bool MainWindow::isDirectoryExpanded(const QString &key) const
{
    return directoriesCollapsed_ == toggledDirectories_.contains(key);
}

void MainWindow::setDirectoryExpanded(const QString &key, bool expanded)
{
    if (expanded == directoriesCollapsed_) toggledDirectories_.insert(key);
    else toggledDirectories_.remove(key);
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
    experimentalAgents_ = settings.value("experimentalAgents").toBool(false);
    const QJsonObject tree = settings.value("conversationTree").toObject();
    treeByDirectory_ = tree.value("groupBy").toString() == "directory";
    chatsByModified_ = tree.value("chatOrder").toString() != "created";
    directoriesByName_ = tree.value("directoryOrder").toString() == "path";
    updateTreeControls();
    notifier_->setSettings(Notifier::Settings::fromJson(settings.value("notifications").toObject()));
    lastAudioDirectory_ = settings.value("lastAudioDirectory").toString();
    {
        const QSignalBlocker blocker(usageVisibleAction_);
        const bool visible = settings.value("showUsageLimits").toBool(true);
        usageVisibleAction_->setChecked(visible);
        usage_->setVisible(visible);
    }
    {
        const QSignalBlocker blocker(toolsVisibleAction_);
        const bool visible = settings.value("showTools").toBool(true);
        toolsVisibleAction_->setChecked(visible);
        chatView_->setToolsShown(visible);
    }
    {
        const QSignalBlocker blocker(reasoningVisibleAction_);
        const bool visible = settings.value("showReasoning").toBool(false);
        reasoningVisibleAction_->setChecked(visible);
        reasoning_->setVisible(visible);
    }
    usagePanelHeight_ = qMax(1, settings.value("usagePanelHeight").toInt(usage_->sizeHint().height()));
    usageSplitter_->setSizes({usagePanelHeight_, 600});
    updateUsage();
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
    reasoning_->setFont(font);
    emptyDocument_->setDefaultFont(font);
    for (int i = 0; i < tabs_->count(); ++i) {
        if (ChatTab *tab = chatTab(tabs_->widget(i))) {
            tab->document()->setDefaultFont(font);
            tab->reasoningDocument()->setDefaultFont(font);
        }
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
                                                         {"experimentalAgents", experimentalAgents_},
                                                         {"conversationTree", QJsonObject{
                                                             {"groupBy", treeByDirectory_ ? "directory" : "agent"},
                                                             {"chatOrder", chatsByModified_ ? "modified" : "created"},
                                                             {"directoryOrder", directoriesByName_ ? "path" : "recent"}}},
                                                         {"chatFont", chatView_->font().toString()},
                                                         {"notifications", notifier_->settings().toJson()},
                                                         {"lastAudioDirectory", lastAudioDirectory_},
                                                         {"showUsageLimits", usageVisibleAction_->isChecked()},
                                                         {"showReasoning", reasoningVisibleAction_->isChecked()},
                                                         {"showTools", toolsVisibleAction_->isChecked()},
                                                         {"usagePanelHeight", usagePanelHeight_},
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
    auto *experimental = new QCheckBox("Show experimental agents: GLM, Gemini and Antigravity", &dialog);
    experimental->setObjectName("experimentalAgents");
    experimental->setChecked(experimentalAgents_);
    experimental->setToolTip("Offers these agents in the conversation tree and for new chats. Their default "
                             "models are listed here the next time the options are opened.");
    enterForm->addRow(QString(), experimental);
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
    for (AgentProvider *listed : visibleProviders()) {
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
    setExperimentalAgentsEnabled(experimental->isChecked());
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
    const bool playing = notifier_->isPlaying();
    const bool muted = notifier_->settings().muted;
    const QSignalBlocker blocker(muteSounds_);
    muteSounds_->setCheckable(!playing);
    muteSounds_->setChecked(muted);
    muteSounds_->setStyleSheet(playing ? "QToolButton { background-color: #d32f2f; border-radius: 3px; }" : QString());
    muteSounds_->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    muteSounds_->setToolTip(playing ? "Audio is playing; click to stop playback"
                                  : muted ? "Sounds are off; click to turn them on" : "Sounds are on; click to turn them off");
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
    const auto soundRow = [this, &dialog, form](const QString &label, const QString &file, const QString &key) {
        auto *row = new QHBoxLayout;
        auto *path = new QLineEdit(file, &dialog);
        path->setPlaceholderText("No sound");
        path->setObjectName(key + "Path");
        auto *browse = new QPushButton("Browse…", &dialog);
        browse->setObjectName(key + "Browse");
        auto *play = new QPushButton("Play", &dialog);
        row->addWidget(path, 1);
        row->addWidget(browse);
        row->addWidget(play);
        form->addRow(label, row);
        connect(browse, &QPushButton::clicked, &dialog, [this, path, &dialog] {
            QString directory = lastAudioDirectory_;
            if (!QDir(directory).exists() || directory.isEmpty()) {
                const QFileInfo current(path->text().trimmed());
                directory = current.exists() ? (current.isDir() ? current.absoluteFilePath() : current.absolutePath())
                                             : QDir::homePath();
            }
            QxFileDialog picker(&dialog, QxFileDialog::Open);
            picker.setWindowTitle("Choose a sound");
            picker.setDirectory(directory);
            picker.setNameFilter("Sounds (*.wav *.mp3 *.ogg *.oga *.flac)");
            picker.setAudioDurationVisible(true);
            picker.setFileName(path->text().trimmed());
            const bool accepted = picker.exec() == QDialog::Accepted;
            lastAudioDirectory_ = picker.directory();
            saveSettings();
            if (accepted) path->setText(picker.selectedFile());
        });
        connect(play, &QPushButton::clicked, &dialog, [this, path] {
            if (!notifier_->playSound(path->text()))
                appendLine("[Could not play the sound; install ffplay, mpv, pw-play or paplay, and check the file.]");
        });
        return path;
    };
    QLineEdit *finished = soundRow("Turn finished:", settings.finishedSound, "finishedSound");
    QLineEdit *failed = soundRow("Turn failed:", settings.failedSound, "failedSound");
    QLineEdit *waiting = soundRow("Agent waits for you:", settings.waitingSound, "waitingSound");
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
    auto *approvals = new QCheckBox("Announce approval requests (questions are always announced)", &dialog);
    approvals->setObjectName("announceApprovals");
    approvals->setChecked(settings.announceApprovals);
    form->addRow(QString(), approvals);
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
        if (!notifier_->say(chosen, polish ? "Codex skończył: przykładowa rozmowa" : "Codex finished: an example chat"))
            appendLine("[No voice program was found for this choice.]");
    });
    auto *note = new QLabel("A waiting agent is announced regardless of how long the turn has run, because its work "
                            "stops until you answer. The speaker button turns red during audio playback; click it to stop. "
                            "When idle, it mutes notification sounds.", &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton *stopPlayback = buttons->addButton("Stop playback", QDialogButtonBox::ActionRole);
    stopPlayback->setEnabled(notifier_->isPlaying());
    connect(notifier_, &Notifier::playbackChanged, stopPlayback, &QPushButton::setEnabled);
    connect(stopPlayback, &QPushButton::clicked, notifier_, &Notifier::stopPlayback);
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
    settings.announceApprovals = approvals->isChecked();
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
                            "withdraw native App Server session approvals; they end when agentin closes. Withdrawing "
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
                    "chat \"" + tab->title().left(40) + "\" (managed by agentin)"});
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
    // A directory or chat selected in the tree gives its directory; otherwise the last one chosen here.
    const QTreeWidgetItem *selected = conversationTree_->currentItem();
    QString currentPath = selected ? selected->data(0, Qt::UserRole + 2).toString() : QString();
    if (currentPath.isEmpty()) currentPath = recentDirectories_.value(0);
    if (currentPath.isEmpty()) currentPath = tab ? tab->workingDirectory() : workingDirectory_;
    QDialog dialog(this);
    dialog.setWindowTitle("New conversation");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Agent:", &dialog));
    auto *providerInput = new QComboBox(&dialog);
    providerInput->setObjectName("newConversationProvider");
    const QList<AgentProvider *> offered = visibleProviders();
    for (const AgentProvider *listed : offered) providerInput->addItem(listed->name());
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
        newConversation(offered.value(providerInput->currentIndex()), path);
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
    // The selected agent, directory or chat stays selected when the tree is rebuilt or regrouped.
    QString selectedKind, selectedName, selectedId, selectedPath;
    if (const QTreeWidgetItem *selected = conversationTree_->currentItem()) {
        selectedKind = selected->data(0, Qt::UserRole).toString();
        if (selectedKind == "provider") {
            selectedName = selected->text(0);
        } else if (selectedKind == "directory") {
            selectedName = selected->parent() ? selected->parent()->text(0) : QString();
            selectedPath = selected->data(0, Qt::UserRole + 2).toString();
        } else {
            selectedName = selectedKind;
            selectedKind = "chat";
            selectedId = selected->data(0, Qt::UserRole + 1).toString();
        }
    }
    conversationTree_->clear();
    if (treeByDirectory_) {
        QList<QJsonObject> chats;
        for (AgentProvider *listed : visibleProviders()) chats += treeChats(listed);
        addDirectoryItems(nullptr, chats);
    } else {
        for (AgentProvider *listed : visibleProviders()) {
            const QString name = listed->name();
            auto *root = new QTreeWidgetItem(conversationTree_, {name});
            root->setData(0, Qt::UserRole, "provider");
            root->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            if (!expandedProviders_.contains(name)) continue;
            root->setExpanded(true);
            addDirectoryItems(root, treeChats(listed));
        }
    }
    QTreeWidgetItem *match = nullptr;
    for (QTreeWidgetItemIterator it(conversationTree_); *it && !selectedKind.isEmpty(); ++it) {
        QTreeWidgetItem *item = *it;
        const QString kind = item->data(0, Qt::UserRole).toString();
        if (selectedKind == "provider" ? kind == "provider" && item->text(0) == selectedName
            : selectedKind == "directory" ? kind == "directory" && item->data(0, Qt::UserRole + 2).toString() == selectedPath
            : kind == selectedName && item->data(0, Qt::UserRole + 1).toString() == selectedId) {
            // The same directory under the same agent is preferred over its rows under other agents.
            if (!match || (item->parent() && item->parent()->text(0) == selectedName)) match = item;
            if (selectedKind != "directory") break;
        }
    }
    // A chat under an agent row that is closed, and so has no rows, is represented by that agent.
    for (int i = 0; !match && selectedKind != "provider" && i < conversationTree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = conversationTree_->topLevelItem(i);
        if (top->data(0, Qt::UserRole).toString() == "provider" && top->text(0) == selectedName) match = top;
    }
    if (!match) {
        revealCurrentConversation(false);
        return;
    }
    // A row inside a closed branch is represented by the closed row that contains it.
    for (QTreeWidgetItem *parent = match->parent(); parent; parent = parent->parent())
        if (!parent->isExpanded()) match = parent;
    conversationTree_->setCurrentItem(match);
    conversationTree_->scrollToItem(match);
}

// Restored chats must be reachable even before the provider finishes its discovery. Each chat gets
// its agent and the time it is sorted by; a chat without a modification time sorts by its creation.
QList<QJsonObject> MainWindow::treeChats(AgentProvider *listed) const
{
    QList<QJsonObject> chats = listed->conversations();
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
    for (QJsonObject &chat : chats) {
        const qint64 modified = chat.value("modifiedAt").toInteger();
        chat.insert("sortAt", chatsByModified_ && modified > 0 ? modified : chat.value("createdAt").toInteger());
        chat.insert("provider", listed->name());
    }
    return chats;
}

void MainWindow::addDirectoryItems(QTreeWidgetItem *root, QList<QJsonObject> chats)
{
    std::stable_sort(chats.begin(), chats.end(), [](const QJsonObject &left, const QJsonObject &right) {
        return left.value("sortAt").toInteger() > right.value("sortAt").toInteger();
    });
    // Without sorting by path, a directory comes where its first chat would.
    QStringList paths;
    QHash<QString, QList<QJsonObject>> chatsByPath;
    for (const QJsonObject &chat : chats) {
        const QString path = chat.value("cwd").toString();
        if (!chatsByPath.contains(path)) paths.append(path);
        chatsByPath[path].append(chat);
    }
    if (directoriesByName_) {
        std::stable_sort(paths.begin(), paths.end(), [](const QString &left, const QString &right) {
            if (left.isEmpty() != right.isEmpty()) return right.isEmpty();
            return left.compare(right, Qt::CaseInsensitive) < 0;
        });
    }
    for (const QString &path : paths) {
        const QString label = path.isEmpty() ? "(unknown directory)" : QDir::toNativeSeparators(path);
        auto *directory = root ? new QTreeWidgetItem(root, {label}) : new QTreeWidgetItem(conversationTree_, {label});
        const QString key = (root ? root->text(0) : QString()) + '\n' + path;
        directory->setData(0, Qt::UserRole, "directory");
        directory->setData(0, Qt::UserRole + 1, key);
        directory->setData(0, Qt::UserRole + 2, path);
        directory->setToolTip(0, path);
        for (const QJsonObject &chat : chatsByPath.value(path)) {
            const QString name = chat.value("provider").toString();
            QString title = shortPreview(chat.value("title").toString());
            if (chat.value("archived").toBool()) title += " (archived)";
            if (!root) title = name + ": " + title;
            auto *item = new QTreeWidgetItem(directory, {title});
            item->setData(0, Qt::UserRole, name);
            item->setData(0, Qt::UserRole + 1, chat.value("id").toString());
            item->setData(0, Qt::UserRole + 2, path);
            item->setToolTip(0, chat.value("tooltip").toString());
        }
        directory->setExpanded(isDirectoryExpanded(key));
    }
}

void MainWindow::revealCurrentConversation(bool expandBranch, bool focusTree)
{
    const ChatTab *tab = currentTab();
    if (!tab) return;
    const QString name = tab->provider()->name();
    if (!treeByDirectory_) {
        if (tab->conversationId().isEmpty() && !expandedProviders_.contains(name)) return;
        if (expandBranch && !expandedProviders_.contains(name)) {
            expandedProviders_.insert(name);
            refreshConversationTree();
        }
    }
    // Selecting the matching row must not create a preview tab or reload its history.
    const QSignalBlocker blocker(conversationTree_);
    QList<QTreeWidgetItem *> directories;
    for (int i = 0; i < conversationTree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = conversationTree_->topLevelItem(i);
        if (treeByDirectory_) {
            directories.append(top);
        } else if (top->text(0) == name && top->isExpanded()) {
            for (int j = 0; j < top->childCount(); ++j) directories.append(top->child(j));
        }
    }
    for (QTreeWidgetItem *directory : directories) {
        QTreeWidgetItem *match = nullptr;
        if (tab->conversationId().isEmpty() && directory->data(0, Qt::UserRole + 2).toString() == tab->workingDirectory())
            match = directory;
        for (int k = 0; k < directory->childCount() && !match; ++k) {
            QTreeWidgetItem *item = directory->child(k);
            if (item->data(0, Qt::UserRole).toString() == name
                && item->data(0, Qt::UserRole + 1).toString() == tab->conversationId()) match = item;
        }
        if (!match) continue;
        if (match != directory && !directory->isExpanded()) {
            // Selecting a row scrolls to it, which would open a directory the user closed.
            if (!expandBranch) return;
            setDirectoryExpanded(directory->data(0, Qt::UserRole + 1).toString(), true);
            directory->setExpanded(true);
        }
        conversationTree_->setCurrentItem(match);
        conversationTree_->scrollToItem(match, QAbstractItemView::EnsureVisible);
        if (focusTree) conversationTree_->setFocus(Qt::OtherFocusReason);
        return;
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
    tab->reasoningDocument()->setDefaultFont(chatView_->font());
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
        if (chatRefreshPending_) return;
        chatRefreshPending_ = true;
        QTimer::singleShot(0, this, [this] {
            chatRefreshPending_ = false;
            // Refresh the current document, even if the user changed tabs during the batch.
            chatView_->refreshTools();
        });
    });
    // A tab in the background that waits for an answer is marked until the user switches to it.
    connect(tab, &ChatTab::requestsChanged, this, [this, page, tab] {
        if (tabs_->currentWidget() == page) updateRequestPanel();
        else if (tab->pendingRequest()) tabs_->setTabAttention(page, true);
        // An agent waiting for an answer is announced even at the current tab: the user may be away.
        const QString key = QString::number(quintptr(page));
        const PendingRequest *request = tab->pendingRequest();
        if (!request || (request->approval && !notifier_->settings().announceApprovals)) {
            notifier_->waitingEnded(key);
            return;
        }
        const QString what = request->approval ? request->description.section('\n', 0, 0).trimmed()
                                               : request->questions.value(request->current).text;
        notifier_->waitingStarted(key, request->id, tab->provider()->name(), tab->title().left(60), what);
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
    QTextDocument *reasoningDocument = tab ? tab->reasoningDocument() : emptyDocument_;
    if (reasoning_->document() != reasoningDocument) {
        reasoning_->setDocument(reasoningDocument);
        reasoning_->moveCursor(QTextCursor::End);
    }
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

// Each change of a provider's reported limits is written to the log, so their pace can be followed.
void MainWindow::logUsageLimits(AgentProvider *provider)
{
    QList<UsageLimit> limits = provider->usageLimits();
    std::sort(limits.begin(), limits.end(), [](const UsageLimit &a, const UsageLimit &b) {
        return a.windowMinutes == b.windowMinutes ? a.id < b.id : a.windowMinutes > b.windowMinutes;
    });
    QStringList parts;
    for (const UsageLimit &limit : limits) {
        QString window = limit.windowMinutes == 7 * 24 * 60 ? QString("Week")
            : limit.windowMinutes > 0 && limit.windowMinutes % 60 == 0 ? QString("%1 h").arg(limit.windowMinutes / 60)
            : QString("%1 min").arg(limit.windowMinutes);
        if (!limit.name.isEmpty()) window = limit.name + " " + window;
        QString part = window + " " + (limit.usedPercent < 0 ? QString("usage not reported")
                                                               : QLocale::c().toString(limit.usedPercent, 'g', 4) + "% used");
        if (limit.resetsAt > 0)
            part += ", resets " + QDateTime::fromSecsSinceEpoch(limit.resetsAt).toString("ddd d MMM HH:mm");
        if (!limit.status.isEmpty() && limit.status != "allowed") part += " (" + limit.status + ")";
        parts.append(part);
    }
    const QString summary = parts.join("; ");
    if (summary.isEmpty() || loggedUsage_.value(provider->name()) == summary) return;
    loggedUsage_.insert(provider->name(), summary);
    appendLine("[" + provider->name() + " limits: " + summary + "]");
}

// Account snapshots follow the selected provider; only that provider is asked for fresh limits.
void MainWindow::updateUsage()
{
    const ChatTab *tab = currentTab();
    AgentProvider *selected = tab && usageVisibleAction_->isChecked() ? tab->provider() : nullptr;
    QList<ProviderLimits> limits;
    for (AgentProvider *listed : providers_)
        listed->setUsageLimitsActive(listed == selected);
    if (selected) limits.append({selected->name(), selected->usageLimits()});
    usage_->setLimits(limits);
}

// Shows the current chat's model and effort. Without a choice yet, the provider's default model and
// that model's default effort are shown.
void MainWindow::updateModelControls()
{
    const ChatTab *tab = currentTab();
    const QList<AgentModel> models = tab ? tab->provider()->models() : QList<AgentModel>{};
    {
        const QSignalBlocker fastBlocker(fastInput_);
        const bool supported = tab && tab->agent()->supportsFastMode();
        fastInput_->setVisible(supported);
        fastInput_->setEnabled(supported && tab->isLive());
        fastInput_->setChecked(supported && tab->agent()->isFastMode());
    }
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
