#include "CommandApproval.h"
#include "shell/CommandCatalog.h"
#include "shell/ShellAst.h"
#include "shell/ShellEvaluator.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

// The decisions agentin takes about shell command lines, checked against tests/data/shell-commands.json
// with the default rules. A case marked "pending" describes behaviour of a later stage of the Bash parsing
// work: it is expected to fail until then, and passing it is reported, so that the mark is removed.
// The parser and the evaluation of commands, which the decisions will use, are tested here as well.
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
    void evaluate_data();
    void evaluate();
    void corpusEvaluates();
    void scripts_data();
    void scripts();
};

namespace {
QJsonArray corpusCases()
{
    QFile file(SHELL_COMMANDS_FILE);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray();
}

// Whether a path is a directory of the list or inside one.
bool inside(const QString &path, const QStringList &directories)
{
    for (const QString &directory : directories)
        if (path == directory || path.startsWith(directory + '/')) return true;
    return false;
}

// What the evaluation alone allows, before any rule: every command is known and judged in full, none removes
// files or uses the network, and all of them write and run only inside the writable directories.
bool catalogAllows(const shell::Evaluation &evaluation, const QStringList &writable)
{
    if (!evaluation.judged) return false;
    for (const shell::CommandUse &use : evaluation.uses) {
        if (!use.problems.isEmpty()) return false;
        if (use.effect == shell::Effect::Unknown || use.effect == shell::Effect::Network || use.effect == shell::Effect::Remove)
            return false;
        for (const QString &path : use.writes + use.executes)
            if (!inside(path, writable)) return false;
    }
    return true;
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
    QTest::newRow("escapes-in-quotes") << "echo $'a b' $'a\\tb'" << "(cmd echo 'a b' [opaque $'a\\tb'])";
    QTest::newRow("append") << "X+=a ls" << "(cmd X+=a ls)";
    QTest::newRow("nested-too-deeply") << QString("( ").repeated(100) + "ls" + QString(" )").repeated(100)
                                       << "(unsupported commands nested too deeply)";
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

// Text cut anywhere, as a command still being written, must give a tree and an evaluation, never a crash or
// a hang.
void ShellApprovalTest::prefixesDoNotCrash()
{
    const shell::Environment environment{"/p", "/h", {}};
    for (const QJsonValue &value : corpusCases()) {
        const QString command = value.toObject().value("command").toString();
        for (int length = 0; length <= command.size(); ++length) {
            const shell::NodePtr tree = shell::parseBash(command.left(length));
            QVERIFY(tree);
            shell::evaluate(tree, environment);
        }
    }
}

// The commands of a line as the evaluation sees them, started in /p with the home directory /h: the words
// with the known values put in, the effect, the directories the command may run in, and what it reads,
// writes and runs.
void ShellApprovalTest::evaluate_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("uses");
    QTest::newRow("cd-and") << "cd a && cat x | head -5"
        << "cd a | none | in /p\n"
           "cat x | read | in /p/a | reads /p/a/x\n"
           "head -5 | read | in /p/a";
    QTest::newRow("cd-semicolon") << "cd a; cat x"
        << "cd a | none | in /p\n"
           "cat x | read | in /p /p/a | reads /p/x /p/a/x";
    QTest::newRow("cd-or-exit") << "cd a || exit 1; touch x"
        << "cd a | none | in /p\n"
           "exit 1 | none | in /p/a\n"
           "touch x | write | in /p/a | writes /p/a/x";
    QTest::newRow("cd-outside") << "cd /etc && rm passwd"
        << "cd /etc | none | in /p\n"
           "rm passwd | remove | in /etc | writes /etc/passwd";
    QTest::newRow("cd-home") << "cd && touch x"
        << "cd | none | in /p\n"
           "touch x | write | in /h | writes /h/x";
    QTest::newRow("cd-parent") << "cd .. && cd /a/../b && pwd"
        << "cd .. | none | in /p\n"
           "cd /a/../b | none | in /\n"
           "pwd | none | in /b";
    QTest::newRow("cd-unknown") << "cd $d && cat x"
        << "cd $d | none | in /p\n"
           "cat x | read | in ? | problem: the directory of x is not known";
    QTest::newRow("cd-back") << "cd a && cd - && touch x"
        << "cd a | none | in /p\n"
           "cd - | none | in /p/a\n"
           "touch x | write | in ? | problem: the directory of x is not known";
    QTest::newRow("subshell-scope") << "(cd sub && touch x); touch y"
        << "cd sub | none | in /p\n"
           "touch x | write | in /p/sub | writes /p/sub/x\n"
           "touch y | write | in /p | writes /p/y";
    QTest::newRow("group-scope") << "{ cd a; }; touch x"
        << "cd a | none | in /p\n"
           "touch x | write | in /p /p/a | writes /p/x /p/a/x";
    QTest::newRow("pipeline-scope") << "cd a | cat; touch x"
        << "cd a | none | in /p\n"
           "cat | read | in /p\n"
           "touch x | write | in /p | writes /p/x";
    QTest::newRow("if-branches") << "if test -f x; then cd a; else cd b; fi; touch y"
        << "test -f x | none | in /p\n"
           "cd a | none | in /p\n"
           "cd b | none | in /p\n"
           "touch y | write | in /p /p/a /p/b | writes /p/y /p/a/y /p/b/y";
    QTest::newRow("while-loop") << "while read l; do echo $l; cd ..; touch x; done < f"
        << "< f | none | in /p | reads /p/f\n"
           "read l | none | in ? | env REPLY l\n"
           "echo $l | none | in ? | env REPLY l\n"
           "cd .. | none | in ? | env REPLY l\n"
           "touch x | write | in ? | env REPLY l | problem: the directory of x is not known";
    QTest::newRow("assignment") << "X=build; cmake --build $X && echo \"rc=$?\""
        << "cmake --build build | build | in /p | writes /p/build | runs /p/build | env X\n"
           "echo \"rc=$?\" | none | in /p | env X";
    QTest::newRow("assignment-chain") << "X=a && Y=$X/b; cat $Y"
        << "cat a/b | read | in /p | reads /p/a/b | env X Y";
    QTest::newRow("assignment-after-failure") << "X=$(false) && Y=1; echo > $Y"
        << "false | none | in /p\n"
           "echo | none | in /p | env X Y | problem: the target of a redirection is not known: $Y";
    QTest::newRow("assignment-in-branch") << "x=/tmp; if true; then x=/etc; fi; touch $x/f"
        << "true | none | in /p | env x\n"
           "touch $x/f | write | in /p | env x | problem: the value of $x/f is not known";
    QTest::newRow("assignment-append") << "X=/tmp; X+=/../etc; touch $X/f"
        << "touch $X/f | write | in /p | env X | problem: the value of $X/f is not known";
    QTest::newRow("unquoted-spaces") << "X=\"a b\"; touch $X \"$X\""
        << "touch $X a b | write | in /p | writes /p/a b | env X | problem: the value of $X is not known";
    QTest::newRow("tilde") << "cat ~/.ssh/id_rsa ~user/x \"~/q\""
        << "cat /h/.ssh/id_rsa ~user/x ~/q | read | in /p | reads /h/.ssh/id_rsa /p/~/q | problem: the value of ~user/x is not known";
    QTest::newRow("tilde-in-assignment") << "X=~/.ssh; cat $X/id_rsa"
        << "cat /h/.ssh/id_rsa | read | in /p | reads /h/.ssh/id_rsa | env X";
    QTest::newRow("home-changed") << "export HOME=/tmp/x; cat ~/y"
        << "export HOME=/tmp/x | none | in /p\n"
           "cat /tmp/x/y | read | in /p | reads /tmp/x/y | env HOME";
    QTest::newRow("separator-changed") << "IFS=/; X=/tmp/a; touch $X"
        << "touch $X | write | in /p | env IFS X | problem: the value of $X is not known";
    QTest::newRow("search-path-for-cd") << "CDPATH=/etc; cd x && touch y"
        << "cd x | none | in /p | env CDPATH\n"
           "touch y | write | in ? | env CDPATH | problem: the directory of y is not known";
    QTest::newRow("dynamic-variable") << "OLDPWD=/tmp; cd /etc && touch $OLDPWD/x"
        << "cd /etc | none | in /p | env OLDPWD\n"
           "touch $OLDPWD/x | write | in /etc | env OLDPWD | problem: the value of $OLDPWD/x is not known";
    QTest::newRow("braces") << "cat {a,b}.txt"
        << "cat {a,b}.txt | read | in /p | problem: the value of {a,b}.txt is not known";
    QTest::newRow("escapes-not-decoded") << "cat $'\\x2e'"
        << "cat $'\\x2e' | read | in /p | problem: the value of $'\\x2e' is not known";
    QTest::newRow("environment-prefix") << "LD_PRELOAD=./x.so ls"
        << "ls | read | in /p | env LD_PRELOAD";
    QTest::newRow("environment-assigned") << "PATH=.; make"
        << "make | build | in /p | writes /p | runs /p | env PATH";
    QTest::newRow("for-literal") << "for d in a b; do cmake --build $d -j16 2>&1 | grep -E \"error\"; (cd $d && ctest); done"
        << "cmake --build a -j16 | build | in /p | writes /p/a | runs /p/a | env d\n"
           "grep -E error | read | in /p | env d\n"
           "cd a | none | in /p | env d\n"
           "ctest | build | in /p/a | writes /p/a | runs /p/a | env d\n"
           "cmake --build b -j16 | build | in /p | writes /p/b | runs /p/b | env d\n"
           "grep -E error | read | in /p | env d\n"
           "cd b | none | in /p | env d\n"
           "ctest | build | in /p/b | writes /p/b | runs /p/b | env d";
    QTest::newRow("for-substitution") << "for d in $(ls); do cat $d/x; done"
        << "ls | read | in /p\n"
           "cat $d/x | read | in /p | env d | problem: the value of $d/x is not known";
    QTest::newRow("for-pattern") << "for f in src/*.cpp; do wc -l \"$f\"; cat $f; done"
        << "wc -l src/*.cpp | read | in /p | reads /p/src/*.cpp | env f\n"
           "cat $f | read | in /p | env f | problem: the value of $f is not known";
    QTest::newRow("for-break") << "for i in 1 2; do break; done"
        << "cannot judge: break in a loop\n"
           "break | none | in /p | env i\n"
           "break | none | in /p | env i";
    QTest::newRow("substitution") << "x=$(curl https://example.com); echo $x"
        << "curl https://example.com | network | in /p\n"
           "echo $x | none | in /p | env x";
    QTest::newRow("here-document-substitution") << "cat <<EOF\n$(rm -rf x)\nEOF"
        << "rm -rf x | remove | in /p | writes /p/x\n"
           "cat | read | in /p";
    QTest::newRow("here-document-quoted") << "cat <<'EOF'\n$(rm -rf x)\nEOF"
        << "cat | read | in /p";
    QTest::newRow("commit-here-document") << "git commit -m \"$(cat <<'EOF'\nFix it\nEOF\n)\""
        << "cat | read | in /p\n"
           "git commit -m \"$(cat <<'EOF'\n"
           "Fix it\n"
           "EOF\n"
           ")\" | git-write | in /p | writes /p";
    QTest::newRow("input-substitution") << "echo \"$(< ~/.ssh/id_rsa)\""
        << "< ~/.ssh/id_rsa | none | in /p | reads /h/.ssh/id_rsa\n"
           "echo \"$(< ~/.ssh/id_rsa)\" | none | in /p";
    QTest::newRow("process-substitution") << "diff <(ls a) <(ls b)"
        << "ls a | read | in /p | reads /p/a\n"
           "ls b | read | in /p | reads /p/b\n"
           "diff /dev/fd/63 /dev/fd/63 | read | in /p";
    QTest::newRow("redirections") << "ls >> a < in 2>&1; ls &> out; > empty; wc -l x 2>/dev/null >&2"
        << "ls | read | in /p | reads /p/in | writes /p/a\n"
           "ls | read | in /p | writes /p/out\n"
           "> empty | none | in /p | writes /p/empty\n"
           "wc -l x | read | in /p | reads /p/x";
    QTest::newRow("redirection-unknown") << "echo a > $f; echo b > *.txt; echo c > /dev/tcp/h/80"
        << "echo a | none | in /p | problem: the target of a redirection is not known: $f\n"
           "echo b | none | in /p | problem: the target of a redirection is not known: *.txt\n"
           "echo c | none | in /p | problem: a redirection that opens a network connection";
    QTest::newRow("compound-redirection") << "{ ls; } > /etc/x"
        << "> /etc/x | none | in /p | writes /etc/x\n"
           "ls | read | in /p";
    QTest::newRow("expansion-sets-variable") << "echo ${y:=1}"
        << "cannot judge: an expansion that sets a variable\n"
           "echo ${y:=1} | none | in /p";
    QTest::newRow("substitution-in-index") << "echo ${arr[$(ls)]}"
        << "cannot judge: an expansion inside an index\n"
           "echo ${arr[$(ls)]} | none | in /p";
    QTest::newRow("arithmetic-sets-variable") << "echo $((x=1))"
        << "cannot judge: arithmetic that sets a variable\n"
           "echo $((x=1)) | none | in /p";
    QTest::newRow("background") << "ls &"
        << "cannot judge: a command that runs in the background\n"
           "ls | read | in /p";
    QTest::newRow("unsupported") << "case $x in a) ls;; esac"
        << "cannot judge: case is not supported";
    QTest::newRow("shell-script") << "bash -lc 'cd sub && git add -A && git commit -m x'"
        << "cd sub | none | in /p\n"
           "git add -A | git-write | in /p/sub | writes /p/sub\n"
           "git commit -m x | git-write | in /p/sub | writes /p/sub";
    QTest::newRow("shell-script-variable") << "X=1 bash -c 'echo hi > out$X'"
        << "echo hi | none | in /p | writes /p/out1 | env X";
    QTest::newRow("shell-script-directory") << "sh -c 'cd /etc; echo x > passwd'"
        << "cd /etc | none | in /p\n"
           "echo x | none | in /etc /p | writes /etc/passwd /p/passwd";
    QTest::newRow("wrappers") << "timeout 5 env A=1 nice -n 5 rm -rf x"
        << "rm -rf x | remove | in /p | writes /p/x | env A";
    QTest::newRow("xargs") << "git ls-files | xargs wc -l"
        << "git ls-files | git-read | in /p\n"
           "wc -l | read | in /p | arguments from input";
    QTest::newRow("xargs-contents") << "git ls-files | xargs cat; ls | xargs rm; ls | xargs grep -n x"
        << "git ls-files | git-read | in /p\n"
           "cat | read | in /p | arguments from input | problem: the files come from the input, so a secret file could be shown\n"
           "ls | read | in /p\n"
           "rm | remove | in /p | arguments from input | problem: the arguments come from the input\n"
           "ls | read | in /p\n"
           "grep -n x | read | in /p | arguments from input | problem: the files come from the input, so a secret file could be shown";
    QTest::newRow("xargs-replace") << "ls | xargs -I{} cp {} /tmp/out"
        << "ls | read | in /p\n"
           "cp {} /tmp/out | write | in /p | problem: the value of {} is not known";
    QTest::newRow("not-judged") << "eval \"$cmd\"; $prog arg; source s.sh; bash s.sh; echo x | sh"
        << "eval \"$cmd\" | unknown | in /p | problem: eval runs text that cannot be judged\n"
           "$prog arg | unknown | in /p | problem: the program is not known\n"
           "source s.sh | unknown | in /p | problem: source runs text that cannot be judged\n"
           "bash s.sh | unknown | in /p | problem: a script that is not given as text\n"
           "echo x | none | in /p\n"
           "sh | unknown | in /p | problem: a script that is not given as text";
    QTest::newRow("test-bracket") << "[ -f \"$f\" ] && cat x > /etc/y"
        << "[ -f \"$f\" ] | none | in /p\n"
           "cat x | read | in /p | reads /p/x | writes /etc/y";
    QTest::newRow("privilege") << "env sudo ls"
        << "sudo ls | unknown | in /p | problem: privilege escalation";
    QTest::newRow("program-path") << "QCE=$HOME/.local build/tests/test_x 2>&1 | tail; /usr/bin/git status; /usr/lib/x/prog"
        << "build/tests/test_x | execute | in /p | runs /p/build/tests/test_x | env QCE\n"
           "tail | read | in /p\n"
           "/usr/bin/git status | git-read | in /p\n"
           "/usr/lib/x/prog | execute | in /p | runs /usr/lib/x/prog";
    QTest::newRow("sed") << "sed -n 95,120p f; sed -i s/a/b/ /etc/hosts; sed 's/a/b/e' f; sed -e '1w out' f"
        << "sed -n 95,120p f | read | in /p | reads /p/f\n"
           "sed -i s/a/b/ /etc/hosts | write | in /p | writes /etc/hosts\n"
           "sed s/a/b/e f | unknown | in /p | reads /p/f | problem: the sed script may write files or run programs\n"
           "sed -e 1w out f | unknown | in /p | reads /p/f | problem: the sed script may write files or run programs";
    QTest::newRow("sort-uniq") << "sort -o /etc/x y; uniq a b"
        << "sort -o /etc/x y | read | in /p | reads /p/y | writes /etc/x\n"
           "uniq a b | read | in /p | reads /p/a | writes /p/b";
    QTest::newRow("awk") << "awk '{print $1}' f; awk 'NR>3' f; awk '{print > \"x\"}' f"
        << "awk {print $1} f | read | in /p | reads /p/f\n"
           "awk NR>3 f | read | in /p | reads /p/f\n"
           "awk {print > \"x\"} f | unknown | in /p | reads /p/f | problem: the awk program may write files or run programs";
    QTest::newRow("find") << "find . -name '*.cpp'; find . -name x -exec rm {} \\;; find / -delete"
        << "find . -name *.cpp | read | in /p | reads /p\n"
           "find . -name x -exec rm {} ; | unknown | in /p | reads /p | problem: find with -exec writes files or runs programs\n"
           "find / -delete | unknown | in /p | reads / | problem: find with -delete writes files or runs programs";
    QTest::newRow("grep") << "grep -rn \"pat\" src; grep -e \"$p\" -f pats x; rg --pre ./evil x"
        << "grep -rn pat src | read | in /p | reads /p/src\n"
           "grep -e \"$p\" -f pats x | read | in /p | reads /p/pats /p/x\n"
           "rg --pre ./evil x | unknown | in /p | reads /p/x | problem: the option --pre runs a program";
    QTest::newRow("copy") << "cp a b; cp -r /usr/share/x .; mv a /etc/b; ln -s /etc/passwd x"
        << "cp a b | write | in /p | reads /p/a | writes /p/b\n"
           "cp -r /usr/share/x . | write | in /p | reads /usr/share/x | writes /p\n"
           "mv a /etc/b | write | in /p | writes /etc/b /p/a\n"
           "ln -s /etc/passwd x | write | in /p | writes /p/x /etc/passwd";
    QTest::newRow("cmake") << "cmake -S . -B /tmp/b -DX=OFF && cmake --build /tmp/b -j16; cmake --build b --target install; cmake -P x.cmake"
        << "cmake -S . -B /tmp/b -DX=OFF | build | in /p | writes /tmp/b | runs /p\n"
           "cmake --build /tmp/b -j16 | build | in /p | writes /tmp/b | runs /tmp/b\n"
           "cmake --build b --target install | unknown | in /p | writes /p/b | runs /p/b | problem: the target install writes outside the project\n"
           "cmake -P x.cmake | unknown | in /p | problem: cmake -P cannot be judged";
    QTest::newRow("ctest-make") << "ctest --test-dir build -R kate -j8; ctest -S s.cmake; make -C sub -j8 all; make install; ninja -t clean"
        << "ctest --test-dir build -R kate -j8 | build | in /p | writes /p/build | runs /p/build\n"
           "ctest -S s.cmake | unknown | in /p | writes /p | runs /p | problem: ctest with -S cannot be judged; ctest with s.cmake cannot be judged\n"
           "make -C sub -j8 all | build | in /p | writes /p/sub | runs /p/sub\n"
           "make install | unknown | in /p | writes /p | runs /p | problem: the target install writes outside the project\n"
           "ninja -t clean | unknown | in /p | writes /p | runs /p | problem: ninja with -t cannot be judged";
    QTest::newRow("git-read") << "git log --oneline | head -5; git diff --output=/etc/x; git branch; git tag -l 'v*'; git config user.name"
        << "git log --oneline | git-read | in /p\n"
           "head -5 | read | in /p\n"
           "git diff --output=/etc/x | unknown | in /p | problem: the option --output cannot be judged\n"
           "git branch | git-read | in /p\n"
           "git tag -l v* | git-read | in /p | reads /p/v*\n"
           "git config user.name | git-read | in /p";
    QTest::newRow("git-write") << "git -C sub add a.txt; git branch new; git tag -fa v1 -m \"x y\" HEAD; git commit -m \"$msg\"; git add $files"
        << "git -C sub add a.txt | git-write | in /p | writes /p/sub\n"
           "git branch new | git-write | in /p | writes /p\n"
           "git tag -fa v1 -m x y HEAD | git-write | in /p | writes /p\n"
           "git commit -m \"$msg\" | git-write | in /p | writes /p\n"
           "git add $files | git-write | in /p | writes /p | problem: the value of $files is not known";
    QTest::newRow("git-other") << "git push; git remote show origin; git -c core.pager=x log; git rebase -x 'cmd' main; git config user.name X"
        << "git push | network | in /p\n"
           "git remote show origin | network | in /p\n"
           "git -c core.pager=x log | unknown | in /p | problem: git with options that change its configuration or repository\n"
           "git rebase -x cmd main | unknown | in /p | writes /p | problem: git rebase that runs commands or an editor\n"
           "git config user.name X | unknown | in /p";
}

void ShellApprovalTest::evaluate()
{
    QFETCH(QString, command);
    QFETCH(QString, uses);
    const shell::Environment environment{"/p", "/h", {}};
    QCOMPARE(shell::dumpEvaluation(shell::evaluate(shell::parseBash(command), environment)), uses);
}

// The corpus against the evaluation alone: a line that must run without a question has to be allowed by it,
// and a line that must ask or be declined is refused by it, or is one of those that only the rules of the
// next stage can catch: secret files, the .git directory, dangerous variables and the Git commands that ask.
void ShellApprovalTest::corpusEvaluates()
{
    QFile file(SHELL_COMMANDS_FILE);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject corpus = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject directories = corpus.value("directories").toObject();
    const auto directory = [&directories](const QString &name) { return directories.value(name).toString(name); };
    QStringList writable;
    for (const QJsonValue &value : corpus.value("writable").toArray()) writable.append(directory(value.toString()));
    const QStringList leftToRules{"status-tag-log", "git-directory-redirect-denied", "redirect-into-git-hooks", "read-ssh-key",
                                  "read-api-key-file", "grep-env-file", "ld-preload", "path-prefix", "reset-hard", "tag-force"};
    for (const QJsonValue &value : corpus.value("cases").toArray()) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value("name").toString();
        const shell::Environment environment{directory(entry.value("cwd").toString()), "/home/user", {}};
        const shell::Evaluation evaluation = shell::evaluate(shell::parseBash(entry.value("command").toString()), environment);
        const bool expected = entry.value("expect").toString() == "allow" || leftToRules.contains(name);
        QVERIFY2(catalogAllows(evaluation, writable) == expected, qPrintable(name + ":\n" + shell::dumpEvaluation(evaluation)));
    }
}

// The scripts of sed and awk that only read.
void ShellApprovalTest::scripts_data()
{
    QTest::addColumn<QString>("program");
    QTest::addColumn<QString>("script");
    QTest::addColumn<bool>("safe");
    for (const char *script : {"p", "95,120p", "$p", "1!G;h;$!d", "/re/,/end/p", "/re/{p;q}", "s/a/b/g", "s|a/b|c|2p", "s/a/b/;s/c/d/I",
                               "2,+3d", "1~2p", "y/abc/xyz/", "5q", "$!N;s/\\n/ /", ""})
        QTest::newRow(qPrintable(QString("sed %1").arg(script))) << "sed" << script << true;
    for (const char *script : {"s/a/b/e", "s/a/b/w out", "s/a/b/gw out", "1e ls", "e", "w out", "1,5w out", "r in", "p;w out",
                               "/re/{p;w out}", "1{e ls", "a text", "i\\ text", "s/a/b", "/re", "bx", "\\%re%p", "1,p", "W out", "R in"})
        QTest::newRow(qPrintable(QString("sed %1").arg(script))) << "sed" << script << false;
    for (const char *script : {"{print $1}", "NR>3", "NR==1 || NR==5", "/re/ {n++} END {print n}", "BEGIN{FS=\":\"} {print $2, $NF}"})
        QTest::newRow(qPrintable(QString("awk %1").arg(script))) << "awk" << script << true;
    for (const char *script : {"{print > \"x\"}", "{print | \"sh\"}", "BEGIN{system(\"ls\")}", "{\"date\" | getline d}",
                               "{printf \"%s\", $1 >> \"x\"}", "@include \"x\"", "BEGIN{ARGV[1]=\"/etc/passwd\"; ARGC=2} {print}",
                               "NR>3 {print}"})
        QTest::newRow(qPrintable(QString("awk %1").arg(script))) << "awk" << script << false;
}

void ShellApprovalTest::scripts()
{
    QFETCH(QString, program);
    QFETCH(QString, script);
    QFETCH(bool, safe);
    QCOMPARE(program == "sed" ? shell::sedScriptIsSafe(script) : shell::awkProgramIsSafe(script), safe);
}

QTEST_GUILESS_MAIN(ShellApprovalTest)
#include "ShellApprovalTest.moc"
