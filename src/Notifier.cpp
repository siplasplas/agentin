#include "Notifier.h"

#include <QApplication>
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
    return settings;
}

QJsonObject Notifier::Settings::toJson() const
{
    return {{"popups", popups}, {"muted", muted}, {"minimumMinutes", minimumMinutes},
            {"finishedSound", finishedSound}, {"failedSound", failedSound}, {"waitingSound", waitingSound},
            {"waitingDelaySeconds", waitingDelaySeconds}, {"waitingRepeatMinutes", waitingRepeatMinutes}};
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
    if (!settings_.muted) playSound(succeeded ? settings_.finishedSound : settings_.failedSound);
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
    if (!settings_.muted) playSound(settings_.waitingSound);
    if (settings_.waitingRepeatMinutes > 0) found->timer->start(settings_.waitingRepeatMinutes * 60 * 1000);
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
