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
