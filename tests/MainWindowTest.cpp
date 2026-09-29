#include "CodexLocator.h"
#include "MainWindow.h"

#include <QFile>
#include <QFileInfo>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QtTest>

class MainWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void codexExecutableFromEnvironment();
    void desktopExecutableWithoutPath();
    void helpAndConversation();
    void claudeConversationAndStop();
};

void MainWindowTest::codexExecutableFromEnvironment()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = directory.filePath("codex-test");
    QFile file(executable);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("#!/bin/sh\nexit 0\n");
    file.close();
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QByteArray previous = qgetenv("CODEX_BIN");
    qputenv("CODEX_BIN", executable.toLocal8Bit());
    const QString found = locateCodex();
    if (previous.isNull()) qunsetenv("CODEX_BIN");
    else qputenv("CODEX_BIN", previous);
    QCOMPARE(found, executable);
}

void MainWindowTest::desktopExecutableWithoutPath()
{
    const QString desktopCodex = "/usr/lib/chatgpt/resources/codex";
    if (!QFileInfo(desktopCodex).isExecutable()) QSKIP("Codex desktop executable is not installed here");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousCodexBin = qgetenv("CODEX_BIN");
    qputenv("PATH", directory.path().toLocal8Bit());
    qunsetenv("CODEX_BIN");
    const QString found = locateCodex();
    if (previousPath.isNull()) qunsetenv("PATH");
    else qputenv("PATH", previousPath);
    if (previousCodexBin.isNull()) qunsetenv("CODEX_BIN");
    else qputenv("CODEX_BIN", previousCodexBin);
    QCOMPARE(found, desktopCodex);
}

void MainWindowTest::helpAndConversation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeServer = directory.filePath("fake-codex");
    QFile script(fakeServer);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(#!/usr/bin/env python3
import json
import sys

if "--help" in sys.argv:
    print("Usage: codex app-server [OPTIONS] [COMMAND]", flush=True)
    print("Commands:\n  daemon  Manage local daemon\n  proxy  Connect to local daemon", flush=True)
    print("Options:\n  --stdio  Use stdio transport", flush=True)
    sys.exit(0)

def send(message):
    print(json.dumps(message), flush=True)

for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if method == "initialize":
        send({"id": request["id"], "result": {"userAgent": "fake"}})
    elif method == "thread/start":
        send({"id": request["id"], "result": {"thread": {"id": "test-thread"}}})
    elif method == "turn/start":
        send({"id": request["id"], "result": {"turn": {"id": "test-turn"}}})
        if request["params"]["input"][0]["text"] == "check roots":
            roots = request["params"].get("sandboxPolicy", {}).get("writableRoots", [])
            send({"method": "item/completed", "params": {
                "item": {"id": "roots", "type": "agentMessage", "text": "Roots: " + ", ".join(roots)}}})
            send({"method": "turn/completed", "params": {
                "turn": {"id": "test-turn", "status": "completed"}}})
            continue
        if request["params"]["input"][0]["text"].startswith("long"):
            send({"method": "turn/started", "params": {"turn": {"id": "test-turn"}}})
            continue
        send({"method": "item/agentMessage/delta", "params": {
            "itemId": "test-item", "delta": "Hello from App Server"}})
        send({"method": "item/completed", "params": {
            "item": {"id": "test-item", "type": "agentMessage", "text": "Hello from App Server"}}})
        send({"method": "turn/completed", "params": {
            "turn": {"id": "test-turn", "status": "completed"}}})
    elif method == "turn/interrupt":
        send({"id": request["id"], "result": {}})
        send({"method": "turn/completed", "params": {
            "turn": {"id": "test-turn", "status": "interrupted"}}})
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    MainWindow window(fakeServer, directory.path());
    window.show();
    auto *input = window.findChild<QLineEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("output");
    QVERIFY(input);
    QVERIFY(output);
    auto *stopButton = window.findChild<QPushButton *>("stopButton");
    auto *addDirButton = window.findChild<QPushButton *>("addDirectoryButton");
    QVERIFY(stopButton);
    QVERIFY(addDirButton);

    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(output->toPlainText().contains("Commands:"));
    QTRY_VERIFY(output->toPlainText().contains("Codex connection: codex app-server --stdio"));
    QTRY_VERIFY(output->toPlainText().contains("--stdio  Use stdio transport"));
    QVERIFY(output->toPlainText().contains("Manage local daemon"));
    QTRY_VERIFY(output->toPlainText().contains("[Connected to Codex]"));

    QTest::keyClicks(input, "test");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().contains("Codex: Hello from App Server"));
    QCOMPARE(output->toPlainText().count("Hello from App Server"), 1);

    QTest::keyClicks(input, "long response");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(stopButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY(output->toPlainText().contains("[Response: interrupted]"));

    QTest::keyClicks(input, "long race");
    QTest::keyClick(input, Qt::Key_Return);
    QTest::keyClicks(input, "stop");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().count("[Response: interrupted]") == 2);

    QTemporaryDir extraDirectory;
    QVERIFY(extraDirectory.isValid());
    QVERIFY(QMetaObject::invokeMethod(&window, "addCodexDirectory", Q_ARG(QString, extraDirectory.path())));
    QTest::keyClicks(input, "check roots");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().contains("Roots: " + directory.path() + ", " + extraDirectory.path()));
}

void MainWindowTest::claudeConversationAndStop()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python 3 is unavailable");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeBridge = directory.filePath("fake-claude.py");
    QFile script(fakeBridge);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(import json
import sys

def send(message):
    print(json.dumps(message), flush=True)

send({"type": "ready"})
for line in sys.stdin:
    request = json.loads(line)
    kind = request.get("type")
    if kind == "prompt":
        if request["text"].startswith("long"):
            continue
        send({"type": "delta", "text": "Hello from Claude"})
        send({"type": "complete", "status": "completed"})
    elif kind == "stop":
        send({"type": "complete", "status": "interrupted"})
    elif kind == "new":
        send({"type": "ready"})
    elif kind == "add_directory":
        send({"type": "ready"})
        send({"type": "directory_added", "path": request["path"]})
    elif kind == "shutdown":
        break
)PY");
    script.close();

    const QString fakeClaude = directory.filePath("claude");
    QFile cli(fakeClaude);
    QVERIFY(cli.open(QIODevice::WriteOnly | QIODevice::Text));
    cli.write("#!/bin/sh\nprintf 'Usage: claude [OPTIONS]\\n'\n");
    cli.close();
    QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    MainWindow window("/nonexistent/codex", directory.path(), python, fakeBridge);
    window.show();
    auto *provider = window.findChild<QComboBox *>("providerSelect");
    auto *input = window.findChild<QLineEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("output");
    auto *stopButton = window.findChild<QPushButton *>("stopButton");
    auto *sendButton = window.findChild<QPushButton *>("sendButton");
    auto *addDirButton = window.findChild<QPushButton *>("addDirectoryButton");
    QVERIFY(provider);
    QVERIFY(input);
    QVERIFY(output);
    QVERIFY(stopButton);
    QVERIFY(sendButton);
    QVERIFY(addDirButton);
    provider->setCurrentIndex(1);
    QTRY_VERIFY(output->toPlainText().contains("[Connected to Claude Agent SDK]"));
    QCOMPARE(sendButton->text(), QString("Send to Claude"));
    QVERIFY(addDirButton->isVisible());
    QVERIFY(addDirButton->isEnabled());

    const QByteArray previousPath = qgetenv("PATH");
    qputenv("PATH", directory.path().toLocal8Bit());
    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return);
    if (previousPath.isNull()) qunsetenv("PATH");
    else qputenv("PATH", previousPath);
    QVERIFY(output->toPlainText().contains("Claude Agent SDK:"));
    QVERIFY(output->toPlainText().contains("Tool approvals and questions appear in dialogs."));
    QTRY_VERIFY(output->toPlainText().contains("Usage: claude [OPTIONS]"));

    QTest::keyClicks(input, "hello");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().contains("Claude: Hello from Claude"));

    QTest::keyClicks(input, "long response");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(stopButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY(output->toPlainText().contains("[Claude response: interrupted]"));

    QTest::keyClicks(input, "new");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().count("[Connected to Claude Agent SDK]") == 2);

    QTemporaryDir extraDirectory;
    QVERIFY(extraDirectory.isValid());
    QVERIFY(QMetaObject::invokeMethod(&window, "addClaudeDirectory", Q_ARG(QString, extraDirectory.path())));
    QTRY_VERIFY(output->toPlainText().contains("[Claude directory available: " + extraDirectory.path() + "]"));
    auto *directoriesLabel = window.findChild<QLabel *>("claudeDirectories");
    QVERIFY(directoriesLabel);
    QVERIFY(directoriesLabel->text().contains(extraDirectory.path()));
}

QTEST_MAIN(MainWindowTest)
#include "MainWindowTest.moc"
