#include "diffcore/SliderHeuristics.h"

#include <algorithm>
#include <QString>

namespace diffcore {

namespace {

// Leading indentation width in columns. Tab counts as 4 spaces.
int indentWidth(const QString& line) {
    int w = 0;
    for (QChar c : line) {
        if (c == QLatin1Char('\t'))      w += 4;
        else if (c == QLatin1Char(' '))  ++w;
        else break;
    }
    return w;
}

// Phase 1: exclusive heuristics (first match wins).
// Returns the shifted position and which heuristic fired (None if none).
//   H5  : JSON "},\n{" boundary → slide back by 1.
//   H3b : multi-line doc-comment (/**…*/) before block → slide back by N.
//   H2  : indent drop in slide-left range → slide back.
//   H3  : single-line section separator → slide back by 1.
//   H4  : #pragma mark / // MARK → slide back.
static std::pair<int, HeuristicId>
exclusiveHeuristic(int p, int size, const QStringList& rel) {
    const int N = rel.size();

    // H5: "},\n{" boundary.
    if (p >= 2 && p + size - 1 < N &&
        rel[p - 2].trimmed() == QStringLiteral("},") &&
        rel[p - 1].trimmed() == QStringLiteral("{") &&
        rel[p - 2] == rel[p + size - 2] &&
        rel[p - 1] == rel[p + size - 1]) {
        return {p - 1, HeuristicId::H5};
    }

    // Slide-left range: max k s.t. rel[p-i] == rel[p+size-i] for i=1..k.
    int k = 0;
    while (p - (k + 1) >= 0 && p + size - (k + 1) < N &&
           rel[p - (k + 1)] == rel[p + size - (k + 1)])
        ++k;

    // H3b: doc-comment block (/**…*/) before block matches last k lines.
    // Must precede H2: indent of `*/` is smaller than code indent, which
    // would cause H2 to slide by 1 (to `*/`) instead of k (to `/**`).
    if (k > 0 &&
        rel[p - k].trimmed().startsWith(QStringLiteral("/*")) &&
        rel[p - 1].trimmed().endsWith(QStringLiteral("*/"))) {
        return {p - k, HeuristicId::H3b};
    }

    // H6: contiguous run of "//" lines immediately before block (within the
    // slide-left range) matches the corresponding last lines of the block
    // → slide back to the first comment of the run.
    // Counts only the unbroken series starting at p-1; stops at the first
    // non-"//" line.  Placed before H2 so indent-drop doesn't steal it.
    if (k > 0) {
        int kLC = 0;
        while (kLC < k &&
               rel[p - kLC - 1].trimmed().startsWith(QStringLiteral("//")))
            ++kLC;
        if (kLC > 0) return {p - kLC, HeuristicId::H6};
    }

    // H7: at least 2 repeating lines, second-from-block (rel[p-2]) is empty,
    // immediately-before-block (rel[p-1]) is NOT empty → "blank + content"
    // group separator; slide back by 2 to absorb both lines.
    if (k >= 2 &&
        rel[p - 2].trimmed().isEmpty() &&
        !rel[p - 1].trimmed().isEmpty()) {
        return {p - 1, HeuristicId::H7};
    }

    // H2: indent drop in slide-left range.
    if (k > 0) {
        const int indFirst = indentWidth(rel[p]);
        for (int j = 1; j <= k; ++j) {
            if (indentWidth(rel[p - j]) < indFirst)
                return {p - j, HeuristicId::H2};
        }
        if (indentWidth(rel[p - k]) == indFirst) {
            bool allComments = true;
            for (int i = 1; i <= k; ++i) {
                const QString t = rel[p - i].trimmed();
                if (!t.startsWith(QStringLiteral("//")) &&
                    !t.startsWith(QStringLiteral("#"))) {
                    allComments = false; break;
                }
            }
            if (allComments) return {p - k, HeuristicId::H2};
        }
    }

    // H3: single-line section separator.
    if (p > 0 && p + size - 1 < N && rel[p - 1] == rel[p + size - 1]) {
        const QString t = rel[p - 1].trimmed();
        if (t.startsWith(QStringLiteral("/*"))    ||
            t.startsWith(QStringLiteral(";;***")) ||
            t.startsWith(QStringLiteral(";***"))  ||
            t.startsWith(QStringLiteral(";;;;"))  ||
            t.startsWith(QStringLiteral("#---"))) {
            return {p - 1, HeuristicId::H3};
        }
    }

    // H4: #pragma mark / // MARK.
    auto isMark = [](const QString& line) {
        const QString t = line.trimmed();
        return t.startsWith(QStringLiteral("#pragma mark")) ||
               t.startsWith(QStringLiteral("// MARK"))      ||
               t.startsWith(QStringLiteral("@"));
    };
    for (int j = k; j >= 1; --j) {
        if (isMark(rel[p - j])) return {p - j, HeuristicId::H4};
    }
    if (p > 0 && isMark(rel[p - 1])) return {p - 1, HeuristicId::H4};

    return {p, HeuristicId::None};
}

// Phase 2: H1 — always applied after exclusive, independent.
// If the block starts with a homogeneous series of lines (all empty, all
// starting with "#", or all starting with "//") and the same kind of series
// follows immediately after the block, slide forward by min(n, m) so the
// series moves from the block's beginning to its end.
static int applyH1(int p1, int size, const QStringList& rel) {
    const int N = rel.size();
    if (p1 >= N) return p1;

    // Determine series type from the first line of the block.
    // Only three recognised types: blank, exactly "//", exactly "#".
    const QString first = rel[p1].trimmed();
    auto sameType = [&](const QString& line) -> bool {
        const QString t = line.trimmed();
        if (first.isEmpty())
            return t.isEmpty();
        if (first == QStringLiteral("//"))
            return t == QStringLiteral("//");
        if (first == QStringLiteral("#"))
            return t == QStringLiteral("#");
        return false;
    };

    if (!sameType(rel[p1]))  // first line doesn't qualify → H1 never fires
        return p1;

    int n = 0;
    while (n < size && p1 + n < N && sameType(rel[p1 + n])) ++n;
    int m = 0;
    while (p1 + size + m < N && sameType(rel[p1 + size + m])) ++m;
    return p1 + std::min(n, m);
}

HeuristicProbe adjustPosition(int p, int size, const QStringList& rel) {
    if (p < 0 || p >= rel.size() || size < 1)
        return {p, HeuristicId::None, p, false};

    auto [p1, excl] = exclusiveHeuristic(p, size, rel);
    const int p2    = applyH1(p1, size, rel);
    const bool h1   = (p2 != p1);

    return {p2, excl, p1, h1};
}

}  // namespace

HeuristicProbe probeHeuristic(int p, int size, const QStringList& rel) {
    return adjustPosition(p, size, rel);
}

void applySliderHeuristics(std::vector<Hunk>& hunks,
                            const QStringList& left,
                            const QStringList& right) {
    for (Hunk& h : hunks) {
        if (h.type == ChangeType::Insert) {
            const int oldP = h.rightRange.start;
            const int newP = adjustPosition(oldP, h.rightRange.count, right).pos;
            const int delta = newP - oldP;
            h.rightRange.start += delta;
            h.leftRange.start  += delta;
        } else if (h.type == ChangeType::Delete) {
            const int oldP = h.leftRange.start;
            const int newP = adjustPosition(oldP, h.leftRange.count, left).pos;
            const int delta = newP - oldP;
            h.leftRange.start  += delta;
            h.rightRange.start += delta;
        }
    }
}

}  // namespace diffcore
