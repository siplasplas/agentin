#pragma once

#include <QObject>
#include <QSet>
#include <QString>

// Keeps two agents from working on turns in the same directory, or in one directory and its
// subdirectory, at the same time. A chat holds its directory only from sending a message until the
// agent hands control back. The running turns of all application instances are kept in a registry
// file; busy Claude Code sessions outside the application count as well.
class TurnLocks : public QObject
{
    Q_OBJECT

public:
    explicit TurnLocks(const QString &dataDirectory, QObject *parent = nullptr);
    ~TurnLocks() override;

    // Returns an empty string when owner now holds directory, otherwise a description of who holds a
    // conflicting directory.
    QString acquire(const QString &owner, const QString &directory, const QString &label);
    void release(const QString &owner);

signals:
    // A directory held in this window became free, so waiting chats can try again.
    void released();

private:
    QString registryPath_;
    QString guardPath_;
    QSet<QString> held_;
};
