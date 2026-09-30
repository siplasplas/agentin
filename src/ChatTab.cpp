#include "ChatTab.h"
#include "ChatView.h"

#include "TurnLocks.h"

#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QPlainTextDocumentLayout>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

ChatTab::ChatTab(AgentProvider *provider, const QString &workingDirectory, QObject *parent)
    : QObject(parent), provider_(provider), document_(new QTextDocument(this)),
      path_(workingDirectory), title_("New chat")
{
    document_->setDocumentLayout(new QPlainTextDocumentLayout(document_));
    // Other windows and outside tools do not announce a free directory, so waiting chats also retry.
    retry_ = new QTimer(this);
    retry_->setInterval(2000);
    connect(retry_, &QTimer::timeout, this, &ChatTab::dispatch);
    setAgent(provider->createChat(workingDirectory, this));
}

ChatTab::~ChatTab()
{
    releaseDirectory();
}

void ChatTab::setTurnLocks(TurnLocks *locks)
{
    locks_ = locks;
    connect(locks, &TurnLocks::released, this, [this] {
        if (isWaiting()) QTimer::singleShot(0, this, &ChatTab::dispatch);
    });
}

void ChatTab::cancelWaiting()
{
    if (!isWaiting()) return;
    outgoing_.clear();
    waitingFor_.clear();
    retry_->stop();
    emit logMessage("[Stopped waiting; the messages were not sent.]");
    emit changed();
}

bool ChatTab::dispatch()
{
    if (outgoing_.isEmpty() || inTurn_ || agent_->isCompacting() || agent_->isSteering()) return true;
    // A read-only turn in a sandbox cannot change files, so it neither takes nor waits for the directory;
    // an agent that only promises not to change files still waits.
    if (locks_ && !holdsDirectory_ && !(agent_->isReadOnly() && agent_->readOnlyIsEnforced())) {
        const QString label = provider_->name() + " chat \"" + title_.left(40) + "\"";
        const QString holder = locks_->acquire(QString("%1").arg(quintptr(this)),
                                               QStringList{path_} + agent_->writableDirectories(), label);
        if (!holder.isEmpty()) {
            if (waitingFor_ != holder) {
                waitingFor_ = holder;
                emit logMessage("[" + provider_->name() + " waits for " + QDir::toNativeSeparators(path_)
                                + ": it is used by " + holder + "]");
                emit changed();
            }
            retry_->start();
            return true;
        }
        holdsDirectory_ = true;
    }
    retry_->stop();
    const bool wasWaiting = isWaiting();
    waitingFor_.clear();
    inTurn_ = true;
    turnStartedAt_ = QDateTime::currentMSecsSinceEpoch();
    turnUsage_ = {};
    if (!agent_->prompt(outgoing_.takeFirst())) {
        inTurn_ = false;
        releaseDirectory();
        emit changed();
        return false;
    }
    if (wasWaiting) emit changed();
    return true;
}

void ChatTab::releaseDirectory()
{
    if (!holdsDirectory_) return;
    holdsDirectory_ = false;
    if (locks_) locks_->release(QString("%1").arg(quintptr(this)));
}

namespace {
QString compactCount(qint64 count)
{
    if (count < 1000) return QString::number(count);
    if (count < 1000000) return QString::number(count / 1000.0, 'f', count < 100000 ? 1 : 0) + "k";
    return QString::number(count / 1000000.0, 'f', 1) + "M";
}

QString elapsedTime(qint64 milliseconds)
{
    const qint64 seconds = qMax<qint64>(0, milliseconds) / 1000;
    return QString("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0'))
                            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString fullCount(qint64 count)
{
    QLocale locale = QLocale::c();
    locale.setNumberOptions({});
    return locale.toString(count);
}
}

// For example "12.3k→1.2k": input tokens, then output tokens.
QString ChatTab::shortUsage(const TokenUsage &usage)
{
    if (usage.isEmpty()) return {};
    const QString in = usage.input < 0 ? "?" : compactCount(usage.input);
    const QString out = usage.output < 0 ? "?" : compactCount(usage.output);
    return in + QChar(0x2192) + out;
}

QString ChatTab::usageDetails(const TokenUsage &usage)
{
    QStringList parts;
    if (usage.input >= 0) {
        parts.append(fullCount(usage.input) + " in"
                     + (usage.cached >= 0 ? " (" + fullCount(usage.cached) + " cached)" : QString()));
    }
    if (usage.output >= 0) {
        parts.append(fullCount(usage.output) + " out"
                     + (usage.reasoning >= 0 ? " (" + fullCount(usage.reasoning) + " reasoning)" : QString()));
    }
    if (usage.total >= 0) parts.append(fullCount(usage.total) + " total");
    if (usage.costUsd >= 0) parts.append(QString("$%1").arg(usage.costUsd, 0, 'f', 4));
    if (usage.contextUsed >= 0 && usage.contextWindow > 0) {
        parts.append(QString("context %1% of %2").arg(qRound(100.0 * usage.contextUsed / usage.contextWindow))
                         .arg(compactCount(usage.contextWindow)));
    }
    return parts.join(", ");
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
    if (isWaiting()) parts.append("waiting: the directory is used by " + waitingFor_);
    if (!lockNotice_.isEmpty()) parts.append("locked: " + lockNotice_);
    else if (!live_) parts.append("read-only preview");
    return parts.join("  •  ");
}

QString ChatTab::operationTimeText() const
{
    if (compactionClockRunning_) return "Compact " + elapsedTime(compactionClock_.elapsed());
    if (taskClockRunning_) return "Task " + elapsedTime(taskClock_.elapsed());
    return lastOperationName_ + ' ' + elapsedTime(lastOperationDurationMs_);
}

void ChatTab::updateTaskClock()
{
    const bool running = agent_->isResponding() && !agent_->isCompacting();
    if (running && !taskClockRunning_) {
        taskClock_.start();
        taskClockRunning_ = true;
    } else if (!running && taskClockRunning_) {
        lastOperationDurationMs_ = taskClock_.elapsed();
        lastOperationName_ = "Task";
        taskClockRunning_ = false;
        emit logMessage("[" + provider_->name() + " task duration: " + elapsedTime(lastOperationDurationMs_) + "]");
    }
}

void ChatTab::finishCompactionClock()
{
    if (!compactionClockRunning_) return;
    lastOperationDurationMs_ = compactionClock_.elapsed();
    lastOperationName_ = "Compact";
    compactionClockRunning_ = false;
    emit logMessage("[" + provider_->name() + " compaction duration: " + elapsedTime(lastOperationDurationMs_) + "]");
    emit changed();
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
    lastOperationDurationMs_ = 0;
    lastOperationName_ = "Time";
    live_ = true;
    liveTranscript_.clear();
    historyRefreshCutoff_ = 0;
    historyRefreshQueuedText_.clear();
    toolGroup_ = 0;
    turnUsage_ = {};
    conversationUsage_ = {};
    conversationUsageFromAgent_ = false;
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
    historyRefreshCutoff_ = 0;
    historyRefreshQueuedText_.clear();
    toolGroup_ = 0;
    turnUsage_ = {};
    conversationUsage_ = {};
    conversationUsageFromAgent_ = false;
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
    // The title names the chat also to other chats that wait for its directory.
    if (id_.isEmpty() && title_ == "New chat") {
        title_ = text.simplified().left(200);
        emit changed();
    }
    outgoing_.append(text);
    if (!dispatch()) return false;
    appendText("\nYou: " + text + '\n');
    sentMessages_.append(text);
    emit userMessagesChanged();
    return true;
}

bool ChatTab::steer(const QString &text)
{
    return live_ && !pendingRequest() && agent_->steer(text);
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
    connect(agent, &AgentBackend::stateChanged, this, [this] {
        updateTaskClock();
        emit changed();
        if (!inTurn_ && !agent_->isCompacting() && !outgoing_.isEmpty())
            QTimer::singleShot(0, this, &ChatTab::dispatch);
    });
    connect(agent, &AgentBackend::steerAccepted, this, [this](const QString &text) {
        appendText("\nYou (steer): " + text + '\n');
        sentMessages_.append(text);
        emit userMessagesChanged();
    });
    connect(agent, &AgentBackend::steerFailed, this, [this, name](const QString &text, const QString &reason) {
        emit logMessage("[" + name + " steering was not confirmed: " + reason + "]");
        appendText("\nYou (steer not confirmed): " + text + '\n');
        emit steeringFailed(text);
    });
    connect(agent, &AgentBackend::messageStarted, this, [this, name] { appendText("\n" + name + ": "); });
    connect(agent, &AgentBackend::messageDelta, this, &ChatTab::appendText);
    connect(agent, &AgentBackend::messageFinished, this, [this] { appendText("\n"); });
    connect(agent, &AgentBackend::toolStarted, this, [this, name](const QString &tool, const QString &details) {
        toolGroup_ = 0;
        appendText("\n[" + name + " tool: " + tool + "] " + details.simplified().left(120) + '\n');
        toolGroup_ = ++nextToolGroup_;
        QTextBlock header = document_->lastBlock().previous();
        auto *data = new ToolBlockData;
        data->group = toolGroup_;
        data->header = true;
        header.setUserData(data);
        if (details != details.simplified().left(120)) appendToolText(details + '\n');
        else emit textAppended();
    });
    connect(agent, &AgentBackend::toolOutput, this, &ChatTab::appendToolText);
    connect(agent, &AgentBackend::toolFinished, this, [this](const QString &tool, const QString &status) {
        // Keep the final status visible even while the output is folded.
        appendToolText("\n");
        toolGroup_ = 0;
        document_->lastBlock().setUserData(nullptr);
        appendText("[" + tool + ": " + status + "]\n");
    });
    connect(agent, &AgentBackend::turnCompleted, this, [this, name](const QString &status, const QString &details) {
        updateTaskClock();
        // Requests of a finished turn can no longer be answered.
        if (!requests_.isEmpty()) {
            requests_.clear();
            emit requestsChanged();
        }
        if (!turnUsage_.isEmpty()) {
            if (!conversationUsageFromAgent_) conversationUsage_ += turnUsage_;
            emit logMessage("[" + name + " turn: " + usageDetails(turnUsage_) + "]");
            emit changed();
        }
        // Between turns the directory is free for other agents; the next queued message takes it again.
        inTurn_ = false;
        releaseDirectory();
        QTimer::singleShot(0, this, &ChatTab::dispatch);
        if (status != "completed")
            emit logMessage("[" + name + " response: " + status + (details.isEmpty() ? "" : ": " + details) + "]");
        if (status != "interrupted" && turnStartedAt_ > 0)
            emit turnEnded(status == "completed", QDateTime::currentMSecsSinceEpoch() - turnStartedAt_);
        turnStartedAt_ = 0;
    });
    connect(agent, &AgentBackend::approvalRequested, this,
            [this](int id, const QString &title, const QString &description, bool canAcceptForSession,
                   const QString &alwaysRule) {
        PendingRequest request;
        request.id = id;
        request.approval = true;
        request.title = title;
        request.description = description;
        request.canAcceptForSession = canAcceptForSession;
        request.alwaysRule = alwaysRule;
        requests_.append(request);
        emit requestsChanged();
    });
    connect(agent, &AgentBackend::questionsRequested, this, [this](int id, const QList<AgentQuestion> &questions) {
        if (questions.isEmpty()) {
            agent_->answerQuestions(id, {});
            return;
        }
        PendingRequest request;
        request.id = id;
        request.questions = questions;
        requests_.append(request);
        emit requestsChanged();
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
    connect(agent, &AgentBackend::turnUsage, this, [this](const TokenUsage &usage) {
        turnUsage_ = usage;
        emit changed();
    });
    connect(agent, &AgentBackend::conversationUsage, this, [this](const TokenUsage &usage) {
        conversationUsageFromAgent_ = true;
        conversationUsage_ = usage;
        emit changed();
    });
    connect(agent, &AgentBackend::compactionStarted, this, [this] {
        if (compactionClockRunning_) return;
        compactionClock_.start();
        compactionClockRunning_ = true;
        emit changed();
    });
    connect(agent, &AgentBackend::compactionFinished, this, &ChatTab::finishCompactionClock);
    connect(agent, &AgentBackend::contextCompacted, this, [this] { appendText("\n[Context compacted]\n"); });
    connect(agent, &AgentBackend::historyRefreshStarted, this, [this] {
        historyRefreshCutoff_ = liveTranscript_.size();
        historyRefreshQueuedText_.clear();
        historyRefreshSentCount_ = sentMessages_.size() - outgoing_.size();
        for (const QString &text : outgoing_) historyRefreshQueuedText_ += "\nYou: " + text + '\n';
    });
    connect(agent, &AgentBackend::historyRefreshed, this,
            [this](const QString &id, const QList<ChatEntry> &entries, bool hasMore) {
        if (id != id_) return;
        // No next turn starts until the snapshot arrives. Keep prompts queued before and during it.
        liveTranscript_ = historyRefreshQueuedText_ + liveTranscript_.mid(historyRefreshCutoff_);
        historyRefreshCutoff_ = 0;
        historyRefreshQueuedText_.clear();
        sentMessages_ = sentMessages_.mid(historyRefreshSentCount_);
        historyRefreshSentCount_ = 0;
        // The retained live text now contains only queued prompts, so old tool offsets no longer apply.
        for (QTextBlock block = document_->begin(); block.isValid(); block = block.next())
            block.setUserData(nullptr);
        showHistory(entries, hasMore, {});
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

void ChatTab::appendToolText(const QString &text)
{
    if (!live_ || text.isEmpty()) return;
    if (!toolGroup_) {
        appendText(text);
        return;
    }
    const int start = document_->characterCount() - 1;
    // Tag before announcing the update so the view never displays the full output briefly.
    liveTranscript_ += text;
    QTextCursor cursor(document_);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    for (QTextBlock block = document_->findBlock(start); block.isValid(); block = block.next()) {
        auto *data = new ToolBlockData;
        data->group = toolGroup_;
        block.setUserData(data);
    }
    emit textAppended();
}

void ChatTab::showHistory(const QList<ChatEntry> &entries, bool hasMore, const QString &notice)
{
    struct SavedToolBlock { int offset; int group; bool header; bool collapsed; };
    QList<SavedToolBlock> saved;
    const int oldPrefix = document_->characterCount() - 1 - liveTranscript_.size();
    if (live_) {
        for (QTextBlock block = document_->begin(); block.isValid(); block = block.next()) {
            auto *data = dynamic_cast<ToolBlockData *>(block.userData());
            if (data && block.position() >= oldPrefix)
                saved.append({block.position() - oldPrefix, data->group, data->header, data->collapsed});
        }
    }
    const QString name = provider_->name();
    QStringList blocks;
    QList<int> historyTools;
    historyMessages_.clear();
    for (const ChatEntry &entry : entries) {
        if (entry.role == "user") {
            historyMessages_.append(entry.text);
            blocks.append("You: " + entry.text);
        } else if (entry.role == "tool") {
            historyTools.append(blocks.size());
            const QString heading = entry.text.section('\n', 0, 0).left(120);
            blocks.append("[" + name + " tool: " + heading + "]"
                          + (entry.text == heading ? QString() : "\n" + entry.text));
        } else {
            blocks.append(name + ": " + entry.text);
        }
    }
    if (!notice.isEmpty()) blocks.append("[" + notice + "]");
    else if (blocks.isEmpty()) blocks.append("[This conversation has no messages to show.]");
    const QString prefix = blocks.join("\n\n") + '\n';
    document_->setPlainText(prefix + (live_ ? liveTranscript_ : QString()));
    int offset = 0;
    for (int i = 0; i < blocks.size(); ++i) {
        if (historyTools.contains(i)) {
            const int group = ++nextToolGroup_;
            const int end = offset + blocks.at(i).size();
            bool header = true;
            for (QTextBlock block = document_->findBlock(offset); block.isValid() && block.position() < end;
                 block = block.next()) {
                auto *data = new ToolBlockData;
                data->group = group;
                data->header = header;
                block.setUserData(data);
                header = false;
            }
        }
        offset += blocks.at(i).size() + 2;
    }
    for (const SavedToolBlock &item : saved) {
        QTextBlock block = document_->findBlock(prefix.size() + item.offset);
        auto *data = new ToolBlockData;
        data->group = item.group;
        data->header = item.header;
        data->collapsed = item.collapsed;
        block.setUserData(data);
    }
    hasMore_ = hasMore;
    emit userMessagesChanged();
    emit changed();
    emit textAppended();
}

void ChatTab::answerApproval(ApprovalDecision decision)
{
    if (requests_.isEmpty() || !requests_.first().approval) return;
    const PendingRequest request = requests_.takeFirst();
    if (decision == ApprovalDecision::AcceptForSession) {
        QStringList lines = request.description.split('\n', Qt::SkipEmptyParts);
        for (QString &line : lines) line = line.trimmed();
        sessionApprovals_.append(lines.join(" · "));
    }
    agent_->answerApproval(request.id, decision);
    emit requestsChanged();
}

void ChatTab::resetSessionApprovals()
{
    if (!agent_->canResetSessionApprovals() || agent_->isResponding()) return;
    agent_->resetSessionApprovals();
    sessionApprovals_.clear();
}

void ChatTab::answerQuestion(const QStringList &values)
{
    if (requests_.isEmpty() || requests_.first().approval) return;
    PendingRequest &request = requests_.first();
    request.answers.insert(request.questions.at(request.current).id, values);
    if (++request.current < request.questions.size()) {
        emit requestsChanged();
        return;
    }
    finishQuestions();
}

void ChatTab::skipQuestions()
{
    if (!requests_.isEmpty() && !requests_.first().approval) finishQuestions();
}

void ChatTab::finishQuestions()
{
    const PendingRequest request = requests_.takeFirst();
    agent_->answerQuestions(request.id, request.answers);
    emit requestsChanged();
}

bool ChatTab::answerWithText(const QString &text)
{
    if (requests_.isEmpty() || requests_.first().approval) return false;
    const PendingRequest &request = requests_.first();
    const AgentQuestion &question = request.questions.at(request.current);
    if (question.secret) {
        emit logMessage("[Type the answer in the hidden field above the message field, so it is not shown.]");
        return true;
    }
    if (question.options.isEmpty()) {
        answerQuestion({text});
        return true;
    }
    // Option numbers, several separated by commas or spaces for a multi-select question.
    QStringList chosen;
    const QStringList numbers = text.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
    for (const QString &number : numbers) {
        bool isNumber = false;
        const int index = number.toInt(&isNumber) - 1;
        if (!isNumber || index < 0 || index >= question.options.size()) {
            chosen.clear();
            break;
        }
        chosen.append(question.options.at(index));
    }
    if (!chosen.isEmpty() && (question.multiSelect || chosen.size() == 1)) {
        answerQuestion(chosen);
    } else if (question.allowOther) {
        answerQuestion({text});
    } else {
        emit logMessage(QString("[Choose an option by its number, 1 to %1.]").arg(question.options.size()));
    }
    return true;
}
