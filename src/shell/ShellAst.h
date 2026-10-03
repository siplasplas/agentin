#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

// The tree of a Bash command line as BashParser reads it, before any expansion. It is the input of the
// evaluation that decides about command approvals, so it keeps what can matter: where commands start, what
// they run, where they read and write, and which parts of a word are not known until the shell runs it.
namespace shell {

struct Node;
using NodePtr = std::shared_ptr<Node>;

// One part of a word.
struct WordPart
{
    enum class Kind {
        // Unquoted text; glob tells whether it holds an unquoted *, ? or [.
        Literal,
        // '…' or $'…': the text as written.
        SingleQuoted,
        // "…": the parts inside.
        DoubleQuoted,
        // $NAME, ${NAME}, ${NAME[i]}, $?, $1: text is the name, with its index.
        Variable,
        // $(…) or `…`: command is the parsed list.
        CommandSubstitution,
        // ~ or ~user at the start of a word: text is the user, empty for one's own home.
        Tilde,
        // A part whose value the shell computes and the evaluation does not: ${NAME:-…} and other
        // parameter operators, $((…)), and process substitution <(…) or >(…), whose list is in command.
        Opaque,
    };
    Kind kind = Kind::Literal;
    QString text;
    bool glob = false;
    QList<WordPart> parts;
    NodePtr command;
};

struct Word
{
    QList<WordPart> parts;
    // The word as written.
    QString source;
    // Whether the word is made only of literal and quoted text without globs, and that text.
    bool isLiteral() const;
    QString literalText() const;
};

struct Assignment
{
    QString name;
    Word value;
};

struct Redirection
{
    enum class Kind {
        Output,               // >
        Clobber,              // >|
        Append,               // >>
        Input,                // <
        ReadWrite,            // <>
        OutputAndError,       // &>
        AppendOutputAndError, // &>>
        DuplicateOutput,      // >&  (target is a descriptor, - or a file)
        DuplicateInput,       // <&
        HereDocument,         // << or <<-
        HereString,           // <<<
    };
    Kind kind = Kind::Output;
    // The descriptor written before the operator, or -1 for the operator's default.
    int fd = -1;
    // The file, the descriptor, the here-string, or for a here-document its delimiter.
    Word target;
    // The body of a here-document, and whether its delimiter was quoted, so that the body is not expanded.
    QString hereText;
    bool hereQuoted = false;
};

struct Node
{
    enum class Kind {
        // children run one after another; separators[i] follows children[i]: "&&", "||", ";", "&", or ""
        // after the last.
        List,
        // children joined by separators "|" or "|&"; negated by a leading !.
        Pipeline,
        // assignments, words (the program and its arguments) and redirections.
        Simple,
        // ( list ) and { list; }: children[0] is the list.
        Subshell,
        Group,
        // for variable [in words]; do children[0]; done
        For,
        // children alternate condition and body; an odd last child is the else body.
        If,
        // children[0] is the condition and children[1] the body.
        While,
        Until,
        // [[ … ]]: a test without effects; source keeps it.
        Test,
        // A construct the parser does not support, or a syntax error; reason says which.
        Unsupported,
    };
    Kind kind = Kind::Simple;
    QList<NodePtr> children;
    QStringList separators;
    bool negated = false;
    QList<Assignment> assignments;
    QList<Word> words;
    QList<Redirection> redirections;
    QString variable;
    // A for loop with "in words", rather than one over the positional parameters.
    bool forHasList = false;
    QString source;
    QString reason;
};

// Parses a command line. Constructs it does not support, and syntax errors, become Unsupported nodes, so the
// result can always be inspected and never throws.
NodePtr parseBash(const QString &text);
// A compact form of the tree, for tests and messages.
QString dumpTree(const NodePtr &node);
// Whether the tree holds an Unsupported node anywhere, also inside substitutions.
bool hasUnsupported(const NodePtr &node);

}
