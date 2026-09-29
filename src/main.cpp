#include "CodexLocator.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("agentdeskt");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Simple Qt client for Codex App Server");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption cwdOption({"C", "cwd"}, "Conversation working directory.", "directory", QDir::currentPath());
    QCommandLineOption codexOption("codex", "Path to the codex executable.", "program", "codex");
    parser.addOption(cwdOption);
    parser.addOption(codexOption);
    parser.process(app);

    const QString codexProgram = parser.isSet(codexOption) ? parser.value(codexOption) : locateCodex();
    MainWindow window(codexProgram, QDir(parser.value(cwdOption)).absolutePath());
    window.show();
    return app.exec();
}
