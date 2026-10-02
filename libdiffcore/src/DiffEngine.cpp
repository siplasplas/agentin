#include "diffcore/DiffEngine.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "diffcore/LineInterner.h"
#include "diffcore/SliderHeuristics.h"
#include "internal/Diff.h"

namespace diffcore {

namespace {

using internal::Block;
using internal::EditType;

// QStringList::size() returns qsizetype (64-bit on modern platforms).
// Our public Hunk/LineRange use int. Line counts above INT_MAX would mean
// a 2-billion-line file which we don't support anyway; this helper makes
// the narrowing explicit and easy to audit in one place.
constexpr int iSize(qsizetype n) {
    return static_cast<int>(n);
}

// Map internal EditType to public ChangeType (one-to-one).
ChangeType toChangeType(EditType t) {
    switch (t) {
        case EditType::Insert: return ChangeType::Insert;
        case EditType::Delete: return ChangeType::Delete;
        case EditType::Equal:  return ChangeType::Equal;
    }
    return ChangeType::Equal;  // Unreachable, silence compiler.
}

// Build a single Hunk from a Block, treating X as left and Y as right.
Hunk hunkFromBlock(const Block& b, ChangeType type) {
    Hunk h;
    h.type = type;
    h.leftRange.start = b.startX;
    h.leftRange.count = b.endX - b.startX;
    h.rightRange.start = b.startY;
    h.rightRange.count = b.endY - b.startY;
    return h;
}

// If the engine swapped A and B, blocks reference swapped axes.
// Undo that so X always means "left" and Y always means "right".
void unswapBlocks(std::vector<Block>& blocks) {
    for (Block& b : blocks) {
        std::swap(b.startX, b.startY);
        std::swap(b.endX, b.endY);
        if (b.type == EditType::Insert) b.type = EditType::Delete;
        else if (b.type == EditType::Delete) b.type = EditType::Insert;
    }
}

// Build the raw hunk list directly from blocks (no merging).
std::vector<Hunk> rawHunksFromBlocks(const std::vector<Block>& blocks) {
    std::vector<Hunk> hunks;
    hunks.reserve(blocks.size());
    for (const Block& b : blocks) {
        hunks.push_back(hunkFromBlock(b, toChangeType(b.type)));
    }
    return hunks;
}

// Adjacent Delete+Insert (or Insert+Delete) pairs represent replaced
// regions. Merge them into a single Replace hunk covering both ranges.
std::vector<Hunk> mergeReplaceHunks(const std::vector<Hunk>& input) {
    std::vector<Hunk> out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const Hunk& current = input[i];
        const bool isIns = current.type == ChangeType::Insert;
        const bool isDel = current.type == ChangeType::Delete;
        if ((isIns || isDel) && i + 1 < input.size()) {
            const Hunk& next = input[i + 1];
            const bool pair = (isIns && next.type == ChangeType::Delete) ||
                              (isDel && next.type == ChangeType::Insert);
            if (pair) {
                Hunk merged;
                merged.type = ChangeType::Replace;
                // Take left range from whichever hunk is a Delete
                // (covers lines in left file), right range from Insert.
                const Hunk& del = isDel ? current : next;
                const Hunk& ins = isIns ? current : next;
                merged.leftRange = del.leftRange;
                merged.rightRange = ins.rightRange;
                out.push_back(merged);
                ++i;  // Skip the consumed next hunk.
                continue;
            }
        }
        out.push_back(current);
    }
    return out;
}

// Merge consecutive hunks that share the same ChangeType into one hunk.
// This consolidates runs that the O(NP) backtracker emits as separate
// single-element blocks (e.g. two adjacent Insert{1} → one Insert{2}).
std::vector<Hunk> coalesceAdjacentHunks(const std::vector<Hunk>& input) {
    if (input.empty()) return {};
    std::vector<Hunk> out;
    out.push_back(input[0]);
    for (size_t i = 1; i < input.size(); ++i) {
        Hunk& last = out.back();
        const Hunk& h = input[i];
        if (h.type == last.type) {
            last.leftRange.count  += h.leftRange.count;
            last.rightRange.count += h.rightRange.count;
        } else {
            out.push_back(h);
        }
    }
    return out;
}

// Fill in aggregate stats based on final hunks and raw edit distance.
DiffStats computeStats(const std::vector<Hunk>& hunks, int editDistance) {
    DiffStats s;
    s.editDistance = editDistance;
    for (const Hunk& h : hunks) {
        switch (h.type) {
            case ChangeType::Insert:
                s.additions += h.rightRange.count;
                break;
            case ChangeType::Delete:
                s.deletions += h.leftRange.count;
                break;
            case ChangeType::Replace:
                s.deletions += h.leftRange.count;
                s.additions += h.rightRange.count;
                s.modifications += 1;
                break;
            case ChangeType::Equal:
                break;
        }
    }
    return s;
}

// Special-case handling for pairs of empty and trivial inputs, where the
// backtracking code in the internal engine does not generate blocks.
// We synthesize the equivalent hunks here.
DiffResult handleTrivialCase(const QStringList& left, const QStringList& right) {
    DiffResult r;
    r.leftLineCount = iSize(left.size());
    r.rightLineCount = iSize(right.size());
    if (left.isEmpty() && right.isEmpty()) {
        return r;  // Both empty - no hunks, stats zero.
    }
    if (left.isEmpty()) {
        Hunk h;
        h.type = ChangeType::Insert;
        h.leftRange = {0, 0};
        h.rightRange = {0, iSize(right.size())};
        r.hunks.push_back(h);
        r.stats.additions = iSize(right.size());
        r.stats.editDistance = iSize(right.size());
        return r;
    }
    // right.isEmpty()
    Hunk h;
    h.type = ChangeType::Delete;
    h.leftRange = {0, iSize(left.size())};
    h.rightRange = {0, 0};
    r.hunks.push_back(h);
    r.stats.deletions = iSize(left.size());
    r.stats.editDistance = iSize(left.size());
    return r;
}

}  // namespace

DiffResult DiffEngine::compute(const QStringList& left,
                               const QStringList& right,
                               const DiffOptions& opts) {
    // Trivial cases: the O(NP) engine requires at least one non-empty side
    // to produce sensible blocks. Handle empties directly.
    if (left.isEmpty() || right.isEmpty()) {
        return handleTrivialCase(left, right);
    }

    // Identical quick-check: avoid running the algorithm when the inputs
    // are already equal after normalization. Also a fast path for large
    // identical files (common case in directory diffs).
    LineInterner interner;
    auto ids = interner.intern(left, right, opts);
    if (ids.leftIds == ids.rightIds) {
        DiffResult r;
        r.leftLineCount = iSize(left.size());
        r.rightLineCount = iSize(right.size());
        Hunk h;
        h.type = ChangeType::Equal;
        h.leftRange = {0, iSize(left.size())};
        h.rightRange = {0, iSize(right.size())};
        r.hunks.push_back(h);
        return r;
    }

    // Run the O(NP) engine on interned ID sequences.
    internal::Diff<std::vector<int>> engine(ids.leftIds, ids.rightIds);
    std::vector<Block> blocks = engine.walk();

    if (engine.swapped()) {
        unswapBlocks(blocks);
    }

    // Translate blocks -> hunks, optionally merging Replace regions.
    std::vector<Hunk> hunks = rawHunksFromBlocks(blocks);
    if (opts.mergeReplaceHunks) {
        hunks = mergeReplaceHunks(hunks);
    }
    if (opts.coalesceAdjacentSameType) {
        hunks = coalesceAdjacentHunks(hunks);
    }
    if (opts.applySliderHeuristics) {
        applySliderHeuristics(hunks, left, right);
    }

    DiffResult result;
    result.hunks = std::move(hunks);
    result.leftLineCount = iSize(left.size());
    result.rightLineCount = iSize(right.size());
    result.stats = computeStats(result.hunks, engine.editDistance());
    return result;
}

ChangeCounts DiffEngine::countChanges(const QStringList& left,
                                      const QStringList& right,
                                      const DiffOptions& opts,
                                      const CountLimits& limits) {
    ChangeCounts counts;
    if (left.isEmpty() || right.isEmpty()) {
        counts.added = iSize(right.size());
        counts.removed = iSize(left.size());
        return counts;
    }
    LineInterner interner;
    auto ids = interner.intern(left, right, opts);
    std::vector<int>& a = ids.leftIds;
    std::vector<int>& b = ids.rightIds;
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix
           && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) ++suffix;
    a.assign(a.begin() + static_cast<std::ptrdiff_t>(prefix), a.end() - static_cast<std::ptrdiff_t>(suffix));
    b.assign(b.begin() + static_cast<std::ptrdiff_t>(prefix), b.end() - static_cast<std::ptrdiff_t>(suffix));
    if (a.empty() || b.empty()) {
        counts.added = static_cast<int>(b.size());
        counts.removed = static_cast<int>(a.size());
        return counts;
    }

    // A line one side has more often than the other must be removed or added by any edit script, so
    // these surpluses bound the edit distance from below in linear time. A rewritten file exceeds the
    // bound at once, while the search would need about P^2 steps to reach it.
    if (limits.maxEditDistance > 0) {
        int maxId = 0;
        for (int id : a) maxId = std::max(maxId, id);
        for (int id : b) maxId = std::max(maxId, id);
        std::vector<int> balance(static_cast<size_t>(maxId) + 1, 0);
        for (int id : a) ++balance[static_cast<size_t>(id)];
        for (int id : b) --balance[static_cast<size_t>(id)];
        long long bound = 0;
        for (int surplus : balance) bound += std::abs(surplus);
        if (bound > limits.maxEditDistance) {
            counts.status = ChangeCounts::Status::TooManyDifferences;
            return counts;
        }
    }

    internal::Limits bounds;
    bounds.maxEditDistance = limits.maxEditDistance;
    bounds.cancel = limits.cancel;
    if (limits.timeLimitMs > 0)
        bounds.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.timeLimitMs);
    internal::Diff<std::vector<int>> engine(a, b);
    switch (engine.count(bounds)) {
        case internal::Stop::None: break;
        case internal::Stop::TooManyDifferences: counts.status = ChangeCounts::Status::TooManyDifferences; return counts;
        case internal::Stop::TimedOut: counts.status = ChangeCounts::Status::TimedOut; return counts;
        case internal::Stop::Cancelled: counts.status = ChangeCounts::Status::Cancelled; return counts;
    }
    // With only insertions and deletions, D = added + removed and the common lines are (M + N - D) / 2.
    const int common = (static_cast<int>(a.size() + b.size()) - engine.editDistance()) / 2;
    counts.added = static_cast<int>(b.size()) - common;
    counts.removed = static_cast<int>(a.size()) - common;
    return counts;
}

}  // namespace diffcore
