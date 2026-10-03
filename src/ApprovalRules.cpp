#include "ApprovalRules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace {
QString codexHome()
{
    return qEnvironmentVariable("CODEX_HOME", QDir::home().filePath(".codex"));
}

QString claudeConfigDirectory()
{
    return qEnvironmentVariable("CLAUDE_CONFIG_DIR", QDir::home().filePath(".claude"));
}

void readClaudeSettings(const QString &file, const QString &where, QList<ApprovalRule> &rules)
{
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) return;
    const QJsonArray allowed = QJsonDocument::fromJson(input.readAll()).object().value("permissions").toObject()
                                   .value("allow").toArray();
    for (const QJsonValue &value : allowed) {
        const QString entry = value.toString();
        if (!entry.isEmpty()) rules.append({"Claude and GLM", entry, where, file, entry});
    }
}

QString removeCodexRule(const ApprovalRule &rule)
{
    QFile input(rule.file);
    if (!input.open(QIODevice::ReadOnly)) return input.errorString();
    QStringList lines = QString::fromUtf8(input.readAll()).split('\n');
    input.close();
    const qsizetype index = lines.indexOf(rule.stored);
    if (index < 0) return "the rule is no longer in the file";
    lines.removeAt(index);
    QSaveFile output(rule.file);
    if (!output.open(QIODevice::WriteOnly) || output.write(lines.join('\n').toUtf8()) < 0 || !output.commit())
        return output.errorString();
    return {};
}

// Rewrites the settings file without the entry; other settings are kept, but the file is reformatted.
QString removeClaudeRule(const ApprovalRule &rule)
{
    QFile input(rule.file);
    if (!input.open(QIODevice::ReadOnly)) return input.errorString();
    QJsonObject settings = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    QJsonObject permissions = settings.value("permissions").toObject();
    QJsonArray allowed = permissions.value("allow").toArray();
    bool removed = false;
    for (qsizetype i = 0; i < allowed.size() && !removed; ++i) {
        if (allowed.at(i).toString() != rule.stored) continue;
        allowed.removeAt(i);
        removed = true;
    }
    if (!removed) return "the rule is no longer in the file";
    permissions.insert("allow", allowed);
    settings.insert("permissions", permissions);
    QSaveFile output(rule.file);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(settings).toJson(QJsonDocument::Indented)) < 0
        || !output.commit())
        return output.errorString();
    return {};
}
}

QList<ApprovalRule> codexRules()
{
    QList<ApprovalRule> rules;
    const QDir directory(QDir(codexHome()).filePath("rules"));
    static const QRegularExpression pattern(R"re(pattern\s*=\s*\[(.*?)\])re");
    static const QRegularExpression word(R"re("((?:[^"\\]|\\.)*)")re");
    for (const QFileInfo &info : directory.entryInfoList({"*.rules"}, QDir::Files, QDir::Name)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        for (const QString &line : QString::fromUtf8(file.readAll()).split('\n')) {
            const QString trimmed = line.trimmed();
            if (!trimmed.startsWith("prefix_rule(") || !trimmed.contains(R"re(decision="allow")re")) continue;
            QStringList words;
            auto matches = word.globalMatch(pattern.match(trimmed).captured(1));
            while (matches.hasNext()) words.append(matches.next().captured(1));
            rules.append({"Codex", "commands starting with \"" + words.join(' ') + "\"",
                          "Codex rules (" + info.fileName() + ")", info.filePath(), line, words});
        }
    }
    return rules;
}

QList<ApprovalRule> claudeRules(const QStringList &projectDirectories)
{
    QList<ApprovalRule> rules;
    readClaudeSettings(QDir(claudeConfigDirectory()).filePath("settings.json"), "user settings", rules);
    QSet<QString> seen;
    for (const QString &project : projectDirectories) {
        const QString path = QDir(project).canonicalPath();
        if (path.isEmpty() || seen.contains(path)) continue;
        seen.insert(path);
        readClaudeSettings(QDir(path).filePath(".claude/settings.json"), "project settings in " + path, rules);
        readClaudeSettings(QDir(path).filePath(".claude/settings.local.json"), "local settings in " + path, rules);
    }
    return rules;
}

QString removeApprovalRule(const ApprovalRule &rule)
{
    return rule.agent == "Codex" ? removeCodexRule(rule) : removeClaudeRule(rule);
}
