#include "CodexLocator.h"
#include "MainWindow.h"

#include <QFile>
#include <QFileInfo>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
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
    void glmConversation();
    void geminiConversation();
    void antigravityConversationResumesAndPersists();
    void codexConversationIndexSurvivesRestart();
    void claudeSessionMetadataRefreshesExistingIndex();
    void missingGeminiCliReportsOneDiscoveryError();
    void geminiListsSessionsFromAllProjects();
    void claudeAttachRespectsExternalLock();
    void geminiAttachRespectsExternalLock();
};

// Starts a chat through the New chat dialog, choosing the agent and working directory there.
static void startChat(MainWindow &window, int provider, const QString &path)
{
    auto *newChatButton = window.findChild<QPushButton *>("newChatButton");
    QVERIFY(newChatButton);
    bool accepted = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>();
        QVERIFY(dialog);
        auto *providerInput = dialog->findChild<QComboBox *>("newConversationProvider");
        auto *pathInput = dialog->findChild<QLineEdit *>("newConversationPath");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(providerInput && pathInput && buttons);
        QCOMPARE(providerInput->count(), 5);
        providerInput->setCurrentIndex(provider);
        pathInput->setText(path);
        buttons->button(QDialogButtonBox::Ok)->click();
        accepted = true;
    });
    QTest::mouseClick(newChatButton, Qt::LeftButton);
    QVERIFY(accepted);
}

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
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    QVERIFY(input);
    QVERIFY(output);
    auto *stopButton = window.findChild<QPushButton *>("stopButton");
    QVERIFY(stopButton);
    QVERIFY(!window.findChild<QPushButton *>("addDirectoryButton"));
    QVERIFY(!window.findChild<QComboBox *>("providerSelect"));

    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(output->toPlainText().contains("Commands:"));
    QTRY_VERIFY(output->toPlainText().contains("Codex connection: codex app-server --stdio"));
    QTRY_VERIFY(output->toPlainText().contains("--stdio  Use stdio transport"));
    QVERIFY(output->toPlainText().contains("Manage local daemon"));
    QVERIFY(!output->toPlainText().contains("[Connected to Codex]"));

    QTest::keyClicks(input, "test");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().contains("[Connected to Codex]"));
    QTRY_VERIFY(chat->toPlainText().contains("Codex: Hello from App Server"));
    QCOMPARE(chat->toPlainText().count("Hello from App Server"), 1);
    QVERIFY(!output->toPlainText().contains("Hello from App Server"));

    QTest::keyClicks(input, "long response");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(stopButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY(output->toPlainText().contains("[Codex response: interrupted]"));

    QTest::keyClicks(input, "long race");
    QTest::keyClick(input, Qt::Key_Return);
    QTest::keyClicks(input, "stop");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().count("[Codex response: interrupted]") == 2);
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
    auto *input = window.findChild<QLineEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    auto *stopButton = window.findChild<QPushButton *>("stopButton");
    auto *sendButton = window.findChild<QPushButton *>("sendButton");
    QVERIFY(input);
    QVERIFY(output);
    QVERIFY(stopButton);
    QVERIFY(sendButton);
    startChat(window, 1, directory.path());
    QTRY_VERIFY(output->toPlainText().contains("[Connected to Claude Agent SDK]"));
    QCOMPARE(sendButton->text(), QString("Send to Claude"));

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
    QTRY_VERIFY(chat->toPlainText().contains("Claude: Hello from Claude"));

    QTest::keyClicks(input, "long response");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(stopButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY(output->toPlainText().contains("[Claude response: interrupted]"));

    bool rejectedMissingDirectory = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>();
        QVERIFY(dialog);
        auto *path = dialog->findChild<QLineEdit *>("newConversationPath");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(path);
        QVERIFY(buttons);
        path->setText(directory.filePath("missing"));
        rejectedMissingDirectory = !buttons->button(QDialogButtonBox::Ok)->isEnabled();
        path->setText(fakeBridge);
        rejectedMissingDirectory = rejectedMissingDirectory
            && !buttons->button(QDialogButtonBox::Ok)->isEnabled();
        path->setText(directory.path());
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    QTest::keyClicks(input, "new");
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(rejectedMissingDirectory);
    QTRY_VERIFY(output->toPlainText().count("[Connected to Claude Agent SDK]") == 2);
}

void MainWindowTest::glmConversation()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python 3 is unavailable");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeBridge = directory.filePath("fake-glm.py");
    QFile script(fakeBridge);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(import json
import sys

assert sys.argv[sys.argv.index("--provider") + 1] == "glm"
print(json.dumps({"type": "ready"}), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    if request["type"] == "prompt":
        print(json.dumps({"type": "delta", "text": "Hello from GLM"}), flush=True)
        print(json.dumps({"type": "complete", "status": "completed"}), flush=True)
    elif request["type"] == "shutdown":
        break
)PY");
    script.close();
    MainWindow window("/nonexistent/codex", directory.path(), python, fakeBridge);
    window.show();
    auto *input = window.findChild<QLineEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    QVERIFY(input);
    QVERIFY(output);
    startChat(window, 2, directory.path());
    QTRY_VERIFY(output->toPlainText().contains("[Connected to GLM via Claude Agent SDK]"));
    QTest::keyClicks(input, "hello");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(chat->toPlainText().contains("GLM: Hello from GLM"));
}

void MainWindowTest::antigravityConversationResumesAndPersists()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeAgy = directory.filePath("agy");
    QFile script(fakeAgy);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(#!/usr/bin/env python3
import json
import sys

args = sys.argv[1:]
if "--help" in args:
    print("Usage: agy --prompt TEXT --conversation ID", flush=True)
    raise SystemExit(0)
assert args[:2] == ["--output-format", "stream-json"]
prompt = args[args.index("--prompt") + 1]
if prompt == "second":
    assert args[args.index("--conversation") + 1] == "agy-test-id"
else:
    assert "--conversation" not in args
print(json.dumps({"event": "init", "conversation_id": "agy-test-id"}), flush=True)
print(json.dumps({"event": "step_update", "step_update": {
    "step_type": "agent_response", "state": "DONE", "text_delta": prompt + " answered"
}}), flush=True)
print(json.dumps({"event": "result", "result": {
    "conversation_id": "agy-test-id", "status": "SUCCESS", "response": prompt + " answered"
}}), flush=True)
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString indexPath = directory.filePath("codex-conversations.json");
    MainWindow window("/nonexistent/codex", directory.path(), {}, {}, "gemini", nullptr, indexPath, fakeAgy);
    auto *input = window.findChild<QLineEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    auto *tree = window.findChild<QTreeWidget *>("conversationTree");
    QVERIFY(input && output && tree);
    startChat(window, 4, directory.path());
    input->setText("help");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().contains("Usage: agy"));
    input->setText("first");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(chat->toPlainText().contains("Antigravity: first answered"));
    QTRY_VERIFY(QFileInfo(directory.filePath("antigravity-conversations.json")).exists());
    input->setText("second");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(chat->toPlainText().contains("Antigravity: second answered"));
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (tree->topLevelItem(i)->text(0) == "Antigravity") {
            tree->topLevelItem(i)->setExpanded(true);
            QTRY_COMPARE(tree->topLevelItem(i)->childCount(), 1);
            QCOMPARE(tree->topLevelItem(i)->child(0)->child(0)->data(0, Qt::UserRole + 1).toString(),
                     QString("agy-test-id"));
        }
    }
}

void MainWindowTest::geminiConversation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeGemini = directory.filePath("fake-gemini");
    QFile script(fakeGemini);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(#!/usr/bin/env python3
import json
import sys

args = sys.argv[1:]
if "--help" in args:
    print("Usage: gemini [--resume ID]", flush=True)
    sys.exit(0)
if "--resume" in args:
    assert args[args.index("--resume") + 1] == "gemini-test-session"
prompt = args[args.index("--prompt") + 1]
print(json.dumps({"type": "init", "session_id": "gemini-test-session", "model": "fake"}), flush=True)
if prompt == "long":
    import time
    time.sleep(10)
else:
    print(json.dumps({"type": "message", "role": "assistant", "content": prompt + " answered", "delta": True}), flush=True)
    print(json.dumps({"type": "result", "status": "success"}), flush=True)
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    MainWindow window("/nonexistent/codex", directory.path(), {}, {}, fakeGemini);
    window.show();
    auto *input = window.findChild<QLineEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    auto *stopButton = window.findChild<QPushButton *>("stopButton");
    QVERIFY(input);
    QVERIFY(output);
    QVERIFY(stopButton);
    startChat(window, 3, directory.path());
    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(output->toPlainText().contains("Usage: gemini"));
    QTest::keyClicks(input, "first");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(chat->toPlainText().contains("Gemini: first answered"));
    QTest::keyClicks(input, "second");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(chat->toPlainText().contains("Gemini: second answered"));
    QTest::keyClicks(input, "long");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(stopButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY(output->toPlainText().contains("[Gemini response: interrupted]"));
}

void MainWindowTest::codexConversationIndexSurvivesRestart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeServer = directory.filePath("fake-codex-index");
    QFile script(fakeServer);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(#!/usr/bin/env python3
import json
import os
import sys

folder = os.path.dirname(__file__)
for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if method == "initialize":
        result = {}
    elif method == "thread/list":
        params = request["params"]
        assert "appServer" in params["sourceKinds"]
        with open(os.path.join(folder, "threads.json")) as source:
            state = json.load(source)
        items = state["archived" if params["archived"] else "active"]
        offset = int(params.get("cursor", "0"))
        page = items[offset:offset + 2]
        next_offset = offset + len(page)
        result = {"data": page, "nextCursor": str(next_offset) if next_offset < len(items) else None}
        with open(os.path.join(folder, "requests.log"), "a") as log:
            log.write(("archived" if params["archived"] else "active") + "\n")
    elif method == "thread/resume":
        if request["params"]["threadId"] == "new-101":
            print(json.dumps({"id": request["id"], "error": {"code": -32000,
                "message": "thread is in use by another client"}}), flush=True)
            continue
        result = {"thread": {"id": request["params"]["threadId"]}}
    elif method == "thread/items/list":
        params = request["params"]
        assert params["sortDirection"] == "desc"
        def user(text):
            return {"turnId": "t", "item": {"type": "userMessage", "id": text, "content": [{"type": "text", "text": text}]}}
        def agent(text):
            return {"turnId": "t", "item": {"type": "agentMessage", "id": text, "text": text}}
        if params.get("cursor") == "older":
            result = {"data": [agent("Old answer"), user("Old question")], "nextCursor": None}
        else:
            command = {"turnId": "t", "item": {"type": "commandExecution", "id": "c", "command": "ls"}}
            result = {"data": [agent("Latest answer"), command, user("Latest question")], "nextCursor": "older"}
    elif method == "turn/start":
        with open(os.path.join(folder, "requests.log"), "a") as log:
            log.write("turn\n")
        continue
    else:
        continue
    print(json.dumps({"id": request["id"], "result": result}), flush=True)
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString projectPath = directory.filePath("project");
    QVERIFY(QDir().mkpath(projectPath));
    auto writeState = [&](bool includeNew) {
        QJsonArray active;
        if (includeNew) {
            active.append(QJsonObject{{"id", "new-102"}, {"createdAt", 102}, {"cwd", projectPath},
                                      {"preview", QString(300, 'N')}});
            active.append(QJsonObject{{"id", "new-101"}, {"createdAt", 101}, {"cwd", projectPath}, {"preview", "New"}});
        }
        for (int date = 100; date >= 96; --date) {
            active.append(QJsonObject{{"id", QString("old-%1").arg(date)}, {"createdAt", date},
                                       {"cwd", projectPath}, {"preview", "Old conversation"}});
        }
        QJsonArray archived{QJsonObject{{"id", "archived-1"}, {"createdAt", 50},
                                        {"cwd", projectPath}, {"preview", "Archived conversation"}}};
        QFile state(directory.filePath("threads.json"));
        if (!state.open(QIODevice::WriteOnly)) return false;
        return state.write(QJsonDocument(QJsonObject{{"active", active}, {"archived", archived}}).toJson()) > 0;
    };
    QVERIFY(writeState(false));
    const QString indexPath = directory.filePath("codex-index.json");
    {
        MainWindow window(fakeServer, directory.path(), {}, {}, "gemini", nullptr, indexPath);
        window.show();
        auto *tree = window.findChild<QTreeWidget *>("conversationTree");
        auto *output = window.findChild<QPlainTextEdit *>("log");
        QVERIFY(tree);
        QVERIFY(output);
        QCOMPARE(tree->topLevelItemCount(), 5);
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("Codex"));
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QCOMPARE(tree->topLevelItem(i)->childIndicatorPolicy(), QTreeWidgetItem::ShowIndicator);
        }
        QCOMPARE(tree->topLevelItem(0)->childCount(), 0);
        tree->topLevelItem(1)->setExpanded(true);
        QCOMPARE(tree->topLevelItem(1)->childCount(), 0);
        tree->topLevelItem(3)->setExpanded(true);
        QCOMPARE(tree->topLevelItem(3)->childCount(), 0);
        tree->topLevelItem(0)->setExpanded(true);
        QTRY_VERIFY(output->toPlainText().contains("[Codex conversations: 6 total, 6 new; fetched 4 pages]"));
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->child(0)->text(0), projectPath);
        QVERIFY(QFileInfo::exists(indexPath));
    }
    QVERIFY(writeState(true));
    QFile log(directory.filePath("requests.log"));
    QVERIFY(log.open(QIODevice::WriteOnly | QIODevice::Truncate));
    log.close();
    {
        MainWindow window(fakeServer, directory.path(), {}, {}, "gemini", nullptr, indexPath);
        window.show();
        auto *tree = window.findChild<QTreeWidget *>("conversationTree");
        auto *output = window.findChild<QPlainTextEdit *>("log");
        QVERIFY(tree);
        QVERIFY(output);
        QVERIFY(output->toPlainText().contains("[Cached Codex conversations: 6]"));
        tree->topLevelItem(0)->setExpanded(true);
        QTRY_VERIFY(output->toPlainText().contains("[Codex conversations: 8 total, 2 new; fetched 3 pages]"));
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(log.readAll()).count("active\n"), 2);
        log.close();
        auto *folder = tree->topLevelItem(0)->child(0);
        QVERIFY(folder);
        QVERIFY(folder->childCount() >= 1);
        auto *chatItem = folder->child(0);
        QVERIFY(chatItem->text(0).size() <= 72);
        QVERIFY(chatItem->toolTip(0).contains(QString(199, 'N')));
        QVERIFY(!chatItem->toolTip(0).contains(QString(200, 'N')));
        QFile index(indexPath);
        QVERIFY(index.open(QIODevice::ReadOnly));
        const QJsonArray saved = QJsonDocument::fromJson(index.readAll()).object().value("threads").toArray();
        bool foundPreview = false;
        for (const QJsonValue &entry : saved) {
            if (entry.toObject().value("id").toString() == "new-102") {
                foundPreview = true;
                QCOMPARE(entry.toObject().value("preview").toString().size(), 200);
            }
        }
        QVERIFY(foundPreview);
        auto *chat = window.findChild<QPlainTextEdit *>("chatView");
        auto *header = window.findChild<QLabel *>("chatHeader");
        auto *loadEarlier = window.findChild<QPushButton *>("loadEarlierButton");
        auto *input = window.findChild<QLineEdit *>("commandInput");
        auto *sendButton = window.findChild<QPushButton *>("sendButton");
        QVERIFY(chat && header && loadEarlier && input && sendButton);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualItemRect(chatItem).center());
        QTRY_VERIFY(chat->toPlainText().contains("Codex: Latest answer"));
        const QString latest = chat->toPlainText();
        QVERIFY(latest.indexOf("You: Latest question") < latest.indexOf("[Codex tool: $ ls]"));
        QVERIFY(latest.indexOf("[Codex tool: $ ls]") < latest.indexOf("Codex: Latest answer"));
        QVERIFY(!latest.contains("Old question"));
        QVERIFY(header->text().contains("read-only preview"));
        QVERIFY(!sendButton->isEnabled());
        QVERIFY(loadEarlier->isVisible());
        QTest::mouseClick(loadEarlier, Qt::LeftButton);
        QTRY_VERIFY(chat->toPlainText().contains("Codex: Old answer"));
        const QString full = chat->toPlainText();
        QVERIFY(full.indexOf("You: Old question") < full.indexOf("Codex: Old answer"));
        QVERIFY(full.indexOf("Codex: Old answer") < full.indexOf("You: Latest question"));
        QVERIFY(!loadEarlier->isVisible());
        input->setText("write something");
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(output->toPlainText().contains("[The displayed chat is a read-only preview."));
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(!QString::fromUtf8(log.readAll()).contains("turn\n"));
        log.close();

        QTreeWidgetItem *lockedItem = nullptr;
        for (int i = 0; i < folder->childCount(); ++i) {
            if (folder->child(i)->data(0, Qt::UserRole + 1).toString() == "new-101") lockedItem = folder->child(i);
        }
        QVERIFY(lockedItem);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualItemRect(lockedItem).center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, tree->visualItemRect(lockedItem).center());
        QTRY_VERIFY(header->text().contains("locked: the Codex App Server refused to open it"));
        QVERIFY(header->text().contains("thread is in use by another client"));
        QVERIFY(!sendButton->isEnabled());

        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualItemRect(chatItem).center());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, tree->visualItemRect(chatItem).center());
        QTRY_VERIFY(sendButton->isEnabled());
        QTRY_VERIFY(chat->toPlainText().contains("Codex: Latest answer"));
        QVERIFY(!header->text().contains("read-only"));
        QVERIFY(output->toPlainText().contains("[Resumed Codex conversation: new-102]"));
        input->setText("continue here");
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_VERIFY(chat->toPlainText().contains("You: continue here"));
        QVERIFY(chat->toPlainText().contains("You: Latest question"));
        const auto requested = [&log] {
            if (!log.open(QIODevice::ReadOnly)) return QString();
            const QString text = QString::fromUtf8(log.readAll());
            log.close();
            return text;
        };
        QTRY_VERIFY(requested().contains("turn\n"));
    }
}

void MainWindowTest::claudeSessionMetadataRefreshesExistingIndex()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python 3 is unavailable");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString scriptPath = directory.filePath("fake-claude.py");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(import json
import sys

if "--read-session" in sys.argv:
    assert sys.argv[sys.argv.index("--read-session") + 1] == "claude-1"
    limit = int(sys.argv[sys.argv.index("--limit") + 1])
    entries = []
    for i in range(21):
        entries += [{"role": "user", "text": f"Question {i}"}, {"role": "assistant", "text": f"Answer {i}"}]
    entries.append({"role": "tool", "text": "Read"})
    print(json.dumps({"type": "history", "entries": entries[-limit:], "total": len(entries)}), flush=True)
    sys.exit(0)
if "--list-sessions" in sys.argv:
    print(json.dumps({"type": "sessions", "sessions": [{
        "id": "claude-1", "cwd": sys.argv[sys.argv.index("--cwd") + 1],
        "title": "Updated title", "createdAt": 100, "lastModified": 200,
        "fileSize": 1234, "customTitle": "Updated title", "summary": "A summary",
        "firstPrompt": "First question", "gitBranch": "main", "tag": "work"
    }]}), flush=True)
)PY");
    script.close();
    QFile index(directory.filePath("claude-conversations.json"));
    QVERIFY(index.open(QIODevice::WriteOnly));
    const QJsonArray original{QJsonObject{{"provider", "Claude"}, {"id", "claude-1"},
                                      {"cwd", directory.path()}, {"title", "Old title"},
                                      {"createdAt", 100}}};
    QVERIFY(index.write(QJsonDocument(QJsonObject{{"version", 1}, {"threads", original}}).toJson()) > 0);
    index.close();

    MainWindow window("/bin/true", directory.path(), python, scriptPath, "gemini", nullptr,
                      directory.filePath("codex-conversations.json"));
    window.show();
    auto *tree = window.findChild<QTreeWidget *>("conversationTree");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    QVERIFY(tree);
    QVERIFY(output);
    tree->topLevelItem(1)->setExpanded(true);
    QTRY_VERIFY(output->toPlainText().contains("[Claude sessions discovered: 1]"));
    auto *chatItem = tree->topLevelItem(1)->child(0)->child(0);
    QCOMPARE(chatItem->text(0), QString("Updated title"));
    QVERIFY(chatItem->toolTip(0).contains("Modified:"));
    QVERIFY(chatItem->toolTip(0).contains("Git branch: main"));
    QVERIFY(chatItem->toolTip(0).contains("First prompt: First question"));
    tree->setCurrentItem(chatItem);
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    auto *loadEarlier = window.findChild<QPushButton *>("loadEarlierButton");
    QVERIFY(chat && loadEarlier);
    QTRY_VERIFY(chat->toPlainText().contains("Claude: Answer 20"));
    QVERIFY(chat->toPlainText().contains("[Claude tool: Read]"));
    QVERIFY(!chat->toPlainText().contains("Question 0"));
    QVERIFY(loadEarlier->isVisible());
    QTest::mouseClick(loadEarlier, Qt::LeftButton);
    QTRY_VERIFY(chat->toPlainText().contains("You: Question 2"));
    QVERIFY(!chat->toPlainText().contains("Question 0"));
    QVERIFY(loadEarlier->isVisible());
    QTest::mouseClick(loadEarlier, Qt::LeftButton);
    QTRY_VERIFY(chat->toPlainText().contains("You: Question 0"));
    QVERIFY(!loadEarlier->isVisible());
    QVERIFY(index.open(QIODevice::ReadOnly));
    const QJsonArray saved = QJsonDocument::fromJson(index.readAll()).object().value("threads").toArray();
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().toObject().value("fileSize").toInteger(), 1234);
}

void MainWindowTest::missingGeminiCliReportsOneDiscoveryError()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QJsonArray threads;
    for (int i = 0; i < 25; ++i) {
        const QString path = directory.filePath(QString("project-%1").arg(i));
        QVERIFY(QDir().mkpath(path));
        threads.append(QJsonObject{{"id", QString("codex-%1").arg(i)}, {"cwd", path},
                                   {"createdAt", i}});
    }
    QFile index(directory.filePath("codex-conversations.json"));
    QVERIFY(index.open(QIODevice::WriteOnly));
    QVERIFY(index.write(QJsonDocument(QJsonObject{{"version", 1}, {"threads", threads}}).toJson()) > 0);
    index.close();

    const auto verifyOneError = [&](const QString &program, const QString &prefix) {
        MainWindow window("/bin/true", directory.path(), {}, {}, program, nullptr, index.fileName());
        window.show();
        auto *tree = window.findChild<QTreeWidget *>("conversationTree");
        auto *output = window.findChild<QPlainTextEdit *>("log");
        QVERIFY(tree);
        QVERIFY(output);
        tree->topLevelItem(2)->setExpanded(true);
        QTRY_COMPARE(output->toPlainText().count(prefix), 1);
        if (prefix == "[Gemini CLI is not installed") {
            tree->topLevelItem(2)->setExpanded(false);
            tree->topLevelItem(2)->setExpanded(true);
            QCOMPARE(output->toPlainText().count(prefix), 1);
        }
    };
    verifyOneError(directory.filePath("missing-gemini"), "[Gemini CLI is not installed");

    QFile scanner(directory.filePath("fake-gemini"));
    QVERIFY(scanner.open(QIODevice::WriteOnly));
    scanner.write(R"PY(#!/usr/bin/env python3
import os
from pathlib import Path

with Path(__file__).with_name("gemini-scans.log").open("a") as log:
    log.write(os.getcwd() + "\n")
)PY");
    scanner.close();
    QVERIFY(scanner.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    {
        MainWindow window("/bin/true", directory.path(), {}, {}, scanner.fileName(), nullptr, index.fileName());
        window.show();
        auto *tree = window.findChild<QTreeWidget *>("conversationTree");
        auto *output = window.findChild<QPlainTextEdit *>("log");
        QVERIFY(tree && output);
        tree->topLevelItem(2)->setExpanded(true);
        const QString logPath = directory.filePath("gemini-scans.log");
        QTRY_VERIFY(output->toPlainText().contains("[Gemini sessions discovered:"));
        QVERIFY(!QFileInfo::exists(logPath));
    }
}

void MainWindowTest::geminiListsSessionsFromAllProjects()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString geminiData = directory.filePath("gemini-data");
    const QString projectA = directory.filePath("project-a");
    const QString projectB = directory.filePath("project-b");
    QVERIFY(QDir().mkpath(projectA));
    QVERIFY(QDir().mkpath(projectB));
    QVERIFY(QDir().mkpath(geminiData + "/tmp/a/chats"));
    QVERIFY(QDir().mkpath(geminiData + "/tmp/b/chats"));
    QFile projects(geminiData + "/projects.json");
    QVERIFY(projects.open(QIODevice::WriteOnly));
    projects.write(QJsonDocument(QJsonObject{{"projects", QJsonObject{{projectA, "a"}, {projectB, "b"}}}}).toJson());
    projects.close();
    const auto writeSession = [&](const QString &slug, const QString &id, const QString &prompt) {
        QFile file(geminiData + "/tmp/" + slug + "/chats/session-2026-09-29T10-00-" + id + ".jsonl");
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write(QJsonDocument(QJsonObject{{"sessionId", id}, {"kind", "main"},
                                         {"startTime", "2026-09-29T10:00:00Z"}}).toJson(QJsonDocument::Compact) + "\n");
        file.write(QJsonDocument(QJsonObject{{"type", "user"}, {"id", "u1"}, {"content", prompt}}).toJson(QJsonDocument::Compact) + "\n");
        file.write(QJsonDocument(QJsonObject{{"type", "gemini"}, {"id", "g1"}, {"content", "Draft answer"}})
                       .toJson(QJsonDocument::Compact) + "\n");
        file.write(QJsonDocument(QJsonObject{{"$set", QJsonObject{{"lastUpdated", "2026-09-29T10:01:00Z"}}}})
                       .toJson(QJsonDocument::Compact) + "\n");
        file.write(QJsonDocument(QJsonObject{{"type", "gemini"}, {"id", "g1"}, {"content", "Updated answer"},
                                         {"toolCalls", QJsonArray{QJsonObject{{"name", "read_file"}}}}})
                       .toJson(QJsonDocument::Compact) + "\n");
        return true;
    };
    QVERIFY(writeSession("a", "session-a", "First project prompt"));
    QVERIFY(writeSession("b", "session-b", "Second project prompt"));

    MainWindow window("/bin/true", projectA, {}, {}, directory.filePath("missing-gemini"), nullptr,
                      directory.filePath("codex-conversations.json"), "agy", geminiData);
    auto *tree = window.findChild<QTreeWidget *>("conversationTree");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    QVERIFY(tree && output);
    tree->topLevelItem(2)->setExpanded(true);
    QTRY_VERIFY(output->toPlainText().contains("[Gemini sessions discovered: 2]"));
    QTreeWidgetItem *root = tree->topLevelItem(2);
    QCOMPARE(root->childCount(), 2);
    QSet<QString> found;
    for (int i = 0; i < root->childCount(); ++i) {
        QTreeWidgetItem *folder = root->child(i);
        QCOMPARE(folder->childCount(), 1);
        found.insert(folder->child(0)->data(0, Qt::UserRole + 1).toString());
    }
    QCOMPARE(found, QSet<QString>({"session-a", "session-b"}));
    QTreeWidgetItem *sessionA = nullptr;
    for (int i = 0; i < root->childCount(); ++i) {
        if (root->child(i)->child(0)->data(0, Qt::UserRole + 1).toString() == "session-a") sessionA = root->child(i)->child(0);
    }
    QVERIFY(sessionA);
    tree->setCurrentItem(sessionA);
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    const QString history = chat->toPlainText();
    QVERIFY(history.contains("You: First project prompt"));
    QVERIFY(history.contains("Gemini: Updated answer"));
    QVERIFY(!history.contains("Draft answer"));
    QVERIFY(history.contains("[Gemini tool: read_file]"));

    const QString indexPath = directory.filePath("gemini-conversations.json");
    QFile index(indexPath);
    QVERIFY(index.open(QIODevice::ReadOnly));
    const QJsonArray saved = QJsonDocument::fromJson(index.readAll()).object().value("threads").toArray();
    QCOMPARE(saved.size(), 2);
    QSet<QString> savedIds;
    for (const QJsonValue &value : saved) savedIds.insert(value.toObject().value("id").toString());
    QCOMPARE(savedIds, found);

    MainWindow reopened("/bin/true", projectA, {}, {}, directory.filePath("missing-gemini"), nullptr,
                        directory.filePath("codex-conversations.json"), "agy", directory.filePath("empty-data"));
    auto *reopenedTree = reopened.findChild<QTreeWidget *>("conversationTree");
    QVERIFY(reopenedTree);
    reopenedTree->topLevelItem(2)->setExpanded(true);
    QCOMPARE(reopenedTree->topLevelItem(2)->childCount(), 2);
}

// Clicks and then double-clicks a tree item, which is how a real double-click reaches QTreeWidget.
static void doubleClickItem(QTreeWidget *tree, QTreeWidgetItem *item)
{
    tree->scrollToItem(item);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualItemRect(item).center());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, tree->visualItemRect(item).center());
}

void MainWindowTest::claudeAttachRespectsExternalLock()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python 3 is unavailable");
    if (!QFileInfo::exists("/proc/1")) QSKIP("Process information is unavailable");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeBridge = directory.filePath("fake-claude.py");
    QFile script(fakeBridge);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write(R"PY(import json
import sys

def send(message):
    print(json.dumps(message), flush=True)

if "--read-session" in sys.argv:
    send({"type": "history", "total": 2, "entries": [
        {"role": "user", "text": "Saved question"}, {"role": "assistant", "text": "Saved answer"}]})
    sys.exit(0)
if "--list-sessions" in sys.argv:
    send({"type": "sessions", "sessions": []})
    sys.exit(0)
send({"type": "ready"})
for line in sys.stdin:
    request = json.loads(line)
    if request["type"] == "resume":
        with open(sys.argv[0] + ".resumed", "a") as log:
            log.write(request["session_id"] + "\n")
        send({"type": "ready"})
    elif request["type"] == "prompt":
        send({"type": "delta", "text": "Continued"})
        send({"type": "complete", "status": "completed"})
    elif request["type"] == "shutdown":
        break
)PY");
    script.close();
    QFile index(directory.filePath("claude-conversations.json"));
    QVERIFY(index.open(QIODevice::WriteOnly));
    const QJsonArray threads{QJsonObject{{"provider", "Claude"}, {"id", "claude-1"}, {"cwd", directory.path()},
                                         {"title", "Saved chat"}, {"createdAt", 1}}};
    QVERIFY(index.write(QJsonDocument(QJsonObject{{"version", 1}, {"threads", threads}}).toJson()) > 0);
    index.close();
    // PID 1 is alive and is not started by this test, so it stands in for another Claude Code window.
    const QString configDirectory = directory.filePath("claude-config");
    QVERIFY(QDir().mkpath(configDirectory + "/sessions"));
    QFile lock(configDirectory + "/sessions/1.json");
    QVERIFY(lock.open(QIODevice::WriteOnly));
    lock.write(QJsonDocument(QJsonObject{{"pid", 1}, {"sessionId", "claude-1"}, {"kind", "interactive"}}).toJson());
    lock.close();
    const QByteArray previousConfig = qgetenv("CLAUDE_CONFIG_DIR");
    const auto restoreConfig = qScopeGuard([&previousConfig] {
        if (previousConfig.isNull()) qunsetenv("CLAUDE_CONFIG_DIR");
        else qputenv("CLAUDE_CONFIG_DIR", previousConfig);
    });
    qputenv("CLAUDE_CONFIG_DIR", configDirectory.toLocal8Bit());

    MainWindow window("/bin/true", directory.path(), python, fakeBridge, "gemini", nullptr,
                      directory.filePath("codex-conversations.json"));
    window.show();
    auto *tree = window.findChild<QTreeWidget *>("conversationTree");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *header = window.findChild<QLabel *>("chatHeader");
    auto *input = window.findChild<QLineEdit *>("commandInput");
    auto *sendButton = window.findChild<QPushButton *>("sendButton");
    QVERIFY(tree && chat && output && header && input && sendButton);
    tree->topLevelItem(1)->setExpanded(true);
    QTRY_VERIFY(output->toPlainText().contains("[Claude sessions discovered: 0]"));
    QTreeWidgetItem *chatItem = tree->topLevelItem(1)->child(0)->child(0);
    QVERIFY(chatItem);

    doubleClickItem(tree, chatItem);
    QTRY_VERIFY(chat->toPlainText().contains("Claude: Saved answer"));
    QVERIFY(header->text().contains("locked: open in Claude Code interactive (PID 1)"));
    QVERIFY(output->toPlainText().contains("stays read-only"));
    QVERIFY(!sendButton->isEnabled());
    QVERIFY(!QFileInfo::exists(fakeBridge + ".resumed"));

    QVERIFY(lock.remove());
    doubleClickItem(tree, chatItem);
    QTRY_VERIFY(sendButton->isEnabled());
    QVERIFY(!header->text().contains("locked"));
    QVERIFY(!header->text().contains("read-only"));
    input->setText("more");
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(chat->toPlainText().contains("Claude: Continued"));
    const QString transcript = chat->toPlainText();
    QVERIFY(transcript.indexOf("Claude: Saved answer") < transcript.indexOf("You: more"));
    QFile resumed(fakeBridge + ".resumed");
    QVERIFY(resumed.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(resumed.readAll()), QString("claude-1\n"));
}

void MainWindowTest::geminiAttachRespectsExternalLock()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python 3 is unavailable");
    if (!QFileInfo::exists("/proc/self/cmdline")) QSKIP("Process information is unavailable");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString geminiData = directory.filePath("gemini-data");
    const QString project = directory.filePath("project");
    QVERIFY(QDir().mkpath(project));
    QVERIFY(QDir().mkpath(geminiData + "/tmp/p/chats"));
    QFile projects(geminiData + "/projects.json");
    QVERIFY(projects.open(QIODevice::WriteOnly));
    projects.write(QJsonDocument(QJsonObject{{"projects", QJsonObject{{project, "p"}}}}).toJson());
    projects.close();
    const QString sessionId = "gemini-lock-" + QString::number(QCoreApplication::applicationPid());
    QFile session(geminiData + "/tmp/p/chats/session-2026-09-29T10-00-lock.jsonl");
    QVERIFY(session.open(QIODevice::WriteOnly));
    session.write(QJsonDocument(QJsonObject{{"sessionId", sessionId}, {"kind", "main"},
                                        {"startTime", "2026-09-29T10:00:00Z"}}).toJson(QJsonDocument::Compact) + "\n");
    session.write(QJsonDocument(QJsonObject{{"type", "user"}, {"id", "u1"}, {"content", "Locked prompt"}})
                      .toJson(QJsonDocument::Compact) + "\n");
    session.close();

    // A detached process is not a child of this application, like a Gemini CLI started elsewhere.
    qint64 holder = 0;
    QVERIFY(QProcess::startDetached(python, {"-c", "import time; time.sleep(30)", "--resume", sessionId}, {}, &holder));
    const auto stopHolder = qScopeGuard([holder] { QProcess::execute("kill", {QString::number(holder)}); });

    MainWindow window("/bin/true", project, {}, {}, directory.filePath("missing-gemini"), nullptr,
                      directory.filePath("codex-conversations.json"), "agy", geminiData);
    window.show();
    auto *tree = window.findChild<QTreeWidget *>("conversationTree");
    auto *header = window.findChild<QLabel *>("chatHeader");
    auto *sendButton = window.findChild<QPushButton *>("sendButton");
    QVERIFY(tree && header && sendButton);
    tree->topLevelItem(2)->setExpanded(true);
    QTRY_COMPARE(tree->topLevelItem(2)->childCount(), 1);
    QTreeWidgetItem *chatItem = tree->topLevelItem(2)->child(0)->child(0);
    doubleClickItem(tree, chatItem);
    QTRY_VERIFY(header->text().contains(QString("locked: open in PID %1").arg(holder)));
    QVERIFY(!sendButton->isEnabled());

    QProcess::execute("kill", {QString::number(holder)});
    const auto holderRunning = [holder] {
        QFile commandLine(QString("/proc/%1/cmdline").arg(holder));
        return commandLine.open(QIODevice::ReadOnly) && !commandLine.readAll().isEmpty();
    };
    QTRY_VERIFY(!holderRunning());
    doubleClickItem(tree, chatItem);
    QTRY_VERIFY(sendButton->isEnabled());
    QVERIFY(!header->text().contains("locked"));
}

QTEST_MAIN(MainWindowTest)
#include "MainWindowTest.moc"
