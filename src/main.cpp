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

#include <cstdio>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
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

    // Diagnostics and, after a fatal signal or an unhandled exception, backward-cpp's stack trace go
    // to the crash log instead of the terminal.
    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDirectory);
    const QString crashLog = QDir(dataDirectory).filePath("agentin-crash.log");
    if (std::freopen(crashLog.toLocal8Bit().constData(), "a", stderr)) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::fprintf(stderr, "\n=== agentin %s, started %s ===\n", qPrintable(QCoreApplication::applicationVersion()),
                     qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)));
    }
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
    const QString settingsPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("settings.json");
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
    if (claudePython.isEmpty()) window.useManagedClaudeEnvironment();
    // A directory given with -C keeps its new chat next to the reopened tabs.
    window.restoreSession(parser.isSet(cwdOption));
    window.show();
    return app.exec();
}
