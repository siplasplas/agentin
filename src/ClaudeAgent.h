#pragma once

#include "AgentBackend.h"
#include "CommandApproval.h"
#include "ConversationIndex.h"

#include <QJsonObject>
#include <QMap>
#include <QPointer>

#include <functional>

class QProcess;

// A Python virtual environment that gets claude-agent-sdk the first time a Claude or GLM bridge needs it.
class ClaudeEnvironment : public QObject
{
    Q_OBJECT

public:
    explicit ClaudeEnvironment(const QString &directory, QObject *parent = nullptr);
    ~ClaudeEnvironment() override;

    QString pythonProgram() const;
    bool isPreparing() const { return process_ != nullptr; }
    // Checks the environment, creating it and installing the SDK when needed, then calls done whether or
    // not that worked, unless context has been destroyed by then; a failed bridge reports its own error.
    void prepare(QObject *context, const std::function<void()> &done);

signals:
    void message(const QString &text);

private:
    void install();
    void finish(bool ready);
    void run(const QString &program, const QStringList &arguments,
             const std::function<void(bool ok, const QString &output)> &next);

    QString directory_;
    bool ready_ = false;
    QProcess *process_ = nullptr;
    QList<QPair<QPointer<QObject>, std::function<void()>>> waiters_;
};

// Claude or GLM through claude/bridge.py, which runs the Claude Agent SDK. The provider keeps the
// conversation index and runs one-shot bridge queries; each chat has its own bridge process.
class ClaudeProvider : public AgentProvider
{
    Q_OBJECT

public:
    // kind is "claude" or "glm", as the bridge's --provider option expects.
    ClaudeProvider(const QString &pythonProgram, const QString &scriptPath, const QString &workingDirectory,
                   const QString &kind, const QString &indexPath, QObject *parent = nullptr);
    ~ClaudeProvider() override;

    QString name() const override;
    AgentHelp help() const override;
    QString externalLock(const QString &id) const override;
    void loadConversations() override;
    // GLM sessions are only those recorded here; Claude also lists SDK sessions from all projects.
    void refreshConversations() override;
    QList<QJsonObject> conversations() const override { return index_.treeEntries(); }
    void recordActivity(const QString &id) override;
    QList<AgentModel> models() const override;
    bool supportsUsageLimits() const override { return true; }
    bool supportsSuggestions() const override { return !pythonProgram_.isEmpty() && !scriptPath_.isEmpty(); }
    void suggest(const QString &workingDirectory, const QString &prompt, QObject *context,
                 const std::function<void(const QString &text, const QString &error)> &done) override;
    QList<UsageLimit> usageLimits() const override { return usage_.values(); }
    AgentBackend *createChat(const QString &workingDirectory, QObject *parent) override;

    // Claude and GLM share the SDK transcript store; sessions recorded by the other agent are not listed here.
    void excludeSessionsOf(const ClaudeProvider *other) { excluded_ = other; }

    QString kind() const { return kind_; }
    // Z.AI publishes no model list, so GLM offers the model names entered in the options.
    QStringList extraModels() const { return extraModels_; }
    void setExtraModels(const QStringList &models) { extraModels_ = models; emit modelsChanged(); }
    QString pythonProgram() const { return pythonProgram_; }
    QString scriptPath() const { return scriptPath_; }
    // Runs the bridge with the environment's Python, which is prepared before the first bridge starts.
    void setEnvironment(ClaudeEnvironment *environment);
    bool isPreparingEnvironment() const { return environment_ && environment_->isPreparing(); }
    void prepareEnvironment(QObject *context, const std::function<void()> &done);
    void rememberConversation(const QString &id, const QString &workingDirectory, const QString &firstPrompt);
    // Records a rate limit event that a chat's bridge received.
    void updateUsage(const QJsonObject &event);
    // Runs the bridge for a one-shot query and calls done when it ends or fails to start, unless
    // context has been destroyed by then.
    void runHelper(const QStringList &arguments, const QString &workingDirectory, QObject *context,
                   const std::function<void(QProcess *process, bool started)> &done);

private:
    void reportIndexError(const QString &error);
    void startHelper(const QStringList &arguments, const QString &workingDirectory, const QPointer<QObject> &context,
                     const std::function<void(QProcess *process, bool started)> &done);

    QPointer<ClaudeEnvironment> environment_;
    QString pythonProgram_;
    QString scriptPath_;
    QString workingDirectory_;
    QString kind_;
    ConversationIndex index_;
    const ClaudeProvider *excluded_ = nullptr;
    QList<QProcess *> helperProcesses_;
    QStringList extraModels_;
    QMap<QString, UsageLimit> usage_;
};

class ClaudeAgent : public AgentBackend
{
    Q_OBJECT

public:
    ClaudeAgent(ClaudeProvider *provider, const QString &workingDirectory, QObject *parent = nullptr);
    ~ClaudeAgent() override;

    QString name() const override { return name_; }
    QString workingDirectory() const override { return workingDirectory_; }
    QString sessionId() const override { return sessionId_; }
    QString statusText() const override;
    bool isResponding() const override { return busy_; }
    bool canInterrupt() const override { return busy_ && !stopRequested_; }
    // A message sent during a turn joins it at the model's next step, or is answered right after it.
    bool supportsSteering() const override { return true; }
    bool canSteer() const override { return busy_ && !stopRequested_ && steeringText_.isEmpty(); }
    bool isSteering() const override { return !steeringText_.isEmpty(); }
    bool steer(const QString &text) override;
    QString model() const override { return model_; }
    QString effort() const override { return effort_; }
    void setModel(const QString &model, const QString &effort) override;
    bool supportsReadOnly() const override { return true; }
    bool isReadOnly() const override { return readOnly_; }
    void setReadOnly(bool readOnly) override;
    bool newConversation(const QString &workingDirectory) override;
    bool resumeConversation(const QString &id, const QString &workingDirectory) override;
    bool prompt(const QString &text) override;
    void interrupt() override;
    void loadHistory(const QString &id, const QString &workingDirectory, bool older) override;
    void cancelHistory() override;
    void answerApproval(int id, ApprovalDecision decision) override;
    bool canResetSessionApprovals() const override { return true; }
    void resetSessionApprovals() override;
    QStringList trustedSessionCommands() const override;
    void trustSessionCommands(const QStringList &rules) override;
    bool removeTrustedSessionCommand(const QString &rule) override;
    void answerQuestions(int id, const QHash<QString, QStringList> &answers) override;

private:
    bool isRunning() const;
    void start(const QString &workingDirectory);
    void startBridge();
    void sendNextPrompt();
    bool applySettings();
    void send(const QJsonObject &message);
    void handleLine(const QByteArray &line);

    QPointer<ClaudeProvider> provider_;
    QString name_;
    QString kind_;
    QString workingDirectory_;
    QProcess *process_;
    QByteArray buffer_;
    QString sessionId_;
    QString pendingResumeId_;
    QString firstPrompt_;
    QString model_;
    QString effort_;
    // What the running bridge uses; a difference is sent before the next prompt.
    QString appliedModel_;
    QString appliedEffort_;
    bool readOnly_ = false;
    bool appliedReadOnly_ = false;
    QStringList queuedPrompts_;
    // The steering message the bridge has not confirmed yet.
    QString steeringText_;
    // The API error the CLI is retrying after, until the turn makes progress or ends.
    QString retry_;
    QHash<int, int> pendingQuestionCounts_;
    // What agentin's rules said about the shell commands of pending approvals, for the commands a chat can
    // trust.
    QHash<int, CommandVerdict> approvalVerdicts_;
    // Hook checks that agentin's rules turned into approvals; they are answered as hook decisions.
    QSet<int> askedCommandChecks_;
    // Command prefixes trusted for this conversation; they count as Allow rules.
    QSet<QString> trustedSessionCommands_;
    // The place a shell command runs in, as the bridge reports it with the command.
    CommandContext commandContext(const QJsonObject &event) const;
    HistoryPages history_;
    quint64 historyGeneration_ = 0;
    bool ready_ = false;
    bool preparing_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool textStarted_ = false;
};
