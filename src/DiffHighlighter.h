#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QTextCharFormat>

// Colours the code of a file shown in the changes window with the Kate syntax definitions that qcodeedit reads,
// from qce::kate::dataDir() (~/.local/share/qcodeedit/kate-<version> on Linux), which other editors built on
// qcodeedit share. Without a definition for the file's name, or without the definitions at all, nothing is
// coloured and the diff is shown as before.
namespace DiffHighlighter {

// A run of characters of one line and its colour.
struct Span
{
    int start = 0;
    int length = 0;
    QTextCharFormat format;
};

// The spans of each line of a file's content, empty when nothing is coloured: no definition fits the path,
// the file is too long to colour at once, or a dark window has no dark theme.
QList<QList<Span>> highlight(const QString &path, const QStringList &lines, bool dark);

}
