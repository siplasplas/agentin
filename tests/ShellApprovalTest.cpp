#include "CommandApproval.h"
#include "shell/ShellAst.h"

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
    void parse_data();
    void parse();
    void corpusParses();
    void prefixesDoNotCrash();
};

namespace {
QJsonArray corpusCases()
{
    QFile file(SHELL_COMMANDS_FILE);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray();
}

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

void ShellApprovalTest::parse_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("tree");
    QTest::newRow("simple") << "ls -la src" << "(cmd ls -la src)";
    QTest::newRow("and-pipe") << "cd a && ls | head -5" << "(list (cmd cd a) && (pipe (cmd ls) | (cmd head -5)))";
    QTest::newRow("separators") << "a; b || c &" << "(list (cmd a) ; (cmd b) || (cmd c) &)";
    QTest::newRow("newlines") << "a\nb\n" << "(list (cmd a) ; (cmd b) ;)";
    QTest::newRow("null-redirect") << "wc -l x 2>/dev/null" << "(cmd wc -l x 2>/dev/null)";
    QTest::newRow("duplicate") << "make 2>&1 | tail" << "(pipe (cmd make 2>&1) | (cmd tail))";
    QTest::newRow("both-outputs") << "make &>log" << "(cmd make &>log)";
    QTest::newRow("variables") << "echo \"rc=${PIPESTATUS[0]} $? $d\"" << "(cmd echo \"rc=${PIPESTATUS[0]} ${?} ${d}\")";
    QTest::newRow("for") << "for d in a b; do echo $d; done" << "(for d in a b (list (cmd echo ${d}) ;))";
    QTest::newRow("for-newlines") << "for d in a b\ndo\n  ls $d\ndone" << "(for d in a b (list (cmd ls ${d}) ;))";
    QTest::newRow("subshell") << "(cd $d && ctest)" << "(subshell (list (cmd cd ${d}) && (cmd ctest)))";
    QTest::newRow("group-redirect") << "{ ls; } > out" << "(group (list (cmd ls) ;) >out)";
    QTest::newRow("assignments") << "X=1 Y=$HOME/a prog" << "(cmd X=1 Y=${HOME}/a prog)";
    QTest::newRow("assignment-only") << "x=$(date)" << "(cmd x=$((cmd date)))";
    QTest::newRow("commit-heredoc") << "git commit -m \"$(cat <<'EOF'\nline )\nEOF\n)\""
                                    << "(cmd git commit -m \"$((list (cmd cat <<'EOF'{line )}) ;))\")";
    QTest::newRow("backticks") << "echo `date`" << "(cmd echo $((cmd date)))";
    QTest::newRow("if") << "if test -f x; then cat x; else echo no; fi"
                        << "(if (list (cmd test -f x) ;) (list (cmd cat x) ;) (list (cmd echo no) ;))";
    QTest::newRow("while-input") << "while read l; do echo $l; done < f"
                                 << "(while (list (cmd read l) ;) (list (cmd echo ${l}) ;) <f)";
    QTest::newRow("globs-tilde") << "ls *.h ~/x ~user" << "(cmd ls *.h ~/x ~user)";
    QTest::newRow("heredoc-then-command") << "cat <<EOF\nhi $x\nEOF\necho next"
                                          << "(list (cmd cat <<EOF{hi $x}) ; (cmd echo next))";
    QTest::newRow("substitution-assignment") << "x=$(curl https://e.com); echo $x"
                                             << "(list (cmd x=$((cmd curl https://e.com))) ; (cmd echo ${x}))";
    QTest::newRow("shell-wrapper") << "bash -lc 'git add .'" << "(cmd bash -lc 'git add .')";
    QTest::newRow("process-substitution") << "diff <(ls a) b" << "(cmd diff [opaque <(ls a) (cmd ls a)] b)";
    QTest::newRow("negation") << "! grep x f" << "(pipe ! (cmd grep x f))";
    QTest::newRow("test") << "[[ -f x && -d y ]] && ls" << "(list (test [[ -f x && -d y ]]) && (cmd ls))";
    QTest::newRow("continuation") << "echo a \\\n b" << "(cmd echo a b)";
    QTest::newRow("comments") << "# note\nls # trailing" << "(cmd ls)";
    QTest::newRow("escapes-quotes") << "grep -E 'a|b' \"x y\" c\\ d" << "(cmd grep -E 'a|b' \"x y\" c d)";
    QTest::newRow("parameter-operator") << "echo ${x:-none}" << "(cmd echo [opaque ${x:-none}])";
    QTest::newRow("time") << "time make -j4" << "(cmd make -j4)";
    QTest::newRow("case") << "case $x in a) ls;; esac" << "(unsupported case is not supported)";
    QTest::newRow("function") << "f() { ls; }" << "(unsupported functions are not supported)";
    QTest::newRow("array") << "a=(1 2)" << "(unsupported array assignments are not supported)";
    QTest::newRow("arithmetic-for") << "for ((i=0;i<3;i++)); do :; done" << "(unsupported arithmetic for loops are not supported)";
    QTest::newRow("unclosed-quote") << "echo 'x" << "(unsupported unclosed ')";
    QTest::newRow("dangling-and") << "ls &&" << "(unsupported command missing after &&)";
}

void ShellApprovalTest::parse()
{
    QFETCH(QString, command);
    QFETCH(QString, tree);
    QCOMPARE(shell::dumpTree(shell::parseBash(command)), tree);
}

void ShellApprovalTest::corpusParses()
{
    const QJsonArray cases = corpusCases();
    QVERIFY(!cases.isEmpty());
    for (const QJsonValue &value : cases) {
        const QJsonObject entry = value.toObject();
        const shell::NodePtr tree = shell::parseBash(entry.value("command").toString());
        QVERIFY2(shell::hasUnsupported(tree) == entry.value("unsupported").toBool(),
                 qPrintable(entry.value("name").toString() + ": " + shell::dumpTree(tree)));
    }
}

// Text cut anywhere, as a command still being written, must give a tree, never a crash or a hang.
void ShellApprovalTest::prefixesDoNotCrash()
{
    for (const QJsonValue &value : corpusCases()) {
        const QString command = value.toObject().value("command").toString();
        for (int length = 0; length <= command.size(); ++length) QVERIFY(shell::parseBash(command.left(length)));
    }
}

QTEST_GUILESS_MAIN(ShellApprovalTest)
#include "ShellApprovalTest.moc"
