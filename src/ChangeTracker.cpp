#include "ChangeTracker.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QThread>

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

signals:
    void untrackedFound(const QStringList &directories);
    void counted(const QList<FileChange> &changes);

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
    worker_ = new Worker(cancel_);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &Worker::untrackedFound, this, [this](const QStringList &directories) { untracked_ = directories; });
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
    changes_.clear();
    emit changesUpdated();
    QMetaObject::invokeMethod(worker_, [worker = worker_, directories] { worker->capture(directories); });
}

void ChangeTracker::refresh()
{
    QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->count(); });
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
