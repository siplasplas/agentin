#pragma once

#include <QPlainTextEdit>
#include <QTextBlock>

// Metadata stays with the document when the shared view moves between tabs.
struct ToolBlockData : QTextBlockUserData
{
    int group = 0;
    bool header = false;
    bool collapsed = true;
};

class ChatView : public QPlainTextEdit
{
public:
    explicit ChatView(QWidget *parent = nullptr);
    void refreshTools();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    QWidget *foldMargin_;
    bool toggleTool(const QTextBlock &block);
    bool hasToolContent(const QTextBlock &header) const;
};
