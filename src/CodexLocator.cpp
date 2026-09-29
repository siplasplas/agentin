#include "CodexLocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

QString locateCodex()
{
    const QString configured = qEnvironmentVariable("CODEX_BIN");
    if (!configured.isEmpty()) return configured;

    const QString fromPath = QStandardPaths::findExecutable("codex");
    if (!fromPath.isEmpty()) return fromPath;

    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + "/codex",
        "/usr/lib/chatgpt/resources/codex",
        QDir::homePath() + "/.local/bin/codex"
    };
    for (const QString &candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.isFile() && file.isExecutable()) return candidate;
    }
    return "codex";
}
