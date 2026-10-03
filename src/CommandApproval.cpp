#include "CommandApproval.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <functional>
#include <optional>

namespace {
struct ShellWords {
    QList<QStringList> commands;
    QStringList substitutions;
    bool simple = true;
    bool valid = true;
};

ShellWords words(const QString &text)
{
    ShellWords result;
    QStringList command;
    QString word;
    QChar quote;
    bool started = false;
    const auto finishWord = [&] {
        if (started) command.append(word);
        word.clear();
        started = false;
    };
    const auto finishCommand = [&] {
        finishWord();
        if (!command.isEmpty()) result.commands.append(command);
        command.clear();
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quote != '\'' && (c == '`' || (c == '$' && i + 1 < text.size() && text.at(i + 1) == '('))) {
            result.simple = false;
            const bool backtick = c == '`';
            const qsizetype begin = i + (backtick ? 1 : 2);
            qsizetype end = begin;
            int balance = 1;
            QChar innerQuote;
            for (; end < text.size(); ++end) {
                const QChar inner = text.at(end);
                if (inner == '\\' && innerQuote != '\'') { ++end; continue; }
                if (!innerQuote.isNull()) {
                    if (inner == innerQuote) innerQuote = QChar();
                    continue;
                }
                if (inner == '\'' || inner == '"') { innerQuote = inner; continue; }
                if (backtick && inner == '`') break;
                if (!backtick && inner == '(') ++balance;
                if (!backtick && inner == ')' && --balance == 0) break;
            }
            if (end >= text.size()) result.valid = false;
            else result.substitutions.append(text.mid(begin, end - begin));
            word += text.mid(i, end - i + 1);
            started = true;
            i = end;
        } else if (c == '\\' && quote != '\'') {
            if (++i == text.size()) { result.valid = false; break; }
            // Escapes in double quotes only remove the backslash for these characters.
            if (quote == '"' && !QString("$`\"\\\n").contains(text.at(i))) word += '\\';
            if (text.at(i) != '\n') { word += text.at(i); started = true; }
        } else if (!quote.isNull()) {
            if (c == quote) quote = QChar();
            else {
                word += c;
                if (quote == '"' && (c == '$' || c == '`')) result.simple = false;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
            started = true;
        } else if (QString(";&|\n()").contains(c)) {
            result.simple = false;
            finishCommand();
        } else if (QString("<>$`*?[]{}~").contains(c)) {
            result.simple = false;
            word += c;
            started = true;
        } else if (c == '#' && !started) {
            result.simple = false;
            while (i < text.size() && text.at(i) != '\n') ++i;
            finishCommand();
        } else if (c.isSpace()) finishWord();
        else { word += c; started = true; }
    }
    finishCommand();
    if (!quote.isNull()) result.valid = false;
    return result;
}

QString executable(QString value)
{
    value = QFileInfo(value).fileName();
#ifdef Q_OS_WIN
    value = value.toLower();
    if (value.endsWith(".exe")) value.chop(4);
#endif
    return value;
}

CommandApproval classify(const QString &text, int depth)
{
    if (depth > 8) return {};
    const ShellWords parsed = words(text);
    CommandApproval result;
    for (const QString &substitution : parsed.substitutions) {
        const CommandApproval nested = classify(substitution, depth + 1);
        if (!nested.deniedReason.isEmpty()) return nested;
    }
    for (QStringList args : parsed.commands) {
        bool plain = parsed.simple && parsed.valid && parsed.commands.size() == 1;
        while (!args.isEmpty() && QRegularExpression("^[A-Za-z_][A-Za-z_0-9]*=").match(args.first()).hasMatch()) {
            args.removeFirst();
            plain = false;
        }
        if (args.isEmpty()) continue;
        QString program = executable(args.first());
        while (QStringList{"command", "exec", "env", "nohup"}.contains(program)) {
            if (program == "command" && (args.value(1) == "-v" || args.value(1) == "-V")) {
                args.clear();
                break;
            }
            args.removeFirst();
            while (!args.isEmpty() && (args.first().startsWith('-') || args.first().contains('='))) args.removeFirst();
            if (args.isEmpty()) break;
            program = executable(args.first());
            plain = false;
        }
        if (args.isEmpty()) continue;
        if (QStringList{"sh", "bash", "dash", "zsh", "ksh"}.contains(program)) {
            for (int i = 1; i + 1 < args.size(); ++i) {
                if (args.at(i) != "-c" && args.at(i) != "-lc") continue;
                const CommandApproval nested = classify(args.at(i + 1), depth + 1);
                if (!nested.deniedReason.isEmpty()) return nested;
                if (plain && i == 1 && args.size() == 3
                    && (args.first() == program || args.first() == "/bin/" + program
                        || args.first() == "/usr/bin/" + program)) result.sessionRule = nested.sessionRule;
                break;
            }
            continue;
        }
        if (QStringList{"sudo", "doas", "su"}.contains(program))
            return {{}, "Privilege escalation (" + program + ")"};
        if (program == "git") {
            int i = 1;
            while (i < args.size() && args.at(i).startsWith('-')) {
                const QString option = args.at(i++);
                if (QStringList{"-C", "--git-dir", "--work-tree", "-c", "--config-env"}.contains(option)) {
                    if (option == "-c" || option == "--config-env") plain = false;
                    ++i;
                } else if (option.startsWith("--git-dir=") || option.startsWith("--work-tree=")) {
                    // These options do not change the subcommand.
                } else {
                    plain = false;
                }
            }
            const QString subcommand = args.value(i);
            if (subcommand == "push") return {{}, "git push"};
            if (plain && args.first() == "git" && (subcommand == "add" || subcommand == "commit"))
                result.sessionRule = "git " + subcommand;
            continue;
        }
#ifdef Q_OS_WIN
        if (QStringList{"cmd", "powershell", "pwsh"}.contains(program)) {
            for (int i = 1; i + 1 < args.size(); ++i) {
                if (args.at(i).toLower() != "/c" && args.at(i).toLower() != "-command") continue;
                QString script;
                if (args.size() == i + 2) script = args.at(i + 1);
                else {
                    QStringList quoted;
                    for (QString arg : args.mid(i + 1)) {
                        arg.replace("'", "'\\''");
                        quoted.append("'" + arg + "'");
                    }
                    script = quoted.join(' ');
                }
                const CommandApproval nested = classify(script, depth + 1);
                if (!nested.deniedReason.isEmpty()) return nested;
                break;
            }
            continue;
        }
        if (program == "runas") return {{}, "Privilege escalation (runas)"};
        if (QStringList{"winget", "choco", "scoop"}.contains(program)
            && QStringList{"install", "upgrade", "uninstall", "update"}.contains(args.value(1).toLower()))
            return {{}, "System package changes (" + program + ")"};
        if (QStringList{"install-package", "install-module", "install-script"}.contains(program))
            return {{}, "System package changes (" + program + ")"};
#endif
    }
    return result;
}
}

CommandApproval classifyCommandApproval(const QString &command)
{
    return classify(command, 0);
}

namespace {
QList<CommandRule> &ruleList()
{
    static QList<CommandRule> rules = defaultCommandRules();
    return rules;
}

std::function<void()> &rulesChangedHandler()
{
    static std::function<void()> handler;
    return handler;
}

// Only words, quotes and the separators of a command list; anything that could run or write more is refused.
bool plainCommandList(const QString &text)
{
    QChar quote;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == '\\' && quote != '\'') {
            ++i;
            continue;
        }
        if (!quote.isNull()) {
            if (c == quote) quote = QChar();
            else if (quote == '"' && (c == '$' || c == '`')) return false;
            continue;
        }
        if (c == '\'' || c == '"') quote = c;
        else if (QString("<>()$`").contains(c)) return false;
        else if (c == '&') {
            if (i + 1 < text.size() && text.at(i + 1) == '&') ++i;
            else return false;
        }
    }
    return quote.isNull();
}

// A message given as "$(cat <<'EOF' ... EOF)", as Claude Code writes commits, is plain text: a quoted
// delimiter turns off expansion in the here-document, which ends at the first line that is exactly the
// delimiter. Such a substitution becomes a plain word; any other form is left for the checks to refuse.
QString withoutLiteralMessages(QString text)
{
    static const QRegularExpression start(R"(\$\(cat <<(['"])(\w+)\1\n)");
    qsizetype from = 0;
    for (QRegularExpressionMatch match = start.match(text, from); match.hasMatch(); match = start.match(text, from)) {
        const QString delimiter = match.captured(2);
        qsizetype line = match.capturedEnd();
        qsizetype end = -1;
        while (line <= text.size()) {
            qsizetype next = text.indexOf('\n', line);
            if (next < 0) next = text.size();
            if (text.mid(line, next - line) == delimiter) {
                end = next;
                break;
            }
            line = next + 1;
        }
        if (end < 0) return text;
        qsizetype close = end;
        while (close < text.size() && text.at(close).isSpace()) ++close;
        if (close >= text.size() || text.at(close) != ')') return text;
        text.replace(match.capturedStart(), close + 1 - match.capturedStart(), "message");
        from = match.capturedStart() + 7;
    }
    return text;
}

// Git's options that only choose the repository are dropped, so that rules see the subcommand.
QStringList withoutGitDirectory(QStringList args)
{
    if (args.value(0) != "git") return args;
    int i = 1;
    while (i < args.size()) {
        const QString option = args.at(i);
        if (option == "-C" || option == "--git-dir" || option == "--work-tree") i += 2;
        else if (option.startsWith("--git-dir=") || option.startsWith("--work-tree=")) ++i;
        else break;
    }
    return QStringList{"git"} + args.mid(qMin(i, int(args.size())));
}

bool isShellWrapper(const ShellWords &parsed)
{
    const QStringList &only = parsed.commands.value(0);
    return parsed.commands.size() == 1 && only.size() == 3 && (only.at(1) == "-c" || only.at(1) == "-lc")
        && QStringList{"sh", "bash", "zsh", "/bin/sh", "/bin/bash", "/bin/zsh", "/usr/bin/bash", "/usr/bin/zsh"}
               .contains(only.first());
}

// The commands of a plain command line, each as its words, or nothing when the line is not plain: plain
// commands joined by &&, ||, ; or |, also inside a shell -c wrapper, with literal commit messages.
std::optional<QList<QStringList>> plainCommands(const QString &command, int depth = 0)
{
    if (depth > 4) return std::nullopt;
    const QString text = withoutLiteralMessages(command);
    if (!plainCommandList(text)) return std::nullopt;
    const ShellWords parsed = words(text);
    if (!parsed.valid || parsed.commands.isEmpty() || !parsed.substitutions.isEmpty()) return std::nullopt;
    if (isShellWrapper(parsed)) return plainCommands(parsed.commands.first().at(2), depth + 1);
    QList<QStringList> result;
    for (const QStringList &args : parsed.commands) {
        if (args.first().contains('=')) return std::nullopt;
        result.append(withoutGitDirectory(args));
    }
    return result;
}

// Every command a line may run, as far as its words can be read, including substitutions and shell -c
// scripts; for deny rules, which must also catch commands in lines that are not plain.
QList<QStringList> anyCommands(const QString &command, int depth = 0)
{
    QList<QStringList> result;
    if (depth > 8) return result;
    const ShellWords parsed = words(withoutLiteralMessages(command));
    for (const QString &substitution : parsed.substitutions) result += anyCommands(substitution, depth + 1);
    for (QStringList args : parsed.commands) {
        while (!args.isEmpty() && args.first().contains('=')) args.removeFirst();
        if (args.isEmpty()) continue;
        for (int i = 1; i + 1 < args.size(); ++i)
            if (args.at(i) == "-c" || args.at(i) == "-lc") result += anyCommands(args.at(i + 1), depth + 1);
        result.append(withoutGitDirectory(args));
    }
    return result;
}
}

QList<CommandRule> defaultCommandRules()
{
    // "git commit *-m *" also covers options before -m, such as the -q Claude Code writes.
    QList<CommandRule> rules{{"git add *", CommandDecision::Allow, true}, {"git commit *-m *", CommandDecision::Allow, true},
                             {"git status *", CommandDecision::Allow, true}, {"git log *", CommandDecision::Allow, true},
                             {"git diff *", CommandDecision::Allow, true}, {"git show *", CommandDecision::Allow, true},
                             {"rm *", CommandDecision::Ask, true}, {"rmdir *", CommandDecision::Ask, true}};
    for (const QString &pattern : fixedDeniedPatterns()) rules.append({pattern, CommandDecision::Deny, true});
    // Installing and upgrading system packages; "*" before an operation also covers its options and variants
    // such as reinstall or full-upgrade. These can be removed, unchecked or allowed; removals are fixed denials.
    for (const QString &pattern : {"apt *install *", "apt *upgrade *", "apt-get *install *", "apt-get *upgrade *",
                                   "dnf *install *", "dnf *upgrade *", "yum *install *", "yum *update *",
                                   "zypper *install *", "zypper in *", "zypper *update *", "pacman -S*", "pacman -U*",
                                   "snap install *", "snap refresh *", "flatpak install *", "flatpak update *",
                                   "brew install *", "brew upgrade *"})
        rules.append({pattern, CommandDecision::Deny, true});
    return rules;
}

QString commandDecisionName(CommandDecision decision)
{
    return decision == CommandDecision::Deny ? "Deny" : decision == CommandDecision::Ask ? "Ask" : "Allow";
}

CommandDecision commandDecisionFromName(const QString &name)
{
    return name.compare("Deny", Qt::CaseInsensitive) == 0 ? CommandDecision::Deny
        : name.compare("Ask", Qt::CaseInsensitive) == 0  ? CommandDecision::Ask
                                                          : CommandDecision::Allow;
}

namespace {
// Removing system packages can break the system, so it is always declined; installing and upgrading are
// removable Deny lines of the defaults instead.
const QStringList &packageRemovalPatterns()
{
    static const QStringList patterns{"apt *remove *", "apt *purge *", "apt-get *remove *", "apt-get *purge *",
                                      "dnf *remove *", "dnf *erase *", "yum *remove *", "yum *erase *",
                                      "zypper *remove *", "zypper rm *", "pacman -R*", "snap remove *",
                                      "flatpak uninstall *", "brew uninstall *"};
    return patterns;
}
}

QStringList fixedDeniedPatterns()
{
    return QStringList{"git push *", "sudo *", "doas *", "su *"} + packageRemovalPatterns();
}

QString fixedDenial(const QString &pattern)
{
    const QStringList words = pattern.split(' ', Qt::SkipEmptyParts);
    if (QStringList{"sudo", "doas", "su"}.contains(executable(words.value(0)))) return "privilege escalation is always declined";
    if (words.value(0) == "git" && words.contains("push")) return "git push is always declined";
    if (packageRemovalPatterns().contains(pattern.simplified())) return "removing system packages is always declined";
    return {};
}

QList<CommandRule> commandRules()
{
    return ruleList();
}

void setCommandRules(const QList<CommandRule> &rules)
{
    ruleList() = rules;
}

void setCommandRulesChangedHandler(const std::function<void()> &handler)
{
    rulesChangedHandler() = handler;
}

bool addAllowRule(const QString &pattern)
{
    if (pattern.trimmed().isEmpty() || !fixedDenial(pattern).isEmpty()) return false;
    for (CommandRule &rule : ruleList()) {
        if (rule.pattern != pattern) continue;
        if (rule.decision == CommandDecision::Allow && rule.enabled) return false;
        rule.decision = CommandDecision::Allow;
        rule.enabled = true;
        if (rulesChangedHandler()) rulesChangedHandler()();
        return true;
    }
    ruleList().append({pattern, CommandDecision::Allow, true});
    if (rulesChangedHandler()) rulesChangedHandler()();
    return true;
}

bool commandPatternMatches(const QString &pattern, const QString &command)
{
    const auto matches = [&command](const QString &glob) {
        const QRegularExpression expression(
            QRegularExpression::wildcardToRegularExpression(glob, QRegularExpression::NonPathWildcardConversion));
        return expression.isValid() && expression.match(command).hasMatch();
    };
    const QString simplified = pattern.simplified();
    if (simplified.isEmpty()) return false;
    // "git stash *" also matches "git stash" alone.
    if (simplified.endsWith(" *") && matches(simplified.chopped(2))) return true;
    return matches(simplified);
}

bool patternOverlapsPrefix(const QString &pattern, const QString &prefix)
{
    // The literal start keeps a space before a wildcard, so that "apt *" does not reach "apt-get".
    QString literal = pattern.simplified();
    static const QRegularExpression wildcard(R"([*?\[])");
    const qsizetype first = literal.indexOf(wildcard);
    if (first >= 0) literal = literal.left(first);
    else literal += ' ';
    const QString start = prefix.simplified() + ' ';
    return literal.trimmed().isEmpty() || start.startsWith(literal) || literal.startsWith(start);
}

namespace {
// Between Allow and Ask the more specific pattern wins, judged by its text before the first wildcard, so that
// "rm -rf build *", allowed with Always, passes while "rm *" still asks about other removals.
int specificity(const QString &pattern)
{
    static const QRegularExpression wildcard(R"([*?\[])");
    const qsizetype first = pattern.simplified().indexOf(wildcard);
    return int(first < 0 ? pattern.simplified().size() + 1 : first);
}

// The most specific enabled rule of the decision that matches the command, or an empty string.
QString bestRule(const QString &line, CommandDecision decision)
{
    QString found;
    for (const CommandRule &rule : ruleList())
        if (rule.enabled && rule.decision == decision && commandPatternMatches(rule.pattern, line)
            && (found.isEmpty() || specificity(rule.pattern) > specificity(found)))
            found = rule.pattern;
    return found;
}
}

CommandVerdict commandRuleVerdict(const QString &command)
{
    const CommandApproval classified = classifyCommandApproval(command);
    if (!classified.deniedReason.isEmpty()) return {CommandDecision::Deny, classified.deniedReason};
    QList<CommandRule> denies;
    for (const QString &pattern : fixedDeniedPatterns()) denies.append({pattern, CommandDecision::Deny, true});
    for (const CommandRule &rule : ruleList())
        if (rule.enabled && rule.decision == CommandDecision::Deny) denies.append(rule);
    const QList<QStringList> all = anyCommands(command);
    for (const QStringList &args : all) {
        // The repository's .git is changed only through git.
        if (executable(args.first()) != "git") {
            for (const QString &arg : args.mid(1))
                if (arg.split('/').contains(".git"))
                    return {CommandDecision::Deny, "the repository's .git is used only through git"};
        }
        const QString line = args.join(' ');
        for (const CommandRule &rule : denies)
            if (commandPatternMatches(rule.pattern, line)) return {CommandDecision::Deny, rule.pattern};
    }
    const auto best = bestRule;
    for (const QStringList &args : all) {
        const QString line = args.join(' ');
        const QString ask = best(line, CommandDecision::Ask);
        if (ask.isEmpty()) continue;
        const QString allow = best(line, CommandDecision::Allow);
        if (allow.isEmpty() || specificity(allow) <= specificity(ask)) return {CommandDecision::Ask, ask};
    }
    const std::optional<QList<QStringList>> commands = plainCommands(command);
    if (!commands) return {};
    QStringList patterns;
    QStringList missing;
    for (const QStringList &args : *commands) {
        const QString matched = best(args.join(' '), CommandDecision::Allow);
        if (matched.isEmpty()) missing.append(args.join(' '));
        else if (!patterns.contains(matched)) patterns.append(matched);
    }
    if (missing.isEmpty()) return {CommandDecision::Allow, patterns.join(", ")};
    // A chain that the rules allow only in part is asked about here, naming the rest, so that agreeing runs the
    // whole chain and Always adds only what was missing; with nothing allowed, the agent decides.
    if (missing.size() < commands->size()) return {CommandDecision::Ask, "not in agentin's rules: " + missing.join("; ")};
    return {};
}

QStringList sessionCommandFamilies(const QString &command)
{
    const QString families = trustedCommandRule(command, {"git add", "git commit"});
    return families.isEmpty() ? QStringList() : families.split(", ");
}

QString trustedCommandRule(const QString &command, const QStringList &prefixes)
{
    const std::optional<QList<QStringList>> commands = plainCommands(command);
    if (prefixes.isEmpty() || !commands) return {};
    QStringList rules;
    for (const QStringList &args : *commands) {
        QString rule;
        for (const QString &prefix : prefixes) {
            const QStringList words = prefix.split(' ', Qt::SkipEmptyParts);
            if (!words.isEmpty() && args.mid(0, words.size()) == words) rule = words.join(' ');
        }
        if (rule.isEmpty()) return {};
        if (!rules.contains(rule)) rules.append(rule);
    }
    return rules.join(", ");
}

QStringList lastingRulePrefix(const QStringList &proposed)
{
    if (proposed.isEmpty() || QStringList{"sudo", "doas", "su"}.contains(executable(proposed.first()))) return {};
    if (proposed.first() != "git") return proposed;
    const QString subcommand = proposed.value(1);
    if (subcommand.isEmpty() || subcommand.startsWith('-') || subcommand == "push") return {};
    QStringList prefix{"git", subcommand};
    if (subcommand == "commit" && (proposed.value(2) == "-m" || proposed.value(2) == "--message")) prefix.append(proposed.at(2));
    return prefix;
}

QStringList alwaysAllowPatterns(const QString &command)
{
    QStringList patterns;
    const std::optional<QList<QStringList>> commands = plainCommands(command);
    if (!commands) return patterns;
    for (const QStringList &args : *commands) {
        // Commands the rules allow already need no line of their own.
        if (!bestRule(args.join(' '), CommandDecision::Allow).isEmpty()) continue;
        if (std::any_of(patterns.cbegin(), patterns.cend(), [&args](const QString &pattern) {
                return commandPatternMatches(pattern, args.join(' ')); }))
            continue;
        const QStringList prefix = lastingRulePrefix(args);
        if (!prefix.isEmpty() && !patterns.contains(prefix.join(' ') + " *")) patterns.append(prefix.join(' ') + " *");
    }
    return patterns;
}
