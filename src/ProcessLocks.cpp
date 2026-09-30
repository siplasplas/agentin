#include "ProcessLocks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
