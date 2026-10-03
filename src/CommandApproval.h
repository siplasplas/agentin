#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// The lasting rule to save for a command prefix the agent proposed, which is often the whole command. Git
// rules keep only the subcommand, and "-m" for a commit, so that they match other files and messages;
// other commands keep the proposal. Empty when no lasting rule may be offered: privilege escalation and
// git push.
QStringList lastingRulePrefix(const QStringList &proposed);

// agentin's own rules: a pattern with * and ? matched against each command of a command line, after wrappers
// such as env and timeout and after Git's directory options; a trailing " *" also matches the command without
// arguments, so "git stash *" matches "git stash". Deny rules and the fixed denials win over everything.
// Allow runs without asking; Ask always asks; Deny declines at once, with no way to agree. A command that no
// rule matches is judged by what it does: see commandRuleVerdict.
enum class CommandDecision { None, Allow, Ask, Deny };
struct CommandRule
{
    QString pattern;
    CommandDecision decision = CommandDecision::Allow;
    bool enabled = true;
};
// "Allow", "Ask" or "Deny", and back; an unknown name is Allow.
QString commandDecisionName(CommandDecision decision);
CommandDecision commandDecisionFromName(const QString &name);

// What a chat knows about the place a command line runs in.
struct CommandContext
{
    // The directory the line starts in; empty when it is not known, and then relative paths cannot be judged.
    QString directory;
    // The directories where writing and running programs needs no question: the chat's directory, those
    // added to it, and the temporary directory.
    QStringList writable;
    // The command prefixes the chat trusts for its session, such as "git fetch"; they count as Allow rules.
    QStringList trusted;
    // The home directory; the user's own when empty.
    QString home;
};

// One command of a line that asks or is declined.
struct CommandFinding
{
    // The command as written, without its redirections.
    QString command;
    CommandDecision decision = CommandDecision::Ask;
    QString reason;
    // The prefix of a rule that would allow the command, such as "git tag -f" or the whole command; empty
    // when no rule can allow it, as for a redirection outside the writable directories.
    QString rule;
    // Whether the rule may be kept for good; installing asks each session at least.
    bool lasting = true;
};

struct CommandVerdict
{
    CommandDecision decision = CommandDecision::None;
    // The rules that allowed the line, or why it asks or is declined.
    QString reason;
    // The commands that ask or are declined, each once.
    QList<CommandFinding> findings;

    // The allow patterns that Always adds, one per command that can get a lasting rule.
    QStringList alwaysPatterns() const;
    // The prefixes a chat can trust for its session, or nothing when some command of the line cannot be
    // trusted, as trusting the others would not let the line run.
    QStringList sessionRules() const;
};

// The rules of a first start: git add, git commit with -m and git's read-only status, log, diff and show as
// Allow, rm * and rmdir * as Ask, and the fixed denials and changes to system packages as Deny.
QList<CommandRule> defaultCommandRules();
// Always denied, whatever the rules say: privilege escalation, removing system packages, and the commands
// that change a remote server, such as git push and gh pr create.
QStringList fixedDeniedPatterns();
// Why a pattern can never be allowed, or an empty string.
QString fixedDenial(const QString &pattern);
QList<CommandRule> commandRules();
void setCommandRules(const QList<CommandRule> &rules);
// Called when an approval adds a rule, so that the settings can be saved.
void setCommandRulesChangedHandler(const std::function<void()> &handler);
// Adds an enabled allow rule, or enables an existing one; false when nothing changed or it is never allowed.
bool addAllowRule(const QString &pattern);
bool commandPatternMatches(const QString &pattern, const QString &command);
// Whether some command could match both the pattern and a rule allowing commands that start with prefix,
// judged by the pattern's text before its first wildcard.
bool patternOverlapsPrefix(const QString &pattern, const QString &prefix);

// The decision about a command line. The line is parsed as Bash and each command it could run is judged, also
// those in substitutions, loops and shell -c scripts:
// - Deny when a command matches a fixed denial or a Deny rule, or writes into a .git directory other than
//   through git.
// - Ask when a command matches an Ask rule and no more specific Allow rule (judged by the text before the
//   first wildcard), or when no rule matches and the command is not harmless by itself: it removes files,
//   uses the network, writes or runs something outside the writable directories, is a Git command that can
//   lose work, or is not known. A command that reads a file that may hold secrets, a redirection outside the
//   writable directories, a variable that changes what programs run, and a line that cannot be parsed ask
//   whatever the Allow rules say.
// - Allow when every command is allowed by a rule or reads only, or writes, builds and runs only inside the
//   writable directories.
// Paths are compared by where they really are: symbolic links that exist when the line is judged are followed.
// None is the answer only for an empty line.
CommandVerdict commandRuleVerdict(const QString &command, const CommandContext &context = {});
