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

The lock is in place for all tabs and agentin windows (registry `turn-locks.json`), counts busy
Claude Code sessions from their registry, Codex turns from the rollout files that Codex keeps open, and
running Gemini and Antigravity CLI processes, and lets read-only Codex turns through; read-only turns of
the other agents (their plan modes) still wait, because only Codex enforces them. Still open:

- **Writable directories of other agents:** Codex reports its writable roots, and they are held with
  the working directory. Claude, GLM, Gemini and Antigravity chats hold only their working directory,
  since agentin does not give them other directories; Claude Code sessions outside agentin do
  not report theirs.

## Files changed by a turn

A window listing the files changed since the current turn started (or since a chosen point), with the
lines added and removed for each, a diff of each file, and opening the file in its application.

Done: libdiffcore is part of agentin (`libdiffcore/`), with a counting mode of its main loop that keeps
memory linear and stops at an edit distance bound, a deadline or a cancellation flag
(`DiffEngine::countChanges`), and `countLineChanges` (`src/LineChanges.h`) applies the limits below.
Done: `GitBaseline` (`src/GitBaseline.h`, libgit2) captures the start of a turn and lists the changed
files as described below, keeping the start content of tracked files that already differed from Git in
memory rather than as blobs, so nothing is written to the repository.
Done: `ChangeTracker` (`src/ChangeTracker.h`) captures the baselines when a chat's turn starts, counts in a
worker thread with a cache by file size and modification time, refreshes every 4 seconds during the turn
and at its end, and the chat header shows `Changes: 7 files, +120 −34`.
Done: `ChangesWindow` (`src/ChangesWindow.h`), opened from that summary, lists the files and shows the
unified diff of the selected one, computed in the tracker's worker thread, and opens files with their
default application. It also has the side-by-side view, moving between changes, folding of long
unchanged stretches and the "Since" choice (turn, chat, `HEAD`). Still open: syntax highlighting
(later, possibly with KSyntaxHighlighting), reverting a file to its baseline, and comparing earlier turns.

### Baseline: Git plus the start content of files changed before the turn

The content of a file before the turn must be known when the turn starts; after a change it is gone,
so a fingerprint taken at the start cannot recover it. Git already stores the content of every committed
file, so only tracked files that differ from Git need their start content kept:

1. **At turn start**, in each directory the turn holds (the chat's directory and its writable
   directories) that is inside a Git work tree, record the commit of `HEAD`, whose tree is the baseline
   of every clean tracked file, and for each tracked file that is not clean keep its current content as
   a blob in Git's object database (`git hash-object -w`, `git_blob_create_from_disk` in libgit2). That
   changes neither the index, the branches nor the work tree; the loose objects are pruned by `git gc`
   after their expiry. Keeping `git diff HEAD` for those files and applying it to the `HEAD` blob later
   is the alternative.
2. **Skipped:** everything `.gitignore` ignores.
   **Binary files** tracked by Git are compared by hash only: the blob id in the start commit's tree, or
   the hash of a kept blob, against the hash of the file now. A changed one is listed with its sizes and
   no line counts or diff. Text files above the size limit below are treated the same way.
3. **New files:** a file Git does not track and does not ignore is shown as a whole new file, without a
   diff. Its size and modification time are noted at the start, so that one existing before the turn is
   shown only if the turn changed it.
4. **During and after the turn**, the changed files are the union of the tracked files the status
   reports as not clean now, the files changed by commits made during the turn (diff of the start
   commit's tree with the current `HEAD` tree, so that an agent committing its work does not hide it),
   and the files that were not clean at the start (they may have been restored). The "before" content is
   the kept blob when there is one, otherwise the blob in the start commit's tree; the "after" content is
   the file on disk, or nothing when it was deleted. A file whose before and after content are equal is
   dropped.
5. **Outside a Git work tree**, changes are not tracked for that directory, and the window says so.

The agents also report files they edit (Codex `fileChange` items, Claude `Edit`/`Write` inputs). They
can mark files as changed at once during a turn, but shell commands change files unreported, so the
status scan stays the source of truth.

**Git access:** libgit2 (C, `libgit2-dev`, 1.9 here; the same package exists in the major
distributions) reads the status, trees and blobs in process, without starting a `git` process for each
file and without parsing its output. The `git` command line (`rev-parse`, `status --porcelain=v2 -z`,
`diff-tree`, `cat-file --batch`) is the alternative if a dependency is unwanted; it is slower for many
files.

**Counting:** the O(NP) diff from libdiffcore (`diffcore::DiffEngine` over interned lines). The library
comes from `/home/andrzej/wazne/gitmy/diffmerge/libdiffcore` and is included as a subdirectory of
agentin's sources (`add_subdirectory`), so it is part of agentin and is changed there as needed;
changes worth sharing can be taken back to diffmerge. It is self-contained: a static library using only
the Qt Core that agentin already links and the C++ standard library, nothing else from DiffMerge, and it
compiles as C++17. Its `DIFFMERGE_BUILD_TESTS` option is dropped or renamed in the copy; its main loop is enough for the counts, and the slider
heuristics matter only when a diff is shown. Counts are computed in a worker thread and cached by the
pair of content hashes, so live updates during a turn recompute only files that changed again.

**Limits:** the O(NP) diff costs about (N + M) · P for N and M lines and P differences, so size alone
is rarely the problem; a large file rewritten completely is. Three guards keep it bounded, as constants
to tune after measuring real files:

- text files up to **64 MB** (about one or two million lines) get counts and a diff; larger ones are
  compared by hash only and listed with their sizes, as binary files are;
- a file with more than **100 000** differences is listed as rewritten, with its line counts before and
  after instead of exact added and removed lines. Lines one side has more often than the other bound the
  differences from below in linear time, which tells most rewritten files apart before the search; the
  main loop also stops once P passes the limit;
- the main loop also has a time budget of about **3 seconds** per file: it reads a monotonic clock now
  and then, for example once per value of P or every few thousand snake steps, so the check costs
  nothing noticeable, and stops when the budget is spent; the file is then listed as rewritten like
  above. The same check lets a computation stop early when the file changes again or the window closes;
- the work runs in the worker thread, so a slow file never blocks the window, which shows "counting…"
  for it until the result arrives.

### The window

A non-modal window per chat (or a dock beside the chat), opened from the chat header ("Changes: 7 files,
+120 −34") and from the menu, updated live while the turn runs:

```
┌ Changes — Codex: "Fix the tree sorting" ─────────────────────────────────────┐
│ Since: [This turn ▾]  (turn started 14:32)   Files: 7   +120  −34   [⟳]      │
│ ┌──────────────────────────────────────────────────────────────────────────┐ │
│ │ St  File                                     +     −   ▕████████▏        │ │
│ │ m   src/MainWindow.cpp                      +84   −20   ██████░░          │ │
│ │ m   src/MainWindow.h                         +9    −1   █░░░░░░░          │ │
│ │ n   src/TailFollower.cpp                   (new, 22 lines)                │ │
│ │ d   src/old/Legacy.cpp                            −13   █░░░░░░░          │ │
│ │ r   docs/a.md → docs/b.md                    +3    −0                     │ │
│ │ b   assets/icon.png                       (binary, 4.1 → 4.3 kB)          │ │
│ └──────────────────────────────────────────────────────────────────────────┘ │
│ ┌ src/MainWindow.cpp ─────────────────── [Unified|Side by side] [◀ hunk ▶] ┐ │
│ │ 1412   1412      refreshConversationTree();                              │ │
│ │ 1413        -    revealCurrentConversation(false);                       │ │
│ │        1413 +    if (!match) {                                           │ │
│ │        1414 +        revealCurrentConversation(false);                   │ │
│ └──────────────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────────────┘
```

- **Since:** this turn (default), since the chat started (the first turn's baseline is kept), or since
  `HEAD`, which shows Git's view including changes made before the chat.
- **List:** status (`m` modified, `n` new and shown without a diff, `d` deleted, `r` renamed, `b` binary
  and changed, compared by hash), path relative to the chat's directory (other held directories shown
  with their full path), lines added in green and removed in red, and a bar with their share; sortable
  by path or by size of change; a total in the header.
- **Diff pane:** the selected file's diff from libdiffcore with the slider heuristics, unified or side by
  side, with line numbers and moving between hunks; long unchanged stretches are folded.
- **Opening:** double-click or Enter opens the file with the application the system associates with its
  type (`QDesktopServices::openUrl`, as `xdg-open`), for example CLion for `.cpp` or RustRover for `.rs`
  when those are the defaults, which reuses a running instance as the desktop does. The context menu also
  offers opening the containing folder and copying the path.
- **Later:** reverting a file to its baseline, and keeping the baseline of each turn so that earlier
  turns can be compared too.
