#include "ChatView.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QScrollBar>
#include <QSet>
#include <QPlainTextDocumentLayout>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextLayout>

UserMessageHighlighter::UserMessageHighlighter(QTextDocument *document, std::function<QString()> agentName)
    : QSyntaxHighlighter(document), agentName_(std::move(agentName))
{
}

bool UserMessageHighlighter::startsMessage(const QString &text)
{
    static const QRegularExpression start("^You( \\([^)\n]*\\))?: ");
    return start.match(text).hasMatch();
}

void UserMessageHighlighter::highlightBlock(const QString &text)
{
    const bool inMessage = previousBlockState() == Message || previousBlockState() == MessageGap;
    if (startsMessage(text)) setCurrentBlockState(Message);
    else if (text.startsWith('[') || text.startsWith(agentName_() + ": ")) setCurrentBlockState(Other);
    // Blank lines are left unmarked so that no band separates a message from the answer below it.
    else if (inMessage) setCurrentBlockState(text.trimmed().isEmpty() ? MessageGap : Message);
    else setCurrentBlockState(Other);
}

TailFollower::TailFollower(QPlainTextEdit *view, const QString &buttonName)
    : QObject(view), view_(view), button_(new QToolButton(view))
{
    button_->setObjectName(buttonName);
    button_->setArrowType(Qt::DownArrow);
    button_->setToolTip("New text below; scroll to the end");
    button_->setCursor(Qt::PointingHandCursor);
    button_->resize(28, 28);
    button_->hide();
    QScrollBar *bar = view->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) {
        following_ = value >= bar->maximum();
        if (!following_) return;
        seenDocument_ = view_->document();
        seenRevision_ = view_->document()->revision();
        button_->hide();
    });
    // Queued, so that scrolling never happens inside an edit of the document.
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar] {
        if (following_) {
            bar->setValue(bar->maximum());
            seenDocument_ = view_->document();
            seenRevision_ = view_->document()->revision();
        } else if (bar->value() < bar->maximum() && view_->document() == seenDocument_
                   && view_->document()->revision() != seenRevision_) {
            place();
            button_->show();
            button_->raise();
        }
    }, Qt::QueuedConnection);
    connect(button_, &QToolButton::clicked, this, &TailFollower::scrollToEnd);
    view->viewport()->installEventFilter(this);
}

void TailFollower::scrollToEnd()
{
    following_ = true;
    view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->maximum());
    button_->hide();
}

bool TailFollower::eventFilter(QObject *object, QEvent *event)
{
    if (object == view_->viewport() && event->type() == QEvent::Resize) place();
    return QObject::eventFilter(object, event);
}

void TailFollower::place()
{
    const QRect area = view_->viewport()->geometry();
    button_->move(area.right() - button_->width() - 8, area.bottom() - button_->height() - 8);
}

ChatView::ChatView(QWidget *parent) : QPlainTextEdit(parent), foldMargin_(new QWidget(this))
{
    setViewportMargins(20, 0, 0, 0);
    foldMargin_->installEventFilter(this);
    foldMargin_->setToolTip("+ expands tool output; − collapses it. You can also click the heading.");
    connect(this, &QPlainTextEdit::updateRequest, this, [this] { foldMargin_->update(); });
    setToolTip("Click a tool heading to expand or collapse its details and output.");
}

void ChatView::showDocument(QTextDocument *document)
{
    if (QPlainTextEdit::document() == document) return;
    setExtraSelections({});
    separators_.clear();
    setDocument(document);
}

void ChatView::refreshTools()
{
    QHash<int, bool> collapsed;
    QList<QTextEdit::ExtraSelection> selections;
    bool changed = false;
    separators_.clear();
    // While tools are hidden, the blank lines after them go too, and a line marks where they were.
    bool afterTool = false;
    int lastVisible = -1;
    bool hiddenRun = false;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        auto *data = dynamic_cast<ToolBlockData *>(block.userData());
        bool visible = true;
        if (!toolsShown_ && (data || (afterTool && block.text().trimmed().isEmpty()))) {
            visible = false;
            afterTool = true;
            hiddenRun = true;
        } else if (data) {
            afterTool = false;
            if (data->header) collapsed.insert(data->group, data->collapsed);
            else if (!data->status) visible = !collapsed.value(data->group, true);
            if (visible && !data->status) {
                QTextEdit::ExtraSelection selection;
                selection.cursor = QTextCursor(block);
                selection.cursor.select(QTextCursor::BlockUnderCursor);
                selection.format.setForeground(palette().color(QPalette::Link));
                if (data->header) selection.format.setFontWeight(QFont::Bold);
                selections.append(selection);
            }
        }
        if (!data && toolsShown_) afterTool = false;
        if (visible) {
            if (hiddenRun && lastVisible >= 0) separators_.insert(lastVisible);
            hiddenRun = false;
            if (!data) afterTool = false;
            lastVisible = block.blockNumber();
        }
        if (block.isVisible() != visible) {
            block.setVisible(visible);
            // Folding changes visibility, not the cached text layout. Preserve
            // wrapped line counts when expanding an already laid-out block.
            block.setLineCount(visible ? qMax(1, block.layout()->lineCount()) : 0);
            changed = true;
        }
    }
    setExtraSelections(selections);
    if (changed) {
        auto *layout = qobject_cast<QPlainTextDocumentLayout *>(document()->documentLayout());
        if (layout) {
            // Update the scrollbar range before repainting. Invalidating every
            // block here leaves stale line counts until lazy layout runs during
            // scrolling, which can reenter Qt's scrollbar updates recursively.
            emit layout->documentSizeChanged(layout->documentSize());
            layout->requestUpdate();
        }
    }
    viewport()->update();
    foldMargin_->update();
}

// The user's messages get a yellow band across the view, darker on a dark background.
QString ChatView::copyText(CopyMode mode) const
{
    const QTextCursor selection = textCursor();
    const bool selected = selection.hasSelection();
    const int start = selected ? selection.selectionStart() : 0;
    const int end = selected ? selection.selectionEnd() : document()->characterCount() - 1;
    QStringList lines;
    int lastGroup = 0;
    for (QTextBlock block = document()->findBlock(start); block.isValid() && block.position() <= end; block = block.next()) {
        const auto *tool = dynamic_cast<const ToolBlockData *>(block.userData());
        const bool output = tool && !tool->header && !tool->status;
        if (mode == CopyMode::ToolsFolded && output) continue;
        if (mode == CopyMode::WithoutTools && tool) continue;
        if (mode == CopyMode::ToolsOnly && !tool) continue;
        if (mode == CopyMode::ToolsOnly && tool->group != lastGroup && !lines.isEmpty()) lines.append(QString());
        if (tool) lastGroup = tool->group;
        // A selection starting or ending inside a line takes only its selected part.
        QString text = block.text();
        const int from = qMax(0, start - block.position());
        const int to = qMin(int(text.size()), end - block.position());
        text = text.mid(from, qMax(0, to - from));
        lines.append(text);
    }
    // Removed tool calls leave no runs of empty lines behind.
    QString text = lines.join('\n');
    static const QRegularExpression blankRuns("\n{3,}");
    text.replace(blankRuns, "\n\n");
    return text.trimmed() + '\n';
}

void ChatView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = createStandardContextMenu(event->pos());
    menu->addSeparator();
    const QString scope = textCursor().hasSelection() ? "the selection" : "the whole chat";
    const auto add = [this, menu, &scope](const QString &label, CopyMode mode, const QString &tip) {
        QAction *action = menu->addAction(label, this, [this, mode] { QGuiApplication::clipboard()->setText(copyText(mode)); });
        action->setToolTip("Copies " + scope + ": " + tip);
    };
    add("Copy with tools expanded", CopyMode::ToolsExpanded, "messages, answers and tool calls with their output");
    add("Copy with tools folded", CopyMode::ToolsFolded, "messages, answers, and the tool calls' headings and status without output");
    add("Copy without tools", CopyMode::WithoutTools, "only the messages and the answers");
    add("Copy tools only", CopyMode::ToolsOnly, "only the tool calls, with their output");
    menu->setToolTipsVisible(true);
    menu->exec(event->globalPos());
    delete menu;
}

void ChatView::paintEvent(QPaintEvent *event)
{
    {
        QPainter painter(viewport());
        const QColor band = palette().color(QPalette::Base).lightness() < 128 ? QColor(84, 74, 18) : QColor(255, 243, 176);
        const QPointF offset = contentOffset();
        for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
            if (!block.isVisible()) continue;
            const QRectF rect = blockBoundingGeometry(block).translated(offset);
            if (rect.top() > event->rect().bottom()) break;
            // A blank line inside a message joins its paragraphs; one after the message separates it.
            const QTextBlock next = block.next();
            const bool inside = block.userState() == UserMessageHighlighter::Message
                || (block.userState() == UserMessageHighlighter::MessageGap && next.isValid()
                    && next.userState() == UserMessageHighlighter::Message
                    && !UserMessageHighlighter::startsMessage(next.text()));
            if (inside) painter.fillRect(QRectF(0, rect.top(), viewport()->width(), rect.height()), band);
            if (separators_.contains(block.blockNumber())) {
                // On a blank line the separator runs through its middle, otherwise below the text.
                const qreal y = block.text().trimmed().isEmpty() ? rect.center().y() : rect.bottom() - 1;
                painter.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DashLine));
                painter.drawLine(QPointF(4, y), QPointF(viewport()->width() - 4, y));
            }
        }
    }
    QPlainTextEdit::paintEvent(event);
}

void ChatView::setToolsShown(bool shown)
{
    if (toolsShown_ == shown) return;
    toolsShown_ = shown;
    refreshTools();
}

void ChatView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !textCursor().hasSelection()) {
        const QTextBlock block = cursorForPosition(event->position().toPoint()).block();
        if (toggleTool(block)) {
            event->accept();
            return;
        }
    }
    QPlainTextEdit::mouseReleaseEvent(event);
}

bool ChatView::toggleTool(const QTextBlock &block)
{
    auto *data = dynamic_cast<ToolBlockData *>(block.userData());
    if (!data || !data->header || !hasToolContent(block)) return false;
    data->collapsed = !data->collapsed;
    refreshTools();
    return true;
}

bool ChatView::hasToolContent(const QTextBlock &header) const
{
    const auto *heading = dynamic_cast<ToolBlockData *>(header.userData());
    if (!heading || !heading->header) return false;
    for (QTextBlock block = header.next(); block.isValid(); block = block.next()) {
        const auto *data = dynamic_cast<ToolBlockData *>(block.userData());
        if (data && data->header) break;
        if (data && !data->status && data->group == heading->group && !block.text().trimmed().isEmpty()) return true;
    }
    return false;
}

void ChatView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    foldMargin_->setGeometry(viewport()->geometry().left() - 20, viewport()->geometry().top(),
                             20, viewport()->height());
}

bool ChatView::eventFilter(QObject *object, QEvent *event)
{
    if (object != foldMargin_) return QPlainTextEdit::eventFilter(object, event);
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton)
            return toggleTool(cursorForPosition(QPoint(0, mouse->position().toPoint().y())).block());
    } else if (event->type() == QEvent::Paint) {
        QPainter painter(foldMargin_);
        painter.fillRect(foldMargin_->rect(), palette().brush(QPalette::Base));
        painter.setPen(palette().color(QPalette::Link));
        for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
            if (!block.isVisible()) continue;
            const qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
            if (top > viewport()->height()) break;
            auto *data = dynamic_cast<ToolBlockData *>(block.userData());
            if (!data || !data->header) continue;
            const int centerY = qRound(top + fontMetrics().height() / 2.0);
            painter.drawRect(4, centerY - 5, 10, 10);
            if (hasToolContent(block)) {
                painter.drawLine(6, centerY, 12, centerY);
                if (data->collapsed) painter.drawLine(9, centerY - 3, 9, centerY + 3);
            }
        }
        return true;
    }
    return QPlainTextEdit::eventFilter(object, event);
}
