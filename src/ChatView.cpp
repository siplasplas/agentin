#include "ChatView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QPlainTextDocumentLayout>
#include <QTextDocument>
#include <QTextLayout>

ChatView::ChatView(QWidget *parent) : QPlainTextEdit(parent), foldMargin_(new QWidget(this))
{
    setViewportMargins(20, 0, 0, 0);
    foldMargin_->installEventFilter(this);
    foldMargin_->setToolTip("+ expands tool output; − collapses it. You can also click the heading.");
    connect(this, &QPlainTextEdit::updateRequest, this, [this] { foldMargin_->update(); });
    setToolTip("Click a tool heading to expand or collapse its details and output.");
}

void ChatView::refreshTools()
{
    QHash<int, bool> collapsed;
    QList<QTextEdit::ExtraSelection> selections;
    bool changed = false;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        auto *data = dynamic_cast<ToolBlockData *>(block.userData());
        bool visible = true;
        if (data) {
            if (data->header) collapsed.insert(data->group, data->collapsed);
            else visible = !collapsed.value(data->group, true);
            if (visible) {
                QTextEdit::ExtraSelection selection;
                selection.cursor = QTextCursor(block);
                selection.cursor.select(QTextCursor::BlockUnderCursor);
                selection.format.setForeground(palette().color(QPalette::Link));
                if (data->header) selection.format.setFontWeight(QFont::Bold);
                selections.append(selection);
            }
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
        if (data && data->group == heading->group && !block.text().trimmed().isEmpty()) return true;
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
