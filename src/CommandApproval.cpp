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

// Whether a path is .git or inside one; a pattern that could name .git counts too.
bool namesGitDirectory(const QString &path)
{
    for (const QString &component : path.split('/', Qt::SkipEmptyParts))
        if (component == ".git" || (hasGlob(component) && globMatches(component, ".git"))) return true;
    return false;
}

// The files that may hold keys and passwords: reading them asks. ** stands for any directories.
const QStringList &secretPatterns()
{
    static const QStringList patterns{"~/.ssh/**",  "~/.gnupg/**",      "~/.zai-key", "~/.netrc",     "~/.config/gh/**", "~/.aws/**",
                                      "**/.env",    "**/.env.*",        "**/*credentials*", "**/*.pem", "**/id_rsa*",
                                      "**/id_ed25519*"};
    return patterns;
}

QRegularExpression secretExpression(QString pattern, const QString &home)
{
    if (pattern.startsWith("~/")) pattern = home + pattern.mid(1);
    QString expression = "^";
    for (int i = 0; i < pattern.size(); ++i) {
        if (pattern.mid(i, 3) == "**/") {
            expression += "(?:.*/)?";
            i += 2;
        } else if (pattern.mid(i) == "/**") {
            // The directory itself, as a recursive search reads all of it.
            expression += "(?:/.*)?";
            i += 2;
        } else if (pattern.at(i) == '*') {
            expression += "[^/]*";
        } else {
            expression += QRegularExpression::escape(pattern.at(i));
        }
    }
    return QRegularExpression(expression + "$");
}

bool mayHoldSecrets(const QString &path, const QString &home)
{
    for (const QString &pattern : secretPatterns())
        if (secretExpression(pattern, home).match(path).hasMatch()) return true;
    if (!hasGlob(path)) return false;
    // A pattern asks when it could name one of the usual secret files.
    for (const char *file : {"/.ssh/id_rsa", "/.ssh/id_ed25519", "/.ssh/config", "/.gnupg/secring.gpg", "/.zai-key", "/.netrc",
                             "/.config/gh/hosts.yml", "/.aws/credentials"})
        if (globMatches(path, home + file)) return true;
    // A pattern that takes any file, such as *, is no more a way to a secret than a recursive search is.
    const QString name = path.section('/', -1);
    for (const char *file : {"x", "file.txt", "data.json"})
        if (globMatches(name, file)) return false;
    for (const char *file : {".env", ".env.local", "credentials", "credentials.json", "key.pem", "id_rsa", "id_ed25519"})
        if (globMatches(name, file)) return true;
    return false;
}

// Variables that choose the programs a command runs, or change how the shell reads the line.
bool changesWhatRuns(const QString &variable)
{
    static const QSet<QString> variables{"LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "PATH", "PYTHONPATH", "BASH_ENV", "ENV",
                                         "IFS", "PS4", "CDPATH", "HOME", "GIT_DIR", "GIT_WORK_TREE", "GIT_SSH", "GIT_SSH_COMMAND",
                                         "GIT_EXEC_PATH", "GIT_EDITOR", "GIT_SEQUENCE_EDITOR", "GIT_PAGER", "GIT_EXTERNAL_DIFF",
                                         "GIT_ASKPASS", "GIT_PROXY_COMMAND", "SSH_ASKPASS", "EDITOR", "VISUAL", "PAGER"};
    return variables.contains(variable) || variable.startsWith("GIT_CONFIG");
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

// The most specific allow among the rules and the prefixes a chat trusts, or an empty string.
QString bestAllow(const QString &line, const QStringList &trusted)
{
    QString found = bestRule(line, CommandDecision::Allow);
    for (const QString &prefix : trusted) {
        const QString pattern = prefix + " *";
        if (commandPatternMatches(pattern, line) && (found.isEmpty() || specificity(pattern) > specificity(found))) found = pattern;
    }
    return found;
}

struct Judged
{
    CommandDecision decision = CommandDecision::Allow;
    QString reason;
    QString rule;
    bool lasting = true;
    QString allowedBy;
};

Judged judge(const CommandUse &use, const CommandContext &context, const QString &home)
{
    const QStringList words = ruleWords(use);
    const QString text = words.join(' ');
    const auto denied = [](const QString &reason) { return Judged{CommandDecision::Deny, reason, {}, false, {}}; };

    if (QStringList{"sudo", "doas", "su"}.contains(use.program)) return denied("privilege escalation is always declined");
    const QString remote = remoteChange(use, words);
    if (!remote.isEmpty()) return denied(remote);
    if (!text.isEmpty()) {
        for (const QString &pattern : fixedDeniedPatterns())
            if (commandPatternMatches(pattern, text)) return denied(fixedDenial(pattern).isEmpty() ? pattern : fixedDenial(pattern));
        for (const CommandRule &rule : ruleList())
            if (rule.enabled && rule.decision == CommandDecision::Deny && commandPatternMatches(rule.pattern, text))
                return denied("agentin's rule " + rule.pattern);
    }
    // The repository's .git is changed only by git itself.
    for (const QString &path : use.program == "git" ? use.redirectionWrites : use.writes + use.executes)
        if (namesGitDirectory(path) || namesGitDirectory(realPath(path)))
            return denied("the repository's .git is changed only through git");

    // What a rule about the command does not cover: where its redirections write.
    QStringList reasons = use.redirectionProblems;
    for (const QString &path : use.redirectionWrites)
        if (const QString reason = outside(path, context.writable, "writes"); !reason.isEmpty()) reasons.append(reason);
    const bool coverable = reasons.isEmpty() && !text.isEmpty() && !use.program.isEmpty();

    const QString ask = text.isEmpty() ? QString() : bestRule(text, CommandDecision::Ask);
    const QString allow = text.isEmpty() ? QString() : bestAllow(text, context.trusted);
    const int allowed = allow.isEmpty() ? -1 : specificity(allow);
    // A rule that names the whole command, as Always adds it, also covers what the command reads and what the
    // catalog cannot judge about it.
    const bool exact = allowed >= text.size();

    QStringList problems = use.problems;
    for (const QString &problem : use.redirectionProblems) problems.removeAll(problem);
    if (!exact && !shell::describesWithoutContents(use.program))
        for (const QString &path : use.reads)
            if (mayHoldSecrets(path, home) || mayHoldSecrets(realPath(path), home))
                reasons.append("reads " + path + ", which may hold secrets");

    Judged result;
    result.rule = text;
    const GitAsk git = gitAsk(use, words);
    if (!ask.isEmpty() && allowed <= specificity(ask)) {
        reasons.prepend("agentin's rule " + ask);
    } else if (!git.reason.isEmpty() && allowed <= QString("git " + use.subcommand + " ").size()) {
        reasons.append(git.reason);
        result.rule = git.rule;
    } else if (!allow.isEmpty()) {
        if (!exact) reasons += problems;
        result.allowedBy = allow;
    } else {
        const bool plain = reasons.isEmpty() && problems.isEmpty();
        reasons += problems;
        if (use.effect == Effect::Unknown && problems.isEmpty())
            reasons.append(use.program.isEmpty() ? QString("the program is not known") : use.program + " is not a program agentin knows");
        else if (use.effect == Effect::Network) reasons.append("it uses the network");
        else if (use.effect == Effect::Remove) reasons.append("it removes files");
        for (const QString &path : use.writes) {
            if (use.redirectionWrites.contains(path)) continue;
            if (const QString reason = outside(path, context.writable, "writes"); !reason.isEmpty()) reasons.append(reason);
        }
        for (const QString &path : use.executes)
            if (const QString reason = outside(path, context.writable, "runs"); !reason.isEmpty()) reasons.append(reason);
        // A rule for a Git command covers its other files and messages too.
        if (plain && words.value(0) == "git") result.rule = lastingRulePrefix(words).join(' ');
    }
    if (reasons.isEmpty()) return result;
    reasons.removeDuplicates();
    result.decision = CommandDecision::Ask;
    result.allowedBy.clear();
    if (!coverable) result.rule.clear();
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
        const QString pattern = finding.rule + " *";
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
        if (finding.rule.isEmpty()) return {};
        if (!rules.contains(finding.rule)) rules.append(finding.rule);
    }
    return rules;
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
        for (const QString &variable : use.environment) {
            if (!changesWhatRuns(variable) || variables.contains(variable)) continue;
            variables.insert(variable);
            reasons.append("the line sets " + variable + ", which can change what commands run");
            // No rule about the command covers the variable.
            judged.rule.clear();
        }
        if (reasons.isEmpty()) {
            if (!judged.allowedBy.isEmpty() && !allowedBy.contains(judged.allowedBy)) allowedBy.append(judged.allowedBy);
            continue;
        }
        const CommandFinding finding{use.text, CommandDecision::Ask, reasons.join("; "), judged.rule, judged.lasting};
        if (std::none_of(verdict.findings.cbegin(), verdict.findings.cend(), [&finding](const CommandFinding &other) {
                return other.command == finding.command && other.reason == finding.reason; }))
            verdict.findings.append(finding);
    }
    if (!evaluation.judged)
        verdict.findings.append({shortened(command), CommandDecision::Ask, "the line cannot be judged: " + evaluation.reason, {}, false});
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
