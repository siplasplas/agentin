#pragma once

#include "GitBaseline.h"
#include "LineChanges.h"

#include <diffcore/DiffTypes.h>

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class QThread;

// A changed file with its Git work tree and its line counts.
struct FileChange
{
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
};
Q_DECLARE_METATYPE(FileDiff)

// The files a chat's turn changed in the directories it holds. The Git work is done in a worker thread,
// where the baselines live, and line counts are cached by file size and modification time.
class ChangeTracker : public QObject
{
    Q_OBJECT

public:
    explicit ChangeTracker(QObject *parent = nullptr);
    ~ChangeTracker() override;

    // Records the start of a turn in these directories; earlier changes are forgotten.
    void turnStarted(const QStringList &directories);
    void refresh();
    // Computes the file's diff in the worker thread and reports it with diffReady().
    void requestDiff(const FileChange &change);
    QDateTime startedAt() const { return startedAt_; }
    QList<FileChange> changes() const { return changes_; }
    // Directories that are not in a Git work tree, whose changes are not tracked.
    QStringList untracked() const { return untracked_; }
    // "7 files, +120 −34", or empty when nothing changed.
    QString summary() const;

signals:
    void changesUpdated();
    void diffReady(const FileDiff &diff);

private:
    class Worker;
    QThread *thread_;
    Worker *worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    QList<FileChange> changes_;
    QStringList untracked_;
    QDateTime startedAt_;
};
