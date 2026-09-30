#pragma once

#include "AgentBackend.h"

#include <QWidget>

class QTreeWidget;

enum class UsagePace { Unknown, Green, Yellow, Red, Expired };

struct UsageAssessment
{
    UsagePace pace = UsagePace::Unknown;
    double remainingPercent = -1;
    // The first whole second when unchanged usage is below the elapsed fraction.
    qint64 greenAt = 0;
};

UsageAssessment assessUsageLimit(const UsageLimit &limit, qint64 now);

struct ProviderLimits
{
    QString provider;
    QList<UsageLimit> windows;
};

// Account snapshots for the selected conversation provider.
class UsageLimitsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit UsageLimitsPanel(QWidget *parent = nullptr);
    void setLimits(const QList<ProviderLimits> &limits);
    void refresh(qint64 now);
    QSize sizeHint() const override;

private:
    QList<ProviderLimits> limits_;
    QTreeWidget *tree_;
};
