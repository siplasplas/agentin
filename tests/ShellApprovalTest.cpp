#include "CommandApproval.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

// The decisions agentin takes about shell command lines, checked against tests/data/shell-commands.json
// with the default rules. A case marked "pending" describes behaviour of a later stage of the Bash parsing
// work: it is expected to fail until then, and passing it is reported, so that the mark is removed.
class ShellApprovalTest : public QObject
{
    Q_OBJECT

private slots:
    void verdict_data();
    void verdict();
};

namespace {
QString decisionName(CommandDecision decision)
{
    switch (decision) {
    case CommandDecision::Allow: return "allow";
    case CommandDecision::Ask: return "ask";
    case CommandDecision::Deny: return "deny";
    case CommandDecision::None: break;
    }
    return "agent";
}
}

void ShellApprovalTest::verdict_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("pending");
    QFile file(SHELL_COMMANDS_FILE);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
    const QJsonObject corpus = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray cases = corpus.value("cases").toArray();
    QVERIFY(!cases.isEmpty());
    for (const QJsonValue &value : cases) {
        const QJsonObject entry = value.toObject();
        // The working and writable directories, and the commands an ask names ("asks"), are used from the
        // stage where the verdict knows them; today only the decision is compared.
        QTest::newRow(qPrintable(entry.value("name").toString()))
            << entry.value("command").toString() << entry.value("expect").toString() << entry.value("pending").toBool();
    }
}

void ShellApprovalTest::verdict()
{
    QFETCH(QString, command);
    QFETCH(QString, expected);
    QFETCH(bool, pending);
    setCommandRules(defaultCommandRules());
    const CommandVerdict verdict = commandRuleVerdict(command);
    if (pending) QEXPECT_FAIL("", "planned for a later stage of the Bash parsing work", Continue);
    QCOMPARE(decisionName(verdict.decision), expected);
}

QTEST_GUILESS_MAIN(ShellApprovalTest)
#include "ShellApprovalTest.moc"
