#include "CodexLocator.h"
#include "MainWindow.h"

#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

class MainWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void codexExecutableFromEnvironment();
    void desktopExecutableWithoutPath();
    void helpAndConversation();
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
    print("Usage: codex app-server [OPTIONS]", flush=True)
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
    QVERIFY(stopButton);

    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(output->toPlainText().contains("Commands:"));
    QTRY_VERIFY(output->toPlainText().contains("Usage: codex app-server [OPTIONS]"));
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
}

QTEST_MAIN(MainWindowTest)
#include "MainWindowTest.moc"
