#include "CodexLocator.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("agentin");
    QCoreApplication::setApplicationVersion("0.1.0");

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
