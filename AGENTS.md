# Repository guidance for coding assistants

## Language policy

- Write project documentation in English.
- Write code comments in English.
- Write texts, messages, helps in code in English.
- Write Git commit subjects and bodies in English.
- Communicate with the user in their preferred natural language, including progress updates, questions, and final responses.

Determine the conversation language in this order:

1. Follow an explicit language request from the user.
2. When continuing or resuming a session, preserve the language established in that conversation, including language preferences recorded in its continuation summary. English repository files or an English summary do not by themselves change the conversation language.
3. For a new conversation, infer the language from the user's opening words and messages. Do not treat quoted text, code, or technical terms as a language switch.
4. If the current conversation provides no clear signal, use a known language preference from other sessions when that context is available. Do not assume access to unavailable session history.
5. If no preference can be inferred, ask briefly which language the user prefers.

The conversation language does not change the English-language requirements for tracked documentation, code comments, or Git commit messages.

## Git

- AI assistants may create local Git commits, but must never run `git push` or use another tool to push commits, branches or tags to a remote repository. A human performs all pushes.
- `ROADMAP.md` is tracked, for anyone who works on agentin, and lists only work not done yet. When an item is finished, describe it in `README.md` and remove it from the roadmap in the same commit.
- Handoff notes (`HANDOFF-*.md`) are working notes between sessions: never commit them and never delete them; leave them untracked, and the user removes them when no longer needed.
- Hand a turn's work over to the user, and commit it, only when the project builds (`cmake --build build` with no errors, also for the test targets when they are configured). If something out of the ordinary keeps the project from building, such as a missing system package or a broken toolchain, do not present the work as done: tell the user what fails and why.
- The repository is public. Before every commit, check the staged changes (`git diff --cached`) for anything private: API keys, tokens or passwords, personal paths such as home directories, e-mail addresses, private notes, logs, settings files, or the content of the user's conversations. Leave such things out or replace them with neutral examples, and tell the user what was found.

## Building and running

- Qt 6 only (Widgets; Test for the tests) and C++17. Do not add Qt 5 compatibility code.
- Build: `cmake -S . -B build && cmake --build build`; run `./build/agentin -C /path/to/project`.
- With tests: `cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build`.
- The Claude and GLM bridge (`claude/bridge.py`) runs in the project virtual environment `.venv` with `claude-agent-sdk`; the build copies the script next to the executable as `claude_bridge.py`.
- The application stores its conversation indexes, recent directories and command rules in its application data directory (`codex-conversations.json`, `<provider>-conversations.json`, `recent-directories.json`, `approvals.json`; the last one is version 2, with the rules, the lists of secret files, protected paths and variables, and the optional time limit).

## qt-extra

- `QxFileDialog` and `MruTabWidget` come from qt-extra (source: `/home/andrzej/wazne/gitmy/qt-extra`, published as github.com/siplasplas/qt-extra). `CMakeLists.txt` takes it with `FetchContent` and `FIND_PACKAGE_ARGS`: an installed qt-extra 2.1 or newer (here in `/usr/local`) is used, and otherwise the pinned commit is fetched and built. Pin a commit or tag that is on GitHub, and move the pin when agentin needs a newer qt-extra.
- Changes to those widgets belong in qt-extra, which has its own `AGENTS.md` and semver rules; gemini-commander uses the same package. Installing needs `sudo`, so the user does it. For larger changes, write a handoff note in the qt-extra directory and let the user start a session there.

## Tests

- When a change would benefit from tests, propose them briefly (what they would cover and roughly how) and add them once the user agrees; do not add tests without that agreement.
- Fast tests may be committed and run with `ctest --test-dir build --output-on-failure`: they take seconds, use fake agent scripts, and need no network, accounts, API keys or installed agent CLIs. The current suite is `tests/MainWindowTest.cpp` (Qt Test, offscreen), `tests/ShellApprovalTest.cpp` (decisions about shell command lines, table-driven from `tests/data/shell-commands.json`, and the parser and evaluator of `src/shell`), `tests/ClaudeBridgeTest.py` and libdiffcore's `libdiffcore/tests`.
- Long-running tests (real agent CLIs, network, accounts, or many seconds each) are not part of the committed suite. Run them at most once, when a feature is handed over, not after every change.
- While working, run only the one or two tests that cover the change, for example `QT_QPA_PLATFORM=offscreen ./build/agentin_test helpAndConversation`, and say which tests were not run.
- Fake agent scripts in tests must follow the real protocol. For Codex, `codex app-server generate-json-schema --out <dir>` writes the App Server schema; for example, notifications for a thread carry `threadId`.

## Code structure

- `AgentProvider` (in `src/AgentBackend.h`) is one kind of agent: it lists conversations for the tree, gives help and lock checks, and creates chats. `AgentBackend` is one chat session with its own protocol state, prompt queue and turn state.
- Providers and chats: `CodexConnection` and `CodexAgent` (one shared `codex app-server --stdio` process, messages routed by `threadId`), `ClaudeProvider` and `ClaudeAgent` for Claude and GLM (one bridge process per chat), `GeminiProvider` and `GeminiAgent`, `AntigravityProvider` and `AntigravityAgent` (one CLI process per turn).
- `ChatTab` is the model of one tab: its chat session, text document, preview or live state, and history. `MainWindow` owns the providers, the conversation tree, the tabs and one chat view that moves into the current tab.
- `ConversationIndex` keeps a provider's local JSON index; `ProcessLocks` detects conversations held open by other tools.
- A new agent needs a provider and a chat class; `MainWindow` should not need provider-specific branches.
- `libdiffcore/` is the O(NP) line diff, taken from diffmerge and maintained here as part of agentin (C++17, Qt Core only); `countLineChanges` in `src/LineChanges.h` counts added and removed lines with the limits of the changed files view. `GitBaseline` (`src/GitBaseline.h`, libgit2) records a Git work tree at the start of a turn and lists the files changed since. `ChangeTracker` keeps a chat's baselines in a worker thread, where libgit2 objects must stay, and counts the changes of its latest turn. `ChangesWindow` shows them with the diff of the selected file.
- `src/shell/` (Qt Core only) reads Bash command lines for the command approvals: `parseBash` (`ShellAst.h`, `BashParser.cpp`) gives a tree, `evaluate` (`ShellEvaluator.h`) follows the directory and the variables through it and gives one `CommandUse` per command, and `classifyProgram` (`CommandCatalog.h`) tells what a known program reads, writes and runs. None of them touches the file system. A program is in the catalog as reading only when no option of it writes or runs something; anything not understood is `Unknown` or a problem, never a guess. `commandRuleVerdict` in `CommandApproval.cpp` turns the commands of a line into Allow, Ask or Deny with the rules of `approvals.json` and a `CommandContext` (directory, writable directories, chat trust) that each agent fills in; a change there needs a row in `tests/ShellApprovalTest.cpp` or the corpus.
- Match the surrounding code style. User-visible behavior changes go into `README.md`.
