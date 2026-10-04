#include "ChangesWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <diffmerge/DiffEditor.h>
#include <diffmerge/FileDiffWidget.h>
#include <qce/CodeEditArea.h>

#include <algorithm>

namespace {
constexpr int kContextLines = 3;

QString key(const FileChange &change)
{
    return change.root + '\n' + change.file.path;
}

// What a refresh can change about a listed file.
QString changeState(const FileChange &change)
{
    return QString("%1|%2|%3|%4|%5|%6").arg(int(change.since)).arg(int(change.file.status)).arg(change.file.sizeAfter)
        .arg(change.lines.added).arg(change.lines.removed).arg(int(change.lines.kind));
}

QString statusLetter(ChangedFile::Status status)
{
    switch (status) {
    case ChangedFile::Status::Modified: return "m";
    case ChangedFile::Status::New: return "n";
    case ChangedFile::Status::Deleted: return "d";
    case ChangedFile::Status::Renamed: return "r";
    case ChangedFile::Status::Binary: return "b";
    }
    return "?";
}

QString byteSize(qint64 bytes)
{
    return bytes < 0 ? QString("none") : QLocale::system().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

}

ChangesWindow::ChangesWindow(ChangeTracker *tracker, const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window), tracker_(tracker), summary_(new QLabel(this)), since_(new QComboBox(this)),
      groupNew_(new QCheckBox("Group new files", this)), list_(new QTreeWidget(this)), view_(new QComboBox(this)),
      skipUnchanged_(new QCheckBox("Skip unchanged lines", this)), previous_(new QToolButton(this)),
      next_(new QToolButton(this)), pages_(new QStackedWidget(this)), message_(new QLabel(this)),
      diffView_(new diffmerge::gui::FileDiffWidget(this))
{
    setObjectName("changesWindow");
    setWindowTitle("Changes — " + title);
    resize(1000, 700);
    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    since_->setObjectName("changesSince");
    since_->addItem("Since this turn", int(ChangesSince::Turn));
    since_->addItem("Since this chat", int(ChangesSince::Chat));
    since_->addItem("Since HEAD", int(ChangesSince::Head));
    since_->setToolTip("Since the latest turn started, since the chat's first turn started, or Git's view of "
                       "everything that differs from the current commit");
    summary_->setObjectName("changesSummary");
    summary_->setWordWrap(true);
    groupNew_->setObjectName("groupNewFiles");
    groupNew_->setChecked(true);
    groupNew_->setToolTip("List new files apart from changed ones");
    top->addWidget(since_);
    top->addWidget(groupNew_);
    top->addWidget(summary_, 1);
    layout->addLayout(top);

    list_->setObjectName("changedFiles");
    list_->setHeaderLabels({"", "File", "+", "−"});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    list_->header()->setSectionsClickable(true);
    list_->header()->setSortIndicatorShown(true);
    list_->header()->setSortIndicator(sortColumn_, Qt::AscendingOrder);
    // A click on File sorts by path, on + or − by that count, largest first.
    connect(list_->header(), &QHeaderView::sectionClicked, this, [this](int column) {
        const bool sorts = column >= 1 && column != sortColumn_;
        if (sorts) sortColumn_ = column;
        list_->header()->setSortIndicator(sortColumn_, sortColumn_ == 1 ? Qt::AscendingOrder : Qt::DescendingOrder);
        if (sorts) updateList();
    });
    list_->setToolTip("Double-click or press Enter to open a file with its default application");

    auto *diffPanel = new QWidget(this);
    auto *diffLayout = new QVBoxLayout(diffPanel);
    diffLayout->setContentsMargins(0, 0, 0, 0);
    auto *tools = new QHBoxLayout;
    view_->setObjectName("diffView");
    view_->addItem("Unified");
    view_->addItem("Side by side");
    skipUnchanged_->setObjectName("skipUnchanged");
    skipUnchanged_->setChecked(true);
    skipUnchanged_->setToolTip("Fold unchanged stretches beyond three lines around each change; click a folded "
                               "line to show it");
    previous_->setObjectName("previousChange");
    previous_->setArrowType(Qt::UpArrow);
    previous_->setToolTip("Previous change (Alt+Up)");
    next_->setObjectName("nextChange");
    next_->setArrowType(Qt::DownArrow);
    next_->setToolTip("Next change (Alt+Down)");
    tools->addWidget(view_);
    tools->addWidget(skipUnchanged_);
    tools->addWidget(previous_);
    tools->addWidget(next_);
    tools->addStretch(1);
    diffLayout->addLayout(tools);
    message_->setObjectName("diffMessage");
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    diffView_->setObjectName("fileDiff");
    diffView_->setPathBarVisible(false);
    diffView_->setNavigationBarVisible(false);
    diffView_->setViewMode(diffmerge::gui::ViewMode::Unified);
    diffView_->setUnchangedLinesSkipped(true);
    diffView_->setContextLines(kContextLines);
    pages_->addWidget(message_);
    pages_->addWidget(diffView_);
    diffLayout->addWidget(pages_, 1);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(list_);
    splitter->addWidget(diffPanel);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 500});
    layout->addWidget(splitter, 1);


    connect(since_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (tracker_) tracker_->setExtraSince(since());
        updateList();
    });
    connect(groupNew_, &QCheckBox::toggled, this, &ChangesWindow::updateList);
    connect(view_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        diffView_->setViewMode(index == 1 ? diffmerge::gui::ViewMode::SideBySide : diffmerge::gui::ViewMode::Unified);
    });
    connect(skipUnchanged_, &QCheckBox::toggled, diffView_, &diffmerge::gui::FileDiffWidget::setUnchangedLinesSkipped);
    connect(previous_, &QToolButton::clicked, diffView_, &diffmerge::gui::FileDiffWidget::navigateToPrev);
    connect(next_, &QToolButton::clicked, diffView_, &diffmerge::gui::FileDiffWidget::navigateToNext);
    connect(new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Up), this), &QShortcut::activated, diffView_,
            &diffmerge::gui::FileDiffWidget::navigateToPrev);
    connect(new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Down), this), &QShortcut::activated, diffView_,
            &diffmerge::gui::FileDiffWidget::navigateToNext);
    connect(list_, &QTreeWidget::currentItemChanged, this, &ChangesWindow::showSelected);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { openFile(item); });
    connect(list_, &QTreeWidget::customContextMenuRequested, this, &ChangesWindow::showMenu);
    flashEnd_ = new QTimer(this);
    flashEnd_->setSingleShot(true);
    flashEnd_->setInterval(1000);
    connect(flashEnd_, &QTimer::timeout, this, [this] {
        flashing_.clear();
        for (QTreeWidgetItemIterator it(list_); *it; ++it)
            for (int column = 0; column < list_->columnCount(); ++column) (*it)->setBackground(column, QBrush());
    });
    connect(tracker, &ChangeTracker::changesUpdated, this, &ChangesWindow::updateList);
    connect(tracker, &ChangeTracker::diffReady, this, [this](const FileDiff &diff) {
        if (diff.root + '\n' + diff.path != shownKey_) return;
        diff_ = diff;
        showDiff();
    });
    // The window belongs to the chat; it goes when the chat's tab closes.
    connect(tracker, &QObject::destroyed, this, &QObject::deleteLater);
    showMessage({});
    updateList();
}

ChangesSince ChangesWindow::since() const
{
    return ChangesSince(since_->currentData().toInt());
}

void ChangesWindow::closeEvent(QCloseEvent *event)
{
    // Only the latest turn is counted while nobody looks at another starting point.
    if (tracker_) tracker_->setExtraSince(ChangesSince::Turn);
    QWidget::closeEvent(event);
}

std::optional<FileChange> ChangesWindow::selectedChange() const
{
    const QTreeWidgetItem *item = list_->currentItem();
    if (!tracker_ || !item) return std::nullopt;
    const QString selected = item->data(1, Qt::UserRole).toString();
    for (const FileChange &change : tracker_->changes(since()))
        if (key(change) == selected) return change;
    return std::nullopt;
}

void ChangesWindow::updateList()
{
    if (!tracker_) return;
    if (isVisible()) tracker_->setExtraSince(since());
    QList<FileChange> changes = tracker_->changes(since());
    sortChanges(changes);
    const QDateTime started = tracker_->startedAt(since());
    QString text;
    if (since() == ChangesSince::Head) {
        text = tracker_->summary(since()).isEmpty() ? QString("No changes against HEAD")
                                                    : "Changes against HEAD: " + tracker_->summary(since());
    } else {
        const QString point = since() == ChangesSince::Turn ? "the turn" : "the chat's first turn";
        const QString from = started.isValid() ? " started at " + started.toString("HH:mm:ss") : QString(" started");
        text = tracker_->summary(since()).isEmpty() ? "No changes since " + point + from
                                                    : "Changes since " + point + from + ": " + tracker_->summary(since());
    }
    if (!tracker_->untracked().isEmpty()) {
        QStringList directories;
        for (const QString &directory : tracker_->untracked()) directories.append(QDir::toNativeSeparators(directory));
        text += "\nNot tracked, outside a Git work tree: " + directories.join(", ");
    }
    summary_->setText(text);

    const QTreeWidgetItem *current = list_->currentItem();
    const QString selected = current ? current->data(1, Qt::UserRole).toString() : QString();
    QSet<QString> roots;
    for (const FileChange &change : changes) roots.insert(change.root);
    // Files that changed since the previous refresh of the same list light up for a moment.
    QHash<QString, QString> states;
    for (const FileChange &change : changes) states.insert(key(change), changeState(change));
    if (listedSince_ == int(since()) && !listedStates_.isEmpty()) {
        for (auto it = states.cbegin(); it != states.cend(); ++it)
            if (listedStates_.value(it.key()) != it.value()) flashing_.insert(it.key());
        if (!flashing_.isEmpty()) flashEnd_->start();
    }
    listedStates_ = states;
    listedSince_ = int(since());
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    const QBrush flash(dark ? QColor(0x5c, 0x4b, 0x00) : QColor(0xff, 0xf1, 0x76));

    const int scroll = list_->verticalScrollBar()->value();
    const QSignalBlocker blocker(list_);
    list_->clear();
    QTreeWidgetItem *reselect = nullptr;
    int newCount = 0;
    for (const FileChange &change : changes)
        if (change.file.status == ChangedFile::Status::New) ++newCount;
    const bool grouped = groupNew_->isChecked() && newCount > 0;
    list_->setRootIsDecorated(grouped);
    QTreeWidgetItem *changedGroup = nullptr;
    QTreeWidgetItem *newGroup = nullptr;
    if (grouped) {
        QFont bold = list_->font();
        bold.setBold(true);
        if (newCount < changes.size()) {
            // A heading spans the row, which shows its first column.
            changedGroup = new QTreeWidgetItem(list_, {QString("Changed files (%1)").arg(changes.size() - newCount)});
            changedGroup->setFont(0, bold);
            changedGroup->setFirstColumnSpanned(true);
        }
        newGroup = new QTreeWidgetItem(list_, {QString("New files (%1)").arg(newCount)});
        newGroup->setFont(0, bold);
        newGroup->setFirstColumnSpanned(true);
    }
    for (const FileChange &change : changes) {
        // With several work trees, paths are shown in full so that their files can be told apart.
        QString path = roots.size() > 1 ? QDir(change.root).filePath(change.file.path) : change.file.path;
        if (change.file.status == ChangedFile::Status::Renamed)
            path = (roots.size() > 1 ? QDir(change.root).filePath(change.file.oldPath) : change.file.oldPath) + " → " + path;
        QString added;
        QString removed;
        switch (change.lines.kind) {
        case LineChanges::Kind::Counted:
            // A new file's lines count as added; it has nothing removed to show.
            added = "+" + QString::number(change.lines.added);
            if (change.file.status != ChangedFile::Status::New) removed = "−" + QString::number(change.lines.removed);
            break;
        case LineChanges::Kind::Binary:
            path += "  (binary, " + byteSize(change.file.sizeBefore) + " → " + byteSize(change.file.sizeAfter) + ")";
            break;
        case LineChanges::Kind::TooLarge:
            path += "  (too large to compare lines, " + byteSize(change.file.sizeAfter) + ")";
            break;
        case LineChanges::Kind::Rewritten:
            path += QString("  (rewritten, %1 → %2 lines)").arg(change.lines.linesBefore).arg(change.lines.linesAfter);
            break;
        case LineChanges::Kind::Cancelled:
            break;
        }
        const QStringList columns{statusLetter(change.file.status), QDir::toNativeSeparators(path), added, removed};
        QTreeWidgetItem *group = !grouped ? nullptr
            : change.file.status == ChangedFile::Status::New ? newGroup : changedGroup;
        auto *item = group ? new QTreeWidgetItem(group, columns) : new QTreeWidgetItem(list_, columns);
        item->setData(1, Qt::UserRole, key(change));
        item->setToolTip(1, QDir::toNativeSeparators(QDir(change.root).filePath(change.file.path)));
        item->setForeground(2, QColor(0x1a, 0x7f, 0x37));
        item->setForeground(3, QColor(0xcf, 0x22, 0x2e));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        if (key(change) == selected) reselect = item;
        if (flashing_.contains(key(change)))
            for (int column = 0; column < list_->columnCount(); ++column) item->setBackground(column, flash);
    }
    list_->expandAll();
    if (reselect) list_->setCurrentItem(reselect);
    // A refresh keeps the place the list was scrolled to.
    list_->verticalScrollBar()->setValue(scroll);
    showSelected();
}

// By the chosen column, counts largest first; equal counts of added lines go by removed ones and the other way round, then by
// path. Files whose lines were not counted, such as binary ones, count below zero.
void ChangesWindow::sortChanges(QList<FileChange> &changes) const
{
    const auto counts = [](const FileChange &change) {
        if (change.lines.kind != LineChanges::Kind::Counted) return std::pair{-1, -1};
        return std::pair{change.lines.added, change.file.status == ChangedFile::Status::New ? 0 : change.lines.removed};
    };
    const auto path = [](const FileChange &change) { return QDir(change.root).filePath(change.file.path); };
    std::stable_sort(changes.begin(), changes.end(), [&](const FileChange &a, const FileChange &b) {
        if (sortColumn_ != 1) {
            auto [aAdded, aRemoved] = counts(a);
            auto [bAdded, bRemoved] = counts(b);
            if (sortColumn_ == 3) {
                std::swap(aAdded, aRemoved);
                std::swap(bAdded, bRemoved);
            }
            if (aAdded != bAdded) return aAdded > bAdded;
            if (aRemoved != bRemoved) return aRemoved > bRemoved;
        }
        return QString::compare(path(a), path(b), Qt::CaseInsensitive) < 0;
    });
}

// A diff is requested again only when the selected file changed since it was shown.
void ChangesWindow::showSelected()
{
    const std::optional<FileChange> change = selectedChange();
    if (!change) {
        shownKey_.clear();
        shownState_.clear();
        diff_.reset();
        showDiff();
        return;
    }
    const QString state = changeState(*change);
    if (key(*change) == shownKey_ && state == shownState_) return;
    // The same file's refreshed diff replaces the shown one where it is scrolled to.
    if (key(*change) != shownKey_) {
        diff_.reset();
        showMessage("Computing the differences…");
    }
    shownKey_ = key(*change);
    shownState_ = state;
    tracker_->requestDiff(*change);
}

void ChangesWindow::showMessage(const QString &text)
{
    message_->setText(text);
    pages_->setCurrentWidget(message_);
    diffView_->clearComparison();
    previous_->setEnabled(false);
    next_->setEnabled(false);
}

void ChangesWindow::showDiff()
{
    if (!diff_) {
        showMessage(list_->topLevelItemCount() ? "Select a file to see its differences." : QString());
        return;
    }
    if (!diff_->comparison) {
        showMessage(diff_->note);
        return;
    }
    if (diff_->comparison->changes().isEmpty()) {
        showMessage("The contents are the same.");
        return;
    }
    // A refreshed diff of the shown file keeps the place it is scrolled to.
    const bool sameFile = pages_->currentWidget() == diffView_ && diffView_->comparison();
    const auto scrollBar = [this] {
        const diffmerge::gui::DiffEditor *editor =
            diffView_->viewMode() == diffmerge::gui::ViewMode::Unified ? diffView_->unifiedEditor() : diffView_->rightEditor();
        return editor->edit()->area()->verticalScrollBar();
    };
    const int scroll = sameFile ? scrollBar()->value() : 0;
    diffView_->setComparison(diff_->comparison);
    pages_->setCurrentWidget(diffView_);
    if (sameFile) scrollBar()->setValue(scroll);
    previous_->setEnabled(true);
    next_->setEnabled(true);
}

void ChangesWindow::openFile(QTreeWidgetItem *item) const
{
    if (!item || !tracker_ || item->data(1, Qt::UserRole).toString().isEmpty()) return;
    const QString itemKey = item->data(1, Qt::UserRole).toString();
    const QStringList parts = itemKey.split('\n');
    const QString path = QDir(parts.value(0)).filePath(parts.value(1));
    if (!QFileInfo(path).isFile()) return;
    // The shown diff gives the first changed line of the file, where an editor can open it.
    int line = 1;
    if (diff_ && diff_->comparison && diff_->root + '\n' + diff_->path == itemKey && !diff_->comparison->changes().isEmpty())
        line = diff_->comparison->changes().first().rightRange.start + 1;
    if (opener_) opener_(path, line);
    else QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ChangesWindow::showMenu(const QPoint &position)
{
    QTreeWidgetItem *item = list_->itemAt(position);
    // Group headings are not files.
    if (!item || item->data(1, Qt::UserRole).toString().isEmpty()) return;
    const QStringList parts = item->data(1, Qt::UserRole).toString().split('\n');
    const QString path = QDir(parts.value(0)).filePath(parts.value(1));
    QMenu menu(this);
    QAction *open = menu.addAction("Open");
    open->setEnabled(QFileInfo(path).isFile());
    QAction *folder = menu.addAction("Open containing folder");
    QAction *copy = menu.addAction("Copy path");
    QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(position));
    if (chosen == open) openFile(item);
    else if (chosen == folder) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    else if (chosen == copy) QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
}
