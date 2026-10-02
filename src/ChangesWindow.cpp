#include "ChangesWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

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

struct Line
{
    char kind;   // ' ', '-' or '+'
    int before;  // 0-based line in the old file, or -1
    int after;   // 0-based line in the new file, or -1
};
}

ChangesWindow::ChangesWindow(ChangeTracker *tracker, const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window), tracker_(tracker), summary_(new QLabel(this)), list_(new QTreeWidget(this)),
      diff_(new QPlainTextEdit(this))
{
    setObjectName("changesWindow");
    setWindowTitle("Changes — " + title);
    resize(900, 650);
    auto *layout = new QVBoxLayout(this);
    summary_->setObjectName("changesSummary");
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
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
    diff_->setObjectName("fileDiff");
    diff_->setReadOnly(true);
    diff_->setLineWrapMode(QPlainTextEdit::NoWrap);
    diff_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(list_);
    splitter->addWidget(diff_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 450});
    layout->addWidget(splitter, 1);

    connect(list_, &QTreeWidget::currentItemChanged, this, &ChangesWindow::showSelected);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { openFile(item); });
    connect(list_, &QTreeWidget::customContextMenuRequested, this, &ChangesWindow::showMenu);
    connect(tracker, &ChangeTracker::changesUpdated, this, &ChangesWindow::updateList);
    connect(tracker, &ChangeTracker::diffReady, this, [this](const FileDiff &diff) {
        if (diff.root + '\n' + diff.path == shownKey_) showDiff(diff);
    });
    // The window belongs to the chat; it goes when the chat's tab closes.
    connect(tracker, &QObject::destroyed, this, &QObject::deleteLater);
    updateList();
}

std::optional<FileChange> ChangesWindow::selectedChange() const
{
    const QTreeWidgetItem *item = list_->currentItem();
    if (!tracker_ || !item) return std::nullopt;
    const QString selected = item->data(1, Qt::UserRole).toString();
    for (const FileChange &change : tracker_->changes())
        if (key(change) == selected) return change;
    return std::nullopt;
}

void ChangesWindow::updateList()
{
    if (!tracker_) return;
    const QList<FileChange> changes = tracker_->changes();
    const QString since = tracker_->startedAt().isValid()
        ? "since the turn started at " + tracker_->startedAt().toString("HH:mm:ss") : QString("since the turn started");
    QString text = tracker_->summary().isEmpty() ? "No changes " + since : "Changes " + since + ": " + tracker_->summary();
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
                path += QString("  (new, %1 lines)").arg(change.lines.added);
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
        auto *item = new QTreeWidgetItem(list_, {statusLetter(change.file.status), QDir::toNativeSeparators(path), added, removed});
        item->setData(1, Qt::UserRole, key(change));
        item->setToolTip(1, QDir::toNativeSeparators(QDir(change.root).filePath(change.file.path)));
        item->setForeground(2, QColor(0x1a, 0x7f, 0x37));
        item->setForeground(3, QColor(0xcf, 0x22, 0x2e));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        if (key(change) == selected) reselect = item;
    }
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
        diff_->setPlainText(list_->topLevelItemCount() ? "Select a file to see its differences." : QString());
        return;
    }
    const QString state = QString("%1|%2|%3|%4|%5").arg(int(change->file.status)).arg(change->file.sizeAfter)
                              .arg(change->lines.added).arg(change->lines.removed).arg(int(change->lines.kind));
    if (key(*change) == shownKey_ && state == shownState_) return;
    shownKey_ = key(*change);
    shownState_ = state;
    diff_->setPlainText("Computing the differences…");
    tracker_->requestDiff(*change);
}

void ChangesWindow::showDiff(const FileDiff &diff)
{
    diff_->clear();
    if (!diff.note.isEmpty()) {
        diff_->setPlainText(diff.note);
        return;
    }
    // Replace hunks show their old lines first, as unified diffs do.
    QList<Line> lines;
    for (const diffcore::Hunk &hunk : diff.hunks) {
        if (hunk.type == diffcore::ChangeType::Equal) {
            for (int i = 0; i < hunk.leftRange.count; ++i) lines.append({' ', hunk.leftRange.start + i, hunk.rightRange.start + i});
            continue;
        }
        for (int i = 0; i < hunk.leftRange.count; ++i) lines.append({'-', hunk.leftRange.start + i, -1});
        for (int i = 0; i < hunk.rightRange.count; ++i) lines.append({'+', -1, hunk.rightRange.start + i});
    }
    QList<bool> shown(lines.size(), false);
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).kind == ' ') continue;
        for (int j = qMax(0, i - kContextLines); j <= qMin(int(lines.size()) - 1, i + kContextLines); ++j) shown[j] = true;
    }
    if (!shown.contains(true)) {
        diff_->setPlainText("The contents are the same.");
        return;
    }

    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    QTextBlockFormat addedFormat;
    addedFormat.setBackground(dark ? QColor(0x1f, 0x3d, 0x27) : QColor(0xe6, 0xff, 0xec));
    QTextBlockFormat removedFormat;
    removedFormat.setBackground(dark ? QColor(0x4b, 0x1f, 0x24) : QColor(0xff, 0xeb, 0xe9));
    QTextBlockFormat headerFormat;
    headerFormat.setBackground(dark ? QColor(0x1c, 0x2b, 0x3a) : QColor(0xdd, 0xf4, 0xff));
    const QTextBlockFormat plainFormat;
    const int width = QString::number(qMax(diff.before.size(), diff.after.size())).size();
    QTextCursor cursor(diff_->document());
    cursor.beginEditBlock();
    bool first = true;
    int written = 0;
    const auto add = [&](const QString &text, const QTextBlockFormat &format) {
        if (!first) cursor.insertBlock();
        first = false;
        cursor.setBlockFormat(format);
        cursor.insertText(text);
        ++written;
    };
    for (int i = 0; i < lines.size() && written < kMaximumShownLines; ++i) {
        if (!shown.at(i)) continue;
        if (i == 0 || !shown.at(i - 1)) {
            int end = i;
            while (end < lines.size() && shown.at(end)) ++end;
            int beforeStart = -1, beforeCount = 0, afterStart = -1, afterCount = 0;
            for (int j = i; j < end; ++j) {
                if (lines.at(j).before >= 0) {
                    if (beforeStart < 0) beforeStart = lines.at(j).before;
                    ++beforeCount;
                }
                if (lines.at(j).after >= 0) {
                    if (afterStart < 0) afterStart = lines.at(j).after;
                    ++afterCount;
                }
            }
            add(QString("@@ -%1,%2 +%3,%4 @@").arg(beforeStart + 1).arg(beforeCount).arg(afterStart + 1).arg(afterCount),
                headerFormat);
        }
        const Line &line = lines.at(i);
        const QString before = line.before >= 0 ? QString::number(line.before + 1) : QString();
        const QString after = line.after >= 0 ? QString::number(line.after + 1) : QString();
        const QString text = line.after >= 0 ? diff.after.value(line.after) : diff.before.value(line.before);
        add(before.rightJustified(width) + ' ' + after.rightJustified(width) + "  " + QChar(line.kind) + ' ' + text,
            line.kind == '+' ? addedFormat : line.kind == '-' ? removedFormat : plainFormat);
    }
    if (written >= kMaximumShownLines) add(QString("… the diff is cut after %1 lines").arg(kMaximumShownLines), headerFormat);
    cursor.endEditBlock();
    diff_->moveCursor(QTextCursor::Start);
}

void ChangesWindow::openFile(QTreeWidgetItem *item) const
{
    if (!item || !tracker_) return;
    const QStringList parts = item->data(1, Qt::UserRole).toString().split('\n');
    const QString path = QDir(parts.value(0)).filePath(parts.value(1));
    if (QFileInfo(path).isFile()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ChangesWindow::showMenu(const QPoint &position)
{
    QTreeWidgetItem *item = list_->itemAt(position);
    if (!item) return;
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
