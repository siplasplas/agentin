# agentin

A Qt 6 desktop client for Codex, Claude, GLM, Gemini, and Antigravity. Start a
chat with an agent, enter a message, and read streamed responses in the chat
pane; application messages appear in the log pane at the bottom. Codex uses the local `codex` executable and its existing sign-in and
configuration. Claude and GLM run through a Python JSONL bridge to the Claude
Agent SDK. Gemini and Antigravity run through their respective CLIs in headless mode.

## Build and run

Requires Qt 6 Widgets, CMake, a C++17 compiler, the `qt-extra` library version 2.1 or newer
(installed so that `find_package(qt-extra 2.1)` finds it; it provides the chat tabs
and the directory chooser), and an installed `codex` CLI for Codex. Claude requires Python 3, `claude-agent-sdk`, and a configured API
key. GLM uses the same Python SDK with a Z.AI API key. Gemini requires an
installed and authenticated Gemini CLI. Antigravity requires an installed and
authenticated `agy` CLI. The optional test also requires Qt 6
Test and Python 3.

```sh
cmake -S . -B build
cmake --build build
./build/agentin -C /path/to/project
```

`cmake --install build` (with `--prefix` to choose another location) installs
`bin/agentin` and the Claude bridge as `share/agentin/claude_bridge.py`. The
executable uses the bridge next to itself, as in the build directory, and
otherwise the one in `../share/agentin`; `--claude-bridge` overrides both.

For Claude, provide `ANTHROPIC_API_KEY` in the application's environment:

```sh
ANTHROPIC_API_KEY=your-key ./build/agentin -C /path/to/project
```

Claude and GLM need the Claude Agent SDK. The first time either is used (a new
or resumed chat, a history preview, or expanding Claude in the tree), the
application creates a Python virtual environment `claude-venv` in its
application data directory and installs `claude-agent-sdk` into it; the log
shows the progress. This needs `python3` with the `venv` module (on Debian and
Ubuntu the `python3-venv` package) and network access, takes about a minute and
a few hundred megabytes, and happens once; Claude and GLM share the
environment. A failed installation is tried again the next time Claude or GLM
starts. When developing, you can instead install the SDK in a project virtual
environment, which the application then uses:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install claude-agent-sdk
```

For GLM, install the same SDK and set `ZAI_API_KEY` to your Z.AI Coding Plan
key. The GLM bridge connects to Z.AI's Anthropic-compatible endpoint. Set
`GLM_MODEL` to override the default `glm-5.3` model. Claude and GLM keep
separate processes and conversations.

```sh
ZAI_API_KEY=your-zai-key ./build/agentin -C /path/to/project
```

For Gemini, install and authenticate [Gemini CLI](https://geminicli.com/docs/get-started/).
The application starts `gemini --output-format stream-json --prompt ...` for
each turn and uses the session ID returned by Gemini CLI to resume the next
turn. Use `--gemini /path/to/gemini` if it is not in `PATH`.
Google no longer accepts personal Google account sign-in in Gemini CLI. Use a
[Gemini API key](https://geminicli.com/docs/get-started/authentication/#use-gemini-api-key)
or an eligible enterprise Gemini Code Assist account. Google's replacement for
personal account terminal use is Antigravity CLI, available as a separate provider
in this application.

For Antigravity, install and sign in to [Antigravity CLI](https://antigravity.google/docs/cli/install/)
interactively first. The application starts `agy --output-format stream-json
--prompt ...` for each turn and uses `--conversation ID` for later turns. Pass
`--antigravity /path/to/agy` if the executable is not in `PATH`.

The client uses `.venv/bin/python` when the build directory is directly inside
this project and that environment exists. `--claude-python /path/to/python`
selects another Python with the SDK installed; in both cases nothing is
installed automatically. Use
`--claude-bridge /path/to/bridge.py` when running an executable from another
location without its copied bridge script.

Without `-C`, the conversation uses the terminal's current directory. If
`codex` is not in `PATH`, the client also checks the Codex desktop installation
and `CODEX_BIN`. You can select an executable explicitly with
`--codex /path/to/codex`.

## Conversations

The left pane groups chats by agent and working directory. Expanding Codex
loads its conversations through App Server, including chats from Codex CLI,
Codex desktop, and this application when they use the same local Codex data.
Each provider node has an expand control. Expanding Codex refreshes
the list. The app saves a local JSON index in its application data directory;
the first sync scans the full history and can take longer. Later syncs read
new active chats from newest to oldest until they reach the saved boundary.
Archived chats are scanned separately. Double click a chat to resume it.
After restoring tabs, the tree expands the current chat's agent and directory,
selects and reveals its conversation, and receives keyboard focus so the chat is
easy to resume. Switching tabs also reveals the matching conversation without
moving keyboard focus. Restored chats appear even before discovery finishes.
Directories appear under an agent only when they contain a discovered chat.
Chat previews are shortened in the tree; hover over one to read the longer stored preview.
Codex previews in the local JSON index are limited to 200 characters.

Expanding Claude lists SDK sessions across all Claude projects. Expanding
Gemini reads the CLI's local `~/.gemini/projects.json` and session files under
`~/.gemini/tmp/<project>/chats/` to list conversations across all Gemini
projects. Discovery does not depend on the application's current directory or
on Gemini CLI authentication. The working directory stored with a session is
used when that session is resumed. Discovered sessions are saved in the local
`gemini-conversations.json` index and reloaded on the next launch.
GLM and Antigravity show chats recorded by this application. The Antigravity
CLI documents resuming by ID but does not expose a machine-readable command to
list all existing conversations. The Claude SDK's shared transcript location does
not identify which endpoint produced an older external session. Claude, Gemini,
GLM, and Antigravity chats started here are saved in separate `claude-conversations.json`,
`gemini-conversations.json`, `glm-conversations.json`, and `antigravity-conversations.json` files in the local
application data directory. The old `agent-conversations.json` file is ignored.
Claude's local SDK list is separate from the chat history in a Claude web
account. Available Claude metadata includes creation and modification times,
transcript size, Git branch, tag, summary, and first prompt; hover over a chat
to see its details.
Deleting the Codex index makes the next Codex expansion scan its full history.
Claude can rediscover SDK sessions. Gemini can rediscover locally saved CLI
sessions across its projects. GLM entries created here need the local GLM index to
remain available. Antigravity entries created here likewise need the local
Antigravity index to remain available.

Use the **New chat…** button, Ctrl+T, **Conversations → New conversation in
directory…**, or type `new` to start a chat. Ctrl+W closes the current tab. In the dialog, choose the agent,
then enter a directory path or choose one with **Browse…**, which also lists
recently used directories. The **Create chat**
button is available only when that directory exists. Each chat opens in its own
tab and has one working directory; files under that directory are available
subject to the selected agent's permissions. At startup one Codex tab is ready
in the working directory, and its conversation starts with the first message.

When the window closes, the open tabs are saved in `open-tabs.json` in the
application data directory, with their model, effort and read-only mode, and
the next start reopens them and continues the chats that were live; Codex chats
continue once the App Server is connected. The preview tab is not saved. When a
directory is given with `-C`, its new chat opens next to the reopened tabs.

Chats in different tabs run independently, also several chats with the same
agent. All Codex chats share one App Server process. A tab whose agent is
responding shows a busy marker, and a background tab that receives output is
marked until you switch to it. Closing a tab while its agent is responding asks
first and then stops the response. Ctrl+Tab switches tabs in most recently used
order.

In a Codex tab, choose the model and reasoning effort in the chat header.
The lists come from the App Server (`model/list`); each model offers only the
efforts it supports, and hovering shows their descriptions. A new choice applies
from the next message on and stays with that conversation. When a saved
conversation is continued, the fields show the model and effort it used.

Claude and Gemini tabs have the same fields. Claude offers Claude Code's model
aliases (`fable`, `opus`, `sonnet`, `haiku`) and efforts from `low` to `max`; a
new model applies within the session, while a new effort reconnects the bridge
and resumes the session before the next message. Gemini offers Gemini CLI's
aliases (`auto`, `pro`, `flash`, `flash-lite`), passed as `--model` on the next
turn; it has no effort setting. Z.AI publishes no model list, so GLM offers the
default model (`GLM_MODEL`, or `glm-5.3`) and the model names typed into the GLM
row of **Settings → Options…**; a new GLM model reconnects the bridge and
resumes the session. Antigravity lists its models with `agy models` when the
application starts and passes the chosen one as `--model`; the reasoning level is
part of each model (for example `gemini-3.8-flash-high`), so there is no separate
effort.

A Codex chat can be switched to **Read-only** in its header. From the next
message on, turns use Codex's read-only sandbox, so the agent can read files but
not change them, also through shell commands. Switching it off restores the
thread's own sandbox policy. The box also shows when a thread is read-only
already, for example because Codex does not trust its directory. In Claude and
GLM chats the box switches the session to Claude Code's plan mode, and Gemini
and Antigravity turns get `--approval-mode plan` and `--mode plan`: the agent
reads and plans but is told not to change files. Only Codex enforces this with a
sandbox, so only read-only Codex turns skip the directory lock.

**Settings → Options…** sets the model and effort that new chats start with, for
each agent that offers a choice. The effort is `medium` unless changed there.
The options are saved in `settings.json` in the application data directory.
A continued Codex conversation keeps the model and effort it used; Claude and
Gemini sessions do not record them, so continued chats start with the defaults.

The chat header shows the tokens of the latest turn and of the conversation as
input→output, for example `turn 2.5k→300  chat 11.5k→1.1k`; its tooltip lists
input with cached tokens, output with reasoning tokens, the total, the cost and
how full the context window is, as far as the agent reports them, and each
finished turn is summarized in the log. Codex reports the whole thread (a turn is
the difference in the thread's totals), Claude and GLM report input, cache and
output tokens and the cost but not reasoning separately, and Gemini reports
input, cached and output tokens. For agents other than Codex, the conversation
figure covers the turns sent from the tab. Antigravity's statistics are read
where its result event carries them.

During a running Codex turn, **Steer** sends the message field's text directly to
that turn, while **Send** continues to queue a message for the next turn. Steer is
available once the turn ID is known, except during manual compaction, stopping,
or a pending approval/question. Accepted messages appear as `You (steer)` and are
included in message history. If the turn finishes before the server accepts the
message, the error is shown and the text is restored to an empty message field
when its chat is still selected; it is also kept in the transcript. Steering does
not interrupt tools or guarantee an immediate answer.

The chat header shows elapsed operation time as `mm:ss`: **Task** while an agent
works and **Compact** during manual or automatic Codex compaction. The counter
stops at the final duration when the operation ends and follows the selected tab.
The application log records each task and compaction duration, including operations
that fail or are interrupted. Waiting for a directory lock is not timed.

The **Compact** button above a Codex chat manually compacts its context when the
chat is live and idle. The read-only field beside it shows current context tokens
reported by App Server, with apostrophes grouping thousands (for example
`100'000` or `1'000'000`). It updates with the server's usage reports, including
after compaction; an em dash means no usage report has arrived yet. Compaction
can be interrupted with **Stop** and does not count as a normal prompt completion.

After Codex compacts its context, a marker appears in the chat. When the running
turn ends, the chat reloads its latest stored history from App Server before
starting another turn. Messages queued before or during the refresh stay visible
without being appended a second time. Older messages remain available through
**Show earlier messages**. A refresh error leaves the existing transcript intact.

In a Codex chat, **Fast** after the **Compact** token counter requests faster model responses
with higher limit usage, where supported. It applies from the next turn and is
kept separately for each open chat. It is always off after restarting agentin,
including restored conversations; the setting is not saved in Codex configuration.

The chat, message field and log use a 10-point monospace font by default.
**Settings → Chat font…** changes the font and size and saves the choice.
**Settings → Chat font size…** also accepts fractional sizes, such as 10.5 points,
with a 0.5-point step.
Tool details and output are folded by default; click the colored tool heading
or its margin marker to expand or collapse them. A plus marks folded output and
a minus marks expanded output. Headings without additional content have an empty
box and cannot be toggled. The final tool status stays visible. Folding keeps
the full text in the conversation and also applies to multiline tool entries
in loaded history.
The scroll range follows the visible lines when tool output is folded, expanded,
or streamed, including wrapped headings and output.

**Settings → Notifications…** sets desktop notifications and sounds (WAV, MP3 or
OGG files, played with ffplay, mpv, pw-play or paplay, whichever is installed).
A finished or failed turn notifies only when it took at least a set time (5
minutes by default). An agent that waits for an approval or an answer is always
announced, after a short delay (30 seconds by default) so that answering at once
stays quiet, and optionally again every few minutes until it is answered, since
its work stops until then. The speaker button in the status row turns red while a sound or voice is
playing (including voice preparation); click it to stop the current audio without
changing the mute setting. When idle, it mutes notification sounds. The
audio chooser remembers its last browsed directory across all three notification
sound fields and application restarts, including when the chooser is cancelled.
When a sound field already contains a file, Browse opens its directory and selects
and scrolls to that file; an empty field starts in the last browsed directory.
The audio chooser always shows the sortable duration column (`mm:ss.t`), without
a checkbox. Image metadata is not enabled in this audio
chooser. Pasted absolute or relative paths are handled by QxFileDialog, relative
to its displayed directory. The
notification settings dialog also has **Stop playback** for cancelling **Play**
or **Try** previews while the dialog is open. Starting another sound or voice
replaces the current playback, and closing the application stops it.


Announcements can be spoken instead, for example "Codex finished: fix the build"
or "Claude is waiting for you: …", so it is clear which chat needs attention.
agentin looks for Piper (on `PATH`, in `~/.venvs/piper/bin` or `~/.local/bin`)
with a voice (a `.onnx` model with its `.onnx.json`, for example in `~/piper` or
`~/.local/share/piper`), or for espeak-ng. The speech is generated when needed,
so the repository contains no audio files. A Polish Piper voice (its file name
starts with `pl`, such as `pl_PL-gosia-medium` from
[piper-voices](https://huggingface.co/rhasspy/piper-voices)) speaks Polish
sentences; other voices speak English. The voice and its pace are chosen and tried
in **Settings → Notifications…**; a slowness above 1 (1.3 by default) speaks
slower and usually clearer. Without a voice program the sound files play.

Incoming Codex messages are processed in short batches, and streamed chat view
updates are coalesced, allowing the interface to handle input between batches
when tool output arrives in bursts.

Codex is launched with structured questions enabled in its default mode, using a
process-local configuration override. Blocking questions end with their turn;
nonblocking questions remain visible and answerable until answered or resolved by
the server. A question written only as ordinary chat text does not create a form.

**View → Reasoning** toggles a read-only panel beneath the conversation tree.
It is hidden by default, and the visibility preference is saved. Drag the divider
between the tree and the panel to adjust their heights. The panel follows the
selected conversation and keeps background conversations separate. It displays
reasoning summaries or raw text supplied by Codex, preferring raw text when
available, and thinking blocks supplied by Claude/GLM. Text updates during a turn
and is also read from loaded history. It is separate from the main transcript;
Codex turns explicitly request detailed reasoning summaries; models that do not
expose text can still report reasoning-token usage while leaving the panel empty. Only text shared by
the provider is shown, not hidden reasoning or encrypted/redacted blocks.

**View → Provider limits** shows account limits for the selected conversation's
provider. Switching chats switches the panel; unsupported providers and unreported
windows do not add placeholder rows. It is visible by default, and visibility is
saved in settings. Weekly limits appear before five-hour limits, including separate
model-family windows when reported. Rows show the provider, window, remaining
percentage, local reset time and pacing. Missing fields within a reported window
are marked as not reported. Drag the divider below the panel to resize it, even
to less than half a row. Its default height is about one and a half rows; the chosen
height is saved. Column headings are hidden to keep the panel compact.

Green means the consumed percentage is strictly below the fraction of the window
that has elapsed. Yellow means it is equal or greater: for example, after one day
of a seven-day window, consuming at least 1/7 is yellow. Each row shows **Balance
at**: the first second when the elapsed fraction exceeds the consumed percentage.
On green rows that moment has passed; on yellow rows it is ahead, assuming no
further account use, and with several constrained windows you wait until the
latest balance. Red means 100% is used or the provider reports rejection; its
balance is the end of the window, the same as its reset time. The panel recalculates pacing each second while visible.
After a reported reset passes, it shows **Awaiting updated limits**, without
assuming that a fresh quota is available.

Which windows exist depends on the plan. Codex reads them from the App Server
when its conversation is selected and the panel is visible (`account/rateLimits/read`),
and updates the snapshot as the server reports changes. Explicit reads are deferred
while another provider is selected or the panel is hidden. Claude reports limits through SDK events while it answers. The
bridge reads both typed limit fields and per-window `unifiedWindows` snapshots
preserved in the SDK's raw data, when available. This also supports events where
the top-level percentage is omitted during normal use. The panel uses the last
reported snapshot and does not promise live quota polling for Claude.
GLM has the same event handling through its bridge but may report no quota data.
Gemini and Antigravity currently do not report account limits.

Select a chat in the conversation tree to show a read-only preview of its
latest messages in the preview tab, whose title is shown in italics. Selecting
another chat replaces the preview; a chat that is already open in a tab is shown
there instead. Only the last 20 entries are loaded at first; use **Show
earlier messages** to load older ones. Codex history is paged through the App
Server (`thread/items/list`), Claude and GLM history is read through the Claude
Agent SDK, and Gemini history is read from the saved CLI session file.
Antigravity does not expose its history, so no preview is available. Messages
cannot be sent to a previewed chat; `help`, `new`, `clear`, and `quit` still work.

Double-click a chat to keep its tab and continue it. Its history stays visible
and new messages are added below it. The message field hint updates immediately
when the preview becomes live. A chat that is open in another tool stays read-only and the
chat header shows it as locked:

- Codex: the App Server decides; if `thread/resume` fails, its error is shown.
- Claude and GLM: Claude Code registers running sessions in
  `~/.claude/sessions/<pid>.json` (or under `CLAUDE_CONFIG_DIR`); a live process
  there that this application did not start locks the session.
- Gemini and Antigravity: on Linux, another process whose command line contains
  the session ID (for example `gemini --resume <id>`) locks the session. This is
  a heuristic, because these CLIs do not publish which sessions are open.

Type `help` (or `/help`) in the command field to see the available commands.
In a Codex tab, it shows the complete output of `codex app-server --help`
and explains that the client uses direct stdio mode. CLI subcommands shown in
that output are reference information and are not chat messages.
In a Claude or GLM tab, it explains the SDK workflow
and shows `claude --help` when the Claude CLI is installed. CLI options are shown for reference; this window
communicates through the SDK. Claude Code's interactive slash commands are
listed in the [Claude Code commands reference](https://code.claude.com/docs/en/commands).
See [Z.AI's Claude Code setup](https://docs.z.ai/devpack/tool/claude) for GLM.

In a Gemini tab, `help` shows `gemini --help`. If [folder trust](https://geminicli.com/docs/cli/trusted-folders/)
is enabled, trust the working folder in Gemini CLI before starting a headless
conversation. Headless tool approvals follow Gemini CLI's configured policy;
they are not shown as Qt approval dialogs.

In an Antigravity tab, `help` shows `agy --help`. The selected working
directory is passed as the CLI process directory. Headless mode uses the CLI's cached authentication and its configured permission
policy; approval prompts are not shown as Qt dialogs.

The client also supports `new`, `clear` (clears the log), `stop`, and `quit`.

The message field grows with its text. Shift+Enter always starts a new line and
Ctrl+Enter always sends. By default Enter sends a short message of one line (up
to 60 characters) and starts a new line in a longer one, so a message you are
still writing is not sent by accident. Once you type a line break yourself,
Enter keeps adding lines. A recalled message or pasted text is sent with Enter
as long as you do not change it. **Settings → Options…** sets the length of a
short message (0 never sends typed text with Enter) or makes Enter always send
or always start a new line; the choices are saved as `enterKey` and
`enterSendsUpTo` in `settings.json`. Undo and redo work in the field, also for a
recalled message; after sending, Ctrl+Z brings the sent message back for
editing, unless **Settings → Options…** turns that off (`undoAfterSend`). A badge next to **Send** shows what Enter
does now: a green arrow when it sends and a gray return sign when it starts a new
line; its tooltip names the key for the other action.

Up and Down move between lines and, on the first or last line, recall the
previous or next message you wrote in the current tab's conversation; Page Up
and Page Down always recall messages. After the newest message, the text you
typed before browsing comes back. The messages come from the conversation's
loaded history and from what you sent in the tab, so no separate history is
stored; older messages become available after **Show earlier messages**.

Two agents never work on turns in the same directory, or in a directory and one
of its subdirectories, at the same time. A turn holds its working directory and
every other directory the agent may write to (Codex's writable roots), so no
other agent works in them either; the shared `/tmp` is not held. A chat holds its directory only from
sending a message until the agent hands control back, so between turns another
chat in the same directory can be used. A message that would conflict waits: the
chat header and the log say which chat holds the directory, and the message is
sent when that turn ends. **Stop** (or `stop`) gives up waiting. The running turns
of all agentin windows are kept in `turn-locks.json` in the application data
directory, and Claude Code sessions outside agentin that are busy on a turn
count as well, as do Codex turns outside agentin: Codex (usually its App
Server daemon, which the Codex CLI and desktop app use) keeps each session's
`~/.codex/sessions/.../rollout-*.jsonl` open, and a session whose last turn has
started but not completed holds its directory, unless the turn runs in Codex's
read-only sandbox. Gemini CLI and Antigravity CLI running outside agentin publish
no turn state, so such a process holds its directory for as long as it runs; the
waiting chat names it with its PID, and closing it lets the chat continue.
Processes started in `/` or the home directory are not counted, because they
would cover every project. Turns in a read-only Codex chat neither take nor wait for the
directory. Chats send one message per turn; messages written in the meantime
wait in the tab.

Any other text is sent to the chat in the current tab when it is not a read-only
preview. Messages entered while a response is in progress are queued. Requests to
approve an action or answer a question appear in a
panel below the chat of the tab that asked, so other tabs stay usable; a
background tab with a waiting request is marked. An approval can be given once
or, for Codex and for Claude when Claude Code suggests a rule, for the rest of
the session. When the agent proposes a lasting rule, **Always allow** adds it, so
similar actions run without asking from now on: Codex allows commands with the
proposed prefix (for example `git status`), and Claude saves its suggested rule
where Claude Code proposes it, such as the project's local settings; the panel
shows the rule first. Declining lets the agent continue, and **Decline and stop**
also ends its turn. **Settings → Approvals…** lists the lasting rules (Codex's
`~/.codex/rules/*.rules`, and Claude Code's `permissions.allow` in the user
settings and in the `.claude` settings of the directories of open chats and
recent directories) and the approvals given for the session in open chats, and
removes the selected ones. Codex may keep using a removed rule until its App
Server restarts and cannot withdraw native App Server session approvals; for a Claude or GLM
chat, withdrawing reconnects it and withdraws all of its session approvals.

Claude and GLM connections explicitly use `default` permission mode, with
approval questions for actions that are not already allowed by permission rules.
The mode is reapplied after every connection, including resumed conversations;
**Read-only** uses `plan`, and switching it off restores `default`. Tool approval
suggestions cannot change the permission mode to `auto`, `acceptEdits`, or
`bypassPermissions`. Existing allow rules still apply in `default` mode.

For Codex command approval requests, **Trust git add for this chat** and
**Trust git commit for this chat** remember the command family in agentin's
memory, without writing a lasting Codex rule. Matching simple commands receive
`accept` (allow once), even when their arguments change. Normal shell `-c`/`-lc`
wrappers and Git directory options are recognized; compound commands, shell
expansions, configuration overrides, and unknown syntax are never automatically
trusted. **Settings → Approvals…** lists these rules as managed by agentin;
removing one immediately restores questions, including during a running turn.
Trust belongs to one conversation in one chat and ends when changing the
conversation or exiting the application.
Network approval requests require their own decision.

Incoming Codex command approval requests for `git push`, `sudo`, `doas`, `su`,
and system package changes through apt/apt-get, dnf/dnf5, yum, zypper, pacman,
apk, pkg, or brew are automatically declined. On Windows, runas and recognized
winget/choco/scoop changes and PowerShell installation commands are also declined.
These checks apply to recognized commands in approval requests, not commands
executed by the agent without requesting approval; unknown shell syntax still
requires a human decision. Quoted argument text does not count as a command.

Question options are listed with numbers and descriptions: choose
them in the panel (several where the question allows it) or type their numbers
in the message field, for example `2` or `1, 3`. Where the agent accepts an
answer in your own words, type it in the message field with the usual Enter
behavior. Secrets are typed in a hidden field in the panel. Requests of a turn
that ends are dropped.

## Test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

If the Codex App Server exits while it was working, agentin starts it again
after a growing delay (1 s, 2 s, 4 s and so on, at most 30 s) and each Codex tab
reopens its thread; the turn that was running is reported as failed. After more
than five exits within five minutes it is left stopped.

The Codex client uses JSONL over stdin/stdout (`codex app-server --stdio`). See the
[OpenAI Docs for Codex App Server](https://learn.chatgpt.com/docs/app-server)
for the protocol. The Claude bridge also uses JSONL over stdin/stdout and calls
[`ClaudeSDKClient`](https://code.claude.com/docs/en/agent-sdk/python) in the
Python SDK.
