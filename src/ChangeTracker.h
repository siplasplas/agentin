#pragma once

#include "GitBaseline.h"
#include "LineChanges.h"

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
    QList<FileChange> changes() const { return changes_; }
    // Directories that are not in a Git work tree, whose changes are not tracked.
    QStringList untracked() const { return untracked_; }
    // "7 files, +120 −34", or empty when nothing changed.
    QString summary() const;

signals:
    void changesUpdated();

private:
    class Worker;
    QThread *thread_;
    Worker *worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    QList<FileChange> changes_;
    QStringList untracked_;
};
