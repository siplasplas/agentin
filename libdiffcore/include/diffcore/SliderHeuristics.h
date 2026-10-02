// Slider-placement heuristics: shift Insert/Delete hunks so their boundaries
// match typical human diff preferences. See SliderHeuristics.cpp for the
// individual rules (H1..H5, H3b).

#ifndef DIFFCORE_SLIDERHEURISTICS_H
#define DIFFCORE_SLIDERHEURISTICS_H

#include <QStringList>

#include "DiffTypes.h"

namespace diffcore {

enum class HeuristicId {
    None = 0,
    H1   = 1,   // empty-line run → slide forward
    H2   = 2,   // indent drop → slide back
    H3   = 3,   // section separator single line (/*, ;;***, …)
    H3b  = 4,   // multi-line doc-comment block (/**…*/) → slide back N
    H4   = 5,   // #pragma mark / // MARK → slide back
    H5   = 6,   // },\n{ boundary → slide back 1
    H6   = 7,   // series of // comments before block matches last N lines → slide back N
    H7   = 8    // k>=2, rel[p-2] empty & rel[p-1] non-empty → slide back by 1
};
static constexpr int kHeuristicCount = 9;  // valid indices 0..8

struct HeuristicProbe {
    int         pos;         // 0-based final position (after exclusive + H1)
    HeuristicId exclusive;   // which exclusive heuristic fired (H2/H3/H3b/H4/H5 or None)
    int         posAfterExcl;// position after exclusive only (== original pos if exclusive==None)
    bool        h1Applied;   // whether H1 additionally shifted empty lines forward
};

// Probe: what would adjustPosition do for a block of `size` lines at
// 0-based position `p` in `rel`?  Does NOT modify anything.
HeuristicProbe probeHeuristic(int p, int size, const QStringList& rel);

// Apply slider heuristics to all Insert/Delete hunks in `hunks`.
void applySliderHeuristics(std::vector<Hunk>& hunks,
                            const QStringList& left,
                            const QStringList& right);

}  // namespace diffcore

#endif  // DIFFCORE_SLIDERHEURISTICS_H
