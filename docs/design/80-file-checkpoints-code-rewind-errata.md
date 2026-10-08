# 80 - File Checkpoints + Code Rewind Errata: restoring FILE/CODE state from `/rewind`

```
Status: verified (Rev 6)
Revision: 6
Verification status: adversarial reviewer (independent, Rev 6): GATE PASS (0 HIGH / 0 MEDIUM, 3 LOW); Oracle confirm PASS
Component: 80 (errata) - adds a per-workspace FILE checkpoint subsystem that
           snapshots the PRE-state of files mutated by the agent's trackable
           file-mutating tools (`write_file`/`edit_file`) keyed to a turn, and
           extends spec 79's `/rewind` from conversation-only to a three-way
           restore (code / conversation / both) with a full-screen two-step menu.
           Design only: no implementation code.
Depends on: 00-architecture.md (verified; sec.9.2/sec.9.9/sec.20.24/sec.44/sec
            54 D14/D16), 01-session.md (verified; sec.3/sec.6/sec.9.3),
            02-persistence.md (verified; sec.4.5/sec.6.4-6.6/sec.9 P11 - the
            durable session store, the derived-cache invariant, and the physical
            fork strategy; it is NOT the atomic-write precedent - see sec.6.3),
            05-transport.md (verified; sec.7.4 method table),
            06-agent-loop.md (verified; turn lifecycle),
            07-tools-execution.md (verified; sec.5/sec.6/sec.15 - the tool
            contract, `ToolContext`, and the root-confined `resolve()` seam),
            10-supervisor-tui.md (verified; sec.4.3/sec.4.4/sec.9.2),
            16-daemon-ownership.md (verified; daemon lifetime unchanged),
            21-config-jsonc-errata.md (verified; JSONC is the sole config),
            22-switcher-sessions-errata.md (verified; sec.3/sec.4 the
            live-vs-stored split), 23-session-lifecycle-errata.md (verified;
            `session prune` and session deletion), 24-agent-lifetime-errata.md
            (verified), 42-agent-presets.md (verified), 45-ui-interaction-errata.md
            (verified; key precedence), 48-ui-and-config-errata.md (verified;
            EscArm), 57-switcher-sessions-popup-errata.md (verified; key
            ownership), 60-scroll-keybindings-errata.md (verified),
            78-fork-command-errata.md (verified, Rev 4; `CommandContext::fork`,
            `forkSession`), 79-rewind-command-errata.md (verified, Rev 2; the
            conversation rewind: `CommandContext::rewind`,
            `RewindOverlayModel`, `UiMode::Rewind`, `session.rewind_targets`,
            `session.fork` with `seed_length = boundary_index`),
            81-session-dashboard-errata.md (verified, Rev 6; the full-screen base
            selector 81-D1, chain-A/chain-R, 81-D13 overlay save/restore).
Scope: one user-requested feature: restore FILE/CODE state (Claude-Code
       "checkpointing"), and integrate it with spec 79's conversation rewind so
       `/rewind` offers the three CC code/conversation outcomes. The mechanism:
       (1) a daemon-side `CheckpointStore` under `<ws>/.ymh/checkpoints/` holding
       content-addressed pre-image blobs plus a JSON index keyed by
       `(session, turn)`; (2) a capture hook at the single tool-dispatch choke
       point that snapshots a mutating file's PRE-state before it is changed;
       (3) a root-confined, symlink-refusing restore over
       `ExecutionEnvironment::resolve()`; (4) a bounded retention/eviction sweep
       tied to session deletion and daemon startup; (5) a two-step full-screen
       `/rewind` menu (checkpoint list -> action menu) reusing spec 81's
       full-screen surface. Conversation restore still branches exactly as
       spec 79 (unchanged); code restore mutates the working tree only.
       Out of scope: shell/PTY/terminal file edits (untrackable, excluded, CC
       parity); subagent edits (excluded, CC parity); git-based snapshotting
       (no git dependency); CC's "Summarize from/up to here"; an in-place
       conversation truncation; per-file hand-picked restore; binary-delta
       compression; a configurable retention policy (constants this revision).
Supersedes: 79-D6's `clear_under` render-mode decision and 79-RW13's "it is never
            a full-screen renderer and it introduces no new framework" clause:
            spec 80 promotes the `/rewind` surface to a full-screen surface
            (spec 81's base-selector pattern) so the checkpoint list and the
            action menu render full-screen. Spec 79's conversation mechanism
            (79-D2/79-D3/D4/D5, RW1-RW12, RW-F1-RW-F11) is NOT superseded and is
            reused unchanged; only its render mode is replaced. Also supersedes
            79 sec.13's "File/code checkpoints are spec 80, deferred" statement
            by delivering that deferred half, and supersedes 79-D7's
            "conversation-only, one action" scope: the picker now offers the
            four-action code/conversation menu (80-D9), and 79-D7's parity
            statement (code restore deferred to spec 80) is fulfilled here.
Amends:     05 sec.7.4 (method table) - adds one method `session.restore_code`
            (params `{session, turn}`, result `protocol::RestoreReport`); the existing
            read-only `session.rewind_targets` result gains one additive field
            (`RewindTarget::file_change_count`, default 0).
            The `TransportHost` base virtual set - adds
            `restoreCode(const SessionId&, TurnId)`; the frozen interface changes,
            so the full implementer set is enumerated in sec.4.4 (beside the 78/79
            deltas).
            07 sec.14.1 - `Tool` gains one additive virtual `checkpoint_paths`;
            built-ins that are not `write_file`/`edit_file` inherit the empty
            default, so the change is additive and the implementer set is the two
            mutating tools (sec.4.2).
            10 sec.9.2 (the command surface) - `/rewind` help text changes from
            "rewind the conversation to a previous turn (branch)" to the
            three-way wording; no new command name.
            45 (key precedence) and 81 sec.2.2 - additive: a new chain-A guard
            `mode == UiMode::RewindAction` beside the spec 79 `Rewind` guard, and
            a new chain-R full-screen base branch (sec.7).
            02-persistence.md - records one new on-disk artifact
            (`<ws>/.ymh/checkpoints/`, sec.6) but changes no SQLite schema; its
            atomic-write discipline follows the existing JSON-state precedents
            (`src/config/grant_store.cpp:242-278`, `src/cli/cli.cpp:824-884`,
            `src/skills/workspace_trust.cpp:97-107`), NOT 02 sec.6 (which is the
            SQLite flush/snapshot policy).
            00-architecture.md / build - promotes `fsync_parent_directory` from
            the `src/cli/cli.cpp` anonymous namespace into a shared
            `include/ymh/core/fs.hpp` + `src/core/fs.cpp` so the store can reuse
            it (sec.6.3); the old file-local copy is deleted and callers are
            enumerated.
            23-session-lifecycle-errata.md - extends session deletion (`session
            prune` / `session.delete`) to also drop a session's checkpoint
            manifests and garbage-collect its blobs (sec.9).
```

ASCII only. Every anchor is `file:line` against the merged `tmp` tree (main+dev),
except where a cited spec is the anchor (then `NN sec...`).

---

## 1. Purpose and scope

Claude Code's `/rewind` "opens a checkpoint menu and can restore the conversation
and/or code" ([recon-claude-code.md sec.1.2], official
`code.claude.com/docs/en/checkpointing`). ymh implements only the conversation
half (spec 79, verified + implemented): `/rewind` lists the focused session's
user turns and branches the session at the chosen turn via `session.fork`.

This spec adds the **code** half and the **menu** that selects between them. The
substrate that already exists:

| Layer | Symbol | Anchor |
|---|---|---|
| Turn boundary | `payload::TurnStarted{turn, origin}` / `TurnEnded` | `include/ymh/session/events.hpp:52-84` |
| Trackable mutating tools | `write_file`, `edit_file` | `src/tools/builtin_tools.cpp:151,183`; factories `:457` |
| Single dispatch choke point | `ToolRegistry::execute` -> `Tool::execute` | `src/tools/tool_registry.cpp:321-347` |
| Path-safety seam | `ExecutionEnvironment::resolve()` | `include/ymh/execution/environment.hpp:33`; `src/execution/environment.cpp:61-95` |
| Turn identity on a call | `ToolContext::turn()`, `sessionId()` | `include/ymh/tools/tool_context.hpp:50-51` |
| Content-addressed digest | `ymh::sha256_hex` | `include/ymh/llm/llm_runtime.hpp:40`; `src/llm/sha256.cpp:32` |
| Atomic JSON state precedent | temp+fsync+rename | `src/config/grant_store.cpp:242,260,270,278`; `src/cli/cli.cpp:799-806` (the `fsync_parent_directory` helper); `src/skills/workspace_trust.cpp:97-107`. (`src/agent/handoff.cpp:730-760` does temp+rename **without** an fsync and is not an fsync precedent; NEW-L4) |
| Dir fsync helper | `fsync_parent_directory` | `src/cli/cli.cpp:799-806` |
| Conversation rewind (reused) | `RewindOverlayModel`, `session.fork` branch | `79 sec.4.4/sec.5`; `include/ymh/ui/ui_model.hpp:600-625` |
| Full-screen base selector | `dashboard.open ? render_dashboard : main` | `src/ui/ui_render.cpp:2395-2399`; 81-D1 |

The design is deliberately additive:

1. A **checkpoint** is a logical marker at each user-origin turn, populated
   lazily with the PRE-image of each tracked file the turn mutates (80-D1).
2. Pre-images are stored as **content-addressed blobs** in a per-workspace shadow
   directory with a crash-safe JSON index; no git, no new external dependency
   (80-D3).
3. Capture happens at the **single tool-dispatch choke point**, before the
   mutation, using the tool's own resolved path (80-D2).
4. Restore is **root-confined** through `ExecutionEnvironment::resolve()`,
   refuses symlinks, and reports per-file outcomes (80-D5/80-D6).
5. `/rewind` becomes a **two-step full-screen menu**: checkpoint list, then a
   four-action menu whose code actions appear only when there is code to revert
   (80-D8/80-D9). Conversation restore reuses spec 79's branch untouched.
6. Retention is **bounded** (100 checkpoints/session, 30 days) and integrated
   with session deletion and a daemon-startup sweep (80-D7).

The fork itself remains spec 78's; the conversation branch remains spec 79's.
This spec sends the same `seed_length = boundary_index` and adds one new
mutating control-plane method for code.

---

## 2. Current behaviour (verified against the tree, not trusted from the brief)

**2.1 Only two built-ins mutate files through the tool seam.** The registered
file-mutating built-ins are `write_file` (`src/tools/builtin_tools.cpp:151`,
factory `:457`) and `edit_file` (`:183`). `shell` (`:381`) and `terminal`
(`src/tools/terminal_tool.cpp:57`) mutate via subprocesses and cannot be observed
at a file granularity. `read_file`/`grep`/`glob` are read-only. There is **no
delete or rename tool**: `write_file` creates-or-replaces, `edit_file` replaces a
literal string in an existing file. Both take the target path from the `path`
argument and resolve it with `context.resolve(...)` through
`resolve_required_path` (`src/tools/builtin_tools.cpp:33`).

**2.2 The tool dispatch has a single validated choke point.** All tool calls flow
through `ToolRegistry::execute(const payload::ToolCall&, const ToolContext&)`
(`src/tools/tool_registry.cpp:321`), which validates the schema and then calls
`tool->execute(context, arguments)` at `:347`. The agent loop reaches it from
`AgentLoop::runToolCall` (`src/agent/agent_loop.cpp:963-998`) at `:987`
(`services_.tools->execute(plan.call, context)`); `AgentLoop::prepareToolCall`
(`:828`) builds the `payload::ToolCall` with its `turn`/`step`. The
`ToolContext` carries `sessionId()` and `turn()` (`include/ymh/tools/tool_context.hpp:50-51`)
and `execution()` (`:43`). A capture placed immediately before `:347` therefore
sees the resolved environment, the session, the turn, and the tool, and runs
strictly before any bytes change.

**2.3 `resolve()` is root-relative, realpath-canonical, and escape-rejecting.**
`LocalEnvironment::resolve` (`src/execution/environment.cpp:61-95`): empty path
throws `ToolErrorCode::PathEscape`; the root is canonicalized at construction
(`:53-57`); a relative path is joined to the root; `Unrestricted` mode returns a
`lexically_normal` path only when the sandbox opts out (`:69-71`); otherwise the
**parent** directory is `weakly_canonical`ized (`:14`, `:73`) and checked with
`path_is_within` (`:36-39`), then an existing final path is `canonical`ized and
re-checked. Because the check is on the canonical parent, a path whose parent
escapes the root is rejected; a not-yet-existing target (a `write_file` create)
resolves to `parent/filename` without a canonical final step and is still
containment-checked. `path_is_within` uses component-prefix comparison
(`:24-34`). This is the only path base tool code may use; `getcwd()` is never a
resolution base (`07 sec.6.3`).

There is **no non-following (lstat) mode in `resolve()`**: it calls
`std::filesystem::canonical` on an existing final path (`:79-84`), so a symlink
leaf is returned as its **target**, and a caller that checks `symlink_status` on
the resolve() result always sees a regular file. Capture and restore must
therefore inspect the **unresolved** candidate path with
`symlink_status`/`hard_link_count` **before** handing it to `resolve()`
(sec.5/7, `first_symlink_component`). `resolve()` still correctly prevents
escaping the root, including through a symlinked parent.

**2.4 The persistence layer writes crash-safe JSON today.** Config import
(`src/cli/cli.cpp:824-884`), the file grant store
(`src/config/grant_store.cpp:242-278`), and workspace trust
(`src/skills/workspace_trust.cpp:97-107`) all write to a `*.tmp.<pid>` file,
`fsync` it, `rename()` it over the target, then `fsync` the parent dir
(`fsync_parent_directory`, `src/cli/cli.cpp:799-806`), guarded by an flock
where concurrent writers exist (grant store). The durable session store
`<ws>/.ymh/sessions.db` is SQLite in WAL with `synchronous = NORMAL`
(`src/session/session_persistence.cpp:691-699`). The per-workspace `.ymh/`
directory is created by the daemon (`src/host/workspace_host.cpp:1405`) and by
the CLI (`src/cli/session_cli.cpp:54,58`). There is **no content-addressed blob
store and no generic atomic-write helper** in `include/ymh/`; each writer
re-implements the temp+rename idiom. `fsync_parent_directory` itself is
**file-local**: it sits inside the anonymous namespace opened at
`src/cli/cli.cpp:98` and closed at `:1003`, so another TU cannot link it. Spec 80
promotes it to `include/ymh/core/fs.hpp`/`src/core/fs.cpp` (sec.6.3) and
introduces the first blob store (sec.6).

**2.5 Session deletion is transaction-scoped in SQLite and does not touch files
outside `sessions.db`.** `SessionPersistence::eraseWithEvent`
(`src/session/session_persistence.cpp:963-1010`) deletes the session's
`session_snapshots`, `session_leases`, `events`, and `sessions` rows in one
`BEGIN IMMEDIATE` transaction; `SessionManager::deleteSession`
(`src/session/session_manager.cpp:181-214`) wraps it and publishes the terminal
`SessionEnded`. The daemon path is `HostRuntime::deleteSession`
(`src/host/host_runtime.cpp:918-1010`) which disposes the agent and calls
`runtime_.sessions().deleteSession` (`:973`). `ymh session prune`
(`src/cli/session_cli.cpp:540-690`) selects **unprompted root** sessions
(`select_empty_roots`, `:370-398`) and deletes them either through a live daemon
RPC `session.delete` (`:432-441`) or, offline, directly via a read-write store
and `manager.deleteSession` (`:501-504`). No deletion path removes any file under
`<ws>/.ymh/` other than rows in `sessions.db`.

**2.6 The `/rewind` UI is a `clear_under` overlay today.** `UiMode::Rewind`
(`include/ymh/ui/ui_event.hpp:56-57`); `RewindOverlayModel` with `cursor`,
`targets`, `prev_mode` (`include/ymh/ui/ui_model.hpp:600-625`); the handler
`handle_rewind` and the fork caller `rewind_to`/`apply_rewind_success`
(`src/ui/supervisor.cpp:2037-2135`); registered at `src/ui/command_registry.cpp:299-305`;
rendered as a `dbox` overlay in `src/ui/ui_render.cpp:2414-2416`
(`render_rewind`, `:1584`). The full-screen base selector is
`model.dashboard.open ? render_dashboard(...) : main`
(`src/ui/ui_render.cpp:2395-2399`). Chain A guards are ordered
exitConfirm > dialog > Context > Switcher > ModelPicker > Rewind > Dashboard
(`src/ui/supervisor.cpp:4304-4335`). The wire projection is
`session.rewind_targets` -> `protocol::RewindTargets`
(`include/ymh/transport/protocol.hpp:411-423`; `:554`), dispatched at
`src/transport/protocol_server.cpp:442-444`.

**2.7 `session_snapshots` is an unwired derived cache, not a restore point.**
The table (`src/session/session_persistence.cpp:75-83`) has no production writer
or reader; `Session::snapshot()` is a resolve cache only (`79 sec.13`). Spec 80
does not use it; code checkpoints are a separate shadow store (80-D3).

---

## 3. Decisions

### 80-D1 - A checkpoint is a lazily-populated per-turn PRE-image set

A **checkpoint** is a logical record keyed by `(session, TurnId)` for each
user-origin turn - the same turns spec 79 lists (79-D8, `session.rewind_targets`)
- created the **first time** a tracked mutating tool runs inside that turn. It is
not an eager copy of the tree at turn start (which would be unbounded and would
snapshot untouched files). When path `P` is first mutated in turn `T`, the store
records `P`'s **pre-image** - its bytes immediately before that mutation - which,
because no tracked mutation of `P` happened earlier in `T` (`write_file` /
`edit_file` are the only writers and each first-touch is captured), equals `P`'s
state at the start of `T`. Later mutations of `P` in the same `T` do not
overwrite the first pre-image. This mirrors CC's "captures the state of your code
before each prompt" while keeping storage proportional to changed files.

### 80-D2 - Only `write_file` and `edit_file` are tracked; shell/PTY are excluded

The tracked set is exactly `{write_file, edit_file}`
(`src/tools/builtin_tools.cpp:151,183`; `include/ymh/tools/tool.hpp:66-70`).
`shell`, `pty`, and `terminal` are **not** tracked: their file effects are not
observable at the tool seam, exactly CC's documented limitation ("Checkpointing
does not track files modified by Bash commands", [recon-claude-code.md sec.1.3]).
Note that `tool_is_mutating` (`src/policy/permission_policy.cpp:191-205`)
classifies `shell`/`pty`/`terminal`/`checkout`/`git_checkout` as mutating for
**permission** purposes; that is a different axis and is not reused here. The
tracked set is realized **only** by the `checkpoint_paths` overrides on those two
tools (sec.4.2): there is no name-list constant and no name-based classification,
so every other tool - including MCP tools - inherits the empty default and is
never tracked.

### 80-D3 - Storage: content-addressed blobs + a JSON index under `<ws>/.ymh/checkpoints/`

Default storage medium is a shadow directory (**no git dependency**,
design-brief.md sec.G.3). Layout and atomic-write protocol are pinned in sec.6:
`blobs/<aa>/<sha256>` for pre-image bytes, `index.json` for the manifests.
Content addressing gives cross-file/cross-turn dedup for free. The blob id is
`ymh::sha256_hex` (`include/ymh/llm/llm_runtime.hpp:40`, `src/llm/sha256.cpp:32`)
- an in-tree, dependency-free pure function already used by the agent loop
(`src/agent/agent_loop.cpp:144,159,757,1166,1455`) and the LLM runtime
(`src/llm/llm_runtime.cpp:145,160,170`). Spec 80 includes it only from
`src/session/checkpoints.cpp`, keeping the header free of an `llm/` edge
(recorded risk in sec.15: relocate it to `core/` if a reviewer prefers). No new
external dependency is added.

### 80-D4 - Creation, overwrite, delete, and rename semantics

- **Create** (`write_file` to a path that did not exist): the pre-image is
  "absent" (`entry.kind = PreImageKind::Absent`, no blob). Restore **removes**
  the file.
- **Overwrite / edit**: the pre-image is the prior bytes; restore writes them
  back.
- **Delete / rename**: no tracked tool performs either, so deletes and renames
  done through `shell` are untracked (80-D2) and are not restored. If a future
  tool adds delete/rename, it must override `checkpoint_paths` to return both the
  source and destination; that is a future amendment, not designed here.
- **Symlinks / hard links**: the **unresolved** candidate path is checked with
  `symlink_status` and `hard_link_count` (80-D5); a symlink (or a leaf with
  `hard_link_count() > 1`) is captured as `PreImageKind::Symlink` with no content
  blob, and restore **skips** it (CC parity: "skips any tracked path that is a
  symlink or hard link").

### 80-D5 - Restore is root-confined and symlink-refusing

Every restore path is resolved with `ExecutionEnvironment::resolve()`
(`include/ymh/execution/environment.hpp:33`), so it is root-relative and
canonical, and an escaping path throws `ToolErrorCode::PathEscape`. But
`resolve()` canonicalizes an existing final path
(`src/execution/environment.cpp:79-84`), so checking `symlink_status()` on its
**result** always sees a regular file. The symlink refusal must therefore run on
the **unresolved** candidate, before `resolve()`: the store reconstructs
`candidate = workspace_root_ / stored-relative-path`, walks its components with a
new `first_symlink_component` helper (sec.5), and **skips** (counting `skipped`)
any path where a component is a symlink or the leaf's `hard_link_count() > 1`.
Only after that check does it call `resolve()` (for containment) and write.
Restore never `chdir()`s and never uses `getcwd()` as a base (`07 sec.6.3`).
Writes use the same root-respecting `LocalFilesystem::write`
(`include/ymh/execution/filesystem.hpp:64,76`), which re-verifies containment as
defense-in-depth against TOCTOU (`filesystem.hpp:3-6`).

### 80-D6 - Restore is authoritative; the report is per-file

Restore overwrites the current content with the checkpoint's pre-image (that is
the user's explicit request), but it is **best-effort per file**: an escaping
path, a symlink, a missing blob, a permission error, or a vanished parent
records a per-file failure and the batch continues. The result is a
`CheckpointRestoreReport` (sec.4.1). If zero files were restored and at least one
failed,
the UI surfaces CC's wording "No files were restored: N files failed (backup
missing, or the file could not be updated)" ([recon-claude-code.md sec.1.6]).
If a file's current `size`/`mtime` differs from the values captured in the
manifest, it is still restored but counted under `changed` and the notice
appends "(N files changed since the checkpoint)".

### 80-D7 - Bounded retention: 100 checkpoints/session, 30 days, with a contiguous window

Per session, keep the newest `kCheckpointKeepPerSession = 100` checkpoints by
ascending `TurnId`, mirroring CC's "100 most recent checkpoints". Independently,
drop any checkpoint whose `created_at_ms` is older than
`kCheckpointRetention = 30 days`, mirroring CC's default `cleanupPeriodDays`
([recon-claude-code.md sec.1.6]). Both rules delete from the **oldest end only**,
so the retained set of a session is always a **contiguous suffix by `TurnId`**:
there is an `oldest_retained_turn` (the smallest turn with a retained checkpoint)
such that every retained checkpoint has `turn >= oldest_retained_turn`, and no
evicted checkpoint has `turn >= oldest_retained_turn`. This contiguity is the
load-bearing property that keeps the restore-set semantics correct (HIGH-3):
because `/rewind`'s turn list comes from the event log and is independent of the
store (`src/ui/supervisor.cpp:1984-2015`), a listed turn `T` may predate the
window. For `T < oldest_retained_turn` the store **cannot** reconstruct the state
at the start of `T` (the captures of turns in `[T, oldest_retained_turn)` are
gone), so the code actions are **unavailable** rather than silently restoring a
later state:
- `revertible_count(session, T)` returns `0` whenever `T < oldest_retained_turn`
  (or the session has no retained checkpoint), which hides the code actions and
  makes `file_change_count == 0` (80-D8/80-D9);
- `restore(session, T)` defensively returns a report with `expired = true` and
  `detail = "checkpoint expired; code restore unavailable"`, restoring nothing.
The daemon never presents a later checkpoint as the target turn's state. Eviction
runs at three points: on capture (after appending), on `session.delete`
(`HostRuntime::deleteSession`, `src/host/host_runtime.cpp:918`), and at daemon
startup as a reconcile sweep (sec.9). The two constants are compile-time in Rev 1;
exposing them as JSONC keys is explicitly deferred (sec.15) to avoid touching the
config surface, and `0`/negative is not a valid value under that future revision.

### 80-D8 - One additive DTO field plus one new mutating method

The read-only `session.rewind_targets` result gains one additive field
`RewindTarget::file_change_count` (default 0): the number of **distinct file
paths** with a capture in any **retained** checkpoint whose turn is `>=` this
target's turn - i.e. the number of files that restoring to this checkpoint would
revert, and the exact signal CC uses to decide whether the code actions appear
([recon-claude-code.md sec.1.2]). It is `0` when the target is older than
`oldest_retained_turn` (80-D7), so an expired target never advertises a code
action. Adding a defaulted field is wire-compatible (older readers ignore it),
the same additive discipline as 78-D7. Code restore is one new method
`session.restore_code` with params `{session, turn}` (TurnId) and result
`protocol::RestoreReport`. No read-only method is added: the counts ride the existing
projection.

### 80-D9 - `/rewind` becomes a two-step full-screen menu (4 actions)

Step 1 preserves spec 79's turn list (checkpoint list) and its key handling.
Selecting a row opens **Step 2**, a new `UiMode::RewindAction` action menu with
exactly CC's code-relevant actions ([recon-claude-code.md sec.1.2]):

| # | Action | Effect |
|---|---|---|
| 1 | Restore code and conversation | branch the conversation (79) **and** restore code |
| 2 | Restore conversation | branch the conversation (79); working tree unchanged |
| 3 | Restore code | restore code; current conversation unchanged |
| 4 | Never mind | close; no RPC, no mutation |

Code actions (1, 3) appear **only** when the selected row's
`file_change_count > 0`; otherwise the menu shows only "Restore conversation"
and "Never mind", mirroring CC. This also covers an **expired** target (80-D7):
its count is 0, so no code action is offered, and if `restore` is invoked anyway
it returns `expired = true` and changes nothing (CP-F13). Numbers `1`-`4` select
directly; Up/Down/`k`/`j`
move; Enter confirms; Escape/Ctrl+C cancels (57-D4). CC's "Summarize from here /
up to here" are not mirrored (sec.15).

Both the list and the action menu render **full-screen** using spec 81's
base-selector pattern (`src/ui/ui_render.cpp:2395-2399`). This supersedes
79-D6's `clear_under` render-mode decision and 79-RW13's "never a full-screen
renderer" clause; it does not change key ownership or the conversation mechanism.
An overlay raised from a rewind screen (e.g. the exit confirm) still composes
over it because the base selector is consulted first.

### 80-D10 - Conversation restore is spec 79 unchanged; both is code-then-branch

"Restore conversation" and "Restore code and conversation" call spec 79's
`rewind_to` (`src/ui/supervisor.cpp:2070-2102`) unchanged: `session.fork` with
`seed_length = target.boundary_index`, focus the child, restore the prompt. This
spec adds no fork semantics and does not redefine `boundary_index` (79-D2/RW3).
"Both" issues `session.restore_code` for `(parent, turn)` **and** the spec 79
fork; the two are independent - the fork creates a session row and touches no
working-tree file, and code restore touches no session row - so ordering is not
load-bearing. The code result is surfaced as its own notice; a partial code
restore does not roll back the conversation branch, and vice versa.

### 80-D11 - Code restore is refused while a turn is running

Code restore rewrites the working tree, which a running turn may be mutating
concurrently. Before issuing `session.restore_code`, the supervisor checks the
focused session's agent state and refuses with a notice when it is not idle
(mirroring `HostRuntime::deleteSession`'s mid-turn refusal,
`src/host/host_runtime.cpp:919-940`). The daemon re-checks on its side (a capture
and a restore both take the store's `mutex`, and capture holds it only for the
duration of one file read), so a race cannot corrupt the index (sec.6/CP8).

### 80-D12 - No schema change, no ownership change, no live-vs-stored change

No SQLite column or table changes (`session_snapshots` stays unused). The daemon
remains supervisor-owned (spec 16); the checkpoint store lives and dies with the
daemon process. The Ctrl+S live surface and the `/sessions` stored catalog are
untouched (22 sec.3/sec.4; 81-D10). `/rewind` continues to list only the focused
session's turns.

### 80-D13 - Capture is best-effort and never fails a tool call

A capture failure (read error, disk full, race with an external writer) must not
fail the tool call that triggered it (`07` preserves pure tool execution). The
store records the affected checkpoint as `incomplete` and the file entry as
`PreImageKind::Failed` (`entry.kind = Failed`); the action menu still offers code
restore but the notice warns that the restore is partial. This trades silent data
loss for an explicit, visible marker.

---

## 4. Interface sketches

### 4.1 `CheckpointStore` (new) and its recorder interface

New header `include/ymh/session/checkpoints.hpp` (impl
`src/session/checkpoints.cpp`; owned by the daemon, sec.6/9):

```cpp
namespace ymh {

// 80-D1: one captured file's PRE-state within a checkpoint.
enum class PreImageKind : std::uint8_t {
    Content,   // bytes captured in `blob` (content address)
    Absent,    // the path did not exist (write_file create) -> restore removes it
    Symlink,   // the path was a symlink -> restore skips it (80-D5)
    Failed,    // capture failed (80-D13)
};

struct CheckpointFile {
    std::string   path;        // workspace-relative, as resolved by resolve()
    PreImageKind  kind = PreImageKind::Content;
    std::string   blob;        // sha256 hex id when kind == Content
    std::int64_t  size{0};     // bytes at capture time (0 when Absent/Symlink)
    std::int64_t  mtime_ms{0}; // mtime at capture time (0 when Absent/Symlink)
};

struct Checkpoint {
    SessionId                 session;
    TurnId                    turn{0};
    std::int64_t              created_at_ms{0};
    bool                      incomplete{false};
    std::vector<CheckpointFile> files;
};

// Per-file outcome of a restore (80-D6): the SESSION-LAYER domain type. This is
// the only declaration of the domain report (NEW-M3). The transport DTO
// `protocol::RestoreReport` (sec.4.3) is a DISTINCT type; the host layer converts
// one to the other in `HostRuntime::restoreCode` (sec.4.4), so checkpoints.hpp
// does NOT include transport/protocol.hpp.
struct CheckpointRestoreReport {
    std::int64_t restored{0};
    std::int64_t skipped{0};
    std::int64_t failed{0};
    std::int64_t changed{0};   // restored, but modified since capture
    bool         expired{false}; // 80-D7: target older than the retained window
    std::string  detail;       // human summary (CC-shaped when all failed)
};

// NEW-M3: the store's index I/O error (CP-F14) as a session-layer domain error.
// `HostRuntime::restoreCode` maps it to `protocol::AppCode::CheckpointUnavailable`
// via `translate`; the store never names a `protocol::` type. A corrupt/
// unknown-version index is NOT this error (CP-F5: it loads as empty).
struct CheckpointUnavailableError : std::runtime_error {
    explicit CheckpointUnavailableError(std::string what)
        : std::runtime_error(std::move(what)) {}
};

// 80-D2: implemented by the daemon; injected into the ToolRegistry. The
// `ToolContext` supplies session/turn/environment, so the recorder needs none.
class CheckpointRecorder {
public:
    virtual ~CheckpointRecorder() = default;
    // Captures the PRE-state of `paths` before the tool mutates them. Idempotent
    // per (session, turn, path): the first capture wins (80-D1). Never throws.
    virtual void capture(const SessionId& session, TurnId turn,
                         const std::vector<std::filesystem::path>& paths,
                         const ExecutionEnvironment& env) = 0;
};

// 80-D3: blob id = lowercase-hex sha256 of the raw bytes. Caller: the
// CheckpointStore constructor uses it as the default hasher; tests call it
// directly (tests/unit/checkpoints_test.cpp).
[[nodiscard]] std::string checkpoint_blob_id(std::string_view bytes);

// 80-D5: the first symlink component of the UNRESOLVED `candidate`, or a leaf
// whose hard_link_count() > 1, or nullopt. Called by capture and restore BEFORE
// resolve() (which would otherwise canonicalize a symlink away). Body in sec.5.
[[nodiscard]] std::optional<std::filesystem::path>
first_symlink_component(const std::filesystem::path& candidate);

// 80-D1: `path`'s workspace-relative form, used as the manifest key. The in-tree
// `relative_string` (src/tools/builtin_tools.cpp:51) takes a ToolContext and is
// not reusable here. Called by capture. Body in sec.5.
[[nodiscard]] std::string workspace_relative(const std::filesystem::path& workspace_root,
                                             const std::filesystem::path& path);

class CheckpointStore final : public CheckpointRecorder {
public:
    // 80-D1: `workspace_root` is the canonical workspace root (the value
    // `ExecutionEnvironment::root()` returns). The store derives its own
    // `store_dir_ = workspace_root / ".ymh" / "checkpoints"` in the constructor;
    // one argument, one meaning (H1). `hash` defaults to `checkpoint_blob_id`
    // (injected for tests).
    explicit CheckpointStore(std::filesystem::path workspace_root,
                             std::function<std::string(std::string_view)> hash = {});

    // CheckpointRecorder:
    void capture(const SessionId&, TurnId,
                 const std::vector<std::filesystem::path>&,
                 const ExecutionEnvironment&) override;

    // 80-D7/80-D8: distinct retained paths capturable at turns >= `turn`; 0 when
    // `turn` is older than the retained window (expired) or nothing is retained.
    // NEW-M5: this is a READ projection consumed by spec 79's frozen
    // `session.rewind_targets`; it must never fail that method. An index I/O
    // error (CP-F14) is caught here and degraded to 0 (same hiding as CP-F5),
    // logged; conversation rewind then still opens with no code actions. Only the
    // mutating `restore` surfaces the error.
    [[nodiscard]] std::int64_t revertible_count(const SessionId&, TurnId) const;

    // 80-D5/D6/D7: root-confined, symlink-refusing restore. Never throws for a
    // per-file problem; returns the domain report. Throws
    // `CheckpointUnavailableError` only for an I/O error opening/reading the
    // index; a corrupt or unknown-version index loads as empty and is quarantined
    // (CP-F5). Returns `expired = true` when `turn` predates the retained window
    // (80-D7). NEW-M4: the env is non-const because restore calls the non-const
    // `ExecutionEnvironment::fs()` (`environment.hpp:39`) to write/remove.
    CheckpointRestoreReport restore(ExecutionEnvironment&, const SessionId&, TurnId);

    // 80-D7/80-D12: drop a session's manifests and GC its blobs.
    void removeSession(const SessionId&);
    // 80-D7: count/age eviction + reconcile against existing sessions + blob GC.
    // `live_sessions` is the session set from the workspace store
    // (`SessionPersistence::list()`, include/ymh/session/session_persistence.hpp:82);
    // a session present on disk but not resident is still "live" for reconcile.
    // NEW-M6: this is a maintenance pass run at daemon startup; it NEVER throws.
    // An index I/O error (CP-F14) is caught here, logged, the reconcile/GC pass
    // is skipped, and `dirty_` is left set for a later retry - a checkpoint-subsystem
    // fault must never fail `WorkspaceHost::Impl::startup` (spec 76 hardened it).
    void sweep(const std::set<SessionId>& live_sessions,
               std::chrono::system_clock::time_point now);

private:
    // NEW-L5: the lazy loader is `const` so the read-only `revertible_count`
    // can trigger it. Caller: `capture`, `revertible_count`, `restore`, `sweep`.
    // It is a no-op after the first call. An I/O error throws
    // `CheckpointUnavailableError`; a corrupt/unknown-version index quarantines
    // and loads empty (CP-F5/CP-F14).
    void ensure_loaded_locked() const;
    // 80-D7: the smallest retained turn for `session` (0 when none).
    [[nodiscard]] TurnId oldest_retained_turn_locked(const std::string& session) const;
    // 80-D1: the checkpoint at `turn`, or nullopt.
    [[nodiscard]] std::optional<Checkpoint> find_locked(const std::string& session,
                                                        TurnId turn) const;
    // 80-D7: drop from the oldest end so the retained window stays contiguous.
    void evict_locked(const std::string& session,
                      std::chrono::system_clock::time_point now);

    std::filesystem::path workspace_root_;   // H1: containment + key base
    std::filesystem::path store_dir_;        // H1: workspace_root_/".ymh"/"checkpoints"
    std::function<std::string(std::string_view)> hash_;
    mutable std::mutex mutex_;               // 80-D11/CP8: serializes
    // NEW-L5: `sessions_`/`loaded_`/`dirty_` are `mutable` so the const
    // `revertible_count` can lazily load under `mutex_` (ensure_loaded_locked).
    mutable std::map<std::string, std::vector<Checkpoint>> sessions_;  // in-memory index
    mutable bool loaded_ = false;
    mutable bool dirty_ = false;   // M6: persist failed; retried on next capture/sweep
};

} // namespace ymh
```

Concrete callers (one per new symbol): `capture` is called by
`ToolRegistry::execute` (sec.4.2); `revertible_count` is called by
`HostRuntime::rewindTargets` (sec.4.5); `restore` is called by
`HostRuntime::restoreCode` (sec.4.5); `removeSession` is called by
`HostRuntime::deleteSession` (`src/host/host_runtime.cpp:918`); `sweep` is called
by `WorkspaceHost::Impl::startup` (`src/host/workspace_host.cpp:774`);
`checkpoint_blob_id` is called by the store constructor and by
`tests/unit/checkpoints_test.cpp`; `first_symlink_component` and
`workspace_relative` are called by `capture` and `restore` (sec.5/7). The
`oldest_retained_turn_locked`/`find_locked`/`evict_locked` privates are called by
`revertible_count`/`capture`/`restore`/`sweep`, and `ensure_loaded_locked` (NEW-L5)
is called by `capture`, `revertible_count`, `restore`, and `sweep`. Of those,
`capture` catches everything (80-D13), `revertible_count` catches and returns 0
(NEW-M5), `sweep` catches and skips the pass (NEW-M6), and only `restore` lets
`CheckpointUnavailableError` escape.

### 4.2 `Tool::checkpoint_paths` (new additive virtual) and the capture hook

```cpp
// include/ymh/tools/tool.hpp, in `class Tool` after `version()` (:70).
// 80-D2/80-D5: the workspace paths this call will mutate that the checkpoint
// store should snapshot BEFORE `execute` runs. The default is empty (read-only
// and untracked tools). A path is returned UNRESOLVED - the raw `path` argument
// - so the store can run its non-following symlink check before resolve()
// canonicalizes the leaf away (H2/80-D5).
[[nodiscard]] virtual std::vector<std::filesystem::path>
checkpoint_paths(const ToolContext&, const ToolArguments&) const { return {}; }
```

Override in `WriteFileTool` and `EditFileTool` (`src/tools/builtin_tools.cpp`):

```cpp
[[nodiscard]] std::vector<std::filesystem::path>
checkpoint_paths(const ToolContext& /*context*/,
                 const ToolArguments& arguments) const override {
    // `path` is required in both schemas (src/tools/builtin_tools.cpp:158,193-194).
    // Return it raw; the store resolves it (and checks symlinks) itself.
    return {std::filesystem::path{arguments.value.at("path").get<std::string>()}};
}
```

Hook in `ToolRegistry::execute` (`src/tools/tool_registry.cpp:321`), immediately
before `tool->execute(...)` at `:347`:

```cpp
if (checkpoint_recorder_ != nullptr) {
    checkpoint_recorder_->capture(context.sessionId(), context.turn(),
                                  tool->checkpoint_paths(context, arguments),
                                  context.execution());
}
```

`ToolRegistry` gains `void set_checkpoint_recorder(CheckpointRecorder*)`
(called once at daemon startup; sec.6). The recorder pointer is non-owning; the
daemon owns the `CheckpointStore`.

### 4.3 Wire DTOs and the new method

Additive field - `include/ymh/transport/protocol.hpp` in `RewindTarget`
(`:412-418`):

```cpp
struct RewindTarget {
    SessionId     session;
    std::uint64_t turn{0};
    std::int64_t  boundary_index{0};
    std::int64_t  started_at_ms{0};
    std::string   prompt;
    // 80-D8: distinct files a restore to this checkpoint would revert; 0 hides
    // the code actions (defaulted => wire-compatible).
    std::int64_t  file_change_count{0};
};
```

NEW-L10: the existing `RewindTarget` serializers
`void to_json(nlohmann::json&, const RewindTarget&)` and
`void from_json(const nlohmann::json&, RewindTarget&)`
(`src/transport/protocol.cpp:637-651`) each gain one line for
`file_change_count` (`json["file_change_count"] = ...` / `value("file_change_count", 0)`);
`from_json`'s default 0 preserves wire compatibility for an older sender. The
round-trip is asserted by CP-U15.

New DTOs (after `RewindTargets`, `:420-423`), with `to_json`/`from_json` defined
in `src/transport/protocol.cpp` beside `SessionDetail`:

```cpp
// 80-D6/D7: the WIRE result of a code restore (namespace ymh::protocol; the
// comment at protocol.hpp:31 pins the namespace). This is the transport DTO; the
// session-layer domain type is `ymh::CheckpointRestoreReport` (sec.4.1). They are
// distinct types, bridged field-by-field in `HostRuntime::restoreCode` (sec.4.4)
// so `session/checkpoints.hpp` need not include this header (NEW-M3).
struct RestoreReport {
    std::int64_t restored{0};
    std::int64_t skipped{0};
    std::int64_t failed{0};
    std::int64_t changed{0};
    bool         expired{false};   // 80-D7: target older than the retained window
    std::string  detail;
};
```

Method constant - `include/ymh/transport/protocol.hpp` near `:554`:

```cpp
inline constexpr std::string_view kSessionRestoreCode = "session.restore_code";
```

Wire binding:

```text
session.restore_code  params: { session, turn }   result: { restored, skipped,
                                                              failed, changed,
                                                              expired, detail }
```

The method is added to `kMethodCatalog` (`src/transport/protocol.cpp:665-687`),
making that code catalog 40 -> 41 entries, and adds one row to the 05 sec.7.4
session-lifecycle method table (`docs/design/05-transport.md:887-898`, 8 -> 9
rows). One new app code
`AppCode::CheckpointUnavailable = -32022` (next free after `EndpointNotRouted`
`-32021`, `:207`) is returned only when the index **cannot be opened or read**
(an I/O error); it is the wire mapping of the session-layer
`CheckpointUnavailableError` (sec.4.1), pinned in sec.4.4. A corrupt file or an
unknown `version` loads as empty (CP-F5), not thrown. A missing per-file blob is
a **per-file failure**, not an RPC error (80-D6).

### 4.4 `TransportHost::restoreCode` (new base virtual + daemon/fake)

Base pure virtual - `include/ymh/transport/host.hpp` after `rewindTargets`
(`:116`):

```cpp
// 80-D8: restore the working tree to the checkpoint at `turn`. Mutates files
// under the workspace root only; never touches the event log. Returns the
// per-file report (80-D6).
virtual protocol::RestoreReport restoreCode(const SessionId& id, TurnId turn) = 0;
```

Implementers, both updated in the same change set or the tree does not build
(the same enumeration discipline as 79 sec.4.2): `HostRuntime`
(`include/ymh/host/host_runtime.hpp:175`; production) and `FakeTransportHost`
(`tests/support/fake_transport_host.hpp:22`).

Daemon override (`src/host/host_runtime.cpp`, beside `rewindTargets` at `:869`):

```cpp
protocol::RestoreReport HostRuntime::restoreCode(const SessionId& id, TurnId turn) {
    return translate([&]() -> protocol::RestoreReport {
        if (!runtime_.store().load(id).has_value()) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        // 80-D11: a running turn must not race the restore. This is the same
        // predicate `deleteSession` uses (an explicit AgentState switch at
        // src/host/host_runtime.cpp:929-937); there is no `is_running` helper.
        const std::shared_ptr<AgentLoop> agent = runtime_.agents().findShared(id);
        if (agent != nullptr) {
            switch (agent->state()) {
                case AgentState::Thinking:
                case AgentState::CallingTool:
                case AgentState::WaitingForPermission:
                case AgentState::WaitingForInput:
                case AgentState::Cancelling:
                    throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                                           "turn in progress"});
                case AgentState::Idle:
                case AgentState::Error:
                    break;
            }
        }
        if (checkpoints_ == nullptr) {          // M1: never wired
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::StoreUnavailable),
                                   "StoreUnavailable"});
        }
        // 80-D5/D6/D7: root-confined, symlink-refusing, expiry-aware. The store
        // returns the SESSION-LAYER domain type; convert field-by-field to the
        // wire DTO here (NEW-M3) so session/checkpoints.hpp never includes
        // transport/protocol.hpp.
        const CheckpointRestoreReport r =
            checkpoints_->restore(runtime_.environment(), id, turn);
        protocol::RestoreReport out;
        out.restored = r.restored;
        out.skipped  = r.skipped;
        out.failed   = r.failed;
        out.changed  = r.changed;
        out.expired  = r.expired;
        out.detail   = r.detail;
        return out;
    });
}
```

`translate` also maps the session-layer `CheckpointUnavailableError` (thrown by
`CheckpointStore::restore` for the index I/O error, CP-F14) to
`protocol::AppCode::CheckpointUnavailable` (`-32022`); that domain-error ->
`AppCode` row is added beside the existing mapping table (05 sec.7.1), so the
store never names a `protocol::` symbol.

### 4.5 `HostRuntime::rewindTargets` extension, store injection, and startup wiring

**Ownership and injection (H1/M1).** `CheckpointStore` is owned by
`WorkspaceHost::Impl` as `std::unique_ptr<CheckpointStore> checkpoints_;`,
**declared before** `runtime_` (`src/host/workspace_host.cpp:687`) and
`host_runtime_` (`:692`) so that C++'s reverse-order member destruction keeps the
store alive until after both dependents are destroyed - their destructors/handles
(`runtime_->tools()`, `HostRuntime::checkpoints_`) may still reference it during
teardown (NEW-L3). `HostRuntime` holds only a non-owning
`CheckpointStore* checkpoints_ = nullptr;`, set by the daemon in `startup()`; both
declarations are shown in sec.3/4.1-4.4. Add to
`include/ymh/host/host_runtime.hpp` (beside `attachServer`, `:130`) with a
forward declaration `class CheckpointStore;`:

```cpp
// 80/M1: non-owning; the daemon owns the store. Binary-compatible setter for
// the two-phase construction. Wired once in WorkspaceHost::Impl::startup.
void set_checkpoint_store(CheckpointStore* store) noexcept { checkpoints_ = store; }
```

`HostRuntime::rewindTargets` (`src/host/host_runtime.cpp:869-904`) fills the new
field per target from the store (the loop already computes `started.turn`); when
`checkpoints_ == nullptr` (unit tests that do not wire a store) the field stays 0.
NEW-M5: this call can never fail `session.rewind_targets` - `revertible_count` is
a read projection that degrades any index I/O error to 0 (sec.4.1), so spec 79's
turn list is returned unchanged even when the checkpoint store is unreadable
(consistent with 80-D3/80-D10/80-D12/CP12):

```cpp
target.file_change_count = checkpoints_ == nullptr
    ? 0
    : checkpoints_->revertible_count(id, TurnId{started.turn});   // 80-D8
```

Startup wiring - `WorkspaceHost::Impl::startup` (`src/host/workspace_host.cpp:774`,
after the `.ymh` dir exists at `:1405`). `runtime_` is the `WorkspaceRuntime`; the
live session set comes from its store (`SessionPersistence::list()` returns
`std::vector<SessionHeader>`, `include/ymh/session/session_persistence.hpp:82`),
NOT an undefined `live_session_ids()`:

```cpp
checkpoints_ = std::make_unique<CheckpointStore>(canonical_root_);   // H1: workspace root
std::set<SessionId> live;
for (const SessionHeader& header : runtime_->store().list()) {
    live.insert(header.id);
}
checkpoints_->sweep(live, std::chrono::system_clock::now());         // 80-D7
runtime_->tools().set_checkpoint_recorder(checkpoints_.get());       // 80-D2
host_runtime_->set_checkpoint_store(checkpoints_.get());             // M1
```

NEW-M6: `sweep` is non-throwing by contract (sec.4.1) - an unreadable
`index.json` is logged, the reconcile/GC pass is skipped, `dirty_` stays set, and
`startup()` proceeds to `Serving`. The checkpoint subsystem never contributes a
failure to `WorkspaceHost::Impl::startup` (`src/host/workspace_host.cpp:774`,
wrapped by the `run()` catch at `:548-552`); this preserves spec 76's hardened
startup.

`HostRuntime::deleteSession` (`src/host/host_runtime.cpp:973`, after the manager
delete) calls `if (checkpoints_ != nullptr) { checkpoints_->removeSession(id); }`
(80-D7/80-D12).

### 4.6 UI state: `RewindTargetView`, `RewindActionModel`, `UiMode::RewindAction`

Extend `RewindTargetView` (`include/ymh/ui/ui_model.hpp:600-606`) with
`std::int64_t file_change_count{0};` and add the action model:

```cpp
// 80-D9: the second step of `/rewind`. `actions` is the (gated) action list.
enum class RewindAction : std::uint8_t {
    RestoreCodeAndConversation,
    RestoreConversation,
    RestoreCode,
    Cancel,
};

struct RewindActionModel {
    bool                     open = false;
    WorkspaceId              workspace;
    SessionId                session;
    TurnId                   turn{0};
    std::int64_t             file_change_count{0};   // 80-D9 gating
    std::vector<RewindAction> actions;               // built in ascending order
    std::size_t              cursor = 0;
    UiMode                   prev_mode = UiMode::Rewind;

    void open_with(WorkspaceId ws, SessionId session, TurnId turn,
                   std::int64_t file_change_count);
    void close();
    void moveUp();
    void moveDown();
    [[nodiscard]] const RewindAction* selected() const;
};
```

`UiModel` gains `RewindActionModel rewind_action;` beside `rewind`
(`include/ymh/ui/ui_model.hpp:812-813`). New `UiMode::RewindAction`
(`include/ymh/ui/ui_event.hpp:57`) beside `Rewind`.

### 4.7 `SupervisorApp` methods (new)

```cpp
// src/ui/supervisor.cpp, wired at dispatch_command beside context.rewind (:3227).
void open_rewind_action(const RewindTargetView& target);   // 80-D9 step 2
bool handle_rewind_action(const ftxui::Event& event);      // chain A
void apply_rewind_action(RewindAction action);             // 80-D10 dispatch
void restore_code(const WorkspaceId&, const SessionId&, TurnId); // 80-D8
void apply_restore_report(const WorkspaceId&, const SessionId&,
                          protocol::RestoreReport report); // 80-D6 notice
std::optional<std::pair<SessionId, TurnId>> restore_query_; // stale-reply guard
```

`rewind_to` (`:2070`) is split: Enter on the list now calls `open_rewind_action`
instead of forking immediately; only the action steps call the spec 79 fork path
(`apply_rewind_success`, `:2104`) and/or `restore_code`. The spec 79 body of
`rewind_to` (the fork submit) is preserved verbatim inside the
"Restore conversation" branch (80-D10).

### 4.8 Renderer

Two full-screen renderers, selected by the base selector (80-D9):
`render_rewind(model, theme, size.width)` (the existing overlay renderer at
`src/ui/ui_render.cpp:1584`, whose real signature is
`Element render_rewind(const UiModel&, const Theme&, int available_width)`; it is
promoted from overlay to full-screen) and `render_rewind_action(model, theme,
size.width)` (new, same signature shape). The `dbox` Rewind branch at
`src/ui/ui_render.cpp:2414-2416` is replaced by a base-selector branch (sec.7).

---

## 5. The checkpoint capture algorithm (normative)

This section is the exact algorithm required by 80-D1/80-D2/80-D4/80-D5. Inputs
to `CheckpointStore::capture(session, turn, paths, env)`, where `paths` are the
**unresolved** raw paths returned by `checkpoint_paths` (sec.4.2) and
`workspace_root_`/`store_dir_` are the two fields pinned in sec.4.1 (H1).

**Step 1 - early out.** If `paths` is empty, return. If `turn == 0`, return
(no turn can hold a checkpoint).

**Step 2 - lock.** Acquire `mutex_` (80-D11/CP8), then `ensure_loaded_locked()`,
which lazily loads `index.json` on first use and is a no-op afterwards (NEW-L5;
`const`, so `revertible_count` can call it too). See sec.6 for the failure
classes it applies (corrupt -> empty/quarantine; I/O error -> throw).

**Step 3 - per raw `path`, idempotent.**
1. Reject if `path` is empty.
2. Build the **unresolved candidate** exactly as `resolve()` would join it:
   `candidate = path.is_absolute() ? path : workspace_root_ / path` (mirrors
   `src/execution/environment.cpp:66-67`). Do NOT resolve yet.
3. **Non-following symlink check (H2/80-D5):**
   `if (auto bad = first_symlink_component(candidate)) { entry = Symlink; continue; }`.
   `first_symlink_component` (body below) walks the candidate's components with
   `symlink_status` and tests the leaf's `hard_link_count`; this runs *before*
   `resolve()`, which would otherwise canonicalize a symlink leaf away.
4. `resolved = env.resolve(path)`; on `ToolErrorCode::PathEscape` record
   `PreImageKind::Failed` and continue (80-D5).
5. `key = workspace_relative(workspace_root_, resolved)`. If `(session, turn)`
   already has an entry for `key`, **skip** (first capture wins, 80-D1).
6. `std::error_code ec; auto status = std::filesystem::symlink_status(resolved, ec);`
   - `!exists(status)` -> entry `Absent` (a `write_file` create).
   - otherwise -> read the bytes (bounded by the file read limit; if the read
     fails, entry `Failed`, mark checkpoint `incomplete`, continue - 80-D13).
     Compute `blob = hash_(bytes)`; if `store_dir_/blobs/<aa>/<blob>` does not
     exist, write the blob atomically (sec.6). Store `size`, `mtime_ms`.
7. Append the entry to the `(session, turn)` checkpoint; create the checkpoint
   if absent, with `created_at_ms = now`. No resolved-view position is stored:
   `capture` has no event-log access (only `SessionId`/`TurnId`/paths/`env`), and
   the UI's `boundary_index` comes from `session.rewind_targets`, not from the
   store (NEW-M1).

**Step 4 - persist.** Serialize the index and persist it atomically under
`store_dir_` (sec.6).

**Step 5 - evict.** Run the count/age eviction for this session (80-D7),
preserving the contiguous-window property.

**Exceptions.** `capture` catches everything (a tool call must not fail;
80-D13). A failed index persist leaves the in-memory entry but logs and sets
`dirty_ = true` for retry on the next capture/sweep.

**Helper bodies (new symbols of sec.4.1).**

```cpp
std::string workspace_relative(const std::filesystem::path& root,
                               const std::filesystem::path& path) {
    return path.lexically_relative(root).generic_string();   // manifest key
}

std::optional<std::filesystem::path>
first_symlink_component(const std::filesystem::path& candidate) {
    std::filesystem::path prefix = candidate.root_path();
    for (const auto& component : candidate.relative_path()) {
        prefix /= component;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(prefix, ec);
        if (!ec && std::filesystem::is_symlink(status)) {
            return prefix;                                   // symlink component
        }
    }
    std::error_code ec;
    if (!ec && std::filesystem::hard_link_count(candidate, ec) > 1) {
        return candidate;                                    // hard-linked leaf
    }
    return std::nullopt;
}
```

---

## 6. On-disk layout, index format, and atomic-write protocol (normative)

### 6.1 Layout

The root of the layout is the store's `store_dir_`, defined in the constructor
as `workspace_root_ / ".ymh" / "checkpoints"` (H1/sec.4.1); every path below is
relative to it.

```text
<ws>/.ymh/checkpoints/
  index.json                      # 80-D3: the manifest; versioned JSON
  index.json.tmp.<pid>            # transient atomic-write target
  blobs/
    <aa>/<sha256>                 # 80-D3: pre-image bytes, content-addressed
    <aa>/<sha256>.tmp.<pid>       # transient blob write target
```

`<aa>` is the first two hex characters of the blob id, to bound directory
entries. Nothing outside `<ws>/.ymh/checkpoints/` is written. The directory is
created by the daemon with `create_directories` (`src/host/workspace_host.cpp:1405`
already creates `.ymh`; the store creates `checkpoints/`).

### 6.2 `index.json` format

```json
{
  "version": 1,
  "sessions": {
    "<session-id>": {
      "checkpoints": [
        {
          "turn": 3,
          "created_at_ms": 1728000000000,
          "incomplete": false,
          "files": [
            {"path": "src/a.cpp", "kind": "content",
             "blob": "<sha256hex>", "size": 120, "mtime_ms": 1727999999000},
            {"path": "src/new.cpp", "kind": "absent", "size": 0, "mtime_ms": 0},
            {"path": "link.h", "kind": "symlink", "size": 0, "mtime_ms": 0}
          ]
        }
      ]
    }
  }
}
```

`kind` is one of `content|absent|symlink|failed`. `version` gates future
migrations. Two distinct index-failure classes are pinned (NEW-M2), and the
whole spec uses this vocabulary:
- **corrupt / unknown version** (unparseable JSON, a schema mismatch, or a
  `version` this build does not know): the store **loads as empty** (no throw),
  `revertible_count` returns 0, and startup `sweep` quarantines the file to
  `index.json.corrupt.<ts>` (CP-F5).
- **unreadable (I/O error)** (the file cannot be opened or read, e.g. EACCES or
  an I/O fault): the mutating `restore` **throws** the session-layer
  `CheckpointUnavailableError` (sec.4.1), which `HostRuntime::restoreCode` maps to
  `AppCode::CheckpointUnavailable` (`-32022`); the store names no `protocol::`
  symbol (NEW-M3/CP-F14). The READ projection `revertible_count` instead catches
  it and returns 0 (NEW-M5), so spec 79's `session.rewind_targets` - and the turn
  list - still succeeds with no code actions. The startup `sweep` also catches it
  (logs, skips the reconcile/GC pass, leaves `dirty_` set), so daemon startup is
  never failed (NEW-M6); only the mutating `restore` surfaces the error.
A parse failure is classified as corrupt (first bullet), never as an I/O error.

### 6.3 Atomic-write protocol (existing JSON-state precedents)

The discipline is the one already used by the workspace's JSON state, NOT
`02-persistence.md` sec.6 (which is the SQLite flush/snapshot policy): config
import (`src/cli/cli.cpp:824-884`), the grant store
(`src/config/grant_store.cpp:242-278`), and workspace trust
(`src/skills/workspace_trust.cpp:97-107`).

**Blob write.** Open `store_dir_/blobs/<aa>/<sha>.tmp.<pid>` with
`O_CREAT|O_EXCL`, write all bytes, `fsync(fd)`, `close`, `rename` to
`blobs/<aa>/<sha>`; if the rename target already exists (a dedup race), `unlink`
the temp and keep the existing blob. Call the promoted
`ymh::fsync_parent_directory` (sec.4.1/Amends; now
`include/ymh/core/fs.hpp`, `src/core/fs.cpp`) on `blobs/<aa>`. This is the
config-import idiom (`src/cli/cli.cpp:824-884`) applied to blobs.

**Index write.** Serialize to `index.json.tmp.<pid>`, `fsync`, `rename` over
`index.json`, `fsync` the parent directory (the grant-store idiom,
`src/config/grant_store.cpp:242-278`). A crash before the rename leaves the old
`index.json` intact and only a stale temp file, which `sweep` removes. A crash
after a blob rename but before the index rename leaves an **orphan blob** (valid,
unreferenced); `sweep`'s blob GC reclaims it. The index is therefore always
self-consistent and never half-written (CP9).

**Durability window (LOW-7).** That same crash window means the **checkpoint
record is lost** even though the blob survives: the index is persisted after the
blobs, so a crash between the two loses the capture for that turn (an orphan
blob, no manifest entry). Capture is therefore **best-effort durability**, not
atomic-commit: the guarantee is "whatever is in `index.json` is internally
consistent and restorable", not "every read that returned to the tool survives a
crash". The tool call always proceeds; the lost checkpoint simply does not appear
(and `file_change_count` for that turn is lower). Reversing the order (persist
the index first) is not used because it would reference blobs that may not exist,
trading a lost record for an unrestorable one.

**Single writer.** The daemon holds the workspace's single-writer position
(spec 16; `session_leases`), so there is exactly one `CheckpointStore` writer per
workspace. `mutex_` orders capture/restore/evict within the process. No flock is
needed for the index because no other process writes it.

### 6.4 Size bound

A per-file pre-image is capped by the existing `ToolConfig::read_file_max_bytes`
(`include/ymh/execution/config.hpp:20`); a larger file is captured as
`Failed` with a notice rather than silently truncated (a truncated pre-image
would restore corrupted bytes). Blob GC keeps total size proportional to the
retained checkpoint window.

---

## 7. The restore algorithm (normative)

`CheckpointStore::restore(env, session, turn)`:

1. Load the index. A **corrupt / unknown-version** file is quarantined and
   treated as empty (no throw, CP-F5); an **I/O error** opening/reading the file
   throws the session-layer `CheckpointUnavailableError`, mapped by
   `translate` to `AppCode::CheckpointUnavailable` (CP-F14).
2. **Expiry guard (H3/80-D7).** Let `oldest = oldest_retained_turn_locked(session)`.
   - If the session has **no** retained checkpoint at all, return a report with
     all counts zero and `detail = "nothing to restore"` - there is nothing to
     revert and nothing to mislead about.
   - If `turn < oldest`, the captures needed to reconstruct the state at the
     start of `turn` are gone (they were evicted from the oldest end). Return
     `CheckpointRestoreReport{expired = true, detail = "checkpoint expired; code restore
     unavailable"}` and **touch no file**. The daemon never substitutes a later
     checkpoint (this is the HIGH-3 fix; the same condition makes
     `revertible_count` return 0 so the UI hides the code actions).
3. Build the **restore set** from the **retained** checkpoints only: for each
   path `P` appearing in any checkpoint of `session` with checkpoint
   `turn' >= turn`, take the entry from the **smallest** `turn'` (the earliest
   capture at or after the target). Because the retained window is a contiguous
   suffix (80-D7), every capture in `[turn, oldest)` is present, so this is the
   state of `P` at the start of `turn` (80-D1). Paths captured only before
   `turn` are untouched.
4. For each `(P, entry)`, in a stable path order:
   - `candidate = workspace_root_ / P`; `first_symlink_component(candidate)` ->
     if a component is a symlink or the leaf is hard-linked, count `skipped` and
     continue (H2/80-D5; checked **before** `resolve()`, which would canonicalize
     the leaf away).
   - `resolved = env.resolve(P)` (80-D5); on `ToolErrorCode::PathEscape`, count
     `failed` and continue.
   - `entry.kind == Absent`: `env.fs().remove(resolved)` (ignore ENOENT ->
     `restored` if it was already gone, else `failed`).
   - `entry.kind == Failed` or missing blob: count `failed` and continue (CP-F4).
   - `entry.kind == Symlink`: count `skipped`.
   - else read `store_dir_/blobs/<aa>/<blob>`; on a missing/unreadable blob,
     `failed` (CP-F4). Compare the current `size`/`mtime_ms` with the entry; if
     different, increment `changed` (still restore, 80-D6). Write via
     `env.fs().write(resolved, Data{bytes})` (root-re-verified,
     `filesystem.hpp:3-6`); `restored`.
5. Build `detail`: when `restored == 0 && failed > 0`, the CC-shaped sentence
   "No files were restored: N files failed (backup missing, or the file could
   not be updated)"; when `changed > 0`, append "(N files changed since the
   checkpoint)".
6. Return the `CheckpointRestoreReport`. Per-file failures never abort (80-D6); only an
   index **I/O error** throws `CheckpointUnavailableError` (mapped to
   `AppCode::CheckpointUnavailable`; a corrupt/unknown-version
   index loads empty, CP-F5).

The restore never appends an event, never mutates a header, never acquires a
lease, and never touches the conversation (CP7). A "nothing to restore" case
(step 2, first bullet; or a within-window target whose restore set is empty)
returns a report with all counts zero and `detail = "nothing to restore"`; the
UI surfaces that notice rather than an error, distinct from the expired case.

---

## 8. `/rewind` menu: modes, keys, precedence

**Step 1 (list).** `UiMode::Rewind`, `RewindOverlayModel` (79 sec.4.4). Keys
unchanged (ArrowUp/Down/`k`/`j`, Enter, Escape/Ctrl+C). Enter now opens Step 2
(`open_rewind_action`) instead of forking. Full-screen render (80-D9).

**Step 2 (actions).** `UiMode::RewindAction`, `RewindActionModel` (sec.4.6).
Keys: ArrowUp/Down/`k`/`j` move; `1`-`4` select directly (numbering per the
action table in 80-D9); Enter confirms; Escape/Ctrl+C cancels (57-D4). Every
other key is a consumed no-op.

**Chain A.** Insert the action guard immediately after the existing Rewind guard
(`src/ui/supervisor.cpp:4328-4331`):

```cpp
if (model_.mode == UiMode::Rewind) {
    return handle_rewind(event);
}
// 80-D9: the action menu is a peer of the list, above the dashboard.
if (model_.mode == UiMode::RewindAction) {
    return handle_rewind_action(event);
}
if (model_.mode == UiMode::Dashboard) {
    return handle_dashboard(event);
}
```

Resulting order: exitConfirm > dialog > Context > Switcher > ModelPicker >
Rewind > RewindAction > Dashboard > globals.

**Chain R (base selector).** Replace the `dbox` Rewind branch
(`src/ui/ui_render.cpp:2414-2416`) and extend the base at `:2395-2399` so both
rewind screens are full-screen bases, with overlays composing over them:

```cpp
Element base = model.dashboard.open     ? render_dashboard(model, size, theme)
             : model.rewind_action.open ? render_rewind_action(model, theme, size.width)
             : model.rewind.open        ? render_rewind(model, theme, size.width)
             : std::move(main);
```

This is the supersession of 79-D6/79-RW13's render mode (sec.header). The exit
confirm / dialog / context / switcher `dbox` overlays still wrap `base`, so they
compose over a rewind screen exactly as they compose over the dashboard.

---

## 9. Retention and lifecycle integration

**Count/age eviction (80-D7).** On every capture and on `sweep`, for each
session: sort checkpoints ascending by `turn`; drop all but the newest
`kCheckpointKeepPerSession = 100`; then drop any with
`now - created_at_ms > kCheckpointRetention = 30 days`. Both rules delete only
from the **oldest end**, so the retained set stays a **contiguous suffix by
`turn`** (80-D7); this is what keeps `restore`/`revertible_count` correct (H3).
After dropping, GC blobs not referenced by any surviving entry across all
sessions.

**Session deletion.** `HostRuntime::deleteSession` (`src/host/host_runtime.cpp:918`)
calls `if (checkpoints_ != nullptr) { checkpoints_->removeSession(id); }` at
`:973` (M1 pointer), deleting the session's manifests and GCing its blobs. This
covers both the live `session.delete` RPC and `ymh session prune` when it goes
through a running daemon (`src/cli/session_cli.cpp:432-441`).

**Offline prune.** `ymh session prune` run **offline** deletes rows directly
(`src/cli/session_cli.cpp:501-504`) without the daemon, so the checkpoint files
for pruned sessions are orphaned until the next daemon startup. The startup
`sweep` (sec.4.5) reconciles against the workspace store's session set
(`runtime_->store().list()`, `include/ymh/session/session_persistence.hpp:82`):
any manifest whose `session` is not in that set is dropped and its blobs GCed.
This closes the offline gap without requiring `session prune` to know about
checkpoints, preserving the spec 23 flow (CP-F8).

**Subagents (spec 24).** A subagent is a separate `kind='subagent'` session with
its own agent and turns; captures are keyed by that session id, so subagent edits
never land in a parent's checkpoint. `/rewind` refuses subagent focus (79-D9), so
subagent checkpoints are never restorable. `session prune` and `removeSession`
clean them up with their session. This mirrors CC's "subagent edits are not
restored" ([recon-claude-code.md sec.1.3]) at the session boundary; the residual
that a subagent may have mutated a file the parent's restore set also touches is
a recorded risk (sec.15).

---

## 10. Failure modes

Continue the repo `F1-F12` convention with a spec-local `CP-F` prefix (disjoint
from `00 sec.54 F1-F12`, `FK-F`, and `RW-F`).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| CP-F1 | Disk full / write error during a blob or index write | capture marks the checkpoint `incomplete` (or the persist is retried); the tool call still succeeds | `sweep` retries the persist; the action menu warns the code restore may be partial (80-D13) |
| CP-F2 | A tracked path is a symlink or hard link | captured as `Symlink`; restore counts it in `skipped` | file keeps its current contents (CC parity, 80-D4/80-D5) |
| CP-F3 | A file changed since capture (external edit, shell edit) | restored anyway (authoritative), counted in `changed`; notice appends "(N files changed since the checkpoint)" | none needed; the pre-image overwrites (80-D6) |
| CP-F4 | A manifest references a missing/unreadable blob | that file counts in `failed`; the rest of the batch restores | report; re-run after retention/restore; re-capture on the next turn (80-D6) |
| CP-F5 | `index.json` is **corrupt** or has an unknown `version` (parse/schema failure) | the store loads as empty (no throw); `revertible_count` returns 0 so all code actions are hidden; a log entry names the path | startup `sweep` quarantines the file to `index.json.corrupt.<ts>` and starts a fresh index; blobs remain until GC. I/O errors are CP-F14, not this |
| CP-F6 | A restored path escapes the workspace root (root changed, traversal via an absolute path) | `resolve()` throws `PathEscape`; that file counts in `failed` | no write outside the root; report (80-D5) |
| CP-F7 | The process crashes mid-index-write | the old `index.json` is intact; the stale `index.json.tmp.<pid>` remains | `sweep` removes stale temp files; the index is never corrupt (CP9). A crash **between** a blob rename and the index persist loses that turn's checkpoint (an orphan blob): best-effort durability, LOW-7/sec.6.3 |
| CP-F8 | `ymh session prune` runs offline and leaves orphan checkpoint files | harmless until the next daemon | startup `sweep` reconciles manifests against the workspace store's session set (`runtime_->store().list()`) and GCs (sec.9) |
| CP-F9 | Code restore requested while the focused session has a running turn | the supervisor (80-D11) and the daemon both refuse with `"turn in progress"`; no RPC reaches the disk | user retries when idle |
| CP-F10 | A restored file is read-only / permission denied at write | that file counts in `failed`; the batch continues | report; the user fixes permissions (80-D6) |
| CP-F11 | `session.restore_code` names an unknown/evicted session | `AppCode::UnknownSession` (`-32006`) -> notice `"restore failed: <error>"` | `refresh_sessions` drops the stale row; no files touched |
| CP-F12 | The `session.restore_code` reply arrives after a focus change | the reply is dropped via `restore_query_`; no notice on the wrong session | user re-invokes (mirrors 79-F11) |
| CP-F13 | Code restore for a turn older than the retained window (target evicted) | `revertible_count` returns 0 (actions hidden); `restore` returns `expired = true` and restores nothing | notice `"checkpoint expired; code restore unavailable"`; never restore a later state as the target (H3/80-D7) |
| CP-F14 | `index.json` cannot be opened or read (I/O error, e.g. EACCES/EIO) | `CheckpointStore::restore` **throws** `CheckpointUnavailableError` (session layer), mapped by `translate` to `AppCode::CheckpointUnavailable` (`-32022`) -> notice `"restore failed: <error>"`. The read projection `revertible_count` instead returns 0, so `session.rewind_targets` still succeeds (NEW-M5) | fix filesystem permissions/I-O and retry; distinct from CP-F5 (corrupt/unknown version loads empty) |
| CP-F15 | Startup `sweep` hits the same index I/O error (CP-F14) | logged; the reconcile/GC pass is skipped; `dirty_` stays set; **`WorkspaceHost::Impl::startup` still reaches `Serving`** (no `HostExitCode::Internal`, no `HostState::Failed`) | retried on the next capture/`sweep`; a checkpoint-subsystem fault never fails daemon startup (NEW-M6; preserves spec 76) |

`CP-F2`/`CP-F3` are consequences of the CC contract, not redefinitions; they are
routed, not invented.

---

## 11. Invariants

Spec-local `CP` numbering. Every row is testable and cites the code/spec that
would be violated.

| ID | Invariant |
|---|---|
| CP1 | No new external dependency and no git: storage is `<ws>/.ymh/checkpoints/` with in-tree `ymh::sha256_hex` (`src/llm/sha256.cpp:32`) (80-D3). |
| CP2 | Every capture path and every restore path goes through `ExecutionEnvironment::resolve()`; no `chdir()` and no `getcwd()` base in checkpoint code (`07 sec.6.3`; `src/execution/environment.cpp:61-95`). |
| CP3 | The symlink/hard-link refusal runs on the **unresolved** candidate (`first_symlink_component`) before `resolve()` canonicalizes the leaf: a symlinked or hard-linked target is `skipped`, never written through (H2/80-D5/sec.5). |
| CP4 | Only `write_file`/`edit_file` contribute captures; `shell`/`pty`/`terminal` contribute none (80-D2; `src/tools/builtin_tools.cpp:151,183,381`). |
| CP5 | A checkpoint is keyed by `(session, TurnId)` and stores the pre-image (state at turn start); the first capture of a path in a turn wins (80-D1). |
| CP6 | The store lives under `<ws>/.ymh/checkpoints/` (= `workspace_root_/".ymh"/"checkpoints"`, H1), is written by the daemon only, and adds no row/column to `sessions.db` (80-D12). |
| CP7 | Code restore mutates only the working tree: it appends no event, changes no header/lease, and does not touch the conversation; conversation restore still branches via spec 79 (`src/ui/supervisor.cpp:2070-2102`) (80-D10). |
| CP8 | `capture`, `restore`, `removeSession`, and `sweep` are serialized by the store mutex; the daemon is the workspace's single writer (80-D11; spec 16). |
| CP9 | `index.json` is only ever replaced atomically (temp+fsync+rename); a crash leaves a valid index; an interrupted write leaves at most an orphan blob or a stale temp. Durability is best-effort: a crash between a blob write and the index persist loses that record, never corrupts the index (LOW-7/sec.6.3). |
| CP10 | Retention is bounded (at most `kCheckpointKeepPerSession` per session, none older than `kCheckpointRetention`) and both rules delete from the oldest end, so the retained set is a **contiguous suffix by `turn`**; unreferenced blobs are GCed (80-D7). |
| CP11 | `RewindTarget::file_change_count` is the count of distinct **retained** files revertible at or after the target turn, and is `0` when the target predates the retained window; the code action rows appear iff it is `> 0` (80-D8/80-D9). |
| CP12 | Conversation restore is spec 79 unchanged: `seed_length = boundary_index`, focus-only, prompt restored (79-D3/D4; RW1-RW12 not restated but not weakened). |
| CP13 | The Ctrl+S live surface and the `/sessions` stored catalog are unchanged by `/rewind` (22 sec.3/sec.4; 81-D10). |
| CP14 | The `Tool` virtual addition is additive: a tool that does not override `checkpoint_paths` returns empty and is never tracked (`include/ymh/tools/tool.hpp:70`). |

---

## 12. State lifetime

Columns follow 78 sec.10 / 79 sec.10. Every new state introduced by this spec is
tabulated.

| State | Created | Destroyed / evicted | Owner | Survives restart | Survives reconnect | Crash behaviour |
|---|---|---|---|---|---|---|
| `<ws>/.ymh/checkpoints/` directory (= `store_dir_`) | `CheckpointStore` constructor (`store_dir_ = workspace_root_/".ymh"/"checkpoints"`, H1) | with the workspace directory (never automatically) | daemon (spec 16) | yes (on disk) | yes | persists; harmless if empty |
| `index.json` | first capture / first `sweep` write | session eviction, count/age eviction, session deletion; quarantined on corruption (CP-F5) | daemon | yes (on disk) | yes | atomic rename keeps the previous copy valid; the record in flight is lost on a crash between blob and index write (LOW-7/CP9) |
| `blobs/<aa>/<sha256>` | capture, atomically (sec.6.3) | blob GC on eviction / `removeSession` / `sweep`; orphan blobs at next `sweep` | daemon | yes (on disk) | yes | orphan blobs are valid and reclaimed later |
| In-memory `sessions_` index (`CheckpointStore`) | lazy load on first capture/read | process exit; `removeSession` removes a key | daemon process | no (rebuilt from `index.json`) | n/a (daemon-local) | rebuilt from the on-disk index |
| `CheckpointStore::dirty_` | a failed index persist (80-D13) | next successful persist / process exit | daemon process | no | n/a | the in-memory entry is lost; the tool call already returned (best-effort, CP9) |
| `WorkspaceHost::Impl::checkpoints_` (owns the store) | daemon `startup()` (`src/host/workspace_host.cpp:774`); **declared before `runtime_` (:687) and `host_runtime_` (:692)** (NEW-L3) | daemon exit (`cleanupStartupFailure` on a failed startup, `:1074`); destroyed last among the three, by reverse declaration order | `WorkspaceHost::Impl` | no (reconstructed at startup) | n/a | the on-disk store is left intact; `sweep` runs next startup |
| `HostRuntime::checkpoints_` (non-owning pointer, M1) | `set_checkpoint_store` in `startup()` | daemon exit | non-owning; the daemon owns the target | no | n/a | always valid: the store is declared before `host_runtime_`, so it is destroyed after it (reverse declaration order, NEW-L3) |
| `ToolRegistry::checkpoint_recorder_` | startup (`set_checkpoint_recorder`) | daemon exit | daemon (non-owning pointer) | no | n/a | dangling is impossible: the store outlives the registry in the daemon |
| Per-call capture buffer (bytes read in `capture`) | `capture` | end of the `capture` call (blob written or dropped) | daemon request scope (transient) | no | no | lost; the tool call is unaffected |
| `UiModel::rewind` (`RewindOverlayModel`) + `file_change_count` per row | `open_rewind_overlay` (`src/ui/supervisor.cpp:2016`) | `close()` on Enter/Escape; process exit | supervisor process (in-memory) | no | no | lost; re-open `/rewind` |
| `UiModel::rewind_action` (`RewindActionModel`) | `open_rewind_action` | `close()` on selection/cancel; process exit | supervisor process (in-memory) | no | no | lost; re-open `/rewind` |
| `restore_query_` (in-flight restore guard) | `restore_code` submit | reply / connection drop / focus change (CP-F12) | supervisor process | no | no | pending request dropped on reconnect; no notice |
| `CheckpointRestoreReport` (session-layer domain result, NEW-M3) | `CheckpointStore::restore` | end of `HostRuntime::restoreCode` (copied into the wire DTO) | daemon request scope (stack temporary) | no | no | lost; recomputed on the next call |
| `protocol::RestoreReport` (wire result) | `HostRuntime::restoreCode` per request (converted from the session-layer `CheckpointRestoreReport`, NEW-M3) | when the JSON reply is serialized/sent | daemon request scope (transient) | n/a | n/a | none; recomputed on the next call |
| `protocol::RestoreReport` on the client | reply handler | surfaced as a notice then discarded | supervisor process | no | no | none |

No existing state's lifetime changes: the session rows, leases, agents, the
supervisor focus, and the `session_snapshots` cache are untouched (CP6/CP7/CP12).

---

## 13. dsh mapping

The DeepSeek Harness separates durable session state from ephemeral client
presentation. Code checkpoints are durable **workspace** state (not session
state), and `/rewind` stays a control-plane caller over both.

| dsh concept | ymh mapping here | justification |
|---|---|---|
| Durable session store | `<ws>/.ymh/sessions.db`; code checkpoints add **no** row/column | `02 sec.9` P11 / `02 sec.6.4-6.6` / 80-D12: `session_snapshots` stays an unwired derived cache (`src/session/session_persistence.cpp:75-83`); code state is a separate artifact |
| Durable workspace artifact | `<ws>/.ymh/checkpoints/` (CAS blobs + JSON index) | 80-D3; same per-workspace directory the daemon already owns (`src/host/workspace_host.cpp:1405`) and the same temp+fsync+rename discipline (`src/config/grant_store.cpp:242-278`) |
| Snapshot / COW | pre-image keyed by `(session, turn)`; content-addressed blobs dedup across files/turns | 80-D1/80-D3; a capture is O(changed bytes), not O(tree) |
| Open set / membership | shared `registry.db` unchanged; checkpoints are daemon-local | 80-D12; the store is not registered (it lives in the workspace dir) |
| Client-local focus | `UiModel::rewind` / `rewind_action` | never written to the registry (80-D12); `/rewind` reuses spec 79's focus-only path |
| Ownership / liveness | supervisor-owned daemon (spec 16); the store is created at daemon startup and dies with it | `WorkspaceHost::Impl::startup` (`src/host/workspace_host.cpp:774`); no coordinator change (80-D12) |
| Control plane | new `session.restore_code`; `session.fork` reused for conversation | 80-D8/80-D10; same JSON-RPC-over-UDS transport (05) |
| Event stream is truth | restore never edits the log; the conversation branch is still an event-log prefix | 80-D6/CP7; `resolve_after_locked` (`src/session/session_persistence.cpp:477-517`) unchanged |
| Agent lifetime | a capture happens inside the existing tool dispatch; no new agent/lifetime | `ToolRegistry::execute` (`src/tools/tool_registry.cpp:321-347`); spec 24 unchanged |
| **Non-mirror**: CC "Summarize from here / up to here" | **NOT mirrored** | scope decision: it is a compaction action, not a checkpoint action, and ymh's conversation rewind is already branch-not-truncate. Reason anchored in 80-D9 and 79 sec.13 (spec 79's out-of-scope list); the compaction seam (`13 sec.4.4`) is the future home |
| **Non-mirror**: CC tracks `NotebookEdit`/hard-link nuance | **NOT mirrored** | ymh has no notebook tool and no tracked hard-link operation; the tracked set is `write_file`/`edit_file` (80-D2, `src/tools/builtin_tools.cpp:151,183`) |
| **Non-mirror**: CC git-object / VSCode baseline storage | **NOT mirrored** | explicit no-git decision (80-D3, design-brief.md sec.G.3): ymh ships a content-addressed shadow store with no external dependency; the "first snapshot" baseline is not needed because ymh has no VSCode diff consumer |
| **Non-mirror**: CC `cleanupPeriodDays` config key and SDK `enable_file_checkpointing` | **NOT mirrored as config/opt-in** | 80-D7: retention is bounded by constants in Rev 1; ymh has no SDK surface, and making checkpointing opt-in would violate the "restore as a first-class `/rewind` action" goal. Reason anchored in 80-D7 |

---

## 14. Test plan

Strategy is `00 sec.44`: unit, integration (Fake LLM / fake daemon), persistence,
golden render, and an opt-in live PTY layer (`YMH_LIVE_LLM=1`). ID scheme:
`CP-U*` unit, `CP-I*` integration, `CP-G*` golden render, `CP-P*` PTY/live.
This spec does not edit tests; the implementation phase does.

### 14.1 Unit

| ID | Test (`file`) | Assertion |
|---|---|---|
| CP-U1 | `Checkpoints.CP_U1_CapturePreImageKeyedByTurn` (`tests/unit/checkpoints_test.cpp`) | capturing `P` in turn 3 stores `P`'s bytes; capturing `P` again in turn 3 keeps the first bytes (80-D1/CP5) |
| CP-U2 | `Checkpoints.CP_U2_AddedFileRestoresByRemoval` (`tests/unit/checkpoints_test.cpp`) | a `write_file` create is `kind=absent`; restore removes the file and counts `restored` (80-D4) |
| CP-U3 | `Checkpoints.CP_U3_BlobDedup` (`tests/unit/checkpoints_test.cpp`) | two identical pre-images in different turns/files share one blob file (80-D3) |
| CP-U4 | `Checkpoints.CP_U4_AbsentAndSymlinkKinds` (`tests/unit/checkpoints_test.cpp`) | a symlink is `kind=symlink` and restore counts `skipped`, never writes through it; `first_symlink_component` detects the **unresolved** leaf (a path whose `resolve()` would canonicalize to a regular file) and a hard-linked leaf (H2/80-D5/CP3/CP-F2) |
| CP-U5 | `Checkpoints.CP_U5_RestoreSetEarliestAtOrAfter` (`tests/unit/checkpoints_test.cpp`) | restoring to turn 3 uses a file's earliest capture with turn >= 3, leaving files touched only before turn 3 untouched (sec.7) |
| CP-U6 | `Checkpoints.CP_U6_CountAndAgeEviction` (`tests/unit/checkpoints_test.cpp`) | 101 checkpoints evict the oldest; a checkpoint older than 30 days is dropped; unreferenced blobs are GCed (80-D7/CP10) |
| CP-U7 | `Checkpoints.CP_U7_IndexCrashSafety` (`tests/unit/checkpoints_test.cpp`) | a stale `index.json.tmp.<pid>`/orphan blob does not break load; `sweep` removes temps and GCs orphans (CP9/CP-F7) |
| CP-U8 | `Checkpoints.CP_U8_CorruptIndexQuarantined` (`tests/unit/checkpoints_test.cpp`) | a corrupt `index.json` makes the store load as empty (`revertible_count == 0`, code actions hidden), and `sweep` quarantines the file (CP-F5) |
| CP-U9 | `Checkpoints.CP_U9_RestoreMissingBlobPartial` (`tests/unit/checkpoints_test.cpp`) | a missing blob counts `failed` but the other files restore; the report's `detail` matches the CC wording when none restore (80-D6/CP-F4) |
| CP-U10 | `Checkpoints.CP_U10_RevertibleCount` (`tests/unit/checkpoints_test.cpp`) | `revertible_count` counts distinct retained paths at turns >= T; returns 0 for an expired target (80-D8/CP11/CP-F13) |
| CP-U11 | `ToolRegistry.CP_U11_CaptureBeforeExecuteAndToolPaths` (`tests/unit/tools_test.cpp`) | the recorder is invoked with the tool's **raw** `path` (unresolved) before `tool->execute`; `write_file`/`edit_file` override `checkpoint_paths`, others return empty (80-D2/CP4/CP14) |
| CP-U12 | `LocalEnvironment.CP_U12_RestorePathIsRootConfined` (`tests/unit/path_safety_test.cpp`) | a restore path escaping the root throws `PathEscape` and counts `failed` (80-D5/CP2/CP-F6) |
| CP-U13 | `HostRuntime.CP_U13_RestoreCodeDispatches` (`tests/unit/host_runtime_test.cpp`) | `restoreCode` on a running session is refused; on an idle session it calls the store and maps `UnknownSession` (80-D8/80-D11/CP-F9/CP-F11) |
| CP-U14 | `TransportHost.CP_U14_FakeRestoreCodeCompiles` (`tests/unit/transport_server_test.cpp`) | `FakeTransportHost::restoreCode` satisfies the new base virtual and the TU compiles (sec.4.4) |
| CP-U15 | `Protocol.CP_U15_RestoreReportRoundTrip` (`tests/unit/transport_protocol_test.cpp`) | `protocol::RestoreReport` and `RewindTarget.file_change_count` round-trip; a missing count defaults to 0 (80-D8) |
| CP-U16 | `UiModel.CP_U16_RewindActionGatingAndCursor` (`tests/unit/errata80_ui_test.cpp`) | `open_with` builds the 4-action list when `file_change_count > 0`, the 2-action list otherwise; move/clamp/selected behave (80-D9/CP11) |
| CP-U17 | `Supervisor.CP_U17_RewindOpensActionNotFork` (`tests/unit/errata80_ui_test.cpp`) | Enter on the list opens the action menu and issues no `session.fork` until an action is chosen (80-D9/80-D10) |
| CP-U18 | `Checkpoints.CP_U18_ExpiredTargetRefused` (`tests/unit/checkpoints_test.cpp`) | a target older than `oldest_retained_turn` yields `revertible_count == 0` and a `restore` report with `expired == true` that writes no file, not a later checkpoint's state (H3/80-D7/CP-F13) |
| CP-U19 | `Checkpoints.CP_U19_WorkspaceRootVsStoreDir` (`tests/unit/checkpoints_test.cpp`) | the constructor's single argument is the workspace root; the index/blobs are written under `workspace_root/.ymh/checkpoints` and `workspace_relative` keys are workspace-relative (H1) |
| CP-U20 | `Checkpoints.CP_U20_IndexIoErrorThrows` (`tests/unit/checkpoints_test.cpp`) | the store's `restore` throws `CheckpointUnavailableError` for an index I/O error (mapped to `AppCode::CheckpointUnavailable`, NEW-M3/CP-F14); a corrupt/unknown-version file does **not** throw but loads empty (NEW-M2/CP-F5) |
| CP-U21 | `Checkpoints.CP_U21_ConstLazyLoad` (`tests/unit/checkpoints_test.cpp`) | `revertible_count` on a fresh store (const path) lazily loads once via `ensure_loaded_locked` and returns the right count; `loaded_`/`sessions_` are `mutable` (NEW-L5) |
| CP-U22 | `HostRuntime.CP_U22_RewindTargetsDegradesOnStoreError` (`tests/unit/host_runtime_test.cpp`) | with an unreadable `index.json`, `HostRuntime::rewindTargets` still returns the full turn list with every `file_change_count == 0` (no throw, no `CheckpointUnavailable`); `restore` on the same store still throws (NEW-M5/CP-F14) |
| CP-U23 | `Checkpoints.CP_U23_SweepNeverThrows` (`tests/unit/checkpoints_test.cpp`) | `sweep` with an unreadable `index.json` returns normally (logs, skips the pass, leaves `dirty_` set); a `WorkspaceHost::Impl::startup`-level harness with that store still reaches `Serving` (NEW-M6/CP-F15) |

### 14.2 Integration (fake daemon / Fake LLM)

Extend `tests/integration_host_harness_test.cpp` (in-process host via
`tests/support/host_harness.hpp`) and `tests/unit/errata80_ui_test.cpp`
(FakeLLM driver / `FakeTransportHost`).

| ID | Test | Assertion |
|---|---|---|
| CP-I1 | FakeLLM writes a file then rewinds code | after a `write_file` turn, `session.restore_code` restores the pre-image; the event log is unchanged (CP7) |
| CP-I2 | Restore code and conversation | the conversation is a `kind='fork'` child at the boundary (79) and the working tree is restored; both outcomes are surfaced (80-D10) |
| CP-I3 | Shell edit is not restored | a file changed through `shell` in the turn is untouched by restore (80-D2/CP4) |
| CP-I4 | `session prune` integration | deleting a session through a live daemon removes its manifests and GCs blobs; an offline prune leaves orphans that the startup `sweep` reclaims (sec.9/CP-F8) |
| CP-I5 | Retention across a resume | a session with >100 checkpoints keeps the newest 100 after `sweep`; a resume finds the surviving checkpoints (80-D7) |
| CP-I6 | Restore partial failure | with one blob removed, restore reports `failed=1` and still restores the others; the notice is accurate (CP-F4) |

### 14.3 Golden render

`tests/unit/errata80_ui_test.cpp` (FTXUI snapshot harness, like `errata79_ui_test.cpp`)
and `tests/unit/ui_render_golden_test.cpp`:

| ID | Test | Assertion |
|---|---|---|
| CP-G1 | Checkpoint list renders full-screen with code counts | the list draws the `file_change_count` marker and uses the full-screen base (80-D9) |
| CP-G2 | Action menu shows gate-correct rows | code actions present iff `file_change_count > 0`; numbering and highlight match (80-D9/CP11) |
| CP-G3 | Overlays compose over a rewind screen | the exit-confirm `dbox` composes over the full-screen rewind base, not the conversation (sec.8) |

### 14.4 PTY / live (opt-in)

`tests/unit/ui_live_pty_test.cpp` (opt-in, `YMH_LIVE_LLM=1`):

| ID | Test | Assertion |
|---|---|---|
| CP-P1 | `/rewind` code restore inside the TUI | with a real model, a write then `/rewind` "Restore code" reverts the file, and "Restore code and conversation" branches and reverts |
| CP-P2 | Restore while busy refused | invoking code restore during a running turn shows the refusal notice and touches no file (80-D11/CP-F9) |

### 14.5 Invariant and failure-mode coverage

| Invariant / mode | Test |
|---|---|
| CP1/CP6 | CP-U1, CP-U3, CP-U19 (store under the workspace dir; no git) |
| CP2/CP3 | CP-U4, CP-U12 |
| CP4/CP14 | CP-U11, CP-I3 |
| CP5 | CP-U1, CP-U5 |
| CP7/CP12 | CP-I1, CP-I2, CP-U17 |
| CP8 | CP-U13 (idle vs running), CP-U7 |
| CP9 | CP-U7, CP-U8 |
| CP10 | CP-U6, CP-I5 |
| CP11 | CP-U10, CP-U16, CP-G2 |
| CP13 | existing 22/81 tests (no change) |
| CP-F1..CP-F15 | CP-U7 (F1/F7), CP-U4 (F2), CP-I1 + changed counter (F3), CP-U9 (F4), CP-U8 (F5), CP-U12 (F6), CP-I4 (F8), CP-U13 (F9/F11), CP-I6 (F10), CP-U17 (F12), CP-U18 (F13), CP-U20 (F14), CP-U23 (F15) |

---

## 15. Out of scope, parity notes, and recorded risks

- **Shell/PTY/terminal file edits are not captured** (80-D2), mirroring CC
  ([recon-claude-code.md sec.1.3]). A file created or deleted by a shell command
  is invisible to restore. This is a documented parity limit, not a bug.
- **Subagent edits are not captured into a parent checkpoint** (sec.9). If a
  subagent mutates a file that the parent's restore set also names, restoring the
  parent's checkpoint reverts only the parent's captures; the subagent's change
  to that file is what was current at capture time if the subagent wrote before
  the parent's first capture in a later turn. Recorded risk (CC parity: subagent
  edits are not restored).
- **No "Summarize from here / up to here"** (80-D9). Not a checkpoint action;
  ymh's compaction seam is spec 13.
- **Retention is constant, not configured** (80-D7). `cleanupPeriodDays` is not
  mirrored as a JSONC key in Rev 1; a future revision can add
  `checkpoint.keep`/`checkpoint.retention_days` to the config spec (21).
- **`sha256_hex` lives under `llm/`** (recorded risk, 80-D3). The store includes
  it only from `src/session/checkpoints.cpp`; if a reviewer objects to the
  `session -> llm` edge, the fix is a pure move of the declaration/definition to
  `include/ymh/core/sha256.hpp` + `src/core/sha256.cpp` (callers already
  enumerated: `src/llm/llm_runtime.cpp:145,160,170`,
  `src/agent/agent_loop.cpp:144,159,757,1166,1455`) with `llm_runtime.hpp`
  including it - the same promotion precedent as 79 sec.4.2.1's `text_of_blocks`.
- **Cross-turn dedup by content, not delta.** A 1-byte edit to a large file
  stores the full new pre-image as a distinct blob. Acceptable in Rev 1; a
  delta compression pass (or an in-tree zlib, if it becomes a dependency) is a
  future optimization, not required.
- **Changed-since-capture is advisory.** CP-F3 detects `size`/`mtime`
  divergence, not content divergence; a same-size, same-mtime external edit is
  restored silently. The manifest cannot detect it without storing a digest of
  the expected post-state, which Rev 1 does not.
- **Restore is authoritative.** A partial restore can leave the tree between two
  states; the report is the record. There is no transactional multi-file restore
  (no journaled rename set); a future revision could stage to temps and commit,
  but the CC contract is a best-effort per-file restore.
- **No in-place conversation truncation.** Spec 79's branch remains the only
  conversation mechanism (80-D10); this spec does not add a suffix-shadow op.
- **Ownership and live-vs-stored are unchanged.** The store is supervisor-owned
  daemon state (spec 16); `/sessions` stays the stored catalog and Ctrl+S the
  live surface (22 sec.3/sec.4; 80-D12).

---

## 16. Revision log

| Rev | Change |
|---|---|
| 1 | Initial draft: a lazily-populated per-turn pre-image checkpoint model (80-D1) tracking only `write_file`/`edit_file` (80-D2); a content-addressed shadow store with a JSON index under `<ws>/.ymh/checkpoints/` and a temp+fsync+rename protocol (80-D3/sec.6); creation/overwrite/delete/rename/symlink semantics (80-D4); a root-confined, symlink-refusing restore over `ExecutionEnvironment::resolve()` with a per-file `RestoreReport` (80-D5/80-D6/sec.7); bounded retention (100/30d) tied to `session.delete`, offline prune, and a startup sweep (80-D7/sec.9); one additive DTO field `RewindTarget::file_change_count` plus one new method `session.restore_code` and one new `TransportHost` virtual (80-D8/sec.4.3-4.4); a two-step full-screen `/rewind` menu (list -> 4-action menu, code actions gated) reusing spec 81's base selector, superseding 79-D6's render mode and 79-RW13 (80-D9/sec.8); one additive `Tool::checkpoint_paths` virtual and the `ToolRegistry::execute` capture hook (sec.4.2); failure modes `CP-F1..CP-F12`, invariants `CP1..CP14`, a state-lifetime table (sec.12), a dsh-mapping table with reasons (sec.13), and test IDs `CP-U1..CP-U17`, `CP-I1..CP-I6`, `CP-G1..CP-G3`, `CP-P1..CP-P2`. |
| 2 | Gate-80 fixes. **H1**: split `CheckpointStore`'s one ctor argument into `workspace_root_` (containment + key base) and `store_dir_ = workspace_root_/".ymh"/"checkpoints"` (index/blobs), fixed the startup caller and the `root()` removal (sec.4.1/sec.5/sec.6.1/sec.12). **H2**: `checkpoint_paths` now returns the **unresolved** raw path and capture/restore run the new `first_symlink_component` (symlink component or hard-linked leaf) **before** `resolve()` canonicalizes it (sec.2.3/80-D4/80-D5/sec.5/sec.7). **H3**: retention is pinned as a contiguous oldest-end window with `oldest_retained_turn`; `revertible_count` returns 0 and `restore` returns `expired=true` for a target older than the window, never substituting a later checkpoint (80-D7/80-D8/sec.7/CP-F13). **M1**: `CheckpointStore` is owned by `WorkspaceHost::Impl` and injected into `HostRuntime` via `set_checkpoint_store`; added to the state-lifetime table. **M2**: promoted `fsync_parent_directory` to `include/ymh/core/fs.hpp`/`src/core/fs.cpp` (no longer the anon-ns copy). **M3**: deleted the dead `list`/`load`/`root`/`kTrackedCheckpointTools`; every remaining new symbol has a body and a caller. **M4**: dropped the `02 sec.6` atomic-write claim and cited the real JSON-state precedents. **M5**: `02 sec.9 P10` -> `P11` (+`sec.6.4-6.6`). **M6**: replaced `live_session_ids()` with `runtime_->store().list()` and added the `dirty_` member + lifetime row. **LOW-1**: added 79-D7 to Supersedes. **LOW-2**: `read_file_max_bytes` -> `config.hpp:20`. **LOW-3**: replaced `is_running()` with the explicit `AgentState` switch. **LOW-4**: defined `workspace_relative`. **LOW-6**: fixed the `ui_model.hpp`/`ui_render.cpp` off-by-ones. **LOW-7**: documented the blob/index crash-window (best-effort durability). Tests `CP-U18`/`CP-U19` added. |
| 3 | Gate-80 re-review fixes (0 HIGH / 2 MEDIUM / 5 LOW remaining). **NEW-M1**: removed the underivable, consumerless `Checkpoint::seq` from the struct (L493), the `index.json` example, and capture Step 7; `capture` has no event-log access, and the UI's `boundary_index` comes from `session.rewind_targets` (sec.4.1/sec.5/sec.6.2). **NEW-M2**: pinned two index-failure classes - **corrupt/unknown-version** loads empty + quarantines (CP-F5), **I/O error** throws `CheckpointUnavailable` (new CP-F14); aligned sec.4.1, sec.4.3, sec.6.2, sec.7 Step 1, and CP-F5. **NEW-L1**: `entry.added` -> `entry.kind = PreImageKind::Absent` (80-D4). **NEW-L2**: `fake_transport_host.hpp:19` -> `:22` (sec.4.4). **NEW-L3**: pinned `WorkspaceHost::Impl::checkpoints_` as declared **before** `runtime_`/`host_runtime_` for reverse-order teardown; updated the lifetime rows (sec.4.5/sec.12). **NEW-L4**: dropped `handoff.cpp` from the fsync precedent row (temp+rename only) and cited the real `fsync_parent_directory`/trust/grant-store precedents (sec.1). **NEW-L5**: marked `sessions_`/`loaded_`/`dirty_` `mutable` and added the `ensure_loaded_locked()` const loader with callers; documented in sec.5 Step 2 and the callers list. Added tests `CP-U20` (I/O vs corrupt) and `CP-U21` (const lazy load). |
| 4 | Gate-80 second re-review fixes (0 HIGH / 1 MEDIUM / 3 LOW remaining). **NEW-M3**: pinned ONE report per layer - the session-layer domain `ymh::CheckpointRestoreReport` (renamed, sec.4.1) plus the wire `protocol::RestoreReport` (sec.4.3), bridged field-by-field in `HostRuntime::restoreCode` (sec.4.4) so `session/checkpoints.hpp` does not include `transport/protocol.hpp`; the index I/O error is the session-layer `CheckpointUnavailableError`, mapped by `translate` to `AppCode::CheckpointUnavailable` (sec.4.1/sec.4.4/sec.6.2/sec.7/CP-F14); updated the state-lifetime rows. **NEW-L6**: `capture_failed` -> `PreImageKind::Failed` (80-D13). **NEW-L7**: `render_rewind` corrected to `(const UiModel&, const Theme&, int)` in sec.4.8 and the sec.8 base selector. **NEW-L8**: anchors `command_registry.cpp:296-305` -> `:299-305`, `builtin_tools.cpp:157,190` -> `:158,193-194`, `protocol.hpp:411-418` -> `:412-418`. |
| 5 | Gate-80 third re-review fixes (0 HIGH / 2 MEDIUM / 1 LOW remaining). **NEW-M4**: made the pinned `restore` body const-correct by dropping `const` on the environment only - `CheckpointRestoreReport restore(ExecutionEnvironment&, ...)` (sec.4.1) - because `restore` calls the non-const `ExecutionEnvironment::fs()` (`environment.hpp:39`) to `remove`/`write`; the daemon call still binds and sec.7 uses the non-const `env` verbatim. **NEW-M5**: pinned the read-projection degradation - `revertible_count` catches `CheckpointUnavailableError` and returns 0 (sec.4.1/sec.6.2), so spec 79's frozen `session.rewind_targets` never fails on an unreadable checkpoint index; only the mutating `restore` surfaces the error. Updated sec.4.5, CP-F14, and added test `CP-U22`. **NEW-L9**: rewrote the method-count sentence - `kMethodCatalog` goes 40 -> 41 and the 05 sec.7.4 session-lifecycle table gains one row (8 -> 9), rather than calling sec.7.4 the 41-entry table (sec.4.3). |
| 6 | Gate-80 fourth re-review fixes (0 HIGH / 1 MEDIUM / 1 LOW remaining). **NEW-M6**: pinned `sweep` as non-throwing - it catches `CheckpointUnavailableError`, logs, skips the reconcile/GC pass, and leaves `dirty_` set, so a checkpoint-subsystem I/O error never fails `WorkspaceHost::Impl::startup` (spec 76 preserved); pinned in sec.4.1, sec.4.5, the sec.6.2 I/O bullet, new failure mode **CP-F15**, and test **CP-U23**. **NEW-L10**: named the `RewindTarget` serializers (`to_json`/`from_json`, `src/transport/protocol.cpp:637-651`) as gaining one line for `file_change_count` with a default-0 `from_json` (sec.4.3). |
