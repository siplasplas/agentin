#include "CommandApproval.h"

#include "shell/ShellEvaluator.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <functional>

namespace {
QString executable(QString value)
{
    value = QFileInfo(value).fileName();
#ifdef Q_OS_WIN
    value = value.toLower();
    if (value.endsWith(".exe")) value.chop(4);
#endif
    return value;
}

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

// What changes a remote server cannot be taken back from here, so it is always declined.
const QStringList &remotePatterns()
{
    static const QStringList patterns{
        "git push *",          "git send-pack *",       "git lfs push *",        "git svn dcommit *",
        "gh pr create *",      "gh pr merge *",         "gh pr close *",         "gh release create *",
        "gh release delete *", "gh release upload *",   "gh repo create *",      "gh repo delete *",
        "gh repo rename *",    "gh issue create *",     "gh issue close *",      "gh workflow run *",
        "gh secret set *",     "glab mr create *",      "glab mr merge *",       "glab mr close *",
        "glab release create *", "glab release delete *", "glab release upload *", "glab repo create *",
        "glab repo delete *",  "glab issue create *",   "glab issue close *",    "glab ci run *",
        "glab variable set *", "hub pull-request *",    "hub release create *",  "hub release delete *",
        "hub create *",        "hub delete *"};
    return patterns;
}
}

QStringList fixedDeniedPatterns()
{
    return QStringList{"sudo *", "doas *", "su *"} + remotePatterns() + packageRemovalPatterns();
}

QString fixedDenial(const QString &pattern)
{
    const QStringList words = pattern.split(' ', Qt::SkipEmptyParts);
    if (QStringList{"sudo", "doas", "su"}.contains(executable(words.value(0)))) return "privilege escalation is always declined";
    if (words.value(0) == "git" && words.contains("push")) return "git push is always declined";
    if (remotePatterns().contains(pattern.simplified())) return "commands that change a remote server are always declined";
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

namespace {
const QRegularExpression &placeholderExpression()
{
    static const QRegularExpression expression("<(int|path|writable)>");
    return expression;
}

// The pattern as a regular expression for a whole command; each <writable> is a captured group.
QRegularExpression patternExpression(const QString &pattern)
{
    static QHash<QString, QRegularExpression> cache;
    const auto found = cache.constFind(pattern);
    if (found != cache.constEnd()) return *found;
    const auto glob = [](const QString &text) {
        return text.isEmpty() ? QString()
                              : QRegularExpression::wildcardToRegularExpression(
                                    text, QRegularExpression::NonPathWildcardConversion | QRegularExpression::UnanchoredWildcardConversion);
    };
    QString expression;
    qsizetype from = 0;
    for (auto it = placeholderExpression().globalMatch(pattern); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        expression += glob(pattern.mid(from, match.capturedStart() - from));
        const QString name = match.captured(1);
        expression += name == "int" ? "[0-9]+" : name == "path" ? "[^ ]+" : "([^ ]+)";
        from = match.capturedEnd();
    }
    expression += glob(pattern.mid(from));
    const QRegularExpression compiled("\\A(?:" + expression + ")\\z");
    if (cache.size() > 2000) cache.clear();
    cache.insert(pattern, compiled);
    return compiled;
}

// The position of the first wildcard or placeholder, or -1.
qsizetype firstWildcard(const QString &pattern)
{
    static const QRegularExpression wildcard(R"([*?\[]|<(int|path|writable)>)");
    return pattern.indexOf(wildcard);
}
}

bool commandPatternMatches(const QString &pattern, const QString &command, const std::function<bool(const QString &)> &writable)
{
    const auto matches = [&](const QString &text) {
        const QRegularExpression expression = patternExpression(text);
        if (!expression.isValid()) return false;
        const QRegularExpressionMatch match = expression.match(command);
        if (!match.hasMatch()) return false;
        for (int group = 1; group <= match.lastCapturedIndex(); ++group)
            if (!writable || !writable(match.captured(group))) return false;
        return true;
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
    const qsizetype first = firstWildcard(literal);
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
    // A pattern without wildcards beats one that adds " *" to the same text.
    const qsizetype first = firstWildcard(pattern.simplified());
    return int(first < 0 ? pattern.simplified().size() + 2 : first);
}

using Writable = std::function<bool(const QString &)>;

// The most specific enabled rule of the decision that matches the command, or an empty string.
QString bestRule(const QString &line, CommandDecision decision, const Writable &writable = {})
{
    QString found;
    for (const CommandRule &rule : ruleList())
        if (rule.enabled && rule.decision == decision && commandPatternMatches(rule.pattern, line, writable)
            && (found.isEmpty() || specificity(rule.pattern) > specificity(found)))
            found = rule.pattern;
    return found;
}

// Whether a pattern names the whole command, as Always adds it: only <int> may stand for a part of it, and a
// trailing " *" adds nothing to the command it matched.
bool coversExactly(const QString &pattern, const QString &line)
{
    QString text = pattern.simplified();
    if (text.endsWith(" *")) text.chop(2);
    static const QRegularExpression broad(R"([*?\[]|<(path|writable)>)");
    return !text.contains(broad) && commandPatternMatches(text, line);
}

ApprovalLists &lists()
{
    static ApprovalLists value = defaultApprovalLists();
    return value;
}
}

ApprovalLists defaultApprovalLists()
{
    ApprovalLists defaults;
    defaults.secretPaths = {"~/.ssh",  "~/.gnupg", "~/.zai-key",    "~/.netrc", "~/.config/gh", "~/.aws",
                            ".env",    ".env.*",   "*credentials*", "*.pem",    "id_rsa*",      "id_ed25519*"};
    defaults.askVariables = {"LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "PATH", "PYTHONPATH", "BASH_ENV", "ENV", "IFS", "PS4",
                             "CDPATH", "HOME", "GIT_DIR", "GIT_WORK_TREE", "GIT_SSH", "GIT_SSH_COMMAND", "GIT_EXEC_PATH",
                             "GIT_CONFIG*", "GIT_EDITOR", "GIT_SEQUENCE_EDITOR", "GIT_PAGER", "GIT_EXTERNAL_DIFF", "GIT_ASKPASS",
                             "GIT_PROXY_COMMAND", "SSH_ASKPASS", "EDITOR", "VISUAL", "PAGER", "PKG_CONFIG_PATH",
                             "PKG_CONFIG_LIBDIR"};
    return defaults;
}

ApprovalLists approvalLists()
{
    return lists();
}

namespace {
int &timeLimit()
{
    static int minutes = 0;
    return minutes;
}
}

int commandTimeLimit()
{
    return timeLimit();
}

void setCommandTimeLimit(int minutes)
{
    timeLimit() = qMax(0, minutes);
}

void setApprovalLists(const ApprovalLists &value)
{
    lists() = value;
}

namespace {
using shell::CommandUse;
using shell::Effect;

bool hasGlob(const QString &text)
{
    return text.contains('*') || text.contains('?') || text.contains('[');
}

bool globMatches(const QString &glob, const QString &text)
{
    const QRegularExpression expression(QRegularExpression::wildcardToRegularExpression(glob));
    return expression.isValid() && expression.match(text).hasMatch();
}

// Whether a path is a directory of the list or inside one.
bool inside(const QString &path, const QStringList &directories)
{
    for (const QString &directory : directories) {
        const QString clean = QDir::cleanPath(directory);
        if (clean.isEmpty()) continue;
        if (path == clean || path.startsWith(clean == "/" ? clean : clean + '/')) return true;
    }
    return false;
}

// Where a path really is: the longest part of it that exists, with its symbolic links followed, and the rest
// as written. A link made later in the same line is not seen, and neither is one that changes before the
// command runs.
QString realPath(const QString &path, int depth = 0)
{
    if (!path.startsWith('/') || depth > 8) return path;
    QString existing = path;
    QStringList rest;
    while (existing.size() > 1) {
        const QFileInfo info(existing);
        if (info.exists() || info.isSymLink()) break;
        rest.prepend(existing.section('/', -1));
        existing.truncate(qMax(qsizetype(1), existing.lastIndexOf('/')));
    }
    const QFileInfo info(existing);
    QString real = info.canonicalFilePath();
    // A link to something that does not exist yet still decides where a new file goes.
    if (real.isEmpty() && info.isSymLink()) real = realPath(QDir::cleanPath(info.symLinkTarget()), depth + 1);
    if (real.isEmpty()) return path;
    return rest.isEmpty() ? real : QDir::cleanPath(real + '/' + rest.join('/'));
}

// A written or run path that is not in the writable directories, described for the question; an empty
// string for one that is.
QString outside(const QString &path, const QStringList &writable, const char *verb)
{
    const QString real = realPath(path);
    if (inside(real, writable)) return {};
    return QString(verb) + ' ' + path + (real == path ? QString() : " (really " + real + ")") + ", outside the writable directories";
}

// Whether a path is .git or inside one; a pattern that could name .git counts too. The shell's * and ? do not
// match a leading dot, so only a pattern that starts with one can.
bool namesGitDirectory(const QString &path)
{
    for (const QString &component : path.split('/', Qt::SkipEmptyParts))
        if (component == ".git" || (hasGlob(component) && component.startsWith('.') && globMatches(component, ".git")))
            return true;
    return false;
}

// Whether two names, either of which may hold glob characters, could be the same name. A glob that takes any
// name, such as * or **, does not lead to a particular file or directory, no more than a recursive search does;
// nor would the shell's * match the leading dot of names such as .ssh.
bool namesMeet(const QString &path, const QString &pattern)
{
    if (path == pattern) return true;
    const bool pathGlob = hasGlob(path);
    if (pathGlob)
        for (const char *name : {"x", "file.txt", "data.json"})
            if (globMatches(path, name)) return false;
    if (pathGlob && hasGlob(pattern)) {
        // Two globs meet when one matches a name the other stands for, with its wildcards left empty or filled.
        for (const QString &fill : {QString(), QString("x")}) {
            QString patternName = pattern;
            QString pathName = path;
            patternName.replace('*', fill).replace('?', 'x');
            pathName.replace('*', fill).replace('?', 'x');
            if (globMatches(path, patternName) || globMatches(pattern, pathName)) return true;
        }
        return false;
    }
    if (pathGlob) return globMatches(path, pattern);
    return hasGlob(pattern) && globMatches(pattern, path);
}

// Whether the path from part i on could name a file that the pattern from part j on covers; ** stands for
// any directories, and what is below a matched pattern is covered too.
bool partsMeet(const QStringList &path, int i, const QStringList &pattern, int j)
{
    if (j == pattern.size()) return true;
    if (pattern.at(j) == "**") return partsMeet(path, i, pattern, j + 1) || (i < path.size() && partsMeet(path, i + 1, pattern, j));
    if (i == path.size()) return false;
    return namesMeet(path.at(i), pattern.at(j)) && partsMeet(path, i + 1, pattern, j + 1);
}
}

bool pathMatchesList(const QString &path, const QStringList &patterns, const QString &home)
{
    if (!path.startsWith('/')) return false;
    const QStringList parts = QDir::cleanPath(path).split('/', Qt::SkipEmptyParts);
    for (QString pattern : patterns) {
        pattern = QDir::cleanPath(pattern.trimmed());
        if (pattern.isEmpty() || pattern == ".") continue;
        if (pattern == "~" || pattern.startsWith("~/")) pattern = home + pattern.mid(1);
        else if (!pattern.startsWith('/')) pattern = "**/" + pattern;
        if (partsMeet(parts, 0, pattern.split('/', Qt::SkipEmptyParts), 0)) return true;
    }
    return false;
}

namespace {
bool mayHoldSecrets(const QString &path, const QString &home)
{
    return pathMatchesList(path, lists().secretPaths, home);
}

// Variables that choose the programs a command runs, or change how the shell reads the line.
bool changesWhatRuns(const QString &variable)
{
    for (const QString &pattern : lists().askVariables)
        if (pattern == variable || (hasGlob(pattern) && globMatches(pattern, variable))) return true;
    return false;
}

// A command as the text of a rule that names it: glob characters in its words stand for themselves, and a
// number, alone or after an option such as -j, becomes <int>.
QString ruleText(const QStringList &words)
{
    static const QRegularExpression number("^(-{1,2}[A-Za-z][A-Za-z-]*=?|-)?[0-9]+$");
    QStringList parts;
    for (QString word : words) {
        word.replace(QRegularExpression(R"(([*?\[<]))"), "[\\1]");
        const QRegularExpressionMatch match = number.match(word);
        if (match.hasMatch()) word = match.captured(1) + "<int>";
        parts.append(word);
    }
    return parts.join(' ');
}

// The scheme and host an address starts with, such as "https://example.com/", or an empty string; an address
// without a scheme is taken as http, as curl does.
QString urlBase(const QString &url)
{
    static const QRegularExpression base("^([A-Za-z][A-Za-z0-9+.-]*://[^/?#\\s]+)");
    const QRegularExpressionMatch match = base.match(url.contains("://") ? url : "http://" + url);
    return match.hasMatch() ? match.captured(1).toLower() + '/' : QString();
}

// The address part of a rule, such as "https://example.com/" in "curl *https://example.com/*", or an empty string.
QString ruleUrl(const QString &pattern)
{
    static const QRegularExpression address("[A-Za-z][A-Za-z0-9+.-]*://[^*?\\[ ]*");
    const QRegularExpressionMatch match = address.match(pattern);
    return match.hasMatch() ? match.captured(0).toLower() : QString();
}

// The addresses a rule for a fetching command does not cover: all of them must start with the address the
// rule names, so that one address it allows does not let the command send to another.
QStringList urlsOutside(const QString &pattern, const QStringList &urls)
{
    const QString allowed = ruleUrl(pattern);
    QStringList outside;
    if (allowed.isEmpty()) return outside;
    for (const QString &url : urls) {
        const QString full = (url.contains("://") ? url : "http://" + url).toLower();
        if (!full.startsWith(allowed)) outside.append(url);
    }
    return outside;
}

// The rule for a command that asks only because agentin does not know it or it uses the network: its program
// with the words that name what it does, such as "gh run list" or "npm test", when there are any.
QString commandFamily(const QStringList &words, const QStringList &urls = {})
{
    // A fetching command is allowed for the one server it talks to.
    if (!urls.isEmpty()) {
        const QString base = urlBase(urls.first());
        const bool same = !base.isEmpty() && std::all_of(urls.cbegin(), urls.cend(), [&base](const QString &url) { return urlBase(url) == base; });
        if (same) return words.first() + " *" + base + '*';
    }
    static const QRegularExpression plain("^[A-Za-z][A-Za-z0-9._+-]*$");
    int count = 0;
    while (count < words.size() && plain.match(words.at(count)).hasMatch()) ++count;
    return count >= 2 ? words.mid(0, count).join(' ') : ruleText(words);
}

// The words the rules see: the program by its name, and Git without the options that only choose the
// repository.
QStringList ruleWords(const CommandUse &use)
{
    QStringList words = use.words;
    if (words.isEmpty()) return words;
    if (!use.program.isEmpty() && use.effect != Effect::Execute) words[0] = use.program;
    if (words.first() != "git") return words;
    int i = 1;
    while (i < words.size()) {
        const QString option = words.at(i);
        if (option == "-C" || option == "--git-dir" || option == "--work-tree") i += 2;
        else if (option.startsWith("--git-dir=") || option.startsWith("--work-tree=")) ++i;
        else break;
    }
    return QStringList{"git"} + words.mid(qMin(i, int(words.size())));
}

// The fixed denial of a command that changes a remote server, or an empty string.
QString remoteChange(const CommandUse &use, const QStringList &words)
{
    const QString program = words.value(0);
    if (!QStringList{"git", "gh", "glab", "hub"}.contains(program)) return {};
    QStringList operands;
    for (const QString &word : words.mid(1))
        if (!word.startsWith('-')) operands.append(word);
    for (const QString &pattern : remotePatterns()) {
        const QStringList wanted = pattern.chopped(2).split(' ');
        if (wanted.first() != program) continue;
        bool found = false;
        if (program == "git") {
            // The subcommand is known also behind options that change the configuration.
            found = use.subcommand == wanted.at(1) && (wanted.size() == 2 || operands.contains(wanted.at(2)));
        } else {
            for (int i = 0; !found && i + wanted.size() - 1 <= operands.size(); ++i) found = operands.mid(i, wanted.size() - 1) == wanted.mid(1);
        }
        if (found) return program == "git" && wanted.at(1) == "push" ? QString("git push is always declined")
                                                                    : "it changes a remote server (" + pattern + ")";
    }
    if ((program == "gh" || program == "glab") && operands.value(0) == "api") {
        // A request that sends fields or names a method other than GET writes.
        QString method;
        bool fields = false;
        for (int i = 1; i < words.size(); ++i) {
            const QString &word = words.at(i);
            if (word == "-X" || word == "--method") method = words.value(i + 1);
            else if (word.startsWith("--method=")) method = word.mid(9);
            else if (word.startsWith("-X") && word.size() > 2) method = word.mid(2);
            else if (QStringList{"-f", "-F", "--field", "--raw-field", "--input"}.contains(word) || word.startsWith("--field=")
                     || word.startsWith("--raw-field=") || word.startsWith("--input="))
                fields = true;
        }
        if (method.isEmpty() ? fields : method.compare("GET", Qt::CaseInsensitive) != 0)
            return "it changes a remote server (" + program + " api with a writing request)";
    }
    return {};
}

// The Git commands that can lose work ask, unless an Allow rule names them more closely than "git <command> *".
struct GitAsk
{
    QString reason;
    // The words up to the one that makes the command ask, as the prefix of a rule that allows it.
    QString rule;
};

GitAsk gitAsk(const CommandUse &use, const QStringList &words)
{
    if (words.value(0) != "git" || use.effect != Effect::GitWrite) return {};
    const QString &command = use.subcommand;
    const int first = int(words.indexOf(command));
    if (first < 0) return {};
    const auto shortOption = [](const QString &word, QChar letter) {
        return word.size() > 1 && word.startsWith('-') && !word.startsWith("--") && word.contains(letter);
    };
    const auto found = [&](const QString &reason, int index) { return GitAsk{reason, words.mid(0, index + 1).join(' ')}; };
    const int next = qMin(first + 1, int(words.size()) - 1);
    if (command == "rebase") return found("git rebase rewrites commits", next);
    if (command == "restore") return found("git restore discards changes", next);
    if (command == "clean") return found("git clean removes files that Git does not track", next);
    for (int i = first + 1; i < words.size(); ++i) {
        const QString &word = words.at(i);
        if (command == "reset" && word == "--hard") return found("git reset --hard discards changes", i);
        if (command == "checkout" && (word == "--" || word == "." || word == "--force" || shortOption(word, 'f')))
            return found("git checkout of files discards changes", i);
        if (command == "tag" && (word == "--force" || word == "--delete" || shortOption(word, 'f') || shortOption(word, 'd')))
            return found("git tag replaces or removes a tag", i);
        if (command == "branch" && shortOption(word, 'D')) return found("git branch -D removes a branch that may not be merged", i);
        if (command == "stash" && i == first + 1 && (word == "drop" || word == "clear")) return found("git stash " + word + " discards stashed changes", i);
        if (command == "commit" && word == "--amend") return found("git commit --amend rewrites a commit", i);
        if (command == "gc" && word.startsWith("--prune")) return found("git gc --prune removes objects", i);
        if (command == "reflog" && i == first + 1) return found("git reflog " + word + " removes the record of earlier states", i);
    }
    return {};
}

struct Allowed
{
    QString pattern;
    // Whether some matching rule names the whole command.
    bool exact = false;
};

// The most specific allow among the rules and the prefixes a chat trusts.
Allowed bestAllow(const QString &line, const QStringList &trusted, const Writable &writable)
{
    Allowed found;
    const auto consider = [&](const QString &pattern) {
        if (!commandPatternMatches(pattern, line, writable)) return;
        if (found.pattern.isEmpty() || specificity(pattern) > specificity(found.pattern)) found.pattern = pattern;
        if (coversExactly(pattern, line)) found.exact = true;
    };
    for (const CommandRule &rule : ruleList())
        if (rule.enabled && rule.decision == CommandDecision::Allow) consider(rule.pattern);
    for (const QString &prefix : trusted) consider(prefix + " *");
    return found;
}

// Whether an interpreter or a shell runs code that the command line gives, or that it reads from its input,
// rather than a script file it names: python3 -c, python3 -, node -e, a shell fed by a pipe.
bool runsCodeFromLine(const CommandUse &use, const QStringList &words)
{
    static const QStringList interpreters{"python", "python3", "python2", "node", "perl", "ruby", "php", "lua", "deno", "bun",
                                          "bash", "sh", "dash", "zsh", "ksh"};
    if (!interpreters.contains(use.program)) return false;
    for (const QString &word : words.mid(1)) {
        if (word == "-c" || word == "-e" || word == "--eval" || word == "-s" || word == "-") return true;
        // A module, as in python3 -m json.tool, is named like a script.
        if (word == "-m") return false;
        if (!word.startsWith('-')) return false;
    }
    return true;
}

// Why a command changes or runs something outside the writable directories, one reason per place; its
// redirections are judged on their own.
QStringList places(const CommandUse &use, const QStringList &writable)
{
    QStringList reasons;
    // Git writes into the repository of the directory it runs in.
    const char *verb = use.effect == Effect::GitWrite ? "changes the repository in" : "writes";
    for (const QString &path : use.writes) {
        if (use.redirectionWrites.contains(path)) continue;
        if (const QString reason = outside(path, writable, verb); !reason.isEmpty()) reasons.append(reason);
    }
    // A build tool runs the project's own code: its build files, tests or install scripts.
    const char *runs = use.effect == Effect::Build ? "runs the project code in" : "runs";
    for (const QString &path : use.executes)
        if (const QString reason = outside(path, writable, runs); !reason.isEmpty()) reasons.append(reason);
    return reasons;
}

struct Judged
{
    CommandDecision decision = CommandDecision::Allow;
    QString reason;
    QString rule;
    bool lasting = true;
    QString allowedBy;
    bool exact = false;
};

Judged judge(const CommandUse &use, const CommandContext &context, const QString &home)
{
    const QStringList words = ruleWords(use);
    const QString text = words.join(' ');
    const auto denied = [](const QString &reason) { return Judged{CommandDecision::Deny, reason, {}, false, {}}; };
    // <writable> in a rule: a path that is inside the writable directories from every directory the command
    // may run in.
    const Writable writable = [&use, &context](const QString &path) {
        if (path.startsWith('/')) return inside(realPath(QDir::cleanPath(path)), context.writable);
        if (use.directories.isEmpty()) return false;
        for (const QString &directory : use.directories)
            if (!inside(realPath(QDir::cleanPath(directory + '/' + path)), context.writable)) return false;
        return true;
    };

    if (QStringList{"sudo", "doas", "su"}.contains(use.program)) return denied("privilege escalation is always declined");
    const QString remote = remoteChange(use, words);
    if (!remote.isEmpty()) return denied(remote);
    if (!text.isEmpty()) {
        for (const QString &pattern : fixedDeniedPatterns())
            if (commandPatternMatches(pattern, text)) return denied(fixedDenial(pattern).isEmpty() ? pattern : fixedDenial(pattern));
        for (const CommandRule &rule : ruleList())
            if (rule.enabled && rule.decision == CommandDecision::Deny && commandPatternMatches(rule.pattern, text, writable))
                return denied("agentin's rule " + rule.pattern);
    }
    // The repository's .git is changed only by git itself.
    for (const QString &path : use.program == "git" ? use.redirectionWrites : use.writes + use.executes)
        if (namesGitDirectory(path) || namesGitDirectory(realPath(path)))
            return denied("the repository's .git is changed only through git");

    // What a rule about the command does not cover: where its redirections write.
    QStringList reasons = use.redirectionProblems;
    for (const QString &path : use.redirectionWrites) {
        if (const QString reason = outside(path, context.writable, "writes"); !reason.isEmpty()) reasons.append(reason);
        else if (pathMatchesList(path, lists().protectedPaths, home) || pathMatchesList(realPath(path), lists().protectedPaths, home))
            reasons.append("writes " + path + ", a protected path");
    }
    const bool coverable = reasons.isEmpty() && !text.isEmpty() && !use.program.isEmpty();

    const QString ask = text.isEmpty() ? QString() : bestRule(text, CommandDecision::Ask, writable);
    const Allowed allowedBy = text.isEmpty() ? Allowed() : bestAllow(text, context.trusted, writable);
    const QString allow = allowedBy.pattern;
    const int allowed = allow.isEmpty() ? -1 : specificity(allow);
    // A rule that names the whole command, as Always adds it, also covers what the command reads, where it
    // writes and what the catalog cannot judge about it.
    const bool exact = allowedBy.exact;

    QStringList problems = use.problems;
    for (const QString &problem : use.redirectionProblems) problems.removeAll(problem);
    if (!exact) {
        if (!shell::describesWithoutContents(use.program))
            for (const QString &path : use.reads)
                if (mayHoldSecrets(path, home) || mayHoldSecrets(realPath(path), home))
                    reasons.append("reads " + path + ", which may hold secrets");
        for (const QString &path : use.writes)
            if (!use.redirectionWrites.contains(path)
                && (pathMatchesList(path, lists().protectedPaths, home) || pathMatchesList(realPath(path), lists().protectedPaths, home)))
                reasons.append("writes " + path + ", a protected path");
    }

    Judged result;
    result.rule = ruleText(words);
    const QStringList outsidePlaces = places(use, context.writable);
    // A place the command does not name as a whole path, such as the repository git works in, the directory a
    // compiler writes into by default, or a file given relative to the current directory, cannot be part of a
    // rule: one naming the command would allow it in any directory.
    bool implicitPlace = use.effect == Effect::GitWrite && !outsidePlaces.isEmpty();
    // A path is named when a word is the path, or an option with the path attached, as in -o/x or --output=/x.
    const auto named = [&use](const QString &path) {
        return std::any_of(use.words.cbegin(), use.words.cend(), [&path](const QString &word) {
            if (word == path) return true;
            if (!word.endsWith(path)) return false;
            const QString option = word.chopped(path.size());
            return option.startsWith('-') && !option.contains('/');
        });
    };
    for (const QString &path : use.writes + use.executes)
        if (!use.redirectionWrites.contains(path) && !inside(realPath(path), context.writable) && !named(path))
            implicitPlace = true;
    const GitAsk git = gitAsk(use, words);
    if (!ask.isEmpty() && allowed <= specificity(ask)) {
        reasons.prepend("agentin's rule " + ask);
    } else if (!git.reason.isEmpty() && allowed <= QString("git " + use.subcommand + " ").size()) {
        reasons.append(git.reason);
        result.rule = git.rule;
    } else if (!allow.isEmpty()) {
        // A rule names a kind of command, such as "git add *", not the places it may change: those stay the
        // writable directories, as without a rule, unless the rule names the whole command.
        if (!exact) {
            reasons += problems;
            reasons += outsidePlaces;
            // A rule for a fetching command names a server.
            if (use.effect == Effect::Network)
                for (const QString &url : urlsOutside(allow, use.urls)) reasons.append("fetches " + url + ", which " + allow + " does not name");
        }
        result.allowedBy = allow;
    } else {
        const bool plain = reasons.isEmpty() && problems.isEmpty();
        reasons += problems;
        if (use.effect == Effect::Unknown && runsCodeFromLine(use, words))
            reasons.append("it runs " + use.program + " code given in the command or read from its input, which agentin cannot judge");
        else if (use.effect == Effect::Unknown && problems.isEmpty())
            reasons.append(use.program.isEmpty() ? QString("the program is not known") : use.program + " is not a program agentin knows");
        else if (use.effect == Effect::Network) reasons.append("it uses the network");
        else if (use.effect == Effect::Remove) reasons.append("it removes files");
        reasons += outsidePlaces;
        // A rule for a Git command covers its other files and messages too, and one for a command that asks only
        // as it is not known or uses the network covers the same kind of work with other arguments.
        if (plain && words.value(0) == "git") result.rule = lastingRulePrefix(words).join(' ');
        else if (plain && outsidePlaces.isEmpty() && (use.effect == Effect::Network || use.effect == Effect::Unknown))
            result.rule = commandFamily(words, use.urls);
    }
    if (reasons.isEmpty()) return result;
    reasons.removeDuplicates();
    result.decision = CommandDecision::Ask;
    result.allowedBy.clear();
    // A value that is not known, such as "$U" from a substitution, cannot be named by a rule: one naming it
    // as written would allow it whatever it turns out to be.
    const bool unknownValue = std::any_of(problems.cbegin(), problems.cend(),
                                          [](const QString &problem) { return problem.contains(" not known"); });
    if (!coverable || unknownValue || implicitPlace) result.rule.clear();
    // The files xargs adds are not in the command's words, so a rule names the command as it is.
    result.exact = use.inputArguments;
    // Code given on the command line or read from the input is new each time: a rule naming the command would
    // allow any code.
    if (runsCodeFromLine(use, words)) result.rule.clear();
    // Installing writes to a place the command does not name, so it should be decided each time.
    if (std::any_of(reasons.cbegin(), reasons.cend(), [](const QString &reason) { return reason.contains("outside the project"); })) {
        result.lasting = false;
        reasons.append("allowing it once is the safe choice");
    }
    result.reason = reasons.join("; ");
    return result;
}

// For a line that cannot be parsed: the fixed denials found by the words at the start of each command.
QString textDenial(const QString &command)
{
    static const QRegularExpression separators("[;|&()`\n]|\\$\\(");
    static const QRegularExpression assignment("^[A-Za-z_][A-Za-z0-9_]*=");
    static const QStringList skipped{"then", "do", "else", "elif", "if", "while", "until", "!", "{", "time", "env", "command", "exec", "nohup"};
    for (const QString &part : command.split(separators, Qt::SkipEmptyParts)) {
        QStringList words = part.simplified().remove('"').remove('\'').split(' ', Qt::SkipEmptyParts);
        while (!words.isEmpty() && (skipped.contains(words.first()) || assignment.match(words.first()).hasMatch())) words.removeFirst();
        if (words.isEmpty()) continue;
        const QString program = executable(words.first());
        if (QStringList{"sudo", "doas", "su"}.contains(program)) return "privilege escalation is always declined";
        if (program == "git" && words.contains("push")) return "git push is always declined";
#ifdef Q_OS_WIN
        if (program == "runas") return "privilege escalation is always declined";
        if (QStringList{"winget", "choco", "scoop"}.contains(program)
            && QStringList{"install", "upgrade", "uninstall", "update"}.contains(words.value(1).toLower()))
            return "system package changes are declined";
#endif
    }
    return {};
}

QString shortened(const QString &command)
{
    const QString text = command.simplified();
    return text.size() > 80 ? text.left(77) + "…" : text;
}
}

QStringList CommandVerdict::alwaysPatterns() const
{
    QStringList patterns;
    for (const CommandFinding &finding : findings) {
        const QString pattern = finding.exact ? finding.rule : finding.rule + " *";
        if (finding.decision == CommandDecision::Ask && finding.lasting && !finding.rule.isEmpty() && fixedDenial(pattern).isEmpty()
            && !patterns.contains(pattern))
            patterns.append(pattern);
    }
    return patterns;
}

QStringList CommandVerdict::sessionRules() const
{
    QStringList rules;
    for (const CommandFinding &finding : findings) {
        if (finding.decision != CommandDecision::Ask) continue;
        if (finding.rule.isEmpty() || finding.exact) return {};
        if (!rules.contains(finding.rule)) rules.append(finding.rule);
    }
    return rules;
}

QList<ApprovalChoice> CommandVerdict::choices() const
{
    QList<ApprovalChoice> result;
    for (const CommandFinding &finding : findings) {
        if (finding.decision != CommandDecision::Ask) continue;
        ApprovalChoice choice{finding.command, finding.reason, {}, {}};
        if (!finding.rule.isEmpty() && !finding.exact) choice.trust = finding.rule;
        const QString pattern = finding.exact ? finding.rule : finding.rule + " *";
        if (!finding.rule.isEmpty() && finding.lasting && fixedDenial(pattern).isEmpty()) choice.pattern = pattern;
        result.append(choice);
    }
    return result;
}

QString CommandVerdict::explanation() const
{
    QStringList lines;
    for (const CommandFinding &finding : findings) {
        QString command = finding.command.simplified();
        if (command.size() > 100) command = command.left(97) + "…";
        lines.append("Asks because of " + command + ": " + finding.reason);
    }
    return lines.join('\n');
}

CommandVerdict commandRuleVerdict(const QString &command, const CommandContext &context)
{
    CommandVerdict verdict;
    if (command.trimmed().isEmpty()) return verdict;
    const QString home = context.home.isEmpty() ? QDir::homePath() : context.home;
    // The writable directories by their real places, as the paths of the commands are compared.
    CommandContext real = context;
    for (QString &directory : real.writable) directory = realPath(QDir::cleanPath(directory));
    const shell::NodePtr tree = shell::parseBash(command);
    const shell::Evaluation evaluation = shell::evaluate(tree, {context.directory, home, {}});
    const auto decline = [&verdict](const QString &text, const QString &reason) {
        verdict.decision = CommandDecision::Deny;
        verdict.reason = reason;
        verdict.findings = {{text, CommandDecision::Deny, reason, {}, false}};
        return verdict;
    };
    bool scanText = tree->kind == shell::Node::Kind::Unsupported;
#ifdef Q_OS_WIN
    // Lines for cmd or PowerShell are not Bash.
    scanText = true;
#endif
    if (scanText) {
        const QString reason = textDenial(command);
        if (!reason.isEmpty()) return decline(shortened(command), reason);
    }
    QStringList allowedBy;
    QSet<QString> variables;
    for (const CommandUse &use : evaluation.uses) {
        Judged judged = judge(use, real, home);
        if (judged.decision == CommandDecision::Deny) return decline(use.text, judged.reason);
        QStringList reasons;
        if (!judged.reason.isEmpty()) reasons.append(judged.reason);
        // A program of the project runs without a question and can do whatever such a variable would make it
        // do, so the variables ask only about other programs.
        const bool projectProgram = use.effect == Effect::Execute && !use.executes.isEmpty()
            && std::all_of(use.executes.cbegin(), use.executes.cend(),
                           [&real](const QString &path) { return inside(realPath(path), real.writable); });
        for (const QString &variable : use.environment) {
            if (projectProgram || !changesWhatRuns(variable) || variables.contains(variable)) continue;
            variables.insert(variable);
            reasons.append("the line sets " + variable + ", which can change what commands run");
            // No rule about the command covers the variable.
            judged.rule.clear();
        }
        if (reasons.isEmpty()) {
            if (!judged.allowedBy.isEmpty() && !allowedBy.contains(judged.allowedBy)) allowedBy.append(judged.allowedBy);
            continue;
        }
        const CommandFinding finding{use.text, CommandDecision::Ask, reasons.join("; "), judged.rule, judged.lasting, judged.exact};
        if (std::none_of(verdict.findings.cbegin(), verdict.findings.cend(), [&finding](const CommandFinding &other) {
                return other.command == finding.command && other.reason == finding.reason; }))
            verdict.findings.append(finding);
    }
    if (!evaluation.judged)
        verdict.findings.append({shortened(command), CommandDecision::Ask, "the line cannot be judged: " + evaluation.reason, {}, false});
    // A long run is a question of its own: no rule about the commands covers it.
    if (timeLimit() > 0 && (context.timeoutSeconds < 0 || context.timeoutSeconds > timeLimit() * 60)) {
        const QString reason = context.timeoutSeconds < 0
            ? QString("it runs in the background, with no time limit")
            : QString("it may run %1 minutes, longer than the limit of %2").arg(QString::number(context.timeoutSeconds / 60.0, 'g', 3)).arg(timeLimit());
        verdict.findings.append({shortened(command), CommandDecision::Ask, reason, {}, false});
    }
    if (verdict.findings.isEmpty()) {
        verdict.decision = CommandDecision::Allow;
        verdict.reason = allowedBy.isEmpty() ? QString("it reads, or writes only inside the writable directories") : allowedBy.join(", ");
        return verdict;
    }
    verdict.decision = CommandDecision::Ask;
    QStringList reasons;
    for (const CommandFinding &finding : verdict.findings) reasons.append(shortened(finding.command) + ": " + finding.reason);
    verdict.reason = reasons.join(" | ");
    return verdict;
}

CommandVerdict readVerdict(const QString &tool, const QStringList &paths, const CommandContext &context)
{
    const QString home = context.home.isEmpty() ? QDir::homePath() : context.home;
    CommandVerdict verdict;
    for (const QString &given : paths) {
        QString path = given;
        if (path == "~" || path.startsWith("~/")) path = home + path.mid(1);
        if (!path.startsWith('/')) {
            if (context.directory.isEmpty()) continue;
            path = context.directory + '/' + path;
        }
        path = QDir::cleanPath(path);
        if (!mayHoldSecrets(path, home) && !mayHoldSecrets(realPath(path), home)) continue;
        verdict.findings.append({tool + ' ' + given, CommandDecision::Ask, "reads " + path + ", which may hold secrets", {}, false});
    }
    if (verdict.findings.isEmpty()) {
        verdict.decision = CommandDecision::Allow;
        verdict.reason = "reading is allowed";
        return verdict;
    }
    verdict.decision = CommandDecision::Ask;
    QStringList reasons;
    for (const CommandFinding &finding : verdict.findings) reasons.append(finding.command + ": " + finding.reason);
    verdict.reason = reasons.join(" | ");
    return verdict;
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
