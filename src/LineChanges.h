#pragma once

#include <QByteArray>

#include <atomic>

// Lines added and removed in a file between two versions, within the bounds of the changed files view.
struct LineChanges
{
    enum class Kind {
        Counted,
        // Compared by content only: a binary file, or text above kMaximumDiffBytes.
        Binary,
        TooLarge,
        // Too many differences or too slow to count; only the line counts are known.
        Rewritten,
        Cancelled
    };
    Kind kind = Kind::Counted;
    int added = 0;
    int removed = 0;
    int linesBefore = 0;
    int linesAfter = 0;
};

constexpr qint64 kMaximumDiffBytes = 64 * 1024 * 1024;
constexpr int kMaximumDifferences = 100000;
constexpr int kDiffTimeLimitMs = 3000;

// Meant for a worker thread; cancel, when set, stops a long count early.
LineChanges countLineChanges(const QByteArray &before, const QByteArray &after,
                             const std::atomic<bool> *cancel = nullptr);
