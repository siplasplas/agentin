#include "UsageLimitsPanel.h"

#include <QDateTime>
#include <QHeaderView>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

UsageAssessment assessUsageLimit(const UsageLimit &limit, qint64 now)
{
    UsageAssessment result;
    if (limit.resetsAt > 0 && limit.resetsAt <= now) {
        result.pace = UsagePace::Expired;
        return result;
    }
    const bool percentageKnown = std::isfinite(limit.usedPercent) && limit.usedPercent >= 0;
    const double used = percentageKnown ? qBound(0.0, limit.usedPercent, 100.0) : -1;
    if (percentageKnown) result.remainingPercent = 100 - used;
    if (used >= 100 || limit.status == "rejected") {
        result.pace = UsagePace::Red;
        return result;
    }
    if (!percentageKnown || limit.resetsAt <= 0 || limit.windowMinutes <= 0
        || limit.windowMinutes > std::numeric_limits<qint64>::max() / 60) return result;
    const qint64 duration = limit.windowMinutes * 60;
    const qint64 start = limit.resetsAt - duration;
    if (now < start) return result;
    const double elapsedPercent = double(now - start) / duration * 100;
    if (used < elapsedPercent) result.pace = UsagePace::Green;
    else {
        result.pace = UsagePace::Yellow;
        result.greenAt = qMax(now + 1, start + qint64(std::floor(duration * used / 100)) + 1);
    }
    return result;
}

namespace {
QString windowName(const UsageLimit &limit)
{
    const qint64 minutes = limit.windowMinutes;
    QString length;
    if (minutes == 7 * 24 * 60) length = "Week";
    else if (minutes > 0 && minutes % (24 * 60) == 0) length = QString("%1 days").arg(minutes / (24 * 60));
    else if (minutes > 0 && minutes % 60 == 0) length = QString("%1 h").arg(minutes / 60);
    else if (minutes > 0) length = QString("%1 min").arg(minutes);
    else length = "Limit";
    return limit.name.isEmpty() ? length : limit.name + " " + length;
}

QString timestamp(qint64 seconds)
{
    return QDateTime::fromSecsSinceEpoch(seconds).toString("ddd d MMM HH:mm:ss");
}
}

UsageLimitsPanel::UsageLimitsPanel(QWidget *parent) : QWidget(parent), tree_(new QTreeWidget(this))
{
    setObjectName("usageLimitsPanel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    tree_->setObjectName("usageLimitsTree");
    tree_->setHeaderLabels({"Provider", "Window", "Remaining", "Resets", "Pace / break"});
    tree_->setHeaderHidden(true);
    tree_->setMinimumHeight(0);
    tree_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    tree_->setRootIsDecorated(false);
    tree_->setSelectionMode(QAbstractItemView::NoSelection);
    tree_->setFocusPolicy(Qt::NoFocus);
    tree_->setUniformRowHeights(true);
    tree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(true);
    layout->addWidget(tree_);
    setMinimumHeight(1);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible()) refresh(QDateTime::currentSecsSinceEpoch());
    });
    timer->start();
}

QSize UsageLimitsPanel::sizeHint() const
{
    // Leave half of a second row visible to suggest that more windows can be revealed.
    const int rowHeight = tree_->fontMetrics().height() + 4;
    return QSize(500, rowHeight * 3 / 2 + 2 * tree_->frameWidth());
}

void UsageLimitsPanel::setLimits(const QList<ProviderLimits> &limits)
{
    limits_ = limits;
    refresh(QDateTime::currentSecsSinceEpoch());
}

void UsageLimitsPanel::refresh(qint64 now)
{
    int row = 0;
    for (const ProviderLimits &provider : limits_) {
        QList<UsageLimit> windows = provider.windows;
        std::sort(windows.begin(), windows.end(), [](const UsageLimit &a, const UsageLimit &b) {
            return a.windowMinutes == b.windowMinutes ? a.id < b.id : a.windowMinutes > b.windowMinutes;
        });
        for (const UsageLimit &limit : windows) {
            QTreeWidgetItem *item = row < tree_->topLevelItemCount() ? tree_->topLevelItem(row)
                                                                  : new QTreeWidgetItem(tree_);
            ++row;
            const UsageAssessment assessment = assessUsageLimit(limit, now);
            const QString remaining = assessment.remainingPercent < 0 ? "Not reported"
                : assessment.remainingPercent > 0 && assessment.remainingPercent < 0.1 ? "<0.1%"
                : QString::number(assessment.remainingPercent, 'f', 1) + '%';
            QString pace;
            QColor color = palette().color(QPalette::Text);
            switch (assessment.pace) {
            case UsagePace::Green: pace = "On pace"; color = QColor("#218739"); break;
            case UsagePace::Yellow:
                pace = "Pause until " + timestamp(assessment.greenAt); color = QColor("#b77900"); break;
            case UsagePace::Red: pace = "Exhausted"; color = QColor("#d32f2f"); break;
            case UsagePace::Expired: pace = "Awaiting updated limits"; break;
            case UsagePace::Unknown: pace = "Pace not reported"; break;
            }
            const QStringList text{provider.provider, windowName(limit), remaining,
                limit.resetsAt > 0 ? timestamp(limit.resetsAt) : "Not reported", pace};
            QString tooltip = text.join(" · ");
            tooltip += "\nBased on the last limits reported by the provider. "
                       "Green: usage is below elapsed window time. Yellow: let time catch up. Red: exhausted.";
            if (assessment.pace == UsagePace::Yellow)
                tooltip += "\nAssumes no further use. If several windows need a break, wait until the latest time.";
            if (assessment.pace == UsagePace::Expired)
                tooltip += "\nThe previous window ended; no new quota is assumed before the provider reports it.";
            if (!limit.status.isEmpty()) tooltip += "\nProvider status: " + limit.status;
            for (int column = 0; column < text.size(); ++column) {
                item->setText(column, text.at(column));
                item->setForeground(column, color);
                item->setToolTip(column, tooltip);
            }
            item->setData(0, Qt::UserRole, limit.id);
            item->setData(4, Qt::UserRole, int(assessment.pace));
        }
    }
    while (tree_->topLevelItemCount() > row) delete tree_->takeTopLevelItem(row);
}
