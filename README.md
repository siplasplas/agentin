# agentdeskt

A Qt 6 desktop client for Codex App Server and Claude Agent SDK. Select an
agent in the window, enter a message, and read streamed responses in one output
pane. Codex uses the local `codex` executable and its existing sign-in and
configuration. Claude runs through a small Python JSONL bridge.

## Build and run

Requires Qt 6 Widgets, CMake, a C++17 compiler, and an installed `codex` CLI
for Codex. Claude requires Python 3, `claude-agent-sdk`, and a configured API
key. The optional test also requires Qt 6 Test and Python 3.

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

The client automatically uses `.venv/bin/python` when the build directory is
directly inside this project. Otherwise pass `--claude-python /path/to/python`. Use
`--claude-bridge /path/to/bridge.py` when running an executable from another
location without its copied bridge script.

Without `-C`, the conversation uses the terminal's current directory. If
`codex` is not in `PATH`, the client also checks the Codex desktop installation
and `CODEX_BIN`. You can select an executable explicitly with
`--codex /path/to/codex`.

Type `help` (or `/help`) in the command field to see the available commands.
With Codex selected, it shows the relevant options reported by
`codex app-server --help` and explains that the client uses direct stdio mode.
The `daemon` and `proxy` subcommands are for a separate persistent server.
With Claude selected, it explains the SDK workflow
and shows `claude --help` when the Claude CLI is installed. CLI options are
shown for reference; this window communicates through the SDK.
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
