#include "GitBaseline.h"

#include "LineChanges.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>

#include <git2.h>

#include <memory>

namespace {
template <typename T, void (*Free)(T *)>
struct Freer
{
    void operator()(T *object) const { Free(object); }
};
template <typename T, void (*Free)(T *)>
using Owned = std::unique_ptr<T, Freer<T, Free>>;

struct Library
{
    Library() { git_libgit2_init(); }
    ~Library() { git_libgit2_shutdown(); }
};

void ensureLibrary()
{
    static Library library;
}

QByteArray idText(const git_oid &id)
{
    // Sized for the longest ID the library supports, SHA-256 when it is built with it.
    char text[GIT_OID_MAX_HEXSIZE + 1] = {};
    git_oid_tostr(text, sizeof text, &id);
    return QByteArray(text);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The same test countLineChanges uses: a zero byte near the start, as Git decides.
bool binaryPrefix(const QByteArray &content)
{
    return content.left(8000).contains('\0');
}

bool binaryFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && binaryPrefix(file.read(8000));
}
}

GitBaseline::GitBaseline(const QString &directory)
{
    ensureLibrary();
    git_repository *repository = nullptr;
    if (git_repository_open_ext(&repository, QFile::encodeName(directory).constData(), 0, nullptr) != 0) {
        fail("Not a Git work tree");
        return;
    }
    if (git_repository_is_bare(repository)) {
        git_repository_free(repository);
        error_ = "Bare Git repository";
        return;
    }
    repository_ = repository;
    root_ = QDir::cleanPath(QFile::decodeName(git_repository_workdir(repository_)));
    const QString relative = QDir(root_).relativeFilePath(QDir(directory).canonicalPath());
    prefix_ = relative == "." || relative.isEmpty() ? QString() : relative + '/';
}

GitBaseline::~GitBaseline()
{
    if (repository_) git_repository_free(repository_);
}

bool GitBaseline::fail(const QString &context)
{
    const git_error *last = git_error_last();
    error_ = context + (last && last->message ? QString(": ") + QString::fromUtf8(last->message) : QString());
    return false;
}

bool GitBaseline::inside(const QString &path) const
{
    return prefix_.isEmpty() || path.startsWith(prefix_);
}

bool GitBaseline::capture()
{
    if (!repository_) return false;
    captured_ = false;
    kept_.clear();
    untracked_.clear();
    startCommit_.clear();
    git_oid head;
    // An unborn HEAD, as in a repository without commits, leaves every file without a start blob.
    if (git_reference_name_to_id(&head, repository_, "HEAD") == 0) startCommit_ = idText(head);

    git_status_options options = GIT_STATUS_OPTIONS_INIT;
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
        | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
    git_status_list *rawList = nullptr;
    if (git_status_list_new(&rawList, repository_, &options) != 0) return fail("Could not read the Git status");
    Owned<git_status_list, git_status_list_free> list(rawList);
    for (size_t i = 0; i < git_status_list_entrycount(list.get()); ++i) {
        const git_status_entry *entry = git_status_byindex(list.get(), i);
        const git_diff_delta *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
        if (!delta) continue;
        const QString path = QString::fromUtf8(delta->new_file.path);
        if (!inside(path)) continue;
        const QString absolute = QDir(root_).filePath(path);
        if (entry->status == GIT_STATUS_WT_NEW) {
            const QFileInfo info(absolute);
            untracked_.insert(path, {info.size(), info.lastModified()});
            continue;
        }
        Kept kept;
        const QFileInfo info(absolute);
        kept.exists = info.exists();
        if (kept.exists) {
            kept.size = info.size();
            if (kept.size <= kMaximumDiffBytes) {
                kept.content = readFile(absolute);
                git_oid id;
                if (git_odb_hash(&id, kept.content.constData(), size_t(kept.content.size()), GIT_OBJECT_BLOB) == 0)
                    kept.objectId = idText(id);
            } else {
                kept.objectId = diskId(path, false);
            }
        }
        kept_.insert(path, kept);
    }
    captured_ = true;
    return true;
}

QByteArray GitBaseline::startBlobId(const QString &path) const
{
    if (startCommit_.isEmpty()) return {};
    git_oid commitId;
    if (git_oid_fromstr(&commitId, startCommit_.constData()) != 0) return {};
    git_commit *rawCommit = nullptr;
    if (git_commit_lookup(&rawCommit, repository_, &commitId) != 0) return {};
    Owned<git_commit, git_commit_free> commit(rawCommit);
    git_tree *rawTree = nullptr;
    if (git_commit_tree(&rawTree, commit.get()) != 0) return {};
    Owned<git_tree, git_tree_free> tree(rawTree);
    git_tree_entry *rawEntry = nullptr;
    if (git_tree_entry_bypath(&rawEntry, tree.get(), path.toUtf8().constData()) != 0) return {};
    Owned<git_tree_entry, git_tree_entry_free> entry(rawEntry);
    if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB) return {};
    return idText(*git_tree_entry_id(entry.get()));
}

QByteArray GitBaseline::startBlob(const QString &path) const
{
    const QByteArray id = startBlobId(path);
    if (id.isEmpty()) return {};
    git_oid blobId;
    if (git_oid_fromstr(&blobId, id.constData()) != 0) return {};
    git_blob *rawBlob = nullptr;
    if (git_blob_lookup(&rawBlob, repository_, &blobId) != 0) return {};
    Owned<git_blob, git_blob_free> blob(rawBlob);
    return QByteArray(static_cast<const char *>(git_blob_rawcontent(blob.get())), qsizetype(git_blob_rawsize(blob.get())));
}

// Filtered hashing applies Git's conversions such as line endings, as blobs in trees have them; raw hashing
// matches content kept as it was on disk.
QByteArray GitBaseline::diskId(const QString &path, bool filtered) const
{
    const QByteArray absolute = QFile::encodeName(QDir(root_).filePath(path));
    git_oid id;
    const int result = filtered ? git_repository_hashfile(&id, repository_, absolute.constData(), GIT_OBJECT_BLOB, nullptr)
                                : git_odb_hashfile(&id, absolute.constData(), GIT_OBJECT_BLOB);
    return result == 0 ? idText(id) : QByteArray();
}

QList<ChangedFile> GitBaseline::changes()
{
    QList<ChangedFile> result;
    if (!repository_ || !captured_) return result;

    // Candidates: what differs from the index or HEAD now, what commits made during the turn changed,
    // and what already differed at the start. Renames staged in the index pair an old and a new path.
    QSet<QString> candidates(kept_.keyBegin(), kept_.keyEnd());
    QHash<QString, QString> renamedFrom;
    QSet<QString> untrackedNow;
    git_status_options options = GIT_STATUS_OPTIONS_INIT;
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
        | GIT_STATUS_OPT_EXCLUDE_SUBMODULES | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX;
    git_status_list *rawList = nullptr;
    if (git_status_list_new(&rawList, repository_, &options) != 0) {
        fail("Could not read the Git status");
        return result;
    }
    Owned<git_status_list, git_status_list_free> list(rawList);
    for (size_t i = 0; i < git_status_list_entrycount(list.get()); ++i) {
        const git_status_entry *entry = git_status_byindex(list.get(), i);
        const git_diff_delta *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
        if (!delta) continue;
        const QString path = QString::fromUtf8(delta->new_file.path);
        if (entry->status == GIT_STATUS_WT_NEW) untrackedNow.insert(path);
        candidates.insert(path);
        if (entry->head_to_index && (entry->status & GIT_STATUS_INDEX_RENAMED)) {
            const QString oldPath = QString::fromUtf8(entry->head_to_index->old_file.path);
            renamedFrom.insert(QString::fromUtf8(entry->head_to_index->new_file.path), oldPath);
        }
    }
    git_oid head;
    if (!startCommit_.isEmpty() && git_reference_name_to_id(&head, repository_, "HEAD") == 0
        && idText(head) != startCommit_) {
        git_oid start;
        git_commit *rawStart = nullptr;
        git_commit *rawNow = nullptr;
        if (git_oid_fromstr(&start, startCommit_.constData()) == 0 && git_commit_lookup(&rawStart, repository_, &start) == 0
            && git_commit_lookup(&rawNow, repository_, &head) == 0) {
            Owned<git_commit, git_commit_free> startCommit(rawStart);
            Owned<git_commit, git_commit_free> nowCommit(rawNow);
            git_tree *rawStartTree = nullptr;
            git_tree *rawNowTree = nullptr;
            if (git_commit_tree(&rawStartTree, startCommit.get()) == 0 && git_commit_tree(&rawNowTree, nowCommit.get()) == 0) {
                Owned<git_tree, git_tree_free> startTree(rawStartTree);
                Owned<git_tree, git_tree_free> nowTree(rawNowTree);
                git_diff *rawDiff = nullptr;
                if (git_diff_tree_to_tree(&rawDiff, repository_, startTree.get(), nowTree.get(), nullptr) == 0) {
                    Owned<git_diff, git_diff_free> diff(rawDiff);
                    for (size_t i = 0; i < git_diff_num_deltas(diff.get()); ++i) {
                        const git_diff_delta *delta = git_diff_get_delta(diff.get(), i);
                        candidates.insert(QString::fromUtf8(delta->old_file.path));
                        candidates.insert(QString::fromUtf8(delta->new_file.path));
                    }
                }
            }
        }
    }

    QMap<QString, ChangedFile> files;
    QSet<QString> renamedAway;
    for (auto it = renamedFrom.cbegin(); it != renamedFrom.cend(); ++it) renamedAway.insert(it.value());
    for (const QString &path : candidates) {
        if (!inside(path)) continue;
        const QString absolute = QDir(root_).filePath(path);
        const QFileInfo info(absolute);
        const bool existsNow = info.isFile();
        ChangedFile file;
        file.path = path;
        file.sizeAfter = existsNow ? info.size() : -1;

        // Untracked files are new; one that existed at the start counts only if the turn changed it.
        if (untrackedNow.contains(path) && !kept_.contains(path)) {
            const auto noted = untracked_.constFind(path);
            if (noted != untracked_.constEnd() && noted->size == info.size() && noted->modified == info.lastModified())
                continue;
            file.status = ChangedFile::Status::New;
            files.insert(path, file);
            continue;
        }

        const QString source = renamedFrom.value(path, path);
        bool existedBefore = false;
        bool unchanged = false;
        const auto kept = kept_.constFind(source);
        if (kept != kept_.constEnd()) {
            existedBefore = kept->exists;
            file.sizeBefore = kept->exists ? kept->size : -1;
            unchanged = existsNow && existedBefore && kept->objectId == diskId(path, false);
        } else {
            const QByteArray blobId = startBlobId(source);
            existedBefore = !blobId.isEmpty();
            if (existedBefore) {
                git_oid id;
                git_odb *rawDatabase = nullptr;
                if (git_oid_fromstr(&id, blobId.constData()) == 0 && git_repository_odb(&rawDatabase, repository_) == 0) {
                    Owned<git_odb, git_odb_free> database(rawDatabase);
                    size_t size = 0;
                    git_object_t type;
                    if (git_odb_read_header(&size, &type, database.get(), &id) == 0) file.sizeBefore = qint64(size);
                }
            }
            unchanged = existsNow && existedBefore && blobId == diskId(path, true);
        }
        if (unchanged && source == path) continue;
        if (!existsNow && !existedBefore) continue;
        // A path renamed away appears as the new path's old one rather than as deleted.
        if (!existsNow && renamedAway.contains(path)) continue;
        if (source != path) {
            file.status = ChangedFile::Status::Renamed;
            file.oldPath = source;
        } else if (!existedBefore) {
            file.status = ChangedFile::Status::New;
        } else if (!existsNow) {
            file.status = ChangedFile::Status::Deleted;
        } else {
            file.status = binaryFile(absolute) ? ChangedFile::Status::Binary : ChangedFile::Status::Modified;
        }
        files.insert(path, file);
    }
    for (const ChangedFile &file : files) result.append(file);
    return result;
}

QByteArray GitBaseline::contentBefore(const ChangedFile &file)
{
    const QString source = file.oldPath.isEmpty() ? file.path : file.oldPath;
    const auto kept = kept_.constFind(source);
    if (kept != kept_.constEnd()) return kept->content;
    return startBlob(source);
}

QByteArray GitBaseline::contentAfter(const ChangedFile &file) const
{
    return readFile(QDir(root_).filePath(file.path));
}
