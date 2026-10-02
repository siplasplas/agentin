#pragma once

#include "ChangeTracker.h"

#include <QPointer>
#include <QSet>
#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// The files a chat changed since a chosen point, with lines added and removed, and the diff of the selected
// file, unified or side by side. Long unchanged stretches are folded and open with a click. It follows the
// tracker live while a turn runs. A file opens with the application the system associates with its type.
class ChangesWindow : public QWidget
{
    Q_OBJECT

public:
    ChangesWindow(ChangeTracker *tracker, const QString &title, QWidget *parent = nullptr);
    ChangeTracker *tracker() const { return tracker_; }

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    // One row of the diff view; Changed pairs a removed and an added line side by side.
    struct Row
    {
        enum Kind { Context, Removed, Added, Changed, Fold };
        Kind kind = Context;
        int before = -1;
        int after = -1;
        // For Fold: the first hidden line in the diff's sequence and the number of hidden lines.
        int fold = -1;
        int count = 0;
    };

    ChangesSince since() const;
    void updateList();
    void showSelected();
    void render();
    QList<Row> rows(bool sideBySide) const;
    void moveToChange(int step);
    void openFile(QTreeWidgetItem *item) const;
    void showMenu(const QPoint &position);
    std::optional<FileChange> selectedChange() const;

    QPointer<ChangeTracker> tracker_;
    QLabel *summary_;
    QComboBox *since_;
    QTreeWidget *list_;
    QComboBox *view_;
    QToolButton *previous_;
    QToolButton *next_;
    QStackedWidget *pages_;
    QPlainTextEdit *unified_;
    QPlainTextEdit *before_;
    QPlainTextEdit *after_;
    // The file whose diff is shown or requested, and the state it was requested in.
    QString shownKey_;
    QString shownState_;
    std::optional<FileDiff> diff_;
    QSet<int> openedFolds_;
    // Rows where a run of changes starts, and the one moved to last.
    QList<int> changeStarts_;
    int currentChange_ = -1;
};
