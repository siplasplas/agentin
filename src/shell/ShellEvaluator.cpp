#include "shell/ShellEvaluator.h"

#include <QDir>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace shell {

namespace {

// The bodies of for loops are evaluated once per value up to this many times in a line; beyond it, and for
// lists that are not known, a body is evaluated with its variable not known.
constexpr int kMaximumIterations = 64;
constexpr int kMaximumDirectories = 8;
constexpr int kMaximumUses = 1000;
constexpr int kMaximumScriptDepth = 8;
constexpr int kMaximumLoopPasses = 8;
// Loops inside loops are evaluated again for each pass of the loop around them.
constexpr int kMaximumSteps = 20000;

bool hasGlob(const QString &text)
{
    return text.contains('*') || text.contains('?') || text.contains('[');
}

// NAME=, also with += and in a word that is not known, where the text is the word as written.
const QRegularExpression &assignmentPattern()
{
    static const QRegularExpression pattern("^([A-Za-z_][A-Za-z0-9_]*)(\\+?)=");
    return pattern;
}

bool isName(const QString &text)
{
    static const QRegularExpression pattern("^[A-Za-z_][A-Za-z0-9_]*$");
    return pattern.match(text).hasMatch();
}

void appendUnique(QStringList &list, const QString &value)
{
    if (!list.contains(value)) list.append(value);
}

// What the line knows at one point: where it is and the values of variables. A variable that is not in
// variables has a value that is not known.
struct Scope
{
    bool directoryKnown = false;
    // Sorted; more than one after a cd that may have failed.
    QStringList directories;
    QMap<QString, QString> variables;
    // The variables the line has set, whatever their values.
    QSet<QString> assigned;

    bool operator==(const Scope &other) const
    {
        return directoryKnown == other.directoryKnown && directories == other.directories && variables == other.variables
            && assigned == other.assigned;
    }

    void set(const QString &name, const QString &value)
    {
        // Variables that the shell changes on its own do not keep the value they are given.
        static const QSet<QString> dynamic{"OLDPWD", "RANDOM", "SRANDOM", "SECONDS", "LINENO", "BASHPID", "EPOCHSECONDS",
                                           "EPOCHREALTIME", "DIRSTACK", "FUNCNAME", "GROUPS", "HISTCMD", "BASH_COMMAND",
                                           "BASH_SUBSHELL", "PIPESTATUS", "OPTARG", "OPTIND", "REPLY", "_"};
        if (dynamic.contains(name)) variables.remove(name);
        else variables.insert(name, value);
        assigned.insert(name);
    }

    // The home directory as the line has it, or an empty string when it is not known.
    QString home() const { return variables.value("HOME"); }

    void forget(const QString &name)
    {
        variables.remove(name);
        assigned.insert(name);
    }

    void forgetDirectory()
    {
        directoryKnown = false;
        directories.clear();
        variables.remove("PWD");
    }

    void setDirectories(QStringList list)
    {
        list.removeDuplicates();
        list.sort();
        if (list.isEmpty() || list.size() > kMaximumDirectories) {
            forgetDirectory();
            return;
        }
        directoryKnown = true;
        directories = list;
        if (list.size() == 1) variables.insert("PWD", list.first());
        else variables.remove("PWD");
    }

    void mergeDirectories(const Scope &other)
    {
        if (!directoryKnown || !other.directoryKnown) forgetDirectory();
        else setDirectories(directories + other.directories);
    }

    // After this, the scope holds what is true whichever of the two states the shell is in.
    void merge(const Scope &other)
    {
        mergeDirectories(other);
        for (auto it = variables.begin(); it != variables.end();) {
            const auto found = other.variables.constFind(it.key());
            if (found == other.variables.constEnd() || *found != *it) it = variables.erase(it);
            else ++it;
        }
        assigned.unite(other.assigned);
    }
};

// The state at the start of a loop's body when the body may already have run: what the body changes is not
// known, also the directory, as each round may move it further.
Scope widened(const Scope &before, const Scope &after)
{
    Scope result = before;
    if (before.directoryKnown != after.directoryKnown || before.directories != after.directories) result.forgetDirectory();
    for (auto it = result.variables.begin(); it != result.variables.end();) {
        const auto found = after.variables.constFind(it.key());
        if (found == after.variables.constEnd() || *found != *it) it = result.variables.erase(it);
        else ++it;
    }
    result.assigned.unite(after.assigned);
    return result;
}

struct Expansion
{
    QString text;
    bool known = true;
};

class Evaluator
{
public:
    Evaluator() = default;

    Evaluation result;

    void evaluate(const NodePtr &node, Scope &scope)
    {
        if (!node) {
            fail("an empty command");
            return;
        }
        if (++steps_ > kMaximumSteps) {
            fail("too much to follow");
            return;
        }
        switch (node->kind) {
        case Node::Kind::List: evaluateList(node, scope); break;
        case Node::Kind::Pipeline:
            // Each command of a pipeline runs in its own shell.
            for (const NodePtr &child : node->children) {
                if (node->children.size() == 1) {
                    evaluate(child, scope);
                } else {
                    Scope inner = scope;
                    evaluate(child, inner);
                }
            }
            break;
        case Node::Kind::Simple: evaluateSimple(node, scope); break;
        case Node::Kind::Subshell: {
            redirections(node, scope);
            Scope inner = scope;
            evaluate(node->children.value(0), inner);
            break;
        }
        case Node::Kind::Group:
            redirections(node, scope);
            evaluate(node->children.value(0), scope);
            break;
        case Node::Kind::For: evaluateFor(node, scope); break;
        case Node::Kind::If: evaluateIf(node, scope); break;
        case Node::Kind::While:
        case Node::Kind::Until:
            redirections(node, scope);
            loop(scope, [&](Scope &state) {
                evaluate(node->children.value(0), state);
                // The loop ends after its condition.
                const Scope exit = state;
                evaluate(node->children.value(1), state);
                return exit;
            });
            break;
        case Node::Kind::Test: redirections(node, scope); break;
        case Node::Kind::Unsupported: fail(node->reason); break;
        }
    }

private:
    int iterations_ = 0;
    int steps_ = 0;
    int scriptDepth_ = 0;

    void fail(const QString &reason)
    {
        if (!result.judged) return;
        result.judged = false;
        result.reason = reason;
    }

    void emitUse(CommandUse use)
    {
        if (result.uses.size() >= kMaximumUses) {
            fail("too many commands");
            return;
        }
        use.environment.removeDuplicates();
        use.environment.sort();
        result.uses.append(use);
    }

    void evaluateList(const NodePtr &node, Scope &scope)
    {
        // The states before the commands of a chain joined by &&: a later command of the chain may not run,
        // and a cd may fail, so what follows the chain may run in any of them.
        QList<Scope> chain;
        // The first command of the chain after one that can fail; those before it always run.
        int skippable = -1;
        for (int i = 0; i < node->children.size(); ++i) {
            const NodePtr &child = node->children.at(i);
            const QString separator = node->separators.value(i);
            if (separator == "&") {
                fail("a command that runs in the background");
                Scope inner = scope;
                evaluate(child, inner);
                continue;
            }
            chain.append(scope);
            evaluate(child, scope);
            if (skippable < 0 && !alwaysSucceeds(child)) skippable = int(chain.size());
            if (separator == "&&") continue;
            // After "|| exit" the shell goes on only when the chain succeeded.
            if (!(separator == "||" && leavesShell(node->children.value(i + 1)))) {
                scope.mergeDirectories(chain.first());
                for (int k = qMax(1, skippable); skippable >= 0 && k < chain.size(); ++k) scope.merge(chain.at(k));
            }
            chain.clear();
            skippable = -1;
        }
    }

    static bool runsCommand(const QList<WordPart> &parts)
    {
        for (const WordPart &part : parts)
            if (part.command || part.kind == WordPart::Kind::Opaque || runsCommand(part.parts)) return true;
        return false;
    }

    // Plain assignments, whose status is always success.
    static bool alwaysSucceeds(const NodePtr &node)
    {
        if (!node || node->kind != Node::Kind::Simple || !node->words.isEmpty() || !node->redirections.isEmpty()) return false;
        for (const Assignment &assignment : node->assignments)
            if (runsCommand(assignment.value.parts)) return false;
        return true;
    }

    static bool leavesShell(const NodePtr &node)
    {
        if (!node || node->kind != Node::Kind::Simple || node->words.isEmpty() || !node->assignments.isEmpty()) return false;
        const Word &first = node->words.first();
        return first.isLiteral() && (first.literalText() == "exit" || first.literalText() == "return");
    }

    void evaluateIf(const NodePtr &node, Scope &scope)
    {
        redirections(node, scope);
        // The conditions run one after another; each body runs or not, and so does the else part.
        Scope after;
        bool any = false;
        const auto add = [&](const Scope &state) {
            if (any) after.merge(state);
            else after = state;
            any = true;
        };
        int i = 0;
        for (; i + 1 < node->children.size(); i += 2) {
            evaluate(node->children.at(i), scope);
            Scope branch = scope;
            evaluate(node->children.at(i + 1), branch);
            add(branch);
        }
        if (i < node->children.size()) {
            Scope branch = scope;
            evaluate(node->children.at(i), branch);
            add(branch);
        } else {
            add(scope);
        }
        scope = after;
    }

    void evaluateFor(const NodePtr &node, Scope &scope)
    {
        redirections(node, scope);
        QStringList values;
        bool known = node->forHasList;
        bool globs = false;
        for (const Word &word : node->words) {
            const Argument value = expandWord(word, scope);
            if (!value.known) known = false;
            if (hasGlob(value.text)) globs = true;
            values.append(value.text);
        }
        if (known && iterations_ + values.size() <= kMaximumIterations) {
            iterations_ += int(values.size());
            if (!globs) {
                for (const QString &value : std::as_const(values)) {
                    scope.set(node->variable, value);
                    evaluate(node->children.value(0), scope);
                }
                return;
            }
            // A pattern stands for each file it matches, so its round runs any number of times. In quotes the
            // variable is the pattern, as if the command had the pattern as its argument.
            loop(scope, [&](Scope &state) {
                const Scope exit = state;
                for (const QString &value : std::as_const(values)) {
                    state.set(node->variable, value);
                    evaluate(node->children.value(0), state);
                }
                return exit;
            });
            return;
        }
        loop(scope, [&](Scope &state) {
            state.forget(node->variable);
            // The loop may end before any round.
            const Scope exit = state;
            evaluate(node->children.value(0), state);
            return exit;
        });
    }

    // Evaluates a body that runs an unknown number of times: from a state that holds only what no round
    // changes. The body returns the state in which the loop can end.
    template <typename Body>
    void loop(Scope &scope, Body body)
    {
        Scope start = scope;
        for (int pass = 0; pass < kMaximumLoopPasses; ++pass) {
            const qsizetype mark = result.uses.size();
            Scope state = start;
            Scope exit = body(state);
            const Scope next = widened(start, state);
            if (next == start) {
                exit.merge(state);
                scope = exit;
                return;
            }
            // The commands of this pass were evaluated in a state that later rounds do not have.
            if (pass + 1 < kMaximumLoopPasses) result.uses.erase(result.uses.begin() + mark, result.uses.end());
            start = next;
        }
        fail("a loop that changes too much to follow");
        scope = start;
        scope.forgetDirectory();
    }

    void expandParts(const QList<WordPart> &parts, bool quoted, Scope &scope, Expansion &out)
    {
        for (const WordPart &part : parts) {
            switch (part.kind) {
            case WordPart::Kind::Literal:
            case WordPart::Kind::SingleQuoted: out.text += part.text; break;
            case WordPart::Kind::DoubleQuoted: expandParts(part.parts, true, scope, out); break;
            case WordPart::Kind::Variable: {
                if (part.text.contains('[')) {
                    // An index is evaluated as arithmetic, which can run a substitution.
                    if (part.text.contains('$') || part.text.contains('`')) fail("an expansion inside an index");
                    out.known = false;
                    break;
                }
                const auto found = scope.variables.constFind(part.text);
                if (found == scope.variables.constEnd()) {
                    out.known = false;
                    break;
                }
                // Without quotes the shell splits the value into words and matches it against file names.
                static const QRegularExpression unsafe("[\\s*?\\[]");
                if (!quoted && (found->isEmpty() || found->contains(unsafe) || scope.assigned.contains("IFS"))) out.known = false;
                else out.text += *found;
                break;
            }
            case WordPart::Kind::CommandSubstitution: {
                Scope inner = scope;
                evaluate(part.command, inner);
                out.known = false;
                break;
            }
            case WordPart::Kind::Tilde:
                if (part.text.isEmpty() && !scope.home().isEmpty()) out.text += scope.home();
                else out.known = false;
                break;
            case WordPart::Kind::Opaque:
                opaque(part, scope);
                out.known = false;
                break;
            }
        }
    }

    // A part whose value is not followed may still run a command or set a variable.
    void opaque(const WordPart &part, Scope &scope)
    {
        if (part.command) {
            Scope inner = scope;
            evaluate(part.command, inner);
            return;
        }
        const QString &text = part.text;
        if (text.startsWith("$((")) {
            const QString inner = text.mid(3, text.size() - 5);
            static const QRegularExpression assigns("(^|[^=!<>])=($|[^=])|\\+\\+|--");
            if (inner.contains("$(") || inner.contains('`')) fail("a command substitution inside arithmetic");
            else if (inner.contains(assigns)) fail("arithmetic that sets a variable");
        } else if (text.startsWith("${")) {
            const QString inner = text.mid(2, text.size() - 3);
            static const QRegularExpression assigns("^[A-Za-z_][A-Za-z0-9_]*(\\[[^\\]]*\\])?:?=");
            if (inner.contains("$(") || inner.contains('`')) fail("a command substitution inside " + text);
            else if (inner.contains(assigns)) fail("an expansion that sets a variable");
        }
    }

    // {a,b} and {1..3} become several words.
    static bool hasBraceExpansion(const Word &word)
    {
        QString unquoted;
        for (const WordPart &part : word.parts) unquoted += part.kind == WordPart::Kind::Literal ? part.text : QString("\x01");
        static const QRegularExpression braces("\\{[^{}]*(,|\\.\\.)[^{}]*\\}");
        return unquoted.contains(braces);
    }

    // A word of a command: its text when every part is known, and the word as written otherwise.
    Argument expandWord(const Word &word, Scope &scope)
    {
        // <(command) alone is the name of a pipe to that command.
        if (word.parts.size() == 1 && word.parts.first().kind == WordPart::Kind::Opaque && word.parts.first().command) {
            opaque(word.parts.first(), scope);
            return {"/dev/fd/63", true};
        }
        Expansion expansion;
        expandParts(word.parts, false, scope, expansion);
        if (hasBraceExpansion(word)) expansion.known = false;
        return {expansion.known ? expansion.text : word.source, expansion.known};
    }

    // The value of NAME=value, where ~ at the start is the home directory.
    Argument expandValue(const Word &word, Scope &scope)
    {
        Expansion expansion;
        QList<WordPart> parts = word.parts;
        for (const WordPart &part : std::as_const(parts))
            if (part.kind == WordPart::Kind::Literal && part.text.contains(":~")) expansion.known = false;
        if (!parts.isEmpty() && parts.first().kind == WordPart::Kind::Literal && parts.first().text.startsWith('~')) {
            const QString first = parts.first().text;
            if (!scope.home().isEmpty() && (first.startsWith("~/") || (first == "~" && parts.size() == 1))) {
                expansion.text = scope.home();
                parts.first().text = first.mid(1);
            } else {
                expansion.known = false;
            }
        }
        expandParts(parts, false, scope, expansion);
        return {expansion.known ? expansion.text : word.source, expansion.known};
    }

    static bool harmlessDevice(const QString &path)
    {
        static const QSet<QString> devices{"/dev/null", "/dev/stdout", "/dev/stderr", "/dev/stdin", "/dev/tty"};
        return devices.contains(path) || path.startsWith("/dev/fd/");
    }

    // Adds a path a command names, for each directory the command may run in.
    static void addPath(QStringList &list, const QString &raw, CommandUse &use)
    {
        if (raw.isEmpty()) return;
        if (raw.startsWith('/')) {
            const QString path = QDir::cleanPath(raw);
            if (!harmlessDevice(path)) appendUnique(list, path);
            return;
        }
        if (use.directories.isEmpty()) {
            appendUnique(use.problems, "the directory of " + raw + " is not known");
            return;
        }
        for (const QString &directory : std::as_const(use.directories)) appendUnique(list, QDir::cleanPath(directory + '/' + raw));
    }

    void applyRedirections(const NodePtr &node, Scope &scope, CommandUse &use)
    {
        CommandUse redirected;
        redirected.directories = use.directories;
        collectRedirections(node, scope, redirected);
        for (const QString &path : std::as_const(redirected.reads)) appendUnique(use.reads, path);
        for (const QString &path : std::as_const(redirected.writes)) appendUnique(use.writes, path);
        for (const QString &problem : std::as_const(redirected.problems)) appendUnique(use.problems, problem);
        use.redirectionWrites = redirected.writes;
        use.redirectionProblems = redirected.problems;
    }

    void collectRedirections(const NodePtr &node, Scope &scope, CommandUse &use)
    {
        using Kind = Redirection::Kind;
        for (const Redirection &redirection : node->redirections) {
            if (redirection.kind == Kind::HereDocument) {
                // Its body may hold substitutions.
                Expansion body;
                expandParts(redirection.hereParts, true, scope, body);
                continue;
            }
            const Argument target = expandWord(redirection.target, scope);
            if (redirection.kind == Kind::HereString) continue;
            const bool duplicate = redirection.kind == Kind::DuplicateOutput || redirection.kind == Kind::DuplicateInput;
            static const QRegularExpression descriptor("^([0-9]+-?|-)$");
            if (duplicate && target.known && descriptor.match(target.text).hasMatch()) continue;
            if (!target.known || hasGlob(target.text)) {
                appendUnique(use.problems, "the target of a redirection is not known: " + redirection.target.source);
                continue;
            }
            if (target.text.startsWith("/dev/tcp/") || target.text.startsWith("/dev/udp/")) {
                appendUnique(use.problems, "a redirection that opens a network connection");
                continue;
            }
            const bool input = redirection.kind == Kind::Input || redirection.kind == Kind::DuplicateInput;
            addPath(input ? use.reads : use.writes, target.text, use);
        }
    }

    // The redirections of a compound command, as an entry of their own.
    void redirections(const NodePtr &node, Scope &scope)
    {
        if (node->redirections.isEmpty()) return;
        CommandUse use;
        use.effect = Effect::None;
        use.directories = scope.directories;
        QStringList text;
        for (const Redirection &redirection : node->redirections) {
            const bool input = redirection.kind == Redirection::Kind::Input || redirection.kind == Redirection::Kind::DuplicateInput
                || redirection.kind == Redirection::Kind::HereDocument || redirection.kind == Redirection::Kind::HereString;
            text.append((input ? "< " : "> ") + redirection.target.source);
        }
        use.text = text.join(' ');
        applyRedirections(node, scope, use);
        if (!use.reads.isEmpty() || !use.writes.isEmpty() || !use.problems.isEmpty()) emitUse(use);
    }

    void evaluateSimple(const NodePtr &node, Scope &scope)
    {
        if (node->words.isEmpty()) {
            for (const Assignment &assignment : node->assignments) {
                const Argument value = expandValue(assignment.value, scope);
                if (value.known && !assignment.append) scope.set(assignment.name, value.text);
                else scope.forget(assignment.name);
            }
            // Redirections alone still create or empty their files.
            CommandUse use;
            use.effect = Effect::None;
            use.text = node->source;
            use.directories = scope.directories;
            applyRedirections(node, scope, use);
            if (!use.reads.isEmpty() || !use.writes.isEmpty() || !use.problems.isEmpty()) emitUse(use);
            return;
        }
        CommandUse use;
        QStringList text;
        // The variables set for this command only; a script it runs sees them.
        Scope local = scope;
        for (const Assignment &assignment : node->assignments) {
            const Argument value = expandValue(assignment.value, local);
            if (value.known && !assignment.append) local.set(assignment.name, value.text);
            else local.forget(assignment.name);
            text.append(assignment.name + (assignment.append ? "+=" : "=") + assignment.value.source);
        }
        QList<Argument> arguments;
        for (const Word &word : node->words) {
            arguments.append(expandWord(word, scope));
            text.append(word.source);
        }
        use.text = text.join(' ');
        use.directories = scope.directories;
        use.environment = local.assigned.values();
        const bool own = resolve(arguments, use, scope, local);
        applyRedirections(node, scope, use);
        if (own || !use.reads.isEmpty() || !use.writes.isEmpty() || !use.problems.isEmpty()) emitUse(use);
    }

    static QStringList texts(const QList<Argument> &arguments)
    {
        QStringList result;
        for (const Argument &argument : arguments) result.append(argument.text);
        return result;
    }

    static void unknown(CommandUse &use, const QString &problem)
    {
        use.effect = Effect::Unknown;
        appendUnique(use.problems, problem);
    }

    void changeDirectory(Scope &scope, const QString &path) const
    {
        if (path.startsWith('/')) {
            scope.setDirectories({QDir::cleanPath(path)});
            return;
        }
        if (!scope.directoryKnown) return;
        QStringList directories;
        for (const QString &directory : std::as_const(scope.directories)) directories.append(QDir::cleanPath(directory + '/' + path));
        scope.setDirectories(directories);
    }

    // Finds what a command is, through the wrappers that run another command, and fills in its entry. False
    // when the command was a shell -c script whose commands have entries of their own.
    bool resolve(QList<Argument> arguments, CommandUse &use, Scope &scope, const Scope &local)
    {
        static const QStringList systemDirectories{"/bin", "/usr/bin", "/usr/local/bin", "/sbin", "/usr/sbin", "/opt/homebrew/bin"};
        for (int wrappers = 0; wrappers < 16; ++wrappers) {
            use.words = texts(arguments);
            use.program.clear();
            if (arguments.isEmpty()) {
                use.effect = Effect::None;
                return true;
            }
            const Argument head = arguments.first();
            if (!head.known || head.text.isEmpty() || (hasGlob(head.text) && head.text != "[")) {
                unknown(use, "the program is not known");
                return true;
            }
            QString name = head.text;
            const qsizetype slash = name.lastIndexOf('/');
            if (slash >= 0) {
                if (!systemDirectories.contains(name.left(slash))) {
                    // A program given by its path, such as a test of the project.
                    use.effect = Effect::Execute;
                    use.program = name.mid(slash + 1);
                    addPath(use.executes, name, use);
                    return true;
                }
                name = name.mid(slash + 1);
            }
            use.program = name;
            const QList<Argument> rest = arguments.mid(1);
            const auto option = [&rest](int index) { return index < rest.size() && rest.at(index).known ? rest.at(index).text : QString(); };

            if (name == "env") {
                int i = 0;
                for (; i < rest.size(); ++i) {
                    const QString text = rest.at(i).text;
                    const QRegularExpressionMatch match = assignmentPattern().match(text);
                    if (match.hasMatch()) {
                        use.environment.append(match.captured(1));
                    } else if (!rest.at(i).known || !text.startsWith('-')) {
                        break;
                    } else if (text == "-u" || text == "--unset") {
                        ++i;
                    } else if (text == "--") {
                        ++i;
                        break;
                    } else if (text != "-i" && text != "-" && text != "--ignore-environment" && !text.startsWith("--unset=")) {
                        unknown(use, "env with " + text + " cannot be judged");
                        return true;
                    }
                }
                arguments = rest.mid(i);
                if (arguments.isEmpty()) {
                    // Prints the environment.
                    use.effect = Effect::Read;
                    return true;
                }
                continue;
            }
            if (name == "command") {
                if (option(0) == "-v" || option(0) == "-V") {
                    use.effect = Effect::None;
                    return true;
                }
                int i = 0;
                while (option(i) == "-p" || option(i) == "--") ++i;
                arguments = rest.mid(i);
                continue;
            }
            if (name == "nohup") {
                arguments = rest;
                continue;
            }
            if (name == "time") {
                arguments = rest.mid(option(0) == "-p" ? 1 : 0);
                continue;
            }
            if (name == "nice") {
                static const QRegularExpression level("^-[0-9]+$");
                int i = 0;
                if (option(0) == "-n") i = 2;
                else if (option(0).startsWith("--adjustment=") || level.match(option(0)).hasMatch()) i = 1;
                arguments = rest.mid(i);
                continue;
            }
            if (name == "timeout") {
                int i = 0;
                while (option(i).startsWith('-')) {
                    const QString text = option(i);
                    if (text == "-s" || text == "--signal" || text == "-k" || text == "--kill-after") {
                        i += 2;
                    } else if (text.startsWith("-s") || text.startsWith("-k") || text.startsWith("--signal=")
                               || text.startsWith("--kill-after=") || text == "--preserve-status" || text == "--foreground"
                               || text == "-v" || text == "--verbose") {
                        ++i;
                    } else {
                        unknown(use, "timeout with " + text + " cannot be judged");
                        return true;
                    }
                }
                // The duration.
                arguments = rest.mid(i + 1);
                if (arguments.isEmpty()) {
                    unknown(use, "timeout without a command");
                    return true;
                }
                continue;
            }
            if (name == "stdbuf") {
                int i = 0;
                while (option(i).startsWith('-')) {
                    const QString text = option(i);
                    if (text == "-i" || text == "-o" || text == "-e") {
                        i += 2;
                    } else if (text.startsWith("-i") || text.startsWith("-o") || text.startsWith("-e") || text.startsWith("--input=")
                               || text.startsWith("--output=") || text.startsWith("--error=")) {
                        ++i;
                    } else {
                        unknown(use, "stdbuf with " + text + " cannot be judged");
                        return true;
                    }
                }
                arguments = rest.mid(i);
                continue;
            }
            if (name == "exec") {
                if (rest.isEmpty()) {
                    // Only its redirections matter.
                    use.effect = Effect::None;
                    return true;
                }
                if (!rest.first().known || rest.first().text.startsWith('-')) {
                    unknown(use, "exec with options cannot be judged");
                    return true;
                }
                arguments = rest;
                continue;
            }
            if (name == "xargs") {
                static const QStringList plain{"-0", "-r", "-t", "-x", "--null", "--no-run-if-empty", "--verbose", "--exit"};
                static const QStringList valued{"-n", "-P", "-L", "-s", "-d", "-E", "--max-args", "--max-procs", "--max-lines",
                                                "--max-chars", "--delimiter"};
                static const QRegularExpression attached("^(-[nPLs][0-9]+|--(max-args|max-procs|max-lines|max-chars|delimiter)=.*)$");
                QString replace;
                int i = 0;
                while (i < rest.size() && rest.at(i).known && rest.at(i).text.startsWith('-')) {
                    const QString text = rest.at(i).text;
                    if (text == "--") {
                        ++i;
                        break;
                    }
                    if (plain.contains(text) || attached.match(text).hasMatch()) {
                        ++i;
                    } else if (valued.contains(text)) {
                        i += 2;
                    } else if (text == "-I" && i + 1 < rest.size() && rest.at(i + 1).known) {
                        replace = rest.at(i + 1).text;
                        i += 2;
                    } else if (text.startsWith("-I") && text.size() > 2) {
                        replace = text.mid(2);
                        ++i;
                    } else if (text == "-i") {
                        replace = "{}";
                        ++i;
                    } else {
                        unknown(use, "xargs with " + text + " cannot be judged");
                        return true;
                    }
                }
                arguments = rest.mid(i);
                if (arguments.isEmpty()) {
                    // Prints its input.
                    use.effect = Effect::None;
                    return true;
                }
                // The arguments that come from the input are not known.
                if (replace.isEmpty()) {
                    use.inputArguments = true;
                } else {
                    for (Argument &argument : arguments)
                        if (argument.text.contains(replace)) argument.known = false;
                }
                continue;
            }
            if (QStringList{"bash", "sh", "dash", "zsh", "ksh"}.contains(name)) {
                static const QRegularExpression letters("^-[celux]+$");
                bool script = false;
                int i = 0;
                for (; i < rest.size() && rest.at(i).known; ++i) {
                    const QString text = rest.at(i).text;
                    if (text == "-o") {
                        ++i;
                    } else if (letters.match(text).hasMatch()) {
                        if (!text.contains('c')) continue;
                        script = true;
                        ++i;
                        break;
                    } else if (text != "--login" && text != "--norc" && text != "--noprofile" && text != "--posix") {
                        break;
                    }
                }
                if (!script || i >= rest.size() || !rest.at(i).known) {
                    unknown(use, "a script that is not given as text");
                    return true;
                }
                if (scriptDepth_ >= kMaximumScriptDepth) {
                    fail("scripts nested too deeply");
                    unknown(use, "scripts nested too deeply");
                    return true;
                }
                Scope inner = local;
                inner.directoryKnown = scope.directoryKnown;
                inner.directories = scope.directories;
                for (const QString &variable : std::as_const(use.environment)) inner.assigned.insert(variable);
                ++scriptDepth_;
                evaluate(parseBash(rest.at(i).text), inner);
                --scriptDepth_;
                use.effect = Effect::None;
                return false;
            }
            classify(name, rest, use, scope);
            // What only describes files may take any of them; what shows them could show a secret.
            if (use.inputArguments && !describesWithoutContents(name)) {
                const bool reads = use.effect == Effect::Read || use.effect == Effect::GitRead;
                appendUnique(use.problems, reads ? QString("the files come from the input, so a secret file could be shown")
                                                 : QString("the arguments come from the input"));
            }
            return true;
        }
        unknown(use, "too many wrappers");
        return true;
    }

    // The commands that change what the line knows, and then the catalog.
    void classify(const QString &name, const QList<Argument> &rest, CommandUse &use, Scope &scope)
    {
        if (name == "sudo" || name == "doas" || name == "su") {
            unknown(use, "privilege escalation");
            return;
        }
        if (name == "eval" || name == "source" || name == ".") {
            unknown(use, name + " runs text that cannot be judged");
            return;
        }
        if (name == "break" || name == "continue") {
            // The state after a loop that ends early is not followed.
            fail(name + " in a loop");
            use.effect = Effect::None;
            return;
        }
        if (name == "cd" || name == "pushd") {
            use.effect = Effect::None;
            QList<Argument> operands;
            for (const Argument &argument : rest)
                if (!argument.known || !QStringList{"-L", "-P", "-e", "-@", "--"}.contains(argument.text)) operands.append(argument);
            static const QRegularExpression stackEntry("^[+-][0-9]*$");
            if (operands.isEmpty()) {
                if (name == "cd" && scope.home().startsWith('/')) scope.setDirectories({QDir::cleanPath(scope.home())});
                else scope.forgetDirectory();
            } else if (operands.size() > 1 || !operands.first().known || operands.first().text.isEmpty()
                       || hasGlob(operands.first().text) || stackEntry.match(operands.first().text).hasMatch()
                       || (scope.assigned.contains("CDPATH") && !operands.first().text.startsWith('/')
                           && !operands.first().text.startsWith('.'))) {
                // Also "cd -", which returns to a directory that is not followed, and a search along CDPATH.
                scope.forgetDirectory();
            } else {
                changeDirectory(scope, operands.first().text);
            }
            return;
        }
        if (name == "popd") {
            use.effect = Effect::None;
            scope.forgetDirectory();
            return;
        }
        if (name == "export") {
            use.effect = Effect::None;
            for (const Argument &argument : rest) {
                const QRegularExpressionMatch match = assignmentPattern().match(argument.text);
                if (match.hasMatch()) {
                    QString value = argument.text.mid(match.capturedLength(0));
                    // As in an assignment, ~ at the start of the value is the home directory.
                    const bool home = value == "~" || value.startsWith("~/");
                    if (!argument.known || !match.captured(2).isEmpty() || value.contains(":~") || (value.startsWith('~') && !home)
                        || (home && scope.home().isEmpty())) {
                        scope.forget(match.captured(1));
                    } else {
                        scope.set(match.captured(1), home ? scope.home() + value.mid(1) : value);
                    }
                } else if (!argument.known || (argument.text.startsWith('-') && argument.text != "-p" && argument.text != "--")) {
                    unknown(use, "export with " + argument.text + " cannot be judged");
                }
            }
            return;
        }
        if (name == "unset" || name == "read") {
            use.effect = Effect::None;
            if (name == "read") scope.forget("REPLY");
            for (const Argument &argument : rest) {
                if (!argument.known) unknown(use, "the value of " + argument.text + " is not known");
                else if (isName(argument.text)) scope.forget(argument.text);
            }
            return;
        }
        if (name == "printf" && !rest.isEmpty() && rest.first().known && rest.first().text == "-v") {
            if (rest.size() > 1 && rest.at(1).known && isName(rest.at(1).text)) {
                scope.forget(rest.at(1).text);
            } else {
                unknown(use, "printf -v with a variable that is not known");
                return;
            }
        }
        const Classification classification = classifyProgram(name, rest);
        use.effect = classification.effect;
        use.subcommand = classification.subcommand;
        for (const QString &problem : classification.problems) appendUnique(use.problems, problem);
        for (const QString &path : classification.reads) addPath(use.reads, path, use);
        for (const QString &file : classification.homeReads) {
            if (scope.home().startsWith('/')) addPath(use.reads, scope.home() + '/' + file, use);
            else appendUnique(use.problems, "the home directory is not known");
        }
        for (const QString &path : classification.writes) addPath(use.writes, path, use);
        for (const QString &path : classification.executes) addPath(use.executes, path, use);
        use.urls = classification.urls;
    }
};

}

Evaluation evaluate(const NodePtr &tree, const Environment &environment)
{
    Evaluator evaluator;
    Scope scope;
    scope.variables = environment.variables;
    if (!environment.home.isEmpty() && !scope.variables.contains("HOME")) scope.variables.insert("HOME", environment.home);
    if (environment.directory.startsWith('/')) scope.setDirectories({QDir::cleanPath(environment.directory)});
    evaluator.evaluate(tree, scope);
    return evaluator.result;
}

QString dumpEvaluation(const Evaluation &evaluation)
{
    QStringList lines;
    if (!evaluation.judged) lines.append("cannot judge: " + evaluation.reason);
    for (const CommandUse &use : evaluation.uses) {
        QStringList parts{use.words.isEmpty() ? use.text : use.words.join(' '), effectName(use.effect),
                          "in " + (use.directories.isEmpty() ? QString("?") : use.directories.join(' '))};
        if (!use.reads.isEmpty()) parts.append("reads " + use.reads.join(' '));
        if (!use.writes.isEmpty()) parts.append("writes " + use.writes.join(' '));
        if (!use.executes.isEmpty()) parts.append("runs " + use.executes.join(' '));
        if (!use.environment.isEmpty()) parts.append("env " + use.environment.join(' '));
        if (use.inputArguments) parts.append("arguments from input");
        if (!use.problems.isEmpty()) parts.append("problem: " + use.problems.join("; "));
        lines.append(parts.join(" | "));
    }
    return lines.join('\n');
}

}
