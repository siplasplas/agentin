#include "diffcore/LineInterner.h"

namespace diffcore {

QString LineInterner::normalize(const QString& line, const DiffOptions& opts) {
    QString key = line;

    if (opts.ignoreTrailingWhitespace && !opts.ignoreWhitespace) {
        // Trim only trailing whitespace; leading whitespace stays.
        qsizetype end = key.size();
        while (end > 0 && key.at(end - 1).isSpace()) --end;
        key.truncate(end);
    }

    if (opts.ignoreWhitespace) {
        // Collapse all runs of whitespace to a single space.
        QString collapsed;
        collapsed.reserve(key.size());
        bool inSpace = false;
        for (const QChar c : key) {
            if (c.isSpace()) {
                if (!inSpace && !collapsed.isEmpty()) {
                    collapsed.append(QLatin1Char(' '));
                }
                inSpace = true;
            } else {
                collapsed.append(c);
                inSpace = false;
            }
        }
        // Trim the trailing space that the loop may have appended just
        // before the final whitespace run terminated the string. Leading
        // whitespace never gets emitted (first space skipped by the
        // !collapsed.isEmpty() guard).
        if (!collapsed.isEmpty() &&
            collapsed.at(collapsed.size() - 1) == QLatin1Char(' ')) {
            collapsed.chop(1);
        }
        key = collapsed;
    }

    if (opts.ignoreCase) {
        key = key.toCaseFolded();
    }

    return key;
}

LineInterner::Result LineInterner::intern(const QStringList& left,
                                          const QStringList& right,
                                          const DiffOptions& opts) {
    Result result;
    result.leftIds.reserve(static_cast<size_t>(left.size()));
    result.rightIds.reserve(static_cast<size_t>(right.size()));

    auto internOne = [&](const QString& line) -> int {
        const QString key = normalize(line, opts);
        auto it = m_dict.find(key);
        if (it != m_dict.end()) return it.value();
        const int id = m_nextId++;
        m_dict.insert(key, id);
        return id;
    };

    for (const QString& line : left) {
        result.leftIds.push_back(internOne(line));
    }
    for (const QString& line : right) {
        result.rightIds.push_back(internOne(line));
    }
    return result;
}

}  // namespace diffcore
