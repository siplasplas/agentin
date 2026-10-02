#include "ChangesWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kContextLines = 3;
constexpr int kMaximumShownLines = 20000;

QString key(const FileChange &change)
{
    return change.root + '\n' + change.file.path;
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

QPlainTextEdit *diffView(QWidget *parent, const QString &name)
{
    auto *view = new QPlainTextEdit(parent);
    view->setObjectName(name);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->viewport()->setCursor(Qt::ArrowCursor);
    return view;
}
}

ChangesWindow::ChangesWindow(ChangeTracker *tracker, const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window), tracker_(tracker), summary_(new QLabel(this)), since_(new QComboBox(this)),
      groupNew_(new QCheckBox("Group new files", this)), list_(new QTreeWidget(this)), view_(new QComboBox(this)), previous_(new QToolButton(this)),
      next_(new QToolButton(this)), pages_(new QStackedWidget(this)), unified_(diffView(this, "fileDiff")),
      before_(diffView(this, "fileDiffBefore")), after_(diffView(this, "fileDiffAfter"))
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
    list_->setToolTip("Double-click or press Enter to open a file with its default application");

    auto *diffPanel = new QWidget(this);
    auto *diffLayout = new QVBoxLayout(diffPanel);
    diffLayout->setContentsMargins(0, 0, 0, 0);
    auto *tools = new QHBoxLayout;
    view_->setObjectName("diffView");
    view_->addItem("Unified");
    view_->addItem("Side by side");
    previous_->setObjectName("previousChange");
    previous_->setArrowType(Qt::UpArrow);
    previous_->setToolTip("Previous change (Alt+Up)");
    next_->setObjectName("nextChange");
    next_->setArrowType(Qt::DownArrow);
    next_->setToolTip("Next change (Alt+Down)");
    tools->addWidget(view_);
    tools->addWidget(previous_);
    tools->addWidget(next_);
    tools->addWidget(new QLabel("Click a folded line to show it.", diffPanel), 1);
    diffLayout->addLayout(tools);
    auto *sides = new QSplitter(Qt::Horizontal, this);
    sides->addWidget(before_);
    sides->addWidget(after_);
    pages_->addWidget(unified_);
    pages_->addWidget(sides);
    diffLayout->addWidget(pages_, 1);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(list_);
    splitter->addWidget(diffPanel);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 500});
    layout->addWidget(splitter, 1);

    // The two sides scroll together.
    for (auto [from, to] : {std::pair{before_, after_}, std::pair{after_, before_}}) {
        connect(from->verticalScrollBar(), &QScrollBar::valueChanged, to->verticalScrollBar(), &QScrollBar::setValue);
        connect(from->horizontalScrollBar(), &QScrollBar::valueChanged, to->horizontalScrollBar(), &QScrollBar::setValue);
    }
    for (QPlainTextEdit *view : {unified_, before_, after_}) view->viewport()->installEventFilter(this);

    connect(since_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (tracker_) tracker_->setExtraSince(since());
        updateList();
    });
    connect(groupNew_, &QCheckBox::toggled, this, &ChangesWindow::updateList);
    connect(view_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        pages_->setCurrentIndex(index);
        render();
    });
    connect(previous_, &QToolButton::clicked, this, [this] { moveToChange(-1); });
    connect(next_, &QToolButton::clicked, this, [this] { moveToChange(1); });
    connect(new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Up), this), &QShortcut::activated, this, [this] { moveToChange(-1); });
    connect(new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Down), this), &QShortcut::activated, this, [this] { moveToChange(1); });
    connect(list_, &QTreeWidget::currentItemChanged, this, &ChangesWindow::showSelected);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { openFile(item); });
    connect(list_, &QTreeWidget::customContextMenuRequested, this, &ChangesWindow::showMenu);
    connect(tracker, &ChangeTracker::changesUpdated, this, &ChangesWindow::updateList);
    connect(tracker, &ChangeTracker::diffReady, this, [this](const FileDiff &diff) {
        if (diff.root + '\n' + diff.path != shownKey_) return;
        const bool sameFile = diff_ && diff_->root == diff.root && diff_->path == diff.path;
        if (!sameFile) {
            openedFolds_.clear();
            currentChange_ = -1;
        }
        diff_ = diff;
        render();
    });
    // The window belongs to the chat; it goes when the chat's tab closes.
    connect(tracker, &QObject::destroyed, this, &QObject::deleteLater);
    pages_->setCurrentIndex(0);
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
    const QList<FileChange> changes = tracker_->changes(since());
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
            if (change.file.status == ChangedFile::Status::New) {
                path += QString("  (new, %1 %2)").arg(change.lines.added).arg(change.lines.added == 1 ? "line" : "lines");
            } else {
                added = "+" + QString::number(change.lines.added);
                removed = "−" + QString::number(change.lines.removed);
            }
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
    }
    list_->expandAll();
    if (reselect) list_->setCurrentItem(reselect);
    showSelected();
}

// A diff is requested again only when the selected file changed since it was shown.
void ChangesWindow::showSelected()
{
    const std::optional<FileChange> change = selectedChange();
    if (!change) {
        shownKey_.clear();
        shownState_.clear();
        diff_.reset();
        render();
        return;
    }
    const QString state = QString("%1|%2|%3|%4|%5|%6").arg(int(change->since)).arg(int(change->file.status))
                              .arg(change->file.sizeAfter).arg(change->lines.added).arg(change->lines.removed)
                              .arg(int(change->lines.kind));
    if (key(*change) == shownKey_ && state == shownState_) return;
    shownKey_ = key(*change);
    shownState_ = state;
    for (QPlainTextEdit *view : {unified_, before_, after_}) view->setPlainText("Computing the differences…");
    tracker_->requestDiff(*change);
}

// The diff's lines in order, a replaced region's old lines first. Unchanged stretches beyond the context
// around changes fold into one row unless opened.
QList<ChangesWindow::Row> ChangesWindow::rows(bool sideBySide) const
{
    struct Line { char kind; int before; int after; };
    QList<Line> lines;
    for (const diffcore::Hunk &hunk : diff_->hunks) {
        if (hunk.type == diffcore::ChangeType::Equal) {
            for (int i = 0; i < hunk.leftRange.count; ++i) lines.append({' ', hunk.leftRange.start + i, hunk.rightRange.start + i});
            continue;
        }
        for (int i = 0; i < hunk.leftRange.count; ++i) lines.append({'-', hunk.leftRange.start + i, -1});
        for (int i = 0; i < hunk.rightRange.count; ++i) lines.append({'+', -1, hunk.rightRange.start + i});
    }
    // Adjacent hunks can alternate insertions and deletions; within one changed region the old lines go
    // first, so that unified diffs read as usual and both sides line up.
    for (int start = 0; start < lines.size();) {
        if (lines.at(start).kind == ' ') {
            ++start;
            continue;
        }
        int end = start;
        while (end < lines.size() && lines.at(end).kind != ' ') ++end;
        std::stable_partition(lines.begin() + start, lines.begin() + end, [](const Line &line) { return line.kind == '-'; });
        start = end;
    }
    QList<bool> shown(lines.size(), false);
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).kind == ' ') continue;
        for (int j = qMax(0, i - kContextLines); j <= qMin(int(lines.size()) - 1, i + kContextLines); ++j) shown[j] = true;
    }
    QList<Row> result;
    for (int i = 0; i < lines.size();) {
        if (!shown.at(i)) {
            int end = i;
            while (end < lines.size() && !shown.at(end)) ++end;
            if (!openedFolds_.contains(i)) {
                result.append({Row::Fold, -1, -1, i, end - i});
                i = end;
                continue;
            }
            for (; i < end; ++i) result.append({Row::Context, lines.at(i).before, lines.at(i).after});
            continue;
        }
        const Line &line = lines.at(i);
        if (line.kind == ' ') {
            result.append({Row::Context, line.before, line.after});
            ++i;
        } else if (!sideBySide) {
            result.append({line.kind == '-' ? Row::Removed : Row::Added, line.before, line.after});
            ++i;
        } else {
            // Side by side, removed and added lines of one region face each other.
            QList<int> removed;
            QList<int> added;
            while (i < lines.size() && lines.at(i).kind == '-') removed.append(lines.at(i++).before);
            while (i < lines.size() && lines.at(i).kind == '+') added.append(lines.at(i++).after);
            for (int j = 0; j < qMax(removed.size(), added.size()); ++j) {
                if (j < removed.size() && j < added.size()) result.append({Row::Changed, removed.at(j), added.at(j)});
                else if (j < removed.size()) result.append({Row::Removed, removed.at(j), -1});
                else result.append({Row::Added, -1, added.at(j)});
            }
        }
    }
    return result;
}

void ChangesWindow::render()
{
    changeStarts_.clear();
    const bool sideBySide = view_->currentIndex() == 1;
    QPlainTextEdit *main = sideBySide ? before_ : unified_;
    const int scroll = main->verticalScrollBar()->value();
    for (QPlainTextEdit *view : {unified_, before_, after_}) view->clear();
    if (!diff_) {
        const QString text = list_->topLevelItemCount() ? "Select a file to see its differences." : QString();
        for (QPlainTextEdit *view : {unified_, before_, after_}) view->setPlainText(text);
        previous_->setEnabled(false);
        next_->setEnabled(false);
        return;
    }
    if (diff_->newFile) {
        // A new file is read as it is: its whole content with line numbers, without diff colors.
        const int width = QString::number(diff_->after.size()).size();
        QStringList lines;
        lines.reserve(qMin(int(diff_->after.size()), kMaximumShownLines));
        for (int i = 0; i < diff_->after.size() && i < kMaximumShownLines; ++i)
            lines.append(QString::number(i + 1).rightJustified(width) + "  " + diff_->after.at(i));
        if (diff_->after.size() > kMaximumShownLines)
            lines.append(QString("\u2026 the file is cut after %1 lines").arg(kMaximumShownLines));
        unified_->setPlainText(lines.join('\n'));
        after_->setPlainText(lines.join('\n'));
        before_->setPlainText(QString("New file with %1 %2").arg(diff_->after.size()).arg(diff_->after.size() == 1 ? "line" : "lines"));
        previous_->setEnabled(false);
        next_->setEnabled(false);
        return;
    }
    if (!diff_->note.isEmpty() || diff_->hunks.isEmpty()) {
        const QString text = diff_->note.isEmpty() ? QString("The contents are the same.") : diff_->note;
        for (QPlainTextEdit *view : {unified_, before_, after_}) view->setPlainText(text);
        previous_->setEnabled(false);
        next_->setEnabled(false);
        return;
    }

    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    QTextBlockFormat addedFormat;
    addedFormat.setBackground(dark ? QColor(0x1f, 0x3d, 0x27) : QColor(0xe6, 0xff, 0xec));
    QTextBlockFormat removedFormat;
    removedFormat.setBackground(dark ? QColor(0x4b, 0x1f, 0x24) : QColor(0xff, 0xeb, 0xe9));
    QTextBlockFormat foldFormat;
    foldFormat.setBackground(dark ? QColor(0x1c, 0x2b, 0x3a) : QColor(0xdd, 0xf4, 0xff));
    QTextBlockFormat emptyFormat;
    emptyFormat.setBackground(palette().color(QPalette::AlternateBase));
    const QTextBlockFormat plainFormat;
    const int width = QString::number(qMax(diff_->before.size(), diff_->after.size())).size();
    const auto number = [width](int line) { return (line >= 0 ? QString::number(line + 1) : QString()).rightJustified(width); };

    QTextCursor unified(unified_->document());
    QTextCursor left(before_->document());
    QTextCursor right(after_->document());
    for (QTextCursor *cursor : {&unified, &left, &right}) cursor->beginEditBlock();
    bool first = true;
    const auto add = [&first](QTextCursor &cursor, const QString &text, const QTextBlockFormat &format, int fold) {
        if (!first) cursor.insertBlock();
        cursor.setBlockFormat(format);
        cursor.insertText(text);
        // Folded rows remember where they start, so a click can open them.
        cursor.block().setUserState(fold);
    };
    const QList<Row> shownRows = rows(sideBySide);
    for (int i = 0; i < shownRows.size() && i < kMaximumShownLines; ++i) {
        const Row &row = shownRows.at(i);
        const bool change = row.kind != Row::Context && row.kind != Row::Fold;
        const bool previousChange = i > 0 && shownRows.at(i - 1).kind != Row::Context && shownRows.at(i - 1).kind != Row::Fold;
        if (change && !previousChange) changeStarts_.append(i);
        const QString beforeText = row.before >= 0 ? diff_->before.value(row.before) : QString();
        const QString afterText = row.after >= 0 ? diff_->after.value(row.after) : QString();
        if (row.kind == Row::Fold) {
            const QString text = QString("⋯ %1 unchanged %2 — click to show").arg(row.count).arg(row.count == 1 ? "line" : "lines");
            if (sideBySide) {
                add(left, text, foldFormat, row.fold);
                add(right, text, foldFormat, row.fold);
            } else {
                add(unified, text, foldFormat, row.fold);
            }
        } else if (sideBySide) {
            const bool leftUsed = row.before >= 0;
            const bool rightUsed = row.after >= 0;
            add(left, leftUsed ? number(row.before) + "  " + beforeText : QString(),
                !leftUsed ? emptyFormat : row.kind == Row::Context ? plainFormat : removedFormat, -1);
            add(right, rightUsed ? number(row.after) + "  " + afterText : QString(),
                !rightUsed ? emptyFormat : row.kind == Row::Context ? plainFormat : addedFormat, -1);
        } else {
            const char marker = row.kind == Row::Removed ? '-' : row.kind == Row::Added ? '+' : ' ';
            const QString text = row.kind == Row::Added ? afterText : beforeText;
            add(unified, number(row.before) + ' ' + number(row.after) + "  " + QChar(marker) + ' ' + text,
                row.kind == Row::Removed ? removedFormat : row.kind == Row::Added ? addedFormat : plainFormat, -1);
        }
        first = false;
    }
    if (shownRows.size() > kMaximumShownLines) {
        const QString text = QString("… the diff is cut after %1 lines").arg(kMaximumShownLines);
        if (sideBySide) {
            add(left, text, foldFormat, -1);
            add(right, text, foldFormat, -1);
        } else {
            add(unified, text, foldFormat, -1);
        }
    }
    for (QTextCursor *cursor : {&unified, &left, &right}) cursor->endEditBlock();
    main->verticalScrollBar()->setValue(scroll);
    previous_->setEnabled(!changeStarts_.isEmpty());
    next_->setEnabled(!changeStarts_.isEmpty());
    if (currentChange_ >= changeStarts_.size()) currentChange_ = int(changeStarts_.size()) - 1;
}

void ChangesWindow::moveToChange(int step)
{
    if (changeStarts_.isEmpty()) return;
    currentChange_ = qBound(0, currentChange_ < 0 && step < 0 ? 0 : currentChange_ + step, int(changeStarts_.size()) - 1);
    QPlainTextEdit *view = view_->currentIndex() == 1 ? before_ : unified_;
    view->setTextCursor(QTextCursor(view->document()->findBlockByNumber(changeStarts_.at(currentChange_))));
    view->centerCursor();
}

// A click on a folded row opens it in place.
bool ChangesWindow::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        for (QPlainTextEdit *view : {unified_, before_, after_}) {
            if (object != view->viewport()) continue;
            const QTextBlock block = view->cursorForPosition(static_cast<QMouseEvent *>(event)->position().toPoint()).block();
            if (block.userState() >= 0) {
                openedFolds_.insert(block.userState());
                render();
                return true;
            }
        }
    }
    return QWidget::eventFilter(object, event);
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
    if (diff_ && diff_->root + '\n' + diff_->path == itemKey) {
        for (const diffcore::Hunk &hunk : diff_->hunks) {
            if (hunk.type == diffcore::ChangeType::Equal) continue;
            line = hunk.rightRange.start + 1;
            break;
        }
    }
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
