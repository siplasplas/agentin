#include "ProcessLocks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace {
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
}

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

QString processStartTime(qint64 pid)
{
    return processStatFields(pid).value(19);
}

bool isProcessAlive(qint64 pid, const QString &startTime)
{
    const QString current = processStartTime(pid);
    return !current.isEmpty() && (startTime.isEmpty() || current == startTime);
}

QList<BusyAgentSession> busyClaudeSessions(const QString &configDirectory)
{
    QList<BusyAgentSession> sessions;
    const QDir registry(QDir(configDirectory).filePath("sessions"));
    for (const QFileInfo &info : registry.entryInfoList({"*.json"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject entry = QJsonDocument::fromJson(file.readAll()).object();
        const qint64 pid = entry.value("pid").toInteger();
        if (entry.value("status").toString() != "busy" || pid <= 0 || isOwnDescendant(pid)
            || !isProcessAlive(pid, entry.value("procStart").toString())) continue;
        sessions.append({pid, entry.value("cwd").toString(), {},
                         QString("a Claude Code %1 (PID %2)").arg(entry.value("kind").toString("session")).arg(pid)});
    }
    return sessions;
}

QList<BusyAgentSession> runningCliSessions()
{
    QList<BusyAgentSession> sessions;
    const QDir processes("/proc");
    const QString home = QDir::homePath();
    for (const QString &name : processes.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 pid = name.toLongLong(&numeric);
        if (!numeric || isOwnDescendant(pid)) continue;
        QFile file(processes.filePath(name + "/cmdline"));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QStringList arguments = QString::fromUtf8(file.readAll()).split(QChar('\0'), Qt::SkipEmptyParts);
        if (arguments.isEmpty()) continue;
        const QString executable = QFileInfo(QFileInfo(processes.filePath(name + "/exe")).symLinkTarget()).fileName();
        const QString command = QFileInfo(arguments.first()).fileName();
        // Match whole program names only, so tools such as gemini-commander are not mistaken for the CLI.
        QString tool;
        if (executable == "agy" || command == "agy") {
            tool = "Antigravity CLI";
        } else if (command == "gemini" || command.startsWith("node") || executable.startsWith("node")) {
            for (const QString &argument : arguments) {
                const QString argumentName = QFileInfo(argument).fileName();
                if (argumentName == "gemini" || argumentName == "gemini.js" || argument.contains("/@google/gemini-cli/")) {
                    tool = "Gemini CLI";
                    break;
                }
            }
        }
        if (tool.isEmpty()) continue;
        const QString directory = QFileInfo(processes.filePath(name + "/cwd")).symLinkTarget();
        // A CLI started in / or the home directory would cover every project, so it is not counted.
        if (directory.isEmpty() || directory == "/" || directory == home) continue;
        sessions.append({pid, directory, {}, QString("%1 (PID %2: %3)").arg(tool).arg(pid).arg(arguments.mid(0, 4).join(' '))});
    }
    return sessions;
}

namespace {
// Reads the working directory, the sandbox and whether a turn is running from a Codex rollout file.
// Only the start and the last part of the file are read; the first line holds the session's directory.
BusyAgentSession codexRolloutState(const QString &path, bool *running)
{
    *running = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QString directory = QJsonDocument::fromJson(file.readLine(1024 * 1024)).object().value("payload").toObject()
                            .value("cwd").toString();
    constexpr qint64 tail = 512 * 1024;
    const bool partial = file.size() > tail;
    if (partial) file.seek(file.size() - tail);
    QString sandbox;
    QStringList writable;
    bool firstLine = partial;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        // After seeking, the first line is usually cut off.
        if (firstLine) {
            firstLine = false;
            continue;
        }
        const QJsonObject record = QJsonDocument::fromJson(line).object();
        const QJsonObject payload = record.value("payload").toObject();
        const QString type = record.value("type").toString();
        if (type == "turn_context") {
            if (payload.contains("cwd")) directory = payload.value("cwd").toString();
            sandbox = payload.value("sandbox_policy").toObject().value("type").toString();
            writable.clear();
            for (const QJsonValue &value : payload.value("file_system_sandbox_policy").toObject().value("entries").toArray()) {
                const QJsonObject entry = value.toObject();
                const QJsonObject place = entry.value("path").toObject();
                if (entry.value("access").toString() == "write" && place.value("type").toString() == "path")
                    writable.append(place.value("path").toString());
            }
        } else if (type == "event_msg") {
            const QString event = payload.value("type").toString();
            if (event == "task_started") *running = true;
            else if (event == "task_complete" || event == "turn_aborted") *running = false;
        }
    }
    if (sandbox == "read-only") *running = false;
    return {0, directory, writable, {}};
}
}

QList<BusyAgentSession> runningCodexTurns(const QString &codexHome)
{
    QList<BusyAgentSession> sessions;
    const QString sessionsRoot = QDir(QDir(codexHome).filePath("sessions")).canonicalPath();
    if (sessionsRoot.isEmpty()) return sessions;
    const QDir processes("/proc");
    QSet<QString> seen;
    for (const QString &name : processes.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 pid = name.toLongLong(&numeric);
        if (!numeric) continue;
        // Only Codex processes are searched for open files, which keeps the scan short.
        QFile cmdline(processes.filePath(name + "/cmdline"));
        if (!cmdline.open(QIODevice::ReadOnly) || !cmdline.readAll().contains("codex") || isOwnDescendant(pid)) continue;
        const QDir descriptors(processes.filePath(name + "/fd"));
        for (const QFileInfo &descriptor : descriptors.entryInfoList(QDir::Files | QDir::System)) {
            const QString target = descriptor.symLinkTarget();
            if (!target.startsWith(sessionsRoot + '/') || !QFileInfo(target).fileName().startsWith("rollout-")
                || seen.contains(target)) continue;
            seen.insert(target);
            bool running = false;
            BusyAgentSession session = codexRolloutState(target, &running);
            if (!running || session.directory.isEmpty()) continue;
            session.pid = pid;
            session.description = QString("a Codex turn outside agentin (PID %1)").arg(pid);
            sessions.append(session);
        }
    }
    return sessions;
}
