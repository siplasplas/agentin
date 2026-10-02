// Wu/Manber/Myers O(NP) diff engine - templated version.
// Original algorithm from the paper computes edit distance only;
// this implementation additionally collects visited points during
// the forward phase and reconstructs the main edit branch by
// walking backwards through the collection.
//
// T must support: operator==, and the container (A, B) must expose
// size() and operator[]. Works with std::string, std::vector<T>, etc.

#ifndef DIFFCORE_INTERNAL_DIFF_H
#define DIFFCORE_INTERNAL_DIFF_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <tuple>
#include <vector>

#include "Structs.h"

namespace diffcore::internal {

// Array that supports negative indexing in a fixed range [minIndex, maxIndex].
class RangeArray {
public:
    RangeArray() = default;
    ~RangeArray() = default;

    // Non-copyable, movable by default - we only need one instance per Diff.
    RangeArray(const RangeArray&) = delete;
    RangeArray& operator=(const RangeArray&) = delete;
    RangeArray(RangeArray&&) noexcept = default;
    RangeArray& operator=(RangeArray&&) noexcept = default;

    void setSize(int minIdx, int maxIdx) {
        m_minIndex = minIdx;
        m_data.assign(static_cast<size_t>(maxIdx - minIdx + 1), 0);
    }

    int& operator[](int index) {
        return m_data[static_cast<size_t>(index - m_minIndex)];
    }

    void fill(int value) {
        std::fill(m_data.begin(), m_data.end(), value);
    }

private:
    int m_minIndex = 0;
    std::vector<int> m_data;
};

// Bounds of a counting run; a default instance has none.
struct Limits {
    int maxEditDistance = 0;  // 0: unlimited
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    const std::atomic<bool>* cancel = nullptr;
};

// Why a counting run stopped before the edit distance was known.
enum class Stop { None, TooManyDifferences, TimedOut, Cancelled };

// Core engine. Container type can be std::string, std::vector<int>, etc.
// Element type T is inferred from Container::value_type.
template <typename Container>
class Diff {
public:
    using ValueType = typename Container::value_type;

    Diff(const Container& a, const Container& b) : m_a(a), m_b(b) {
        m_M = static_cast<int>(m_a.size());
        m_N = static_cast<int>(m_b.size());
        if (m_M > m_N) {
            std::swap(m_a, m_b);
            std::swap(m_M, m_N);
            m_swapped = true;
        }
        m_fp.setSize(-(m_M + 1), m_N + 1);
    }

    // Run algorithm and return blocks (edit script).
    std::vector<Block> walk() {
        compare();
        return backtrack();
    }

    // Computes only the edit distance, without recording the path, so memory stays linear. Stops early
    // when a limit is reached; the edit distance is valid only when it returns Stop::None.
    Stop count(const Limits& limits) {
        m_collect = false;
        m_limits = &limits;
        compare();
        m_limits = nullptr;
        m_collect = true;
        return m_stop;
    }

    // Edit distance (Levenshtein-like, only insert/delete ops).
    // Valid after walk() or compare().
    int editDistance() const { return m_editDistance; }

    // True iff internal swap happened (A was longer than B).
    // Users of the adapter should not need to look at this.
    bool swapped() const { return m_swapped; }

    // Accessors for the (possibly swapped) internal sequences.
    const Container& a() const { return m_a; }
    const Container& b() const { return m_b; }
    int sizeA() const { return m_M; }
    int sizeB() const { return m_N; }

private:
    // Follow the diagonal as long as elements match.
    int snake(int k, int y) {
        int x = y - k;
        while (x < m_M && y < m_N && m_a[x] == m_b[y]) {
            ++x;
            ++y;
        }
        return y;
    }

    // Extend one diagonal k, record the step in collection.
    int collect(int k) {
        const int v0 = m_fp[k - 1] + 1;
        const int v1 = m_fp[k + 1];
        // Direction: +1 means we came from k-1 (insert), -1 from k+1 (delete).
        const int dir = (v0 > v1) ? 1 : -1;
        const int maxV = std::max(v0, v1);
        const int sn = snake(k, maxV);
        if (m_collect) m_collection.emplace_back(k, sn, dir);
        // A single value of p can take long on large inputs, so the limits are also checked meanwhile.
        if (m_limits && (++m_steps & 0x3fff) == 0) checkLimits(-1);
        return sn;
    }

    // Records why counting must stop; p < 0 skips the edit distance bound.
    void checkLimits(int p) {
        if (m_stop != Stop::None) return;
        if (m_limits->cancel && m_limits->cancel->load(std::memory_order_relaxed)) m_stop = Stop::Cancelled;
        else if (std::chrono::steady_clock::now() >= m_limits->deadline) m_stop = Stop::TimedOut;
        else if (p >= 0 && m_limits->maxEditDistance > 0 && (m_N - m_M) + 2 * p > m_limits->maxEditDistance)
            m_stop = Stop::TooManyDifferences;
    }

    // Main algorithm loop. Fills m_collection and computes edit distance.
    void compare() {
        const int delta = m_N - m_M;
        m_fp.fill(-1);
        m_stop = Stop::None;
        int p = -1;
        do {
            ++p;
            if (m_limits) {
                checkLimits(p);
                if (m_stop != Stop::None) return;
            }
            for (int k = -p; k <= delta - 1 && m_stop == Stop::None; ++k) {
                m_fp[k] = collect(k);
            }
            for (int k = delta + p; k >= delta + 1 && m_stop == Stop::None; --k) {
                m_fp[k] = collect(k);
            }
            if (m_stop != Stop::None) return;
            m_fp[delta] = collect(delta);
        } while (m_fp[delta] != m_N);
        m_editDistance = delta + 2 * p;
    }

    // Walk m_collection backwards to find the main edit path.
    std::vector<std::tuple<int, int, int>> findMainBranch() {
        std::vector<std::tuple<int, int, int>> mainBranch;
        int nextIdx = std::get<0>(m_collection.back());
        for (int i = static_cast<int>(m_collection.size()) - 1; i >= 0; --i) {
            const auto& tuple = m_collection[i];
            const int k = std::get<0>(tuple);
            if (k != nextIdx) continue;
            const int dir = std::get<2>(tuple);
            assert(dir == 1 || dir == -1);
            nextIdx = k - dir;
            mainBranch.push_back(tuple);
        }
        std::reverse(mainBranch.begin(), mainBranch.end());
        return mainBranch;
    }

    // Convert the main branch into a sequence of Equal/Insert/Delete blocks.
    std::vector<Block> backtrack() {
        auto mainBranch = findMainBranch();
        std::vector<Block> result;
        if (mainBranch.empty()) return result;

        int lastDir = 0;
        int lastX = -1;
        int lastY = -1;
        EditType insDelType = EditType::Equal;
        Point start;

        for (size_t i = 0; i + 1 < mainBranch.size(); ++i) {
            const auto& tuple = mainBranch[i];
            const auto& nextTuple = mainBranch[i + 1];
            const int dir = std::get<2>(nextTuple);
            const int k = std::get<0>(tuple);
            const int y = std::get<1>(tuple);
            assert(y >= 0);
            const int x = y - k;
            assert(x >= 0);

            if (dir != lastDir || y != lastY) {
                if (lastX == -1) {
                    assert(x == y);
                    if (x > 0) {
                        result.emplace_back(EditType::Equal, 0, 0, x, y);
                    }
                } else {
                    result.emplace_back(insDelType, start.x, start.y,
                        lastX + (insDelType == EditType::Delete ? 1 : 0),
                        lastY + (insDelType == EditType::Insert ? 1 : 0));
                    assert(std::abs((x - lastX) - (y - lastY)) == 1);
                    const int eqLen = std::min(x - lastX, y - lastY);
                    if (eqLen > 0) {
                        result.emplace_back(EditType::Equal,
                            x - eqLen, y - eqLen, x, y);
                    }
                }
                insDelType = (dir < 0) ? EditType::Delete : EditType::Insert;
                start.x = x;
                start.y = y;
            }
            lastDir = dir;
            lastX = x;
            lastY = y;
        }

        if (insDelType != EditType::Equal) {
            result.emplace_back(insDelType, start.x, start.y,
                lastX + (insDelType == EditType::Delete ? 1 : 0),
                lastY + (insDelType == EditType::Insert ? 1 : 0));
        }

        const bool endA = (m_M == lastX);
        const bool endB = (m_N == lastY);
        assert(!(endA && endB));
        if (!endA && !endB && lastX >= 0) {
            assert(std::abs((m_M - lastX) - (m_N - lastY)) == 1);
            const int eqLen = std::min(m_M - lastX, m_N - lastY);
            result.emplace_back(EditType::Equal,
                m_M - eqLen, m_N - eqLen, m_M, m_N);
        }
        return result;
    }

    Container m_a;
    Container m_b;
    int m_M = 0;
    int m_N = 0;
    bool m_swapped = false;
    int m_editDistance = 0;
    RangeArray m_fp;
    std::vector<std::tuple<int, int, int>> m_collection;
    bool m_collect = true;
    const Limits* m_limits = nullptr;
    Stop m_stop = Stop::None;
    unsigned m_steps = 0;
};

}  // namespace diffcore::internal

#endif  // DIFFCORE_INTERNAL_DIFF_H
