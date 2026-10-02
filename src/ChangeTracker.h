#pragma once

#include "GitBaseline.h"
#include "LineChanges.h"

#include <diffcore/DiffTypes.h>

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <atomic>
#include <memory>

class QThread;

// What changes are compared with: the start of the latest turn, the start of the chat's first turn, or
// the current Git HEAD, which also shows changes made before the chat.
enum class ChangesSince { Turn, Chat, Head };

// A changed file with its Git work tree and its line counts.
struct FileChange
{
    ChangesSince since = ChangesSince::Turn;
    QString root;
    ChangedFile file;
    LineChanges lines;
};
Q_DECLARE_METATYPE(QList<FileChange>)

// The diff of one changed file, or the reason it has none.
struct FileDiff
{
    QString root;
    QString path;
    QStringList before;
    QStringList after;
    QList<diffcore::Hunk> hunks;
    QString note;
    // A new file is shown as its content, in after, without a diff.
    bool newFile = false;
};
Q_DECLARE_METATYPE(FileDiff)

// The files a chat changed in the directories it holds. The Git work is done in a worker thread, where
// the baselines live, and line counts are cached by file size and modification time. The latest turn is
// always counted; another starting point is counted while something, such as the changes window, needs it.
class ChangeTracker : public QObject
{
    Q_OBJECT

public:
    explicit ChangeTracker(QObject *parent = nullptr);
    ~ChangeTracker() override;

    // Records the start of a turn in these directories; the first turn also starts the chat's baseline.
    void turnStarted(const QStringList &directories);
    void refresh();
    // Also counts changes since this point, or only the latest turn's for ChangesSince::Turn.
    void setExtraSince(ChangesSince since);
    // Computes the file's diff in the worker thread and reports it with diffReady().
    void requestDiff(const FileChange &change);
    // Invalid for ChangesSince::Head, which has no fixed start.
    QDateTime startedAt(ChangesSince since = ChangesSince::Turn) const;
    QList<FileChange> changes(ChangesSince since = ChangesSince::Turn) const { return changes_.value(since); }
    // Directories that are not in a Git work tree, whose changes are not tracked.
    QStringList untracked() const { return untracked_; }
    // "7 files, +120 −34", or empty when nothing changed.
    QString summary(ChangesSince since = ChangesSince::Turn) const;
    struct Totals
    {
        int files = 0;
        int added = 0;
        int removed = 0;
    };
    // Lines of files that could be counted; binary and too large files count only as files.
    Totals totals(ChangesSince since = ChangesSince::Turn) const;

signals:
    void changesUpdated();
    void diffReady(const FileDiff &diff);

private:
    class Worker;
    QThread *thread_;
    Worker *worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    QHash<ChangesSince, QList<FileChange>> changes_;
    QStringList untracked_;
    QDateTime turnStartedAt_;
    QDateTime chatStartedAt_;
    ChangesSince extraSince_ = ChangesSince::Turn;
};
