#include "MainWindow.h"

#include <QDir>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(const QString &codexProgram, const QString &workingDirectory, QWidget *parent)
    : QMainWindow(parent), codexProgram_(codexProgram), workingDirectory_(workingDirectory), server_(new QProcess(this))
{
    setWindowTitle("agentdeskt — Codex");
    resize(850, 600);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    status_ = new QLabel(central);
    output_ = new QPlainTextEdit(central);
    output_->setObjectName("output");
    output_->setReadOnly(true);
    output_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    output_->setPlaceholderText("Codex responses will appear here.");
    input_ = new QLineEdit(central);
    input_->setObjectName("commandInput");
    input_->setPlaceholderText("Enter a message or help, then press Enter");
    sendButton_ = new QPushButton("Send", central);
    stopButton_ = new QPushButton("Stop", central);
    stopButton_->setObjectName("stopButton");
    stopButton_->setEnabled(false);

    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(sendButton_);
    inputRow->addWidget(stopButton_);
    layout->addWidget(status_);
    layout->addWidget(output_, 1);
    layout->addLayout(inputRow);
    setCentralWidget(central);

    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::submitCommand);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::submitCommand);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::requestStop);
    connect(server_, &QProcess::started, this, [this] {
        sendRequest("initialize", {{"clientInfo", QJsonObject{
            {"name", "agentdeskt"}, {"title", "agentdeskt Qt"}, {"version", "0.1.0"}}}});
    });
    connect(server_, &QProcess::readyReadStandardOutput, this, [this] {
        readBuffer_ += server_->readAllStandardOutput();
        qsizetype newline;
        while ((newline = readBuffer_.indexOf('\n')) >= 0) {
            const QByteArray line = readBuffer_.left(newline).trimmed();
            readBuffer_.remove(0, newline + 1);
            if (!line.isEmpty()) handleLine(line);
        }
    });
    connect(server_, &QProcess::readyReadStandardError, this, [this] {
        const QString message = QString::fromUtf8(server_->readAllStandardError()).trimmed();
        if (!message.isEmpty()) appendLine("[Server] " + message);
    });
    connect(server_, &QProcess::errorOccurred, this, [this, codexProgram](QProcess::ProcessError) {
        appendLine("[Server startup error] " + server_->errorString());
        appendLine("Executable: " + codexProgram);
        appendLine("Working directory: " + workingDirectory_);
        appendLine("Try --codex /absolute/path/to/codex if the executable was not found.");
        updateStatus();
    });
    connect(server_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        initialized_ = false;
        busy_ = false;
        stopRequested_ = false;
        stopSent_ = false;
        threadId_.clear();
        activeTurnId_.clear();
        appendLine(QString("[Server exited with code %1]").arg(code));
        updateStatus();
    });

    appendLine("Directory: " + workingDirectory_);
    appendLine("Type help to see the available commands.\n");
    updateStatus();
    if (!QDir(workingDirectory_).exists()) {
        appendLine("[Working directory does not exist: " + workingDirectory_ + "]");
        return;
    }
    server_->setWorkingDirectory(workingDirectory_);
    server_->start(codexProgram, {"app-server", "--stdio"});
    input_->setFocus();
}

MainWindow::~MainWindow()
{
    if (server_->state() != QProcess::NotRunning) {
        server_->terminate();
        if (!server_->waitForFinished(1000)) {
            server_->kill();
            server_->waitForFinished(1000);
        }
    }
}

void MainWindow::submitCommand()
{
    const QString command = input_->text().trimmed();
    if (command.isEmpty()) return;
    input_->clear();
    const QString local = command.toLower();

    if (local == "help" || local == "/help") {
        showHelp();
    } else if (local == "clear" || local == "/clear") {
        output_->clear();
    } else if (local == "new" || local == "/new") {
        if (busy_) {
            appendLine("[Wait for the response to finish, or enter stop.]");
        } else if (initialized_) {
            queuedPrompts_.clear();
            threadId_.clear();
            startThread();
        } else {
            appendLine("[Server is not connected.]");
        }
    } else if (local == "stop" || local == "/stop") {
        requestStop();
    } else if (local == "quit" || local == "/quit" || local == "exit") {
        close();
    } else {
        if (server_->state() == QProcess::NotRunning) {
            appendLine("[Server is not running. Check the codex executable and restart.]");
            return;
        }
        appendLine("You: " + command);
        queuedPrompts_.append(command);
        sendNextPrompt();
    }
}

void MainWindow::showHelp()
{
    appendLine("Commands:");
    appendLine("  help       show this help and App Server options");
    appendLine("  new        start a new conversation");
    appendLine("  clear      clear the output pane");
    appendLine("  stop       interrupt the current response");
    appendLine("  quit       close the application");
    appendLine("All other text is sent to Codex as a message.\n");
    appendLine("Codex App Server options:");

    auto *helpProcess = new QProcess(this);
    helpProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(helpProcess, &QProcess::readyReadStandardOutput, this, [this, helpProcess] {
        appendText(QString::fromUtf8(helpProcess->readAllStandardOutput()));
    });
    connect(helpProcess, &QProcess::errorOccurred, this, [this, helpProcess](QProcess::ProcessError error) {
        appendLine("[Could not load App Server options] " + helpProcess->errorString());
        if (error == QProcess::FailedToStart) helpProcess->deleteLater();
    });
    connect(helpProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, helpProcess](int code, QProcess::ExitStatus) {
        appendText(QString::fromUtf8(helpProcess->readAllStandardOutput()));
        if (code != 0) appendLine(QString("[App Server help exited with code %1]").arg(code));
        appendText("\n");
        helpProcess->deleteLater();
    });
    helpProcess->start(codexProgram_, {"app-server", "--help"});
}

void MainWindow::startThread()
{
    if (!initialized_) return;
    appendLine("[Starting a new conversation]");
    sendRequest("thread/start", {{"cwd", workingDirectory_}, {"serviceName", "agentdeskt"}});
    updateStatus();
}

void MainWindow::sendNextPrompt()
{
    if (!initialized_ || threadId_.isEmpty() || busy_ || queuedPrompts_.isEmpty()) return;
    const QString prompt = queuedPrompts_.takeFirst();
    busy_ = true;
    activeTurnId_.clear();
    stopRequested_ = false;
    stopSent_ = false;
    sendRequest("turn/start", {{"threadId", threadId_},
                               {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", prompt}}}}});
    updateStatus();
}

void MainWindow::requestStop()
{
    if (!busy_) {
        appendLine("[No active response.]");
        return;
    }
    if (stopRequested_) return;
    stopRequested_ = true;
    appendLine("[Interrupting response]");
    sendStopIfPossible();
    updateStatus();
}

void MainWindow::sendStopIfPossible()
{
    if (!busy_ || !stopRequested_ || stopSent_ || threadId_.isEmpty() || activeTurnId_.isEmpty()) return;
    stopSent_ = true;
    sendRequest("turn/interrupt", {{"threadId", threadId_}, {"turnId", activeTurnId_}});
}

qint64 MainWindow::sendRequest(const QString &method, const QJsonObject &params)
{
    const qint64 id = nextRequestId_++;
    pendingRequests_.insert(id, method);
    sendJson({{"id", id}, {"method", method}, {"params", params}});
    return id;
}

void MainWindow::sendNotification(const QString &method, const QJsonObject &params)
{
    sendJson({{"method", method}, {"params", params}});
}

void MainWindow::sendJson(const QJsonObject &message)
{
    if (server_->state() == QProcess::NotRunning) return;
    server_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void MainWindow::handleLine(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        appendLine("[Invalid server response: " + error.errorString() + "]");
        return;
    }
    const QJsonObject message = document.object();
    if (message.contains("method")) {
        const QString method = message.value("method").toString();
        if (message.contains("id")) {
            handleServerRequest(method, message.value("id"), message.value("params").toObject());
        } else {
            handleNotification(method, message.value("params").toObject());
        }
    } else if (message.contains("id")) {
        handleResponse(message);
    }
}

void MainWindow::handleResponse(const QJsonObject &message)
{
    const qint64 id = message.value("id").toInteger();
    const QString method = pendingRequests_.take(id);
    const QJsonObject error = message.value("error").toObject();
    if (!error.isEmpty()) {
        appendLine("[Error in " + method + "] " + error.value("message").toString());
        if (method == "turn/start") {
            busy_ = false;
            activeTurnId_.clear();
            stopRequested_ = false;
            stopSent_ = false;
            sendNextPrompt();
        } else if (method == "turn/interrupt") {
            stopRequested_ = false;
            stopSent_ = false;
        }
        updateStatus();
        return;
    }
    const QJsonObject result = message.value("result").toObject();
    if (method == "initialize") {
        initialized_ = true;
        sendNotification("initialized", {});
        startThread();
    } else if (method == "thread/start") {
        threadId_ = result.value("thread").toObject().value("id").toString();
        if (threadId_.isEmpty()) appendLine("[Server did not return a conversation ID.]");
        else appendLine("[Connected to Codex]");
        sendNextPrompt();
    } else if (method == "turn/start") {
        if (busy_) {
            activeTurnId_ = result.value("turn").toObject().value("id").toString();
            sendStopIfPossible();
        }
    }
    updateStatus();
}

void MainWindow::handleNotification(const QString &method, const QJsonObject &params)
{
    const QJsonObject item = params.value("item").toObject();
    const QString itemId = params.value("itemId").toString(item.value("id").toString());
    if (method == "turn/started") {
        if (busy_) {
            activeTurnId_ = params.value("turn").toObject().value("id").toString();
            sendStopIfPossible();
        }
        updateStatus();
    } else if (method == "item/agentMessage/delta") {
        if (!streamedMessages_.contains(itemId)) {
            streamedMessages_.insert(itemId);
            appendText("\nCodex: ");
        }
        appendText(params.value("delta").toString());
    } else if (method == "item/commandExecution/outputDelta") {
        streamedCommands_.insert(itemId);
        appendText(params.value("delta").toString());
    } else if (method == "item/started" && item.value("type") == "commandExecution") {
        appendLine("\n[Command] " + item.value("command").toString());
    } else if (method == "item/completed") {
        const QString type = item.value("type").toString();
        const QString completedId = item.value("id").toString();
        if (type == "agentMessage") {
            if (streamedMessages_.remove(completedId)) appendText("\n");
            else appendLine("\nCodex: " + item.value("text").toString());
        } else if (type == "commandExecution") {
            if (!streamedCommands_.remove(completedId)) {
                const QString output = item.value("aggregatedOutput").toString();
                if (!output.isEmpty()) appendText(output);
            }
            appendLine(QString("\n[Command: %1]").arg(item.value("status").toString()));
        } else if (type == "fileChange") {
            appendLine(QString("[File changes: %1]").arg(item.value("status").toString()));
        }
    } else if (method == "turn/completed") {
        const QJsonObject turn = params.value("turn").toObject();
        const QString state = turn.value("status").toString();
        if (state != "completed") {
            QString details = turn.value("error").toObject().value("message").toString();
            if (!details.isEmpty()) details.prepend(": ");
            appendLine("[Response: " + state + details + "]");
        }
        busy_ = false;
        activeTurnId_.clear();
        stopRequested_ = false;
        stopSent_ = false;
        updateStatus();
        sendNextPrompt();
    } else if (method == "error") {
        appendLine("[Error] " + params.value("error").toObject().value("message").toString());
    } else if (method == "warning" || method == "configWarning") {
        appendLine("[Warning] " + params.value("message").toString(params.value("summary").toString()));
    }
}

void MainWindow::handleServerRequest(const QString &method, const QJsonValue &id, const QJsonObject &params)
{
    if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval") {
        QString description = params.value("reason").toString();
        if (method == "item/commandExecution/requestApproval") {
            const QJsonObject network = params.value("networkApprovalContext").toObject();
            if (!network.isEmpty()) {
                description += "\nNetwork access: " + network.value("protocol").toString()
                    + "://" + network.value("host").toString();
            } else {
                description += "\n" + params.value("command").toString();
                description += "\nDirectory: " + params.value("cwd").toString();
            }
        } else {
            description += "\n" + params.value("grantRoot").toString();
        }
        const auto answer = QMessageBox::question(this, "Approve Codex action",
                                                   description.trimmed() + "\n\nAllow this action?",
                                                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        const QString decision = answer == QMessageBox::Yes ? "accept" : "decline";
        sendJson({{"id", id}, {"result", QJsonObject{{"decision", decision}}}});
        appendLine("[Approval: " + decision + "]");
    } else if (method == "item/tool/requestUserInput") {
        QJsonObject answers;
        for (const QJsonValue &value : params.value("questions").toArray()) {
            const QJsonObject question = value.toObject();
            const QString key = question.value("id").toString();
            const QString title = question.value("header").toString("Codex question");
            const QString prompt = question.value("question").toString();
            const QJsonArray options = question.value("options").toArray();
            bool accepted = false;
            QString reply;
            if (options.isEmpty()) {
                reply = QInputDialog::getText(this, title, prompt, QLineEdit::Normal, {}, &accepted);
            } else {
                QStringList labels;
                for (const QJsonValue &option : options) labels.append(option.toObject().value("label").toString());
                reply = QInputDialog::getItem(this, title, prompt, labels, 0, false, &accepted);
            }
            answers.insert(key, QJsonObject{{"answers", accepted ? QJsonArray{reply} : QJsonArray{}}});
        }
        sendJson({{"id", id}, {"result", QJsonObject{{"answers", answers}}}});
    } else {
        appendLine("[Unsupported server request: " + method + "]");
        sendJson({{"id", id}, {"error", QJsonObject{{"code", -32601}, {"message", "Unsupported request"}}}});
    }
}

void MainWindow::appendText(const QString &text)
{
    QTextCursor cursor = output_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    output_->setTextCursor(cursor);
    output_->ensureCursorVisible();
}

void MainWindow::appendLine(const QString &text)
{
    appendText(text + '\n');
}

void MainWindow::updateStatus()
{
    QString state;
    if (server_->state() == QProcess::NotRunning) state = "Server: not running";
    else if (busy_) state = "Codex is responding…";
    else if (!threadId_.isEmpty()) state = "Ready";
    else state = "Connecting to Codex App Server…";
    status_->setText(state + "  •  " + QDir::toNativeSeparators(workingDirectory_));
    stopButton_->setEnabled(busy_ && !stopRequested_);
}
