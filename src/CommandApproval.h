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

// Commands the user allows without asking, as their leading words such as "git commit"; the options set
// them for all chats.
QStringList defaultTrustedCommands();
QStringList trustedCommands();
void setTrustedCommands(const QStringList &prefixes);
// The trusted prefixes that allow the command line, or an empty string. Plain commands joined by &&, ||, ;
// or | qualify when each starts with a trusted prefix; redirections, substitutions, variables, subshells
// and background jobs never do. Git's -C, --git-dir and --work-tree options may precede its subcommand.
QString trustedCommandRule(const QString &command);
