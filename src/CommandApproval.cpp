#include "CommandApproval.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

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
        const QStringList managers{"apt", "apt-get", "dnf", "dnf5", "yum", "zypper", "apk", "pkg", "brew"};
        const QStringList mutations{"install", "reinstall", "upgrade", "full-upgrade", "dist-upgrade", "remove",
                                    "purge", "autoremove", "add", "del", "erase", "update", "in", "rm", "dup"};
        if (managers.contains(program)) {
            // The first non-option is the package manager's operation, never a package name.
            int i = 1;
            while (i < args.size() && args.at(i).startsWith('-')) {
                if (QStringList{"-o", "--option", "-c", "--config", "--root", "--installroot", "--releasever"}
                        .contains(args.at(i))) ++i;
                ++i;
            }
            if (mutations.contains(args.value(i))) return {{}, "System package changes (" + program + ")"};
        }
        if (program == "pacman") {
            for (const QString &arg : args.mid(1))
                if (arg.startsWith('-') && (arg.contains('S') || arg.contains('R') || arg.contains('U')
                    || arg == "--sync" || arg == "--remove" || arg == "--upgrade"))
                    return {{}, "System package changes (pacman)"};
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
QStringList &trustedCommandList()
{
    static QStringList prefixes = defaultTrustedCommands();
    return prefixes;
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
}

QStringList defaultTrustedCommands()
{
    return {"git add", "git commit -m"};
}

QString forbiddenTrustedCommand(const QString &prefix)
{
    const QStringList words = prefix.split(' ', Qt::SkipEmptyParts);
    if (QStringList{"sudo", "doas", "su"}.contains(executable(words.value(0)))) return "privilege escalation is always declined";
    if (words.value(0) == "git" && words.contains("push")) return "git push is always declined";
    return {};
}

QStringList trustedCommands()
{
    return trustedCommandList();
}

void setTrustedCommands(const QStringList &prefixes)
{
    trustedCommandList().clear();
    for (const QString &prefix : prefixes)
        if (forbiddenTrustedCommand(prefix).isEmpty()) trustedCommandList().append(prefix);
}

QString trustedCommandRule(const QString &command)
{
    return trustedCommandRule(command, trustedCommandList());
}

QStringList sessionCommandFamilies(const QString &command)
{
    const QString families = trustedCommandRule(command, {"git add", "git commit"});
    return families.isEmpty() ? QStringList() : families.split(", ");
}

QString trustedCommandRule(const QString &command, const QStringList &prefixes)
{
    if (prefixes.isEmpty()) return {};
    const QString text = withoutLiteralMessages(command);
    if (!plainCommandList(text)) return {};
    const ShellWords parsed = words(text);
    if (!parsed.valid || parsed.commands.isEmpty() || !parsed.substitutions.isEmpty()) return {};
    // A whole command line wrapped in a shell, as Codex runs it, is judged by its script.
    const QStringList &only = parsed.commands.first();
    if (parsed.commands.size() == 1 && only.size() == 3 && (only.at(1) == "-c" || only.at(1) == "-lc")
        && QStringList{"sh", "bash", "zsh", "/bin/sh", "/bin/bash", "/bin/zsh", "/usr/bin/bash", "/usr/bin/zsh"}
               .contains(only.first()))
        return trustedCommandRule(only.at(2), prefixes);
    QStringList rules;
    for (QStringList args : parsed.commands) {
        if (args.first().contains('=')) return {};
        if (args.first() == "git") {
            int i = 1;
            while (i < args.size()) {
                const QString option = args.at(i);
                if (option == "-C" || option == "--git-dir" || option == "--work-tree") i += 2;
                else if (option.startsWith("--git-dir=") || option.startsWith("--work-tree=")) ++i;
                else break;
            }
            args = QStringList{"git"} + args.mid(qMin(i, int(args.size())));
        }
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
