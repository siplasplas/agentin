#include "DiffHighlighter.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <qce/RulesHighlighter.h>
#include <qce/kate/KatePaths.h>
#include <qce/kate/KateSyntaxIndex.h>
#include <qce/kate/KateTheme.h>
#include <qce/kate/KateXmlReader.h>

#include <memory>
#include <optional>

namespace DiffHighlighter {

namespace {
// Beyond these a file is shown without colours, so that a long file never holds up the window.
constexpr int kMaximumLines = 50000;
constexpr qsizetype kMaximumCharacters = 4 * 1024 * 1024;

// The index of the definitions, read once per data directory; it lists the languages and resolves ##Name
// includes.
const qce::kate::KateSyntaxIndex *syntaxIndex()
{
    static QHash<QString, std::shared_ptr<qce::kate::KateSyntaxIndex>> indexes;
    const QString directory = qce::kate::dataDir();
    if (directory.isEmpty() || !QFileInfo(qce::kate::syntaxDir()).isDir()) return nullptr;
    std::shared_ptr<qce::kate::KateSyntaxIndex> &index = indexes[directory];
    if (!index) index = std::make_shared<qce::kate::KateSyntaxIndex>(qce::kate::KateSyntaxIndex::load(directory));
    return index.get();
}

// The Breeze theme that matches the window, from the themes beside the definitions. A light window without
// it uses the reader's built-in light colours; a dark one is not coloured, as those would be hard to read.
std::optional<KateTheme> theme(bool dark)
{
    const KateTheme loaded = KateTheme::load(QDir(qce::kate::themesDir()).filePath(dark ? "breeze-dark.theme" : "breeze-light.theme"));
    if (loaded.isValid()) return loaded;
    if (dark) return std::nullopt;
    return KateTheme();
}

// The highlighter of a definition, built once per definition and theme.
std::shared_ptr<qce::RulesHighlighter> highlighterFor(const QString &definition, bool dark)
{
    static QHash<QString, std::shared_ptr<qce::RulesHighlighter>> cache;
    const QString key = definition + (dark ? "|dark" : "|light");
    const auto found = cache.constFind(key);
    if (found != cache.constEnd()) return *found;
    std::shared_ptr<qce::RulesHighlighter> highlighter;
    const qce::kate::KateSyntaxIndex *index = syntaxIndex();
    if (const std::optional<KateTheme> colours = theme(dark); colours && index)
        highlighter = colours->isValid() ? KateXmlReader::load(definition, *colours, *index) : KateXmlReader::load(definition);
    cache.insert(key, highlighter);
    return highlighter;
}

QTextCharFormat format(const qce::TextAttribute &attribute)
{
    // The background stays the diff's, which shows what was added and removed.
    QTextCharFormat result;
    if (attribute.foreground.isValid()) result.setForeground(attribute.foreground);
    if (attribute.bold) result.setFontWeight(QFont::Bold);
    if (attribute.italic) result.setFontItalic(true);
    if (attribute.underline) result.setFontUnderline(true);
    return result;
}
}

QList<QList<Span>> highlight(const QString &path, const QStringList &lines, bool dark)
{
    if (lines.isEmpty() || lines.size() > kMaximumLines) return {};
    qsizetype characters = 0;
    for (const QString &line : lines) characters += line.size();
    if (characters > kMaximumCharacters) return {};
    const qce::kate::KateSyntaxIndex *index = syntaxIndex();
    if (!index) return {};
    const QList<const qce::kate::LanguageEntry *> languages = index->forFileName(path);
    if (languages.isEmpty()) return {};
    const std::shared_ptr<qce::RulesHighlighter> highlighter = highlighterFor(index->filePath(*languages.first()), dark);
    if (!highlighter) return {};
    QList<QTextCharFormat> palette;
    for (const qce::TextAttribute &attribute : highlighter->attributes()) palette.append(format(attribute));
    QList<QList<Span>> result;
    result.reserve(lines.size());
    qce::HighlightState state = highlighter->initialState();
    QVector<qce::StyleSpan> spans;
    for (const QString &line : lines) {
        qce::HighlightState next;
        highlighter->highlightLine(line, state, spans, next);
        state = next;
        QList<Span> coloured;
        for (const qce::StyleSpan &span : spans)
            if (span.length > 0 && span.attributeId >= 0 && span.attributeId < palette.size())
                coloured.append({span.start, span.length, palette.at(span.attributeId)});
        result.append(coloured);
    }
    return result;
}

}
