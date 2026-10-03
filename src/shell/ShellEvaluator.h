#pragma once

#include "shell/CommandCatalog.h"
#include "shell/ShellAst.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Walks the tree of a command line and tells what each of its commands would do and where, without running
// anything and without looking at the file system. It follows the current directory through cd, the values of
// variables that the line itself sets, the lists of for loops, and the commands inside substitutions and
// shell -c scripts.
namespace shell {

// One command that the line could run.
struct CommandUse
{
    // The command as written, with the variables set before it and without its redirections.
    QString text;
    // The program and its arguments after wrappers such as env, timeout and xargs, with the values that are
    // known put in; a word that is not known stays as written.
    QStringList words;
    // The name of the program, without the directory of a system program; empty when it is not known.
    QString program;
    // Git's subcommand.
    QString subcommand;
    Effect effect = Effect::Unknown;
    // The directories the command may run in, more than one after a cd that may have failed; empty when the
    // directory is not known.
    QStringList directories;
    // Absolute paths, one per possible directory for a relative one. Glob characters are kept as written.
    // A written path counts whatever the effect, as a redirection can add one to a reading command.
    QStringList reads;
    QStringList writes;
    QStringList executes;
    // The addresses a command that fetches, such as curl or wget, talks to.
    QStringList urls;
    // The variables set for the command or earlier in the line, which may change what it does.
    QStringList environment;
    // Whether xargs adds arguments from its input, which the evaluation does not see. It is a problem as
    // well, unless the program only describes files without showing what is in them.
    bool inputArguments = false;
    // Why the command cannot be judged in full; such a command must not run without a question.
    QStringList problems;
    // The part of writes and problems that comes from the command's redirections, which a rule about the
    // command does not cover.
    QStringList redirectionWrites;
    QStringList redirectionProblems;
};

// What is known before the line runs.
struct Environment
{
    // The directory the line starts in.
    QString directory;
    // The home directory, for ~ and cd without a target; empty when it is not known.
    QString home;
    // Variables with known values, such as HOME and TMPDIR.
    QMap<QString, QString> variables;
};

struct Evaluation
{
    // The commands in the order they are met; with judged false the list may be incomplete, but what is
    // there can still be checked against rules that decline.
    QList<CommandUse> uses;
    // False when the line holds something the evaluation cannot follow; reason says what.
    bool judged = true;
    QString reason;
};

Evaluation evaluate(const NodePtr &tree, const Environment &environment);
// One line per command, for tests and messages.
QString dumpEvaluation(const Evaluation &evaluation);

}
