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
Claude Code sessions from their registry, Codex turns from the rollout files that Codex keeps open, and
running Gemini and Antigravity CLI processes, and lets read-only Codex turns through; read-only turns of
the other agents (their plan modes) still wait, because only Codex enforces them. Still open:

- **Open question:** whether directories that an agent may write to outside its working directory need
  to be locked too.
