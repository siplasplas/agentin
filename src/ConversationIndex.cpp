#include "ConversationIndex.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

ConversationIndex::ConversationIndex(const QString &provider, const QString &filePath)
    : provider_(provider), filePath_(filePath)
{
}

QString ConversationIndex::load()
{
    QFile file(filePath_);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) return "Could not read conversation index: " + file.errorString();
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || document.object().value("version").toInt() != 1
        || !document.object().value("threads").isArray()) {
        return "Invalid conversation index: " + filePath_;
    }
    for (const QJsonValue &value : document.object().value("threads").toArray()) {
        const QJsonObject thread = value.toObject();
        const QString id = thread.value("id").toString();
        if (thread.value("provider").toString() == provider_ && !id.isEmpty()) entries_.insert(id, thread);
    }
    return {};
}

QString ConversationIndex::save() const
{
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return "Could not save " + provider_ + " conversation index: cannot create its directory";
    QJsonArray threads;
    for (const QJsonObject &thread : entries_) threads.append(thread);
    QSaveFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(QJsonObject{{"version", 1}, {"threads", threads}}).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        return "Could not save " + provider_ + " conversation index: " + file.errorString();
    }
    return {};
}

bool ConversationIndex::insert(QJsonObject entry)
{
    entry.insert("provider", provider_);
    const QString id = entry.value("id").toString();
    if (id.isEmpty() || entries_.value(id) == entry) return false;
    entries_.insert(id, entry);
    return true;
}

bool ConversationIndex::remember(const QString &id, const QString &workingDirectory, const QString &firstPrompt)
{
    if (id.isEmpty() || entries_.contains(id)) return false;
    return insert({{"id", id}, {"cwd", workingDirectory},
                   {"title", firstPrompt.isEmpty() ? id : firstPrompt.left(120)},
                   {"createdAt", QDateTime::currentSecsSinceEpoch()}});
}

bool ConversationIndex::touch(const QString &id)
{
    if (!entries_.contains(id)) return false;
    entries_[id].insert("lastModified", QDateTime::currentSecsSinceEpoch());
    return true;
}

QList<QJsonObject> ConversationIndex::treeEntries() const
{
    QList<QJsonObject> result;
    for (const QJsonObject &thread : entries_) {
        const QString id = thread.value("id").toString();
        const QString title = thread.value("title").toString(id);
        result.append({{"id", id}, {"cwd", thread.value("cwd").toString()}, {"title", title},
                       {"tooltip", tooltip(thread)},
                       {"createdAt", thread.value("createdAt").toInteger()},
                       {"modifiedAt", thread.value("lastModified").toInteger()}});
    }
    return result;
}

QString ConversationIndex::tooltip(const QJsonObject &entry)
{
    const QString id = entry.value("id").toString();
    QStringList details{entry.value("title").toString(id), "ID: " + id, "Directory: " + entry.value("cwd").toString()};
    const auto addText = [&entry, &details](const QString &key, const QString &label) {
        const QString value = entry.value(key).toString();
        if (!value.isEmpty()) details.append(label + ": " + value);
    };
    const auto addDate = [&entry, &details](const QString &key, const QString &label) {
        const qint64 seconds = entry.value(key).toInteger();
        if (seconds > 0) details.append(label + ": " + QDateTime::fromSecsSinceEpoch(seconds).toString(Qt::ISODate));
    };
    addDate("createdAt", "Created");
    addDate("lastModified", "Modified");
    if (entry.value("fileSize").isDouble())
        details.append("Transcript size: " + QString::number(entry.value("fileSize").toInteger()) + " bytes");
    addText("gitBranch", "Git branch");
    addText("tag", "Tag");
    addText("firstPrompt", "First prompt");
    return details.join('\n');
}
