#include "Notifier.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>

Notifier::Settings Notifier::Settings::fromJson(const QJsonObject &object)
{
    Settings settings;
    settings.popups = object.value("popups").toBool(true);
    settings.muted = object.value("muted").toBool(false);
    settings.minimumMinutes = object.value("minimumMinutes").toInt(5);
    settings.finishedSound = object.value("finishedSound").toString();
    settings.failedSound = object.value("failedSound").toString();
    settings.waitingSound = object.value("waitingSound").toString();
    settings.waitingDelaySeconds = object.value("waitingDelaySeconds").toInt(30);
    settings.waitingRepeatMinutes = object.value("waitingRepeatMinutes").toInt(0);
    settings.voice = object.value("voice").toBool(true);
    settings.voiceEngine = object.value("voiceEngine").toString();
    settings.piperProgram = object.value("piperProgram").toString();
    settings.piperModel = object.value("piperModel").toString();
    settings.speechSlowness = object.value("speechSlowness").toDouble(1.3);
    return settings;
}

QJsonObject Notifier::Settings::toJson() const
{
    return {{"popups", popups}, {"muted", muted}, {"minimumMinutes", minimumMinutes},
            {"finishedSound", finishedSound}, {"failedSound", failedSound}, {"waitingSound", waitingSound},
            {"waitingDelaySeconds", waitingDelaySeconds}, {"waitingRepeatMinutes", waitingRepeatMinutes},
            {"voice", voice}, {"voiceEngine", voiceEngine}, {"piperProgram", piperProgram}, {"piperModel", piperModel},
            {"speechSlowness", speechSlowness}};
}

Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
}

void Notifier::turnFinished(const QString &agent, const QString &chat, bool succeeded, qint64 durationMs)
{
    if (durationMs < qint64(settings_.minimumMinutes) * 60 * 1000) return;
    const qint64 minutes = durationMs / 60000;
    const QString length = minutes > 0 ? QString("%1 min").arg(minutes) : QString("%1 s").arg(durationMs / 1000);
    if (settings_.popups) {
        popup(agent + (succeeded ? " finished" : " stopped with an error"), "\"" + chat + "\" after " + length);
    }
    if (!settings_.muted)
        announce(succeeded ? Event::Finished : Event::Failed, agent, chat,
                 succeeded ? settings_.finishedSound : settings_.failedSound);
}

void Notifier::waitingStarted(const QString &key, const QString &agent, const QString &chat, const QString &request)
{
    if (waiting_.contains(key)) return;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, key] { announceWaiting(key); });
    waiting_.insert(key, {agent, chat, request, timer});
    timer->start(qMax(0, settings_.waitingDelaySeconds) * 1000);
}

void Notifier::waitingEnded(const QString &key)
{
    const Waiting waiting = waiting_.take(key);
    if (waiting.timer) waiting.timer->deleteLater();
}

// Work stops while an agent waits, so this sound has no minimum turn length.
void Notifier::announceWaiting(const QString &key)
{
    const auto found = waiting_.constFind(key);
    if (found == waiting_.constEnd()) return;
    if (settings_.popups) popup(found->agent + " is waiting for you", "\"" + found->chat + "\": " + found->request);
    if (!settings_.muted) announce(Event::Waiting, found->agent, found->chat, settings_.waitingSound);
    if (settings_.waitingRepeatMinutes > 0) found->timer->start(settings_.waitingRepeatMinutes * 60 * 1000);
}

// The sentence follows the language of the Piper voice (its file name starts with it, as in
// pl_PL-gosia-medium); espeak-ng and other voices speak English.
void Notifier::announce(Event event, const QString &agent, const QString &chat, const QString &soundFile)
{
    if (settings_.voice) {
        const bool polish = voiceEngine(settings_) == "piper" && QFileInfo(findPiperModel(settings_)).fileName().startsWith("pl");
        QString sentence;
        switch (event) {
        case Event::Finished: sentence = polish ? "%1 skończył: %2" : "%1 finished: %2"; break;
        case Event::Failed: sentence = polish ? "%1 zakończył się błędem: %2" : "%1 stopped with an error: %2"; break;
        case Event::Waiting: sentence = polish ? "%1 czeka na ciebie: %2" : "%1 is waiting for you: %2"; break;
        }
        if (say(settings_, sentence.arg(agent, chat), this)) return;
    }
    playSound(soundFile);
}

QString Notifier::findPiper(const Settings &settings)
{
    if (!settings.piperProgram.isEmpty()) return QFileInfo(settings.piperProgram).isExecutable() ? settings.piperProgram : QString();
    const QString inPath = QStandardPaths::findExecutable("piper");
    if (!inPath.isEmpty()) return inPath;
    // pip installs Piper into a virtual environment, which is often not on PATH.
    return QStandardPaths::findExecutable("piper", {QDir::home().filePath(".venvs/piper/bin"), QDir::home().filePath(".local/bin"),
                                                     QDir::home().filePath("piper"), QDir::home().filePath(".local/share/piper/bin")});
}

QString Notifier::findPiperModel(const Settings &settings)
{
    if (!settings.piperModel.isEmpty()) return QFileInfo(settings.piperModel).isFile() ? settings.piperModel : QString();
    return piperModels().value(0);
}

// A Piper voice is a model.onnx file with its model.onnx.json next to it.
QStringList Notifier::piperModels()
{
    QStringList models;
    QStringList places{QDir::home().filePath("piper"), QDir::home().filePath(".local/share/piper"),
                       QDir::home().filePath(".local/share/piper-voices"), QDir::home().filePath(".local/share/piper/voices"),
                       "/usr/share/piper-voices", "/usr/local/share/piper-voices"};
    for (const QString &data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        places << QDir(data).filePath("piper") << QDir(data).filePath("piper-voices");
    for (const QString &place : places) {
        for (const QFileInfo &model : QDir(place).entryInfoList({"*.onnx"}, QDir::Files, QDir::Name)) {
            if (QFileInfo::exists(model.filePath() + ".json") && !models.contains(model.filePath())) models.append(model.filePath());
        }
    }
    return models;
}

QString Notifier::findEspeak()
{
    const QString espeakNg = QStandardPaths::findExecutable("espeak-ng");
    return espeakNg.isEmpty() ? QStandardPaths::findExecutable("espeak") : espeakNg;
}

QString Notifier::voiceEngine(const Settings &settings)
{
    const bool piper = !findPiper(settings).isEmpty() && !findPiperModel(settings).isEmpty();
    const bool espeak = !findEspeak().isEmpty();
    if (settings.voiceEngine == "piper") return piper ? "piper" : QString();
    if (settings.voiceEngine == "espeak-ng") return espeak ? "espeak-ng" : QString();
    return piper ? "piper" : (espeak ? "espeak-ng" : QString());
}

// Piper writes the speech to a temporary WAV file, which a sound player then plays.
bool Notifier::say(const Settings &settings, const QString &text, QObject *parent)
{
    const QString engine = voiceEngine(settings);
    // espeak-ng speaks 175 words per minute by default; the slowness lowers that the same way.
    if (engine == "espeak-ng")
        return QProcess::startDetached(findEspeak(), {"-s", QString::number(qRound(175 / qMax(0.5, settings.speechSlowness))), text});
    if (engine != "piper") return false;
    static int counter = 0;
    const QString output = QDir(QDir::tempPath()).filePath(QString("agentdeskt-voice-%1-%2.wav")
                                                                 .arg(QCoreApplication::applicationPid()).arg(++counter));
    auto *piper = new QProcess(parent);
    QObject::connect(piper, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), piper,
                     [piper, output](int code, QProcess::ExitStatus status) {
        piper->deleteLater();
        if (status == QProcess::NormalExit && code == 0) playSound(output);
        // The player opens the file right away; it is removed a little later.
        QTimer::singleShot(60000, [output] { QFile::remove(output); });
    });
    QObject::connect(piper, &QProcess::errorOccurred, piper, [piper](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) piper->deleteLater();
    });
    piper->start(findPiper(settings), {"--model", findPiperModel(settings), "--length_scale",
                                       QString::number(settings.speechSlowness), "--sentence_silence", "0.3",
                                       "--output_file", output});
    piper->write(text.toUtf8() + '\n');
    piper->closeWriteChannel();
    return true;
}

bool Notifier::playSound(const QString &file)
{
    if (file.isEmpty() || !QFileInfo(file).isFile()) return false;
    const QList<QStringList> players{{"ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"},
                                     {"mpv", "--no-video", "--really-quiet"},
                                     {"pw-play"},
                                     {"paplay"}};
    for (const QStringList &player : players) {
        const QString program = QStandardPaths::findExecutable(player.first());
        if (program.isEmpty()) continue;
        return QProcess::startDetached(program, player.mid(1) << file);
    }
    return false;
}

// Uses the system tray when there is one, otherwise the desktop's notify-send.
void Notifier::popup(const QString &title, const QString &text)
{
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        if (!tray_) {
            tray_ = new QSystemTrayIcon(qApp->windowIcon().isNull()
                                            ? qApp->style()->standardIcon(QStyle::SP_MessageBoxInformation)
                                            : qApp->windowIcon(),
                                        this);
            tray_->setToolTip(QApplication::applicationName());
            tray_->show();
        }
        tray_->showMessage(title, text);
        return;
    }
    const QString notifySend = QStandardPaths::findExecutable("notify-send");
    if (!notifySend.isEmpty()) QProcess::startDetached(notifySend, {"--app-name", QApplication::applicationName(), title, text});
}
