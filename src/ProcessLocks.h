#pragma once

#include <QList>
#include <QString>

// Detects conversations held open by another process, so they can stay read-only here.

// Claude Code registers each running session in <config>/sessions/<pid>.json.
QString claudeSessionLock(const QString &configDirectory, const QString &sessionId);
// Heuristic for CLIs without a session registry: another process that names the session on its command line.
QString commandLineLock(const QString &sessionId);

// The process start time from /proc, which tells a live process from a later one with the same PID.
QString processStartTime(qint64 pid);
bool isProcessAlive(qint64 pid, const QString &startTime);

struct BusyAgentSession
{
    qint64 pid = 0;
    QString directory;
    QString description;
};
// Claude Code sessions outside this application that are working on a turn right now.
QList<BusyAgentSession> busyClaudeSessions(const QString &configDirectory);
