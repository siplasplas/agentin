#include "shell/CommandCatalog.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <functional>

namespace shell {

namespace {

QStringList split(const char *list)
{
    return QString::fromLatin1(list).split(' ', Qt::SkipEmptyParts);
}

bool isNumber(const QString &text)
{
    if (text.isEmpty()) return false;
    for (const QChar c : text)
        if (!c.isDigit()) return false;
    return true;
}

// A long option without its value.
QString longName(const QString &argument)
{
    return argument.section('=', 0, 0);
}

// Whether an argument is the long option or an abbreviation of it, which getopt accepts; an abbreviation
// that fits several options counts for each of them.
bool isLong(const QString &argument, const QString &option)
{
    const QString name = longName(argument);
    return name.size() > 2 && name.startsWith("--") && option.startsWith(name);
}

// One or more short options written together, such as -rn.
bool isShort(const QString &argument)
{
    return argument.size() > 1 && argument.startsWith('-') && !argument.startsWith("--");
}

// A relative path named after a directory option such as git -C.
QString joined(const QString &directory, const QString &path)
{
    if (directory.isEmpty() || path.startsWith('/')) return path;
    return directory + '/' + path;
}

void block(Classification &result, const QString &problem)
{
    result.effect = Effect::Unknown;
    if (!result.problems.contains(problem)) result.problems.append(problem);
}

void notKnown(Classification &result, const Argument &argument)
{
    const QString problem = "the value of " + argument.text + " is not known";
    if (!result.problems.contains(problem)) result.problems.append(problem);
}

// How the arguments that are not options are used.
enum class Operands {
    Text,          // none of them is a file
    Read,          // files that are read
    Write,         // files that are written
    TextThenRead,  // a pattern or a script, then files that are read
};

// A program whose options need no code of their own. Options are named as written, short ("-o") or long
// ("--output").
struct Entry
{
    const char *names;
    Effect effect;
    Operands operands;
    // Options followed by a value that is not a file.
    const char *values;
    // Options followed by a file that is written.
    const char *writes;
    // Options that write, run a program or change what the operands mean: the catalog cannot judge them.
    const char *blocked;
};

// One line per kind of program. A program is Read only when no option of it writes a file or runs a program,
// apart from those listed as writes or blocked.
const Entry kEntries[] = {
    {"echo printf true false : test [ pwd sleep seq basename dirname which type nproc uname id whoami printenv exit return wait",
     Effect::None, Operands::Text, "", "", ""},
    {"ps pgrep free uptime tr", Effect::Read, Operands::Text, "", "", ""},
    {"date", Effect::Read, Operands::Text, "", "", "-s --set"},
    {"cat wc nl tac rev od hexdump strings md5sum sha1sum sha256sum sha512sum realpath readlink expand fold fmt comm join paste cmp df",
     Effect::Read, Operands::Read, "", "", ""},
    {"head tail", Effect::Read, Operands::Read, "-n -c --lines --bytes", "", ""},
    {"ls", Effect::Read, Operands::Read, "-I --ignore -w --width -T --tabsize --block-size", "", ""},
    {"stat", Effect::Read, Operands::Read, "-c --format --printf", "", ""},
    {"du", Effect::Read, Operands::Read, "-d --max-depth --exclude -B --block-size -t --threshold", "", ""},
    {"cut", Effect::Read, Operands::Read, "-d -f -c -b --delimiter --fields --characters --bytes --output-delimiter", "", ""},
    {"diff", Effect::Read, Operands::Read,
     "-U -C -I -x -W --unified --context --label --exclude --ignore-matching-lines --width", "", ""},
    {"column", Effect::Read, Operands::Read, "-s -o -c -N -H -R -T -O -E -W -l --separator --output-separator", "", ""},
    {"file", Effect::Read, Operands::Read, "-m --magic-file -F --separator -e --exclude -P --parameter", "", "-C --compile"},
    {"tree", Effect::Read, Operands::Read, "-L -P -I --charset --filelimit --timefmt", "", "-o"},
    {"sort", Effect::Read, Operands::Read,
     "-k -t -S -T --key --field-separator --buffer-size --temporary-directory --parallel --batch-size", "-o --output",
     "--compress-program"},
    {"jq", Effect::Read, Operands::TextThenRead, "", "", "-f --from-file"},
    {"touch", Effect::Write, Operands::Write, "-d -t -r --date --reference", "", ""},
    {"mkdir", Effect::Write, Operands::Write, "-m --mode", "", ""},
    {"truncate", Effect::Write, Operands::Write, "-s --size -r --reference", "", ""},
    // The mode of chmod counts as a file of its directory, which costs nothing and never misses a file.
    {"tee chmod", Effect::Write, Operands::Write, "", "", ""},
    {"rm rmdir unlink", Effect::Remove, Operands::Write, "", "", ""},
    {"ssh scp sftp rsync gh glab hub", Effect::Network, Operands::Text, "", "", ""},
};

Classification classifyEntry(const Entry &entry, const QList<Argument> &arguments)
{
    Classification result;
    result.effect = entry.effect;
    const QStringList values = split(entry.values);
    const QStringList writes = split(entry.writes);
    const QStringList blocked = split(entry.blocked);
    // Where an option could write or run something, or an operand is a file, an argument that is not known
    // could be that option or that file.
    const bool strict = entry.operands != Operands::Text || !writes.isEmpty() || !blocked.isEmpty();
    const bool readsValues = entry.operands == Operands::Read || entry.operands == Operands::TextThenRead;
    QList<Argument> operands;
    bool onlyOperands = false;
    const auto writtenValue = [&](int index) {
        if (index >= arguments.size()) return;
        if (arguments.at(index).known) result.writes.append(arguments.at(index).text);
        else notKnown(result, arguments.at(index));
    };
    for (int i = 0; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        if (!argument.known) {
            operands.append(argument);
            continue;
        }
        const QString &text = argument.text;
        if (onlyOperands || !text.startsWith('-') || text == "-") {
            if (text != "-" || onlyOperands) operands.append(argument);
            continue;
        }
        if (text == "--") {
            onlyOperands = true;
            continue;
        }
        if (text.startsWith("--")) {
            const bool hasValue = text.contains('=');
            bool handled = false;
            for (const QString &option : blocked) {
                if (!option.startsWith("--") || !isLong(text, option)) continue;
                block(result, "the option " + longName(text) + " cannot be judged");
                handled = true;
            }
            for (const QString &option : writes) {
                if (handled || !option.startsWith("--") || !isLong(text, option)) continue;
                if (hasValue) result.writes.append(text.section('=', 1));
                else writtenValue(++i);
                handled = true;
            }
            if (handled) continue;
            if (!hasValue && values.contains(text)) ++i;
            // The value of an option the catalog does not know may be a file.
            else if (hasValue && readsValues) result.reads.append(text.section('=', 1));
            continue;
        }
        bool handled = false;
        for (const QString &option : blocked) {
            if (option.startsWith("--") || !text.contains(option.at(1))) continue;
            block(result, "the option " + option + " cannot be judged");
            handled = true;
        }
        for (const QString &option : writes) {
            if (handled || option.startsWith("--") || !text.contains(option.at(1))) continue;
            if (text == option) writtenValue(++i);
            else if (text.startsWith(option)) result.writes.append(text.mid(option.size()));
            else block(result, "the options " + text + " cannot be judged");
            handled = true;
        }
        if (handled) continue;
        if (values.contains(text)) ++i;
    }
    if (entry.operands == Operands::TextThenRead && !operands.isEmpty()) {
        // Without options that change the meaning of the operands, a pattern may hold anything.
        const Argument pattern = operands.takeFirst();
        if (!pattern.known && !blocked.isEmpty()) notKnown(result, pattern);
    }
    for (const Argument &operand : operands) {
        if (!operand.known) {
            if (strict) notKnown(result, operand);
        } else if (entry.operands == Operands::Write) {
            result.writes.append(operand.text);
        } else if (entry.operands != Operands::Text) {
            result.reads.append(operand.text);
        }
    }
    return result;
}

Classification classifyGrep(const QString &program, const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::Read;
    const bool ripgrep = program == "rg";
    // Short options that take a value, which may be written together with the option.
    const QString valueLetters = ripgrep ? "efgtTmABCjMdEr" : "efmABCdD";
    static const QStringList valueLongs = split(
        "--max-count --after-context --before-context --context --include --exclude --exclude-dir --label --glob --iglob "
        "--type --type-not --type-add --threads --max-columns --max-depth --sort --sortr --color --colors --replace "
        "--encoding --engine --max-filesize --context-separator --field-match-separator --path-separator");
    QList<Argument> operands;
    bool patternGiven = false;
    bool onlyOperands = false;
    const auto readValue = [&](const Argument &value) {
        if (value.known) result.reads.append(value.text);
        else notKnown(result, value);
    };
    for (int i = 0; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        const QString &text = argument.text;
        if (!argument.known || onlyOperands || !text.startsWith('-') || text == "-") {
            if (!argument.known || text != "-" || onlyOperands) operands.append(argument);
            continue;
        }
        if (text == "--") {
            onlyOperands = true;
            // What follows -- is never an option, so a pattern there may hold anything.
            if (!patternGiven && i + 1 < arguments.size()) {
                patternGiven = true;
                ++i;
            }
            continue;
        }
        if (text.startsWith("--")) {
            const QString name = longName(text);
            const bool hasValue = text.contains('=');
            if (ripgrep && (name.startsWith("--pre") || name.startsWith("--hostname"))) {
                block(result, "the option " + name + " runs a program");
            } else if (name == "--regexp") {
                patternGiven = true;
                if (!hasValue) ++i;
            } else if (name == "--file" || name == "--exclude-from" || name == "--ignore-file") {
                if (name == "--file") patternGiven = true;
                if (hasValue) result.reads.append(text.section('=', 1));
                else if (++i < arguments.size()) readValue(arguments.at(i));
            } else if (!hasValue && valueLongs.contains(name)) {
                ++i;
            }
            continue;
        }
        for (int j = 1; j < text.size(); ++j) {
            const QChar letter = text.at(j);
            if (!valueLetters.contains(letter)) continue;
            const bool attached = j + 1 < text.size();
            if (letter == 'e' || letter == 'f') patternGiven = true;
            if (letter == 'f') {
                if (attached) result.reads.append(text.mid(j + 1));
                else if (i + 1 < arguments.size()) readValue(arguments.at(i + 1));
            }
            if (!attached) ++i;
            break;
        }
    }
    if (!patternGiven && !operands.isEmpty()) {
        const Argument pattern = operands.takeFirst();
        // ripgrep has options that run a program, and a pattern that is not known could be one of them.
        if (!pattern.known && ripgrep) notKnown(result, pattern);
    }
    for (const Argument &operand : operands) readValue(operand);
    return result;
}

Classification classifySed(const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::Read;
    QStringList scripts;
    QStringList files;
    bool inPlace = false;
    bool scriptGiven = false;
    bool onlyOperands = false;
    QList<Argument> operands;
    const auto script = [&](int index) {
        scriptGiven = true;
        if (index >= arguments.size()) return;
        if (arguments.at(index).known) scripts.append(arguments.at(index).text);
        else notKnown(result, arguments.at(index));
    };
    for (int i = 0; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        const QString &text = argument.text;
        if (!argument.known || onlyOperands || !text.startsWith('-') || text == "-") {
            operands.append(argument);
            continue;
        }
        if (text == "--") {
            onlyOperands = true;
            continue;
        }
        if (text.startsWith("--")) {
            static const QStringList plain = split("--quiet --silent --regexp-extended --null-data --separate --unbuffered "
                                                   "--sandbox --posix --debug --follow-symlinks --line-length");
            if (isLong(text, "--in-place")) {
                inPlace = true;
            } else if (isLong(text, "--expression")) {
                if (text.contains('=')) {
                    scriptGiven = true;
                    scripts.append(text.section('=', 1));
                } else {
                    script(++i);
                }
            } else if (std::none_of(plain.cbegin(), plain.cend(), [&](const QString &option) { return isLong(text, option); })
                       || isLong(text, "--file")) {
                block(result, "the option " + longName(text) + " cannot be judged");
            }
            continue;
        }
        for (int j = 1; j < text.size(); ++j) {
            const QChar letter = text.at(j);
            if (QString("nErsuz").contains(letter)) continue;
            if (letter == 'i') {
                // The rest is the suffix of the backup.
                inPlace = true;
            } else if (letter == 'e') {
                if (j + 1 < text.size()) {
                    scriptGiven = true;
                    scripts.append(text.mid(j + 1));
                } else {
                    script(++i);
                }
            } else if (letter == 'l') {
                if (j + 1 == text.size()) ++i;
            } else {
                block(result, "the options " + text + " cannot be judged");
            }
            break;
        }
    }
    if (!scriptGiven && !operands.isEmpty()) {
        const Argument first = operands.takeFirst();
        if (first.known) scripts.append(first.text);
        else notKnown(result, first);
    }
    for (const QString &text : std::as_const(scripts))
        if (!sedScriptIsSafe(text)) block(result, "the sed script may write files or run programs");
    if (inPlace && result.effect != Effect::Unknown) result.effect = Effect::Write;
    for (const Argument &operand : std::as_const(operands)) {
        if (!operand.known) notKnown(result, operand);
        else if (inPlace) result.writes.append(operand.text);
        else result.reads.append(operand.text);
    }
    return result;
}

Classification classifyAwk(const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::Read;
    QList<Argument> operands;
    for (int i = 0; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        const QString &text = argument.text;
        if (!operands.isEmpty() || !argument.known || !text.startsWith('-') || text == "-") {
            operands.append(argument);
        } else if (text == "--") {
            operands.append(arguments.mid(i + 1));
            break;
        } else if (text == "-F" || text == "-v") {
            ++i;
        } else if (!text.startsWith("-F") && !text.startsWith("-v")) {
            block(result, "the option " + text + " cannot be judged");
        }
    }
    if (operands.isEmpty()) return result;
    const Argument program = operands.takeFirst();
    if (!program.known) notKnown(result, program);
    else if (!awkProgramIsSafe(program.text)) block(result, "the awk program may write files or run programs");
    for (const Argument &operand : std::as_const(operands)) {
        if (operand.known) result.reads.append(operand.text);
        else notKnown(result, operand);
    }
    return result;
}

Classification classifyFind(const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::Read;
    static const QStringList blocked = split("-delete -exec -execdir -ok -okdir -fprint -fprint0 -fprintf -fls");
    // Tests followed by a value, which may be anything.
    static const QStringList valued = split(
        "-name -iname -path -ipath -wholename -iwholename -regex -iregex -lname -ilname -type -xtype -maxdepth -mindepth "
        "-size -mtime -mmin -atime -amin -ctime -cmin -newer -anewer -cnewer -newermt -user -group -perm -uid -gid -links "
        "-inum -samefile -printf -regextype -used -fstype");
    int i = 0;
    while (i < arguments.size() && arguments.at(i).known && QStringList{"-H", "-L", "-P"}.contains(arguments.at(i).text)) ++i;
    for (; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        if (argument.known && (argument.text.startsWith('-') || argument.text == "(" || argument.text == "!")) break;
        if (argument.known) result.reads.append(argument.text);
        else notKnown(result, argument);
    }
    for (; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        if (!argument.known) {
            notKnown(result, argument);
        } else if (blocked.contains(argument.text)) {
            block(result, "find with " + argument.text + " writes files or runs programs");
        } else if (valued.contains(argument.text)) {
            ++i;
        }
    }
    return result;
}

Classification classifyUniq(const QList<Argument> &arguments)
{
    // Collected as written files, which leaves out the values of options.
    static const Entry entry{"uniq", Effect::Read, Operands::Write, "-f -s -w --skip-fields --skip-chars --check-chars", "", ""};
    Classification result = classifyEntry(entry, arguments);
    // The first file is the input and the second the output.
    if (result.writes.size() > 2) block(result, "uniq with more than two files");
    else if (!result.writes.isEmpty()) result.reads.append(result.writes.takeFirst());
    return result;
}

// cp and mv write their last operand, or the directory given with -t; mv also removes what it moves. ln
// writes the link, and what it links to counts as written too, as the link is a way to write there.
Classification classifyCopy(const QString &program, const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::Write;
    QList<Argument> operands;
    QString target;
    bool targetGiven = false;
    bool onlyOperands = false;
    const auto targetAt = [&](int index) {
        targetGiven = true;
        if (index >= arguments.size()) return;
        if (arguments.at(index).known) target = arguments.at(index).text;
        else notKnown(result, arguments.at(index));
    };
    for (int i = 0; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        const QString &text = argument.text;
        if (!argument.known || onlyOperands || !text.startsWith('-') || text == "-") {
            operands.append(argument);
        } else if (text == "--") {
            onlyOperands = true;
        } else if (text.startsWith("--")) {
            if (isLong(text, "--target-directory")) {
                if (text.contains('=')) {
                    targetGiven = true;
                    target = text.section('=', 1);
                } else {
                    targetAt(++i);
                }
            } else if (text == "--suffix") {
                ++i;
            }
        } else if (text == "-t") {
            targetAt(++i);
        } else if (text == "-S") {
            ++i;
        } else if (text.contains('t') || text.contains('S')) {
            block(result, "the options " + text + " cannot be judged");
        }
    }
    for (const Argument &operand : std::as_const(operands))
        if (!operand.known) notKnown(result, operand);
    if (!result.problems.isEmpty()) return result;
    if (targetGiven) {
        result.writes.append(target);
    } else if (operands.size() >= 2) {
        result.writes.append(operands.takeLast().text);
    } else if (program == "ln" && operands.size() == 1) {
        // The link gets the name of its target, in the current directory.
        result.writes.append(operands.first().text.section('/', -1, -1, QString::SectionSkipEmpty));
    } else {
        block(result, program + " without a destination");
        return result;
    }
    for (const Argument &operand : std::as_const(operands)) {
        if (program == "cp") result.reads.append(operand.text);
        else result.writes.append(operand.text);
    }
    return result;
}

// The arguments of a git command that only reads: none may write a file or run a program, and the rest may
// be files.
void readGitArguments(Classification &result, const QString &subcommand, const QString &directory,
                      const QList<Argument> &arguments)
{
    result.effect = Effect::GitRead;
    for (const Argument &argument : arguments) {
        const QString &text = argument.text;
        if (!argument.known) {
            notKnown(result, argument);
        } else if (text.startsWith("--out") || isLong(text, "--output") || isLong(text, "--ext-diff")) {
            // --output writes a file and --ext-diff runs a program.
            block(result, "the option " + longName(text) + " cannot be judged");
        } else if (subcommand == "grep"
                   && (isLong(text, "--open-files-in-pager") || text.startsWith("--open") || (isShort(text) && text.contains('O')))) {
            block(result, "git grep that opens a pager");
        } else if (!text.startsWith('-')) {
            result.reads.append(joined(directory, text));
        }
    }
}

Classification classifyGit(const QList<Argument> &arguments)
{
    Classification result;
    QString directory;
    bool strange = false;
    int i = 0;
    for (; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        if (!argument.known) {
            notKnown(result, argument);
            return result;
        }
        const QString &text = argument.text;
        if (!text.startsWith('-')) break;
        if (text == "-C") {
            if (++i >= arguments.size()) return result;
            if (arguments.at(i).known) directory = joined(directory, arguments.at(i).text);
            else notKnown(result, arguments.at(i));
        } else if (text == "--version" || text == "--help" || text == "-h" || text == "-v") {
            result.effect = Effect::GitRead;
            return result;
        } else if (text != "--no-pager" && text != "-P" && text != "--no-optional-locks" && text != "--literal-pathspecs") {
            // Options that choose another repository or change the configuration, which can name programs.
            strange = true;
            if (QStringList{"-c", "--git-dir", "--work-tree", "--namespace", "--config-env", "--super-prefix"}.contains(text))
                ++i;
        }
    }
    if (i >= arguments.size()) {
        if (!strange && result.problems.isEmpty()) result.effect = Effect::GitRead;
        return result;
    }
    if (!arguments.at(i).known) {
        notKnown(result, arguments.at(i));
        return result;
    }
    const QString subcommand = arguments.at(i).text;
    const QList<Argument> rest = arguments.mid(i + 1);
    result.subcommand = subcommand;
    if (strange) {
        result.problems.append("git with options that change its configuration or repository");
        return result;
    }
    QStringList operands;
    for (const Argument &argument : rest)
        if (argument.known && !argument.text.startsWith('-')) operands.append(argument.text);
    const QString first = operands.value(0);
    const auto has = [&rest](const QStringList &options) {
        return std::any_of(rest.cbegin(), rest.cend(),
                           [&](const Argument &argument) { return argument.known && options.contains(argument.text); });
    };
    const auto read = [&] { readGitArguments(result, subcommand, directory, rest); };
    const auto write = [&] {
        result.effect = Effect::GitWrite;
        result.writes.append(directory.isEmpty() ? QString(".") : directory);
        for (const Argument &argument : rest)
            if (!argument.known) notKnown(result, argument);
    };

    static const QSet<QString> reading{"status", "diff", "log", "show", "ls-files", "rev-parse", "describe", "blame",
                                       "annotate", "cat-file", "shortlog", "rev-list", "ls-tree", "merge-base", "show-ref",
                                       "for-each-ref", "name-rev", "count-objects", "diff-tree", "diff-index", "diff-files",
                                       "whatchanged", "show-branch", "cherry", "grep", "check-ignore", "check-attr", "version"};
    static const QSet<QString> writing{"add", "restore", "checkout", "switch", "merge", "reset", "mv", "rm", "cherry-pick",
                                       "revert", "clean", "gc", "prune", "update-index"};
    static const QSet<QString> network{"ls-remote", "fetch", "pull", "clone", "push", "send-pack", "fetch-pack", "lfs", "svn"};
    if (reading.contains(subcommand)) {
        read();
    } else if (writing.contains(subcommand)) {
        write();
    } else if (network.contains(subcommand)) {
        result.effect = Effect::Network;
    } else if (subcommand == "commit") {
        result.effect = Effect::GitWrite;
        result.writes.append(directory.isEmpty() ? QString(".") : directory);
        // A message may hold anything, also text that is not known. -m may follow options without a value.
        static const QRegularExpression messageOption("^-[aqsvenpS]*m$");
        for (int j = 0; j < rest.size(); ++j) {
            const Argument &argument = rest.at(j);
            if (!argument.known) {
                if (!argument.text.startsWith("--message=")) notKnown(result, argument);
            } else if (argument.text == "--message" || messageOption.match(argument.text).hasMatch()) {
                ++j;
            }
        }
    } else if (subcommand == "rebase") {
        write();
        for (const Argument &argument : rest) {
            if (!argument.known) continue;
            const QString &text = argument.text;
            if (text.startsWith("--ex") || text.startsWith("--i") || (isShort(text) && (text.contains('x') || text.contains('i'))))
                block(result, "git rebase that runs commands or an editor");
        }
    } else if (subcommand == "branch" || subcommand == "tag") {
        static const QStringList plain = split("-a -r -v -vv --all --remotes --verbose --show-current --no-color --column "
                                               "--no-column -i --ignore-case --omit-empty --sort --format --color --abbrev");
        static const QStringList lists = split("-l --list --contains --no-contains --merged --no-merged --points-at");
        bool listing = true;
        bool listOption = false;
        for (const Argument &argument : rest) {
            const QString name = longName(argument.text);
            if (!argument.known) listing = false;
            else if (lists.contains(name)) listOption = true;
            else if (subcommand == "tag" && name.startsWith("-n") && isNumber(name.mid(2) + "0")) listOption = true;
            else if (name.startsWith('-') && !plain.contains(name)) listing = false;
        }
        if (listing && (operands.isEmpty() || listOption)) read();
        else write();
    } else if (subcommand == "stash") {
        if (first == "list" || first == "show") read();
        else write();
    } else if (subcommand == "reflog") {
        if (first == "expire" || first == "delete" || first == "drop") write();
        else read();
    } else if (subcommand == "remote") {
        if (first.isEmpty() || first == "get-url") read();
        else if (first == "show" || first == "update" || first == "prune") result.effect = Effect::Network;
        else if (QStringList{"add", "set-url", "remove", "rm", "rename", "set-head", "set-branches"}.contains(first)) write();
    } else if (subcommand == "config") {
        // Reading only; writing may change the configuration of the user, outside the repository.
        const bool gets = has(split("--get --get-all --get-regexp --get-urlmatch --get-color --get-colorbool -l --list"))
            || first == "get" || first == "list"
            || (operands.size() == 1 && !QStringList{"set", "unset", "edit", "rename-section", "remove-section"}.contains(first));
        const bool other = has(split("--add --unset --unset-all --replace-all --rename-section --remove-section -e --edit -f "
                                     "--file --blob"))
            || std::any_of(rest.cbegin(), rest.cend(), [](const Argument &argument) {
                   return argument.text.startsWith("--file=") || argument.text.startsWith("--blob=");
               });
        if (gets && !other) {
            result.effect = Effect::GitRead;
            for (const Argument &argument : rest)
                if (!argument.known) notKnown(result, argument);
        }
    } else if (subcommand == "worktree") {
        if (first == "list") read();
    } else if (subcommand == "submodule") {
        if (first.isEmpty() || first == "status" || first == "summary") read();
        else if (first == "update" || first == "add") result.effect = Effect::Network;
    }
    return result;
}

// The value after an option that needs one, which must be known where it is a path.
struct OptionReader
{
    const QList<Argument> &arguments;
    Classification &result;
    int index = 0;

    bool atEnd() const { return index >= arguments.size(); }
    const Argument &current() const { return arguments.at(index); }
    // The next argument as a path, or an empty string with a problem noted.
    QString path()
    {
        if (++index >= arguments.size()) {
            block(result, "an option without its value");
            return {};
        }
        if (!arguments.at(index).known) {
            notKnown(result, arguments.at(index));
            return {};
        }
        return arguments.at(index).text;
    }
    void skipValue() { ++index; }
    // Skips a number that may follow options such as -j.
    void skipNumber()
    {
        if (index + 1 < arguments.size() && arguments.at(index + 1).known && isNumber(arguments.at(index + 1).text)) ++index;
    }
};

// Installing writes outside the project.
bool installs(const QString &target)
{
    return target.contains("install");
}

Classification classifyCMake(const QList<Argument> &arguments)
{
    Classification result;
    if (arguments.isEmpty()) return result;
    result.effect = Effect::Build;
    OptionReader reader{arguments, result};
    if (!arguments.first().known) {
        notKnown(result, arguments.first());
        return result;
    }
    const QString mode = arguments.first().text;
    if (arguments.size() == 1 && QStringList{"--version", "-version", "--help", "-help", "-h"}.contains(mode)) {
        result.effect = Effect::Read;
        return result;
    }
    if (mode == "--build") {
        const QString directory = reader.path();
        for (++reader.index; !reader.atEnd(); ++reader.index) {
            const Argument &argument = reader.current();
            if (!argument.known) {
                notKnown(result, argument);
                continue;
            }
            const QString &text = argument.text;
            if (text == "-j" || text == "--parallel") {
                reader.skipNumber();
            } else if (text.startsWith("-j") && isNumber(text.mid(2))) {
            } else if (text == "--target" || text == "-t") {
                while (reader.index + 1 < arguments.size() && !arguments.at(reader.index + 1).text.startsWith('-')) {
                    const Argument &target = arguments.at(++reader.index);
                    if (!target.known) notKnown(result, target);
                    else if (installs(target.text)) block(result, "the target " + target.text + " writes outside the project");
                }
            } else if (text == "--config") {
                reader.skipValue();
            } else if (text != "--clean-first" && text != "-v" && text != "--verbose") {
                // Also "--", after which the options go to the build tool.
                block(result, "cmake --build with " + text + " cannot be judged");
            }
        }
        if (!directory.isEmpty()) {
            result.executes.append(directory);
            result.writes.append(directory);
        }
        return result;
    }
    if (QStringList{"--install", "--open", "--workflow", "--find-package", "--preset", "-E", "-P"}.contains(mode)
        || mode.startsWith("--preset=")) {
        block(result, "cmake " + mode + " cannot be judged");
        return result;
    }
    QString source;
    QString build;
    QStringList positional;
    for (; !reader.atEnd(); ++reader.index) {
        const Argument &argument = reader.current();
        const QString &text = argument.text;
        if (!argument.known) {
            // A definition may hold a value that is not known.
            if (!text.startsWith("-D")) notKnown(result, argument);
        } else if (text == "-S") {
            source = reader.path();
        } else if (text == "-B") {
            build = reader.path();
        } else if (text == "-C" || text == "--toolchain") {
            // A script that cmake runs.
            result.executes.append(reader.path());
        } else if (QStringList{"-D", "-U", "-G", "-T", "-A", "--install-prefix", "--log-level"}.contains(text)) {
            reader.skipValue();
        } else if (text.startsWith("-S")) {
            source = text.mid(2);
        } else if (text.startsWith("-B")) {
            build = text.mid(2);
        } else if (text.startsWith("-C")) {
            result.executes.append(text.mid(2));
        } else if (text.startsWith("--toolchain=")) {
            result.executes.append(text.section('=', 1));
        } else if (text.startsWith("-D") || text.startsWith("-U") || text.startsWith("-G") || text.startsWith("-T")
                   || text.startsWith("-A") || text.startsWith("-W") || text.startsWith("--log-level=")
                   || QStringList{"--fresh", "-N", "-L", "-LA", "-LH", "-LAH", "--warn-uninitialized", "--no-warn-unused-cli",
                                  "--debug-output", "--trace", "--trace-expand", "--log-context"}
                          .contains(text)) {
        } else if (text.startsWith('-')) {
            block(result, "cmake with " + text + " cannot be judged");
        } else {
            positional.append(text);
        }
    }
    if (positional.size() > 1 || (positional.isEmpty() && source.isEmpty() && build.isEmpty())) {
        block(result, "cmake with these arguments cannot be judged");
        return result;
    }
    if (positional.size() == 1) {
        const QString path = positional.first();
        if (source.isEmpty() && build.isEmpty()) {
            // The sources, built in the current directory, or a build directory that is configured again.
            result.executes.append(path);
            result.writes.append(".");
            result.writes.append(path);
            return result;
        }
        if (source.isEmpty()) source = path;
        else if (build.isEmpty()) build = path;
        else block(result, "cmake with these arguments cannot be judged");
    }
    result.executes.append(source.isEmpty() ? QString(".") : source);
    result.writes.append(build.isEmpty() ? QString(".") : build);
    return result;
}

Classification classifyCTest(const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::Build;
    static const QStringList plain = split("--output-on-failure -V -VV --verbose --extra-verbose -N --show-only -Q --quiet "
                                           "--progress --rerun-failed --stop-on-failure --schedule-random -F "
                                           "--force-new-ctest-process --print-labels --no-label-summary --no-subproject-summary");
    static const QStringList valued = split("-R -E -L -LE -C -I --tests-regex --exclude-regex --label-regex --label-exclude "
                                            "--build-config --timeout --stop-time --repeat --repeat-until-fail --test-load "
                                            "--tests-information -FA -FS -FC --fixture-exclude-any --fixture-exclude-setup "
                                            "--fixture-exclude-cleanup");
    QString directory = ".";
    OptionReader reader{arguments, result};
    for (; !reader.atEnd(); ++reader.index) {
        const Argument &argument = reader.current();
        const QString &text = argument.text;
        if (!argument.known) {
            notKnown(result, argument);
        } else if (text == "--test-dir") {
            directory = reader.path();
        } else if (text == "-O" || text == "--output-log" || text == "--output-junit") {
            result.writes.append(reader.path());
        } else if (text == "-j" || text == "--parallel") {
            reader.skipNumber();
        } else if (valued.contains(text)) {
            reader.skipValue();
        } else if (!plain.contains(text) && !(text.startsWith("-j") && isNumber(text.mid(2))) && !text.startsWith("--no-tests=")) {
            block(result, "ctest with " + text + " cannot be judged");
        }
    }
    result.executes.append(directory);
    result.writes.append(directory);
    return result;
}

// make and ninja run the build files of a directory and write into it.
Classification classifyMake(const QString &program, const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::Build;
    const bool ninja = program == "ninja";
    static const QStringList plainMake = split("-k -s -B -n -i -w -r -R --no-print-directory --silent --quiet --keep-going "
                                               "--always-make --dry-run --just-print --trace --output-sync");
    static const QStringList plainNinja = split("-v --verbose -n --quiet");
    QString directory;
    QStringList files;
    OptionReader reader{arguments, result};
    for (; !reader.atEnd(); ++reader.index) {
        const Argument &argument = reader.current();
        const QString &text = argument.text;
        if (!argument.known) {
            notKnown(result, argument);
        } else if (text == "-C" || (!ninja && text == "--directory")) {
            directory = joined(directory, reader.path());
        } else if (text.startsWith("-C")) {
            directory = joined(directory, text.mid(2));
        } else if (!ninja && text.startsWith("--directory=")) {
            directory = joined(directory, text.section('=', 1));
        } else if (text == "-f" || (!ninja && (text == "--file" || text == "--makefile"))) {
            files.append(reader.path());
        } else if (!ninja && (text.startsWith("--file=") || text.startsWith("--makefile="))) {
            files.append(text.section('=', 1));
        } else if (text == "-j" || (!ninja && text == "--jobs")) {
            reader.skipNumber();
        } else if ((text.startsWith("-j") && isNumber(text.mid(2))) || (!ninja && text.startsWith("--jobs="))
                   || (!ninja && text.startsWith("-O"))) {
        } else if (text == "-l" || (ninja && text == "-k")) {
            reader.skipNumber();
        } else if (text.startsWith('-')) {
            if (!(ninja ? plainNinja : plainMake).contains(text)) block(result, program + " with " + text + " cannot be judged");
        } else if (!text.contains('=') && installs(text)) {
            block(result, "the target " + text + " writes outside the project");
        }
    }
    const QString where = directory.isEmpty() ? QString(".") : directory;
    result.executes.append(where);
    for (const QString &file : std::as_const(files)) result.executes.append(joined(directory, file));
    result.writes.append(where);
    return result;
}

// The options of a program that fetches: those followed by a value, and among them those that name a file it
// writes or reads. Options not listed here cannot be judged, as some of them name files.
struct FetchOptions
{
    QString valueLetters;
    QStringList plain;
    QStringList valued;
    QStringList writes;
    QStringList reads;
    QStringList blocked;
};

// curl and wget: where they write and what they read, as their options say, and the addresses they fetch.
Classification classifyFetch(const QString &program, const QList<Argument> &arguments)
{
    const bool curl = program == "curl";
    static const FetchOptions curlOptions{
        "AbcCdDeEFHKmoPQrTtuUwxXyYz",
        split("-s -S -L -f -k -v -i -I -G -N -n -l -q -j -J -R -O -Z -0 -1 -2 -3 -4 -6 -# --silent --show-error --location "
              "--location-trusted --fail --fail-with-body --fail-early --insecure --verbose --include --head --get --compressed "
              "--globoff --create-dirs --remote-time --remote-name --remote-name-all --remote-header-name --raw --no-buffer "
              "--no-progress-meter --progress-bar --http1.0 --http1.1 --http2 --http2-prior-knowledge --http3 --ipv4 --ipv6 "
              "--netrc --netrc-optional --no-keepalive --tcp-nodelay --path-as-is --ssl-reqd --tlsv1.2 --tlsv1.3 --styled-output "
              "--no-styled-output --parallel --list-only --append --disable-eprt --disable-epsv --digest --basic --ntlm "
              "--negotiate --anyauth --junk-session-cookies"),
        split("-A -C -e -H -m -P -Q -r -t -u -U -x -X -y -Y -z --user-agent --continue-at --referer --header --max-time "
              "--connect-timeout --retry --retry-delay --retry-max-time --max-redirs --range --request --user --proxy --proxy-user "
              "--limit-rate --speed-limit --speed-time --resolve --connect-to --url --max-filesize --interface --dns-servers "
              "--parallel-max --expect100-timeout --keepalive-time --time-cond --data-raw --oauth2-bearer --json"),
        split("-o -D -c --output --dump-header --cookie-jar --trace --trace-ascii --stderr --libcurl --etag-save --hsts --alt-svc"),
        split("-T --upload-file --cacert --cert --key --capath --netrc-file --etag-compare --proxy-cacert"),
        split("-K --config --output-dir")};
    static const FetchOptions wgetOptions{
        "OoaPiDUTtwQBlARIXe",
        split("-q -v -nv -c -N -r -p -k -K -S -x -nd -nH -np -E -H -L -F -4 -6 --quiet --verbose --no-verbose --continue "
              "--timestamping --recursive --page-requisites --convert-links --backup-converted --server-response "
              "--force-directories --no-directories --no-host-directories --no-parent --adjust-extension --span-hosts "
              "--relative --spider --no-check-certificate --content-disposition --no-clobber --inet4-only --inet6-only "
              "--show-progress --no-cookies --trust-server-names --ignore-case --mirror -m --https-only --no-cache"),
        split("-D -U -T -t -w -Q -B -l -A -R -I -X --domains --user-agent --timeout --tries --wait --quota --base --level "
              "--accept --reject --include-directories --exclude-directories --header --method --max-redirect --limit-rate "
              "--user --password --referer --post-data --body-data --dns-timeout --connect-timeout --read-timeout "
              "--waitretry --random-wait"),
        split("-O -o -a -P --output-document --output-file --append-output --directory-prefix --save-cookies"),
        split("-i --input-file --post-file --body-file --load-cookies --ca-certificate --certificate --private-key"),
        split("-e --execute --config")};
    const FetchOptions &options = curl ? curlOptions : wgetOptions;
    Classification result;
    result.effect = Effect::Network;
    bool stdoutOnly = false;
    bool remoteName = false;
    QString directory;
    const auto value = [&](const QString &option, const Argument &argument) {
        const QString text = argument.text;
        if (!argument.known) {
            if (options.writes.contains(option) || options.reads.contains(option)) notKnown(result, argument);
            return;
        }
        if (options.writes.contains(option)) {
            if (text == "-") stdoutOnly = stdoutOnly || option == "-O" || option == "--output-document";
            else if (option == "-P" || option == "--directory-prefix") directory = text;
            else result.writes.append(text);
        } else if (options.reads.contains(option)) {
            result.reads.append(text);
        } else if (curl && QStringList{"-d", "--data", "--data-binary", "--data-urlencode", "-F", "--form", "-w", "--write-out",
                                       "-b", "--cookie"}.contains(option)) {
            // Data and forms read a file named after @ or <, a format after @, and cookies from a file without =.
            const qsizetype at = text.indexOf(QRegularExpression("[@<]"));
            if ((option == "-b" || option == "--cookie") && !text.contains('=')) result.reads.append(text);
            else if (at >= 0 && (at == 0 || option != "-w")) result.reads.append(text.mid(at + 1).section(';', 0, 0));
        } else if (option == "--url") {
            result.urls.append(text);
        }
    };
    const QStringList curlData{"-d", "--data", "--data-binary", "--data-urlencode", "-F", "--form", "-w", "--write-out", "-b", "--cookie"};
    for (int i = 0; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        const QString &text = argument.text;
        if (!argument.known) {
            notKnown(result, argument);
            continue;
        }
        if (text == "-" || !text.startsWith('-')) {
            result.urls.append(text);
            continue;
        }
        if (text.startsWith("--")) {
            const QString name = longName(text);
            const bool attached = text.contains('=');
            if (options.blocked.contains(name)) {
                block(result, "the option " + name + " cannot be judged");
            } else if (options.plain.contains(name)) {
                if (name == "--remote-name" || name == "--remote-name-all") remoteName = true;
                if (name == "--netrc" || name == "--netrc-optional") result.homeReads.append(".netrc");
            } else if (options.valued.contains(name) || options.writes.contains(name) || options.reads.contains(name)
                       || (curl && curlData.contains(name))) {
                if (attached) value(name, {text.section('=', 1), true});
                else if (i + 1 < arguments.size()) value(name, arguments.at(++i));
            } else {
                block(result, "the option " + name + " cannot be judged");
            }
            continue;
        }
        if (!curl && options.plain.contains(text)) continue;
        // Short options written together; one that takes a value ends the group.
        for (int j = 1; j < text.size(); ++j) {
            const QString option = QString('-') + text.at(j);
            if (options.valueLetters.contains(text.at(j))) {
                if (options.blocked.contains(option)) block(result, "the option " + option + " cannot be judged");
                else if (j + 1 < text.size()) value(option, {text.mid(j + 1), true});
                else if (i + 1 < arguments.size()) value(option, arguments.at(++i));
                break;
            }
            if (!options.plain.contains(option)) {
                block(result, "the option " + option + " cannot be judged");
                break;
            }
            if (option == "-O") remoteName = true;
            if (option == "-n") result.homeReads.append(".netrc");
        }
    }
    // curl -O and wget without -O save under the name of the address, in the current or chosen directory.
    if ((curl && remoteName) || (!curl && !stdoutOnly && result.writes.isEmpty()))
        result.writes.append(directory.isEmpty() ? QString(".") : directory);
    if (!curl && !directory.isEmpty() && !result.writes.contains(directory)) result.writes.append(directory);
    return result;
}

// set with the options that only change how the shell reports and stops.
Classification classifySet(const QList<Argument> &arguments)
{
    Classification result;
    result.effect = Effect::None;
    for (int i = 0; i < arguments.size(); ++i) {
        const Argument &argument = arguments.at(i);
        const QString &text = argument.text;
        bool fine = argument.known && text.size() > 1 && (text.startsWith('-') || text.startsWith('+'));
        for (int j = 1; fine && j < text.size(); ++j) fine = QString("euxo").contains(text.at(j));
        if (fine && text.endsWith('o')) {
            ++i;
            fine = i < arguments.size() && arguments.at(i).known
                && QStringList{"pipefail", "errexit", "nounset", "xtrace"}.contains(arguments.at(i).text);
        }
        if (!fine) {
            block(result, "set with these arguments cannot be judged");
            break;
        }
    }
    return result;
}

using Classifier = std::function<Classification(const QString &, const QList<Argument> &)>;

const QHash<QString, Classifier> &classifiers()
{
    static const QHash<QString, Classifier> table = [] {
        QHash<QString, Classifier> entries;
        for (const Entry &entry : kEntries) {
            const Entry *pointer = &entry;
            for (const QString &name : split(entry.names))
                entries.insert(name, [pointer](const QString &, const QList<Argument> &arguments) {
                    return classifyEntry(*pointer, arguments);
                });
        }
        const auto add = [&entries](const char *names, const Classifier &classifier) {
            for (const QString &name : split(names)) entries.insert(name, classifier);
        };
        const auto plain = [](Classification (*function)(const QList<Argument> &)) {
            return [function](const QString &, const QList<Argument> &arguments) { return function(arguments); };
        };
        add("grep egrep fgrep rg", classifyGrep);
        add("sed", plain(classifySed));
        add("awk gawk mawk", plain(classifyAwk));
        add("find", plain(classifyFind));
        add("uniq", plain(classifyUniq));
        add("cp mv ln", classifyCopy);
        add("git", plain(classifyGit));
        add("cmake", plain(classifyCMake));
        add("ctest", plain(classifyCTest));
        add("make gmake ninja", classifyMake);
        add("set", plain(classifySet));
        add("curl wget", classifyFetch);
        return entries;
    }();
    return table;
}

}

QString effectName(Effect effect)
{
    switch (effect) {
    case Effect::Unknown: return "unknown";
    case Effect::None: return "none";
    case Effect::Read: return "read";
    case Effect::GitRead: return "git-read";
    case Effect::GitWrite: return "git-write";
    case Effect::Network: return "network";
    case Effect::Write: return "write";
    case Effect::Remove: return "remove";
    case Effect::Build: return "build";
    case Effect::Execute: return "execute";
    }
    return "?";
}

Classification classifyProgram(const QString &program, const QList<Argument> &arguments)
{
    const auto found = classifiers().constFind(program);
    if (found == classifiers().constEnd()) return {};
    return (*found)(program, arguments);
}

bool describesWithoutContents(const QString &program)
{
    static const QSet<QString> programs{"wc", "ls", "stat", "du", "md5sum", "sha1sum", "sha256sum", "sha512sum", "realpath",
                                        "readlink", "basename", "dirname", "echo", "printf"};
    return programs.contains(program);
}

bool sedScriptIsSafe(const QString &script)
{
    const int size = int(script.size());
    int i = 0;
    const auto at = [&](int index) { return index < size ? script.at(index) : QChar(); };
    const auto skipBlanks = [&] {
        while (at(i) == ' ' || at(i) == '\t') ++i;
    };
    const auto skipDigits = [&] {
        const int start = i;
        while (at(i).isDigit()) ++i;
        return i > start;
    };
    // A line number, $ or /regex/; false for an address that is not understood.
    const auto address = [&] {
        if (at(i).isDigit()) {
            skipDigits();
            if (at(i) == '~') {
                ++i;
                return skipDigits();
            }
            return true;
        }
        if (at(i) == '$') {
            ++i;
            return true;
        }
        if (at(i) == '/') {
            ++i;
            while (i < size && at(i) != '/') {
                if (at(i) == '\n') return false;
                i += at(i) == '\\' ? 2 : 1;
            }
            if (i >= size) return false;
            ++i;
            while (at(i) == 'I' || at(i) == 'M') ++i;
            return true;
        }
        return true;
    };
    for (;;) {
        while (at(i) == ' ' || at(i) == '\t' || at(i) == ';' || at(i) == '\n' || at(i) == '}') ++i;
        if (i >= size) return true;
        const int start = i;
        if (!address()) return false;
        if (i > start) {
            skipBlanks();
            if (at(i) == ',') {
                ++i;
                skipBlanks();
                if (at(i) == '+' || at(i) == '~') {
                    ++i;
                    if (!skipDigits()) return false;
                } else {
                    const int second = i;
                    if (!address() || i == second) return false;
                }
            }
        }
        skipBlanks();
        while (at(i) == '!') {
            ++i;
            skipBlanks();
        }
        if (i >= size) return false;
        const QChar command = at(i++);
        if (command == '{') continue;
        if (command == 'q' || command == 'Q') {
            skipBlanks();
            skipDigits();
        } else if (command == 's' || command == 'y') {
            const QChar delimiter = at(i++);
            if (delimiter.isNull() || delimiter == '\\' || delimiter == '\n' || delimiter.isSpace()) return false;
            for (int part = 0; part < 2; ++part) {
                while (i < size && at(i) != delimiter) i += at(i) == '\\' ? 2 : 1;
                if (i >= size) return false;
                ++i;
            }
            // The flags e and w run the result and write a file.
            if (command == 's')
                while (i < size && QString("gpiIMm0123456789").contains(at(i))) ++i;
        } else if (!QString("pdDPhHgGxnN=").contains(command)) {
            // Also the commands that read, write or run: r, R, w, W, e; and those with text or labels.
            return false;
        }
        skipBlanks();
        if (i < size && at(i) != ';' && at(i) != '\n' && at(i) != '}') return false;
    }
}

bool awkProgramIsSafe(const QString &program)
{
    // Running programs, reading other files, loading code, and changing the list of input files.
    for (const char *word : {"system", "getline", "@", "ARGV", "ARGC"})
        if (program.contains(QLatin1String(word))) return false;
    // Only print and printf can write to a file or a pipe; without them > and | compare and combine.
    return !program.contains("print") || (!program.contains('>') && !program.contains('|'));
}

}
