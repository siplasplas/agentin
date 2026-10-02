#include "ChangeTracker.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QThread>

#include <diffcore/DiffEngine.h>

#include <vector>

class ChangeTracker::Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(std::shared_ptr<std::atomic<bool>> cancel) : cancel_(std::move(cancel)) {}

    void capture(const QStringList &directories)
    {
        baselines_.clear();
        cache_.clear();
        QStringList untracked;
        QStringList taken;
        for (const QString &directory : directories) {
            const QString clean = QDir::cleanPath(directory);
            // A directory inside one already taken would report the same files twice.
            bool nested = false;
            for (const QString &other : taken)
                if (clean == other || clean.startsWith(other + '/')) nested = true;
            if (nested || clean.isEmpty()) continue;
            taken.append(clean);
            auto baseline = std::make_unique<GitBaseline>(clean);
            if (!baseline->isValid() || !baseline->capture()) {
                untracked.append(clean);
                continue;
            }
            baselines_.push_back(std::move(baseline));
        }
        emit untrackedFound(untracked);
        count();
    }

    void count()
    {
        QList<FileChange> result;
        for (const auto &baseline : baselines_) {
            for (const ChangedFile &file : baseline->changes()) {
                if (cancel_->load()) return;
                FileChange change{baseline->root(), file, {}};
                const QFileInfo info(QDir(baseline->root()).filePath(file.path));
                const QString key = baseline->root() + '/' + file.path;
                const auto cached = cache_.constFind(key);
                if (cached != cache_.constEnd() && cached->status == file.status && cached->size == info.size()
                    && cached->modified == info.lastModified()) {
                    change.lines = cached->lines;
                } else {
                    // A new file is shown without a diff, so only its lines are counted.
                    change.lines = countLineChanges(file.status == ChangedFile::Status::New ? QByteArray()
                                                                                            : baseline->contentBefore(file),
                                                    baseline->contentAfter(file), cancel_.get());
                    if (change.lines.kind == LineChanges::Kind::Cancelled) return;
                    cache_.insert(key, {file.status, info.size(), info.lastModified(), change.lines});
                }
                result.append(change);
            }
        }
        emit counted(result);
    }

    // New, binary, too large and rewritten files get a note instead of a diff.
    void diff(const FileChange &change)
    {
        FileDiff result{change.root, change.file.path, {}, {}, {}, {}};
        GitBaseline *baseline = nullptr;
        for (const auto &candidate : baselines_)
            if (candidate->root() == change.root) baseline = candidate.get();
        if (!baseline) {
            result.note = "This file is no longer tracked.";
        } else if (change.file.status == ChangedFile::Status::New) {
            result.note = QString("New file with %1 lines; open it to see its content.").arg(change.lines.added);
        } else if (change.lines.kind == LineChanges::Kind::Binary || change.file.status == ChangedFile::Status::Binary) {
            result.note = "Binary file; it is compared by content only.";
        } else if (change.lines.kind == LineChanges::Kind::TooLarge) {
            result.note = "The file is too large to show its differences.";
        } else if (change.lines.kind == LineChanges::Kind::Rewritten) {
            result.note = QString("The file was largely rewritten (%1 lines before, %2 now); its differences are not shown.")
                              .arg(change.lines.linesBefore).arg(change.lines.linesAfter);
        } else {
            result.before = splitLines(baseline->contentBefore(change.file));
            result.after = splitLines(baseline->contentAfter(change.file));
            const diffcore::DiffResult computed = diffcore::DiffEngine().compute(result.before, result.after);
            result.hunks = QList<diffcore::Hunk>(computed.hunks.begin(), computed.hunks.end());
        }
        emit diffComputed(result);
    }

signals:
    void untrackedFound(const QStringList &directories);
    void counted(const QList<FileChange> &changes);
    void diffComputed(const FileDiff &diff);

private:
    struct Cached
    {
        ChangedFile::Status status;
        qint64 size;
        QDateTime modified;
        LineChanges lines;
    };
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::vector<std::unique_ptr<GitBaseline>> baselines_;
    QHash<QString, Cached> cache_;
};

ChangeTracker::ChangeTracker(QObject *parent)
    : QObject(parent), thread_(new QThread(this)), cancel_(std::make_shared<std::atomic<bool>>(false))
{
    qRegisterMetaType<QList<FileChange>>();
    qRegisterMetaType<FileDiff>();
    worker_ = new Worker(cancel_);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &Worker::untrackedFound, this, [this](const QStringList &directories) { untracked_ = directories; });
    connect(worker_, &Worker::diffComputed, this, &ChangeTracker::diffReady);
    connect(worker_, &Worker::counted, this, [this](const QList<FileChange> &changes) {
        changes_ = changes;
        emit changesUpdated();
    });
    thread_->start();
}

ChangeTracker::~ChangeTracker()
{
    cancel_->store(true);
    thread_->quit();
    thread_->wait();
}

void ChangeTracker::turnStarted(const QStringList &directories)
{
    startedAt_ = QDateTime::currentDateTime();
    changes_.clear();
    emit changesUpdated();
    QMetaObject::invokeMethod(worker_, [worker = worker_, directories] { worker->capture(directories); });
}

void ChangeTracker::refresh()
{
    QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->count(); });
}

void ChangeTracker::requestDiff(const FileChange &change)
{
    QMetaObject::invokeMethod(worker_, [worker = worker_, change] { worker->diff(change); });
}

QString ChangeTracker::summary() const
{
    if (changes_.isEmpty()) return {};
    int added = 0;
    int removed = 0;
    for (const FileChange &change : changes_) {
        if (change.lines.kind != LineChanges::Kind::Counted) continue;
        added += change.lines.added;
        removed += change.lines.removed;
    }
    return QString("%1 %2, +%3 −%4").arg(changes_.size()).arg(changes_.size() == 1 ? "file" : "files").arg(added).arg(removed);
}

#include "ChangeTracker.moc"
