#include "shell/ShellAst.h"

#include <QRegularExpression>

#include <optional>
#include <utility>

// A recursive-descent parser that reads characters directly: Bash recognises reserved words only where a
// command starts, and reads the body of a here-document only after the end of the line that opens it, so a
// separate tokenizer would need the parser's state anyway.
namespace shell {

namespace {

constexpr int kMaximumDepth = 16;

struct ParseError
{
    QString reason;
};

bool isBlank(QChar c)
{
    return c == ' ' || c == '\t';
}

// Characters that end an unquoted word.
bool isMeta(QChar c)
{
    return c.isNull() || isBlank(c) || c == '\n' || c == '|' || c == '&' || c == ';' || c == '<' || c == '>'
        || c == '(' || c == ')';
}

bool isNameStart(QChar c)
{
    return c.isLetter() || c == '_';
}

bool isNameChar(QChar c)
{
    return c.isLetterOrNumber() || c == '_';
}

const QStringList &terminatorWords()
{
    static const QStringList words{"then", "elif", "else", "fi", "do", "done", "esac", "}", "in"};
    return words;
}

class Parser
{
public:
    Parser(const QString &text, int depth) : s_(text), depth_(depth) {}

    NodePtr parseAll()
    {
        if (depth_ > kMaximumDepth) throw ParseError{"substitutions nested too deeply"};
        NodePtr list = parseList({}, false);
        skipBlanks();
        if (!atEnd()) throw ParseError{QString("unexpected '%1'").arg(peek())};
        if (!pending_.isEmpty()) throw ParseError{"here-document without its body"};
        list->source = s_;
        return list;
    }

private:
    struct PendingHere
    {
        NodePtr node;
        int index;
        QString delimiter;
        bool stripTabs;
    };

    QString s_;
    int pos_ = 0;
    int depth_;
    QList<PendingHere> pending_;

    bool atEnd() const { return pos_ >= s_.size(); }
    QChar peek(int offset = 0) const { return pos_ + offset < s_.size() ? s_.at(pos_ + offset) : QChar(); }
    bool startsWith(const QString &text) const { return QStringView(s_).mid(pos_).startsWith(text); }

    NodePtr make(Node::Kind kind, int start) const
    {
        auto node = std::make_shared<Node>();
        node->kind = kind;
        node->source = s_.mid(start, pos_ - start).trimmed();
        return node;
    }

    // Blanks, line continuations and comments; newlines are left to the caller.
    void skipBlanks()
    {
        while (!atEnd()) {
            if (isBlank(peek())) {
                ++pos_;
            } else if (peek() == '\\' && peek(1) == '\n') {
                pos_ += 2;
            } else if (peek() == '#') {
                while (!atEnd() && peek() != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    // A newline ends the lines of pending here-documents, whose bodies follow it.
    void consumeNewline()
    {
        ++pos_;
        readHereDocuments();
    }

    void skipBlanksAndNewlines()
    {
        for (;;) {
            skipBlanks();
            if (peek() != '\n') return;
            consumeNewline();
        }
    }

    void readHereDocuments()
    {
        const QList<PendingHere> pending = std::exchange(pending_, {});
        for (const PendingHere &here : pending) {
            QStringList lines;
            bool closed = false;
            while (!atEnd()) {
                int end = s_.indexOf('\n', pos_);
                if (end < 0) end = s_.size();
                QString line = s_.mid(pos_, end - pos_);
                pos_ = qMin(end + 1, int(s_.size()));
                if (here.stripTabs) {
                    while (line.startsWith('\t')) line.remove(0, 1);
                }
                if (line == here.delimiter) {
                    closed = true;
                    break;
                }
                lines.append(line);
            }
            if (!closed) throw ParseError{"here-document without its delimiter " + here.delimiter};
            here.node->redirections[here.index].hereText = lines.join('\n');
        }
    }

    // The next token as plain text when it is an unquoted word without expansions, as reserved words are.
    QString peekPlainWord() const
    {
        int end = pos_;
        while (end < s_.size() && !isMeta(s_.at(end)) && s_.at(end) != '\'' && s_.at(end) != '"'
               && s_.at(end) != '\\' && s_.at(end) != '$' && s_.at(end) != '`')
            ++end;
        if (end == pos_) return {};
        if (end < s_.size() && !isMeta(s_.at(end))) return {};
        return s_.mid(pos_, end - pos_);
    }

    void expectWord(const QString &word)
    {
        skipBlanksAndNewlines();
        if (peekPlainWord() != word) throw ParseError{"expected " + word};
        pos_ += word.size();
    }

    // Commands separated by ;, &, newlines, && and ||, up to a stop word at a command's start, a closing
    // parenthesis when inside one, or the end.
    NodePtr parseList(const QStringList &stopWords, bool stopAtParenthesis)
    {
        const int start = pos_;
        auto list = std::make_shared<Node>();
        list->kind = Node::Kind::List;
        for (;;) {
            skipBlanksAndNewlines();
            if (atEnd()) break;
            if (stopAtParenthesis && peek() == ')') break;
            const QString word = peekPlainWord();
            if (stopWords.contains(word)) break;
            if (terminatorWords().contains(word)) throw ParseError{"unexpected " + word};
            list->children.append(parsePipeline());
            skipBlanks();
            QString separator;
            if (startsWith("&&") || startsWith("||")) {
                separator = s_.mid(pos_, 2);
                pos_ += 2;
                list->separators.append(separator);
                skipBlanksAndNewlines();
                continue;
            }
            if (startsWith(";;")) throw ParseError{"case is not supported"};
            if (peek() == ';' || peek() == '&') {
                separator = peek();
                ++pos_;
            } else if (peek() == '\n') {
                separator = ";";
                consumeNewline();
            }
            list->separators.append(separator);
            if (separator.isEmpty()) break;
        }
        if (list->children.isEmpty()) throw ParseError{"empty command"};
        if (!list->separators.isEmpty() && (list->separators.last() == "&&" || list->separators.last() == "||"))
            throw ParseError{"command missing after " + list->separators.last()};
        while (list->separators.size() < list->children.size()) list->separators.append(QString());
        list->source = s_.mid(start, pos_ - start).trimmed();
        return list;
    }

    NodePtr parsePipeline()
    {
        const int start = pos_;
        skipBlanks();
        bool negated = false;
        for (;;) {
            const QString word = peekPlainWord();
            if (word == "!") {
                negated = !negated;
            } else if (word != "time") {
                break;
            }
            pos_ += word.size();
            skipBlanks();
        }
        NodePtr first = parseCommand();
        skipBlanks();
        if (!(peek() == '|' && peek(1) != '|') && !negated) return first;
        auto pipeline = std::make_shared<Node>();
        pipeline->kind = Node::Kind::Pipeline;
        pipeline->negated = negated;
        pipeline->children.append(first);
        while (peek() == '|' && peek(1) != '|') {
            const QString separator = peek(1) == '&' ? QString("|&") : QString("|");
            pos_ += separator.size();
            pipeline->separators.append(separator);
            skipBlanksAndNewlines();
            pipeline->children.append(parseCommand());
            skipBlanks();
        }
        pipeline->source = s_.mid(start, pos_ - start).trimmed();
        return pipeline;
    }

    NodePtr parseCommand()
    {
        skipBlanks();
        const int start = pos_;
        if (startsWith("((")) throw ParseError{"arithmetic commands are not supported"};
        if (peek() == '(') {
            ++pos_;
            NodePtr body = parseList({}, true);
            skipBlanksAndNewlines();
            if (peek() != ')') throw ParseError{"unclosed subshell"};
            ++pos_;
            NodePtr node = make(Node::Kind::Subshell, start);
            node->children.append(body);
            parseTrailingRedirections(node);
            node->source = s_.mid(start, pos_ - start).trimmed();
            return node;
        }
        const QString word = peekPlainWord();
        if (word == "{") {
            ++pos_;
            NodePtr body = parseList({"}"}, false);
            expectWord("}");
            NodePtr node = make(Node::Kind::Group, start);
            node->children.append(body);
            parseTrailingRedirections(node);
            node->source = s_.mid(start, pos_ - start).trimmed();
            return node;
        }
        if (word == "for") return parseFor(start);
        if (word == "if") return parseIf(start);
        if (word == "while" || word == "until") return parseWhile(start, word);
        if (word == "[[") return parseTest(start);
        if (word == "case" || word == "select" || word == "function" || word == "coproc")
            throw ParseError{word + " is not supported"};
        return parseSimple(start);
    }

    NodePtr parseFor(int start)
    {
        pos_ += 3;
        skipBlanks();
        if (startsWith("((")) throw ParseError{"arithmetic for loops are not supported"};
        const int nameStart = pos_;
        while (isNameChar(peek())) ++pos_;
        const QString name = s_.mid(nameStart, pos_ - nameStart);
        if (name.isEmpty() || !isNameStart(name.front())) throw ParseError{"for without a variable"};
        NodePtr node = make(Node::Kind::For, start);
        node->variable = name;
        skipBlanksAndNewlines();
        if (peekPlainWord() == "in") {
            pos_ += 2;
            node->forHasList = true;
            for (;;) {
                skipBlanks();
                if (atEnd() || peek() == ';' || peek() == '\n') break;
                if (isMeta(peek())) throw ParseError{"unexpected character in a for list"};
                node->words.append(readWord());
            }
        }
        skipBlanks();
        if (peek() == ';') ++pos_;
        expectWord("do");
        node->children.append(parseList({"done"}, false));
        expectWord("done");
        parseTrailingRedirections(node);
        node->source = s_.mid(start, pos_ - start).trimmed();
        return node;
    }

    NodePtr parseIf(int start)
    {
        pos_ += 2;
        NodePtr node = make(Node::Kind::If, start);
        node->children.append(parseList({"then"}, false));
        expectWord("then");
        node->children.append(parseList({"elif", "else", "fi"}, false));
        for (;;) {
            skipBlanksAndNewlines();
            const QString word = peekPlainWord();
            if (word == "elif") {
                pos_ += 4;
                node->children.append(parseList({"then"}, false));
                expectWord("then");
                node->children.append(parseList({"elif", "else", "fi"}, false));
            } else if (word == "else") {
                pos_ += 4;
                node->children.append(parseList({"fi"}, false));
                expectWord("fi");
                break;
            } else {
                expectWord("fi");
                break;
            }
        }
        parseTrailingRedirections(node);
        node->source = s_.mid(start, pos_ - start).trimmed();
        return node;
    }

    NodePtr parseWhile(int start, const QString &word)
    {
        pos_ += word.size();
        NodePtr node = make(word == "while" ? Node::Kind::While : Node::Kind::Until, start);
        node->children.append(parseList({"do"}, false));
        expectWord("do");
        node->children.append(parseList({"done"}, false));
        expectWord("done");
        parseTrailingRedirections(node);
        node->source = s_.mid(start, pos_ - start).trimmed();
        return node;
    }

    // [[ … ]] only tests; its operators, such as && and <, are not the shell's, so it is read as a whole.
    NodePtr parseTest(int start)
    {
        pos_ += 2;
        QChar quote;
        while (!atEnd()) {
            const QChar c = peek();
            if (!quote.isNull()) {
                if (c == '\\' && quote == '"') ++pos_;
                else if (c == quote) quote = QChar();
                ++pos_;
                continue;
            }
            if (c == '\'' || c == '"') {
                quote = c;
                ++pos_;
                continue;
            }
            if (c == '\\') {
                pos_ += 2;
                continue;
            }
            if (c == '$' && (peek(1) == '(' || peek(1) == '{')) throw ParseError{"expansions in [[ ]] are not supported"};
            if (c == '`') throw ParseError{"expansions in [[ ]] are not supported"};
            if (startsWith("]]") && (pos_ + 2 >= s_.size() || isMeta(peek(2))) && isBlank(s_.at(pos_ - 1))) {
                pos_ += 2;
                NodePtr node = make(Node::Kind::Test, start);
                parseTrailingRedirections(node);
                node->source = s_.mid(start, pos_ - start).trimmed();
                return node;
            }
            ++pos_;
        }
        throw ParseError{"unclosed [["};
    }

    void parseTrailingRedirections(const NodePtr &node)
    {
        for (;;) {
            skipBlanks();
            if (!startsRedirection()) return;
            parseRedirection(node);
        }
    }

    bool startsRedirection() const
    {
        int at = pos_;
        while (at < s_.size() && s_.at(at).isDigit()) ++at;
        if (at >= s_.size()) return false;
        const QChar c = s_.at(at);
        if (c == '<' || c == '>') return !(at + 1 < s_.size() && s_.at(at + 1) == '(');
        return at == pos_ && c == '&' && at + 1 < s_.size() && s_.at(at + 1) == '>';
    }

    void parseRedirection(const NodePtr &node)
    {
        Redirection redirection;
        const int digits = pos_;
        while (peek().isDigit()) ++pos_;
        if (pos_ > digits) redirection.fd = s_.mid(digits, pos_ - digits).toInt();
        static const QList<std::pair<QString, Redirection::Kind>> operators{
            {"&>>", Redirection::Kind::AppendOutputAndError}, {"<<<", Redirection::Kind::HereString},
            {"<<-", Redirection::Kind::HereDocument},         {"&>", Redirection::Kind::OutputAndError},
            {">>", Redirection::Kind::Append},                 {">|", Redirection::Kind::Clobber},
            {">&", Redirection::Kind::DuplicateOutput},        {"<&", Redirection::Kind::DuplicateInput},
            {"<<", Redirection::Kind::HereDocument},           {"<>", Redirection::Kind::ReadWrite},
            {">", Redirection::Kind::Output},                  {"<", Redirection::Kind::Input}};
        QString op;
        for (const auto &[text, kind] : operators) {
            if (startsWith(text)) {
                op = text;
                redirection.kind = kind;
                break;
            }
        }
        if (op.isEmpty()) throw ParseError{"unknown redirection"};
        pos_ += op.size();
        skipBlanks();
        if (isMeta(peek())) throw ParseError{"redirection without a target"};
        const int targetStart = pos_;
        redirection.target = readWord();
        if (redirection.kind == Redirection::Kind::HereDocument) {
            const QString raw = s_.mid(targetStart, pos_ - targetStart);
            redirection.hereQuoted = raw.contains('\'') || raw.contains('"') || raw.contains('\\');
            QString delimiter = raw;
            delimiter.remove('\'').remove('"').remove('\\');
            node->redirections.append(redirection);
            pending_.append({node, int(node->redirections.size()) - 1, delimiter, op == "<<-"});
            return;
        }
        node->redirections.append(redirection);
    }

    NodePtr parseSimple(int start)
    {
        NodePtr node = make(Node::Kind::Simple, start);
        for (;;) {
            skipBlanks();
            const QChar c = peek();
            if (c.isNull() || c == '\n' || c == ';' || c == '|' || c == ')') break;
            if (c == '&' && peek(1) != '>') break;
            if (startsRedirection()) {
                parseRedirection(node);
                continue;
            }
            if (c == '(') {
                if (node->words.size() == 1 && node->assignments.isEmpty()) throw ParseError{"functions are not supported"};
                throw ParseError{"unexpected ("};
            }
            const int wordStart = pos_;
            Word word = readWord();
            if (node->words.isEmpty()) {
                static const QRegularExpression assignment("^([A-Za-z_][A-Za-z0-9_]*)\\+?=");
                const QRegularExpressionMatch match = assignment.match(word.source);
                if (match.hasMatch() && !word.parts.isEmpty() && word.parts.first().kind == WordPart::Kind::Literal
                    && word.parts.first().text.startsWith(match.captured(0))) {
                    if (peek() == '(' && pos_ - wordStart == match.capturedLength(0))
                        throw ParseError{"array assignments are not supported"};
                    node->assignments.append({match.captured(1), valueAfter(word, match.capturedLength(0))});
                    continue;
                }
            }
            node->words.append(word);
        }
        node->source = s_.mid(start, pos_ - start).trimmed();
        if (node->words.isEmpty() && node->assignments.isEmpty() && node->redirections.isEmpty())
            throw ParseError{"empty command"};
        return node;
    }

    // The value of NAME=value: the word without its first characters.
    static Word valueAfter(const Word &word, int length)
    {
        Word value;
        value.source = word.source.mid(length);
        value.parts = word.parts;
        WordPart &first = value.parts.first();
        first.text = first.text.mid(length);
        if (first.text.isEmpty()) value.parts.removeFirst();
        return value;
    }

    // An unquoted word up to the next blank or operator.
    Word readWord()
    {
        Word word;
        const int start = pos_;
        QString literal;
        bool glob = false;
        const auto flush = [&] {
            if (literal.isEmpty()) return;
            WordPart part;
            part.kind = WordPart::Kind::Literal;
            part.text = literal;
            part.glob = glob;
            word.parts.append(part);
            literal.clear();
            glob = false;
        };
        if (peek() == '~') {
            ++pos_;
            const int userStart = pos_;
            while (!isMeta(peek()) && peek() != '/' && peek() != '\'' && peek() != '"' && peek() != '$') ++pos_;
            WordPart part;
            part.kind = WordPart::Kind::Tilde;
            part.text = s_.mid(userStart, pos_ - userStart);
            word.parts.append(part);
        }
        while (!atEnd()) {
            const QChar c = peek();
            if ((c == '<' || c == '>') && peek(1) == '(' ) {
                flush();
                word.parts.append(readProcessSubstitution());
                continue;
            }
            if (isMeta(c)) break;
            if (c == '\\') {
                if (peek(1) == '\n') {
                    pos_ += 2;
                    continue;
                }
                if (pos_ + 1 >= s_.size()) throw ParseError{"backslash at the end"};
                literal += peek(1);
                pos_ += 2;
                continue;
            }
            if (c == '\'') {
                flush();
                const int end = s_.indexOf('\'', pos_ + 1);
                if (end < 0) throw ParseError{"unclosed '"};
                WordPart part;
                part.kind = WordPart::Kind::SingleQuoted;
                part.text = s_.mid(pos_ + 1, end - pos_ - 1);
                word.parts.append(part);
                pos_ = end + 1;
                continue;
            }
            if (c == '"') {
                flush();
                word.parts.append(readDoubleQuoted());
                continue;
            }
            if (c == '$' || c == '`') {
                const std::optional<WordPart> part = readExpansion(false);
                if (part) {
                    flush();
                    word.parts.append(*part);
                } else {
                    literal += c;
                    ++pos_;
                }
                continue;
            }
            if (c == '*' || c == '?' || c == '[') glob = true;
            literal += c;
            ++pos_;
        }
        flush();
        word.source = s_.mid(start, pos_ - start);
        if (word.source.isEmpty()) throw ParseError{QString("unexpected '%1'").arg(peek())};
        return word;
    }

    WordPart readDoubleQuoted()
    {
        ++pos_;
        WordPart quoted;
        quoted.kind = WordPart::Kind::DoubleQuoted;
        QString literal;
        const auto flush = [&] {
            if (literal.isEmpty()) return;
            WordPart part;
            part.kind = WordPart::Kind::Literal;
            part.text = literal;
            quoted.parts.append(part);
            literal.clear();
        };
        for (;;) {
            if (atEnd()) throw ParseError{"unclosed \""};
            const QChar c = peek();
            if (c == '"') {
                ++pos_;
                break;
            }
            if (c == '\\') {
                const QChar next = peek(1);
                if (next == '\n') {
                    pos_ += 2;
                    continue;
                }
                if (next == '$' || next == '`' || next == '"' || next == '\\') {
                    literal += next;
                    pos_ += 2;
                    continue;
                }
                literal += c;
                ++pos_;
                continue;
            }
            if (c == '$' || c == '`') {
                const std::optional<WordPart> part = readExpansion(true);
                if (part) {
                    flush();
                    quoted.parts.append(*part);
                } else {
                    literal += c;
                    ++pos_;
                }
                continue;
            }
            literal += c;
            ++pos_;
        }
        flush();
        return quoted;
    }

    // $NAME, ${…}, $(…), $((…)), $'…', $"…" and `…`; nothing for a $ that starts none of them.
    std::optional<WordPart> readExpansion(bool quoted)
    {
        WordPart part;
        if (peek() == '`') {
            const int end = findBacktickEnd(pos_ + 1);
            QString inner = s_.mid(pos_ + 1, end - pos_ - 1);
            inner.replace("\\`", "`").replace("\\$", "$").replace("\\\\", "\\");
            pos_ = end + 1;
            part.kind = WordPart::Kind::CommandSubstitution;
            part.command = parseNested(inner);
            return part;
        }
        const QChar next = peek(1);
        if (next == '(' && peek(2) == '(') {
            const int end = findArithmeticEnd(pos_ + 3);
            part.kind = WordPart::Kind::Opaque;
            part.text = s_.mid(pos_, end - pos_);
            pos_ = end;
            return part;
        }
        if (next == '(') {
            const int end = findParenthesisEnd(pos_ + 2);
            part.kind = WordPart::Kind::CommandSubstitution;
            part.command = parseNested(s_.mid(pos_ + 2, end - pos_ - 2));
            pos_ = end + 1;
            return part;
        }
        if (next == '{') {
            const int end = findBraceEnd(pos_ + 2);
            const QString content = s_.mid(pos_ + 2, end - pos_ - 2);
            static const QRegularExpression plain("^(?:[A-Za-z_][A-Za-z0-9_]*(?:\\[[^\\]]*\\])?|[0-9]+|[?#@*$!-])$");
            if (plain.match(content).hasMatch()) {
                part.kind = WordPart::Kind::Variable;
                part.text = content;
            } else {
                part.kind = WordPart::Kind::Opaque;
                part.text = s_.mid(pos_, end + 1 - pos_);
            }
            pos_ = end + 1;
            return part;
        }
        if (!quoted && next == '\'') {
            int end = pos_ + 2;
            while (end < s_.size() && s_.at(end) != '\'') end += s_.at(end) == '\\' ? 2 : 1;
            if (end >= s_.size()) throw ParseError{"unclosed $'"};
            part.kind = WordPart::Kind::SingleQuoted;
            part.text = s_.mid(pos_ + 2, end - pos_ - 2);
            pos_ = end + 1;
            return part;
        }
        if (!quoted && next == '"') {
            ++pos_;
            return readDoubleQuoted();
        }
        if (isNameStart(next)) {
            int end = pos_ + 1;
            while (end < s_.size() && isNameChar(s_.at(end))) ++end;
            part.kind = WordPart::Kind::Variable;
            part.text = s_.mid(pos_ + 1, end - pos_ - 1);
            pos_ = end;
            return part;
        }
        if (next.isDigit() || QString("?#@*$!-").contains(next)) {
            part.kind = WordPart::Kind::Variable;
            part.text = next;
            pos_ += 2;
            return part;
        }
        return std::nullopt;
    }

    WordPart readProcessSubstitution()
    {
        const int end = findParenthesisEnd(pos_ + 2);
        WordPart part;
        part.kind = WordPart::Kind::Opaque;
        part.text = s_.mid(pos_, end + 1 - pos_);
        part.command = parseNested(s_.mid(pos_ + 2, end - pos_ - 2));
        pos_ = end + 1;
        return part;
    }

    NodePtr parseNested(const QString &text) const
    {
        Parser nested(text, depth_ + 1);
        return nested.parseAll();
    }

    int findBacktickEnd(int from) const
    {
        for (int at = from; at < s_.size(); ++at) {
            if (s_.at(at) == '\\') ++at;
            else if (s_.at(at) == '`') return at;
        }
        throw ParseError{"unclosed `"};
    }

    int findBraceEnd(int from) const
    {
        int depth = 1;
        QChar quote;
        for (int at = from; at < s_.size(); ++at) {
            const QChar c = s_.at(at);
            if (!quote.isNull()) {
                if (c == '\\' && quote == '"') ++at;
                else if (c == quote) quote = QChar();
                continue;
            }
            if (c == '\\') ++at;
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) return at;
        }
        throw ParseError{"unclosed ${"};
    }

    int findArithmeticEnd(int from) const
    {
        int depth = 2;
        for (int at = from; at < s_.size(); ++at) {
            if (s_.at(at) == '(') ++depth;
            else if (s_.at(at) == ')' && --depth == 0) return at + 1;
        }
        throw ParseError{"unclosed $(("};
    }

    // The ) that closes a $( or ( opened just before from, skipping quotes, nested substitutions, comments
    // and the bodies of here-documents, which may hold any text.
    int findParenthesisEnd(int from) const
    {
        int depth = 1;
        QChar quote;
        QStringList delimiters;
        bool wordStart = true;
        for (int at = from; at < s_.size(); ++at) {
            const QChar c = s_.at(at);
            if (!quote.isNull()) {
                if (c == '\\' && quote != '\'') ++at;
                else if (c == quote) quote = QChar();
                continue;
            }
            if (c == '\n' && !delimiters.isEmpty()) {
                int line = at + 1;
                for (const QString &delimiter : std::exchange(delimiters, {})) {
                    for (;;) {
                        int end = s_.indexOf('\n', line);
                        if (end < 0) end = s_.size();
                        QString text = s_.mid(line, end - line);
                        while (text.startsWith('\t')) text.remove(0, 1);
                        line = end + 1;
                        if (text == delimiter || end >= s_.size()) break;
                    }
                }
                at = line - 2;
                wordStart = true;
                continue;
            }
            if (c == '\\') {
                ++at;
                wordStart = false;
                continue;
            }
            if (c == '\'' || c == '"' || c == '`') {
                quote = c;
                wordStart = false;
                continue;
            }
            if (c == '#' && wordStart) {
                while (at + 1 < s_.size() && s_.at(at + 1) != '\n') ++at;
                continue;
            }
            if (c == '<' && at + 1 < s_.size() && s_.at(at + 1) == '<' && !(at + 2 < s_.size() && s_.at(at + 2) == '<')) {
                static const QRegularExpression here("^<<-?[ \\t]*(['\"]?)([^\\s'\"<>|&;()]+)\\1");
                const QRegularExpressionMatch match = here.matchView(QStringView(s_).mid(at));
                if (match.hasMatch()) {
                    delimiters.append(match.captured(2));
                    at += match.capturedLength(0) - 1;
                    wordStart = false;
                    continue;
                }
            }
            if (c == '(') ++depth;
            else if (c == ')' && --depth == 0) return at;
            wordStart = isMeta(c);
        }
        throw ParseError{"unclosed $("};
    }
};

QString dumpWord(const Word &word);

QString dumpParts(const QList<WordPart> &parts)
{
    QString text;
    for (const WordPart &part : parts) {
        switch (part.kind) {
        case WordPart::Kind::Literal: text += part.text; break;
        case WordPart::Kind::SingleQuoted: text += "'" + part.text + "'"; break;
        case WordPart::Kind::DoubleQuoted: text += "\"" + dumpParts(part.parts) + "\""; break;
        case WordPart::Kind::Variable: text += "${" + part.text + "}"; break;
        case WordPart::Kind::CommandSubstitution: text += "$(" + dumpTree(part.command) + ")"; break;
        case WordPart::Kind::Tilde: text += "~" + part.text; break;
        case WordPart::Kind::Opaque:
            text += "[opaque " + part.text + (part.command ? " " + dumpTree(part.command) : QString()) + "]";
            break;
        }
    }
    return text;
}

QString dumpWord(const Word &word)
{
    return dumpParts(word.parts);
}

QString redirectionOperator(Redirection::Kind kind)
{
    switch (kind) {
    case Redirection::Kind::Output: return ">";
    case Redirection::Kind::Clobber: return ">|";
    case Redirection::Kind::Append: return ">>";
    case Redirection::Kind::Input: return "<";
    case Redirection::Kind::ReadWrite: return "<>";
    case Redirection::Kind::OutputAndError: return "&>";
    case Redirection::Kind::AppendOutputAndError: return "&>>";
    case Redirection::Kind::DuplicateOutput: return ">&";
    case Redirection::Kind::DuplicateInput: return "<&";
    case Redirection::Kind::HereDocument: return "<<";
    case Redirection::Kind::HereString: return "<<<";
    }
    return "?";
}

QString dumpRedirection(const Redirection &redirection)
{
    QString text = (redirection.fd >= 0 ? QString::number(redirection.fd) : QString()) + redirectionOperator(redirection.kind)
        + dumpWord(redirection.target);
    if (redirection.kind == Redirection::Kind::HereDocument)
        text += "{" + QString(redirection.hereText).replace('\n', "\\n") + "}";
    return text;
}

void appendRedirections(QStringList &items, const NodePtr &node)
{
    for (const Redirection &redirection : node->redirections) items.append(dumpRedirection(redirection));
}

bool partsHaveUnsupported(const QList<WordPart> &parts);

bool wordHasUnsupported(const Word &word)
{
    return partsHaveUnsupported(word.parts);
}

bool partsHaveUnsupported(const QList<WordPart> &parts)
{
    for (const WordPart &part : parts) {
        if (part.command && hasUnsupported(part.command)) return true;
        if (partsHaveUnsupported(part.parts)) return true;
    }
    return false;
}

}

bool Word::isLiteral() const
{
    for (const WordPart &part : parts) {
        if (part.kind == WordPart::Kind::Literal && !part.glob) continue;
        if (part.kind == WordPart::Kind::SingleQuoted) continue;
        if (part.kind == WordPart::Kind::DoubleQuoted) {
            for (const WordPart &inner : part.parts)
                if (inner.kind != WordPart::Kind::Literal) return false;
            continue;
        }
        return false;
    }
    return true;
}

QString Word::literalText() const
{
    QString text;
    for (const WordPart &part : parts) {
        if (part.kind == WordPart::Kind::DoubleQuoted) {
            for (const WordPart &inner : part.parts) text += inner.text;
        } else {
            text += part.text;
        }
    }
    return text;
}

NodePtr parseBash(const QString &text)
{
    try {
        Parser parser(text, 0);
        return parser.parseAll();
    } catch (const ParseError &error) {
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Unsupported;
        node->reason = error.reason;
        node->source = text;
        return node;
    }
}

QString dumpTree(const NodePtr &node)
{
    if (!node) return "(null)";
    QStringList items;
    switch (node->kind) {
    case Node::Kind::List:
        if (node->children.size() == 1 && node->separators.value(0).isEmpty()) return dumpTree(node->children.first());
        for (int i = 0; i < node->children.size(); ++i) {
            items.append(dumpTree(node->children.at(i)));
            if (!node->separators.value(i).isEmpty()) items.append(node->separators.at(i));
        }
        return "(list " + items.join(' ') + ")";
    case Node::Kind::Pipeline:
        if (node->negated) items.append("!");
        for (int i = 0; i < node->children.size(); ++i) {
            if (i > 0) items.append(node->separators.value(i - 1));
            items.append(dumpTree(node->children.at(i)));
        }
        return "(pipe " + items.join(' ') + ")";
    case Node::Kind::Simple:
        for (const Assignment &assignment : node->assignments) items.append(assignment.name + "=" + dumpWord(assignment.value));
        for (const Word &word : node->words) items.append(dumpWord(word));
        appendRedirections(items, node);
        return "(cmd " + items.join(' ') + ")";
    case Node::Kind::Subshell:
    case Node::Kind::Group:
        items.append(dumpTree(node->children.value(0)));
        appendRedirections(items, node);
        return QString(node->kind == Node::Kind::Subshell ? "(subshell " : "(group ") + items.join(' ') + ")";
    case Node::Kind::For:
        items.append(node->variable);
        if (node->forHasList) {
            items.append("in");
            for (const Word &word : node->words) items.append(dumpWord(word));
        }
        items.append(dumpTree(node->children.value(0)));
        appendRedirections(items, node);
        return "(for " + items.join(' ') + ")";
    case Node::Kind::If:
        for (const NodePtr &child : node->children) items.append(dumpTree(child));
        appendRedirections(items, node);
        return "(if " + items.join(' ') + ")";
    case Node::Kind::While:
    case Node::Kind::Until:
        items.append(dumpTree(node->children.value(0)));
        items.append(dumpTree(node->children.value(1)));
        appendRedirections(items, node);
        return QString(node->kind == Node::Kind::While ? "(while " : "(until ") + items.join(' ') + ")";
    case Node::Kind::Test:
        return "(test " + node->source + ")";
    case Node::Kind::Unsupported:
        return "(unsupported " + node->reason + ")";
    }
    return "(?)";
}

bool hasUnsupported(const NodePtr &node)
{
    if (!node) return false;
    if (node->kind == Node::Kind::Unsupported) return true;
    for (const NodePtr &child : node->children)
        if (hasUnsupported(child)) return true;
    for (const Assignment &assignment : node->assignments)
        if (wordHasUnsupported(assignment.value)) return true;
    for (const Word &word : node->words)
        if (wordHasUnsupported(word)) return true;
    for (const Redirection &redirection : node->redirections)
        if (wordHasUnsupported(redirection.target)) return true;
    return false;
}

}
