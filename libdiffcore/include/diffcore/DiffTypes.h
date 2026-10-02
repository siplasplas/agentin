// Public types for libdiffcore.
// These are what users of the library see; internal representations
// (Block, EditType) are translated into these by the adapter.

#ifndef DIFFCORE_DIFFTYPES_H
#define DIFFCORE_DIFFTYPES_H

#include <QStringList>
#include <vector>

namespace diffcore {

enum class ChangeType {
    Equal,    // Line exists identically in both files
    Insert,   // Line only in right (B) file
    Delete,   // Line only in left (A) file
    Replace   // Corresponding regions differ (Delete + Insert paired)
};

// Half-open range of lines: [start, start + count).
// For empty ranges (count == 0), start is the insertion point.
struct LineRange {
    int start = 0;
    int count = 0;

    int end() const { return start + count; }
    bool isEmpty() const { return count == 0; }
};

// One contiguous region of the diff. Adjacent Delete+Insert pairs are
// merged into a single Replace hunk by the adapter (unless disabled).
struct Hunk {
    ChangeType type;
    LineRange leftRange;
    LineRange rightRange;
};

// Options controlling how lines are compared.
// Normalization is applied before interning; original lines are preserved
// for rendering.
struct DiffOptions {
    bool ignoreWhitespace = false;   // Collapse all whitespace to single space
    bool ignoreTrailingWhitespace = false;  // Trim only at line end
    bool ignoreCase = false;
    bool ignoreBlankLines = false;   // Treat blank lines as if they match
    bool ignoreEolStyle = true;      // CRLF == LF (done during line splitting)
    bool mergeReplaceHunks = true;         // Merge adjacent Delete+Insert into Replace
    bool coalesceAdjacentSameType = true;  // Merge consecutive hunks of identical type
    bool applySliderHeuristics    = true;  // Shift Insert/Delete hunks to human-preferred boundaries
};

// Summary statistics, filled alongside hunks.
struct DiffStats {
    int additions = 0;      // Inserted lines
    int deletions = 0;      // Deleted lines
    int modifications = 0;  // Replace hunks (counts as 1 per region)
    int editDistance = 0;   // Raw line-level edit distance from engine
};

// Complete result of comparing two line sequences.
struct DiffResult {
    std::vector<Hunk> hunks;
    DiffStats stats;
    int leftLineCount = 0;
    int rightLineCount = 0;

    bool isIdentical() const { return hunks.empty() ||
        (hunks.size() == 1 && hunks[0].type == ChangeType::Equal); }
};

}  // namespace diffcore

#endif  // DIFFCORE_DIFFTYPES_H
