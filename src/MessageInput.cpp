#include "MessageInput.h"

#include <QSet>
#include <QKeyEvent>
#include <QApplication>
#include <QListWidget>
#include <QWindow>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QMimeData>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextOption>

namespace {
constexpr int kMaximumVisibleLines = 8;
}

MessageInput::MessageInput(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setTabChangesFocus(true);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (!recalling_) recalledUntouched_ = false;
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

// Each message is recalled once, at the place of its latest use, so repeated messages do not alternate.
void MessageInput::setHistory(const QStringList &messages)
{
    QStringList unique;
    QSet<QString> seen;
    for (auto it = messages.crbegin(); it != messages.crend(); ++it) {
        const QString key = it->trimmed();
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        unique.prepend(*it);
    }
    if (unique == history_) return;
    const bool browsing = position_ < history_.size();
    history_ = unique;
    position_ = history_.size();
    if (browsing) replaceText(draft_);
    emit historyChanged();
}

namespace {
// A list shown as a popup closes like a combo box's: on Escape, on a click outside it and when the application
// stops being active. The whole application's clicks are watched while it is open, and such a click only closes
// it. On Wayland a click elsewhere in the window comes to the list itself, outside its rectangle; Qt's own
// check misses it, as it compares global positions, which Wayland does not give, and a scroll area passes
// clicks on its frame to no mouse handler.
class HistoryList : public QListWidget
{
public:
    explicit HistoryList(QWidget *parent) : QListWidget(parent) { qApp->installEventFilter(this); }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::ApplicationDeactivate) {
            close();
        } else if (isVisible() && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)) {
            bool inside = false;
            if (object == this || object == windowHandle())
                inside = rect().contains(static_cast<QMouseEvent *>(event)->position().toPoint());
            else if (auto *widget = qobject_cast<QWidget *>(object))
                inside = isAncestorOf(widget);
            if (!inside) {
                close();
                return true;
            }
        }
        return QListWidget::eventFilter(object, event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) close();
        else QListWidget::keyPressEvent(event);
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::WindowDeactivate) close();
        return QListWidget::event(event);
    }
};
}

void MessageInput::showHistoryList()
{
    if (history_.isEmpty()) return;
    // A parent makes it a popup of this window, also on Wayland.
    auto *list = new HistoryList(this);
    list->setWindowFlags(Qt::Popup);
    list->setAttribute(Qt::WA_DeleteOnClose);
    list->setObjectName("messageHistory");
    list->setFont(font());
    list->setUniformItemSizes(true);
    // Wide enough for the field's first line with the marker and the scroll bar beside it.
    const int width = this->width() + list->verticalScrollBar()->sizeHint().width()
                      + QFontMetrics(font()).horizontalAdvance(" …");
    // The newest message comes first. A row shows the message's beginning; the tooltip has all of it.
    for (auto it = history_.crbegin(); it != history_.crend(); ++it) {
        auto *item = new QListWidgetItem(beginning(*it), list);
        item->setToolTip(*it);
    }
    const auto rowOf = [this](qsizetype position) { return int(history_.size() - 1 - position); };
    // The message being recalled, or the newest, is selected.
    list->setCurrentRow(position_ < history_.size() ? rowOf(position_) : 0);
    const int rowHeight = list->sizeHintForRow(0);
    const int frame = 2 * list->frameWidth();
    int height = qMin(int(history_.size()), 12) * rowHeight + frame;

    // Below the field when it fits, as a combo box opens; otherwise on the side with more room, shorter if
    // even that is too small. On Wayland a window does not know where it is on the screen, and a popup that
    // reaches out of its window is placed oddly by some compositors, so there the list stays within the window.
    const QRect field(mapToGlobal(QPoint(0, 0)), size());
    QPoint position = field.bottomLeft() + QPoint(0, 1);
    QRect available;
    if (QGuiApplication::platformName().startsWith("wayland"))
        available = QRect(window()->mapToGlobal(QPoint(0, 0)), window()->size());
    else if (const QScreen *screen = this->screen())
        available = screen->availableGeometry();
    if (available.isValid()) {
        const int below = available.bottom() - field.bottom();
        const int above = field.top() - available.top();
        // Without room for one row on either side the list does not open.
        if (qMax(above, below) < rowHeight + frame) {
            delete list;
            return;
        }
        if (height > below && above > below) {
            height = qMin(height, above);
            position = QPoint(field.left(), field.top() - height);
        } else {
            height = qMin(height, below);
        }
        position.setX(qBound(available.left(), position.x(), qMax(available.left(), available.right() - width + 1)));
    }
    list->resize(width, height);
    list->move(position);
    const auto choose = [this, list, rowOf](QListWidgetItem *item) {
        const int row = list->row(item);
        list->close();
        recallAt(rowOf(row));
        setFocus();
    };
    connect(list, &QListWidget::itemActivated, this, choose);
    connect(list, &QListWidget::itemClicked, this, choose);
    list->show();
    list->scrollToItem(list->currentItem());
    list->setFocus();
}

void MessageInput::replaceText(const QString &text)
{
    QTextCursor cursor(document());
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);
    cursor.endEditBlock();
}

void MessageInput::showSuggestion(const QString &text)
{
    replaceText(text);
    selectAll();
    typed_ = false;
    typedLineBreak_ = false;
    updateEnterAction();
    setFocus();
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
    if ((event->modifiers() & ~Qt::KeypadModifier) == Qt::AltModifier && event->key() == Qt::Key_Down) {
        showHistoryList();
        return;
    }
    if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
        // A recalled message nobody has touched is skipped as a whole, not walked line by line.
        if (event->key() == Qt::Key_PageUp || (event->key() == Qt::Key_Up && (recalledUntouched_ || onFirstLine()))) {
            recall(-1);
            return;
        }
        if (event->key() == Qt::Key_PageDown || (event->key() == Qt::Key_Down && (recalledUntouched_ || onLastLine()))) {
            recall(1);
            return;
        }
    }
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Home
        || event->key() == Qt::Key_End)
        recalledUntouched_ = false;
    // Any change made from the keyboard counts as typing, except pasting.
    const int revision = document()->revision();
    QPlainTextEdit::keyPressEvent(event);
    // Clearing the field by keyboard leaves nothing typed.
    if (document()->revision() != revision && !event->matches(QKeySequence::Paste)) typed_ = !document()->isEmpty();
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
    // insertPlainText() does not scroll as typing does, so the new line could stay below the visible ones.
    insertPlainText("\n");
    ensureCursorVisible();
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
    recallAt(target);
    moveCursor(step < 0 ? QTextCursor::End : QTextCursor::Start);
}

QString MessageInput::beginning(const QString &message) const
{
    const QString paragraph = message.section('\n', 0, 0);
    QTextLayout layout(paragraph, font());
    QTextOption option = document()->defaultTextOption();
    option.setWrapMode(wordWrapMode());
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) line.setLineWidth(qMax(1.0, viewport()->width() - 2 * document()->documentMargin()));
    layout.endLayout();
    const int length = line.isValid() ? line.textLength() : int(paragraph.size());
    QString first = paragraph.left(length);
    while (!first.isEmpty() && first.back().isSpace()) first.chop(1);
    const bool more = length < paragraph.size() || !message.mid(paragraph.size()).trimmed().isEmpty();
    return more ? first + " …" : first;
}

// Shows the message at this place of the history, or the draft past its end, as recalled and untouched.
void MessageInput::recallAt(qsizetype target)
{
    if (target < 0 || target > history_.size()) return;
    if (position_ == history_.size()) draft_ = toPlainText();
    position_ = target;
    recalling_ = true;
    replaceText(position_ == history_.size() ? draft_ : history_.at(position_));
    recalling_ = false;
    recalledUntouched_ = position_ < history_.size();
    typed_ = false;
    updateEnterAction();
    moveCursor(QTextCursor::End);
}

// Line breaks, spaces and tabs around pasted text are dropped: a trailing line break would make Enter
// start a new line instead of sending.
// Pasted text that replaces the whole field counts as unchanged, so Enter sends it whatever its length.
void MessageInput::insertFromMimeData(const QMimeData *source)
{
    const QTextCursor cursor = textCursor();
    const bool replacesAll = document()->isEmpty()
        || (cursor.selectionStart() == 0 && cursor.selectionEnd() == document()->characterCount() - 1);
    if (source->hasText()) insertPlainText(source->text().trimmed());
    else QPlainTextEdit::insertFromMimeData(source);
    if (replacesAll) {
        typed_ = false;
        typedLineBreak_ = false;
        updateEnterAction();
    }
}

// The field grows after its text has changed, when the scrolling has already been decided for the smaller
// height; the cursor is brought into view again at the new size.
void MessageInput::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    if (hasFocus()) ensureCursorVisible();
}

void MessageInput::mousePressEvent(QMouseEvent *event)
{
    recalledUntouched_ = false;
    QPlainTextEdit::mousePressEvent(event);
}

void MessageInput::fitHeight()
{
    const int lines = qBound(1, qRound(document()->documentLayout()->documentSize().height()), kMaximumVisibleLines);
    const QMargins margins = contentsMargins();
    setFixedHeight(lines * fontMetrics().lineSpacing() + 2 * qRound(document()->documentMargin())
                   + margins.top() + margins.bottom() + 2 * frameWidth());
}
