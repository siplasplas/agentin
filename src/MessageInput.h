#pragma once

#include <QPlainTextEdit>
#include <QStringList>

// Message field that grows with its text. Shift+Enter always starts a new line, Ctrl+Enter always
// sends, and Enter follows the EnterPolicy. Up and Down move between lines and, on the first or last
// line, recall the previous or next message of the conversation; Page Up and Page Down always recall
// messages. Past the newest message the text typed before browsing comes back. Alt+Down shows the messages in
// a list, as a combo box does.
class MessageInput : public QPlainTextEdit
{
    Q_OBJECT

public:
    enum class EnterPolicy {
        // Enter sends a recalled message or pasted text left unchanged, and a typed message of one
        // line up to shortMessageLength() characters. A longer typed message, or one where the user
        // typed a line break, gets a new line; Ctrl+Enter sends it.
        Smart,
        Send,
        NewLine,
    };

    explicit MessageInput(QWidget *parent = nullptr);
    void setPlaceholderText(const QString &text);

    EnterPolicy enterPolicy() const { return enterPolicy_; }
    void setEnterPolicy(EnterPolicy policy);
    // 0 means that Enter never sends typed text in the Smart policy.
    int shortMessageLength() const { return shortMessageLength_; }
    void setShortMessageLength(int length);
    // What Enter does for the current text; Shift+Enter and Ctrl+Enter do the other.
    bool enterSends() const;

    // Earlier messages of the conversation, oldest first. Resets browsing.
    void setHistory(const QStringList &messages);
    // Replaces the text as one edit that Ctrl+Z can undo, unlike setPlainText() and clear().
    void replaceText(const QString &text);
    // Shows a suggested message selected, so that typing replaces it and Enter sends it.
    void showSuggestion(const QString &text);
    bool hasHistory() const { return !history_.isEmpty(); }
    // Lists the earlier messages below the field, newest first, as a combo box lists its items; the chosen one is
    // recalled as Page Up would recall it.
    void showHistoryList();

signals:
    void submitted();
    void enterActionChanged(bool sends);
    void historyChanged();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void insertFromMimeData(const QMimeData *source) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    bool onFirstLine() const;
    bool onLastLine() const;
    void recall(int step);
    void recallAt(qsizetype position);
    // The beginning of a message as the field would show it: up to its first line break or the end of its
    // first wrapped line at the field's width, with " …" when more follows.
    QString beginning(const QString &message) const;
    void updateEnterAction();
    void insertLineBreak();
    void submit();
    void fitHeight();

    QStringList history_;
    // history_.size() while editing the draft rather than a recalled message.
    qsizetype position_ = 0;
    QString draft_;
    EnterPolicy enterPolicy_ = EnterPolicy::Smart;
    // The user changed the current text with the keyboard; pasting and recalling do not count.
    bool typed_ = false;
    bool typedLineBreak_ = false;
    // A message recalled from the history that has been neither edited nor entered with the cursor.
    bool recalledUntouched_ = false;
    bool recalling_ = false;
    int shortMessageLength_ = 60;
    bool enterSendsNow_ = true;
};
