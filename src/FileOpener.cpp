#include "FileOpener.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace {
// JetBrains IDEs take --line, and their launcher hands the file to a running instance.
const QStringList kJetBrainsTools{"clion", "rustrover", "idea", "pycharm", "webstorm", "goland", "rider",
                                  "phpstorm", "rubymine", "datagrip", "studio", "fleet"};

QString jetBrainsCommand(const QString &program)
{
    return "\"" + program + "\" --line %l %f";
}

// A key of a desktop entry's main group, read as written; QSettings would treat quotes and commas specially.
QString desktopEntryValue(const QString &file, const QString &key)
{
    QFile entry(file);
    if (!entry.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    bool inMainGroup = false;
    while (!entry.atEnd()) {
        const QString line = QString::fromUtf8(entry.readLine()).trimmed();
        if (line.startsWith('[')) inMainGroup = line == "[Desktop Entry]";
        else if (inMainGroup && line.startsWith(key + '=')) return line.mid(key.size() + 1);
    }
    return {};
}

// The program of an Exec line, which may be quoted.
QString execProgram(const QString &exec)
{
    const QStringList parts = QProcess::splitCommand(exec);
    return parts.value(0);
}
}

namespace FileOpener {

QList<OpenRule> defaultRules()
{
    return {{"*.c *.cc *.cpp *.cxx *.h *.hh *.hpp *.hxx CMakeLists.txt *.cmake", {}},
            {"*.rs Cargo.toml", {}},
            {"*.py", {}},
            {"*.md", {}},
            {"*.json *.yaml *.yml *.toml *.ini", {}},
            {"*", {}}};
}

QList<OpenRule> rulesFromJson(const QJsonArray &array)
{
    QList<OpenRule> rules;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        if (!object.value("patterns").toString().trimmed().isEmpty())
            rules.append({object.value("patterns").toString(), object.value("command").toString()});
    }
    return rules.isEmpty() ? defaultRules() : rules;
}

QJsonArray rulesToJson(const QList<OpenRule> &rules)
{
    QJsonArray array;
    for (const OpenRule &rule : rules) array.append(QJsonObject{{"patterns", rule.patterns}, {"command", rule.command}});
    return array;
}

QList<DetectedApplication> detectApplications()
{
    QList<DetectedApplication> found;
    QStringList names;
    // Toolbox writes a desktop entry for each installed IDE; old versions can leave entries without a program.
    const QString applications = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    for (const QFileInfo &file : QDir(applications).entryInfoList({"jetbrains-*.desktop"}, QDir::Files, QDir::Name)) {
        const QString name = desktopEntryValue(file.filePath(), "Name");
        const QString program = execProgram(desktopEntryValue(file.filePath(), "Exec"));
        const QString tool = QFileInfo(program).fileName();
        // IDEs unpacked by Gradle for plugin development also get desktop entries; they are not installations.
        if (name.isEmpty() || names.contains(name) || !kJetBrainsTools.contains(tool) || !QFileInfo(program).isExecutable()
            || program.contains("/.gradle/"))
            continue;
        names.append(name);
        found.append({name, jetBrainsCommand(program), program.contains("/JetBrains/Toolbox/") ? "JetBrains Toolbox" : "desktop entry"});
    }
    for (const QString &tool : kJetBrainsTools) {
        const QString program = QStandardPaths::findExecutable(tool);
        if (program.isEmpty()) continue;
        // Toolbox's scripts in PATH start the IDEs already found through their desktop entries.
        bool known = false;
        for (const DetectedApplication &application : found)
            if (QFileInfo(execProgram(application.command)).fileName() == tool) known = true;
        if (!known) found.append({tool, jetBrainsCommand(program), "PATH"});
    }
    const QList<QPair<QString, QString>> editors{{"code", "code --goto %f:%l"}, {"codium", "codium --goto %f:%l"},
                                                {"kate", "kate --line %l %f"}, {"kwrite", "kwrite --line %l %f"},
                                                {"gedit", "gedit +%l %f"}, {"subl", "subl %f:%l"}, {"zed", "zed %f:%l"}};
    for (const auto &[tool, command] : editors)
        if (!QStandardPaths::findExecutable(tool).isEmpty()) found.append({tool, command, "PATH"});
    return found;
}

QString systemApplication(const QString &fileName)
{
    const QMimeType type = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    QProcess query;
    query.start("xdg-mime", {"query", "default", type.name()});
    if (!query.waitForFinished(2000)) return {};
    const QString id = QString::fromUtf8(query.readAllStandardOutput()).trimmed();
    if (id.isEmpty()) return {};
    for (const QString &directory : QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation)) {
        const QString file = QDir(directory).filePath(id);
        if (QFileInfo::exists(file)) {
            const QString name = desktopEntryValue(file, "Name");
            return name.isEmpty() ? id : name;
        }
    }
    return id;
}

bool open(const QString &path, int line, const QList<OpenRule> &rules)
{
    const QString fileName = QFileInfo(path).fileName();
    QString command;
    for (const OpenRule &rule : rules) {
        bool matches = false;
        for (const QString &pattern : rule.patterns.split(' ', Qt::SkipEmptyParts))
            if (QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive).match(fileName).hasMatch()) matches = true;
        if (!matches) continue;
        command = rule.command.trimmed();
        break;
    }
    if (command.isEmpty()) return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    QStringList arguments = QProcess::splitCommand(command);
    bool hasFile = false;
    for (QString &argument : arguments) {
        if (argument.contains("%f")) hasFile = true;
        argument.replace("%f", path).replace("%l", QString::number(qMax(1, line)));
    }
    if (!hasFile) arguments.append(path);
    const QString program = arguments.takeFirst();
    return QProcess::startDetached(program, arguments, QFileInfo(path).absolutePath());
}

}
