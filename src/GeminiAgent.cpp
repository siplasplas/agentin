#include "GeminiAgent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

namespace {
constexpr int kHistoryPageSize = 20;

QString messageText(const QJsonValue &content)
{
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const QJsonValue &block : content.toArray()) {
        const QString text = block.toObject().value("text").toString();
        if (!text.isEmpty()) parts.append(text);
    }
    return parts.join(' ');
}

QJsonObject readSession(const QFileInfo &fileInfo, const QString &projectPath)
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
                firstPrompt = messageText(event.value("content"));
        }
    } else {
        metadata = QJsonDocument::fromJson(file.readAll()).object();
        for (const QJsonValue &value : metadata.value("messages").toArray()) {
            const QJsonObject message = value.toObject();
            if (message.value("type").toString() == "user") {
                firstPrompt = messageText(message.value("content"));
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
QList<ChatEntry> readHistory(const QString &filePath)
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
        const QString text = messageText(message.value("content")).trimmed();
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
}

GeminiAgent::GeminiAgent(const QString &program, const QString &workingDirectory, const QString &dataDirectory,
                         QObject *parent)
    : AgentBackend(parent), program_(program), workingDirectory_(workingDirectory), dataDirectory_(dataDirectory),
      process_(new QProcess(this))
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
        if (!details.isEmpty()) emit message("[Gemini] " + details);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (interrupted_ && processError == QProcess::Crashed) return;
        errorDetails_ = process_->errorString();
        emit message("[Gemini] Gemini CLI process error: " + errorDetails_ + " (executable: " + program_ + ")");
        if (processError == QProcess::FailedToStart) finish(-1);
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { finish(code); });
}

GeminiAgent::~GeminiAgent()
{
    disconnect(process_, nullptr, this, nullptr);
    if (isRunning()) {
        process_->terminate();
        if (!process_->waitForFinished(1000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
}

bool GeminiAgent::isRunning() const { return process_->state() != QProcess::NotRunning; }

QString GeminiAgent::statusText() const
{
    return busy_ ? "Gemini is responding…" : "Gemini ready";
}

bool GeminiAgent::newConversation(const QString &workingDirectory)
{
    if (busy_) {
        emit message("[Wait for Gemini to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    sessionId_.clear();
    queuedPrompts_.clear();
    return true;
}

bool GeminiAgent::resumeConversation(const QString &id, const QString &workingDirectory)
{
    if (id.isEmpty() || !QFileInfo(workingDirectory).isDir()) return false;
    if (busy_) {
        emit message("[Wait for Gemini to finish.]");
        return false;
    }
    workingDirectory_ = workingDirectory;
    sessionId_ = id;
    queuedPrompts_.clear();
    emit message("[Resuming Gemini conversation: " + id + "]");
    emit stateChanged();
    return true;
}

bool GeminiAgent::prompt(const QString &text)
{
    queuedPrompts_.append(text);
    sendNextPrompt();
    return true;
}

void GeminiAgent::interrupt()
{
    if (!busy_ || stopRequested_ || !isRunning()) return;
    stopRequested_ = true;
    interrupted_ = true;
    process_->terminate();
    QTimer::singleShot(1000, process_, [this] {
        if (isRunning()) process_->kill();
    });
    emit stateChanged();
}

void GeminiAgent::loadHistory(const QString &id, const QString &, bool older)
{
    historyLimit_ = older ? historyLimit_ + kHistoryPageSize : kHistoryPageSize;
    QString filePath = sessionFiles_.value(id);
    if (!QFileInfo(filePath).isFile()) {
        discoverSessions();
        filePath = sessionFiles_.value(id);
    }
    if (filePath.isEmpty()) {
        emit historyLoaded(id, {}, false, "The Gemini session file was not found.");
        return;
    }
    const QList<ChatEntry> entries = readHistory(filePath);
    const QList<ChatEntry> tail = entries.mid(qMax(0, entries.size() - historyLimit_));
    emit historyLoaded(id, tail, entries.size() > tail.size(), {});
}

QList<QJsonObject> GeminiAgent::discoverSessions()
{
    QHash<QString, QString> projectPaths;
    QFile projects(QDir(dataDirectory_).filePath("projects.json"));
    if (projects.open(QIODevice::ReadOnly)) {
        const QJsonObject entries = QJsonDocument::fromJson(projects.readAll()).object()
                                        .value("projects").toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (QFileInfo(it.key()).isAbsolute() && !it.value().toString().isEmpty())
                projectPaths.insert(it.value().toString(), it.key());
        }
    }
    QList<QJsonObject> sessions;
    const QDir profileRoot(QDir(dataDirectory_).filePath("tmp"));
    for (const QFileInfo &profile : profileRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString path = projectPaths.value(profile.fileName());
        if (path.isEmpty()) {
            QFile root(QDir(profile.filePath()).filePath(".project_root"));
            if (root.open(QIODevice::ReadOnly)) path = QString::fromUtf8(root.readAll()).trimmed();
        }
        if (!QFileInfo(path).isAbsolute()) continue;
        const QDir chats(QDir(profile.filePath()).filePath("chats"));
        for (const QFileInfo &file : chats.entryInfoList({"session-*.jsonl", "session-*.json"}, QDir::Files, QDir::Name)) {
            const QJsonObject session = readSession(file, path);
            const QString id = session.value("id").toString();
            if (id.isEmpty()) continue;
            sessionFiles_.insert(id, session.value("file").toString());
            sessions.append(session);
        }
    }
    return sessions;
}

void GeminiAgent::sendNextPrompt()
{
    if (busy_ || isRunning() || queuedPrompts_.isEmpty()) return;
    busy_ = true;
    stopRequested_ = false;
    textStarted_ = false;
    buffer_.clear();
    errorDetails_.clear();
    interrupted_ = false;
    resultSeen_ = false;
    QStringList arguments{"--output-format", "stream-json"};
    if (!sessionId_.isEmpty()) {
        arguments << "--resume" << (sessionId_.startsWith("index:") ? sessionId_.mid(6) : sessionId_);
    }
    arguments << "--prompt" << queuedPrompts_.takeFirst();
    process_->setWorkingDirectory(workingDirectory_);
    process_->start(program_, arguments);
    emit stateChanged();
}

void GeminiAgent::handleLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        emit message("[Gemini] Invalid Gemini CLI response: " + parseError.errorString());
        return;
    }
    const QJsonObject event = document.object();
    const QString type = event.value("type").toString();
    if (type == "init") {
        const QString id = event.value("session_id").toString();
        if (!id.isEmpty()) {
            sessionId_ = id;
            emit conversationOpened(id, false);
        }
    } else if (type == "message" && event.value("role").toString() == "assistant") {
        const QString content = event.value("content").toString();
        if (content.isEmpty()) return;
        if (!textStarted_) {
            textStarted_ = true;
            emit messageStarted();
        }
        emit messageDelta(content);
    } else if (type == "tool_use") {
        emit toolStarted(event.value("tool_name").toString(),
                         QString::fromUtf8(QJsonDocument(event.value("parameters").toObject()).toJson(QJsonDocument::Compact)));
    } else if (type == "error") {
        const QString details = event.value("message").toString();
        if (event.value("severity").toString() != "warning") errorDetails_ = details;
        emit message("[Gemini] " + details);
    } else if (type == "result") {
        resultSeen_ = true;
        const QString status = event.value("status").toString();
        if (status != "success") errorDetails_ = event.value("error").toObject().value("message").toString(status);
    }
}

void GeminiAgent::finish(int code)
{
    buffer_ += process_->readAllStandardOutput();
    qsizetype newline;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (!line.isEmpty()) handleLine(line);
    }
    if (!buffer_.trimmed().isEmpty()) handleLine(buffer_.trimmed());
    buffer_.clear();
    if (textStarted_) emit messageFinished();
    busy_ = false;
    stopRequested_ = false;
    textStarted_ = false;
    emit turnCompleted(interrupted_ ? "interrupted" : (code == 0 && resultSeen_ && errorDetails_.isEmpty() ? "completed" : "failed"),
                       errorDetails_.isEmpty() && code != 0 && !interrupted_
                           ? QString("Gemini CLI exited with code %1").arg(code) : errorDetails_);
    emit stateChanged();
    sendNextPrompt();
}
