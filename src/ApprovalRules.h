#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// A lasting "allow" rule saved by an agent, for example after "Always allow".
struct ApprovalRule
{
    QString agent;
    // What the rule allows, as the user reads it.
    QString rule;
    // Where it is saved, as the user reads it.
    QString where;
    QString file;
    // The rule as stored: a line of a Codex rules file or an entry of Claude's permissions.allow.
    QString stored;
    // A Codex rule's command prefix, word by word.
    QStringList words;
};

// Codex keeps prefix_rule(...) lines in $CODEX_HOME/rules/*.rules (by default ~/.codex).
QList<ApprovalRule> codexRules();
// Claude Code keeps permissions.allow in the user settings and in each project's .claude/settings.json
// and .claude/settings.local.json; Claude and GLM chats share them.
QList<ApprovalRule> claudeRules(const QStringList &projectDirectories);
// Removes the rule from its file; returns an error message or an empty string.
QString removeApprovalRule(const ApprovalRule &rule);
