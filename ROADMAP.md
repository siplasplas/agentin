# Roadmap

Planned work, roughly in order. Items move to `README.md` once they are implemented.

## Model and effort for Claude, GLM and Gemini

Codex chats already choose a model and reasoning effort from the App Server's `model/list`. Extend the
same controls to the other agents:

- Claude: the Agent SDK has no model list, so offer a fixed list (for example `opus`, `sonnet`,
  `haiku`). The model can change during a session (`set_model`); the effort (`low` to `max`) is a
  connection option, so changing it reconnects the bridge and resumes the session.
- GLM: choose the model in the chat instead of only through `GLM_MODEL`.
- Gemini: a fixed or typed model name passed as `--model` on the next turn; the CLI has no effort
  option.
- Antigravity: check what `agy` offers.

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
- **Across application instances:** keep the active turns in a shared registry in the application data
  directory (guarded by `QLockFile`, entries with PID so stale entries from crashed instances can be
  dropped).
- **Other tools:** agents started outside agentdeskt (for example `codex` or `claude` in a terminal)
  do not use the registry. Detecting them, for example by the working directory of running agent
  processes on Linux, would be a heuristic like the existing session locks; decide later whether it is
  worth it.
- **Open questions:** whether a read-only question should be allowed to bypass the lock, and whether
  directories that an agent may write to outside its working directory need to be locked too.
