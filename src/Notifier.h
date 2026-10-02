#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <optional>
#include <QString>
#include <QStringList>

class QProcess;
class QSystemTrayIcon;
class QTimer;

// Desktop notifications and sounds for long turns and for agents that wait for the user. Sounds are
// played by an external player (ffplay, mpv, pw-play or paplay), so WAV, MP3 and OGG work without Qt
// Multimedia. Instead of a sound, an event can be spoken by an installed Piper voice, or by espeak-ng
// where no Piper voice speaks the language, such as "Codex finished: fix the build"; the speech is generated when needed,
// so no audio files are shipped.
class Notifier : public QObject
{
    Q_OBJECT

public:
    enum class Event { Finished, Failed, Waiting, CompactionStarted, CompactionFinished };

    struct Settings
    {
        bool popups = true;
        bool muted = false;
        // Finished and failed turns notify only when they took at least this long.
        int minimumMinutes = 5;
        QString finishedSound;
        QString failedSound;
        QString waitingSound;
        // Short cues when compaction starts and ends. They play whatever the turn's length.
        QString compactionStartedSound = "rising";
        QString compactionFinishedSound = "falling";
        // A waiting agent is announced after this delay, so an answer given at once stays quiet.
        int waitingDelaySeconds = 30;
        // Repeats the waiting announcement until answered; 0 announces once.
        int waitingRepeatMinutes = 0;
        // Approval requests can stay quiet while questions and finished turns are still announced.
        bool announceApprovals = true;
        // Each sound above is a built-in sound (see builtInSounds()), a file, speech by a voice of
        // voiceLanguage ("speech:" and the voice's name, such as "speech:gosia"), or empty for none.
        // A language is a code such as "pl".
        QString voiceLanguage;
        // Empty finds Piper automatically.
        QString piperProgram;
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
    void compactionChanged(const QString &agent, const QString &chat, bool started);

    // Short sounds generated on demand, as name and label, such as "click" and "Click".
    static QList<std::pair<QString, QString>> builtInSounds();

    // Plays a sound file or a built-in sound even when sounds are muted, for trying it out in the settings.
    bool playSound(const QString &file);
    // Plays a sound or speaks the event as the settings would, even when muted, for trying it out.
    bool preview(const Settings &settings, Event event, const QString &agent, const QString &chat, const QString &sound);
    bool isPlaying() const { return !playbackProcess_.isNull(); }
    void stopPlayback();
    // A voice of one language: a Piper model, or espeak-ng with the language as its voice.
    struct Voice
    {
        QString engine;
        QString model;
        // The Piper speaker, such as "gosia", or "espeak-ng".
        QString name;
    };
    static QString findPiper(const Settings &settings);
    // All installed Piper voices.
    static QStringList piperModels();
    static QString findEspeak();
    // Languages that some installed voice speaks, as codes such as "pl", and their own names.
    static QStringList voiceLanguages(const Settings &settings);
    static QString languageName(const QString &language);
    // The language's Piper voices, or espeak-ng alone where it has none.
    static QList<Voice> voices(const Settings &settings, const QString &language);
    // The named voice of the language, or another one of it when that is not installed.
    static std::optional<Voice> voice(const Settings &settings, const QString &language, const QString &name);
    // What is said for the event; without a chat only its beginning, such as "Codex finished".
    static QString spokenText(Event event, const QString &language, const QString &agent, const QString &chat);
    // Says text with the voice; returns false when its program is not installed.
    bool say(const Settings &settings, const Voice &voice, const QString &text);

signals:
    void playbackChanged(bool playing);
    void playbackFailed(const QString &reason);

private:
    QProcess *startPlaybackProcess(const QString &program, const QStringList &arguments,
                                   std::function<void(bool)> completed = {});
    bool playSoundFile(const QString &file, bool temporary);
    static bool addLeadingSilence(const QString &file, int milliseconds);
    // A file, or a built-in sound written to a temporary file.
    bool playSoundOrBuiltIn(const QString &sound);
    void updatePlaybackState();
    void removeTemporaryFile(const QString &file);
    void announceWaiting(const QString &key);
    // Plays the event's sound or speaks it, as its setting says.
    bool announce(const Settings &settings, Event event, const QString &agent, const QString &chat,
                  const QString &sound);
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
