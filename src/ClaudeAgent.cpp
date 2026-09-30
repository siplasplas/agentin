#include "ClaudeAgent.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>

namespace {
constexpr int kHistoryPageSize = 20;
}

ClaudeAgent::ClaudeAgent(const QString &pythonProgram, const QString &scriptPath,
                         const QString &workingDirectory, const QString &provider, QObject *parent)
    : AgentBackend(parent), pythonProgram_(pythonProgram), scriptPath_(scriptPath),
      workingDirectory_(workingDirectory), provider_(provider), process_(new QProcess(this))
{
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
        buffer_ += process_->readAllStandardOutput();
        qsizetype newline;
        while ((newline = buffer_.indexOf('\n')) >= 0) {
            const QByteArray line = buffer_.left(newline).trimmed();
            buffer_.remove(0, newline + 1);
            if (!line.isEmpty()) handleLine(line);
        }
    });
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QString details = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!details.isEmpty()) emit message("[" + name() + "] " + details);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        emit message("[" + name() + "] " + provider_.toUpper() + " bridge process error: " + process_->errorString()
                     + " (Python: " + pythonProgram_ + ")");
        if (processError != QProcess::FailedToStart) return;
        ready_ = false;
        busy_ = false;
        stopRequested_ = false;
        emit stateChanged();
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        emit message(QString("[%1] %2 bridge exited with code %3").arg(name(), provider_.toUpper()).arg(code));
        ready_ = false;
        busy_ = false;
        stopRequested_ = false;
        emit stateChanged();
    });
}

ClaudeAgent::~ClaudeAgent()
{
    for (QProcess *process : historyProcesses_) {
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    disconnect(process_, nullptr, this, nullptr);
    if (process_->state() != QProcess::NotRunning) {
        send({{"type", "shutdown"}});
        process_->closeWriteChannel();
        if (!process_->waitForFinished(1000)) {
            process_->terminate();
            if (!process_->waitForFinished(1000)) {
                process_->kill();
                process_->waitForFinished(1000);
            }
        }
    }
}

QString ClaudeAgent::name() const
{
    return provider_ == "glm" ? "GLM" : "Claude";
}

QString ClaudeAgent::statusText() const
{
    if (busy_) return name() + " is responding…";
    if (ready_) return name() + " ready";
    if (isRunning()) return provider_ == "glm" ? "Connecting to GLM via Claude Agent SDK…" : "Connecting to Claude Agent SDK…";
    return name() + " bridge is not running";
}

bool ClaudeAgent::isRunning() const
{
    return process_->state() != QProcess::NotRunning;
}

void ClaudeAgent::start(const QString &workingDirectory)
{
    if (isRunning()) return;
    if (!workingDirectory.isEmpty()) workingDirectory_ = workingDirectory;
    process_->setWorkingDirectory(workingDirectory_);
    process_->start(pythonProgram_, {"-u", scriptPath_, "--cwd", workingDirectory_, "--provider", provider_});
}

bool ClaudeAgent::newConversation(const QString &workingDirectory)
{
    if (busy_) {
        emit message("[Wait for " + name() + " to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    sessionId_.clear();
    queuedPrompts_.clear();
    pendingResumeId_.clear();
    ready_ = false;
    if (isRunning()) {
        send({{"type", "new"}, {"cwd", workingDirectory}});
    } else {
        start(workingDirectory);
    }
    emit stateChanged();
    return true;
}

bool ClaudeAgent::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (id.isEmpty() || !QFileInfo(workingDirectory).isDir()) return false;
    if (busy_) {
        emit message("[Wait for " + name() + " to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    sessionId_ = id;
    queuedPrompts_.clear();
    ready_ = false;
    if (isRunning()) {
        send({{"type", "resume"}, {"session_id", id}, {"cwd", workingDirectory}});
    } else {
        // The bridge must report ready before it accepts the resume command.
        pendingResumeId_ = id;
        start(workingDirectory);
    }
    emit message("[Resuming " + name() + " conversation: " + id + "]");
    emit stateChanged();
    return true;
}

bool ClaudeAgent::prompt(const QString &text)
{
    queuedPrompts_.append(text);
    if (!ready_) start({});
    sendNextPrompt();
    return true;
}

void ClaudeAgent::interrupt()
{
    if (!busy_ || stopRequested_) return;
    stopRequested_ = true;
    send({{"type", "stop"}});
    emit stateChanged();
}

// The bridge returns the last historyLimit_ entries; older requests widen that window.
void ClaudeAgent::loadHistory(const QString &id, const QString &workingDirectory, bool older)
{
    const quint64 generation = ++historyGeneration_;
    historyLimit_ = older ? historyLimit_ + kHistoryPageSize : kHistoryPageSize;
    if (pythonProgram_.isEmpty() || scriptPath_.isEmpty()) {
        emit historyLoaded(id, {}, false, "The Claude Agent SDK bridge is not configured.");
        return;
    }
    auto *process = new QProcess(this);
    historyProcesses_.append(process);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process, generation, id](int exitCode, QProcess::ExitStatus status) {
        historyProcesses_.removeAll(process);
        process->deleteLater();
        if (generation != historyGeneration_) return;
        const QJsonObject result = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
        if (status != QProcess::NormalExit || exitCode != 0 || result.value("type") != "history") {
            QString details = result.value("message").toString();
            if (details.isEmpty()) details = QString::fromUtf8(process->readAllStandardError()).trimmed();
            emit message("[" + name() + " history: " + details + "]");
            emit historyLoaded(id, {}, false, "Could not load this conversation: " + details);
            return;
        }
        QList<ChatEntry> entries;
        for (const QJsonValue &value : result.value("entries").toArray()) {
            const QJsonObject entry = value.toObject();
            entries.append({entry.value("role").toString(), entry.value("text").toString()});
        }
        emit historyLoaded(id, entries, result.value("total").toInt() > entries.size(), {});
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, generation, id](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        historyProcesses_.removeAll(process);
        process->deleteLater();
        if (generation == historyGeneration_) emit historyLoaded(id, {}, false, "Could not start Python: " + process->errorString());
    });
    process->start(pythonProgram_, {"-u", scriptPath_, "--cwd", workingDirectory, "--read-session", id,
                                    "--limit", QString::number(historyLimit_)});
}

void ClaudeAgent::cancelHistory()
{
    ++historyGeneration_;
}

void ClaudeAgent::answerApproval(int id, bool allow)
{
    send({{"type", "approval_response"}, {"id", id}, {"allow", allow}});
}

void ClaudeAgent::answerQuestions(int id, const QHash<QString, QString> &answers)
{
    const int count = pendingQuestionCounts_.take(id);
    QJsonObject result;
    for (auto it = answers.begin(); it != answers.end(); ++it) result.insert(it.key(), it.value());
    send({{"type", "question_response"}, {"id", id}, {"accepted", answers.size() == count}, {"answers", result}});
}

void ClaudeAgent::sendNextPrompt()
{
    if (!ready_ || busy_ || queuedPrompts_.isEmpty()) return;
    busy_ = true;
    stopRequested_ = false;
    textStarted_ = false;
    send({{"type", "prompt"}, {"text", queuedPrompts_.takeFirst()}});
    emit stateChanged();
}

void ClaudeAgent::send(const QJsonObject &message)
{
    if (!isRunning()) return;
    process_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void ClaudeAgent::handleLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit message("[" + name() + "] Invalid " + provider_.toUpper() + " bridge message: " + error.errorString());
        return;
    }
    const QJsonObject event = document.object();
    const QString type = event.value("type").toString();
    if (type == "ready") {
        if (!pendingResumeId_.isEmpty()) {
            const QString id = pendingResumeId_;
            pendingResumeId_.clear();
            send({{"type", "resume"}, {"session_id", id}, {"cwd", workingDirectory_}});
            return;
        }
        ready_ = true;
        emit message(provider_ == "glm" ? "[Connected to GLM via Claude Agent SDK]" : "[Connected to Claude Agent SDK]");
        emit stateChanged();
        sendNextPrompt();
    } else if (type == "delta") {
        if (!textStarted_) {
            textStarted_ = true;
            emit messageStarted();
        }
        emit messageDelta(event.value("text").toString());
    } else if (type == "tool") {
        emit toolStarted(event.value("name").toString(),
                         QString::fromUtf8(QJsonDocument(event.value("input").toObject()).toJson(QJsonDocument::Compact)));
    } else if (type == "complete") {
        if (textStarted_) emit messageFinished();
        busy_ = false;
        stopRequested_ = false;
        textStarted_ = false;
        emit turnCompleted(event.value("status").toString(), event.value("details").toString());
        emit stateChanged();
        sendNextPrompt();
    } else if (type == "approval") {
        const QString details = QString::fromUtf8(QJsonDocument(event.value("input").toObject()).toJson(QJsonDocument::Indented));
        emit approvalRequested(event.value("id").toInt(), "Approve " + name() + " action",
                               event.value("tool").toString() + "\n\n" + details.trimmed());
    } else if (type == "question") {
        // The SDK keys answers by question text.
        QList<AgentQuestion> questions;
        for (const QJsonValue &value : event.value("questions").toArray()) {
            const QJsonObject object = value.toObject();
            AgentQuestion question;
            question.text = object.value("question").toString();
            question.id = question.text;
            question.header = object.value("header").toString(name() + " question");
            question.multiSelect = object.value("multiSelect").toBool();
            for (const QJsonValue &option : object.value("options").toArray())
                question.options.append(option.toObject().value("label").toString());
            questions.append(question);
        }
        const int id = event.value("id").toInt();
        pendingQuestionCounts_.insert(id, questions.size());
        emit questionsRequested(id, questions);
    } else if (type == "session") {
        sessionId_ = event.value("id").toString();
        emit conversationOpened(sessionId_, false);
    } else if (type == "error") {
        emit message("[" + name() + "] " + event.value("message").toString());
    }
}
