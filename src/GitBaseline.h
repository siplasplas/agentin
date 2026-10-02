#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

struct git_repository;

// A file a turn changed, relative to the Git work tree root.
struct ChangedFile
{
    enum class Status { Modified, New, Deleted, Renamed, Binary };
    Status status = Status::Modified;
    QString path;
    // The path before a rename.
    QString oldPath;
    qint64 sizeBefore = -1;
    qint64 sizeAfter = -1;
};

// The state of the Git work tree containing a directory when a turn started, and the files changed since.
// Clean tracked files are compared with the start commit; tracked files that already differed from Git
// keep their start content in memory (above kMaximumDiffBytes only their object ID); untracked files are
// noted by size and modification time and are shown as new without a diff; ignored files are skipped.
// Only files inside the directory are reported. Not thread-safe; libgit2 objects stay in one thread.
class GitBaseline
{
public:
    explicit GitBaseline(const QString &directory);
    ~GitBaseline();
    GitBaseline(const GitBaseline &) = delete;
    GitBaseline &operator=(const GitBaseline &) = delete;

    // False outside a Git work tree, in a bare repository, or when Git cannot be read.
    bool isValid() const { return repository_ != nullptr; }
    QString error() const { return error_; }
    QString root() const { return root_; }

    // Records the start of a turn.
    bool capture();
    // Compares with the current HEAD instead, as Git shows changes, including those made before the chat.
    void followHead();
    bool isCaptured() const { return captured_; }
    // Files changed since capture(), sorted by path.
    QList<ChangedFile> changes();
    // Content at capture time; empty for a new file.
    QByteArray contentBefore(const ChangedFile &file);
    QByteArray contentAfter(const ChangedFile &file) const;

private:
    struct Kept
    {
        bool exists = false;
        // Empty when the file was above kMaximumDiffBytes; its object ID is then the only record.
        QByteArray content;
        QByteArray objectId;
        qint64 size = -1;
    };
    struct Noted
    {
        qint64 size = -1;
        QDateTime modified;
    };

    bool fail(const QString &context);
    bool inside(const QString &path) const;
    QByteArray startBlobId(const QString &path) const;
    QByteArray startBlob(const QString &path) const;
    QByteArray diskId(const QString &path, bool filtered) const;

    git_repository *repository_ = nullptr;
    QString root_;
    // The directory relative to root_, ending in '/', or empty for the whole work tree.
    QString prefix_;
    QString error_;
    bool captured_ = false;
    bool followsHead_ = false;
    QByteArray startCommit_;
    QHash<QString, Kept> kept_;
    QHash<QString, Noted> untracked_;
};
