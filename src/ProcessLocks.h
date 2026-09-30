#pragma once

#include <QString>

// Detects conversations held open by another process, so they can stay read-only here.

// Claude Code registers each running session in <config>/sessions/<pid>.json.
QString claudeSessionLock(const QString &configDirectory, const QString &sessionId);
// Heuristic for CLIs without a session registry: another process that names the session on its command line.
QString commandLineLock(const QString &sessionId);
