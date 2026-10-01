#include "CodexLocator.h"
#include "ChatTab.h"
#include "CommandApproval.h"
#include "CodexAgent.h"
#include "CodexConnection.h"
#include <QJsonDocument>
#include "MainWindow.h"
#include "Notifier.h"
#include "UsageLimitsPanel.h"
#include "ClaudeAgent.h"
#include <QProcess>
#include <QToolButton>

#include <qxfiledialog.h>
#include <QAction>
#include <QCheckBox>
#include <QTreeView>
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
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QRadioButton>
#include <QtTest>

static void startChat(MainWindow &window, const QString &provider, const QString &path);

class MainWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void reasoningPanelAndStreaming();
    void codexReasoningAndQuestions();
    void usageLimitPacing();
    void providerLimitsPanelAndVisibility();
    void audioChooserPathsAndLastDirectory();
    void speakerStopsAudio();
    void piperStopsBeforePlayback();
    void commandApproval_data();
    void commandApproval();
    void codexCommandTrust();
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

void MainWindowTest::codexReasoningAndQuestions()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python is needed for the fake App Server");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fakeServer = directory.filePath("codex");
    QFile script(fakeServer);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(("#!" + python + "\n").toUtf8());
    script.write(R"PY(import json, os, sys
folder = os.path.dirname(__file__)
assert "features.default_mode_request_user_input=true" in sys.argv

def send(value):
    print(json.dumps(value), flush=True)

def notify(method, **params):
    send({"method": method, "params": {"threadId": "question-thread", **params}})

for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if not method:
        if request.get("id") == "question-1":
            with open(os.path.join(folder, "answer.json"), "w") as output:
                json.dump(request["result"], output)
            notify("serverRequest/resolved", requestId="question-1")
        continue
    if "id" not in request:
        continue
    if method == "initialize":
        assert request["params"]["capabilities"]["experimentalApi"]
        result = {}
    elif method == "thread/start":
        result = {"thread": {"id": "question-thread", "cwd": folder}}
    elif method == "turn/start":
        assert request["params"]["summary"] == "detailed"
        result = {"turn": {"id": "turn-1"}}
        send({"id": request["id"], "result": result})
        notify("turn/started", turn={"id": "turn-1"})
        notify("item/reasoning/summaryTextDelta", turnId="turn-1", itemId="reason-1", summaryIndex=0, delta="Shared reasoning")
        notify("item/started", turnId="turn-1", item={"type": "commandExecution", "id": "shell-1", "command": "ls"})
        for index in range(512):
            notify("item/commandExecution/outputDelta", turnId="turn-1", itemId="shell-1", delta=f"file-{index}\n")
        notify("item/completed", turnId="turn-1", item={"type": "commandExecution", "id": "shell-1", "status": "completed"})
        send({"id": "question-1", "method": "item/tool/requestUserInput", "params": {
            "threadId": "question-thread", "turnId": "turn-1", "itemId": "ask-1", "isBlocking": False,
            "questions": [{"id": "implementation", "header": "Implementation", "question": "Code or plan?",
                "isOther": True, "options": [{"label": "Implement", "description": "Change the code"},
                                             {"label": "Plan", "description": "Design only"}]}]}})
        notify("turn/completed", turn={"id": "turn-1", "status": "completed"})
        continue
    elif method == "thread/list":
        result = {"data": [], "nextCursor": None}
    elif method == "model/list":
        result = {"data": [], "nextCursor": None}
    else:
        result = {}
    send({"id": request["id"], "result": result})
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    MainWindow window(fakeServer, directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                      "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                      directory.filePath("gemini"));
    window.show();
    window.findChild<QAction *>("showReasoning")->trigger();
    auto *tabs = window.findChild<QTabWidget *>("chatTabs");
    QVERIFY(tabs);
    auto *tab = tabs->currentWidget()->findChild<ChatTab *>();
    QVERIFY(tab);
    auto *connection = window.findChild<CodexConnection *>();
    QVERIFY(connection);
    QTRY_VERIFY(connection->isConnected());
    auto *input = window.findChild<QPlainTextEdit *>("commandInput");
    QVERIFY(input);
    input->setPlainText("Question test");
    window.findChild<QPushButton *>("sendButton")->click();
    auto *reasoning = window.findChild<QPlainTextEdit *>("reasoningPanel");
    auto *panel = window.findChild<QWidget *>("requestPanel");
    QVERIFY(reasoning && panel);
    QTRY_COMPARE(reasoning->toPlainText(), QString("Shared reasoning"));
    QTRY_VERIFY(tab->document()->toPlainText().contains("file-511"));
    QTRY_VERIFY(!tab->document()->findBlock(tab->document()->toPlainText().indexOf("file-511")).isVisible());
    QTRY_VERIFY(!tab->agent()->isResponding());
    QTRY_VERIFY(panel->isVisible());
    auto options = panel->findChildren<QRadioButton *>();
    QCOMPARE(options.size(), 2);
    options.first()->click();
    QPushButton *answer = nullptr;
    for (auto *button : panel->findChildren<QPushButton *>())
        if (button->text() == "Answer") answer = button;
    QVERIFY(answer);
    answer->click();
    QTRY_VERIFY(QFileInfo(directory.filePath("answer.json")).size() > 0);
    QFile savedAnswer(directory.filePath("answer.json"));
    QVERIFY(savedAnswer.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(savedAnswer.readAll()).object().value("answers").toObject()
        .value("implementation").toObject().value("answers").toArray(), QJsonArray{"Implement"});
    QVERIFY(!panel->isVisible());
    auto *agent = qobject_cast<CodexAgent *>(tab->agent());
    QVERIFY(agent);
    const QJsonObject question{{"threadId", "question-thread"}, {"turnId", "turn-2"}, {"itemId", "ask-2"},
        {"isBlocking", false}, {"questions", QJsonArray{QJsonObject{{"id", "q"}, {"header", "Q"}, {"question", "Continue?"}}}}};
    agent->handleServerRequest("item/tool/requestUserInput", "question-2", question);
    QVERIFY(panel->isVisible());
    agent->handleNotification("serverRequest/resolved", {{"threadId", "question-thread"}, {"requestId", "question-2"}});
    QVERIFY(!panel->isVisible());
    QJsonObject blocking = question;
    blocking.remove("isBlocking");
    agent->handleServerRequest("item/tool/requestUserInput", 42, blocking);
    QVERIFY(panel->isVisible());
    agent->handleNotification("turn/completed", {{"threadId", "question-thread"},
        {"turn", QJsonObject{{"id", "turn-2"}, {"status", "interrupted"}}}});
    QVERIFY(!panel->isVisible());
    agent->handleServerRequest("item/tool/requestUserInput", "question-3", question);
    connection->disconnected();
    QVERIFY(!panel->isVisible());
}

void MainWindowTest::reasoningPanelAndStreaming()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    {
        MainWindow window("/nonexistent/codex", directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                          "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                          directory.filePath("gemini"));
        window.show();
        auto *panel = window.findChild<QPlainTextEdit *>("reasoningPanel");
        auto *visible = window.findChild<QAction *>("showReasoning");
        auto *splitter = window.findChild<QSplitter *>("conversationReasoningSplitter");
        auto *tabs = window.findChild<QTabWidget *>("chatTabs");
        QVERIFY(panel && visible && splitter && tabs);
        QVERIFY(panel->isReadOnly());
        QVERIFY(!visible->isChecked());
        visible->trigger();
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(splitter->widget(1), panel);
        QTRY_VERIFY(qAbs(splitter->sizes().value(0) - splitter->sizes().value(1)) <= 2);
        auto *tab = tabs->currentWidget()->findChild<ChatTab *>();
        QVERIFY(tab);
        auto *agent = qobject_cast<CodexAgent *>(tab->agent());
        QVERIFY(agent);
        const auto delta = [&](const QString &text, int index = 0) {
            agent->handleNotification("item/reasoning/summaryTextDelta",
                {{"threadId", "test"}, {"turnId", "turn"}, {"itemId", "reason-1"},
                 {"summaryIndex", index}, {"delta", text}});
        };
        delta("First");
        delta(" section");
        delta("Second section", 1);
        QCOMPARE(panel->toPlainText(), QString("First section\n\nSecond section"));
        agent->handleNotification("item/completed", {{"threadId", "test"}, {"turnId", "turn"},
            {"item", QJsonObject{{"type", "reasoning"}, {"id", "reason-1"},
                                {"summary", QJsonArray{"First section", "Second section"}}}}});
        QCOMPARE(panel->toPlainText(), QString("First section\n\nSecond section"));
        agent->handleNotification("item/reasoning/textDelta", {{"threadId", "test"}, {"turnId", "turn"},
            {"itemId", "reason-2"}, {"contentIndex", 0}, {"delta", "Raw text"}});
        QVERIFY(panel->toPlainText().endsWith("Raw text"));
        QVERIFY(!tab->document()->toPlainText().contains("First section"));
        startChat(window, "Claude", directory.path());
        QVERIFY(panel->toPlainText().isEmpty());
        auto *claudeTab = tabs->currentWidget()->findChild<ChatTab *>();
        QVERIFY(claudeTab);
        claudeTab->agent()->reasoningUpdated("claude-1", "Claude thinking");
        QCOMPARE(panel->toPlainText(), QString("Claude thinking"));
        // Background reasoning stays in its own conversation.
        agent->reasoningUpdated("reason-2", "Raw text continued");
        QCOMPARE(panel->toPlainText(), QString("Claude thinking"));
        tabs->setCurrentIndex(0);
        QVERIFY(panel->toPlainText().endsWith("Raw text continued"));
        // Reloading overlapping history replaces the same item instead of duplicating it.
        agent->historyLoaded("", {{"reasoning", "Saved summary", "reason-1"}, {"assistant", "Saved answer"}}, false, {});
        QCOMPARE(panel->toPlainText(), QString("Saved summary\n\nRaw text continued"));
        QVERIFY(tab->document()->toPlainText().contains("Saved answer"));
        QVERIFY(!tab->document()->toPlainText().contains("Saved summary"));
        ChatTab preview(tab->provider(), directory.path());
        preview.agent()->reasoningUpdated("old", "Old preview text");
        preview.showPreview(tab->provider(), "different", directory.path(), "Different conversation");
        QVERIFY(preview.reasoningDocument()->isEmpty());
    }
    MainWindow restarted("/nonexistent/codex", directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                         "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                         directory.filePath("gemini"));
    QVERIFY(restarted.findChild<QAction *>("showReasoning")->isChecked());
}

void MainWindowTest::usageLimitPacing()
{
    const qint64 start = 1700000000;
    const qint64 week = 7 * 24 * 60 * 60;
    UsageLimit limit;
    limit.windowMinutes = 7 * 24 * 60;
    limit.resetsAt = start + week;
    const qint64 now = start + 24 * 60 * 60;
    limit.usedPercent = 14;
    QCOMPARE(int(assessUsageLimit(limit, now).pace), int(UsagePace::Green));
    limit.usedPercent = 100.0 / 7;
    QCOMPARE(int(assessUsageLimit(limit, now).pace), int(UsagePace::Yellow));
    limit.usedPercent = 20;
    const UsageAssessment yellow = assessUsageLimit(limit, now);
    QCOMPARE(int(yellow.pace), int(UsagePace::Yellow));
    QCOMPARE(yellow.remainingPercent, 80.0);
    QCOMPARE(yellow.greenAt, start + week / 5 + 1);
    QCOMPARE(int(assessUsageLimit(limit, yellow.greenAt - 1).pace), int(UsagePace::Yellow));
    QCOMPARE(int(assessUsageLimit(limit, yellow.greenAt).pace), int(UsagePace::Green));
    limit.usedPercent = 100;
    QCOMPARE(int(assessUsageLimit(limit, now).pace), int(UsagePace::Red));
    QCOMPARE(assessUsageLimit(limit, now).remainingPercent, 0.0);
    QCOMPARE(int(assessUsageLimit(limit, limit.resetsAt).pace), int(UsagePace::Expired));
    QCOMPARE(assessUsageLimit(limit, limit.resetsAt).remainingPercent, -1.0);
    limit.usedPercent = -1;
    limit.status = "rejected";
    QCOMPARE(int(assessUsageLimit(limit, now).pace), int(UsagePace::Red));
    limit.status.clear();
    QCOMPARE(int(assessUsageLimit(limit, now).pace), int(UsagePace::Unknown));
    limit.usedPercent = 20;
    limit.resetsAt = 0;
    QCOMPARE(int(assessUsageLimit(limit, now).pace), int(UsagePace::Unknown));
    limit.windowMinutes = 300;
    limit.resetsAt = start + 5 * 60 * 60;
    limit.usedPercent = 50;
    QCOMPARE(int(assessUsageLimit(limit, start + 60 * 60).pace), int(UsagePace::Yellow));
    QCOMPARE(int(assessUsageLimit(limit, start + 3 * 60 * 60).pace), int(UsagePace::Green));
    QCOMPARE(int(assessUsageLimit(limit, start - 1).pace), int(UsagePace::Unknown));
}

void MainWindowTest::providerLimitsPanelAndVisibility()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    {
        MainWindow window("/nonexistent/codex", directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                          "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                          directory.filePath("gemini"));
        window.show();
        auto *panel = window.findChild<UsageLimitsPanel *>();
        auto *tree = window.findChild<QTreeWidget *>("usageLimitsTree");
        auto *visible = window.findChild<QAction *>("showUsageLimits");
        QVERIFY(panel && tree && visible);
        QVERIFY(visible->isChecked());
        QTRY_VERIFY(panel->isVisible());
        ClaudeProvider *claude = nullptr;
        for (ClaudeProvider *candidate : window.findChildren<ClaudeProvider *>())
            if (candidate->name() == "Claude") claude = candidate;
        QVERIFY(claude);
        // A background provider snapshot must not appear in the selected Codex chat.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 reset = now + 6 * 24 * 60 * 60;
        claude->updateUsage({{"limit", "seven_day"}, {"utilization", 0.2}, {"resetsAt", reset}, {"status", "allowed"}});
        QCOMPARE(tree->topLevelItemCount(), 0);
        startChat(window, "Claude", directory.path());
        QCOMPARE(tree->topLevelItemCount(), 1);
        QVERIFY(tree->isHeaderHidden());
        auto *splitter = window.findChild<QSplitter *>("usageSplitter");
        QVERIFY(splitter);
        QVERIFY(splitter->handle(1)->isVisible());
        QTRY_VERIFY(panel->height() > 0);
        QVERIFY(panel->height() < tree->fontMetrics().height() * 3);
        const int total = splitter->sizes().value(0) + splitter->sizes().value(1);
        splitter->setSizes({5, total - 5});
        QTRY_VERIFY(panel->height() <= 5);
        splitter->setSizes({100, total - 100});
        QTRY_VERIFY(panel->height() >= 90);
        QVERIFY(QMetaObject::invokeMethod(splitter, "splitterMoved", Q_ARG(int, 100), Q_ARG(int, 1)));
        QTreeWidgetItem *weekly = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *item = tree->topLevelItem(i);
            if (item->text(0) == "Claude" && item->text(1) == "Week") {
                weekly = item;
                QCOMPARE(tree->topLevelItemCount(), 1);
            }
        }
        QVERIFY(weekly);
        QCOMPARE(weekly->text(2), QString("80.0%"));
        QCOMPARE(weekly->data(4, Qt::UserRole).toInt(), int(UsagePace::Yellow));
        QVERIFY(weekly->text(4).startsWith("Pause until "));
        claude->updateUsage({{"limit", "seven_day"}, {"utilization", 1.0}, {"resetsAt", reset}, {"status", "rejected"}});
        QCOMPARE(weekly->data(4, Qt::UserRole).toInt(), int(UsagePace::Red));
        QCOMPARE(weekly->foreground(2).color(), QColor("#d32f2f"));
        // A new window without a percentage must not inherit the exhausted snapshot.
        claude->updateUsage({{"limit", "seven_day"}, {"resetsAt", reset + 7 * 24 * 60 * 60}, {"status", "allowed"}});
        QCOMPARE(weekly->text(2), QString("Not reported"));
        QCOMPARE(weekly->data(4, Qt::UserRole).toInt(), int(UsagePace::Unknown));
        auto *tabs = window.findChild<QTabWidget *>("chatTabs");
        QVERIFY(tabs);
        tabs->setCurrentIndex(0);
        QCOMPARE(tree->topLevelItemCount(), 0);
        tabs->setCurrentIndex(1);
        QCOMPARE(tree->topLevelItemCount(), 1);
        visible->trigger();
        QVERIFY(!visible->isChecked());
        QVERIFY(panel->isHidden());
    }
    MainWindow restarted("/nonexistent/codex", directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                         "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                         directory.filePath("gemini"));
    auto *visible = restarted.findChild<QAction *>("showUsageLimits");
    auto *panel = restarted.findChild<UsageLimitsPanel *>();
    QVERIFY(visible && panel);
    QVERIFY(!visible->isChecked());
    QVERIFY(panel->isHidden());
    restarted.show();
    visible->trigger();
    QVERIFY(visible->isChecked());
    QVERIFY(!panel->isHidden());
    QTRY_COMPARE(panel->height(), 100);
    QFile savedSettings(directory.filePath("settings.json"));
    QVERIFY(savedSettings.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(savedSettings.readAll()).object().value("usagePanelHeight").toInt(), 100);
}

void MainWindowTest::audioChooserPathsAndLastDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString oldDirectory = directory.filePath("old");
    const QString firstDirectory = directory.filePath("first");
    const QString secondDirectory = directory.filePath("second");
    const QString cancelledDirectory = directory.filePath("cancelled");
    for (const QString &path : {oldDirectory, firstDirectory, secondDirectory, cancelledDirectory}) QVERIFY(QDir().mkpath(path));
    const QString oldFile = QDir(oldDirectory).filePath("old.mp3");
    const QString firstFile = QDir(firstDirectory).filePath("first.wav");
    const QString secondFile = QDir(secondDirectory).filePath("second.ogg");
    const QString cancelledFile = QDir(cancelledDirectory).filePath("last.wav");
    for (const QString &path : {oldFile, firstFile, secondFile, cancelledFile}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    QFile settings(directory.filePath("settings.json"));
    QVERIFY(settings.open(QIODevice::WriteOnly));
    settings.write(QJsonDocument(QJsonObject{{"notifications", QJsonObject{
        {"finishedSound", oldFile}, {"failedSound", oldFile}}}}).toJson());
    settings.close();
    const auto choose = [&](MainWindow &window, const QString &key, const QString &expectedDirectory,
                            const QString &typedPath, const QString &expectedFile, bool cancel = false) {
        QAction *action = nullptr;
        for (QAction *candidate : window.findChildren<QAction *>())
            if (candidate->text() == "Notifications…") action = candidate;
        QVERIFY(action);
        bool completed = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto closeDialog = qScopeGuard([dialog] { dialog->reject(); });
            QVERIFY(!dialog->findChild<QCheckBox *>("audioDurationVisible"));
            auto *browse = dialog->findChild<QPushButton *>(key + "Browse");
            QVERIFY(browse);
            bool accepted = false;
            QTimer::singleShot(0, &window, [&] {
                auto *picker = qobject_cast<QxFileDialog *>(QApplication::activeModalWidget());
                QVERIFY(picker);
                const auto closePicker = qScopeGuard([picker] { if (picker->isVisible()) picker->reject(); });
                auto *view = picker->findChild<QTreeView *>();
                QVERIFY(view);
                QCOMPARE(picker->directory(), expectedDirectory);
                QVERIFY(!picker->findChild<QCheckBox *>("chooserAudioDurationVisible"));
                QVERIFY(!view->isColumnHidden(4));
                const QFileInfo currentFile(dialog->findChild<QLineEdit *>(key + "Path")->text());
                if (currentFile.isFile()) {
                    QTRY_VERIFY(!view->selectionModel()->selectedRows().isEmpty());
                    QCOMPARE(view->currentIndex().data().toString(), currentFile.fileName());
                    QVERIFY(view->viewport()->rect().intersects(view->visualRect(view->currentIndex())));
                }
                QComboBox *name = nullptr;
                for (QComboBox *combo : picker->findChildren<QComboBox *>())
                    if (combo->isEditable()) name = combo;
                QVERIFY(name);
                name->lineEdit()->selectAll();
                QTest::keyClicks(name->lineEdit(), typedPath);
                QPushButton *open = nullptr;
                for (QPushButton *button : picker->findChildren<QPushButton *>())
                    if (button->text() == "Open") open = button;
                QVERIFY(open);
                open->click();
                if (cancel) {
                    // Enter a directory through the normal pasted-path flow, then cancel the chooser.
                    QCOMPARE(picker->directory(), cancelledDirectory);
                    QVERIFY(picker->isVisible());
                    picker->reject();
                    QCOMPARE(picker->result(), int(QDialog::Rejected));
                } else {
                    QCOMPARE(picker->result(), int(QDialog::Accepted));
                    QCOMPARE(QDir::cleanPath(picker->selectedFile()), expectedFile);
                }
                accepted = true;
            });
            browse->click();
            QVERIFY(accepted);
            auto *path = dialog->findChild<QLineEdit *>(key + "Path");
            QVERIFY(path);
            QCOMPARE(QDir::cleanPath(path->text()), expectedFile);
            completed = true;
        });
        action->trigger();
        QVERIFY(completed);
    };
    {
        MainWindow window("/nonexistent/codex", directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                          "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                          directory.filePath("gemini"));
        choose(window, "finishedSound", oldDirectory, firstFile, firstFile);
        choose(window, "failedSound", oldDirectory, "../second/second.ogg", secondFile);
        choose(window, "waitingSound", secondDirectory, "../cancelled", QString(), true);
    }
    QVERIFY(settings.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(settings.readAll()).object();
    settings.close();
    QCOMPARE(saved.value("lastAudioDirectory").toString(), cancelledDirectory);
    QVERIFY(!saved.contains("audioDurationVisible"));
    // The last browsed directory survives cancelling both dialogs and restarting the application.
    MainWindow restarted("/nonexistent/codex", directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                         "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                         directory.filePath("gemini"));
    choose(restarted, "waitingSound", cancelledDirectory, "last.wav", cancelledFile);
}

void MainWindowTest::speakerStopsAudio()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python is needed for the fake audio player");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile player(directory.filePath("ffplay"));
    QVERIFY(player.open(QIODevice::WriteOnly));
    player.write(("#!" + python + "\n").toUtf8());
    player.write(R"PY(import os, sys
with open(__file__ + ".started", "w") as log:
    log.write(sys.argv[-1])
if not sys.argv[-1].endswith("done.mp3"):
    sys.stdin.read()
)PY");
    player.close();
    QVERIFY(player.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QFile audio(directory.filePath("long.mp3"));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.close();
    QFile done(directory.filePath("done.mp3"));
    QVERIFY(done.open(QIODevice::WriteOnly));
    done.close();
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([previousPath] { qputenv("PATH", previousPath); });
    qputenv("PATH", directory.path().toUtf8());
    MainWindow window("/nonexistent/codex", directory.path(), "/nonexistent/python", "/nonexistent/bridge",
                      "/nonexistent/gemini", nullptr, directory.filePath("index.json"), "/nonexistent/agy",
                      directory.filePath("gemini"));
    auto *notifier = window.findChild<Notifier *>();
    auto *speaker = window.findChild<QToolButton *>("muteSounds");
    QVERIFY(notifier && speaker);
    QSignalSpy failures(notifier, &Notifier::playbackFailed);
    QVERIFY(notifier->playSound(audio.fileName()));
    QVERIFY(notifier->isPlaying());
    QVERIFY(speaker->styleSheet().contains("#d32f2f"));
    QVERIFY(speaker->toolTip().contains("stop playback"));
    QTRY_VERIFY(QFileInfo::exists(player.fileName() + ".started"));
    const QPointer<QProcess> process = notifier->findChild<QProcess *>();
    QVERIFY(process);
    speaker->click();
    QVERIFY(!notifier->isPlaying());
    QVERIFY(!notifier->settings().muted);
    QVERIFY(speaker->styleSheet().isEmpty());
    QTRY_VERIFY(!process || process->state() == QProcess::NotRunning);
    QCOMPARE(failures.size(), 0);

    // Cancelling a preview while muted must preserve the mute setting.
    notifier->setMuted(true);
    QVERIFY(notifier->playSound(audio.fileName()));
    speaker->click();
    QVERIFY(!notifier->isPlaying());
    QVERIFY(notifier->settings().muted);
    QVERIFY(speaker->isChecked());
    QVERIFY(notifier->playSound(done.fileName()));
    QTRY_VERIFY(!notifier->isPlaying());
    QVERIFY(speaker->styleSheet().isEmpty());
    QVERIFY(notifier->settings().muted);
    QCOMPARE(failures.size(), 0);
}

void MainWindowTest::piperStopsBeforePlayback()
{
    const QString python = QStandardPaths::findExecutable("python3");
    if (python.isEmpty()) QSKIP("Python is needed for fake speech programs");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile piper(directory.filePath("piper"));
    QVERIFY(piper.open(QIODevice::WriteOnly));
    piper.write(("#!" + python + "\n").toUtf8());
    piper.write(R"PY(import sys, time
text = sys.stdin.read()
output = sys.argv[sys.argv.index("--output_file") + 1]
with open(output, "w") as audio:
    audio.write("fake speech")
with open(__file__ + ".output", "w") as log:
    log.write(output)
if "finish" not in text:
    time.sleep(60)
)PY");
    piper.close();
    QVERIFY(piper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QFile player(directory.filePath("ffplay"));
    QVERIFY(player.open(QIODevice::WriteOnly));
    player.write(("#!" + python + "\n").toUtf8());
    player.write(R"PY(import sys
with open(__file__ + ".started", "w") as log:
    log.write(sys.argv[-1])
sys.stdin.read()
)PY");
    player.close();
    QVERIFY(player.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QFile model(directory.filePath("voice.onnx"));
    QVERIFY(model.open(QIODevice::WriteOnly));
    model.close();
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([previousPath] { qputenv("PATH", previousPath); });
    qputenv("PATH", directory.path().toUtf8());
    Notifier notifier;
    Notifier::Settings settings;
    settings.voiceEngine = "piper";
    settings.piperProgram = piper.fileName();
    settings.piperModel = model.fileName();
    QSignalSpy failures(&notifier, &Notifier::playbackFailed);
    QVERIFY(notifier.say(settings, "cancel this speech"));
    QTRY_VERIFY(QFileInfo::exists(piper.fileName() + ".output"));
    QFile outputLog(piper.fileName() + ".output");
    QVERIFY(outputLog.open(QIODevice::ReadOnly));
    const QString cancelledFile = QString::fromUtf8(outputLog.readAll());
    outputLog.close();
    QVERIFY(QFileInfo::exists(cancelledFile));
    notifier.stopPlayback();
    QVERIFY(!notifier.isPlaying());
    QTRY_VERIFY(!QFileInfo::exists(cancelledFile));
    QVERIFY(!QFileInfo::exists(player.fileName() + ".started"));
    QCOMPARE(failures.size(), 0);

    // After successful synthesis, Stop must kill the player and remove its temporary WAV.
    QVERIFY(notifier.say(settings, "finish synthesizing"));
    QTRY_VERIFY(QFileInfo::exists(player.fileName() + ".started"));
    QFile playerLog(player.fileName() + ".started");
    QVERIFY(playerLog.open(QIODevice::ReadOnly));
    const QString playingFile = QString::fromUtf8(playerLog.readAll());
    playerLog.close();
    QVERIFY(notifier.isPlaying());
    QVERIFY(QFileInfo::exists(playingFile));
    notifier.stopPlayback();
    QTRY_VERIFY(!QFileInfo::exists(playingFile));
    QVERIFY(!notifier.isPlaying());
    QCOMPARE(failures.size(), 0);
}

void MainWindowTest::commandApproval_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("rule");
    QTest::addColumn<bool>("denied");
    QTest::newRow("add") << "git add file.cpp" << "git add" << false;
    QTest::newRow("commit-message") << "git commit -m \"sudo git push; apt install\"" << "git commit" << false;
    QTest::newRow("quoted-file") << "git add 'file with spaces'" << "git add" << false;
    QTest::newRow("escaped-quote") << "git commit -m \"say \\\"hello\\\"\"" << "git commit" << false;
    QTest::newRow("directory") << "git -C '/tmp/project path' add ." << "git add" << false;
    QTest::newRow("shell") << "/bin/bash -lc 'git commit -m hello'" << "git commit" << false;
    QTest::newRow("other-shell") << "/tmp/bash -lc 'git add .'" << "" << false;
    QTest::newRow("shell-extra") << "bash -lc 'git add .' ignored" << "" << false;
    QTest::newRow("compound") << "git add . && git commit -m hello" << "" << false;
    QTest::newRow("pipeline") << "git add . | cat" << "" << false;
    QTest::newRow("newline") << "git add .\ngit status" << "" << false;
    QTest::newRow("redirect") << "git commit -m hello > log" << "" << false;
    QTest::newRow("expansion") << "git add $FILES" << "" << false;
    QTest::newRow("substitution") << "git commit -m \"$(date)\"" << "" << false;
    QTest::newRow("literal-substitution") << "git commit -m '$(sudo foo)'" << "git commit" << false;
    QTest::newRow("config") << "git -c core.hooksPath=/tmp commit -m hello" << "" << false;
    QTest::newRow("env") << "GIT_CONFIG_COUNT=1 git add ." << "" << false;
    QTest::newRow("other-git") << "/tmp/git add ." << "" << false;
    QTest::newRow("unclosed-quote") << "git commit -m 'hello" << "" << false;
    QTest::newRow("push") << "git -C /tmp push origin main" << "" << true;
    QTest::newRow("compound-push") << "git add . && git push" << "" << true;
    QTest::newRow("shell-push") << "bash -lc 'git add .; git push'" << "" << true;
    QTest::newRow("sudo") << "sudo apt install something" << "" << true;
    QTest::newRow("env-sudo") << "env LANG=C sudo true" << "" << true;
    QTest::newRow("wrapper-sudo") << "exec env LANG=C sudo true" << "" << true;
    QTest::newRow("nested-sudo") << "git commit -m \"$(sudo true)\"" << "" << true;
    QTest::newRow("backtick-sudo") << "echo `sudo true`" << "" << true;
    QTest::newRow("apt") << "apt-get -y install something" << "" << true;
    QTest::newRow("dnf") << "dnf install something" << "" << true;
    QTest::newRow("pacman") << "pacman -Syu" << "" << true;
    QTest::newRow("apt-show") << "apt show install" << "" << false;
    QTest::newRow("echo") << "echo 'sudo git push'" << "" << false;
    QTest::newRow("empty-wrapper") << "env" << "" << false;
    QTest::newRow("lookup") << "command -v sudo" << "" << false;
#ifdef Q_OS_WIN
    QTest::newRow("cmd-push") << "cmd.exe /c \"git push\"" << "" << true;
    QTest::newRow("winget") << "winget install package" << "" << true;
    QTest::newRow("powershell") << "powershell -NoProfile -Command 'Install-Package package'" << "" << true;
#endif
}

void MainWindowTest::commandApproval()
{
    QFETCH(QString, command);
    QFETCH(QString, rule);
    QFETCH(bool, denied);
    const CommandApproval result = classifyCommandApproval(command);
    QCOMPARE(result.sessionRule, rule);
    QCOMPARE(!result.deniedReason.isEmpty(), denied);
}

void MainWindowTest::codexCommandTrust()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString program = directory.filePath("fake-codex");
    QFile script(program);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json, os, sys
for line in sys.stdin:
    request = json.loads(line)
    if "method" not in request:
        with open(os.path.join(os.path.dirname(__file__), "responses.jsonl"), "a") as log:
            log.write(json.dumps(request) + "\n")
        continue
    if "id" not in request:
        continue
    result = {}
    if request["method"] == "thread/start":
        result = {"thread": {"id": "test-thread"}}
    elif request["method"] == "thread/resume":
        result = {"thread": {"id": request["params"]["threadId"]}}
    print(json.dumps({"id": request["id"], "result": result}), flush=True)
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    CodexConnection connection(program, directory.path(), directory.filePath("index.json"));
    connection.start();
    QTRY_VERIFY(connection.isConnected());
    CodexAgent chat(&connection, directory.path());
    CodexAgent other(&connection, directory.path());
    QVERIFY(chat.newConversation(directory.path()));
    QTRY_COMPARE(chat.sessionId(), QString("test-thread"));
    QSignalSpy approvals(&chat, &AgentBackend::approvalRequested);
    const auto request = [&](int id, const QString &command, bool network = false) {
        QJsonObject params{{"command", command}, {"cwd", directory.path()},
            {"proposedExecpolicyAmendment", QJsonArray{"git", "add"}}};
        if (network) params.insert("networkApprovalContext", QJsonObject{{"host", "example.test"}});
        chat.handleServerRequest("item/commandExecution/requestApproval", id, params);
    };
    const auto responses = [&] {
        QMap<int, QString> values;
        QFile log(directory.filePath("responses.jsonl"));
        if (!log.open(QIODevice::ReadOnly)) return values;
        while (!log.atEnd()) {
            const QJsonObject object = QJsonDocument::fromJson(log.readLine()).object();
            values.insert(object.value("id").toInt(), object.value("result").toObject().value("decision").toString());
        }
        return values;
    };
    request(100, "git add once.cpp");
    QCOMPARE(approvals.size(), 1);
    chat.answerApproval(approvals.last().first().toInt(), ApprovalDecision::Accept);
    QTRY_COMPARE(responses().value(100), QString("accept"));
    QVERIFY(chat.trustedSessionCommands().isEmpty());
    approvals.clear();
    request(101, "git add first.cpp");
    QCOMPARE(approvals.size(), 1);
    QCOMPARE(approvals.last().at(4).toString(), QString());
    QCOMPARE(approvals.last().at(5).toString(), QString("git add"));
    chat.answerApproval(approvals.last().first().toInt(), ApprovalDecision::AcceptForSession);
    QTRY_COMPARE(responses().value(101), QString("accept"));
    QCOMPARE(chat.trustedSessionCommands(), QStringList{"git add"});
    QVERIFY(other.trustedSessionCommands().isEmpty());
    request(102, "git -C /tmp add second.cpp");
    QTRY_COMPARE(responses().value(102), QString("accept"));
    QCOMPARE(approvals.size(), 1);
    request(103, "git add . && git push");
    QTRY_COMPARE(responses().value(103), QString("decline"));
    QCOMPARE(approvals.size(), 1);
    request(104, "git add .", true);
    QCOMPARE(approvals.size(), 2);
    QVERIFY(approvals.last().at(5).toString().isEmpty());
    chat.answerApproval(approvals.last().first().toInt(), ApprovalDecision::Decline);
    QVERIFY(chat.removeTrustedSessionCommand("git add"));
    QVERIFY(!chat.removeTrustedSessionCommand("git add"));
    request(105, "git add third.cpp");
    QCOMPARE(approvals.size(), 3);
    chat.answerApproval(approvals.last().first().toInt(), ApprovalDecision::AcceptForSession);
    request(106, "git commit -m hello");
    QCOMPARE(approvals.size(), 4);
    chat.answerApproval(approvals.last().first().toInt(), ApprovalDecision::AcceptForSession);
    request(107, "git commit -m 'different message'");
    QTRY_COMPARE(responses().value(107), QString("accept"));
    QCOMPARE(approvals.size(), 4);
    QCOMPARE(chat.trustedSessionCommands().size(), 2);
    QVERIFY(chat.resumeConversation("test-thread", directory.path()));
    QTRY_COMPARE(chat.sessionId(), QString("test-thread"));
    QCOMPARE(chat.trustedSessionCommands().size(), 2);
    QVERIFY(chat.resumeConversation("different-thread", directory.path()));
    QVERIFY(chat.trustedSessionCommands().isEmpty());
    QTRY_COMPARE(chat.sessionId(), QString("different-thread"));
    request(108, "git add again.cpp");
    QCOMPARE(approvals.size(), 5);
    chat.answerApproval(approvals.last().first().toInt(), ApprovalDecision::AcceptForSession);
    QVERIFY(chat.newConversation(directory.path()));
    QVERIFY(chat.trustedSessionCommands().isEmpty());
    QTRY_COMPARE(chat.sessionId(), QString("test-thread"));
}

// Starts a chat through the New chat dialog, choosing the agent and working directory there.
static void startChat(MainWindow &window, const QString &provider, const QString &path)
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
        providerInput->setCurrentText(provider);
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
            send({"method": "turn/started", "params": {"threadId": "test-thread","turn": {"id": "test-turn"}}})
            continue
        send({"method": "item/agentMessage/delta", "params": {"threadId": "test-thread",
            "itemId": "test-item", "delta": "Hello from App Server"}})
        send({"method": "item/completed", "params": {"threadId": "test-thread",
            "item": {"id": "test-item", "type": "agentMessage", "text": "Hello from App Server"}}})
        send({"method": "turn/completed", "params": {"threadId": "test-thread",
            "turn": {"id": "test-turn", "status": "completed"}}})
    elif method == "turn/interrupt":
        send({"id": request["id"], "result": {}})
        send({"method": "turn/completed", "params": {"threadId": "test-thread",
            "turn": {"id": "test-turn", "status": "interrupted"}}})
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    MainWindow window(fakeServer, directory.path());
    window.show();
    auto *input = window.findChild<QPlainTextEdit *>("commandInput");
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
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QVERIFY(output->toPlainText().contains("Commands:"));
    QTRY_VERIFY(output->toPlainText().contains("Codex connection: codex app-server --stdio"));
    QTRY_VERIFY(output->toPlainText().contains("--stdio  Use stdio transport"));
    QVERIFY(output->toPlainText().contains("Manage local daemon"));
    QVERIFY(!output->toPlainText().contains("[Connected to Codex]"));

    QTest::keyClicks(input, "test");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(output->toPlainText().contains("[Connected to Codex]"));
    QTRY_VERIFY(chat->toPlainText().contains("Codex: Hello from App Server"));
    QCOMPARE(chat->toPlainText().count("Hello from App Server"), 1);
    QVERIFY(!output->toPlainText().contains("Hello from App Server"));

    QTest::keyClicks(input, "long response");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(stopButton->isEnabled());
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY(output->toPlainText().contains("[Codex response: interrupted]"));

    QTest::keyClicks(input, "long race");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClicks(input, "stop");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
    auto *input = window.findChild<QPlainTextEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    auto *stopButton = window.findChild<QPushButton *>("stopButton");
    auto *sendButton = window.findChild<QPushButton *>("sendButton");
    QVERIFY(input);
    QVERIFY(output);
    QVERIFY(stopButton);
    QVERIFY(sendButton);
    startChat(window, "Claude", directory.path());
    QTRY_VERIFY(output->toPlainText().contains("[Connected to Claude Agent SDK]"));
    QCOMPARE(sendButton->text(), QString("Send to Claude"));

    const QByteArray previousPath = qgetenv("PATH");
    qputenv("PATH", directory.path().toLocal8Bit());
    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    if (previousPath.isNull()) qunsetenv("PATH");
    else qputenv("PATH", previousPath);
    QVERIFY(output->toPlainText().contains("Claude Agent SDK:"));
    QVERIFY(output->toPlainText().contains("Tool approvals and questions appear in dialogs."));
    QTRY_VERIFY(output->toPlainText().contains("Usage: claude [OPTIONS]"));

    QTest::keyClicks(input, "hello");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(chat->toPlainText().contains("Claude: Hello from Claude"));

    QTest::keyClicks(input, "long response");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
    auto *input = window.findChild<QPlainTextEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    QVERIFY(input);
    QVERIFY(output);
    startChat(window, "GLM", directory.path());
    QTRY_VERIFY(output->toPlainText().contains("[Connected to GLM via Claude Agent SDK]"));
    QTest::keyClicks(input, "hello");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
    auto *input = window.findChild<QPlainTextEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    auto *tree = window.findChild<QTreeWidget *>("conversationTree");
    QVERIFY(input && output && tree);
    startChat(window, "Antigravity", directory.path());
    input->setPlainText("help");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(output->toPlainText().contains("Usage: agy"));
    input->setPlainText("first");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(chat->toPlainText().contains("Antigravity: first answered"));
    QTRY_VERIFY(QFileInfo(directory.filePath("antigravity-conversations.json")).exists());
    input->setPlainText("second");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
    auto *input = window.findChild<QPlainTextEdit *>("commandInput");
    auto *output = window.findChild<QPlainTextEdit *>("log");
    auto *chat = window.findChild<QPlainTextEdit *>("chatView");
    QVERIFY(chat);
    auto *stopButton = window.findChild<QPushButton *>("stopButton");
    QVERIFY(input);
    QVERIFY(output);
    QVERIFY(stopButton);
    startChat(window, "Gemini", directory.path());
    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(output->toPlainText().contains("Usage: gemini"));
    QTest::keyClicks(input, "first");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(chat->toPlainText().contains("Gemini: first answered"));
    QTest::keyClicks(input, "second");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(chat->toPlainText().contains("Gemini: second answered"));
    QTest::keyClicks(input, "long");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
        auto *input = window.findChild<QPlainTextEdit *>("commandInput");
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
        input->setPlainText("write something");
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
        input->setPlainText("continue here");
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
    auto *input = window.findChild<QPlainTextEdit *>("commandInput");
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
    input->setPlainText("more");
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
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
