// Maps unique lines to integer IDs so the diff engine can compare sequences
// of ints instead of strings. Normalization (whitespace, case) is applied
// to the KEY used for interning, while the original line is preserved for
// rendering.

#ifndef DIFFCORE_LINEINTERNER_H
#define DIFFCORE_LINEINTERNER_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <vector>

#include "DiffTypes.h"

namespace diffcore {

class LineInterner {
public:
    struct Result {
        std::vector<int> leftIds;   // One ID per line in left file
        std::vector<int> rightIds;  // One ID per line in right file
    };

    // Intern both sequences with the same dictionary so equal (normalized)
    // lines get the same ID on both sides.
    Result intern(const QStringList& left,
                  const QStringList& right,
                  const DiffOptions& opts);

private:
    // Convert a line into its canonical form based on options.
    static QString normalize(const QString& line, const DiffOptions& opts);

    QHash<QString, int> m_dict;
    int m_nextId = 0;
};

}  // namespace diffcore

#endif  // DIFFCORE_LINEINTERNER_H
