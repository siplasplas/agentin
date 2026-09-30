#pragma once

#include <QString>

struct CommandApproval
{
    QString sessionRule;
    QString deniedReason;
};

// Conservative shell classification; unknown syntax always requires a human decision.
CommandApproval classifyCommandApproval(const QString &command);
