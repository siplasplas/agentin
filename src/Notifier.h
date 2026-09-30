#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

class QSystemTrayIcon;
class QTimer;

// Desktop notifications and sounds for long turns and for agents that wait for the user. Sounds are
// played by an external player (ffplay, mpv, pw-play or paplay), so WAV, MP3 and OGG work without Qt
// Multimedia. An installed text-to-speech program can say what happened instead, such as "Codex
// finished: fix the build"; the speech is generated when needed, so no audio files are shipped.
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
        // Announcements are spoken when a voice program is found; otherwise the sound files play.
        bool voice = true;
        // "piper" or "espeak-ng"; empty picks the first one found.
        QString voiceEngine;
        // Empty paths are found automatically.
        QString piperProgram;
        QString piperModel;

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
    // The voice program and model the settings lead to, or empty strings when none is installed.
    static QString findPiper(const Settings &settings);
    static QString findPiperModel(const Settings &settings);
    // All installed Piper voices, for choosing one.
    static QStringList piperModels();
    static QString findEspeak();
    // "piper" or "espeak-ng" when the settings can speak, otherwise an empty string.
    static QString voiceEngine(const Settings &settings);
    // Says text with the settings' voice; returns false when no voice program is available.
    static bool say(const Settings &settings, const QString &text, QObject *parent);

private:
    void announceWaiting(const QString &key);
    enum class Event { Finished, Failed, Waiting };
    // Speaks the event in the voice's language when a voice is set up, otherwise plays the sound file.
    void announce(Event event, const QString &agent, const QString &chat, const QString &soundFile);
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
