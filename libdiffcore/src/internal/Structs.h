// Internal data structures used by the diff engine.
// These are implementation details; the public API exposes Hunk instead.

#ifndef DIFFCORE_INTERNAL_STRUCTS_H
#define DIFFCORE_INTERNAL_STRUCTS_H

namespace diffcore::internal {

struct Point {
    Point() = default;
    Point(int x_, int y_) : x(x_), y(y_) {}
    int x = 0;
    int y = 0;
};

enum class EditType {
    Insert,  // Extra element in B (right) not present in A (left)
    Delete,  // Element in A (left) not present in B (right)
    Equal    // Element matches on both sides
};

// Half-open range [start, end) for both X (in A) and Y (in B).
struct Block {
    EditType type;
    int startX;
    int startY;
    int endX;
    int endY;

    Block(EditType t, int sx, int sy, int ex, int ey)
        : type(t), startX(sx), startY(sy), endX(ex), endY(ey) {}
};

}  // namespace diffcore::internal

#endif  // DIFFCORE_INTERNAL_STRUCTS_H
