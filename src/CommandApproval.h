#pragma once

#include <QString>
#include <QStringList>

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

// Commands the user allows without asking, as their leading words such as "git commit"; the options set
// them for all chats.
QStringList defaultTrustedCommands();
// Commands that are never allowed without asking, such as git push: empty, or why not.
QString forbiddenTrustedCommand(const QString &prefix);
QStringList trustedCommands();
void setTrustedCommands(const QStringList &prefixes);
// The trusted prefixes that allow the command line, or an empty string. Plain commands joined by &&, ||, ;
// or | qualify when each starts with a trusted prefix; redirections, substitutions, variables, subshells
// and background jobs never do. Git's -C, --git-dir and --work-tree options may precede its subcommand.
QString trustedCommandRule(const QString &command);
// The same for other prefixes, such as the command families trusted for one chat.
QString trustedCommandRule(const QString &command, const QStringList &prefixes);
// The command families a chat can trust for its session, one per command of the line, such as
// {"git add", "git commit"} for "git add . && git commit -m '…'"; empty when the line has other commands.
QStringList sessionCommandFamilies(const QString &command);
