#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>

// Which program opens files whose name matches one of the patterns (space-separated wildcards such as
// "*.cpp CMakeLists.txt"). An empty command means the application the system associates with the type.
// A command may use %f for the file and %l for the line to show.
struct OpenRule
{
    QString patterns;
    QString command;
};

// An application found on this computer that can be chosen for a rule.
struct DetectedApplication
{
    QString name;
    QString command;
    // Where it was found, such as "JetBrains Toolbox" or "PATH".
    QString source;
};

namespace FileOpener {
// Common groups of file types, all opened as the system does until changed.
QList<OpenRule> defaultRules();
QList<OpenRule> rulesFromJson(const QJsonArray &array);
QJsonArray rulesToJson(const QList<OpenRule> &rules);
// IDEs installed by JetBrains Toolbox and editors and IDEs found in PATH.
QList<DetectedApplication> detectApplications();
// The name of the application the system opens such a file with, or an empty string.
QString systemApplication(const QString &fileName);
// The first rule whose pattern matches the file's name decides; without one, the system opens it.
// JetBrains IDEs pass the file to an instance that is already running instead of starting another.
bool open(const QString &path, int line, const QList<OpenRule> &rules);
}
