#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

// Local JSON index of one provider's conversations: {"version": 1, "threads": [...]}.
class ConversationIndex
{
public:
    ConversationIndex(const QString &provider, const QString &filePath);

    // Merges the saved entries into memory; returns an error message or an empty string.
    QString load();
    QString save() const;
    bool contains(const QString &id) const { return entries_.contains(id); }
    QJsonObject value(const QString &id) const { return entries_.value(id); }
    // Inserts or replaces an entry and returns whether it changed.
    bool insert(QJsonObject entry);
    // Adds a conversation started in this application unless it is already known.
    bool remember(const QString &id, const QString &workingDirectory, const QString &firstPrompt);
    // Sets a known conversation's modification time to now and returns whether it is known.
    bool touch(const QString &id);
    // Entries in the form AgentBackend::conversations() returns.
    QList<QJsonObject> treeEntries() const;

private:
    QString provider_;
    QString filePath_;
    QHash<QString, QJsonObject> entries_;
};
