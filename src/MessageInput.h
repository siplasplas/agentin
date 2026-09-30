#pragma once

#include <QPlainTextEdit>
#include <QStringList>

// Message field that grows with its text. Enter sends and Shift+Enter starts a new line. Up and Down
// move between lines and, on the first or last line, recall the previous or next message of the
// conversation; Page Up and Page Down always recall messages. Past the newest message the text typed
// before browsing comes back.
class MessageInput : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit MessageInput(QWidget *parent = nullptr);

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
    void fitHeight();

    QStringList history_;
    // history_.size() while editing the draft rather than a recalled message.
    qsizetype position_ = 0;
    QString draft_;
};
