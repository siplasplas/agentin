# Roadmap

Work that is planned but not done, roughly in order. Items move to `README.md` once they are implemented,
and leave this file.

## Files changed by a turn

The changes window lists the files a turn changed, with their diffs, side by side or unified, and the
"Since" choice (turn, chat, `HEAD`); see `ChangeTracker` and `ChangesWindow`. Still open:

- **Reverting a file** to its baseline from the window: the start content is known (the blob in the
  start commit's tree, or the content kept in memory for a file that already differed from Git), so the
  window can write it back after a confirmation. A file new in the turn would be removed, and a deleted
  one restored.
- **Comparing earlier turns:** keep the baseline of each turn of a chat, not only of the latest and of the
  chat's first, so that "Since" can offer any earlier turn.
- **Syntax highlighting** of the diff, with qcodeedit (github.com/siplasplas/qcodeedit, LGPL-3.0-or-later):
  - Taken with `FetchContent` at a fixed tag (v1.4.0 or later) and its `FIND_PACKAGE_ARGS`, so that an
    installed qcodeedit is used and otherwise CMake fetches and builds it, with its demo and tests off; no
    one has to install it first. This needs CMake 3.24.
  - qcodeedit is built as C++20, and some of its public headers need it, so agentin would move from C++17
    to C++20 (`AGENTS.md` says C++17 today; libdiffcore builds as C++20 too).
  - `qcodeedit-kate` gives `KateSyntaxIndex` and `RulesHighlighter`; the definitions are the Kate XML files
    in `qce::kate::dataDir()` (`~/.local/share/qcodeedit`), which qceditor shares. Without them the diff is
    shown as today. Downloading them with `qcodeedit-katedata` (Qt Network) can come later.
  - The changes window keeps its view: an adapter colours each line with `RulesHighlighter::highlightLine`,
    keeping the highlighting state apart for the old and the new side, which interleave in the unified
    view.
- **qt-extra the same way:** it is required and found only when installed today; `FetchContent` with
  `FIND_PACKAGE_ARGS` would let anyone clone and build agentin without installing it first.

## Windows

agentin builds and runs on Linux only. What a port needs, as far as it is known:

- **Building:** the terminal uses qtermwidget, which needs a Unix pty. It has to become optional, or use
  ConPTY on Windows.
- **Claude Code** runs its Bash tool through Git Bash there, so the Bash parser applies, but paths do not:
  the evaluator and the policy (`src/shell/`, `src/CommandApproval.cpp`) take only paths that start with
  `/` as absolute. `C:\x`, `C:/x` and `/c/x` need one normal form before every comparison, and paths and
  the patterns of the secret and protected lists must be compared without regard to case. Until then a
  secret file named by a Windows path would not ask.
- **Codex** runs PowerShell or cmd, whose command lines must not be read as Bash (`type file` prints a file
  there, `$(…)` is a subexpression, `rm` and `cp` take other options). Until a PowerShell front end gives
  the same `CommandUse` lists, a Codex line on Windows should pass only by an explicit Allow rule, with the
  catalog's defaults off, and with the fixed denials checked by text as `textDenial` does.
- The catalog needs the Windows names of common commands (`dir`, `type`, `copy`, `del`, `Get-Content`,
  `Select-String`, `Remove-Item`, `Set-Content`) and the secret list the Windows places of keys
  (`%USERPROFILE%\.ssh`, `%APPDATA%\gh`).

## Known limits

- Claude Code sessions started outside agentin do not report the directories they may write, so the
  directory lock knows only their working directory.
- The approval rules guard against mistakes; they are not a sandbox. A program of the project runs
  without a question, so a script an agent writes into the project can do what the rules would ask about.
