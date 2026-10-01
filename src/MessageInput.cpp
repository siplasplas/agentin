#include "MessageInput.h"

#include <QKeyEvent>
#include <QTextBlock>
#include <QTextLayout>

namespace {
constexpr int kMaximumVisibleLines = 8;
}

MessageInput::MessageInput(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setTabChangesFocus(true);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (document()->isEmpty()) typed_ = false;
        if (!toPlainText().contains('\n')) typedLineBreak_ = false;
        fitHeight();
        updateEnterAction();
    });
    fitHeight();
}

void MessageInput::setPlaceholderText(const QString &text)
{
    if (placeholderText() == text) return;
    QPlainTextEdit::setPlaceholderText(text);
    // Qt does not repaint when one visible placeholder replaces another.
    viewport()->update();
}

void MessageInput::setHistory(const QStringList &messages)
{
    if (messages == history_) return;
    const bool browsing = position_ < history_.size();
    history_ = messages;
    position_ = history_.size();
    if (browsing) replaceText(draft_);
}

void MessageInput::replaceText(const QString &text)
{
    QTextCursor cursor(document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);
    cursor.endEditBlock();
}

void MessageInput::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
        if (modifiers == Qt::ShiftModifier) {
            insertLineBreak();
            return;
        }
        if (modifiers == Qt::ControlModifier) {
            submit();
            return;
        }
        if (modifiers == Qt::NoModifier) {
            if (enterSends()) submit();
            else insertLineBreak();
            return;
        }
    }
    if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
        if (event->key() == Qt::Key_PageUp || (event->key() == Qt::Key_Up && onFirstLine())) {
            recall(-1);
            return;
        }
        if (event->key() == Qt::Key_PageDown || (event->key() == Qt::Key_Down && onLastLine())) {
            recall(1);
            return;
        }
    }
    // Any change made from the keyboard counts as typing, except pasting.
    const int revision = document()->revision();
    QPlainTextEdit::keyPressEvent(event);
    if (document()->revision() != revision && !event->matches(QKeySequence::Paste)) typed_ = true;
    updateEnterAction();
}

void MessageInput::setEnterPolicy(EnterPolicy policy)
{
    enterPolicy_ = policy;
    updateEnterAction();
}

void MessageInput::setShortMessageLength(int length)
{
    shortMessageLength_ = length;
    updateEnterAction();
}

void MessageInput::updateEnterAction()
{
    const bool sends = enterSends();
    if (sends == enterSendsNow_) return;
    enterSendsNow_ = sends;
    emit enterActionChanged(sends);
}

bool MessageInput::enterSends() const
{
    if (enterPolicy_ != EnterPolicy::Smart) return enterPolicy_ == EnterPolicy::Send;
    if (!typed_) return true;
    return !typedLineBreak_ && toPlainText().trimmed().size() <= shortMessageLength_;
}

void MessageInput::insertLineBreak()
{
    insertPlainText("\n");
    typed_ = true;
    typedLineBreak_ = true;
    updateEnterAction();
}

void MessageInput::submit()
{
    position_ = history_.size();
    draft_.clear();
    emit submitted();
}

// Lines are visual lines, so a long wrapped message is also walked line by line.
bool MessageInput::onFirstLine() const
{
    const QTextCursor cursor = textCursor();
    const QTextLayout *layout = cursor.block().layout();
    return cursor.block() == document()->firstBlock()
        && (!layout || layout->lineForTextPosition(cursor.positionInBlock()).lineNumber() <= 0);
}

bool MessageInput::onLastLine() const
{
    const QTextCursor cursor = textCursor();
    const QTextLayout *layout = cursor.block().layout();
    return cursor.block() == document()->lastBlock()
        && (!layout || layout->lineForTextPosition(cursor.positionInBlock()).lineNumber() >= layout->lineCount() - 1);
}

// Going back puts the cursor at the end of the message and going forward at its start, so the arrow
// keys continue through its lines before reaching the next message.
void MessageInput::recall(int step)
{
    const qsizetype target = position_ + step;
    if (target < 0 || target > history_.size()) return;
    if (position_ == history_.size()) draft_ = toPlainText();
    position_ = target;
    replaceText(position_ == history_.size() ? draft_ : history_.at(position_));
    typed_ = false;
    updateEnterAction();
    moveCursor(step < 0 ? QTextCursor::End : QTextCursor::Start);
}

void MessageInput::fitHeight()
{
    const int lines = qBound(1, qRound(document()->documentLayout()->documentSize().height()), kMaximumVisibleLines);
    const QMargins margins = contentsMargins();
    setFixedHeight(lines * fontMetrics().lineSpacing() + 2 * qRound(document()->documentMargin())
                   + margins.top() + margins.bottom() + 2 * frameWidth());
}
