#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

struct CommandApproval
{
    QString sessionRule;
    QString deniedReason;
};

// Conservative shell classification; unknown syntax always requires a human decision.
CommandApproval classifyCommandApproval(const QString &command);

// The lasting rule to save for a command prefix the agent proposed, which is often the whole command. Git
// rules keep only the subcommand, and "-m" for a commit, so that they match other files and messages;
// other commands keep the proposal. Empty when no lasting rule may be offered: privilege escalation and
// git push.
QStringList lastingRulePrefix(const QStringList &proposed);
// The allow patterns that Always adds to agentin's list for a plain command line, one per command, such as
// "git add *" and "rm -rf build *"; empty for a line that is not plain.
QStringList alwaysAllowPatterns(const QString &command);

// agentin's own rules: a pattern with * and ? matched against each command of a command line, after Git's
// directory options; a trailing " *" also matches the command without arguments, so "git stash *" matches
// "git stash". Deny rules and the fixed denials win over allow rules, which win over an agent's own rules.
// Allow runs without asking; Ask always asks, whatever chat trust or the agent's own rules say; Deny declines
// at once, with no way to agree.
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
struct CommandVerdict
{
    CommandDecision decision = CommandDecision::None;
    // The patterns that decided, or why a command is always declined.
    QString reason;
};

// The rules of a first start: git add * and git commit -m * as Allow, rm * and rmdir * as Ask, and the
// fixed denials and changes to system packages as Deny.
QList<CommandRule> defaultCommandRules();
// Always denied, whatever the rules say: git push, privilege escalation and removing system packages.
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
// Deny when any command of the line, also in substitutions, matches a deny rule or a fixed denial, or when a
// command other than git names a path in .git; ask when any command matches an ask rule and no more specific
// allow rule (judged by the text before the first wildcard); allow
// when the line is plain (commands joined by &&, ||, ; or |, also inside a shell -c wrapper, without
// redirections, substitutions or variables) and each command matches an enabled allow rule.
CommandVerdict commandRuleVerdict(const QString &command);
// The prefixes that allow every command of a plain line, such as the command families trusted for one chat,
// or an empty string.
QString trustedCommandRule(const QString &command, const QStringList &prefixes);
// The command families a chat can trust for its session, one per command of the line, such as
// {"git add", "git commit"} for "git add . && git commit -m '…'"; empty when the line has other commands.
QStringList sessionCommandFamilies(const QString &command);
