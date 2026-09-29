# agentdeskt

A simple Qt 6 client for Codex App Server. It provides a command input and an
output pane that displays streamed Codex responses. It starts the local `codex`
program and uses your existing Codex sign-in and configuration.

## Build and run

Requires Qt 6 Widgets, CMake, a C++17 compiler, and an installed `codex` CLI.
The optional test also requires Qt 6 Test and Python 3.

```sh
cmake -S . -B build
cmake --build build
./build/agentdeskt -C /path/to/project
```

Without `-C`, the conversation uses the terminal's current directory. If
`codex` is not in `PATH`, the client also checks the Codex desktop installation
and `CODEX_BIN`. You can select an executable explicitly with
`--codex /path/to/codex`.

Type `help` (or `/help`) in the command field to see the available commands and
the options reported by `codex app-server --help`.
The client also supports `new`, `clear`, `stop`, and `quit`. Any other text is
sent to Codex as a message. Messages entered while a response is in progress
are queued. Server requests to approve a command or file change appear in a
separate dialog.

## Test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The client uses JSONL over stdin/stdout (`codex app-server --stdio`). See the
[OpenAI Docs for Codex App Server](https://learn.chatgpt.com/docs/app-server)
for the protocol.
