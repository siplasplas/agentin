#pragma once

#include <QPlainTextEdit>
#include <QStringList>

// Message field that grows with its text. Shift+Enter always starts a new line, Ctrl+Enter always
// sends, and Enter follows the EnterPolicy. Up and Down move between lines and, on the first or last
// line, recall the previous or next message of the conversation; Page Up and Page Down always recall
// messages. Past the newest message the text typed before browsing comes back.
class MessageInput : public QPlainTextEdit
{
    Q_OBJECT

public:
    enum class EnterPolicy {
        // Enter sends unless the user typed a line break in this message: a single line, a pasted text
        // or a recalled message is sent, while a message written over several lines gets new lines.
        Smart,
        Send,
        NewLine,
    };

    explicit MessageInput(QWidget *parent = nullptr);

    EnterPolicy enterPolicy() const { return enterPolicy_; }
    void setEnterPolicy(EnterPolicy policy) { enterPolicy_ = policy; }

    // Earlier messages of the conversation, oldest first. Resets browsing.
    void setHistory(const QStringList &messages);

signals:
    void submitted();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    bool onFirstLine() const;
    bool onLastLine() const;
    void recall(int step);
    void submit();
    void insertLineBreak();
    void fitHeight();

    QStringList history_;
    // history_.size() while editing the draft rather than a recalled message.
    qsizetype position_ = 0;
    QString draft_;
    EnterPolicy enterPolicy_ = EnterPolicy::Smart;
    // The user typed a line break in the current text; line breaks from pasting or recalling do not count.
    bool typedLineBreak_ = false;
};
