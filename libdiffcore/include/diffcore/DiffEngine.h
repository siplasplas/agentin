// High-level diff API. Takes two sequences of lines (QStringList) and
// produces a DiffResult that the GUI/CLI can render directly.
//
// Internally:
//  1. LineInterner assigns integer IDs to unique (normalized) lines.
//  2. The templated O(NP) engine computes an edit script over the IDs.
//  3. Block results are translated into public Hunk objects, with an
//     optional merge step turning adjacent Delete+Insert into Replace.
//  4. If the engine swapped its inputs internally, the adapter swaps
//     coordinates back so the user always sees (left, right) as given.

#ifndef DIFFCORE_DIFFENGINE_H
#define DIFFCORE_DIFFENGINE_H

#include <QStringList>

#include <atomic>

#include "DiffTypes.h"

namespace diffcore {

// Bounds for counting changes; zero or null means no bound.
struct CountLimits {
    int maxEditDistance = 0;   // Added plus removed lines
    int timeLimitMs = 0;
    const std::atomic<bool>* cancel = nullptr;
};

// Lines added and removed between two sequences, as an edit script of insertions and deletions has them.
struct ChangeCounts {
    enum class Status { Complete, TooManyDifferences, TimedOut, Cancelled };
    Status status = Status::Complete;
    int added = 0;     // Valid when Complete
    int removed = 0;   // Valid when Complete
};

class DiffEngine {
public:
    DiffResult compute(const QStringList& left,
                       const QStringList& right,
                       const DiffOptions& opts = {});

    // Runs only the main loop of the O(NP) engine: no path, no hunks and no slider heuristics, with
    // memory linear in the input. Lines equal at both ends are skipped before the search.
    ChangeCounts countChanges(const QStringList& left,
                              const QStringList& right,
                              const DiffOptions& opts = {},
                              const CountLimits& limits = {});
};

}  // namespace diffcore

#endif  // DIFFCORE_DIFFENGINE_H
