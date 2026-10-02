#include "LineChanges.h"

#include <diffcore/DiffEngine.h>

#include <QStringList>

bool looksBinary(const QByteArray &content)
{
    return content.left(8000).contains('\0');
}

QStringList splitLines(const QByteArray &content)
{
    QStringList result;
    qsizetype start = 0;
    while (start < content.size()) {
        qsizetype end = content.indexOf('\n', start);
        if (end < 0) end = content.size();
        qsizetype length = end - start;
        if (length > 0 && content.at(end - 1) == '\r') --length;
        result.append(QString::fromUtf8(content.constData() + start, length));
        start = end + 1;
    }
    return result;
}

LineChanges countLineChanges(const QByteArray &before, const QByteArray &after, const std::atomic<bool> *cancel)
{
    LineChanges changes;
    if (looksBinary(before) || looksBinary(after)) {
        changes.kind = LineChanges::Kind::Binary;
        return changes;
    }
    if (before.size() > kMaximumDiffBytes || after.size() > kMaximumDiffBytes) {
        changes.kind = LineChanges::Kind::TooLarge;
        return changes;
    }
    const QStringList left = splitLines(before);
    const QStringList right = splitLines(after);
    changes.linesBefore = int(left.size());
    changes.linesAfter = int(right.size());
    diffcore::CountLimits limits;
    limits.maxEditDistance = kMaximumDifferences;
    limits.timeLimitMs = kDiffTimeLimitMs;
    limits.cancel = cancel;
    diffcore::DiffOptions options;
    options.applySliderHeuristics = false;
    const diffcore::ChangeCounts counts = diffcore::DiffEngine().countChanges(left, right, options, limits);
    switch (counts.status) {
    case diffcore::ChangeCounts::Status::Complete:
        changes.added = counts.added;
        changes.removed = counts.removed;
        break;
    case diffcore::ChangeCounts::Status::TooManyDifferences:
    case diffcore::ChangeCounts::Status::TimedOut:
        changes.kind = LineChanges::Kind::Rewritten;
        break;
    case diffcore::ChangeCounts::Status::Cancelled:
        changes.kind = LineChanges::Kind::Cancelled;
        break;
    }
    return changes;
}
