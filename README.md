# agentdeskt

A Qt 6 desktop client for Codex, Claude, GLM, Gemini, and Antigravity. Select an
agent in the window, enter a message, and read streamed responses in one output
pane. Codex uses the local `codex` executable and its existing sign-in and
configuration. Claude and GLM run through a Python JSONL bridge to the Claude
Agent SDK. Gemini and Antigravity run through their respective CLIs in headless mode.

## Build and run

Requires Qt 6 Widgets, CMake, a C++17 compiler, and an installed `codex` CLI
for Codex. Claude requires Python 3, `claude-agent-sdk`, and a configured API
key. GLM uses the same Python SDK with a Z.AI API key. Gemini requires an
installed and authenticated Gemini CLI. Antigravity requires an installed and
authenticated `agy` CLI. The optional test also requires Qt 6
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
Google no longer accepts personal Google account sign-in in Gemini CLI. Use a
[Gemini API key](https://geminicli.com/docs/get-started/authentication/#use-gemini-api-key)
or an eligible enterprise Gemini Code Assist account. Google's replacement for
personal account terminal use is Antigravity CLI, available as a separate provider
in this application.

For Antigravity, install and sign in to [Antigravity CLI](https://antigravity.google/docs/cli/install/)
interactively first. The application starts `agy --output-format stream-json
--prompt ...` for each turn and uses `--conversation ID` for later turns. Pass
`--antigravity /path/to/agy` if the executable is not in `PATH`.

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
Each provider node has an expand control. Expanding Codex refreshes
the list. The app saves a local JSON index in its application data directory;
the first sync scans the full history and can take longer. Later syncs read
new active chats from newest to oldest until they reach the saved boundary.
Archived chats are scanned separately. Double click a chat to resume it.
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

Use the **New chat…** button, **Conversations → New conversation in
directory…**, or type `new` to start a chat. In the dialog, choose the agent,
then enter a directory path or choose one with **Browse…**. The **Create chat**
button is available only when that directory exists. Each chat has one working
directory, and files under that directory are available subject to the
selected agent's permissions. Double-click a chat in the conversation tree to
continue it with its agent and working directory.

Type `help` (or `/help`) in the command field to see the available commands.
With Codex selected, it shows the complete output of `codex app-server --help`
and explains that the client uses direct stdio mode. CLI subcommands shown in
that output are reference information and are not chat messages.
With Claude or GLM selected, it explains the SDK workflow
and shows `claude --help` when the Claude CLI is installed. CLI options are shown for reference; this window
communicates through the SDK. Claude Code's interactive slash commands are
listed in the [Claude Code commands reference](https://code.claude.com/docs/en/commands).
See [Z.AI's Claude Code setup](https://docs.z.ai/devpack/tool/claude) for GLM.

With Gemini selected, `help` shows `gemini --help`. If [folder trust](https://geminicli.com/docs/cli/trusted-folders/)
is enabled, trust the working folder in Gemini CLI before starting a headless
conversation. Headless tool approvals follow Gemini CLI's configured policy;
they are not shown as Qt approval dialogs.

With Antigravity selected, `help` shows `agy --help`. The selected working
directory is passed as the CLI process directory. Headless mode uses the CLI's cached authentication and its configured permission
policy; approval prompts are not shown as Qt dialogs.

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
