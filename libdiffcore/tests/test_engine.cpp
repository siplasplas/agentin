// Unit tests for DiffEngine - covers basic cases, edge cases, options,
// and the merge-to-Replace behavior.

#include <QObject>
#include <QStringList>
#include <QTest>

#include "diffcore/DiffEngine.h"

using namespace diffcore;

class TestEngine : public QObject {
    Q_OBJECT

private slots:
    // --- Trivial cases ---

    void identicalFiles() {
        QStringList lines{"alpha", "beta", "gamma"};
        DiffEngine eng;
        DiffResult r = eng.compute(lines, lines);
        QVERIFY(r.isIdentical());
        QCOMPARE(r.stats.additions, 0);
        QCOMPARE(r.stats.deletions, 0);
        QCOMPARE(r.stats.modifications, 0);
    }

    void bothEmpty() {
        DiffEngine eng;
        DiffResult r = eng.compute({}, {});
        QVERIFY(r.hunks.empty());
        QCOMPARE(r.leftLineCount, 0);
        QCOMPARE(r.rightLineCount, 0);
    }

    void leftEmpty() {
        QStringList right{"a", "b", "c"};
        DiffEngine eng;
        DiffResult r = eng.compute({}, right);
        QCOMPARE(int(r.hunks.size()), 1);
        QCOMPARE(int(r.hunks[0].type), int(ChangeType::Insert));
        QCOMPARE(r.hunks[0].rightRange.count, 3);
        QCOMPARE(r.stats.additions, 3);
    }

    void rightEmpty() {
        QStringList left{"a", "b"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, {});
        QCOMPARE(int(r.hunks.size()), 1);
        QCOMPARE(int(r.hunks[0].type), int(ChangeType::Delete));
        QCOMPARE(r.hunks[0].leftRange.count, 2);
        QCOMPARE(r.stats.deletions, 2);
    }

    // --- Basic operations ---

    void singleInsertion() {
        QStringList left{"a", "c"};
        QStringList right{"a", "b", "c"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QCOMPARE(r.stats.additions, 1);
        QCOMPARE(r.stats.deletions, 0);
        QCOMPARE(r.stats.modifications, 0);
    }

    void singleDeletion() {
        QStringList left{"a", "b", "c"};
        QStringList right{"a", "c"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QCOMPARE(r.stats.additions, 0);
        QCOMPARE(r.stats.deletions, 1);
    }

    void singleReplace() {
        // A change in one line should become one Replace hunk.
        QStringList left{"a", "b", "c"};
        QStringList right{"a", "B", "c"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QCOMPARE(r.stats.modifications, 1);
        QCOMPARE(r.stats.additions, 1);
        QCOMPARE(r.stats.deletions, 1);

        // Find the replace hunk and verify its ranges.
        bool found = false;
        for (const Hunk& h : r.hunks) {
            if (h.type == ChangeType::Replace) {
                QCOMPARE(h.leftRange.start, 1);
                QCOMPARE(h.leftRange.count, 1);
                QCOMPARE(h.rightRange.start, 1);
                QCOMPARE(h.rightRange.count, 1);
                found = true;
            }
        }
        QVERIFY(found);
    }

    void mergeDisabledKeepsDeleteAndInsert() {
        QStringList left{"a", "b", "c"};
        QStringList right{"a", "B", "c"};
        DiffOptions opts;
        opts.mergeReplaceHunks = false;
        DiffEngine eng;
        DiffResult r = eng.compute(left, right, opts);

        // With merging off, we expect both Delete and Insert, no Replace.
        int deletes = 0, inserts = 0, replaces = 0;
        for (const Hunk& h : r.hunks) {
            if (h.type == ChangeType::Delete) ++deletes;
            if (h.type == ChangeType::Insert) ++inserts;
            if (h.type == ChangeType::Replace) ++replaces;
        }
        QCOMPARE(replaces, 0);
        QCOMPARE(deletes, 1);
        QCOMPARE(inserts, 1);
    }

    // --- Normalization options ---

    void ignoreCaseMakesEqual() {
        QStringList left{"Hello", "World"};
        QStringList right{"hello", "WORLD"};
        DiffOptions opts;
        opts.ignoreCase = true;
        DiffEngine eng;
        DiffResult r = eng.compute(left, right, opts);
        QVERIFY(r.isIdentical());
    }

    void ignoreWhitespaceCollapsesRuns() {
        QStringList left{"foo  bar", "baz"};
        QStringList right{"foo bar", "baz"};
        DiffOptions opts;
        opts.ignoreWhitespace = true;
        DiffEngine eng;
        DiffResult r = eng.compute(left, right, opts);
        QVERIFY(r.isIdentical());
    }

    void ignoreTrailingWhitespace() {
        QStringList left{"hello   ", "world"};
        QStringList right{"hello", "world"};
        DiffOptions opts;
        opts.ignoreTrailingWhitespace = true;
        DiffEngine eng;
        DiffResult r = eng.compute(left, right, opts);
        QVERIFY(r.isIdentical());
    }

    void caseSensitiveByDefault() {
        QStringList left{"Hello"};
        QStringList right{"hello"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QVERIFY(!r.isIdentical());
    }

    // --- Swap coverage ---

    void leftLongerThanRight() {
        // Internally the engine will swap. Verify left/right stay correct
        // in the output.
        QStringList left{"a", "b", "c", "d", "e", "f"};
        QStringList right{"a", "c", "f"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QCOMPARE(r.stats.deletions, 3);
        QCOMPARE(r.stats.additions, 0);
        // All hunks describing deletes must have right-range count 0
        // and non-empty left-range count.
        for (const Hunk& h : r.hunks) {
            if (h.type == ChangeType::Delete) {
                QVERIFY(h.leftRange.count > 0);
                QCOMPARE(h.rightRange.count, 0);
            }
        }
    }

    void rightLongerThanLeft() {
        QStringList left{"a", "c", "f"};
        QStringList right{"a", "b", "c", "d", "e", "f"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QCOMPARE(r.stats.additions, 3);
        QCOMPARE(r.stats.deletions, 0);
        for (const Hunk& h : r.hunks) {
            if (h.type == ChangeType::Insert) {
                QCOMPARE(h.leftRange.count, 0);
                QVERIFY(h.rightRange.count > 0);
            }
        }
    }

    // --- Sanity: line counts and edit distance ---

    void lineCountsReflectInput() {
        QStringList left{"1", "2", "3", "4", "5"};
        QStringList right{"1", "2"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QCOMPARE(r.leftLineCount, 5);
        QCOMPARE(r.rightLineCount, 2);
    }

    void editDistanceIsExposed() {
        // One changed line in the middle: edit distance should be 2
        // (one delete + one insert) at line level.
        QStringList left{"a", "b", "c"};
        QStringList right{"a", "B", "c"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);
        QCOMPARE(r.stats.editDistance, 2);
    }

    // --- Coalescing adjacent same-type hunks ---

    // Char-by-char input "abc"→"abcXY": O(NP) may emit two Insert{1} blocks.
    // Default coalescing must merge them into one Insert{2}.
    void coalesceAdjacentInserts_producesOneInsertHunk() {
        QStringList left{"a","b","c"};
        QStringList right{"a","b","c","X","Y"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);

        int insertCount = 0, insertTotal = 0;
        for (const auto& h : r.hunks) {
            if (h.type == ChangeType::Insert) {
                ++insertCount;
                insertTotal += h.rightRange.count;
            }
        }
        QCOMPARE(insertCount, 1);
        QCOMPARE(insertTotal, 2);
    }

    // Same input with coalescing disabled: total coverage still correct,
    // but we may get more than one Insert hunk.
    void coalesceDisabled_totalCoveragePreserved() {
        QStringList left{"a","b","c"};
        QStringList right{"a","b","c","X","Y"};
        DiffOptions opts;
        opts.coalesceAdjacentSameType = false;
        DiffEngine eng;
        DiffResult r = eng.compute(left, right, opts);

        int insertTotal = 0;
        for (const auto& h : r.hunks)
            if (h.type == ChangeType::Insert)
                insertTotal += h.rightRange.count;
        QCOMPARE(insertTotal, 2);
    }

    // Adjacent deletes at char level: "XYabc"→"abc" should be one Delete{2}.
    void coalesceAdjacentDeletes_producesOneDeleteHunk() {
        QStringList left{"X","Y","a","b","c"};
        QStringList right{"a","b","c"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);

        int deleteCount = 0, deleteTotal = 0;
        for (const auto& h : r.hunks) {
            if (h.type == ChangeType::Delete) {
                ++deleteCount;
                deleteTotal += h.leftRange.count;
            }
        }
        QCOMPARE(deleteCount, 1);
        QCOMPARE(deleteTotal, 2);
    }

    // With default options, no two adjacent hunks in the output share the same type.
    void coalesceDefault_noAdjacentSameTypeHunks() {
        QStringList left{"a","b","c","d","e"};
        QStringList right{"a","X","Y","d","Z"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);

        for (size_t i = 1; i < r.hunks.size(); ++i)
            QVERIFY(r.hunks[i].type != r.hunks[i-1].type);
    }

    // --- Hunk coverage (hunks span whole file) ---

    void hunksCoverEntireLeftFile() {
        QStringList left{"a", "b", "c", "d", "e"};
        QStringList right{"a", "B", "d", "e", "F"};
        DiffEngine eng;
        DiffResult r = eng.compute(left, right);

        int coveredLeft = 0;
        for (const Hunk& h : r.hunks) {
            coveredLeft += h.leftRange.count;
        }
        QCOMPARE(coveredLeft, r.leftLineCount);

        int coveredRight = 0;
        for (const Hunk& h : r.hunks) {
            coveredRight += h.rightRange.count;
        }
        QCOMPARE(coveredRight, r.rightLineCount);
    }
};

QTEST_APPLESS_MAIN(TestEngine)
#include "test_engine.moc"
