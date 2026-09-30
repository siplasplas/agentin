#include "ChatTab.h"

#include <QDir>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextDocumentLayout>
#include <QTextCursor>
#include <QTextDocument>

ChatTab::ChatTab(AgentProvider *provider, const QString &workingDirectory, QWidget *dialogParent, QObject *parent)
    : QObject(parent), provider_(provider), dialogParent_(dialogParent), document_(new QTextDocument(this)),
      path_(workingDirectory), title_("New chat")
{
    document_->setDocumentLayout(new QPlainTextDocumentLayout(document_));
    setAgent(provider->createChat(workingDirectory, this));
}

QString ChatTab::key(const QString &provider, const QString &id)
{
    return id.isEmpty() ? QString() : provider + '\n' + id;
}

QString ChatTab::key() const
{
    return key(provider_->name(), id_);
}

QString ChatTab::headerText() const
{
    QStringList parts{provider_->name(), title_, QDir::toNativeSeparators(path_)};
    if (!lockNotice_.isEmpty()) parts.append("locked: " + lockNotice_);
    else if (!live_) parts.append("read-only preview");
    return parts.join("  •  ");
}

void ChatTab::startDraft()
{
    live_ = true;
    emit changed();
}

bool ChatTab::startNew(const QString &workingDirectory)
{
    if (!agent_->newConversation(workingDirectory)) return false;
    path_ = workingDirectory;
    id_.clear();
    title_ = "New chat";
    live_ = true;
    liveTranscript_.clear();
    historyMessages_.clear();
    sentMessages_.clear();
    emit userMessagesChanged();
    document_->clear();
    emit changed();
    return true;
}

void ChatTab::showPreview(AgentProvider *provider, const QString &id, const QString &workingDirectory,
                          const QString &title)
{
    if (live_) return;
    if (provider != provider_) {
        provider_ = provider;
        delete agent_;
        setAgent(provider->createChat(workingDirectory, this));
    }
    agent_->cancelHistory();
    id_ = id;
    path_ = workingDirectory;
    title_ = title;
    lockNotice_.clear();
    pendingAttach_ = false;
    hasMore_ = false;
    liveTranscript_.clear();
    historyMessages_.clear();
    sentMessages_.clear();
    emit userMessagesChanged();
    document_->setPlainText("Loading the latest messages…");
    emit changed();
    agent_->loadHistory(id_, path_, false);
}

void ChatTab::continueChat()
{
    if (live_ || id_.isEmpty()) return;
    if (agent_->sessionId() != id_) {
        const QString lock = provider_->externalLock(id_);
        if (!lock.isEmpty()) {
            lockNotice_ = "open in " + lock;
            emit logMessage("[" + provider_->name() + " conversation is open in another tool and stays read-only: "
                            + lock + "]");
            emit changed();
            return;
        }
        lockNotice_.clear();
        pendingAttach_ = true;
        if (!agent_->resumeConversation(id_, path_)) {
            pendingAttach_ = false;
            emit changed();
            return;
        }
        // Codex confirms asynchronously through conversationOpened; the other agents switch at once.
        if (agent_->sessionId() != id_) {
            emit changed();
            return;
        }
        pendingAttach_ = false;
    }
    live_ = true;
    emit changed();
}

bool ChatTab::send(const QString &text)
{
    if (!live_) {
        emit logMessage("[The displayed chat is a read-only preview. Double-click it in the tree to continue it.]");
        return false;
    }
    if (!agent_->prompt(text)) return false;
    if (id_.isEmpty() && title_ == "New chat") {
        title_ = text.simplified().left(200);
        emit changed();
    }
    appendText("\nYou: " + text + '\n');
    sentMessages_.append(text);
    emit userMessagesChanged();
    return true;
}

void ChatTab::loadEarlier()
{
    if (!id_.isEmpty()) agent_->loadHistory(id_, path_, true);
}

void ChatTab::setAgent(AgentBackend *agent)
{
    agent_ = agent;
    const QString name = agent->name();
    connect(agent, &AgentBackend::message, this, &ChatTab::logMessage);
    connect(agent, &AgentBackend::stateChanged, this, &ChatTab::changed);
    connect(agent, &AgentBackend::messageStarted, this, [this, name] { appendText("\n" + name + ": "); });
    connect(agent, &AgentBackend::messageDelta, this, &ChatTab::appendText);
    connect(agent, &AgentBackend::messageFinished, this, [this] { appendText("\n"); });
    connect(agent, &AgentBackend::toolStarted, this, [this, name](const QString &tool, const QString &details) {
        appendText("\n[" + name + " tool: " + tool + "] " + details + '\n');
    });
    connect(agent, &AgentBackend::toolOutput, this, &ChatTab::appendText);
    connect(agent, &AgentBackend::toolFinished, this, [this](const QString &tool, const QString &status) {
        appendText("\n[" + tool + ": " + status + "]\n");
    });
    connect(agent, &AgentBackend::turnCompleted, this, [this, name](const QString &status, const QString &details) {
        if (status != "completed")
            emit logMessage("[" + name + " response: " + status + (details.isEmpty() ? "" : ": " + details) + "]");
    });
    connect(agent, &AgentBackend::approvalRequested, this,
            [this, agent](int id, const QString &title, const QString &description) {
        emit activateRequested();
        const auto answer = QMessageBox::question(dialogParent_, title, description + "\n\nAllow this action?",
                                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        agent->answerApproval(id, answer == QMessageBox::Yes);
    });
    connect(agent, &AgentBackend::questionsRequested, this, [this, agent](int id, const QList<AgentQuestion> &questions) {
        emit activateRequested();
        agent->answerQuestions(id, askQuestions(questions));
    });
    // A resumed conversation becomes live only if this tab still shows the chat it asked for.
    connect(agent, &AgentBackend::conversationOpened, this, [this](const QString &id, bool resumed) {
        if (resumed && pendingAttach_) {
            pendingAttach_ = false;
            if (id != id_) return;
            live_ = true;
            lockNotice_.clear();
        } else if (!resumed && live_) {
            id_ = id;
        }
        emit changed();
    });
    connect(agent, &AgentBackend::conversationOpenFailed, this, [this](const QString &id, const QString &reason) {
        if (!pendingAttach_ || id != id_) return;
        pendingAttach_ = false;
        lockNotice_ = reason;
        emit changed();
    });
    connect(agent, &AgentBackend::historyLoaded, this,
            [this](const QString &id, const QList<ChatEntry> &entries, bool hasMore, const QString &notice) {
        if (id == id_) showHistory(entries, hasMore, notice);
    });
}

// Live output is kept separately so the transcript survives when older history is prepended.
void ChatTab::appendText(const QString &text)
{
    if (!live_) return;
    liveTranscript_ += text;
    QTextCursor cursor(document_);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    emit textAppended();
}

void ChatTab::showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice)
{
    const QString name = provider_->name();
    QStringList blocks;
    historyMessages_.clear();
    for (const ChatEntry &entry : entries) {
        if (entry.role == "user") {
            historyMessages_.append(entry.text);
            blocks.append("You: " + entry.text);
        } else if (entry.role == "tool") {
            blocks.append("[" + name + " tool: " + entry.text + "]");
        } else {
            blocks.append(name + ": " + entry.text);
        }
    }
    if (!notice.isEmpty()) blocks.append("[" + notice + "]");
    else if (blocks.isEmpty()) blocks.append("[This conversation has no messages to show.]");
    document_->setPlainText(blocks.join("\n\n") + '\n' + (live_ ? liveTranscript_ : QString()));
    hasMore_ = hasMore;
    emit userMessagesChanged();
    emit changed();
    emit textAppended();
}

// Asks each question in a dialog and stops at the first one the user cancels.
QHash<QString, QString> ChatTab::askQuestions(const QList<AgentQuestion> &questions)
{
    QHash<QString, QString> answers;
    for (const AgentQuestion &question : questions) {
        bool accepted = false;
        const QString reply = question.multiSelect || question.options.isEmpty()
            ? QInputDialog::getText(dialogParent_, question.header, question.text, QLineEdit::Normal, {}, &accepted)
            : QInputDialog::getItem(dialogParent_, question.header, question.text, question.options, 0, false, &accepted);
        if (!accepted) break;
        answers.insert(question.id, reply);
    }
    return answers;
}
