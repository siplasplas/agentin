# agentdeskt

A Qt 6 desktop client for Codex, Claude, GLM, and Gemini. Select an
agent in the window, enter a message, and read streamed responses in one output
pane. Codex uses the local `codex` executable and its existing sign-in and
configuration. Claude and GLM run through a Python JSONL bridge to the Claude
Agent SDK. Gemini runs through Gemini CLI in headless mode.

## Build and run

Requires Qt 6 Widgets, CMake, a C++17 compiler, and an installed `codex` CLI
for Codex. Claude requires Python 3, `claude-agent-sdk`, and a configured API
key. GLM uses the same Python SDK with a Z.AI API key. Gemini requires an
installed and authenticated Gemini CLI. The optional test also requires Qt 6
Test and Python 3.

```sh
cmake -S . -B build
cmake --build build
./build/agentdeskt -C /path/to/project
```

For Claude, install the SDK in a project virtual environment and provide
`ANTHROPIC_API_KEY` in the application's environment:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install claude-agent-sdk
ANTHROPIC_API_KEY=your-key ./build/agentdeskt -C /path/to/project
```

For GLM, install the same SDK and set `ZAI_API_KEY` to your Z.AI Coding Plan
key. The GLM bridge connects to Z.AI's Anthropic-compatible endpoint. Set
`GLM_MODEL` to override the default `glm-5.3` model. Claude and GLM keep
separate processes and conversations.

```sh
ZAI_API_KEY=your-zai-key ./build/agentdeskt -C /path/to/project
```

For Gemini, install and authenticate [Gemini CLI](https://geminicli.com/docs/get-started/).
The application starts `gemini --output-format stream-json --prompt ...` for
each turn and uses the session ID returned by Gemini CLI to resume the next
turn. Use `--gemini /path/to/gemini` if it is not in `PATH`.

The client automatically uses `.venv/bin/python` when the build directory is
directly inside this project. Otherwise pass `--claude-python /path/to/python`. Use
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
Each of the four agent nodes has an expand control. Expanding Codex refreshes
the list. The app saves a local JSON index in its application data directory;
the first sync scans the full history and can take longer. Later syncs read
new active chats from newest to oldest until they reach the saved boundary.
Archived chats are scanned separately. Double click a chat to resume it.
Directories appear under an agent only when they contain a discovered chat.
Chat previews are shortened in the tree; hover over one to read the longer stored preview.
Codex previews in the local JSON index are limited to 200 characters.

Expanding Claude lists SDK sessions across all Claude projects. Expanding
Gemini asks Gemini CLI for sessions in known working directories. GLM shows chats
recorded by this application; the Claude SDK's shared transcript location does
not identify which endpoint produced an older external session. Claude, Gemini,
and GLM chats started here are saved in separate `claude-conversations.json`,
`gemini-conversations.json`, and `glm-conversations.json` files in the local
application data directory. The old `agent-conversations.json` file is ignored.
Deleting the Codex index makes the next Codex expansion scan its full history.
Claude can rediscover SDK sessions. Gemini can rediscover sessions in known
working directories. GLM entries created here need the local GLM index to
remain available.

Use **Conversations → New conversation in directory…** or type `new` to start
a chat. Enter a directory path or choose one with **Browse…**. The **Create
chat** button is available only when that directory exists. Each chat has one
working directory, and files under that directory are available subject to the
selected agent's permissions.

Type `help` (or `/help`) in the command field to see the available commands.
With Codex selected, it shows the complete output of `codex app-server --help`
and explains that the client uses direct stdio mode. CLI subcommands shown in
that output are reference information and are not chat messages.
With Claude or GLM selected, it explains the SDK workflow
and shows `claude --help` when the Claude CLI is installed. Use the Add directory
button to give that agent access to an additional folder in the current session.
The directory is passed to the SDK using `add_dirs`; it is not saved as a global
Claude Code trust setting. CLI options are shown for reference; this window
communicates through the SDK. Claude Code's interactive slash commands are
listed in the [Claude Code commands reference](https://code.claude.com/docs/en/commands).
See [Z.AI's Claude Code setup](https://docs.z.ai/devpack/tool/claude) for GLM.

With Gemini selected, `help` shows `gemini --help`. Add directory passes
`--include-directories` on future turns. Gemini CLI supports up to five extra
directories. If [folder trust](https://geminicli.com/docs/cli/trusted-folders/)
is enabled, trust the working folder in Gemini CLI before starting a headless
conversation. Headless tool approvals follow Gemini CLI's configured policy;
they are not shown as Qt approval dialogs.

For Codex, Add writable directory adds a root to `sandboxPolicy.workspaceWrite.writableRoots`
on future turns. The working directory remains included. Added directories
include ordinary subdirectories; other permission rules and
protected paths may still apply. Additional directories are kept only while
this application is open.
The client also supports `new`, `clear`, `stop`, and `quit`. Any other text is
sent to the selected agent as a message. Messages entered while a response is
in progress are queued. Requests to approve an action or answer a question
appear in a separate dialog. Each agent keeps its own conversation while the
application is open.

## Test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The Codex client uses JSONL over stdin/stdout (`codex app-server --stdio`). See the
[OpenAI Docs for Codex App Server](https://learn.chatgpt.com/docs/app-server)
for the protocol. The Claude bridge also uses JSONL over stdin/stdout and calls
[`ClaudeSDKClient`](https://code.claude.com/docs/en/agent-sdk/python) in the
Python SDK.
