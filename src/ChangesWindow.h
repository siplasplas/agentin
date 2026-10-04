#pragma once

#include "ChangeTracker.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QTimer;
class QLabel;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace diffmerge::gui {
class FileDiffWidget;
}

// The files a chat changed since a chosen point, with lines added and removed, and the diff of the selected
// file in DiffMerge's view: unified or side by side, with changed words and characters marked, and long
// unchanged stretches folded until clicked. It follows the tracker live while a turn runs. A file opens with
// the application the system associates with its type.
class ChangesWindow : public QWidget
{
    Q_OBJECT

public:
    ChangesWindow(ChangeTracker *tracker, const QString &title, QWidget *parent = nullptr);
    ChangeTracker *tracker() const { return tracker_; }
    // Opens a file at a line; without it the system opens the file.
    void setOpener(std::function<void(const QString &path, int line)> opener) { opener_ = std::move(opener); }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    ChangesSince since() const;
    void updateList();
    void showSelected();
    // Shows the received diff, or a message in its place.
    void showDiff();
    void showMessage(const QString &text);
    void sortChanges(QList<FileChange> &changes) const;
    void openFile(QTreeWidgetItem *item) const;
    void showMenu(const QPoint &position);
    std::optional<FileChange> selectedChange() const;

    QPointer<ChangeTracker> tracker_;
    std::function<void(const QString &path, int line)> opener_;
    QLabel *summary_;
    QComboBox *since_;
    // New files listed in a group of their own below the changed ones, as JetBrains IDEs show them.
    QCheckBox *groupNew_;
    QTreeWidget *list_;
    QComboBox *view_;
    QCheckBox *skipUnchanged_;
    QToolButton *previous_;
    QToolButton *next_;
    QStackedWidget *pages_;
    QLabel *message_;
    diffmerge::gui::FileDiffWidget *diffView_;
    // The file whose diff is shown or requested, and the state it was requested in.
    QString shownKey_;
    QString shownState_;
    // The state of each listed file, so that files that changed since the last refresh light up briefly.
    QHash<QString, QString> listedStates_;
    int listedSince_ = -1;
    QSet<QString> flashing_;
    // The list's order: by path (column 1), added lines (2) or removed lines (3).
    int sortColumn_ = 1;
    QTimer *flashEnd_;
    std::optional<FileDiff> diff_;
};
