#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QTimer;

// Desktop notifications and sounds for long turns and for agents that wait for the user. Sounds are
// played by an external player (ffplay, mpv, pw-play or paplay), so WAV, MP3 and OGG work without Qt
// Multimedia.
class Notifier : public QObject
{
    Q_OBJECT

public:
    struct Settings
    {
        bool popups = true;
        bool muted = false;
        // Finished and failed turns notify only when they took at least this long.
        int minimumMinutes = 5;
        QString finishedSound;
        QString failedSound;
        QString waitingSound;
        // A waiting agent is announced after this delay, so an answer given at once stays quiet.
        int waitingDelaySeconds = 30;
        // Repeats the waiting announcement until answered; 0 announces once.
        int waitingRepeatMinutes = 0;

        static Settings fromJson(const QJsonObject &object);
        QJsonObject toJson() const;
    };

    explicit Notifier(QObject *parent = nullptr);

    Settings settings() const { return settings_; }
    void setSettings(const Settings &settings) { settings_ = settings; }
    void setMuted(bool muted) { settings_.muted = muted; }

    void turnFinished(const QString &agent, const QString &chat, bool succeeded, qint64 durationMs);
    // key identifies the waiting chat; calling it again while it waits changes nothing.
    void waitingStarted(const QString &key, const QString &agent, const QString &chat, const QString &request);
    void waitingEnded(const QString &key);

    // Plays a sound file even when sounds are muted, for trying it out in the settings.
    static bool playSound(const QString &file);

private:
    void announceWaiting(const QString &key);
    void popup(const QString &title, const QString &text);

    Settings settings_;
    QSystemTrayIcon *tray_ = nullptr;
    struct Waiting
    {
        QString agent;
        QString chat;
        QString request;
        QTimer *timer = nullptr;
    };
    QHash<QString, Waiting> waiting_;
};
