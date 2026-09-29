#include "ClaudeBridge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>

ClaudeBridge::ClaudeBridge(const QString &pythonProgram, const QString &scriptPath,
                           const QString &workingDirectory, QObject *parent)
    : QObject(parent), pythonProgram_(pythonProgram), scriptPath_(scriptPath),
      workingDirectory_(workingDirectory), process_(new QProcess(this))
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
        emit error("Claude bridge process error: " + process_->errorString()
                   + " (Python: " + pythonProgram_ + ")");
        if (processError == QProcess::FailedToStart) emit disconnected();
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        emit error(QString("Claude bridge exited with code %1").arg(code));
        emit disconnected();
    });
}

ClaudeBridge::~ClaudeBridge()
{
    if (process_->state() != QProcess::NotRunning) {
        send({{"type", "shutdown"}});
        process_->closeWriteChannel();
        if (!process_->waitForFinished(1000)) {
            process_->terminate();
            if (!process_->waitForFinished(1000)) {
                process_->kill();
                process_->waitForFinished(1000);
            }
        }
    }
}

bool ClaudeBridge::isRunning() const
{
    return process_->state() != QProcess::NotRunning;
}

void ClaudeBridge::start()
{
    if (process_->state() != QProcess::NotRunning) return;
    process_->setWorkingDirectory(workingDirectory_);
    process_->start(pythonProgram_, {"-u", scriptPath_, "--cwd", workingDirectory_});
}

void ClaudeBridge::prompt(const QString &text)
{
    send({{"type", "prompt"}, {"text", text}});
}

void ClaudeBridge::interrupt()
{
    send({{"type", "stop"}});
}

void ClaudeBridge::resetConversation()
{
    send({{"type", "new"}});
}

void ClaudeBridge::addDirectory(const QString &path)
{
    send({{"type", "add_directory"}, {"path", path}});
}

void ClaudeBridge::answerApproval(int id, bool allow)
{
    send({{"type", "approval_response"}, {"id", id}, {"allow", allow}});
}

void ClaudeBridge::answerQuestions(int id, const QJsonObject &answers, bool accepted)
{
    send({{"type", "question_response"}, {"id", id}, {"accepted", accepted}, {"answers", answers}});
}

void ClaudeBridge::send(const QJsonObject &message)
{
    if (process_->state() == QProcess::NotRunning) return;
    process_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void ClaudeBridge::handleLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        emit this->error("Invalid Claude bridge message: " + error.errorString());
        return;
    }
    const QJsonObject message = document.object();
    const QString type = message.value("type").toString();
    if (type == "ready") emit ready();
    else if (type == "delta") emit textDelta(message.value("text").toString());
    else if (type == "tool") emit toolStarted(message.value("name").toString(), message.value("input").toObject());
    else if (type == "complete") emit completed(message.value("status").toString(), message.value("details").toString());
    else if (type == "approval") emit approvalRequested(message.value("id").toInt(), message.value("tool").toString(), message.value("input").toObject());
    else if (type == "question") emit questionsRequested(message.value("id").toInt(), message.value("questions").toArray());
    else if (type == "directory_added") emit directoryAdded(message.value("path").toString());
    else if (type == "error") emit this->error(message.value("message").toString());
}
