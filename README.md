# agentin

A Qt 6 desktop client for Codex, Claude, GLM, Gemini, and Antigravity. Start a
chat with an agent, enter a message, and read streamed responses in the chat
pane; application messages appear in the log pane at the bottom. Codex uses the local `codex` executable and its existing sign-in and
configuration. Claude and GLM run through a Python JSONL bridge to the Claude
Agent SDK. Gemini and Antigravity run through their respective CLIs in headless mode.

Codex and Claude are always available. GLM, Gemini and Antigravity are
experimental and hidden by default: they appear in the conversation tree, the
new chat dialog and the default model options only after **Show experimental
agents** is checked in **Settings → Options…**. While they are hidden, their
saved tabs are not reopened at startup.

## Build and run

Requires Qt 6 Widgets, CMake 3.24 or newer, a C++17 compiler, libgit2 (`libgit2-dev`),
and an installed `codex` CLI for Codex. The `qt-extra` library (the chat tabs and the
directory chooser) and the Kate syntax reader of `qcodeedit` (the colours of the
changes window) need no installing: an installed qt-extra 2.1 or qcodeedit-kate
1.4 or newer is used, and otherwise CMake fetches them from GitHub when it
configures the build and builds them into agentin, as it does with backward-cpp;
that first configuration needs a network connection. Claude requires Python 3, `claude-agent-sdk`, and a configured API
key. GLM uses the same Python SDK with a Z.AI API key. Gemini requires an
installed and authenticated Gemini CLI. Antigravity requires an installed and
authenticated `agy` CLI. The optional test also requires Qt 6
Test and Python 3.

The terminal below the chat uses qtermwidget for Qt 6, which links utf8proc.
Their development packages are named differently by each distribution:

```sh
# Debian, Ubuntu
sudo apt install libqtermwidget6-2-dev libutf8proc-dev
# Fedora
sudo dnf install qtermwidget-devel utf8proc-devel
# Arch Linux
sudo pacman -S qtermwidget libutf8proc
```

On Ubuntu, `libqtermwidget6-2-dev` does not pull in `libutf8proc-dev`, so the
link fails on a missing `libutf8proc.so` until it is installed. Elsewhere, look
for the Qt 6 build of qtermwidget (version 2 or newer), or build it from
[lxqt/qtermwidget](https://github.com/lxqt/qtermwidget) with
[lxqt-build-tools](https://github.com/lxqt/lxqt-build-tools).

```sh
cmake -S . -B build
cmake --build build
./build/agentin -C /path/to/project
```

The first CMake configuration downloads
[backward-cpp](https://github.com/bombela/backward-cpp), which writes a stack
trace with files and line numbers when the application crashes. After the
command line is read, diagnostics go to `agentin-crash.log` in the application
data directory (for example `~/.local/share/agentin/agentin-crash.log`) instead
of the terminal; each start adds a header with the version and time, and a
crash appends its stack trace.

`cmake --install build` installs into `~/.local` unless `CMAKE_INSTALL_PREFIX` or
`--prefix` chooses another location. It installs
`bin/agentin` and the Claude bridge as `share/agentin/claude_bridge.py`. The
executable uses the bridge next to itself, as in the build directory, and
otherwise the one in `../share/agentin`; `--claude-bridge` overrides both.
It also installs `share/applications/agentin.desktop` and the icon
`share/icons/hicolor/scalable/apps/agentin.svg`. The window has the icon on
its own, but on Wayland the desktop shows the icon of the desktop file, so a
build that is run without installing it shows a generic icon in Alt+Tab.

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

When a Claude or GLM request fails, Claude Code retries it up to ten times with
growing delays, which can take several minutes. Each retry is written to the log
and shown in the chat header, for example `GLM: API error 429 (rate_limit),
retry 3 of 10 in 2.0 s`; the server's own message appears in the chat when the
retries are used up. A GLM Coding Plan that has expired is reported as 429.

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

The left pane groups chats by agent and working directory. The buttons above it,
also in **View → Conversation tree**, change the layout and order, and the
choice is saved:

- **By agent** / **By directory**: agent rows with directories and chats, or
  the full directory paths as the top rows, each chat showing its agent as
  `Claude: title`. Grouping by directory discovers the chats of all agents
  shown, as expanding each agent would.
- **Sort**: chats newest created first or most recently changed first (the
  default; a chat without a change time sorts by its creation), and
  directories by path or by their first chat in that order (the default).
  A turn that ends in this application counts as a change at once.
- **1 2 3**: show only the top rows, one level more, or everything; by
  directory there are two levels. Directories you open or close afterwards
  stay so until the next choice.

The selected agent, directory or chat stays selected when these choices
rebuild the tree; a row in a closed branch is represented by the closed row
that contains it.

Expanding Codex
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
A right click on a chat or a directory offers **Copy**, which puts the text of its
tooltip on the clipboard: a chat's preview, details and ID, or a directory's path.
A Codex, Claude or GLM chat also offers **Rename…**, which asks for a new name, and
**Suggest a name**, which asks the chat's own agent for one name with its cheapest
model (Codex's nano or mini model, Claude's Haiku, GLM's flash model), from the user's
first messages and the beginning of the answers, and offers it in the same dialog for
editing before it is saved. The name is saved where the agent keeps it: Codex stores
it as the thread's name, Claude Code as the session's title, as its `/rename` does, so
the agents' own lists show it too.
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
its **Access** (**Can change files**, the default, or **Read-only**, in which it
reads and plans but changes nothing; the **Read-only** box switches it later),
then enter a directory path or choose one with **Browse…**, which also lists
recently used directories. The path starts as the directory selected in the
conversation tree, or the directory of the selected chat; otherwise, for
example when an agent row is selected, it is the directory last chosen in this
dialog. The **Create chat**
button is available only when that directory exists. Each chat opens in its own
tab and has one working directory; files under that directory are available
subject to the selected agent's permissions. At startup one Codex tab is ready
in the working directory, and its conversation starts with the first message.
A new chat is called "New chat" in its tab and in the tree until its first
message is sent; then both show the beginning of that message.

When the window closes, the open tabs are saved in `open-tabs.json` in the
application data directory, with their model, effort and read-only mode, and
the next start reopens them and continues the chats that were live; Codex chats
continue once the App Server is connected. The preview tab is not saved. When a
directory is given with `-C`, its new chat opens next to the reopened tabs.

Chats in different tabs run independently, also several chats with the same
agent. All Codex chats share one App Server process. A tab whose agent is
responding shows a busy marker, and a background tab that receives output is
marked until you switch to it. Closing a tab while its turn runs names the chat
and asks whether to stop the turn and close the tab. Quitting the application
(the window's close button, Alt+F4 or `quit`) while turns are running says how
many run, lists them and asks whether to stop them and quit; otherwise it closes
at once. In both questions Cancel is the default, so Enter keeps everything
running. While a changes window is open, the
close button and Alt+F4 close the changes windows instead, and `quit` still quits. Ctrl+Tab switches tabs in most recently used
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
already, for example because Codex does not trust its directory; a chat
started from the New chat dialog with **Can change files** is not read-only,
whether Codex trusts the directory or not. In Claude and
GLM chats the box switches the session to Claude Code's plan mode, and Gemini
and Antigravity turns get `--approval-mode plan` and `--mode plan`: the agent
reads and plans but is told not to change files. Only Codex enforces this with a
sandbox, so only read-only Codex turns skip the directory lock.

Read-only never changes files silently, but it is not a promise that nothing
changes: Codex may still ask to run a command outside its sandbox, and Claude may
end its plan by asking to leave plan mode; files change only if you agree. agentin's
Allow rules do not apply to read-only chats, so such a request always reaches you.
It is useful:

- **For questions and reviews**, such as "why does this test fail?", "explain this
  module" or "review my last commit", when an answer must not come with an
  unrequested "fix".
- **For planning**, when the agent should first describe the changes it would
  make, so that you can correct the plan before switching Read-only off and
  letting it work.
- **Beside another chat in the same directory**, with Codex only: a read-only
  Codex turn neither takes nor waits for the directory, so it can study the
  project while another chat changes it. Claude, GLM, Gemini and Antigravity
  only follow an instruction, so their read-only turns still wait for the
  directory.
- **In a directory you do not want touched**, such as a checkout of someone
  else's project or a release branch, where Codex's sandbox enforces it unless you
  approve a request to leave it.

For ordinary work, where the agent is meant to change files, leave it off.

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

During a running Codex, Claude or GLM turn, **Steer** (the orange `!` button, placed before Send) sends the message field's text directly to
that turn, while **Send** continues to queue a message for the next turn. Sending
from the keyboard (Enter, or Ctrl+Enter) steers whenever Steer is available, so
the Send button is the way to queue; `help`, `stop` and the other commands still
work as commands. Steer is
available once the turn ID is known, except during manual compaction, stopping,
or a pending approval/question. Accepted messages appear as `You (steer)` and are
included in message history. If the turn finishes before the server accepts the
message, the error is shown and the text is restored to an empty message field
when its chat is still selected; it is also kept in the transcript. Steering does
not interrupt tools or guarantee an immediate answer. Claude and GLM take a steering message at the
model's next step, for example after the running tool; one that arrives after
the last step is answered right after the turn, as part of the same turn in the
chat. Claude Code does not report whether a message was used, so `You (steer)`
means that it was passed to the session.

In a Codex, Claude or GLM chat, **Suggest** (the light bulb, Ctrl+Space, shown while the
message field is empty or holds only spaces and line breaks) asks for up to three short ideas
for your next message, based on the latest exchange (your last message and the
agent's text answer, without tool output) and, in short, the four before it. They are asked outside the conversation,
in the language you write in, from the cheapest model: Haiku for Claude,
`glm-4.7-flash` for GLM (`GLM_SUGGEST_MODEL` changes it), and the smallest model
Codex lists in an ephemeral, read-only thread. Several ideas are offered in a list
under the message field; the chosen one is put in the field selected, so typing
replaces it and Enter sends it. Pressing Suggest again for the same exchange shows
the same ideas without asking the model.

When the chat's directory is in a Git work tree, the chat header also sums up the
files the latest turn changed, for example `Changes: 7 files, +120 −34`, counting
added and removed lines. It is compared with the state when the turn started,
updated every few seconds while the turn runs and at its end, and kept until the
next turn starts. Changes committed during the turn count too; files ignored by
Git and untracked files the turn did not touch do not, and a new file counts its
lines as added. Directories outside Git are not tracked.

The summary, in the header's second row with added lines in green and removed
ones in red, is a link; it and **Conversations → Changes of the latest turn…**
open the chat's changes window, which follows the turn live: the list and the
diff stay where they are scrolled to, and a file that has just changed lights up
for a second. With **Group new files**, as JetBrains
IDEs show them, new files are listed below the changed ones. It lists each file
with its status (`m` modified, `n` new, `d` deleted, `r` renamed in the index,
`b` binary) and its added and removed lines, a new file's lines counted as added.
Clicking **File**, **+** or **−** sorts the list by it, the largest numbers first;
equal counts go by the other count, then by path. The window shows
the diff of the selected file, unified or side by side, with line numbers and three lines of context;
longer unchanged stretches are folded into one line that opens with a click, and
the arrow buttons (or Alt+Up and Alt+Down) move between changes. The code is
coloured by its syntax with the Kate syntax definitions that qcodeedit keeps in
`~/.local/share/qcodeedit/kate-<version>/syntax` (shared with other editors built
on qcodeedit, such as qceditor), chosen by the file's name; a dark window uses the
Breeze Dark theme from the `themes` directory beside them. Without the
definitions, or for a file of more than 50 000 lines, the diff is shown without
syntax colours. agentin does not download the definitions. The list starts
from the latest turn; it can also show the changes since the chat's first turn,
or against `HEAD` as Git sees them, including changes made before the chat. A new
file shows its whole content with line numbers and no diff colors; binary, too
large and largely rewritten files get a note instead of a diff. Double-click or
Enter opens a file; the context menu also opens its folder or copies its path.

**Settings → Open files with…** decides which program opens a changed file, by
file name pattern; the first matching rule wins. The rules start with the
system's choice for each group of file types (C and C++ with CMake, Rust with
`Cargo.toml`, Python, Markdown, configuration files, and everything else), and
the dialog lists the IDEs and editors found on the computer: IDEs installed by
JetBrains Toolbox and JetBrains IDEs, VS Code, Kate, KWrite and gedit in `PATH`.
Any of them can be chosen for a rule, or a command typed, using `%f` for the file
and `%l` for its first changed line. A JetBrains IDE opens the file at that line
in the instance that is already running instead of starting another.

The chat header shows elapsed operation time as `mm:ss`: **Task** while an agent
works and **Compact** during manual or automatic Codex compaction. The counter
stops at the final duration when the operation ends and follows the selected tab.
The application log records each task and compaction duration, including operations
that fail or are interrupted. Waiting for a directory lock is not timed.

The **Compact** button above a Codex, Claude or GLM chat manually compacts its
context when the chat is live and idle: Codex compacts its thread, and Claude Code
runs its `/compact` command. The read-only field beside it shows the tokens the
context holds, with apostrophes grouping thousands (for example `100'000` or
`1'000'000`): Codex reports them with its usage, and a Claude or GLM chat after each
turn, after compaction and when a conversation is resumed; its tooltip names the
model's context window. An em dash means no report has arrived yet. Codex's
compaction can be interrupted with **Stop**; a compaction does not count as a
normal prompt completion, and a message sent meanwhile waits for it.

After Codex compacts its context, a marker appears in the chat. When the running
turn ends, the chat reloads its latest stored history from App Server before
starting another turn. Messages queued before or during the refresh stay visible
without being appended a second time. Older messages remain available through
**Show earlier messages**. A refresh error leaves the existing transcript intact.

In a Codex chat, **Fast** after the **Compact** token counter requests faster model responses
with higher limit usage, where supported. It applies from the next turn and is
kept separately for each chat. Each conversation remembers its latest choice,
also after restarting agentin, for restored tabs and chats continued from the
tree; it is saved in agentin's settings, not in Codex configuration.

Your messages in the chat (from a line starting with `You: ` or `You (…): `, up to
the agent's answer or the next bracketed entry) have a yellow background across
the view, darker on a dark color scheme, so they stand out from the answers.

The chat and the reasoning panel follow new text only while you are at their
end. Scrolled up, they stay where you read, and a button with a down arrow
appears in the corner when new text arrives below; it scrolls to the end, as
does scrolling down by hand, and the view follows again.

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
Codex can run several commands at once. Each output then goes under the heading
of its own command, repeated with `(continued)` when another command wrote in
between, and the final status names the command, such as
`[shell rg -n pattern: completed]`.
**View → Tool calls** hides the tools altogether: their headings, output, final
status lines such as `[shell: completed]` and the blank lines after them, so the
chat shows only your messages and the answers. A thin dashed line marks where
tool calls were. The choice is saved in the settings.

Copying the chat (Ctrl+C) always takes the tool calls with their output, folded
or not. The chat's context menu offers four ways to copy the selection, or the
whole chat when nothing is selected: **Copy with tools expanded** (everything),
**Copy with tools folded** (the tool calls' headings and status lines without
their output), **Copy without tools** (only your messages and the answers) and
**Copy tools only** (the tool calls with their output, separated by empty lines).

**Settings → Notifications…** sets desktop notifications and sounds (WAV, MP3 or
OGG files, played with ffplay, mpv, pw-play or paplay, whichever is installed).
A finished or failed turn notifies only when it took at least a set time (5
minutes by default, set in tenths of a minute). An agent that waits for an approval or an answer is always
announced, after a short delay (30 seconds by default) so that answering at once
stays quiet, and optionally again every few minutes until it is answered, since
its work stops until then. Approval requests can be left unannounced, while
finished turns and the agent's questions still are. The speaker button in the status row turns red while a sound or voice is
playing (including voice preparation); click it to stop the current audio without
changing the mute setting. When idle, it mutes notification sounds. The
audio chooser remembers its last browsed directory across all notification
sound fields and application restarts, including when the chooser is cancelled.
When a sound field already contains a file, Browse opens its directory and selects
and scrolls to that file; an empty field starts in the last browsed directory.
The audio chooser always shows the sortable duration column (`mm:ss.t`), without
a checkbox. Image metadata is not enabled in this audio
chooser. Pasted absolute or relative paths are handled by QxFileDialog, relative
to its displayed directory. The
notification settings dialog also has **Stop playback** for cancelling **Play**
previews while the dialog is open. Starting another sound or voice
replaces the current playback, and closing the application stops it.

Each sound can also be one of the short built-in sounds, generated when played
(a click, a double click, a rising or a falling tone), or speech. A Codex chat's
compaction plays its own sounds when it starts and when it ends, at every
compaction, automatic or started with **Compact**; they do not interrupt an
announcement that is playing, and muting silences them too. Claude and GLM chats
play them for a compaction started with **Compact**; Claude Code's automatic
compaction is not reported to agentin. Where the desktop's sound themes are installed, as on Ubuntu,
unset sounds default to them: freedesktop's `bell.oga` when compaction starts,
Yaru's `complete.oga` when it ends, and freedesktop's `screen-capture.oga` for an
agent that waits. Elsewhere compaction uses the rising and falling tones.

Speech names the agent and the chat, for example "Codex finished: fix the build"
or "Claude is waiting for you: …", so it is clear which chat needs attention.
Each sound list offers speech by every Piper voice of the chosen language, such
as "Speech, gosia", with the sentence of that event as an example: for the
current chat, or only its beginning when no chat is open; **Play** says it. Finished
and failed turns are spoken by default. The speech language and pace are set
once, below the sounds; a slowness above 1 (1.3 by default) speaks slower and
usually clearer. agentin looks for Piper (on `PATH`, in `~/.venvs/piper/bin` or
`~/.local/bin`) with its voices (a `.onnx` model with its `.onnx.json`, for example
in `~/piper` or `~/.local/share/piper`; the file name, such as
`pl_PL-gosia-medium` from [piper-voices](https://huggingface.co/rhasspy/piper-voices),
gives the language and the voice's name). espeak-ng's mechanical voice is offered
only for a language without a Piper voice. Polish is spoken in Polish sentences,
other languages in English ones. The speech is generated when needed, so the
repository contains no audio files; when no voice can speak, a double click plays
instead. Sounds and speech start after a short lead-in, so that an audio output
waking up does not cut off a short sound or the agent's name. The lead-in is noise
far too quiet to hear (about −74 dB) rather than silence, because some monitors and
TVs keep their speakers muted until the signal is more than digital silence.
Built-in sounds and speech begin with it, and a sound file plays after a separate
lead-in, with any player and in any format. It lasts 0.4 s, or 1.5 s when `pactl`
reports the default output as suspended, since an idle output, HDMI above all,
can take over a second to wake up. **Settings → Notifications…** shows the lead-in
for the output as it is when the dialog opens, and then the one each **Play** used.

When short sounds are still cut off, or the delay is unwelcome, it can be worth
keeping the output awake. With PipeWire, a WirePlumber rule does that, for example
for HDMI outputs in `~/.config/wireplumber/wireplumber.conf.d/51-no-suspend.conf`:

```
monitor.alsa.rules = [
  {
    matches = [ { node.name = "~alsa_output.*hdmi.*" } ]
    actions = { update-props = { session.suspend-timeout-seconds = 0 } }
  }
]
```

followed by `systemctl --user restart wireplumber`; agentin then finds the output
awake and waits only 0.4 s. A spoken announcement names the chat by the first words of its title,
about 32 characters. Answering the approval or question it announces, or a new
request of the same chat, stops it at once.

Incoming Codex messages are processed in short batches, and streamed chat view
updates are coalesced, allowing the interface to handle input between batches
when tool output arrives in bursts.

Codex is launched with structured questions enabled in its default mode, using a
process-local configuration override. Blocking questions end with their turn;
nonblocking questions remain visible and answerable until answered or resolved by
the server. A question written only as ordinary chat text does not create a form.

The bottom of the window, as in CLion, holds the **Log** and a **Terminal**,
switched by their tabs below them. The terminal is a shell (`$SHELL`, or
`/bin/bash`) in the current chat's directory, for example for `git status` or
`git push` without leaving agentin. Each directory has its own shell, which two
chats in the same directory share; switching chats shows the shell of the chat's
directory, and every shell keeps running while agentin runs. **View → Terminal**
(Alt+F12) opens it and gives it the focus; pressed in the terminal, it goes back
to the message field. While the terminal has the focus, every key goes to the
shell, also Ctrl+W, Ctrl+Tab and the other keys agentin uses, except Alt+F12. A
shell that ends, for example with `exit`, leaves a button that starts a new one.
As in other terminals, Ctrl+Shift+C copies the selection and Ctrl+Shift+V or
Shift+Insert pastes, while Ctrl+C and Ctrl+V stay with the shell. A right click
in the terminal opens its context menu, with **Copy**, **Paste**, **Select Line**
(the line under the mouse with its wrapped continuation, as a triple click
selects it), **Select All** (including the lines scrolled above the screen) and
**Clear**. Text is selected with the mouse; the line being typed belongs to the
shell, so it is cut with the shell's own keys, such as Ctrl+U and Ctrl+K in bash.

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
Gemini and Antigravity currently do not report account limits. Each change of a
provider's reported limits is also written to the log, for example
`[Codex limits: Week 13% used, resets Wed 7 Oct 16:57]`, so you can follow how
fast they are used; Codex reports whole percentages.

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
policy; approval prompts are not shown as Qt dialogs. The CLI cannot ask for
permission in this mode, so it denies actions that need one, such as running a
shell command, and ends the turn without an answer. The chat then reports the
turn as failed and names the denied actions; allow them under
`permissions.allow` in `~/.gemini/antigravity-cli/settings.json`.

The client also supports `new`, `clear` (clears the log), `stop`, and `quit`.

The message field grows with its text. Shift+Enter always starts a new line and
Ctrl+Enter always sends. By default Enter sends a short message of one line (up
to 60 characters) and starts a new line in a longer one, so a message you are
still writing is not sent by accident. Once you type a line break yourself,
Enter keeps adding lines. A recalled message, or text pasted into an empty field
or over its whole content, is sent with Enter as long as you do not change it. Line breaks, spaces and tabs at the start and end of
pasted text are dropped, so a stray trailing line break does not turn Enter into
a new line. **Settings → Options…** sets the length of a
short message (0 never sends typed text with Enter) or makes Enter always send
or always start a new line; the choices are saved as `enterKey` and
`enterSendsUpTo` in `settings.json`. Undo and redo work in the field, also for a
recalled message; after sending, Ctrl+Z brings the sent message back for
editing, unless **Settings → Options…** turns that off (`undoAfterSend`). The **Send** button's icon shows what Enter
does now: a green arrow when it sends and a gray return sign when it starts a new
line; its tooltip names the key for the other action. **Stop** is the red square
beside it.

Up and Down move between lines and, on the first or last line, recall the
previous or next message you wrote in the current tab's conversation; Page Up
and Page Down always recall messages. A recalled message of several lines that
you have not edited, clicked or moved through with Left, Right, Home or End is
skipped as a whole, so Up and Down go straight to the previous or next message. After the newest message, the text you
typed before browsing comes back. The messages come from the conversation's
loaded history and from what you sent in the tab, so no separate history is
stored; older messages become available after **Show earlier messages**. A
message you wrote several times is recalled once, at the place of its latest
use.

Two agents never work on turns in the same directory, or in a directory and one
of its subdirectories, at the same time. A turn holds its working directory and
every other directory the agent may write to (Codex's writable roots, and for
Claude and GLM the directories added to the chat, allowed for the session or
listed in `permissions.additionalDirectories` of the Claude Code settings, which
the bridge reports as they change), so no other agent works in them either; the
shared `/tmp` is not held. A chat holds its directory only from
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
background tab with a waiting request is marked. A long request, such as a whole
script, scrolls in a field of at most about a third of the window, so that the
buttons stay visible, and **Show all…** opens it in a window of its own. A Codex
request to change files lists the files with what happens to each (add, change,
delete or move) and their diffs, which Codex sends before the request. An approval can be given once
or, for Codex and for Claude when Claude Code suggests a rule, for the rest of
the session. When the agent proposes a lasting rule, **Always allow** adds it, so
similar actions run without asking from now on: Codex allows commands with the
proposed prefix (for example `git status`), and Claude saves its suggested rule
where Claude Code proposes it, such as the project's local settings; the panel
shows the rule first. Declining lets the agent continue, and **Decline and stop**
also ends its turn.

Claude and GLM connections explicitly use `default` permission mode, with
approval questions for actions that are not already allowed by permission rules.
The mode is reapplied after every connection, including resumed conversations;
**Read-only** uses `plan`, and switching it off restores `default`. Tool approval
suggestions cannot change the permission mode to `auto`, `acceptEdits`, or
`bypassPermissions`. Existing allow rules still apply in `default` mode.

Claude and GLM run with Claude Code's own system prompt, with the added advice
to change files with its editing tools. Their edits (Edit, MultiEdit, Write and
NotebookEdit) of files in the chat's directory, in a directory allowed for the
session, in a directory listed in `permissions.additionalDirectories` of the
Claude Code settings the session reads, or in the temporary directory (`/tmp`,
or `TMPDIR`) are approved without asking. An edit elsewhere asks, and its panel
names the file, says that it is outside the chat's directories, for example in
another project, and shows the new content or the replaced text instead of the
tool's raw input. These directories are the *writable directories* of the chat; shell commands are judged by agentin's
rules, described next. The reading tools (Read, Grep, Glob and NotebookRead)
run without a question anywhere, also outside the project, except on a file of
the **Secret files** list (see below), about which agentin asks; Glob only lists
names, so it never asks. Such a question offers no lasting rule: remove the file
from the list instead if it should be read freely.

**Settings → Approvals…** holds agentin's own rules for shell commands: each a
pattern with a check box and **Allow** (runs without asking), **Ask** (always
asks, whatever chat trust or the agent's own rules say) or **Deny** (declined at
once, with no way to agree). `*` matches any text and `?`
one character, and a trailing ` *` also matches the command alone, so
`git stash *` covers `git stash` and `git stash pop`; without it a pattern
allows no further arguments. Three placeholders match within one argument:
`<int>` digits, `<path>` any one argument, and `<writable>` a path inside the
chat's writable directories, judged from the directory the command runs in. So
`cmake --build build -j<int> *` covers any number of jobs, and
`rm -rf <writable>` lets the agent remove what it likes inside the project and
the temporary directory while other removals still ask. A pattern applies to each
command of a command line, after wrappers such as `env` and `timeout` and after
Git's `-C`, `--git-dir` or `--work-tree` options. The column headers show this
syntax as a tooltip. **Add allow**, **Add ask** and **Add deny** add a line, a double-click
edits its pattern or switches its decision, **Remove** deletes it, an unchecked line stays
listed without effect, a click on a column header sorts the list, and a line's
context menu copies it or sets its decision. On the first start the list holds
`git add *`, `git commit *-m *` (which also covers options before `-m`, such as
`git commit -q -m`), and git's read-only `git status *`, `git log *`, `git diff *`
and `git show *` as Allow, `rm *` and `rmdir *` as Ask, and as Deny the installing and upgrading
of system packages through apt, apt-get, dnf, yum, zypper, pacman, snap, flatpak
and brew, which can be removed, unchecked or allowed. `git push *`, `sudo *`,
`doas *`, `su *`, the removal of system packages through the same tools (such
as `apt *remove *`, `apt *purge *` or `pacman -R*`), and the commands that change
a remote server — `git send-pack`, `git lfs push`, `git svn dcommit`,
`docker push` and `podman push`, and the
writing commands of `gh`, `glab` and `hub` (creating, merging and closing pull
requests, releases, repositories and issues, running workflows, setting
secrets, and `gh api` with a writing request) — are listed as Deny at every
start with a checked box that cannot be changed, and they apply whatever the
settings say.

Below the list, **Ask when a Claude or GLM shell command may run longer than …**
(off at first, 10 minutes when turned on) makes a line ask, whatever the rules
say, when its tool may run it longer than the limit. Claude Code gives a command
2 minutes unless it asks for more, at most 10 unless `BASH_MAX_TIMEOUT_MS` raises
that, so a lower limit catches long builds; a command run in the background has
no limit and always asks while the check is on. Codex does not say how long a
command may run, so the check does not apply to it.

The dialog has three more pages, each an editable list with **Add**, **Remove**
and **Restore defaults**: **Secret files** (paths whose contents a command may
not show without a question), **Protected paths** (places where writing by a
shell command asks, empty at first; `.github/workflows` is a typical line), and
**Variables** (names whose setting asks, such as `PATH`, `LD_PRELOAD` or
`GIT_CONFIG*`). A path line uses `~/` for the home directory, `*` and `?` within a
name and `**` for any directories; a line that does not start with `/` or `~/`
matches at any depth, and a directory covers everything in it, so `.env` covers
every `.env` file and `~/.ssh` the whole directory. The **Chat trust** page lists
what each open chat trusts for its session, and the approvals given to an agent
itself with **Allow for this session**; **Remove** withdraws them. An agent's
session approvals go together: a Claude or GLM chat reconnects and resumes its
session, and a Codex chat lets go of its conversation, which Codex closes after
about a minute, and opens it again without them, while the other Codex chats keep
running. A message sent meanwhile waits for the conversation. Nothing changes
before **OK**.

The rules and the lists are kept in `approvals.json` (version 2) in agentin's
data directory, apart from `settings.json`, together with the Codex rules agentin
has seen and kept. A file of version 1, without the lists, loads with the default
lists and is written as version 2 at the next change.
agentin watches the file while it runs: an outside edit takes effect at once,
with the fixed denials added back, and a removed file is written again with the
default rules and lists, so removing it resets them. Earlier versions kept such rules
in `settings.json` (`trustedCommands` or `commandRules`); they are not migrated:
agentin does not start with such a section and shows an error, which can be
copied, naming the file, so that the section or the whole file can be removed.

agentin reads a command line as Bash and judges each command it could run, also
those inside `$(…)`, loops, `if`, subshells, here-documents and `bash -c '…'`
scripts, and behind wrappers such as `env`, `timeout`, `nice` and `xargs`. It
follows `cd`, the variables the line sets and `for` loops over a list, so that
`cd build && make` or `for d in a b; do cmake --build $d; done` are judged by
where they really write. The decision is taken in this order:

1. **Deny** — the line is declined when any command matches a Deny line or a
   fixed denial, or writes into a `.git` directory other than through git
   (`rm -rf .git`, `echo x > .git/hooks/pre-commit`); reading there is allowed.
2. **Ask lines** — a command that matches an Ask line asks, unless an Allow
   line is more specific, judged by the text before the first wildcard:
   `rm -rf build *` beside `rm *` lets that one removal pass.
3. **Allow lines** — a command that matches an Allow line passes, as long as
   it changes and runs things only inside the writable directories: a line
   names a kind of command, not the places it may change, so with `git add *`
   and `git commit *` a commit in another repository, after `cd` to it, still
   asks. Only a line that names the whole command, as Always adds it, also
   covers a place outside; a command whose place is not in its words, such as
   the repository git works in, gets no such line and can only be allowed once.
4. **What the command does**, when no line matches. Passing without a
   question: commands that only read (`cat`, `grep`, `ls`, `sed -n`, `git log`,
   `git diff` and the like, anywhere, and the queries of the package managers:
   `dpkg -l`, `-L` and `-S`, `dpkg-query`, `apt-cache`, `apt list` and `show`,
   `rpm -q`, `pacman -Q`, and the listings of `podman` and `docker`, such as
   `podman images` and `docker ps`), and commands that write, build or run
   programs only inside the writable directories (`touch`, `cp`, `sed -i`,
   `git add`, `cmake`, `cmake --install` with `--prefix`, `make`, `ctest` (also with
   a job count such as `-j$(nproc)`), `ldd` on a program of the project, the C
   and C++ compilers `gcc`, `g++`, `cc`, `c++`, `clang` and `clang++`, also
   versioned as `g++-14`, with flags from `$(pkg-config …)`, a program of the
   project given by its path, a redirection to a file there); `pkg-config`, `nm`,
   `objdump`, `readelf` and `size` only read. A compiler option that loads code
   into it or chooses the programs it runs (`-fplugin`, `-B`, `-specs`,
   `-Xclang`, `@file`) asks. Asking: commands that remove files, use
   the network (`curl`, `git fetch`, `gh pr view`, `podman build` and
   `docker pull`), run or change containers (`podman run`, `docker rmi`), write or run something
   outside the writable directories, install (`make install`), are not known to
   agentin (`python3`, `npm`), or whose arguments cannot be read from the line
   (`cat $FILE` with an unknown value, `eval`, `source`, a script piped to `sh`).
   Git commands that can lose work also ask: `git reset --hard`, `git rebase`,
   `git restore`, `git checkout -- …`, `git clean`, `git tag -f`/`-d`,
   `git branch -D`, `git stash drop`/`clear`, `git commit --amend`,
   `git gc --prune`, `git reflog expire`.

Some things ask whatever the Allow lines say: reading a file of the **Secret
files** list (at first `~/.ssh`, `~/.gnupg`, `~/.zai-key`, `~/.netrc`, `~/.aws`,
`~/.config/gh`, `.env` files, `*.pem`, `id_rsa*`, `id_ed25519*` and
`*credentials*`), writing a path of the **Protected paths** list, a redirection
to a file outside the writable directories, a variable of the **Variables**
list (at first `PATH`, `LD_PRELOAD`, `IFS`, `HOME`, `GIT_EDITOR`,
`GIT_SSH_COMMAND` and others that choose a program) set for a program other than
one of the project — `HOME=/x git status` asks, while
`HOME=/nonexistent build/tests/test_x` does not, as a program of the project runs
without a question anyway and can do whatever the variable would make it do —
a command that an Allow line covers but that is known to write a file or run a
program through an option (`git diff --output=…`), and a line that cannot be
parsed or followed (a `case`, a function, a command run in the background with
`&`). Through `xargs`, programs that describe files without showing them (`wc`,
`ls`, `stat`, `du`, checksums) run freely, while `xargs cat` or `xargs grep`
ask, as the files come from the input and one of them could be a secret.

A line is allowed only when every command of it is. When it asks, agentin names
the commands that need the question and why, and the panel offers:

- **Allow once** runs the whole line now and remembers nothing; the next such
  command asks again.
- **Trust … for this chat** runs the line and lets this chat run the same
  commands, with any further arguments, without asking until its conversation
  changes or agentin exits. Nothing is saved, and **Settings → Approvals… → Chat
  trust** lists it and can withdraw it. For a single short command the button
  trusts it at once; otherwise **Trust for this chat…** opens a list with a
  choice per command.
- **Always allow…** runs the line and adds Allow lines to agentin's rules, so
  that the commands never ask again. It opens the same list, where each command
  can be kept once, trusted for the chat, or added as a line, which can be
  edited first.
- **Decline** runs nothing of the line; the agent is told that agentin declined
  it and usually tries another way or asks you. **Decline and stop** also ends
  its turn.

What is trusted or added depends on why the command asked. A Git command gets
its subcommand (`git fetch *`, `git fetch` for the chat), one that can lose work
the words up to the risky option (`git tag -f *`). A command that agentin does
not know or that uses the network gets the program with the words that name what
it does (`gh run list *`, `npm test *`). `curl` and `wget` get the server they
fetch from, `curl *https://invent.kde.org/* *`: such a line allows any options
and paths on that server, but asks when the command also talks to another
address, writes outside the writable directories (with `-o` or `-O`), or sends a
secret file (`-d @~/.ssh/id_rsa`). Any other command gets itself, with numbers
as `<int>` (`cmake --build /opt/x -j<int> *`, `rm -rf /tmp/qce-nodl *`); edit
such a line to widen it, for example to `rm -rf /tmp/qce-* *`. A command that
takes its files from `xargs` gets a line without the trailing ` *`, as in `rm`
for `ls | xargs rm`, which allows it only as it is, and cannot be trusted for
the chat. A line that names the whole command also covers what asked about it,
such as the secret file it reads or the protected path it writes; a broader line
does not. Some commands offer nothing to remember and can only be allowed once:
one whose argument has a value not known in advance (`curl "$U"` after
`U=$(…)`), code given on the command line or read from the input
(`python3 -c "…"`, `node -e`, `python3 -` with a here-document, a script piped to
`sh`), which would be new code each time, a redirection outside the writable directories or into a protected
path, a variable of the list, a line too long to run within the time limit, and
installing, such as `make install`, which writes to a place the command does not
name (it can still be trusted for the chat).

Read-only chats are never allowed by the list, but its Deny lines apply. The
log notes each command allowed or declined this way.

This is a guard against mistakes, not a sandbox: programs of the project run
without a question, so a script the agent writes into the project can do
anything the agent could not do directly. A path counts by where it really is:
symbolic links that exist when the line is judged are followed, so writing
through a link that leaves the project asks. A link made earlier in the same
line is not seen that way, which is why `ln` to a place outside the writable
directories asks itself.

For Claude and GLM, a `PreToolUse` hook of the bridge asks agentin about every
shell command before Claude Code applies its own permission rules, so agentin
decides first, with the directory the command runs in and the chat's writable
directories. When a command asks, agentin shows the approval itself, since Claude
Code runs commands it deems read-only even when a hook asks. In a read-only chat
the question is left to Claude Code's plan mode. A `sed` command that the rules
allow runs with `--sandbox` (on Linux), which rejects the sed commands that read,
write or run other files, as a second guard behind agentin's reading of its
script. Codex applies the rules in `~/.codex/rules` itself, without asking
agentin, and commands it runs in its sandbox without asking, such as removals in
the chat's directory, never reach agentin: agentin's lines answer Codex's
questions, but an Ask or Deny line cannot stop a command that Codex does not ask
about. The dialog therefore lists the Codex rules that allow what agentin denies
or asks about, with **Remove from Codex**, which
deletes the rule from Codex's file (Codex keeps using it until its App Server
restarts), and **Keep Codex's rule**, after which agentin stops reporting it.
At every start agentin reports, in the log, Codex rules added while it was not
running and the conflicts not yet resolved.

Codex asks about far fewer commands, as it runs most of them in its sandbox;
those it does ask about are judged the same way. Input that Codex wants to send
to a command that runs already is always shown, as that command may be a shell
reading it as commands. **Always allow…** on such a
command adds only agentin's lines and no Codex rule, so that Codex keeps asking
and agentin's rules, with their denials, keep answering. Chat trust is kept in
agentin's memory; a chain such as `git fetch … && git pull` offers each command,
trusted on its own. It belongs to one conversation in one chat and ends when
changing the conversation or exiting the application.

Network approval requests require their own decision: their **Always allow**
saves the rule that Codex proposes, for a Git command without its files or
message (as `git` with the subcommand), in Codex's rules. `sudo`, `doas`, `su`
and `git push` are never offered a lasting rule, and requests for them are
declined.

Codex can also ask for additional permissions, such as writing to or reading
directories outside the chat's directory, or network access. The panel lists
each path with its access and the reason. **Accept** grants them for the
current turn and **Accept for session** until the chat's thread is closed or the
session approvals are withdrawn in **Settings → Approvals… → Chat trust**;
Codex keeps the grant, and nothing is saved in its configuration. Declining
grants nothing, and **Decline and stop** also ends the turn. Directories
granted for writing join the directory lock: from the next turn, and at once
for the running turn when no other chat holds them; otherwise the chat and the
log say that the directory is also used by that chat. Patterns and special
locations, such as the temporary directory, are granted but not locked.

These checks apply to the commands Codex asks about, not to commands it runs in
its sandbox without a request. Quoted argument text does not count as a command:
`echo 'sudo git push'` only prints. A line that cannot be parsed as Bash is still
declined when one of its commands starts with `sudo`, `doas`, `su` or is a
`git push`; on Windows, where command lines are not Bash, the same holds for
`runas` and for winget, choco and scoop changes.

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

## License

agentin is licensed under the GNU General Public License, version 3 only
(GPL-3.0-only); see `LICENSE`. It links qtermwidget, which is under
GPL-2.0-or-later, and uses Qt under the LGPL-3.0. As section 14 of the GPL-3.0
allows, its author, Andrzej Borucki, or a person he names publicly, is the proxy
who may accept a later version of the GPL for agentin; `CONTRIBUTING.md`
describes this and how contributions are licensed.

## Working on agentin with coding agents

The project's rules for coding assistants are in `AGENTS.md`; there is no
`CLAUDE.md`. Codex reads `AGENTS.md`, and so does a current Claude Code. An older
Claude Code that reads only `CLAUDE.md` needs to be updated, or given a local
`CLAUDE.md` containing the line `@AGENTS.md`.
