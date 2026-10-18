#include "CodexLocator.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QPushButton>
#include <QGuiApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QStandardPaths>
#include <backward.hpp>
#include <qce/kate/KatePaths.h>
#include <qxmigration.h>

#include <cstdio>

#include "AppPaths.h"

namespace {

// Files of older versions, all in ~/.local/share/agentin: configuration and data to their siplasplas
// places. The managed Claude environment has absolute paths in its scripts, so it is not moved: a new
// one is created in the data directory and the old one removed once that is ready.
QxMigrationSpec migrationSpec()
{
    QxMigrationSpec spec;
    spec.configEntries = {"settings.json", "approvals.json"};
    spec.dataEntries = {"codex-conversations.json", "claude-conversations.json", "glm-conversations.json",
                        "gemini-conversations.json", "antigravity-conversations.json", "recent-directories.json",
                        "open-tabs.json", "turn-locks.json", "turn-locks.lock", "agentin-crash.log"};
    spec.discardedEntries = {"claude-venv"};
    spec.recognize = [](const QString &path) {
        const QString name = QFileInfo(path).fileName();
        if (name != "settings.json" && name != "approvals.json") return true;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        return name == "settings.json" ? object.contains("version") : object.value("version").toInt() == 2;
    };
    return spec;
}

// Moves the files once; returns the lines to log. Sets *finished when the old managed Claude environment
// may be removed once the new one is ready. With another agentin running, or on a failure, the old
// places are used for this run and the next start tries again.
QStringList migrateFiles(bool *finished)
{
    QStringList log;
    *finished = false;
    if (otherAgentinRunning()) {
        QCoreApplication::setOrganizationName(QString());
        if (QFileInfo::exists(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)))
            log << "Moving files: another agentin runs; the files stay in the old place for this run";
    } else {
        const QxMigrationResult moved = qxMigrateToSiplasplas(migrationSpec());
        for (const QString &line : moved.log) log << "Moving files: " + line;
        if (moved.status == QxMigrationResult::Failed) QCoreApplication::setOrganizationName(QString());
        else *finished = true;
    }
    // The Kate syntax data read by DiffMerge's widgets is qcodeedit's, shared in ~/.local/share/kate-data.
    const qce::kate::DataRootMigration kate = qce::kate::migrateDataRoot();
    for (const QString &line : kate.log + kate.warnings) log << "Moving Kate data: " + line;
    return log;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Configuration in ~/.config/siplasplas/agentin, data in ~/.local/share/siplasplas/agentin.
    QCoreApplication::setOrganizationName("siplasplas");
    QCoreApplication::setApplicationName("agentin");
    QCoreApplication::setApplicationVersion(AGENTIN_VERSION);
    // On Wayland the desktop shows the icon of agentin.desktop, which this name points to.
    QGuiApplication::setDesktopFileName("agentin");
    QApplication::setWindowIcon(QIcon(":/icons/agentin.svg"));

    QCommandLineParser parser;
    parser.setApplicationDescription("Qt client for Codex, Claude, GLM, Gemini and Antigravity");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption cwdOption({"C", "cwd"}, "Conversation working directory.", "directory", QDir::currentPath());
    QCommandLineOption codexOption("codex", "Path to the codex executable.", "program", "codex");
    QCommandLineOption claudePythonOption("claude-python", "Python executable with claude-agent-sdk installed.", "program");
    QCommandLineOption claudeBridgeOption("claude-bridge", "Path to the Claude bridge script.", "script");
    QCommandLineOption geminiOption("gemini", "Path to the Gemini CLI executable.", "program", "gemini");
    QCommandLineOption antigravityOption("antigravity", "Path to the Antigravity CLI executable.", "program", "agy");
    parser.addOption(cwdOption);
    parser.addOption(codexOption);
    parser.addOption(claudePythonOption);
    parser.addOption(claudeBridgeOption);
    parser.addOption(geminiOption);
    parser.addOption(antigravityOption);
    parser.process(app);

    // Before the crash log is opened, since it moves too; its lines are written to the log afterwards.
    bool removeOldEnvironment = false;
    const QStringList migrationLog = migrateFiles(&removeOldEnvironment);

    // Diagnostics and, after a fatal signal or an unhandled exception, backward-cpp's stack trace go
    // to the crash log instead of the terminal.
    const QString dataDirectory = appDataDirectory();
    QDir().mkpath(dataDirectory);
    const QString crashLog = QDir(dataDirectory).filePath("agentin-crash.log");
    if (std::freopen(crashLog.toLocal8Bit().constData(), "a", stderr)) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::fprintf(stderr, "\n=== agentin %s, started %s ===\n", qPrintable(QCoreApplication::applicationVersion()),
                     qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)));
    }
    for (const QString &line : migrationLog) std::fprintf(stderr, "%s\n", qPrintable(line));
    static backward::SignalHandling crashHandler;

    const QString codexProgram = parser.isSet(codexOption) ? parser.value(codexOption) : locateCodex();
    QString claudePython = parser.value(claudePythonOption);
    const QString localPython = QDir(QCoreApplication::applicationDirPath()).filePath("../.venv/bin/python");
    if (claudePython.isEmpty() && QFileInfo(localPython).isExecutable())
        claudePython = QFileInfo(localPython).absoluteFilePath();
    // The build copies the bridge next to the executable; an installation puts it in ../share/agentin.
    QString claudeScript = parser.value(claudeBridgeOption);
    const QDir executableDirectory(QCoreApplication::applicationDirPath());
    if (claudeScript.isEmpty()) {
        claudeScript = executableDirectory.filePath("claude_bridge.py");
        const QString installed = executableDirectory.filePath("../share/agentin/claude_bridge.py");
        if (!QFileInfo::exists(claudeScript) && QFileInfo::exists(installed))
            claudeScript = QFileInfo(installed).absoluteFilePath();
    }
    // Command rules moved from settings.json to approvals.json. Settings of an earlier version are not migrated:
    // agentin stops and names the file, so that the old section or the whole file can be removed.
    const QString settingsPath = QDir(appConfigDirectory()).filePath("settings.json");
    QFile settingsFile(settingsPath);
    if (settingsFile.open(QIODevice::ReadOnly)) {
        const QJsonObject settings = QJsonDocument::fromJson(settingsFile.readAll()).object();
        QStringList old;
        for (const QString &key : {QStringLiteral("trustedCommands"), QStringLiteral("commandRules")})
            if (settings.contains(key)) old.append("\"" + key + "\"");
        if (!old.isEmpty()) {
            const QString text = "agentin cannot start: " + QDir::toNativeSeparators(settingsPath) + " contains the section "
                + old.join(" and ") + " of an earlier version. Command rules are now kept in approvals.json next to it. "
                "Remove that section from the file, or remove the whole file, and start agentin again.";
            // The message can be selected, or copied whole, for example to find the file.
            QMessageBox box(QMessageBox::Critical, "agentin", text, QMessageBox::Close);
            box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            QPushButton *copy = box.addButton("Copy", QMessageBox::ActionRole);
            // Copy keeps the box open.
            copy->disconnect();
            QObject::connect(copy, &QPushButton::clicked, &box, [text] { QGuiApplication::clipboard()->setText(text); });
            box.exec();
            return 1;
        }
    }
    MainWindow window(codexProgram, QDir(parser.value(cwdOption)).absolutePath(), claudePython, claudeScript,
                      parser.value(geminiOption), nullptr, {}, parser.value(antigravityOption));
    // The old managed Claude environment (and the old directory, then empty) goes once the new one works.
    if (removeOldEnvironment)
        window.setWhenClaudeEnvironmentReady([] { qxFinishMigration(migrationSpec()); });
    if (claudePython.isEmpty()) window.useManagedClaudeEnvironment();
    // A directory given with -C keeps its new chat next to the reopened tabs.
    window.restoreSession(parser.isSet(cwdOption));
    window.show();
    return app.exec();
}
