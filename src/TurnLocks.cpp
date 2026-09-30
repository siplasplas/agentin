#include "TurnLocks.h"

#include "ProcessLocks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

namespace {
// Directories conflict when they are the same or one contains the other.
bool overlaps(const QString &left, const QString &right)
{
    const QString a = QDir::cleanPath(left);
    const QString b = QDir::cleanPath(right);
    if (a.isEmpty() || b.isEmpty()) return false;
    const auto within = [](const QString &inner, const QString &outer) {
        return inner == outer || inner.startsWith(outer.endsWith('/') ? outer : outer + '/');
    };
    return within(a, b) || within(b, a);
}

QString canonical(const QString &directory)
{
    const QString path = QDir(directory).canonicalPath();
    return path.isEmpty() ? QDir::cleanPath(directory) : path;
}
}

TurnLocks::TurnLocks(const QString &dataDirectory, QObject *parent)
    : QObject(parent), registryPath_(QDir(dataDirectory).filePath("turn-locks.json")),
      guardPath_(QDir(dataDirectory).filePath("turn-locks.lock"))
{
}

TurnLocks::~TurnLocks()
{
    for (const QString &owner : QSet<QString>(held_)) release(owner);
}

QString TurnLocks::acquire(const QString &owner, const QString &directory, const QString &label)
{
    QDir().mkpath(QFileInfo(registryPath_).absolutePath());
    QLockFile guard(guardPath_);
    guard.setStaleLockTime(10000);
    if (!guard.tryLock(2000)) return "another agentdeskt window that is updating the lock registry";

    const qint64 self = QCoreApplication::applicationPid();
    const QString path = canonical(directory);
    QFile file(registryPath_);
    QJsonArray entries;
    if (file.open(QIODevice::ReadOnly)) entries = QJsonDocument::fromJson(file.readAll()).object().value("turns").toArray();
    file.close();

    QJsonArray kept;
    QString holder;
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const qint64 pid = entry.value("pid").toInteger();
        const QString entryOwner = entry.value("owner").toString();
        // Entries of ended processes, or of this process that no chat holds any more, are stale.
        if (pid == self ? !held_.contains(entryOwner) : !isProcessAlive(pid, entry.value("start").toString())) continue;
        if (entryOwner == owner) continue;
        kept.append(entry);
        if (holder.isEmpty() && overlaps(path, entry.value("directory").toString())) {
            holder = entry.value("label").toString();
            if (pid != self) holder += QString(" in another agentdeskt window (PID %1)").arg(pid);
        }
    }
    if (holder.isEmpty()) {
        const QString claudeConfig = qEnvironmentVariable("CLAUDE_CONFIG_DIR", QDir::home().filePath(".claude"));
        for (const BusyAgentSession &session : busyClaudeSessions(claudeConfig)) {
            if (!overlaps(path, canonical(session.directory))) continue;
            holder = session.description + " in " + session.directory;
            break;
        }
    }
    if (holder.isEmpty()) {
        for (const BusyAgentSession &session : runningCliSessions()) {
            if (!overlaps(path, canonical(session.directory))) continue;
            holder = session.description + " in " + session.directory + "; close it so this chat can continue";
            break;
        }
    }
    if (holder.isEmpty()) {
        kept.append(QJsonObject{{"owner", owner}, {"directory", path}, {"label", label}, {"pid", self},
                                {"start", processStartTime(self)}});
        held_.insert(owner);
    }
    QSaveFile save(registryPath_);
    if (save.open(QIODevice::WriteOnly)) {
        save.write(QJsonDocument(QJsonObject{{"version", 1}, {"turns", kept}}).toJson(QJsonDocument::Indented));
        save.commit();
    }
    return holder;
}

void TurnLocks::release(const QString &owner)
{
    if (!held_.remove(owner)) return;
    QLockFile guard(guardPath_);
    guard.setStaleLockTime(10000);
    if (guard.tryLock(2000)) {
        QFile file(registryPath_);
        QJsonArray kept;
        if (file.open(QIODevice::ReadOnly)) {
            for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value("turns").toArray()) {
                if (value.toObject().value("owner").toString() != owner) kept.append(value);
            }
        }
        file.close();
        QSaveFile save(registryPath_);
        if (save.open(QIODevice::WriteOnly)) {
            save.write(QJsonDocument(QJsonObject{{"version", 1}, {"turns", kept}}).toJson(QJsonDocument::Indented));
            save.commit();
        }
    }
    emit released();
}
