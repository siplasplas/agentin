#pragma once

#include <QPlainTextEdit>
#include <QSet>
#include <QSyntaxHighlighter>
#include <QTextBlock>

#include <functional>

// Metadata stays with the document when the shared view moves between tabs.
struct ToolBlockData : QTextBlockUserData
{
    int group = 0;
    bool header = false;
    bool collapsed = true;
    // The tool's final status line, shown while its output is folded.
    bool status = false;
};

// Marks the blocks of the user's messages for ChatView: a message starts with a "You: " or "You (…): "
// line and lasts until a line of the agent or a bracketed entry such as a tool. The transcript is plain
// text that is often rebuilt, so the marks come from the text rather than from stored formats.
class UserMessageHighlighter : public QSyntaxHighlighter
{
public:
    enum State { Other = 0, Message = 1, MessageGap = 2 };
    UserMessageHighlighter(QTextDocument *document, std::function<QString()> agentName);
    // "You: " or "You (…): ", as the chat writes them; the colon keeps ordinary text from matching.
    static bool startsMessage(const QString &text);

protected:
    void highlightBlock(const QString &text) override;

private:
    std::function<QString()> agentName_;
};

class QToolButton;

// Keeps a text view at its end while the user is there. Scrolled up, the view stays put, and a button
// appears when new text arrives below, which scrolls to the end.
class TailFollower : public QObject
{
public:
    TailFollower(QPlainTextEdit *view, const QString &buttonName);
    void scrollToEnd();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void place();

    QPlainTextEdit *view_;
    QToolButton *button_;
    bool following_ = true;
    // The document and its revision the user last saw at the end.
    const QTextDocument *seenDocument_ = nullptr;
    int seenRevision_ = 0;
};

class ChatView : public QPlainTextEdit
{
public:
    explicit ChatView(QWidget *parent = nullptr);
    // Shows another document. The tool highlights of the shown one go first: they point into it, and Qt
    // repaints the old highlights when new ones are set, which would read a document a closed tab deleted.
    void showDocument(QTextDocument *document);
    void refreshTools();
    // Hidden tool calls leave only the user's messages and the answers, with a thin line where they were.
    void setToolsShown(bool shown);

    // What the copy actions of the context menu take from the selection, or from the whole chat without one.
    enum class CopyMode { ToolsExpanded, ToolsFolded, WithoutTools, ToolsOnly };
    QString copyText(CopyMode mode) const;

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    QWidget *foldMargin_;
    bool toolsShown_ = true;
    // Visible blocks followed by hidden tool calls.
    QSet<int> separators_;
    bool toggleTool(const QTextBlock &block);
    bool hasToolContent(const QTextBlock &header) const;
};
