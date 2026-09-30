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
        fitHeight();
    });
    fitHeight();
}

void MessageInput::setHistory(const QStringList &messages)
{
    if (messages == history_) return;
    const bool browsing = position_ < history_.size();
    history_ = messages;
    position_ = history_.size();
    if (browsing) setPlainText(draft_);
}

void MessageInput::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
        if (modifiers == Qt::ShiftModifier) {
            insertPlainText("\n");
            typed_ = true;
            return;
        }
        if (modifiers == Qt::ControlModifier) {
            submit();
            return;
        }
        if (modifiers == Qt::NoModifier) {
            const bool newLine = enterPolicy_ == EnterPolicy::NewLine
                || (enterPolicy_ == EnterPolicy::Smart && typed_);
            if (!newLine) {
                submit();
                return;
            }
            insertPlainText("\n");
            typed_ = true;
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
    setPlainText(position_ == history_.size() ? draft_ : history_.at(position_));
    typed_ = false;
    moveCursor(step < 0 ? QTextCursor::End : QTextCursor::Start);
}

void MessageInput::fitHeight()
{
    const int lines = qBound(1, qRound(document()->documentLayout()->documentSize().height()), kMaximumVisibleLines);
    const QMargins margins = contentsMargins();
    setFixedHeight(lines * fontMetrics().lineSpacing() + 2 * qRound(document()->documentMargin())
                   + margins.top() + margins.bottom() + 2 * frameWidth());
}
