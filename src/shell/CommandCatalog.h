#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// What common programs do with their arguments: which of them are files that are read, written or run, and
// what kind of effect the command has. The catalog never looks at the file system; it reads only the
// arguments, and answers Unknown for a program, an option or a script it does not know.
namespace shell {

enum class Effect {
    // No entry, or arguments the catalog cannot judge.
    Unknown,
    // Changes at most the shell's own state: cd, export, true, echo.
    None,
    // Reads files or the system's state.
    Read,
    // Reads a Git repository.
    GitRead,
    // Changes a Git repository or its work tree; rules decide which of these commands ask.
    GitWrite,
    // Talks to another machine.
    Network,
    // Creates or changes the files in writes.
    Write,
    // Removes the files in writes.
    Remove,
    // Configures, builds or tests a project: runs the project's code from executes and writes into writes.
    Build,
    // Runs the program in executes, which was given by its path.
    Execute,
};

QString effectName(Effect effect);

// One argument of a command after expansion; an argument that is not known keeps its text as written.
struct Argument
{
    QString text;
    bool known = true;
};

struct Classification
{
    Effect effect = Effect::Unknown;
    // Git's subcommand, also when the effect is Unknown, so that rules can match it.
    QString subcommand;
    // Paths as the command names them, relative to its directory. A written path counts whatever the effect.
    QStringList reads;
    QStringList writes;
    QStringList executes;
    // The addresses a command that fetches, such as curl or wget, talks to.
    QStringList urls;
    // Files in the home directory that the command reads by itself, such as curl's .netrc.
    QStringList homeReads;
    // Why the command cannot be judged in full, such as an argument whose value is not known where a path
    // or an option could stand. A command with problems must not run without a question.
    QStringList problems;
};

// The entry of a program given by its name, with the arguments after the name.
Classification classifyProgram(const QString &program, const QList<Argument> &arguments);

// Whether a program tells about files without showing what is in them, as wc and ls do, and has no option
// that writes or runs something. Such a program may get its files from xargs, where they cannot be checked.
bool describesWithoutContents(const QString &program);

// Whether a sed script only prints, deletes or substitutes lines: no command that reads or writes a file or
// runs a program.
bool sedScriptIsSafe(const QString &script);
// Whether an awk program has nothing that runs a program, redirects output or chooses its own input.
bool awkProgramIsSafe(const QString &program);

}
