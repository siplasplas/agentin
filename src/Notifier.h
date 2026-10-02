#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <QString>
#include <QStringList>

class QProcess;
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
        // Short cues when compaction starts and ends: a built-in sound (see builtInSounds()), a file, or
        // empty for none. They play whatever the turn's length and are never spoken.
        QString compactionStartedSound = "rising";
        QString compactionFinishedSound = "falling";
        // A waiting agent is announced after this delay, so an answer given at once stays quiet.
        int waitingDelaySeconds = 30;
        // Repeats the waiting announcement until answered; 0 announces once.
        int waitingRepeatMinutes = 0;
        // Approval requests can stay quiet while questions and finished turns are still announced.
        bool announceApprovals = true;
        // Finished turns, and separately waiting agents, are spoken when a voice program is found;
        // otherwise their sounds play.
        bool voice = true;
        bool voiceWaiting = true;
        // "piper" or "espeak-ng"; empty picks the first one found.
        QString voiceEngine;
        // Empty paths are found automatically.
        QString piperProgram;
        QString piperModel;
        // Piper's length scale: above 1 speaks slower, which is usually clearer.
        double speechSlowness = 1.3;

        static Settings fromJson(const QJsonObject &object);
        QJsonObject toJson() const;
    };

    explicit Notifier(QObject *parent = nullptr);
    ~Notifier() override;

    Settings settings() const { return settings_; }
    void setSettings(const Settings &settings) { settings_ = settings; }
    void setMuted(bool muted) { settings_.muted = muted; }

    void turnFinished(const QString &agent, const QString &chat, bool succeeded, qint64 durationMs);
    // key identifies the waiting chat and requestId its request; calling it again for the same request
    // changes nothing, while another request of the chat replaces the answered one.
    void waitingStarted(const QString &key, int requestId, const QString &agent, const QString &chat,
                        const QString &request);
    // Also stops the announcement of that chat's request if it is still being played.
    void waitingEnded(const QString &key);
    // A cue does not cut off an announcement that is playing.
    void compactionChanged(bool started);

    // Short sounds generated on demand, as name and label, such as "click" and "Click".
    static QList<std::pair<QString, QString>> builtInSounds();

    // Plays a sound file or a built-in sound even when sounds are muted, for trying it out in the settings.
    bool playSound(const QString &file);
    bool isPlaying() const { return !playbackProcess_.isNull(); }
    void stopPlayback();
    // The voice program and model the settings lead to, or empty strings when none is installed.
    static QString findPiper(const Settings &settings);
    static QString findPiperModel(const Settings &settings);
    // All installed Piper voices, for choosing one.
    static QStringList piperModels();
    // A readable name for a Piper voice, such as "Polish — gosia (Piper, medium)" for pl_PL-gosia-medium.
    static QString voiceLabel(const QString &modelPath);
    static QString findEspeak();
    // "piper" or "espeak-ng" when the settings can speak, otherwise an empty string.
    static QString voiceEngine(const Settings &settings);
    // Says text with the settings' voice; returns false when no voice program is available.
    bool say(const Settings &settings, const QString &text);

signals:
    void playbackChanged(bool playing);
    void playbackFailed(const QString &reason);

private:
    QProcess *startPlaybackProcess(const QString &program, const QStringList &arguments,
                                   std::function<void(bool)> completed = {});
    bool playSoundFile(const QString &file, bool temporary);
    // A file, or a built-in sound written to a temporary file.
    bool playSoundOrBuiltIn(const QString &sound);
    void updatePlaybackState();
    void removeTemporaryFile(const QString &file);
    void announceWaiting(const QString &key);
    enum class Event { Finished, Failed, Waiting };
    // Speaks the event in the voice's language when a voice is set up, otherwise plays the sound file.
    void announce(Event event, const QString &agent, const QString &chat, const QString &soundFile);
    void popup(const QString &title, const QString &text);

    QPointer<QProcess> playbackProcess_;
    bool playbackActive_ = false;
    QStringList temporaryFiles_;
    Settings settings_;
    QSystemTrayIcon *tray_ = nullptr;
    struct Waiting
    {
        QString agent;
        QString chat;
        QString request;
        int requestId = -1;
        QTimer *timer = nullptr;
    };
    QHash<QString, Waiting> waiting_;
    // The waiting chat whose announcement the current playback is.
    QString announcedWaitingKey_;
};
