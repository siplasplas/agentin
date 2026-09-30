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

// The directories a turn holds. Shared temporary directories are left out: every agent may write
// there, so holding them would let only one turn run at a time.
QStringList lockable(const QStringList &directories)
{
    const QStringList shared{canonical("/tmp"), canonical(QDir::tempPath())};
    QStringList result;
    for (const QString &directory : directories) {
        const QString canonicalPath = canonical(directory);
        if (directory.isEmpty() || canonicalPath == "/" || shared.contains(canonicalPath)
            || result.contains(canonicalPath)) continue;
        result.append(canonicalPath);
    }
    return result;
}

bool overlapsAny(const QStringList &left, const QStringList &right)
{
    for (const QString &a : left) {
        for (const QString &b : right) {
            if (overlaps(a, b)) return true;
        }
    }
    return false;
}

// Names the other directories a holder may write to, so it is clear why a directory outside its
// working directory is taken.
QString alsoWrites(const QStringList &writable)
{
    return writable.isEmpty() ? QString() : " (it may also write to " + writable.join(", ") + ")";
}

QStringList entryDirectories(const QJsonObject &entry)
{
    QStringList directories;
    for (const QJsonValue &value : entry.value("directories").toArray()) directories.append(value.toString());
    return directories;
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

QString TurnLocks::acquire(const QString &owner, const QStringList &directories, const QString &label)
{
    QDir().mkpath(QFileInfo(registryPath_).absolutePath());
    QLockFile guard(guardPath_);
    guard.setStaleLockTime(10000);
    if (!guard.tryLock(2000)) return "another agentdeskt window that is updating the lock registry";

    const qint64 self = QCoreApplication::applicationPid();
    const QStringList paths = lockable(directories);
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
        if (holder.isEmpty() && overlapsAny(paths, entryDirectories(entry))) {
            const QStringList held = entryDirectories(entry);
            holder = entry.value("label").toString() + " in " + held.value(0) + alsoWrites(held.mid(1));
            if (pid != self) holder += QString(" in another agentdeskt window (PID %1)").arg(pid);
        }
    }
    if (holder.isEmpty()) {
        const QString claudeConfig = qEnvironmentVariable("CLAUDE_CONFIG_DIR", QDir::home().filePath(".claude"));
        for (const BusyAgentSession &session : busyClaudeSessions(claudeConfig)) {
            if (!overlapsAny(paths, lockable(QStringList{session.directory} + session.writable))) continue;
            holder = session.description + " in " + session.directory + alsoWrites(lockable(session.writable));
            break;
        }
    }
    if (holder.isEmpty()) {
        const QString codexHome = qEnvironmentVariable("CODEX_HOME", QDir::home().filePath(".codex"));
        for (const BusyAgentSession &session : runningCodexTurns(codexHome)) {
            if (!overlapsAny(paths, lockable(QStringList{session.directory} + session.writable))) continue;
            holder = session.description + " in " + session.directory + alsoWrites(lockable(session.writable));
            break;
        }
    }
    if (holder.isEmpty()) {
        for (const BusyAgentSession &session : runningCliSessions()) {
            if (!overlapsAny(paths, lockable(QStringList{session.directory} + session.writable))) continue;
            holder = session.description + " in " + session.directory + "; close it so this chat can continue";
            break;
        }
    }
    if (holder.isEmpty()) {
        kept.append(QJsonObject{{"owner", owner}, {"directories", QJsonArray::fromStringList(paths)}, {"label", label},
                                {"pid", self}, {"start", processStartTime(self)}});
        held_.insert(owner);
    }
    QSaveFile save(registryPath_);
    if (save.open(QIODevice::WriteOnly)) {
        save.write(QJsonDocument(QJsonObject{{"version", 2}, {"turns", kept}}).toJson(QJsonDocument::Indented));
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
            save.write(QJsonDocument(QJsonObject{{"version", 2}, {"turns", kept}}).toJson(QJsonDocument::Indented));
            save.commit();
        }
    }
    emit released();
}
