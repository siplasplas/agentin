#include "CodexLocator.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
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
    MainWindow window(codexProgram, QDir(parser.value(cwdOption)).absolutePath(), claudePython, claudeScript,
                      parser.value(geminiOption), nullptr, {}, parser.value(antigravityOption));
    if (claudePython.isEmpty()) window.useManagedClaudeEnvironment();
    // A directory given with -C keeps its new chat next to the reopened tabs.
    window.restoreSession(parser.isSet(cwdOption));
    window.show();
    return app.exec();
}
