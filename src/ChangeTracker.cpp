#include "ChangeTracker.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <diffcore/DiffEngine.h>

#include <map>
#include <vector>

class ChangeTracker::Worker : public QObject
{
    Q_OBJECT

public:
    explicit Worker(std::shared_ptr<std::atomic<bool>> cancel) : cancel_(std::move(cancel)) {}

    // The chat and HEAD baselines are made at the first turn, in its directories, and kept.
    void capture(const QStringList &directories)
    {
        QStringList untracked;
        const QStringList taken = distinct(directories);
        baselines_[ChangesSince::Turn] = open(taken, untracked);
        for (auto &baseline : baselines_[ChangesSince::Turn]) baseline->capture();
        if (baselines_[ChangesSince::Chat].empty()) {
            QStringList ignored;
            baselines_[ChangesSince::Chat] = open(taken, ignored);
            for (auto &baseline : baselines_[ChangesSince::Chat]) baseline->capture();
            baselines_[ChangesSince::Head] = open(taken, ignored);
            for (auto &baseline : baselines_[ChangesSince::Head]) baseline->followHead();
        }
        cache_.clear();
        emit untrackedFound(untracked);
    }

    void count(const QList<ChangesSince> &sinces)
    {
        QHash<ChangesSince, QList<FileChange>> result;
        for (ChangesSince since : sinces) {
            QList<FileChange> &changes = result[since];
            for (const auto &baseline : baselines_[since]) {
                for (const ChangedFile &file : baseline->changes()) {
                    if (cancel_->load()) return;
                    FileChange change{since, baseline->root(), file, {}};
                    const QFileInfo info(QDir(baseline->root()).filePath(file.path));
                    const QString key = QString::number(int(since)) + baseline->root() + '/' + file.path;
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
                    changes.append(change);
                }
            }
        }
        emit counted(result);
    }

    // New, binary, too large and rewritten files get a note instead of a diff.
    void diff(const FileChange &change)
    {
        FileDiff result{change.root, change.file.path, {}, {}, {}, {}};
        GitBaseline *baseline = nullptr;
        for (const auto &candidate : baselines_[change.since])
            if (candidate->root() == change.root) baseline = candidate.get();
        if (!baseline) {
            result.note = "This file is no longer tracked.";
        } else if (change.file.status == ChangedFile::Status::New) {
            result.note = QString("New file with %1 %2; open it to see its content.")
                              .arg(change.lines.added).arg(change.lines.added == 1 ? "line" : "lines");
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
    void counted(const QHash<ChangesSince, QList<FileChange>> &changes);
    void diffComputed(const FileDiff &diff);

private:
    using Baselines = std::vector<std::unique_ptr<GitBaseline>>;
    struct Cached
    {
        ChangedFile::Status status;
        qint64 size;
        QDateTime modified;
        LineChanges lines;
    };

    // A directory inside one already taken would report the same files twice.
    static QStringList distinct(const QStringList &directories)
    {
        QStringList taken;
        for (const QString &directory : directories) {
            const QString clean = QDir::cleanPath(directory);
            bool nested = clean.isEmpty();
            for (const QString &other : taken)
                if (clean == other || clean.startsWith(other + '/')) nested = true;
            if (!nested) taken.append(clean);
        }
        return taken;
    }

    static Baselines open(const QStringList &directories, QStringList &untracked)
    {
        Baselines baselines;
        for (const QString &directory : directories) {
            auto baseline = std::make_unique<GitBaseline>(directory);
            if (baseline->isValid()) baselines.push_back(std::move(baseline));
            else untracked.append(directory);
        }
        return baselines;
    }

    std::shared_ptr<std::atomic<bool>> cancel_;
    std::map<ChangesSince, Baselines> baselines_;
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
    connect(worker_, &Worker::counted, this, [this](const QHash<ChangesSince, QList<FileChange>> &changes) {
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) changes_.insert(it.key(), it.value());
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
    turnStartedAt_ = QDateTime::currentDateTime();
    if (!chatStartedAt_.isValid()) chatStartedAt_ = turnStartedAt_;
    changes_.remove(ChangesSince::Turn);
    emit changesUpdated();
    QMetaObject::invokeMethod(worker_, [worker = worker_, directories] { worker->capture(directories); });
    refresh();
}

void ChangeTracker::refresh()
{
    QList<ChangesSince> sinces{ChangesSince::Turn};
    if (extraSince_ != ChangesSince::Turn) sinces.append(extraSince_);
    QMetaObject::invokeMethod(worker_, [worker = worker_, sinces] { worker->count(sinces); });
}

void ChangeTracker::setExtraSince(ChangesSince since)
{
    if (extraSince_ == since) return;
    extraSince_ = since;
    refresh();
}

void ChangeTracker::requestDiff(const FileChange &change)
{
    QMetaObject::invokeMethod(worker_, [worker = worker_, change] { worker->diff(change); });
}

QDateTime ChangeTracker::startedAt(ChangesSince since) const
{
    if (since == ChangesSince::Turn) return turnStartedAt_;
    if (since == ChangesSince::Chat) return chatStartedAt_;
    return {};
}

QString ChangeTracker::summary(ChangesSince since) const
{
    const QList<FileChange> changes = changes_.value(since);
    if (changes.isEmpty()) return {};
    int added = 0;
    int removed = 0;
    for (const FileChange &change : changes) {
        if (change.lines.kind != LineChanges::Kind::Counted) continue;
        added += change.lines.added;
        removed += change.lines.removed;
    }
    return QString("%1 %2, +%3 −%4").arg(changes.size()).arg(changes.size() == 1 ? "file" : "files").arg(added).arg(removed);
}

#include "ChangeTracker.moc"
