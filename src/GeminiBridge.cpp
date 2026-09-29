#include "GeminiBridge.h"

#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

GeminiBridge::GeminiBridge(const QString &program, const QString &workingDirectory, QObject *parent)
    : QObject(parent), program_(program), workingDirectory_(workingDirectory), process_(new QProcess(this))
{
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
        buffer_ += process_->readAllStandardOutput();
        qsizetype newline;
        while ((newline = buffer_.indexOf('\n')) >= 0) {
            const QByteArray line = buffer_.left(newline).trimmed();
            buffer_.remove(0, newline + 1);
            if (!line.isEmpty()) handleLine(line);
        }
    });
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QString details = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!details.isEmpty()) emit error(details);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (interrupted_ && processError == QProcess::Crashed) return;
        errorDetails_ = process_->errorString();
        emit error("Gemini CLI process error: " + errorDetails_ + " (executable: " + program_ + ")");
        if (processError == QProcess::FailedToStart) finish(-1);
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { finish(code); });
}

GeminiBridge::~GeminiBridge()
{
    if (isRunning()) {
        process_->terminate();
        if (!process_->waitForFinished(1000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
}

bool GeminiBridge::isRunning() const { return process_->state() != QProcess::NotRunning; }
QString GeminiBridge::program() const { return program_; }

void GeminiBridge::prompt(const QString &text)
{
    if (isRunning()) return;
    buffer_.clear();
    errorDetails_.clear();
    interrupted_ = false;
    resultSeen_ = false;
    QStringList arguments{"--output-format", "stream-json"};
    if (!sessionId_.isEmpty()) {
        arguments << "--resume" << (sessionId_.startsWith("index:") ? sessionId_.mid(6) : sessionId_);
    }
    for (const QString &directory : directories_) arguments << "--include-directories" << directory;
    arguments << "--prompt" << text;
    process_->setWorkingDirectory(workingDirectory_);
    process_->start(program_, arguments);
}

void GeminiBridge::interrupt()
{
    if (!isRunning()) return;
    interrupted_ = true;
    process_->terminate();
    QTimer::singleShot(1000, process_, [this] {
        if (isRunning()) process_->kill();
    });
}

void GeminiBridge::resetConversation() { sessionId_.clear(); }

void GeminiBridge::resetConversation(const QString &workingDirectory)
{
    workingDirectory_ = workingDirectory;
    sessionId_.clear();
}

void GeminiBridge::resumeConversation(const QString &sessionId, const QString &workingDirectory)
{
    if (isRunning()) return;
    sessionId_ = sessionId;
    workingDirectory_ = workingDirectory;
}

QString GeminiBridge::sessionId() const { return sessionId_; }

bool GeminiBridge::addDirectory(const QString &path)
{
    if (directories_.contains(path)) return true;
    if (directories_.size() >= 5) return false;
    directories_.append(path);
    return true;
}

QStringList GeminiBridge::directories() const { return directories_; }

void GeminiBridge::handleLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        emit error("Invalid Gemini CLI response: " + parseError.errorString());
        return;
    }
    const QJsonObject event = document.object();
    const QString type = event.value("type").toString();
    if (type == "init") {
        const QString id = event.value("session_id").toString();
        if (!id.isEmpty()) {
            sessionId_ = id;
            emit sessionChanged(id);
        }
    } else if (type == "message" && event.value("role").toString() == "assistant") {
        const QString content = event.value("content").toString();
        if (!content.isEmpty()) emit textDelta(content);
    } else if (type == "tool_use") {
        emit toolStarted(event.value("tool_name").toString(), event.value("parameters").toObject());
    } else if (type == "error") {
        const QString message = event.value("message").toString();
        if (event.value("severity").toString() != "warning") errorDetails_ = message;
        emit error(message);
    } else if (type == "result") {
        resultSeen_ = true;
        const QString status = event.value("status").toString();
        if (status != "success") errorDetails_ = event.value("error").toObject().value("message").toString(status);
    }
}

void GeminiBridge::finish(int code)
{
    buffer_ += process_->readAllStandardOutput();
    qsizetype newline;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (!line.isEmpty()) handleLine(line);
    }
    if (!buffer_.trimmed().isEmpty()) handleLine(buffer_.trimmed());
    buffer_.clear();
    emit completed(interrupted_ ? "interrupted" : (code == 0 && resultSeen_ && errorDetails_.isEmpty() ? "completed" : "failed"),
                   errorDetails_.isEmpty() && code != 0 && !interrupted_ ? QString("Gemini CLI exited with code %1").arg(code) : errorDetails_);
}
