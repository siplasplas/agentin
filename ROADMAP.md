# Roadmap

Planned work, roughly in order. Items move to `README.md` once they are implemented.

## Model choice for GLM and Antigravity

Codex, Claude and Gemini chats choose a model (and an effort where the agent has one). Still open:

- GLM: choose the model in the chat instead of only through `GLM_MODEL`.
- Antigravity: check what `agy` offers.

## Approvals and questions in the chat

Agents ask for approvals (run a command, change files, access the network) and ask questions with
options. They are answered in a panel in the tab: approval once or for the session, declining with
or without stopping the turn, numbered options with descriptions (several where allowed), option
numbers or an own answer typed in the message field, hidden input for secrets, and lasting rules
the agent proposes (Codex command prefixes, Claude Code's suggested permission rules). Still open:

- **Managing rules:** list and remove the lasting rules added from agentdeskt.

## Directory lock for running turns

Today a conversation is protected only from being continued by two programs at once (the Codex App
Server refuses a thread that another client holds, and Claude, Gemini and Antigravity sessions are
checked through `ProcessLocks`). Nothing stops two agents from changing the same files at the same
time: for example, a Codex tab and a Claude tab in the same directory both editing files while each
runs a task.

Add a lock on working directories, held only while an agent works on a turn:

- **What is locked:** the chat's working directory, compared as a canonical path. A directory conflicts
  with the same directory, with its ancestors and with its descendants, so one agent in `/project` and
  another in `/project/src` cannot run turns at the same time.
- **How long:** from sending a message until the agent hands control back to the user (the turn
  completes, fails or is interrupted). Time spent waiting for an approval or a question in a dialog is
  part of the turn. Between turns the directory is free, so the user can ask Codex, get the answer,
  switch to a Claude tab in the same directory and ask a similar question.
- **Across agents and tabs:** the lock applies to all agents and all tabs of the application, including
  several chats with the same agent.
- **When blocked:** the message waits in the chat's queue, the tab shows which chat holds the directory,
  and the message is sent when that turn ends. The user can still stop the waiting chat or the one
  that holds the lock.
- **Across application instances:** keep the active turns in a shared registry file in the application
  data directory: one entry per running turn with the canonical directory, agent, PID and process start
  time, guarded by `QLockFile`. Entries whose process is gone (PID with a different start time) are
  dropped, so a crashed instance does not keep directories locked. A `QFileSystemWatcher` on the
  registry wakes chats that wait for a directory.
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
  Start with the application registry and Claude Code's registry, which are reliable, and add the Codex
  and process-based detection as heuristics.
- **Read-only turns:** the text of a message does not tell whether the agent will change files; even a
  question can end with an edit or a command that writes. A turn may skip the lock only when the user
  chooses a read-only mode and the agent enforces it. Codex chats already have this mode (the
  **Read-only** box in the chat header); the lock should honor it:
  - Codex: `sandboxPolicy` `readOnly` in `turn/start` (or `sandbox` `read-only` for the thread). The
    operating system sandbox also stops shell commands from writing, so this guarantee is strong.
  - Claude and GLM: `permission_mode` `plan` in the Agent SDK; Gemini: `--approval-mode plan`. These are
    rules of the agent, not an operating system sandbox, so the guarantee is weaker. A cautious first
    version offers read-only turns without the lock only for Codex.
  - Antigravity: check what `agy` offers.
  A read-only turn neither takes nor waits for the lock. It may read files that another agent is
  changing at that moment, so its answer can describe an intermediate state.
- **Open question:** whether directories that an agent may write to outside its working directory need
  to be locked too.
