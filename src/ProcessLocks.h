#pragma once

#include <QList>
#include <QString>
#include <QStringList>

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
    // Other directories the session may write to.
    QStringList writable;
    QString description;
};
// Claude Code sessions outside this application that are working on a turn right now.
QList<BusyAgentSession> busyClaudeSessions(const QString &configDirectory);
// Gemini CLI and Antigravity CLI processes outside this application, found through /proc. They publish
// no turn state, so an interactive CLI counts as working for as long as it runs.
QList<BusyAgentSession> runningCliSessions();
// Codex turns running outside this application. Codex appends each session to
// <codexHome>/sessions/.../rollout-*.jsonl, which the process serving it (usually the App Server
// daemon) keeps open; a session whose last turn started but has not completed or been aborted is
// working. Turns in Codex's read-only sandbox are not counted.
QList<BusyAgentSession> runningCodexTurns(const QString &codexHome);
