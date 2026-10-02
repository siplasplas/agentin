#pragma once

#include "ChangeTracker.h"

#include <QPointer>
#include <QWidget>

#include <optional>

class QLabel;
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

// The files a chat's latest turn changed, with lines added and removed, and the unified diff of the
// selected file. It follows the tracker live while the turn runs. A file opens with the application the
// system associates with its type.
class ChangesWindow : public QWidget
{
    Q_OBJECT

public:
    ChangesWindow(ChangeTracker *tracker, const QString &title, QWidget *parent = nullptr);
    ChangeTracker *tracker() const { return tracker_; }

private:
    void updateList();
    void showSelected();
    void showDiff(const FileDiff &diff);
    void openFile(QTreeWidgetItem *item) const;
    void showMenu(const QPoint &position);
    std::optional<FileChange> selectedChange() const;

    QPointer<ChangeTracker> tracker_;
    QLabel *summary_;
    QTreeWidget *list_;
    QPlainTextEdit *diff_;
    // The file whose diff is shown or requested, and the state it was requested in.
    QString shownKey_;
    QString shownState_;
};
