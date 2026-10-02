// Unit tests for LineInterner - verifies normalization and that equal
// normalized lines get the same ID across both files.

#include <QObject>
#include <QStringList>
#include <QTest>

#include "diffcore/LineInterner.h"

using namespace diffcore;

class TestInterner : public QObject {
    Q_OBJECT

private slots:
    void identicalLinesShareId() {
        LineInterner in;
        auto r = in.intern({"foo", "bar"}, {"foo", "baz"}, {});
        QCOMPARE(r.leftIds.size(), size_t(2));
        QCOMPARE(r.rightIds.size(), size_t(2));
        // "foo" on both sides must share the same ID.
        QCOMPARE(r.leftIds[0], r.rightIds[0]);
        // "bar" vs "baz" must differ.
        QVERIFY(r.leftIds[1] != r.rightIds[1]);
    }

    void distinctLinesGetDistinctIds() {
        LineInterner in;
        auto r = in.intern({"a", "b", "c"}, {}, {});
        QCOMPARE(r.leftIds[0], 0);
        QCOMPARE(r.leftIds[1], 1);
        QCOMPARE(r.leftIds[2], 2);
    }

    void repeatedLineReusesId() {
        LineInterner in;
        auto r = in.intern({"x", "x", "x"}, {}, {});
        QCOMPARE(r.leftIds[0], r.leftIds[1]);
        QCOMPARE(r.leftIds[1], r.leftIds[2]);
    }

    void ignoreCase() {
        DiffOptions opts;
        opts.ignoreCase = true;
        LineInterner in;
        auto r = in.intern({"Hello"}, {"HELLO"}, opts);
        QCOMPARE(r.leftIds[0], r.rightIds[0]);
    }

    void ignoreWhitespaceCollapses() {
        DiffOptions opts;
        opts.ignoreWhitespace = true;
        LineInterner in;
        // "foo  bar" with collapsed whitespace equals "foo bar".
        auto r = in.intern({"foo  bar"}, {"foo bar"}, opts);
        QCOMPARE(r.leftIds[0], r.rightIds[0]);
    }

    void ignoreTrailingWhitespace() {
        DiffOptions opts;
        opts.ignoreTrailingWhitespace = true;
        LineInterner in;
        auto r = in.intern({"hello   "}, {"hello"}, opts);
        QCOMPARE(r.leftIds[0], r.rightIds[0]);
    }

    void ignoreTrailingDoesNotAffectLeading() {
        DiffOptions opts;
        opts.ignoreTrailingWhitespace = true;
        LineInterner in;
        auto r = in.intern({"   hello"}, {"hello"}, opts);
        QVERIFY(r.leftIds[0] != r.rightIds[0]);
    }

    void caseSensitiveByDefault() {
        LineInterner in;
        auto r = in.intern({"Hello"}, {"hello"}, {});
        QVERIFY(r.leftIds[0] != r.rightIds[0]);
    }

    void emptyLinesAreInterned() {
        // Empty lines should behave like any other lines.
        LineInterner in;
        auto r = in.intern({"", "x", ""}, {"", "x"}, {});
        QCOMPARE(r.leftIds[0], r.leftIds[2]);
        QCOMPARE(r.leftIds[0], r.rightIds[0]);
    }
};

QTEST_APPLESS_MAIN(TestInterner)
#include "test_interner.moc"
