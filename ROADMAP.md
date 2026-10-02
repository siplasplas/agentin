# Roadmap

Planned work, roughly in order. Items move to `README.md` once they are implemented.

## Approvals and questions in the chat

Agents ask for approvals (run a command, change files, access the network) and ask questions with
options. They are answered in a panel in the tab: approval once or for the session, declining with
or without stopping the turn, numbered options with descriptions (several where allowed), option
numbers or an own answer typed in the message field, hidden input for secrets, and lasting rules
the agent proposes (Codex command prefixes, Claude Code's suggested permission rules). Still open:

- **Codex session approvals:** find a way to withdraw them without restarting the App Server.

## Directory lock for running turns

The lock is in place for all tabs and agentin windows (registry `turn-locks.json`), counts busy
Claude Code sessions from their registry, Codex turns from the rollout files that Codex keeps open, and
running Gemini and Antigravity CLI processes, and lets read-only Codex turns through; read-only turns of
the other agents (their plan modes) still wait, because only Codex enforces them. Still open:

- **Writable directories of other agents:** Codex reports its writable roots, and they are held with
  the working directory. Claude, GLM, Gemini and Antigravity chats hold only their working directory,
  since agentin does not give them other directories; Claude Code sessions outside agentin do
  not report theirs.

## Files changed by a turn

A window, or a panel beside the chat, listing the files changed since the current turn started, with the
number of lines added and removed for each:

- **Snapshot:** remember the files of the chat's directory, and the writable directories it holds, when a
  turn starts (contents, or a cheap fingerprint first and contents only of files that then change), so
  the comparison does not depend on Git.
- **Counting:** use the O(NP) diff from libdiffcore (`/home/andrzej/wazne/gitmy/diffmerge/libdiffcore`,
  `diffcore::DiffEngine` over interned lines). Only its main loop is needed for the counts; the slider
  heuristics matter only when a diff is shown.
- **Diff view:** for each file, show the diff computed by the same library, with the slider heuristics.
- **Opening a file:** clicking a file opens it with the application the system associates with its
  type (the desktop's default, as `QDesktopServices::openUrl` or `xdg-open` use it), for example
  JetBrains CLion for `.cpp` or RustRover for `.rs` when those are the defaults, switching to an
  already running instance when there is one.
