# Roadmap

Planned work, roughly in order. Items move to `README.md` once they are implemented.

## Token statistics

Show for each turn and for the whole conversation how many tokens went in and out: input, cached
input, output and reasoning, plus the cost and the context window where the agent reports them, for
example in the chat header with details in a tooltip, and a turn summary in the log.

- Codex: `thread/tokenUsage/updated` carries `last` (the latest turn) and `total` (the thread) with
  `inputTokens`, `cachedInputTokens`, `cacheWriteInputTokens`, `outputTokens`, `reasoningOutputTokens`
  and `totalTokens`, and `modelContextWindow`.
- Claude and GLM: the SDK's `ResultMessage` has `usage` (`input_tokens`, `output_tokens`,
  `cache_read_input_tokens`, `cache_creation_input_tokens`), `model_usage` per model and
  `total_cost_usd`; reasoning tokens are not reported separately. The bridge would forward them with
  the turn's completion.
- Gemini CLI: the `result` event of `stream-json` carries statistics with input, output, cached and
  thought tokens; check the exact fields.
- Antigravity CLI: its stream appears to report input, output and thought tokens; check the result
  event.

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

- **Writable directories of other agents:** Codex reports its writable roots, and they are held with
  the working directory. Claude, GLM, Gemini and Antigravity chats hold only their working directory,
  since agentdeskt does not give them other directories; Claude Code sessions outside agentdeskt do
  not report theirs.
