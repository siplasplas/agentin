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

The lock is in place for all tabs and agentdeskt windows (registry `turn-locks.json`), counts busy
Claude Code sessions from their registry, and lets read-only Codex turns through; read-only turns of
the other agents (their plan modes) still wait, because only Codex enforces them. Still open:

- **Agents started outside agentdeskt:** they do not use the registry, so their turns have to be
  detected from what each tool leaves behind. What exists today:
  - Claude Code registers every running session in `~/.claude/sessions/<pid>.json` (or under
    `CLAUDE_CONFIG_DIR`) with `cwd`, `status` (for example `busy`) and `statusUpdatedAt`. A session whose
    process is alive and whose status is busy holds its directory. Check which other status values
    exist before relying on them.
  - Codex appends each session to `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`: `turn_context`
    records carry the working directory, and `event_msg` records of type `task_started` and
    `task_complete` mark the turns. A session whose last `task_started` has no `task_complete` is
    running a turn, but a crashed CLI leaves the same state, so it also needs a live process or a
    recent file modification.
  - Codex also runs a local App Server daemon with a control socket (`~/.codex/app-server-control/`,
    reachable through `codex app-server proxy`). If the Codex CLI and desktop app use that daemon,
    agentdeskt could connect to the same server and read thread states directly: `thread/loaded/list`,
    `ThreadStatus` `active` (with `waitingOnApproval` or `waitingOnUserInput`) or `idle`, and
    `thread/status/changed`. Whether the other clients use the daemon is not verified yet.
  - Gemini CLI and Antigravity have no registry. On Linux the working directory of their processes is
    `/proc/<pid>/cwd`. A headless process lives for one turn, but an interactive CLI lives for the whole
    session, so its turns cannot be told apart from idle time; such a process could either lock its
    directory for the whole session or be ignored.
  The application registry and Claude Code's registry are used; the Codex and process-based detection
  remain to be added as heuristics.
- **Open question:** whether directories that an agent may write to outside its working directory need
  to be locked too.
