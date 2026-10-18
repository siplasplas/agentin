#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

QString appConfigDirectory()
{
    // Without the organization (the old places, used for a run that could not move them) the
    // configuration stays beside the data, as in older versions.
    if (QCoreApplication::organizationName().isEmpty()) return appDataDirectory();
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString appDataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

bool otherAgentinRunning()
{
    const qint64 self = QCoreApplication::applicationPid();
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        const qint64 pid = entry.toLongLong(&numeric);
        if (!numeric || pid == self) continue;
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == "agentin") return true;
    }
    return false;
}
