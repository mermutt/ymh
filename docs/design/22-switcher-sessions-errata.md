# 22 — Switcher & Sessions Errata (S1–S4)

```
Status: verified · verified-by: Oracle (adversarial gate, PASS — zero open HIGH/MEDIUM) · reviewer: Oracle · Rev 5
Component: 22 (errata) — amends 10-supervisor-tui.md by reference only
Depends on: 00-architecture.md §9.2/§9.10/§20.24, 10-supervisor-tui.md (verified),
            03-workspace-registry.md (verified) R5/R10/R11, 16-daemon-ownership.md
            (verified) §3.2.1/§3.6/§4/§5.1/§7.6, 11-m2-errata.md §5.3 (D16),
            17-ui-transcript-errata.md (verified), 19-session-rename-errata.md,
            UI_SURFACE_INVENTORY.md (C1–C5)
Scope: four user-approved changes on the live `ui::run_supervisor` path and the
       `ymh` TUI entry: S1 live-only Ctrl-S switcher + daemon-death eviction;
       S2 a new `/sessions` stored-session catalog read from disk; S3
       spawn/attach-then-resume on selection; S4 honor `ymh --resume <id>` in
       TUI mode
Supersedes: 00 §20.24 :2997-2998 ("It shows ALL workspaces, including detached
            and background ones"); 10 §7.1 :852-853 (same sentence);
            10 §7.1 :871-873 (the "joined with workspace.list / session.list
            reads for detached workspaces not yet attached" clause and the
            "Ordering follows the registry junction" clause); 16 §3.2.1 :537
            ("Until then the workspace is listed as NotRunning and is browsable
            read-only") and 16 §7.7 :2138-2139 ("the cwd workspace shows
            NotRunning if no daemon") — the Ctrl-S switcher half only
Amends:     10 §7.5 :897-902 (adds the `/sessions` selection path alongside the
            retained focus-only rule); 10 §7.2 :875-880 (adds `/sessions` and
            the History-source keybindings); 10 §8.1 :928 (the switcher overlay
            is shared by both sources); UI_SURFACE_INVENTORY.md Surface 3
            :55-74 (the inventory was subsequently refreshed by the separate
            doc-sync pass to describe the live-only switcher and `/sessions`;
            spec 22 itself does not edit it); the switcher's ordering claim at
            ui_model.cpp (comment) and UI_SURFACE_INVENTORY.md :59-60
Retained:   `OwnershipMark` enum and `ownership_mark()` (16 §7.6 :695-703) —
            the enum is unchanged; only its *reachability in the switcher*
            narrows. The spec-16 last-exit prompt (§4) and the daemon-side
            owner watchdog (§5.1) are untouched and are not display-driven.
            03 R5/R10/R11, `releaseHost`, `removeWorkspace` are untouched.
            10 §7.5 ("Selecting a session in the switcher sets activeSessionId
            (focus). It does not call session.activate") is retained for the
            Live source and extended, not replaced, for the History source.
```

This document is the design gate for the user-approved S1–S4 changes. It is
**additive**: it pins new text, interface names, invariants, failure modes, and
tests. It does not rewrite `00-architecture.md`, `10-supervisor-tui.md`,
`16-daemon-ownership.md`, `03-workspace-registry.md`, or
`UI_SURFACE_INVENTORY.md`; each superseded/amended clause is quoted with
`file:line` and its replacement is given here. The convention matches specs 16,
17, 19, and 21.

**Naming note.** Invariants local to this spec are **`SW1`–`SW26`** (Rev 3);
failure modes are **`SW-F1`–`SW-F19`**; decisions are **`22-D1`–`22-D6`**. The
invariant prefixes `S` (01), `P` (02), `R` (03), `H`/`D` (04), `T` (05), `A`
(06), `X`/`E` (07), `L` (08), `Q` (09), `U` (10), `M` (11), `C` (13), `E-P`
(14), `MCP` (15), `O` (16), `U-RB*` (17), `CTX` (18), `RN`/`R-F` (19), `SK`
(20), and `J` (21) are all taken. The task brief suggested `S1`–`Sn`, but `S`
is claimed by spec 01; **`SW` is unused by every spec in `docs/design/`** and
is used here. The user's four decisions keep their brief labels **S1–S4**;
where this spec pins a new architecture-level decision it is written `22-Dn`,
never a bare `Dn`.

**No new RPC (pinned).** S1–S4 add **zero** wire methods. `/sessions` reads
disk directly through the existing `SessionPersistence`; S3 reuses the existing
`session.resume` (`protocol::method::kSessionResume`); S4 reuses the same. If a
future revision wants `session.listAll`, it must be raised as a proposal
(§11.5), not smuggled in here.

---

## 1. Purpose, scope, and supersession map

### 1.1 The problem

The Ctrl-S switcher is described in the architecture as showing **all**
workspaces, including detached and background ones (`00-architecture.md`
§20.24 :2997-2998; `10-supervisor-tui.md` §7.1 :852-853), and spec 16 pins
`NotRunning` workspaces as **browsable read-only** (`16-daemon-ownership.md`
§3.2.1 :537). The shipped code implements the literal "all workspaces" reading
by accident of construction:

- `SwitcherOverlayModel::open(const UiModel&)` (`src/ui/ui_model.cpp`) copies
  **every** entry of `UiModel::workspaces` with no liveness filter; it only
  sorts by title.
- `UiModel::workspaces` is **append-only**. `SupervisorApp::attach_workspace`
  (`src/ui/supervisor.cpp`) inserts a `WorkspaceModel` the first time a
  workspace is attached and returns early if one already exists.
- Nothing evicts a workspace when its daemon dies. `SupervisorApp::on_link_state`
  (`src/ui/supervisor.cpp`) only flips `WorkspaceModel::daemonStatus` to
  `Detached`/`Dead`; the entry and its `SupervisorConnection` stay in the model
  forever.
- `DaemonSetScanner::scanOnce()` (`src/ui/supervisor_presence.cpp`) is the only
  liveness authority (`record.host.has_value() && registry_->probeLiveness(record.id)
  == HostLiveness::Live`), but `SupervisorApp::on_scan` (`src/ui/supervisor.cpp`)
  only ever **adds** the live set.

Net effect: `UiModel::workspaces` is *every daemon ever seen live during this
supervisor process's lifetime*, retained forever, and the switcher renders all
of it. The reporter observed a long-lived supervisor whose switcher listed **42
entries while exactly one daemon was live**. Because the list is never pruned,
this is also an unbounded-memory defect: each stale entry keeps its
`SupervisorConnection`, its `SessionUiState`s, and its `SessionCell`s.

A second, orthogonal gap: the supervisor has no way to see **stored session
history** for workspaces that are not currently running. `SupervisorApp::refresh_sessions`
(`src/ui/supervisor.cpp`) issues `session.list` only on an **attached** (live)
`SupervisorConnection`; there is no `session.listAll` RPC; and
`SupervisorRunOptions::lifecycle` (`include/ymh/ui/supervisor.hpp`), the
existing `HostLifecycle` seam, is constructed and passed in
(`src/cli/cli.cpp`) but is **never dereferenced anywhere in `src/ui/`** (grep:
the only occurrence of `lifecycle` under `src/ui/`/`include/ymh/ui/` is the
member declaration). So a stopped workspace's sessions are invisible and
unresumable from the TUI, and `ymh --resume <id>` is silently ignored in TUI
mode because `run_supervisor_entry` (`src/cli/cli.cpp`) never reads
`CliInvocation::session` (`include/ymh/cli/cli.hpp`), even though
`add_common` registers `--resume` (`src/cli/cli.cpp`) and the dispatch case
`CliInvocation::Command::Tui` drops it.

### 1.2 What this changes in one sentence

The Ctrl-S switcher becomes **live-only with eviction**, a new **`/sessions`**
surface lists every registered workspace's stored sessions read read-only from
disk on a background worker, selecting a stored session in a non-running
workspace **spawns/attaches its daemon and resumes it**, and
**`ymh --resume <id>`** finally resolves the session's own workspace and
resumes it in TUI mode.

### 1.3 Supersession map

#### 1.3.1 Superseded

| ID | Prior text | Change |
|---|---|---|
| 22-S1 | `00-architecture.md` §20.24 :2997-2998 — *"with two levels: workspaces at the top, sessions as leaves. It shows ALL workspaces, including detached and background ones."* | The **Ctrl-S Live switcher** shows only workspaces with a currently-live daemon (§3). The "ALL workspaces" sentence is replaced by: *"the Ctrl-S switcher shows only workspaces whose daemon is currently live; the `/sessions` History source (§4) shows stored sessions for every registered workspace, live or not."* |
| 22-S2 | `10-supervisor-tui.md` §7.1 :852-853 — *"It shows ALL workspaces, including detached and background ones (`§20.24`)."* | Same replacement as 22-S1. |
| 22-S3 | `10-supervisor-tui.md` §7.1 :871-873 — *"The node list is built from `UiModel.workspaces` (status/badges) joined with `workspace.list` / `session.list` reads (`05 §7.3`) for detached workspaces not yet attached. Ordering follows the registry junction (`03 R8`)."* | The **Live** node list is built from `UiModel.workspaces` alone (which is now maintained live-only, §3.2); the "joined with … for detached workspaces not yet attached" clause is deleted. Ordering is pinned to **title ascending (case-insensitive), tie-broken by `canonical_path`** (§3.7), not the registry junction. The **History** node list is built from `UiModel.catalog` (§4). |
| 22-S4 | `16-daemon-ownership.md` §3.2.1 :537 — *"Until then the workspace is listed as **`NotRunning`** and is browsable read-only."* **and** §7.7 :2138-2139 — *"…(no eager `ensureRunning(row->id)`, `:352`); the cwd workspace shows `NotRunning` if no daemon"* | The Ctrl-S switcher no longer lists `NotRunning` workspaces and no longer renders the `NotRunning` mark (§3.4). A `NotRunning` workspace's **stored sessions** are browsable read-only via `/sessions` (§4), and selecting one spawns the daemon and resumes it (§5). The `NotRunning` value of `DaemonStatus` (16 §7.6 :2053) is retained for the model, but is no longer rendered in the switcher. **16-D2's lazy-spawn policy itself is not superseded** (see §11.4); §7.7's "cwd workspace shows `NotRunning`" is superseded by the same rule (LOW-B, Rev 5). |

#### 1.3.2 Amended

| ID | Amended clause | Change |
|---|---|---|
| 22-A1 | `10-supervisor-tui.md` §7.5 :897-902 — *"Selecting a session in the switcher sets `activeSessionId` (focus). It does not call `session.activate`; activation remains daemon-driven …"* | **Retained for the Live source.** Extended for the History source: selecting a session issued from `/sessions` additionally issues `session.resume` (§5.2) for the target workspace after it attaches. `session.activate` is **still not called** — the retained sentence remains literally true. |
| 22-A2 | `10-supervisor-tui.md` §7.2 :875-880 (keybindings) | Adds `/sessions` (History source) and its keys (`r` = refresh catalog, `Esc` = close) to the switcher keymap. Existing keys (`j`/`k`/`↓`/`↑`, Tab, Enter, Esc, Ctrl+W) are unchanged. |
| 22-A3 | `10-supervisor-tui.md` §8.1 :928 — `SwitcherOverlay (§7)` | The one `SwitcherOverlay` is shared by both sources (Live and History); no second overlay is added (C4/O14). |
| 22-A4 | `UI_SURFACE_INVENTORY.md` Surface 3 :55-74 (recorded; refreshed by the doc sync) | Surface 3's "across **all** `model.workspaces`, sorted by title" described the pre-spec behaviour. Spec 22 changes the Live source to live-only and pins title ordering as authoritative. The inventory has since been refreshed by the separate doc-sync pass to describe the live-only switcher and `/sessions`; spec 22 itself does not edit it. |
| 22-A5 (Rev 3) | Switcher ordering claim: `src/ui/ui_model.cpp` comment *"sort by display title … and match the registry's canonical-path order in practice"*; `UI_SURFACE_INVENTORY.md` :59-60 *"sorted by title"* | The claim that title order matches registry `ORDER BY canonical_path` (`WorkspaceRegistry::listWorkspaces`, `src/registry/registry.cpp`) is **false in general** (a workspace's `display_title` need not be the basename of its `canonical_path`). Pin (fixes M1 + MEDIUM-4): both switcher sources order **workspace entries** by `title` ascending (case-insensitive), tie-broken by `canonical_path` ascending; **Live sessions** keep the daemon's `session.list` order, **History sessions** are `updated_at` desc / `id` asc. The registry `ORDER BY canonical_path` is the grouping key/tie-break only (§3.7, §4.3). |
| 22-A6 | `include/ymh/ui/ui_model.hpp` `WorkspaceModel` / `WorkspaceNode` / `SessionNode` | Additive fields only (C4 "extend, do not duplicate"): `WorkspaceModel::live`, `WorkspaceNode::historyOnly`/`note`, `SessionNode` history fields, and `SwitcherOverlayModel::source`. No field is removed. |
| 22-A7 (Rev 3, M3 + §11.4) | `16-daemon-ownership.md` §3.2.1 :531-539 (16-D2) — lazy spawn pinned for *the cwd workspace* on *a prompt / `session.create` / workspace activation* | Extend the **scope** to any registered workspace selected from `/sessions` **and to S4's `--resume`-resolved (possibly non-cwd) workspace**, and the **trigger list** to include `session.resume`. The lazy on-demand policy is retained for S3; S4's initial attach is the shipped eager `ensureRunning`, explicitly authorized here. 16-D2 is extended, not fully superseded. See §11.4. |
| 22-A8 (Rev 2, H1) | `include/ymh/ui/ui_model.hpp` `UiModel` | Additive: `std::deque<UiNotice> notices` + `void pushNotice(std::string)`, rendered in `render_status`; a workspace-independent notice surface so spawn failures for unmodeled workspaces are visible without `ensureSessionIn` injection. |
| 22-A9 (Rev 2, H2) | `src/ui/supervisor.cpp` `SupervisorApp` worker model | The spawn worker becomes a single owned `std::jthread` joined in `~SupervisorApp` (replacing the Rev 1 detached thread). No API change outside `SupervisorApp`. |
| 22-A10 (Rev 3, MEDIUM-1) | `src/ui/supervisor.cpp` `resume_after_attach` | The success lambda guards on `model_.workspaces.count(workspace) != 0`; an evicted workspace routes through `surface_notice` and returns. No reply path injects a `WorkspaceModel`. |
| 22-A11 (Rev 3, MEDIUM-2) | `src/ui/supervisor.cpp` `SupervisorApp` member model | `catalog_` (the `SessionCatalogReader`) is a member owned by `SupervisorApp`; `~SupervisorApp` stops+joins it before other teardown; declaration order pinned after `action_mutex_`/`actions_`/`screen_`. |
| 22-A12 (Rev 3, MEDIUM-3) | `src/session/session_persistence.cpp` read taxonomy (as consumed by `/sessions`) | A pre-flight classification precedes `openReadOnly`; the §4.4 matrix and catch order are aligned with what the code actually throws (non-DB → `(corrupt)`, residual → `(unavailable: <msg>)`). No `SessionPersistence` API change is required. |
| 22-A13 (Rev 3, MEDIUM-4) | Session ordering | Live sessions keep the daemon's `session.list` order; History sessions are `updated_at` desc / `id` asc. Replaces the Rev 2 "newest-first for both sources". |
| 22-A14 (Rev 4, MEDIUM) | `read_workspace_history` pre-flight (§4.4) | Adds **pre-flight step 0** (`is_directory(record.canonicalPath) == false → "workspace missing"`) before the DB-existence check; the "workspace dir gone" matrix row, SW-F11, SW-U4, SW-U20, SW-I4 all key on step 0; the catch-order `StoreOpenError` "workspace root…" branch is TOCTOU-only. |
| 22-A15 (Rev 4, LOW-3) | `read_workspace_history` signature | `read_workspace_history(const WorkspaceRecord&, bool live)`; the caller supplies `probeLiveness == Live` because the read helper has no registry. `WorkspaceHistory.live` is set from the argument. |
| 22-A16 (Rev 4, LOW-4) | `UiModel::eraseWorkspace` promotion | One rule: first remaining **renderable** (`live && Attached/Stopping`) in map-key order, else first remaining by map key, else empty. §3.2 rule 7 and §3.5 step 4 agree. |

#### 1.3.3 Retained (recorded, not changed)

| ID | Clause | Why it is untouched |
|---|---|---|
| 22-R1 | `16-daemon-ownership.md` §3.6/§7.6 `OwnershipMark { Owned, NotRunning, Stopping, Unreachable }` and `ownership_mark()` | The enum and its derivation are **unchanged**. S1 only removes the *rendering* of `NotRunning`/`Unreachable` from the switcher, by removing the workspaces that would carry them. `ownership_mark()` keeps all four mappings (existing test `UiModel.OwnershipMarkDerivesFromDaemonStatus` stays green). |
| 22-R2 | `16-daemon-ownership.md` §4 (last-exit prompt) and §5.1 (owner watchdog) | Not display-driven. `SupervisorApp::compute_orphaning_set`/`query_orphaning_view`/`open_exit_prompt` (`src/ui/supervisor.cpp`) iterate `connections_` and call `host.ownership`; they never read `OwnershipMark` or the switcher. `open_exit_prompt` builds `ExitConfirmState` from `count_sessions`/`count_running`, which read `model_.workspaces`/`model_.sessions`, not `model_.switcher`. The watchdog is daemon-side (`WorkspaceHost`, spec 16 §5.1). S1 cannot affect either. |
| 22-R3 (Rev 3) | `03-workspace-registry.md` R5/R10/R11, `releaseHost`, `removeWorkspace` | S1–S4 perform **no registry writes**. Eviction is a UI-model operation; `/sessions` and the S4 resolver only **read** the already-open registry (`SupervisorRunOptions::registry`, opened by the CLI via `WorkspaceRegistry::open`; reads take no `flock` and see a WAL snapshot, `include/ymh/registry/registry.hpp`), not a separate `openReadOnly`. Liveness stays lock-primary (R5). |
| 22-R4 | `10-supervisor-tui.md` §7.5 `session.activate` is never called by the switcher | S3 uses `session.resume`, a different method. Activation remains daemon-driven. |
| 22-R5 | `10-supervisor-tui.md` §7.4 XOFF/`Ctrl+P`/`/switch`; §7.3 focus blocking; §7.2 `Ctrl+W` detach | Unchanged. `Ctrl+W` now applies only to live workspaces (non-live ones are not in the Live source). `/switch` is spec text only; it is not in the shipped `CommandRegistry::builtin` and S2 does not add it. |
| 22-R6 | `03 R8` ordinal ordering, `11-m2-errata.md` §5.3 (D16) `listSessionsOrdered` | Unchanged; the daemon's `session.list` order is still the live-cell order. `/sessions` does not consume it (it reads disk). |
| 22-R7 (Rev 2) | `16-D2` lazy-spawn **policy** (16 §3.2.1 :531-551) | Retained as policy, but **not unqualified**: 16-D2's **scope/trigger list is amended** by 22-A7 (spawn may target a non-cwd workspace; `session.resume` is a trigger). Only the "browsable as `NotRunning` in the switcher" half is superseded (22-S4). Recorded discrepancy with shipped code in §11.4. |

### 1.4 Scope boundaries

- **In scope:** the live `ui::run_supervisor` path (`src/ui/supervisor.cpp`,
  `src/ui/ui_model.cpp`, `src/ui/ui_render.cpp`, `src/ui/command_registry.cpp`)
  and the `ymh` TUI entry (`run_supervisor_entry`, `src/cli/cli.cpp`).
- **In scope:** the existing `HostLifecycle` seam is wired into the UI (§5).
- **Out of scope:** `ui::run_tui` (`src/ui/ui_application.cpp`) is dead code
  (no caller in `src/`; spec 17 §1). It is not touched.
- **Out of scope:** any new RPC, any new config key, any registry schema change,
  any change to `OwnershipMark`, the exit prompt, or the watchdog.
- **Out of scope:** unregistered/orphan `<dir>/.ymh/sessions.db` files — only
  registry rows are enumerated (§11.1).
- **Out of scope:** the test-isolation defect around `XDG_STATE_HOME` (§11.2).
- **Out of scope:** implementing `--new` (§11.3).
- `requirements_draft.txt` is untracked user notes and is not touched.

### 1.5 Terminology (pinned)

The user asked for an explicit workspace-vs-session distinction. This spec pins
it and uses it consistently:

- **Workspace** = a row in the shared `registry.db` `workspaces` table
  (`WorkspaceRecord`: `id`, `canonical_path`, `display_title`, optional
  `HostClaim`) plus, when live, its daemon. `00-architecture.md` §9.10 :1174:
  *"Workspace = the open set of sessions plus its host registration."* A
  workspace is **live** iff `record.host.has_value() && probeLiveness(record.id)
  == HostLiveness::Live` (the `DaemonSetScanner::scanOnce` predicate,
  `src/ui/supervisor_presence.cpp`). A workspace is **durable** regardless of
  liveness: the registry row exists whether or not a daemon is running (R10).
- **Session** = a row in a workspace's per-workspace `<workspace>/.ymh/sessions.db`
  `sessions` table (`SessionHeader`) plus its append-only `events` log. A
  session is **stored** (durable on disk) iff its row exists in that DB; it is
  **loaded/active** in the daemon iff the daemon's `SessionManager` holds it.
  The focused session is **supervisor-local** (`03 R12`), never registry state.
- **Live switcher (Ctrl-S)** = the `SwitcherSource::Live` projection: live
  workspaces only, sessions from the live `session.list`/stream cells.
- **History catalog (`/sessions`)** = the `SwitcherSource::History` projection:
  every **registered** workspace, sessions read from disk.

**The consequence the user asked to record:** a workspace whose registry row is
**pruned** (via `WorkspaceRegistry::removeWorkspace`, `src/registry/registry.cpp`,
which refuses while junction rows remain) loses its history from the `/sessions`
surface even if its `sessions.db` still exists on disk. Orphan DBs are not
discovered (§11.1).

---

## 2. Amendment register

**Rev 2 rows** resolve the first gate's H1/H2/M1–M5, SW1/`Connecting`, and
L1–L8 findings; **Rev 3 rows** resolve the second gate's MEDIUM-1..4 and LOWs;
**Rev 4 rows** resolve the third gate's MEDIUM (unreachable "workspace dir
gone" note) and LOW-1..6. Earlier rows are unchanged except where a later row
explicitly supersedes them.

| ID | Amended clause | Verified code anchors | New behaviour |
|---|---|---|---|
| S1 | Ctrl-S switcher scope | `SwitcherOverlayModel::open` (`src/ui/ui_model.cpp`); `SupervisorApp::attach_workspace`/`on_scan`/`on_link_state` (`src/ui/supervisor.cpp`); `DaemonSetScanner::scanOnce` (`src/ui/supervisor_presence.cpp`) | Live source copies only `WorkspaceModel::live` entries; `model_.workspaces` is pruned by the scanOnce predicate; `NotRunning`/`Unreachable` are not rendered (§3) |
| S1 (Rev 2, SW1/Connecting) | Display predicate | `ownership_mark(Connecting) == Unreachable` (`src/ui/ui_model.cpp`); `attach_workspace` sets `Connecting` (`src/ui/supervisor.cpp:617`); `WorkspaceModel` default `daemonStatus = Attached` (`include/ymh/ui/ui_model.hpp:232`) | Render iff `live && daemonStatus ∈ {Attached, Stopping}`; `live` becomes true only on `on_link_state(Attached)`; `attach_workspace` inserts `live = false` (§3.1, SW1/SW20) |
| S1 (Rev 2, M4) | `on_scan` re-attach | `attach_workspace` early return (`src/ui/supervisor.cpp:606-609`); `SupervisorConnection` boot_id rejection (`src/ui/supervisor_connection.cpp:204-207`) | Replace a connection whose pinned `boot_id` differs from the scan's; stop/erase/re-attach (§3.2 rule 9, SW22) |
| S1 (Rev 2, M5) | Eviction vs in-flight spawn | `evict_dead_workspaces`; `ensure_in_flight_` | Skip eviction for `ensure_in_flight_`; never erase `pending_resume_` for a spawn in flight (§3.2 rule 8, SW21) |
| S1 (Rev 2, L1) | Switcher source reset | `UiModel::openSwitcher` (`src/ui/ui_model.cpp:735-739`); `SwitcherOverlayModel::close` (`:807-811`) | `openSwitcher()` sets `switcher.source = Live`; `openHistory` sets `History` (§3.6, §4.3) |
| S1 (Rev 2, L3) | Cursor validation | `SwitcherOverlayModel::open` validates against `model.workspaces` (`src/ui/ui_model.cpp:795-804`) | Validate against the built node list; clear a dangling `cursor.workspace`/`cursor.session` (§3.3) |
| S2 (Rev 2, M2) | Read strategy / sidecar honesty | `apply_pragmas` + `exec_sql` (`src/session/session_persistence.cpp:214-222,548-558,640-657`); `errors.hpp` hierarchy | Disk read for every workspace via plain `openReadOnly`; SW8 corrected (may create `-shm`/`-wal`; `StoreError` on a non-writable dir) (§4.1, §4.4, 22-D6) |
| S2 (Rev 2, L8) | App-id gate | `if (app_id != 0 && app_id != kApplicationId)` (`src/session/session_persistence.cpp:659-662`) | App id `0` is accepted; otherwise must equal `0x594D4801` (§4.1) |
| S2 (Rev 2, L2) | History window title | `render_switcher` window title `"workspaces"` (`src/ui/ui_render.cpp:462`), content header `"Switcher"` (`:417`); `00 §20.24 :3001` | **Corrected in Rev 3:** Live window title stays `"workspaces"` and the content header row `"Switcher"`; History window title `"sessions"` (the Rev 2 row cited the wrong element) (§4.3) |
| S3 (Rev 2, H1) | Failure notices | `ensureCellIn` default-constructs `workspaces[workspace]` (`src/ui/ui_model.cpp:348-360`); `ErrorOccurred` → `ensureSession` (`:487,641-648`) | Workspace-independent `UiModel::notices` ring + `pushNotice`; `surface_notice` never calls `ensureSessionIn` for an unmodeled workspace (§3.6, §5.1, SW23) |
| S3 (Rev 2, H2) | Worker lifetime | `~SupervisorApp` (`src/ui/supervisor.cpp:245-256`); `enqueue` (`:596-604`) | Single owned `std::jthread`, joined in the destructor; stop-token check before `enqueue`; no detached threads (§5.1, SW24) |
| S4 (Rev 2, L7) | `--workspace` interaction | `resolve_workspace`/`run_supervisor_entry` (`src/cli/cli.cpp`) | The session's own workspace wins for the initial workspace/daemon; `--workspace` still governs config discovery; `root` override pinned (§6.1) |
| **S3 (Rev 3, MEDIUM-1)** | Resume success path | `ensureCellIn` default-constructs `workspaces[workspace]` (`src/ui/ui_model.cpp:348-360`); `resume_after_attach` success lambda | The success branch guards on `model_.workspaces.count(workspace) != 0`; else `surface_notice` + return; the failure branch uses `surface_notice`. No **resume** reply path injects a workspace (§5.2, SW25, SW-F17; `create_session` is a pre-existing unguarded path, §11.7) |
| **S2 (Rev 3, MEDIUM-2)** | Catalog reader lifetime | `~SupervisorApp` (`src/ui/supervisor.cpp:245-256`); member order `action_mutex_`/`actions_`/`screen_` (`:1663-1665`) | `SupervisorApp` owns `catalog_`; `~SupervisorApp` stops+joins it before other teardown; declaration order pinned (§4.2, §5.1, SW26, SW-F18) |
| **S2 (Rev 3, MEDIUM-3)** | Degradation taxonomy | `apply_pragmas` before the app-id/version/`verify_consistency` gates (`src/session/session_persistence.cpp:548-558,657-676`); `exec_sql` generic `StoreError` (`:214-222`) | Pre-flight classification (missing/not-file/unreadable/bad-magic) + pinned catch order; non-DB file → `(corrupt)`; residual → `(unavailable: <msg>)`; `(read-only location)` via `.ymh` writability (§4.4, SW-F19, SW-U4/SW-U20/SW-I4) |
| **S1/S2 (Rev 3, MEDIUM-4)** | Session ordering | `SupervisorApp::refresh_sessions` (`src/ui/supervisor.cpp`); `WorkspaceRegistry::listSessionsOrdered` | Workspaces title asc (both sources); **Live sessions keep `session.list` order; History `updated_at` desc / `id` asc** (§3.7, §4.3, SW17, 22-D1) |
| **S2 (Rev 3, LOW)** | 22-R3/§4.5 registry read | `SupervisorRunOptions::registry` (opened via `WorkspaceRegistry::open`) | The reader/resolver read the already-open registry handle; no second `openReadOnly` (§4.5, 22-R3) |
| **S4 (Rev 3, LOW)** | 16-D2 eager initial attach | `run_supervisor_entry` (`src/cli/cli.cpp`) | S4's eager `ensureRunning` for the resolved workspace is folded into 22-A7 and pinned (no longer "unresolved") (§6.1, §11.4) |
| **S2 (Rev 4, MEDIUM)** | "Workspace dir gone" row unreachable | `Impl::create` workspace-root check (`src/session/session_persistence.cpp:605-608`); **pre-flight step 0** | Add **pre-flight step 0** `is_directory(record.canonicalPath) == false → "workspace missing"`; the catch-order `StoreOpenError` "workspace root…" branch is **TOCTOU-only**. Every real input maps to exactly one note (§4.4, SW-F11, SW-U4, SW-U20, SW-I4) |
| **S2 (Rev 4, LOW-3)** | `WorkspaceHistory.live` derivation | `read_workspace_history` has no registry | Signature is `read_workspace_history(const WorkspaceRecord&, bool live)`; the caller (reader / CLI resolver) supplies `probeLiveness == Live` (§4.2, §4.6, §6.4) |
| **S1 (Rev 4, LOW-4)** | `eraseWorkspace` promotion rule | `UiModel::eraseWorkspace` | One rule: first remaining **renderable** (`live && Attached/Stopping`), else first by map key, else empty; §3.2 rule 7 and §3.5 step 4 agree |
| **S2 (Rev 4, LOW-5)** | Permission-dependent pre-flight | `::access(R_OK/W_OK)` | Pinned **non-root** assumption; SW-U4/SW-U20/SW-I7 `GTEST_SKIP` the `chmod 0000`/`0555` cases under root |
| **S3 (Rev 4, LOW-6)** | SW25 scope | `create_session` reply handler (`src/ui/supervisor.cpp:854-859`) | SW25 scoped to the resume path; the pre-existing unguarded `create_session` path recorded out-of-scope (§11.7) |
| **S4 (Rev 5, LOW-A)** | S4 spawn path wording | `run_supervisor_entry` eager `lifecycle.ensureRunning` (`src/cli/cli.cpp`) | §6.3 now says S4 reuses `resume_after_attach`/`pending_resume_`, **not** `ensure_workspace_running` (§6.1/§6.3 agree) |
| **16 (Rev 5, LOW-B)** | `NotRunning` supersession coverage | `16-daemon-ownership.md` §7.7 :2138-2139 | 22-S4 now also supersedes "the cwd workspace shows `NotRunning` if no daemon"; header `Supersedes:` extended |
| **S2 (Rev 5, LOW-C/F/H)** | §4.4 accuracy | `::access`/sidecar pre-flight | SW-I7 named in the non-root sentence; `"read-only location"` scoped to absent sidecars; raw-message claim qualified to the `"unavailable"` arm; "Unreadable" row scoped to a `chmod 0000` **file** |
| **S2 (Rev 5, LOW-G)** | §11.6 heading / log citation | — | §11.6 retitled "(Rev 5)"; Rev 4 log citation `§6.1` → `§6.4` |
| S1 | Eviction | `SupervisorApp::on_scan` (`src/ui/supervisor.cpp`) | New `UiModel::eraseWorkspace`; evict on scan tick; bounded by one `scan_interval`; deferred while `mode == ExitConfirm` (§3.2) |
| S1 | Ownership marks | `ownership_mark`, `OwnershipMark` (`src/ui/ui_model.cpp`, `include/ymh/ui/ui_model.hpp`); `render_switcher` (`src/ui/ui_render.cpp`) | Enum unchanged; only `Owned`/`Stopping` render in the Live source; `Unreachable` cannot persist (§3.4) |
| S1 | Ordering (Rev 1) | `ui_model.cpp` sort; `WorkspaceRegistry::listWorkspaces` (`src/registry/registry.cpp`) | **Superseded by the Rev 2 ordering row below** (Rev 1 said tie-break `WorkspaceId` and registry grouping) |
| S1 (Rev 2, M1) | Ordering — single rule | `ui_model.cpp` sort; `WorkspaceRegistry::listWorkspaces` (`src/registry/registry.cpp`) | **Superseded by the Rev 3 ordering row above** (Rev 2 said "sessions newest-first" for both sources; Live cannot re-order `session.list`) |
| S2 | `/sessions` command | `CommandRegistry::builtin`/`CommandContext` (`src/ui/command_registry.cpp`, `include/ymh/ui/command_registry.hpp`); `SupervisorApp::dispatch_command` (`src/ui/supervisor.cpp`) | New builtin command + `CommandContext::sessions` callback (§4) |
| S2 | Stored-session source | `SessionPersistence::list`/`openReadOnly` (`src/session/session_persistence.cpp`); `SessionHeader` (`include/ymh/session/session.hpp`); DDL `sessions` (`session_persistence.cpp`) | Background `SessionCatalogReader` opens each registered workspace's `sessions.db` read-only; groups by workspace, sessions newest-first (§4.1–§4.2) |
| S2 | Rendering | `render_switcher` (`src/ui/ui_render.cpp`); `SwitcherOverlayModel` (`include/ymh/ui/ui_model.hpp`) | Reuses the one switcher overlay via `SwitcherSource::History`; no parallel overlay (§4.3) |
| S3 | On-demand spawn | `SupervisorRunOptions::lifecycle` (`include/ymh/ui/supervisor.hpp`); `HostLifecycle::ensureRunning` (`src/host/workspace_host.cpp`) | Worker-thread `ensureRunning`, then `attach_workspace`; never on the UI thread (§5.1) |
| S3 | Resume | `protocol::method::kSessionResume` (`include/ymh/transport/protocol.hpp`); `ProtocolServer` handler (`src/transport/protocol_server.cpp`); `HostRuntime::resumeSession` → `AgentRegistry::resume` → `SessionManager::resumeSession` (`src/host/host_runtime.cpp`, `src/agent/agent_registry.cpp`, `src/session/session_manager.cpp`) | `/sessions` selection issues explicit `session.resume` after attach; Ctrl-S keeps the lazy `HostRuntime::ensureAgent` path (§5.2) |
| S4 | `--resume` in TUI | `run_supervisor_entry`/`add_common`/dispatch (`src/cli/cli.cpp`); `CliInvocation::session` (`include/ymh/cli/cli.hpp`) | Resolve the session's own workspace via the S2 catalog, set `pending_resume`, spawn/attach, resume; unknown id → exit 1 (§6) |

---

## 3. S1 — Live-only Ctrl-S switcher and daemon-death eviction

### 3.1 Two predicates, stated once

Two distinct predicates are involved and must not be conflated:

- **Live predicate (authoritative; the scanOnce predicate).**
  `record.host.has_value() && registry_->probeLiveness(record.id) ==
  HostLiveness::Live`, exactly as `DaemonSetScanner::scanOnce()`
  (`src/ui/supervisor_presence.cpp`) computes it. This governs **model
  membership and eviction**: a workspace is in `UiModel::workspaces` only while
  it passes this predicate.
- **Display predicate (switcher membership).** A workspace node is rendered in
  the Live source iff **`WorkspaceModel::live == true` AND `daemonStatus ∈
  {Attached, Stopping}`** (Rev 2, fixes the SW1/`Connecting` contradiction). The
  supervisor maintains `live` as follows:
  - `attach_workspace` sets **`live = false`** and `daemonStatus = Connecting`
    for a newly inserted workspace (Rev 2). It is *not* live until a link
    exists: `attach_workspace` is called for scanOnce-live specs, but a live
    claim does not imply a completed handshake, and `ownership_mark(Connecting)
    == Unreachable` (`src/ui/ui_model.cpp`), so treating `Connecting` as live
    would render `[unreachable]` in the Live source.
  - `on_link_state(Attached)` sets `live = true` (and `daemonStatus = Attached`);
    `on_link_state` never sets `live = true` for any other state.
  - `on_scan` sets `live = true` only for a workspace id that is **already in
    the live set AND whose existing connection is `Attached`** (Rev 2); a
    workspace that merely appears in the scan live set is attached with
    `live = false` and becomes live only when its handshake completes. This
    keeps the display predicate independent of scan/attach timing.
  - `on_link_state(Detached)` / `on_link_state(Dead)` sets `live = false`
    **immediately** (the workspace is hidden from the switcher at once, before
    the next scan tick);
  - `on_link_state(Connecting)` leaves `live` unchanged (a reconnect is in
    flight, but the node is hidden by the `daemonStatus` half of the display
    predicate);
  - eviction removes the entry entirely.

The display predicate is deliberately **stricter** than the live predicate: a
workspace can pass the live predicate (a live claim) while this supervisor's
handshake is `Connecting`, `Detached`, or `Dead`. Such a workspace is retained
in the model for reconnect (retry, R5) but is not rendered, because the user
cannot focus its sessions until a link exists. This is what makes
`Unreachable` unreachable in steady state (§3.4) without touching
`OwnershipMark`.

**Invariant SW1.** In the Live source, every rendered `WorkspaceNode` has
`WorkspaceModel::live == true` **and** `daemonStatus ∈ {Attached, Stopping}`.
**Invariant SW2.** Every entry in `UiModel::workspaces` passes the live
predicate at the time of the last completed `on_scan` (modulo entries whose
connection is `Connecting`, §3.2).
**Invariant SW20 (Rev 2).** A `WorkspaceModel` whose `daemonStatus` is
`Connecting`, `Detached`, or `Dead` is never rendered in the Live source, even
if `live == true`; `SwitcherOverlayModel::open` applies both halves of the
display predicate.

**Rev 2 note — the startup seed is not "live by construction" (L4).** The
startup fallback in `run_supervisor_entry` (`src/cli/cli.cpp`) seeds a
`SupervisorWorkspace` when `refreshed->host.has_value()` alone
(`src/cli/cli.cpp:398-408`) — i.e. on a claim, not on a completed handshake.
Spec 22 therefore drops the earlier "live by construction" claim: the seed and
every `attach_workspace` insert start `live = false` / `Connecting`, and become
renderable only when `on_link_state(Attached)` fires. If the handshake never
completes, the workspace is evicted by the normal scan path (it will not be in
the next scan's live set if the claim is gone; if the claim persists but the
handshake fails, it stays hidden and is retried, §3.2).

### 3.2 Eviction semantics (pinned)

`SupervisorApp::on_scan(std::vector<SupervisorWorkspace> live)` becomes:

```cpp
void on_scan(std::vector<SupervisorWorkspace> live) {
    enqueue([this, live = std::move(live)] {
        std::set<WorkspaceId> live_ids;
        for (const SupervisorWorkspace& spec : live) {
            live_ids.insert(spec.id);
        }
        // 1. replace a stale connection when the daemon restarted with a new
        //    boot_id (M4). `attach_workspace` early-returns when a connection
        //    exists, and `SupervisorConnection` rejects a handshake whose
        //    boot_id != expected_boot_id, so without this the workspace would
        //    stay Dead forever while still being in the scan live set.
        for (const SupervisorWorkspace& spec : live) {
            const auto connection = connections_.find(spec.id);
            const auto pinned = specs_.find(spec.id);
            if (connection == connections_.end() || pinned == specs_.end()) {
                continue;
            }
            if (pinned->second.boot_id != spec.boot_id) {
                connection->second->stop();
                connections_.erase(connection);
                specs_.erase(pinned);
                model_.eraseWorkspace(spec.id);   // re-attach fresh below
            }
        }
        // 2. attach new live workspaces (inserts start live=false/Connecting)
        for (const SupervisorWorkspace& spec : live) {
            attach_workspace(spec);
        }
        // 3. re-arm live only for entries whose link is already Attached (SW1)
        for (const WorkspaceId& id : live_ids) {
            const auto it = model_.workspaces.find(id);
            const auto connection = connections_.find(id);
            if (it != model_.workspaces.end() && connection != connections_.end() &&
                connection->second->state() == SupervisorLinkState::Attached) {
                it->second.live = true;
            }
        }
        // 4. evict the dead (S1); deferred during the exit prompt (SW4)
        if (model_.mode != UiMode::ExitConfirm) {
            evict_dead_workspaces(live_ids);
        }
    });
}

void evict_dead_workspaces(const std::set<WorkspaceId>& live_ids) {
    std::vector<WorkspaceId> doomed;
    for (const auto& [id, workspace] : model_.workspaces) {
        (void)workspace;
        if (live_ids.count(id) != 0) {
            continue;                                   // still live
        }
        if (ensure_in_flight_.count(id) != 0) {
            continue;                                   // (M5) spawn in flight
        }
        const auto connection = connections_.find(id);
        if (connection != connections_.end() &&
            connection->second->state() == SupervisorLinkState::Connecting) {
            continue;                                   // attach in flight; never evict
        }
        doomed.push_back(id);
    }
    for (const WorkspaceId& id : doomed) {
        if (auto connection = connections_.find(id); connection != connections_.end()) {
            connection->second->stop();                 // join the pump
            connections_.erase(connection);
        }
        specs_.erase(id);
        pending_creates_.erase(id);
        // (M5) never drop a resume for a spawn that is still in flight; the
        // guard above already skipped those ids, so this erase is safe here.
        pending_resume_.erase(id);
        model_.eraseWorkspace(id);                      // removes sessions + repairs focus
    }
    if (!doomed.empty()) {
        model_.dirty.markAggregate();
    }
}
```

Pinned rules:

1. **Eviction is scan-driven.** It runs on the UI thread inside the `on_scan`
   `enqueue` lambda. The UI thread performs **no** registry I/O for eviction:
   the live set is already computed by `DaemonSetScanner` on its own thread.
   **Invariant SW6.**
2. **Bound.** A dead workspace is evicted within one `scan_interval` (default
   2 s, `SupervisorRunOptions::scan_interval`). With the scanner disabled
   (`registry == nullptr` or `scan_interval <= 0`), `on_scan` never fires and
   there is no eviction; the model is then exactly the startup-attached set.
   Tests that need eviction must enable the scanner (they already construct a
   `DaemonSetScanner`, e.g. `ui_supervisor_pty_test.cpp`).
3. **Never evict an in-flight attach.** A `Connecting` connection is skipped
   (§3.2 rule in code). A `Connecting` entry that never reaches `Attached` is
   evicted on a later tick once its connection state is `Dead`/`Detached`.
4. **Deferred during the exit prompt.** While `model_.mode ==
   UiMode::ExitConfirm`, eviction is skipped for that tick so
   `ExitConfirmState::orphaning` cannot be invalidated mid-prompt. It resumes
   after the prompt resolves (`cancel_exit`/`confirm_exit`). **Invariant SW4.**
5. **Transient `Detached` is not death.** A workspace whose connection is
   `Detached` but which is still in the reported live set is retained; it is
   re-armed to `live = true` only once its `SupervisorConnection` returns to
   `Attached` (rule 3 of the code above). Only absence from the live set evicts.
6. **No registry write.** Eviction never calls `releaseHost`/`removeWorkspace`
   (R5/R10/R11). The daemon's claim is cleared by the daemon (or reaped by the
   next `ensureRunning`), never by the switcher. **Invariant SW6.**
7. **Eviction is observable.** `UiModel::eraseWorkspace` marks the aggregate
   dirty and, if the evicted workspace was active, repairs `activeWorkspaceId`
   by the **single rule in §3.5 step 4** (first remaining **renderable** workspace
   — `live && daemonStatus ∈ {Attached, Stopping}` — else the first remaining by
   map key, else empty) and clears the active session. §3.2 and §3.5 pin the
   same rule; the earlier "remaining live workspace" wording was ambiguous and
   is superseded (LOW-4, Rev 4).
8. **(M5, Rev 2) Never evict or clear a spawn in flight.** `evict_dead_workspaces`
   skips any id in `ensure_in_flight_`, so a scan tick landing between
   `ensure_workspace_running` and the worker's completion cannot erase
   `pending_resume_` or the workspace. This closes the window where the later
   `Attached` would consume nothing and silently drop the resume. **Invariant
   SW21.**
9. **(M4, Rev 2) Daemon restart replaces the stale connection.** When the scan
   reports a workspace whose `boot_id` differs from the connection's pinned
   `specs_.at(id).boot_id`, the stale `Dead` connection is stopped, erased, and
   re-attached. Without this, `attach_workspace`'s early return
   (`src/ui/supervisor.cpp`) plus `SupervisorConnection`'s
   `expected_boot_id` handshake rejection (`src/ui/supervisor_connection.cpp`)
   would leave the workspace hidden from Ctrl-S forever and would prevent
   `pending_resume_` from ever being consumed. **Invariant SW22.**

### 3.3 The overlay is a snapshot — live-set changes while open

`SwitcherOverlayModel::open` is a snapshot taken at open time. Pin:

- When the live set changes while `model_.mode == UiMode::Switcher` **and**
  `switcher.source == SwitcherSource::Live`, the supervisor re-runs
  `model_.switcher.open(model_)` (not `openSwitcher()`, so `mode`/`source` are
  not reset) after `on_scan`/`on_link_state` mutates the model. This rebuilds
  the node list so the overlay never displays a dead workspace.
- **Cursor validation against the node list (L3, Rev 2).** The shipped
  `SwitcherOverlayModel::open` validates the cursor against `model.workspaces`
  (`src/ui/ui_model.cpp:795-804`), not against the nodes it actually built. A
  workspace filtered out by the display predicate could therefore leave a
  dangling `cursor.workspace` until the next `moveDown`. Spec 22 pins: `open`
  and `openHistory` validate `cursor.workspace` against the **new node list**;
  if it is absent, reset to the active workspace when that workspace is present,
  else to the first node (or clear when empty). If `cursor.session` is not in
  the resolved node, clear it. This closes the stale-session-cursor window in
  `moveDown`/`moveUp` and makes the re-snapshot deterministic.
- If the re-snapshot leaves `workspaces` empty, the renderer's existing
  `"(no workspaces)"` row (`render_switcher`, `src/ui/ui_render.cpp`) is shown;
  Enter is a no-op (`focusSession`/`focusWorkspace` already no-op on a missing
  target).
- The **History** source is not re-snapshotted on the live set. It **is**
  rebuilt in place on every new catalog snapshot: `SupervisorApp::on_catalog_snapshot`
  (§4.6) stores the snapshot and, when `mode == UiMode::Switcher &&
  switcher.source == History`, calls `switcher.openHistory(model_)` — reusing the
  L3 cursor revalidation, preserving `filter`/`collapsed`, and never closing the
  overlay. While a rebuild is pending, the last-good snapshot stays displayed.
  Live-set changes only update the per-workspace `live` badge in the History
  render. **Invariant SW19** (with §4.2/§4.6).

**Invariant SW5.** A Live-source `SwitcherOverlayModel` never contains a node
whose `WorkspaceModel::live` is false, including immediately after a
live-set change.

### 3.4 Ownership marks: which remain reachable

`OwnershipMark` and `ownership_mark()` are **unchanged** (22-R1). Reachability
in the Live source is:

| Mark | Reachable? | Why |
|---|---|---|
| `Owned` | Yes | `DaemonStatus::Attached` → a live, linked daemon. |
| `Stopping` | Yes | `DaemonStatus::Stopping`, set by `UiEventAdapter::onHostNotice` on `HostNoticeKind::DaemonShuttingDown` (`src/ui/ui_event_adapter.cpp`) while the daemon still holds its sidecar lock → still passes the live predicate. |
| `NotRunning` | **No** | `NotRunning` means no live daemon (16 §7.6); such workspaces fail the live predicate and are never in `UiModel::workspaces` (22-S4). |
| `Unreachable` | **No (steady state)** | `Unreachable` is `Connecting`/`Detached`/`Dead`. `on_link_state` clears `live` on `Detached`/`Dead`, and `on_scan` evicts absent-from-live entries; a `Connecting` entry is transient and, if it never attaches, is evicted. The enum value and mapping remain, but no Live-source node renders `[unreachable]`. |

`render_switcher` (`src/ui/ui_render.cpp`) keeps `ownership_mark_name` for all
four values; only `Owned`/`Stopping` can occur in the Live source. In the
History source, a non-live workspace is **not** given an ownership mark at all —
it renders a distinct `[history]` marker (§4.3), so the mark vocabulary is not
reused for "no daemon".

**Invariant SW18.** `compute_orphaning_set`, `query_orphaning_view`,
`open_exit_prompt`, `count_sessions`, `count_running`
(`src/ui/supervisor.cpp`), the `host.ownership` wire path, and the daemon-side
owner watchdog (spec 16 §5.1) are structurally independent of `OwnershipMark`
and of `SwitcherOverlayModel`. The S1 display narrowing cannot change the
last-exit prompt or the watchdog. This is verified by reading, not assumed:
`compute_orphaning_set` iterates `connections_`; `open_exit_prompt` reads
`model_.workspaces`/`model_.sessions`; the watchdog is in `WorkspaceHost`.

### 3.5 `UiModel::eraseWorkspace` (new)

```cpp
// include/ymh/ui/ui_model.hpp
void UiModel::eraseWorkspace(const WorkspaceId& workspace);
```

Pinned behaviour (unit-testable, no registry/daemon access):

1. If no such workspace, no-op.
2. For every `SessionUiState` whose `workspace == workspace`, erase it from
   `sessions` (and its `DirtySet` entry via the next `takeDirtySessions`).
3. Erase the `WorkspaceModel` from `workspaces`.
4. If `activeWorkspaceId == workspace` (LOW-4, Rev 4 — single promotion rule):
   promote to the first remaining workspace that is **renderable in the Live
   source** (`live == true && daemonStatus ∈ {Attached, Stopping}`), in map-key
   order; if none is renderable, fall back to the first remaining workspace by
   map key; if none remains, set `{}`. Never promote a hidden `Connecting`
   workspace while a renderable one exists. §3.2 rule 7 references this rule.
5. If `mode == Switcher` and the switcher's cursor targets the erased
   workspace, re-snapshot (Live source) or clear the cursor (History source
   rebuilds on its own snapshot).
6. `dirty.markAggregate()`.

### 3.6 C++ interface sketch (S1)

```cpp
// include/ymh/ui/ui_model.hpp (additive)
struct WorkspaceModel {
    WorkspaceId              id;
    std::string              title;
    std::string              cwd;
    std::string              boot_id;
    DaemonStatus             daemonStatus = DaemonStatus::Attached;
    SessionId                activeSessionId;
    std::vector<SessionCell> sessions;
    bool                     live = false;  // (S1, Rev 2) true only once Attached;
                                            // hidden when false OR not Attached/Stopping

    [[nodiscard]] bool hasDaemon() const { return daemonStatus == DaemonStatus::Attached; }
};

// (H1, Rev 2) workspace-independent notice surface. Rendered in the status bar;
// never routed through ensureSessionIn/ensureCellIn for an unmodeled workspace.
struct UiNotice {
    std::string  text;
    std::int64_t atMs = 0;
};

struct UiModel {
    ...                                    // existing members unchanged
    std::deque<UiNotice> notices;          // bounded ring (kMaxNotices = 8)
    void pushNotice(std::string text);     // drops the oldest past the bound
};

enum class SwitcherSource : std::uint8_t {
    Live,      // Ctrl-S / Ctrl-P — live workspaces only (S1)
    History,   // /sessions — every registered workspace, from disk (S2)
};

struct WorkspaceNode {
    WorkspaceId                id;
    std::string                title;
    DaemonStatus               status = DaemonStatus::Connecting;
    OwnershipMark              mark = OwnershipMark::Unreachable;
    bool                       live = true;       // (S1) Live-source membership
    bool                       historyOnly = false; // (S2) registered, not live
    std::optional<std::string> note;              // (S2) degradation marker
    std::vector<SessionNode>   sessions;
};

class SwitcherOverlayModel {
public:
    std::vector<WorkspaceNode> workspaces;
    SwitcherCursor             cursor;
    std::optional<std::string> filter;
    std::set<WorkspaceId>      collapsed;
    SwitcherSource             source = SwitcherSource::Live;  // (S1)

    // (L1, Rev 2) `open` sets source = Live; `openHistory` sets source = History.
    // `close()` leaves source unchanged, so `UiModel::openSwitcher()` MUST set
    // `source = Live` (otherwise a Ctrl-S after `/sessions` would reuse History).
    void open(const UiModel& model);                  // Live source; live-only (S1)
    void openHistory(const UiModel& model);           // History source (S2)
    void close();
    void moveDown();
    void moveUp();
    void toggleExpand();
};

struct SessionNode {
    SessionId                  id;
    std::string                title;
    AgentState                 state = AgentState::Idle;
    bool                       attention = false;
    // (S2) History-source fields; default-initialized for the Live source.
    bool                       fromDisk = false;
    std::string                kind;                 // "root" | "fork" | "subagent"
    std::string                model;
    std::int64_t               updatedAt = 0;
    std::optional<SessionId>   parent;
};

// src/ui/supervisor.cpp (SupervisorApp, private)
void on_scan(std::vector<SupervisorWorkspace> live);           // attach + evict
void evict_dead_workspaces(const std::set<WorkspaceId>& live_ids);
void push_notice(std::string text);                            // (H1) model_.pushNotice
```

`SwitcherOverlayModel::open` copies a workspace entry only when **both**
`WorkspaceModel::live == true` **and** `daemonStatus ∈ {Attached, Stopping}`
(SW1/SW20). `openHistory` copies every catalog workspace (live or not).

### 3.7 Ordering (pinned; reconciles UI_SURFACE_INVENTORY)

**Decision 22-D1 (Rev 3 — one rule for workspaces, source-specific for
sessions; fixes M1 + MEDIUM-4).**

- **Workspace entries (both sources).** `title` ascending, **case-insensitive**
  (`tolower` on ASCII); when titles are equal (including two empty titles),
  `canonical_path` ascending (the Live source uses `WorkspaceModel::cwd`; the
  History source uses `WorkspaceHistory::canonicalPath`).
- **Sessions — Live source.** The supervisor does **not** re-order them; it
  renders them in the order the daemon returned them from `session.list`
  (unchanged, as today). `SupervisorApp::refresh_sessions`
  (`src/ui/supervisor.cpp`) appends `SessionCell`s in `reply.result` order, and
  the daemon's order is `WorkspaceRegistry::listSessionsOrdered` (junction
  `ordinal`, then store-only by `(createdAt, sessionId)`; 11-m2-errata §5.3
  D16). Re-sorting in the supervisor would diverge from the daemon's
  active-session marker and the `session.list` contract.
- **Sessions — History source.** `updated_at` descending, tie-broken by `id`
  ascending ("newest first"). This is the only source whose session order the
  supervisor owns.

This replaces the shipped title-only workspace sort, the Rev 1 History grouping
by registry `ORDER BY canonical_path`, and the Rev 2 over-generalized
"newest-first for both sources".

**What changes in the docs:** the comment in `src/ui/ui_model.cpp` claiming
title order "match[es] the registry's canonical-path order in practice" is
false in general and is superseded; `UI_SURFACE_INVENTORY.md` :59-60
(`"sorted by title"`) is amended to the rule above (refreshed by the separate
doc-sync pass, which now records the live-only switcher and `/sessions`; spec 22
itself does not edit it). The
registry's `ORDER BY canonical_path ASC` (`WorkspaceRegistry::listWorkspaces`,
`src/registry/registry.cpp`) remains the **grouping key / tie-break** and the
order in which `listWorkspaces()` returns rows; it is no longer the History
display order. Golden tests SW-G2 and SW-U6 assert title order; SW-G1 is
unaffected (single workspace).

---

## 4. S2 — `/sessions`: the stored-session catalog

### 4.1 Data source and schema (read from disk)

Sessions live per workspace at `<workspace>/.ymh/sessions.db`, owned by that
workspace's daemon (`00 §9.2`; `SessionPersistence`, `src/session/session_persistence.cpp`).
The `sessions` DDL (`session_persistence.cpp`, `kDdl`) is:

```sql
CREATE TABLE sessions (
    id TEXT PRIMARY KEY, cwd TEXT NOT NULL, created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL, title TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '', server_profile TEXT NOT NULL DEFAULT 'interactive',
    kind TEXT NOT NULL DEFAULT 'root', parent_session TEXT, seed_length INTEGER,
    metadata JSON, ... CHECK (kind IN ('root','fork','subagent')) ...);
```

`SessionPersistence::list()` selects `id, cwd, created_at, updated_at, title,
model, server_profile, kind, parent_session, seed_length, metadata` ordered by
`created_at, id`. It is exposed through `SessionHeader`
(`include/ymh/session/session.hpp`), which is the field set `/sessions` shows.

**Decision 22-D6 (Rev 2 — read strategy, fixes M2).** `/sessions` enumerates
**every row returned by `WorkspaceRegistry::listWorkspaces()`** and, for each,
opens `<canonical_path>/.ymh/sessions.db` with
**`SessionPersistence::openReadOnly`** (plain path,
`SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX`, no `SQLITE_OPEN_URI`) and calls
`list()`. The disk read is chosen for **every** registered workspace, live or
stopped, and is **not** substituted by a connection `session.list` for live
workspaces. Justification:

- `session.list` returns `SessionSummary{id, ordinal, archived, title, kind,
  updated_at_ms}` (`include/ymh/transport/protocol.hpp`), which lacks `model`,
  `parentSession`, and `createdAt` — three of the pinned §4.1 fields. A
  connection-only live path would create a field-parity fork.
- SQLite WAL gives a read-only reader a consistent snapshot while a live daemon
  writes, so a direct file read is safe.
- For a live workspace the `-shm`/`-wal` sidecars already exist (the daemon
  created them) and the workspace directory is writable (the daemon writes
  there), so the read cannot fail on permissions.
- No daemon is required, satisfying S2's "no daemon required to LIST".

**Honest sidecar behaviour (Rev 2, replaces the false SW8 claim).**
`openReadOnly` is **not** side-effect-free. `Impl::create(..., writable=false)`
still calls `apply_pragmas` (`src/session/session_persistence.cpp`), which runs
`PRAGMA foreign_keys=ON`, `PRAGMA busy_timeout`, `PRAGMA synchronous=NORMAL`,
and `PRAGMA journal_mode=WAL`; on a WAL database SQLite creates/attaches
`sessions.db-shm` and `sessions.db-wal` when they are absent. This is a
**metadata side effect, not a data write**: the `sessions`, `events`,
`session_leases`, and `session_snapshots` tables are never modified. The
catalog reader documents and accepts it for a writable workspace directory.

**Empirical evidence (Rev 2, SQLite 3.53.4, `sqlite3_open_v2`,
`SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX`, scratch DB under
`/tmp/opencode/`):**

| Setup | Result |
|---|---|
| cleanly-closed WAL DB, writable dir | open `SQLITE_OK`; pragmas `SQLITE_OK`; after the open, `db.sqlite-shm` (32768 B) and `db.sqlite-wal` (0 B) were created; `SELECT` returned rows |
| same DB, dir `chmod 0555` (read-only) | open `SQLITE_OK`, but `PRAGMA synchronous` / `PRAGMA journal_mode=WAL` / the first `SELECT` returned `SQLITE_READONLY` (8) "attempt to write a readonly database"; no sidecars created |
| `file:…?mode=ro&immutable=1` (with `SQLITE_OPEN_URI`), read-only dir | open `SQLITE_OK`; pragmas `SQLITE_OK`; `SELECT` returned rows; **no** sidecars created |
| `file:…?mode=ro&nolock=1` | open fails `SQLITE_CANTOPEN` (14) — **not used** |
| missing `sessions.db` (plain or URI) | open fails `SQLITE_CANTOPEN` (14) |
| URI `file:…` without `SQLITE_OPEN_URI` | open fails `SQLITE_CANTOPEN` (14) — a `file:` string is treated as a literal path |

Consequences pinned:

- A read in a **non-writable directory** throws a generic **`StoreError`**
  ("exec failed: attempt to write a readonly database") from `exec_sql`
  (`src/session/session_persistence.cpp:214-222`), **not** `StoreOpenError`.
  The §4.4 matrix therefore adds a `StoreError` catch and a "read-only
  location" row.
- `StoreError` is the base class of `StoreOpenError`, `CorruptionError`, and
  `SchemaVersionError` (`include/ymh/session/errors.hpp`), so the reader catches
  the specific types first and a trailing `StoreError` last.
- **Optional fast path (recorded, not mandatory, not baseline; gating
  tightened in Rev 3).** An implementation MAY open via
  `file:<abs-path>?mode=ro&immutable=1` with `SQLITE_OPEN_URI` to avoid sidecar
  creation on read-only directories, **only when all three hold**: (a) the
  workspace is **not live** (`record.host` absent, or `probeLiveness != Live`)
  — an actively-writing daemon can momentarily have an absent/zero-length `-wal`
  while a checkpoint is in flight, so the file-state check alone is unsound;
  (b) `<ws>/.ymh/sessions.db-wal` is absent or zero-length; and (c) the
  pre-flight magic check passed. `immutable=1` **must not** be used when a
  non-empty `-wal` exists: it skips WAL recovery and would silently miss
  committed frames from a crash. The baseline is plain `openReadOnly`, which
  recovers the WAL correctly and is what SW-I7 asserts; if the gating cannot be
  implemented, drop the fast path entirely.
- **Invariant SW8 (Rev 2, corrected).** `/sessions` opens `sessions.db`
  read-only and never writes table data; it takes no sidecar `flock`. It may
  create/attach `-shm`/`-wal` sidecar files as a documented SQLite side effect
  when the directory is writable, and it fails with `StoreError` (degraded, not
  fatal) when the directory is not writable. It never modifies the durable
  session/event data.
- It validates `PRAGMA application_id` — **0 is accepted, and otherwise must
  equal `0x594D4801`** (`if (app_id != 0 && app_id != kApplicationId)`,
  `src/session/session_persistence.cpp:659-662`; Rev 2 corrects the earlier
  "exactly `0x594D4801`" claim, L8) — and `user_version == kSchemaVersion`
  (1) exactly, then runs `verify_consistency` (foreign-key check + sequence
  monotonicity). A foreign/newer/older DB throws `SchemaVersionError`; a
  corrupt DB throws `CorruptionError`; a missing workspace dir or DB throws
  `StoreOpenError`; a non-writable directory throws `StoreError`. All are caught
  per workspace (§4.4).
- The `lock_path` is not needed read-only; `PersistenceConfig::db_path` is
  `<canonical>/.ymh/sessions.db` and `lock_path` is
  `<canonical>/.ymh/sessions.lock` for symmetry (never opened read-only).

**Fields shown per session (pinned), from `SessionHeader`:**

| Column | Source field | Display |
|---|---|---|
| Session id | `SessionHeader::id` | short id (first 8 chars) when the title is empty |
| Title | `SessionHeader::title` | verbatim (may be empty → short id) |
| Kind | `SessionHeader::kind` (`root`/`fork`/`subagent`) | `root`/`fork`/`sub` |
| Model | `SessionHeader::model` | verbatim (may be empty → `(default)`) |
| Updated | `SessionHeader::updatedAt` (epoch ms) | relative ("3m", "2h", "4d") |
| Fork origin | `SessionHeader::parentSession` (+`seedLength`) | `fork←<parent-short>` when `kind == fork` |

`serverProfile`, `cwd`, `createdAt`, `metadata` are not displayed (the
workspace group already conveys `cwd`; the rest are not user-facing). This is a
pinned list; adding a column is a proposal (§11.5).

### 4.2 Background snapshot / cache (never blocks the UI)

**Decision 22-D2.** A new `SessionCatalogReader` owns one worker thread that
builds an immutable `SessionCatalogSnapshot` and delivers it to the UI thread
via a sink (the same post-to-UI-thread idiom as `DaemonSetScanner`:
`Sink` callback → `SupervisorApp::enqueue`). The UI thread never opens SQLite.

```cpp
// include/ymh/ui/session_catalog.hpp (new)
struct SessionHistoryEntry {
    SessionId                  id;
    std::string                title;
    std::string                kind;        // "root" | "fork" | "subagent"
    std::string                model;
    std::int64_t               createdAt = 0;
    std::int64_t               updatedAt = 0;
    std::optional<SessionId>   parent;
    std::optional<std::size_t> seedLength;
};

struct WorkspaceHistory {
    WorkspaceId                       id;
    std::string                       title;
    std::string                       canonicalPath;
    bool                              live = false;   // passed in by the caller:
                                                     // registry probeLiveness at
                                                     // snapshot time (the read
                                                     // helper has no registry)
    std::optional<std::string>        note;           // nullopt = read OK
    std::vector<SessionHistoryEntry>  sessions;       // updatedAt desc, id asc
};

struct SessionCatalogSnapshot {
    std::vector<WorkspaceHistory> workspaces;  // storage order = registry order
                                               // (canonical_path); the renderer
                                               // sorts by title (22-D1)
    bool                          complete = false; // false = >=1 workspace failed
    std::int64_t                  capturedAtMs = 0;
    std::uint64_t                 generation = 0;    // monotonic; stale replies dropped
};

class SessionCatalogReader {
public:
    using Sink = std::function<void(SessionCatalogSnapshot)>;
    SessionCatalogReader(WorkspaceRegistry& registry, Sink sink,
                         std::chrono::milliseconds refresh_interval);
    ~SessionCatalogReader();
    void start();
    void stop();
    void refreshNow();          // request an immediate rebuild (thread-safe)
private:
    void loop();                // one build per interval, or on refreshNow
    // Per-workspace read; delegates to the shared free function in §4.6.
    // `live` is supplied by the caller (the reader has the registry; the read
    // helper does not): `registry.probeLiveness(record.id) == HostLifecycle::Live`.
    WorkspaceHistory read_workspace(const WorkspaceRecord& record, bool live) const;
};
```

- **Lifetime (MEDIUM-2, Rev 3).** `SupervisorApp` **owns** the reader as a
  member (`std::unique_ptr<SessionCatalogReader> catalog_`) and its
  `~SupervisorApp` explicitly `stop()`s + joins it **in the destructor body,
  before any member teardown**, exactly as `ensure_worker_` (§5.1). The reader's
  sink is `SupervisorApp::enqueue`, which locks `action_mutex_` and reads
  `screen_`; if the reader were allowed to outlive `action_mutex_` the in-flight
  sink call would be a use-after-free. Member declaration order is pinned:
  `catalog_` and `ensure_worker_` are declared **after** `action_mutex_`,
  `actions_`, and `screen_` (`src/ui/supervisor.cpp:1663-1665`), so reverse
  destruction would destroy them first even if the explicit join were ever
  bypassed. **Invariant SW26.** The reader's own destructor `stop()`s (it must
  be idempotent: `stop()` after an explicit `stop()`/`join()` is a no-op).
- **Cadence.** Default `refresh_interval = 15 s` (new
  `SupervisorRunOptions::catalog_refresh_interval`; tests shrink it). Opening
  `/sessions` calls `refreshNow()`. `refreshNow` coalesces: at most one rebuild
  in flight; a request during a build sets a pending flag consumed at the end.
- **Staleness.** The snapshot carries `capturedAtMs`; the overlay footer renders
  `stored sessions · captured <relative> ago` and, when `complete == false`,
  `· partial`. A stale snapshot is displayed as-is (never blanked) until a newer
  one arrives. **Invariant SW19.**
- **Generation.** Each delivered snapshot bumps `generation`; the UI stores the
  latest and ignores out-of-order deliveries (the reader is single-threaded, so
  this is defensive).
- **Loading state.** Before the first snapshot, `UiModel.catalog.loaded ==
  false`; `/sessions` opens the History overlay with a `"loading stored
  sessions…"` placeholder. **Invariant SW7.**
- **Cost note (recorded, not a defect).** `openReadOnly` runs
  `verify_consistency`, which scans `events`; a large workspace DB makes one
  rebuild O(events). This is acceptable on the worker at a 15 s cadence but is
  the reason the cadence is not 2 s like the daemon scanner. If profiling shows
  it too heavy, a lighter read-only open that skips `verify_consistency` is a
  future proposal (§11.5), not part of S2.

### 4.3 Rendering: extend, do not duplicate (O14 / C4)

**Decision 22-D3.** `/sessions` reuses `SwitcherOverlayModel` and
`render_switcher` (`src/ui/ui_render.cpp`) with `source = SwitcherSource::History`.
There is **no** second overlay, no second tree renderer, and no new
`OverlayManager` layer. **Invariant SW11.**

- `CommandContext::sessions` → `SupervisorApp::open_sessions()` sets
  `model_.switcher.source = History; model_.switcher.openHistory(model_);
  model_.mode = UiMode::Switcher;` and calls `catalog_->refreshNow()`.
  `openHistory` sets `source = History`.
- Ctrl-S / Ctrl-P keep `source = Live`; `UiModel::openSwitcher()` **sets
  `switcher.source = Live`** (L1, Rev 2) so a Ctrl-S after `/sessions` cannot
  reuse `History` (`SwitcherOverlayModel::close` does not reset `source`,
  `src/ui/ui_model.cpp`). (`/switch` is mentioned in spec 10 §7.4 but is **not**
  in the shipped `CommandRegistry::builtin`; S2 does not add it.)
- **`handle_switcher` source branch (L6, Rev 2).** `SupervisorApp::handle_switcher`
  (`src/ui/supervisor.cpp`) branches on `model_.switcher.source` on Enter:
  - `Live` → the existing behavior: `cursor.session` set → `UiModel::focusSession`;
    else `UiModel::focusWorkspace`; then close, `mode = Conversation`.
  - `History` → `cursor.session` set → `SupervisorApp::resume_from_history(workspace,
    session)` (§5); else focus the workspace (no resume); then close,
    `mode = Conversation`. `Esc`/`Ctrl+C` close and reset `mode`; the next
    Ctrl-S sets `source = Live` via `openSwitcher()`.
  - `j`/`k`/`↓`/`↑`/Tab are source-independent (navigation on the node tree).
- `render_switcher` branches only on `switcher.source` for the window title,
  the workspace-row tail, and the footer. **Titles (L2, Rev 3 — corrected
  evidence).** The shipped `render_switcher` (`src/ui/ui_render.cpp`) has a
  **window title** `ftxui::window(ftxui::text("workspaces"), …)` (line 462) and
  a bold **content header row** `ftxui::text("Switcher")` (line 417); the Rev 2
  claim that the window title is `"Switcher"` was wrong. Pin: the Live window
  title stays **`"workspaces"`** (shipped, not superseded) and the Live content
  header row stays **`"Switcher"`** (matching `00 §20.24 :3001`'s diagram, not
  superseded); the History window title is **`"sessions"`** and its content
  header row is `"sessions"`. `00 §20.24 :3001` is **not** superseded by this.
  - **Live:** `<glyph> [<ownership_mark_name>]` (unchanged).
  - **History:** live workspace → `<glyph> [owned]`; non-live → `· [history]`.
    A non-live workspace's sessions are always listed (the whole point), so
    `collapsed` still applies but defaults to expanded for non-live groups.
  - **History session leaf:** `[<title|short-id> · <kind> · <model> ·
    <relative updated> · fork←<parent-short>]`. The Live leaf format is
    unchanged (`[<title> <state glyph>[!]]`).
  - **Footer:** Live = existing hint; History = `stored sessions · captured
    <relative> ago [· partial] · r refresh · Enter resume · Esc close`.
- **Ordering (M1 + MEDIUM-4, Rev 3).** Workspace entries in **both** sources are
  ordered by `title` ascending (case-insensitive), tie-broken by `canonical_path`
  ascending (22-D1). The earlier Rev 1 "History groups by registry
  `ORDER BY canonical_path`" is withdrawn, and the Rev 2 "sessions newest-first
  for both sources" is corrected: **Live sessions keep the daemon's
  `session.list` order** (the supervisor never re-sorts them), while **History
  sessions** are `updatedAt` descending, tie-broken by `id` ascending. Golden
  tests SW-G2 and SW-U6 assert title order for workspaces and the per-source
  session order.
- **Empty state.** A workspace with zero stored sessions renders a single
  `(no stored sessions)` dim leaf, not an empty group.

### 4.4 Graceful degradation matrix (pinned)

Every failure is **per workspace** and never propagates: one bad DB must not
suppress the others, crash the TUI, or block. **Invariants SW9, SW10.**

**Why a pre-flight (MEDIUM-3, Rev 3).** The exception taxonomy cannot
distinguish the cases the matrix needs: `apply_pragmas` runs **before** the
app-id/version/`verify_consistency` gates (`src/session/session_persistence.cpp`),
and `exec_sql` maps *any* failed pragma to a generic `StoreError`
(`:214-222`). Empirically (SQLite 3.53.4): a non-SQLite file opens `SQLITE_OK`
and then `PRAGMA synchronous` returns `SQLITE_NOTADB` (26) → generic
`StoreError`; a `chmod 0000` file returns `SQLITE_CANTOPEN` (14) at open —
**identical to a missing file**; a `chmod 0555` directory returns
`SQLITE_READONLY` (8) → generic `StoreError`. So `read_workspace_history`
**pre-flights** the path and then maps the residual exceptions.

**Pre-flight (before any SQLite open), in order (Rev 4 — step 0 added so every
real input maps to exactly one note):**

0. `std::filesystem::is_directory(record.canonicalPath, ec)` is false →
   `"workspace missing"`. This mirrors `SessionPersistence::Impl::create`
   (`src/session/session_persistence.cpp:605-608`, `StoreOpenError("workspace
   root does not exist or is not a directory…")`) but runs **before** the DB
   existence check. Without step 0, a deleted/pruned workspace directory makes
   `exists(<ws>/.ymh/sessions.db)` false and would be mislabeled
   `"no sessions.db"`; with it, the `StoreOpenError` "workspace root…" branch is
   **TOCTOU-only** (the directory disappearing between this check and the
   open).
1. `std::filesystem::exists(db_path, ec)` is false → `"no sessions.db"`.
2. `std::filesystem::is_regular_file(db_path, ec)` is false → `"not a file"`.
3. `::access(db_path.c_str(), R_OK) != 0` → `"unreadable"`.
4. Read the first 16 bytes; if they are not exactly `"SQLite format 3\0"`
   → `"corrupt"`. (A zero-byte or truncated file fails this check.) This is
   what makes the common "corrupt/non-DB file" case honest; the pre-flight is
   cheap and needs no `SessionPersistence` API change.
5. Otherwise open read-only and map exceptions per the catch order below.

**Non-root assumption (LOW-5, Rev 4).** Steps 3 and the read-only heuristic
below use `::access(R_OK)`/`::access(W_OK)`, which a **root** process bypasses
(root ignores DAC permission bits, and can open a `chmod 0000` file). The
permission-dependent notes (`"unreadable"`, `"read-only location"`) are
therefore pinned **under the assumption that the catalog reader runs as a
non-root user** — true for the shipped CLI and the test runner (`id -u != 0`).
The missing / not-a-file / bad-magic notes are uid-independent. SW-U4/SW-U20
**and SW-I7** (the `chmod 0555` directory case) must `GTEST_SKIP` the
permission-dependent cases when `::geteuid() == 0` (or assert the
root-tolerant outcome) rather than fail.

| Condition | Detected by | `WorkspaceHistory.note` | Render |
|---|---|---|---|
| Workspace dir gone | **pre-flight 0** | `"workspace missing"` | workspace row with `[history]` + `(workspace missing)` leaf |
| `sessions.db` missing | pre-flight 1 (also `SQLITE_CANTOPEN` at open) | `"no sessions.db"` | `(no stored sessions)` leaf |
| Not a regular file | pre-flight 2 | `"not a file"` | `(not a file)` leaf |
| Unreadable (`chmod 0000` **file** in a traversable directory, EACCES on the file) | pre-flight 3 (indistinguishable from missing at the SQLite layer — `SQLITE_CANTOPEN`); non-root assumption above | `"unreadable"` | `(unreadable)` leaf |
| **Corrupt / non-DB / zero-byte (bad magic)** | pre-flight 4 | `"corrupt"` | `(corrupt)` leaf |
| **Corrupt (valid magic, FK/sequence)** | `verify_consistency` → `CorruptionError` | `"corrupt"` | `(corrupt)` leaf |
| Schema mismatch | `SchemaVersionError` (app id ≠ 0/`0x594D4801`, or `user_version != 1`) | `"schema mismatch"` | `(schema mismatch)` leaf |
| **Read-only location / cannot create sidecar** (only when the `-shm`/`-wal` sidecars are **absent**; an existing sidecar makes the read succeed) | residual `StoreError` **and** `::access(<ws>/.ymh, W_OK) != 0` **and** no `-shm`/`-wal` present; non-root assumption above | `"read-only location"` | `(read-only location)` leaf |
| **Other open/pragma/query failure** (residual `StoreOpenError`/`StoreError`/`std::exception`) | catch order below; the `StoreOpenError` "workspace root…" branch is **TOCTOU-only** after pre-flight 0 | `"unavailable: <raw sqlite message>"` | `(unavailable)` leaf |
| Registry read fails | `WorkspaceRegistry` `listWorkspaces` throws | snapshot not built; keep last snapshot | footer `partial`; last-good catalog retained |
| Reader thread exception | caught in `loop()` | — | keep last snapshot; log; retry next tick |

**Catch order (pinned).** After the pre-flight, `read_workspace_history` calls
`openReadOnly`/`list()` inside one `try` and catches, in order:
`SchemaVersionError` → `"schema mismatch"`; `CorruptionError` → `"corrupt"`;
`StoreOpenError` → `"unavailable: <what()>"`; `StoreError` → if
`::access(<ws>/.ymh, W_OK) != 0` then `"read-only location"` else
`"unavailable: <what()>"`; `std::exception` → `"unavailable: <what()>"`.
Because `StoreOpenError`, `CorruptionError`, and `SchemaVersionError` derive
from `StoreError` (`include/ymh/session/errors.hpp`), the specific types must
come first. Only the **`"unavailable"` arm** includes the raw SQLite message
(so an unexpected case is still diagnosable); the **`"read-only location"` arm
deliberately drops it** and renders the fixed note (LOW-F, Rev 5). The
`StoreOpenError` "workspace root…" branch is reachable only via **TOCTOU**
(the directory vanishing between pre-flight 0 and the open); it is not the
workspace-missing path (pre-flight 0 is).

**Read-only-location nuance (LOW-D, Rev 5).** The `"read-only location"` arm
fires only when the `-shm`/`-wal` sidecars are **absent**: if a sidecar already
exists (e.g. the daemon created it before exiting), the read-only open can
attach it without creating a file and the read **succeeds**. So a non-writable
`.ymh` directory is not sufficient by itself; the note is
`"read-only location"` only for a non-writable directory with no existing
sidecar. (The baseline read may still create the sidecars in a writable
directory — SW8 — which is why this arm is the narrow case.)

**Permission scope of the `"unreadable"` row (LOW-H, Rev 5).** `"unreadable"`
is pinned for a `chmod 0000` **file** in a **traversable** directory (pre-flight
3). A `chmod 0000` workspace **directory** (or an unsearchable `.ymh`) instead
makes pre-flight 0/1 fail — `is_directory(record.canonicalPath)` false →
`"workspace missing"`, or `exists(db_path)` false → `"no sessions.db"`. This is
one note per input and never a crash; the row is deliberately narrow.

**Explicitly indistinguishable (share a note).** A `chmod 0000` file and a
missing file both surface `SQLITE_CANTOPEN` at the SQLite layer; the pre-flight
`exists`/`access` checks are what separate them (`"no sessions.db"` vs
`"unreadable"`). A `SQLITE_NOTADB` that slips past the magic check (e.g. a torn
write that changed the first 16 bytes after the check) is `"unavailable"`, not
`"corrupt"`; this is accepted and the raw message is shown.

The snapshot is `complete = false` iff any workspace has a `note`. The UI never
throws for a catalog problem; the only user-visible effect is the marker and the
`partial` footer. **Invariant SW10.**

### 4.5 Scope boundary: registered workspaces only (pinned)

`/sessions` enumerates **`registry.db` `workspaces` rows only**. It does not
scan the filesystem for orphan `<dir>/.ymh/sessions.db` files. **Invariant
SW9.** The enumeration reads `WorkspaceRegistry::listWorkspaces()` through the
already-open `SupervisorRunOptions::registry` (opened by the CLI with
`WorkspaceRegistry::open`); reads take no `flock` and see a WAL snapshot
(`include/ymh/registry/registry.hpp`), so the reader does **not** open a second
registry handle. Consequence (recorded, as the user asked): a workspace whose
registry row is pruned via `removeWorkspace` (`src/registry/registry.cpp`; it
refuses while junction rows remain) disappears from `/sessions` even if its
`sessions.db` survives on disk. Recovering such history would require an
explicit "import workspace" action (out of scope; §11.1).

### 4.6 C++ interface sketch (S2)

```cpp
// include/ymh/ui/command_registry.hpp (additive)
struct CommandContext {
    ...
    std::function<void()> context;
    std::function<void()> sessions;   // (S2) open the History catalog overlay
};

// src/ui/command_registry.cpp (builtin)
registry.add(Command{"sessions", "list stored sessions for every workspace",
    [](CommandContext& c, const std::string&) { if (c.sessions) c.sessions(); }});

// include/ymh/ui/session_catalog.hpp (additive, Rev 2; signature Rev 4) —
// shared read helper. Used by SessionCatalogReader (worker) and by the CLI's
// --resume resolver (§6.4). The read helper has no registry, so the caller
// supplies liveness; the returned `WorkspaceHistory.live` is set from it.
[[nodiscard]] WorkspaceHistory read_workspace_history(const WorkspaceRecord& record,
                                                      bool live);

// src/ui/supervisor.cpp (SupervisorApp, private)
void open_sessions();
// UI thread. Stores the snapshot into model_.catalog and, if the History
// overlay is open, rebuilds it in place (LOW, Rev 3):
//   if (model_.mode == UiMode::Switcher &&
//       model_.switcher.source == SwitcherSource::History) {
//       model_.switcher.openHistory(model_);   // re-snapshot; cursor revalidated (L3)
//   }
// While a rebuild is pending (catalog_->refreshNow() in flight) the overlay
// keeps the last-good snapshot and the footer shows `captured <relative> ago`;
// a new snapshot never closes the overlay and never resets `filter`/`collapsed`.
void on_catalog_snapshot(SessionCatalogSnapshot snapshot);

// include/ymh/ui/ui_model.hpp (additive)
struct SessionCatalogModel {
    std::vector<WorkspaceHistory> workspaces;
    bool                          loaded = false;
    bool                          complete = false;
    std::int64_t                  capturedAtMs = 0;
    std::uint64_t                 generation = 0;
};
struct UiModel { ...; SessionCatalogModel catalog; };
```

---

## 5. S3 — Selecting a session in a non-running workspace

### 5.1 Threading and blocking model (pinned)

`HostLifecycle::ensureRunning` blocks: on the spawn path it polls for the
daemon's claim and handshakes for up to **10 s**
(`HostLifecycle::spawnAndAttach`, `src/host/workspace_host.cpp`), then a further
up-to-5 s winner-attach window. It **must never run on the UI thread**.
**Invariant SW12.** Pinned model:

**Notice surfacing (pinned, H1).** S3/S4 failures are surfaced through **two**
paths by a private
`surface_notice(const WorkspaceId& workspace, const SessionId& session, std::string text)`:

1. If `model_.sessions.count(session) != 0` (the session is modeled), reuse the
   existing error path `model_.apply(UiEvent{ErrorOccurred{session, std::move(text)}})`,
   which appends a `ConversationRole::System` entry (`UiModel::apply`,
   `src/ui/ui_model.cpp`) — the same mechanism `context.rename_session` uses
   (`src/ui/supervisor.cpp`).
2. Otherwise (the primary S3 case: a **non-live** workspace, so neither the
   workspace nor the session is in the model), call
   `model_.pushNotice(std::move(text))` — the workspace-independent ring
   rendered in the status bar (§3.6). The notice is visible while
   `model_.workspaces` is **unchanged**.

**Pinned prohibition (H1).** `surface_notice` **must not** call
`ensureSessionIn`/`ensureCellIn` for an unmodeled workspace. Those functions
default-construct `workspaces[workspace]` (`src/ui/ui_model.cpp:348-360,367-382`)
with `daemonStatus = Attached` (`include/ymh/ui/ui_model.hpp:232`) and (Rev 2)
`live = false`; even with `live = false`, the phantom is a latent SW2 violation
and a data-model corruption (it would also be inserted by any later
`ErrorOccurred` for the same session via `ensureSession(e.session)`,
`src/ui/ui_model.cpp:487`). **Invariant SW23.** A failure notice must be
delivered without adding a workspace.

```cpp
// include/ymh/ui/ui_model.hpp (additive, Rev 2)
static constexpr std::size_t kMaxNotices = 8;
void UiModel::pushNotice(std::string text);   // append, drop oldest past kMaxNotices,
                                              // dirty.mark(UiDirtyFlag::Status)
// render_status (src/ui/ui_render.cpp): append the newest notice to the left
// segment, after the active session's status.note; empty when notices is empty.
```

```cpp
// src/ui/supervisor.cpp (SupervisorApp) — H2 (Rev 2): ONE owned jthread, joined
// in the destructor. No detached threads, no raw `this` outliving the object.
struct EnsureRequest {
    WorkspaceId workspace;
    SessionId   resume;
};

// members
std::jthread               ensure_worker_;
std::mutex                 ensure_mutex_;
std::condition_variable    ensure_cv_;
std::deque<EnsureRequest>  ensure_requests_;   // coalesced per workspace
std::set<WorkspaceId>      ensure_in_flight_;  // UI-thread view

void ensure_workspace_running(const WorkspaceId& workspace, const SessionId& resume) {
    if (options_.lifecycle == nullptr) {
        surface_notice(workspace, resume, "cannot start workspace: lifecycle unavailable");
        return;
    }
    pending_resume_[workspace] = resume;   // UI thread; last selection wins (SW14)
    if (ensure_in_flight_.count(workspace) != 0) {
        return;                            // a spawn is already in flight
    }
    ensure_in_flight_.insert(workspace);
    {
        std::lock_guard lock(ensure_mutex_);
        std::erase_if(ensure_requests_,
                      [&](const EnsureRequest& r) { return r.workspace == workspace; });
        ensure_requests_.push_back(EnsureRequest{workspace, resume});
    }
    ensure_cv_.notify_all();
}

void ensure_worker_loop(std::stop_token stop) {   // body of ensure_worker_
    while (!stop.stop_requested()) {
        EnsureRequest request;
        {
            std::unique_lock lock(ensure_mutex_);
            ensure_cv_.wait(lock, [&] {
                return stop.stop_requested() || !ensure_requests_.empty();
            });
            if (stop.stop_requested()) {
                return;
            }
            request = ensure_requests_.front();
            ensure_requests_.pop_front();
        }
        std::optional<SupervisorWorkspace> spec;
        std::string error;
        try {
            AttachResult attach = options_.lifecycle->ensureRunning(
                ymh::WorkspaceId{request.workspace.value}, options_.identity);
            (void)attach;   // dropped (RAII close): attach_workspace makes its own
                            // SupervisorConnection. The daemon survives; the
                            // supervisor presence keeps it owned.
            spec = workspace_spec_from_registry(request.workspace);  // registry read is thread-safe
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (stop.stop_requested()) {
            return;   // (H2) never touch `this` after a stop request
        }
        enqueue([this, request, spec, error] {
            ensure_in_flight_.erase(request.workspace);
            if (!spec.has_value()) {
                pending_resume_.erase(request.workspace);
                surface_notice(request.workspace, request.resume,
                               "cannot start workspace: " + error);
                return;
            }
            attach_workspace(*spec);   // connection reaches Attached asynchronously;
                                       // on_link_state consumes pending_resume_
        });
    }
}

// started in run() before run_loop():
ensure_worker_ = std::jthread([this](std::stop_token stop) { ensure_worker_loop(stop); });

// ~SupervisorApp (Rev 2 H2; Rev 3 MEDIUM-2): stop + join EVERY thread-owning
// member BEFORE any other member teardown. `catalog_`'s sink is `enqueue`,
// which locks `action_mutex_`; both joins must precede its destruction.
~SupervisorApp() override {
    if (catalog_ != nullptr) { catalog_->stop(); }   // (Rev 3) joins the reader
    ensure_worker_.request_stop();
    ensure_cv_.notify_all();
    if (ensure_worker_.joinable()) {
        ensure_worker_.join();
    }
    if (scanner_ != nullptr) { scanner_->stop(); }
    if (presence_.has_value()) { presence_->deregister(); }
    for (auto& [id, connection] : connections_) { (void)id; connection->stop(); }
}

// member declaration order (Rev 3, MEDIUM-2): both thread-owning members are
// declared AFTER action_mutex_/actions_/screen_ so reverse destruction would
// destroy them first even without the explicit joins.
std::unique_ptr<SessionCatalogReader> catalog_;   // after screen_/actions_/action_mutex_
std::jthread                          ensure_worker_;
```

- **One owned worker, no detached threads (H2).** A spawn request is queued and
  coalesced per workspace; `ensure_in_flight_` de-duplicates (mirrors the
  existing `pending_creates_` idiom in `create_session`). **Invariant SW14.**
- **Lifetime (H2).** `~SupervisorApp` calls `request_stop()`, `notify_all()`,
  and `join()` **before** any member teardown. The worker checks
  `stop.stop_requested()` after `ensureRunning` returns and **before** `enqueue`,
  so it never touches `this` (which owns `action_mutex_`/`screen_`,
  `src/ui/supervisor.cpp:596-604`) after the stop request. **Invariant SW24.**
- **Bounded exit wait (H2).** The join waits for the in-flight
  `ensureRunning` to return; the worst case is the lifecycle's own bound
  (10 s spawn poll + 5 s winner window, `src/host/workspace_host.cpp:1063-1102`).
  This is a bounded quit delay, not a UAF. A future cancellation hook in
  `HostLifecycle` could shorten it (§11.5). A single worker also serialises
  spawns; that is acceptable (spawns are rare and per-workspace).
- **Catalog reader lifetime (MEDIUM-2, Rev 3).** `SupervisorApp` owns the
  `SessionCatalogReader` as `catalog_`; `~SupervisorApp` calls `catalog_->stop()`
  (idempotent, joins the reader thread) **first**, before `ensure_worker_`'s
  join and before any member teardown. `catalog_` is declared after
  `action_mutex_`/`actions_`/`screen_` so reverse destruction also orders it
  first. Without this, a quit during a catalog build would let the reader's sink
  (`enqueue` → `action_mutex_`) run after `action_mutex_` is destroyed.
  **Invariant SW26.**
- `attach_workspace` is always called on the UI thread (it mutates the model
  and `connections_`).
- On `SupervisorLinkState::Attached`, `on_link_state` (existing) calls
  `refresh_sessions`; spec 22 adds: if `pending_resume_` holds this workspace,
  consume it and call `resume_after_attach` (§5.2).
- `SupervisorRunOptions` gains `ensure_running_timeout` (default `12'000` ms;
  tests shrink) as documentation of the bound; the actual bound lives in
  `HostLifecycle`. The worker does not add its own timeout (it would race the
  lifecycle's own 10 s deadline); a hung lifecycle is the lifecycle's bug, not
  the UI's.
- The `AttachResult` connection returned by `ensureRunning` is closed (RAII) and
  a fresh `SupervisorConnection` is created by `attach_workspace`. This costs one
  extra `host.hello`; adopting the returned connection is a future optimisation
  proposal (§11.5), not part of S3.

### 5.2 Lazy path vs. explicit `session.resume` (pinned)

Baseline (today): switching to a session **within an already-attached
workspace** is purely local — `UiModel::focusSession`/`SupervisorApp::activate_session`
(`src/ui/ui_model.cpp`, `src/ui/supervisor.cpp`) only set `activeSessionId`; the
agent is resumed lazily on the next prompt by `HostRuntime::ensureAgent`
(`src/host/host_runtime.cpp`). The transcript appears because
`SupervisorApp::refresh_sessions` calls `SupervisorConnection::track` for each
`session.list` entry, which subscribes from `Beginning` and replays
(`src/ui/supervisor_connection.cpp`).

**Decision 22-D4 (pinned).**

- **Ctrl-S Live source:** keeps the existing lazy path. Sessions come from the
  live `session.list` and are already tracked; `focusSession` + lazy
  `ensureAgent` is the established contract (10 §7.5, 22-R4). No `session.resume`.
- **`/sessions` History source:** issues an **explicit `session.resume`**
  (`protocol::method::kSessionResume`) for the selected session, then activates
  it. If the target workspace is already attached, the resume is submitted
  immediately via `submit_to`; if not, `ensure_workspace_running` is called and
  `resume_after_attach` runs when `on_link_state(Attached)` consumes
  `pending_resume_`. One code path, two entry points.

Why explicit for `/sessions`:

1. The selected row came from a **disk snapshot** of `sessions.db`, which may
   include sessions that are not in the daemon's registry junction
   (`03 R9`: the junction is a logical reference; store-only sessions exist).
   `session.resume` validates the id against the daemon's store and yields a
   definitive `AppCode::UnknownSession` for a stale row, instead of silently
   activating a session the daemon cannot load.
2. It **loads** the session into `SessionManager` and **acquires the lease**
   (`HostRuntime::resumeSession` → `AgentRegistry::resume` →
   `SessionManager::resumeSession`, then `acquireLeaseOrThrow`), which is the
   semantic meaning of "resume from history".
3. It is **idempotent** when the agent is already loaded
   (`AgentRegistry::resume` returns the existing `AgentId`), so it is safe in a
   live workspace too.
4. It **shares one wire path** with `ymh run --resume` and S4
   (`run_via_daemon`, `src/cli/cli.cpp`), so `/sessions` and `--resume` cannot
   diverge.

```cpp
// Entry point from handle_switcher (History) and from S4's initial_resume.
void resume_from_history(const WorkspaceId& workspace, const SessionId& session) {
    const auto connection = connections_.find(workspace);
    if (connection != connections_.end() &&
        connection->second->state() == SupervisorLinkState::Attached) {
        resume_after_attach(workspace, session);        // live: resume immediately
    } else {
        ensure_workspace_running(workspace, session);   // non-live: attach, then resume
    }
}

void resume_after_attach(const WorkspaceId& workspace, const SessionId& session) {
    submit_to(workspace, std::string(protocol::method::kSessionResume),
              nlohmann::json{{"session", session.value}},
              [this, workspace, session](SupervisorReply reply) {
                  if (!reply.ok) {
                      enqueue([this, workspace, session, error = reply.error] {
                          // H1: the workspace may still be unmodeled if the
                          // daemon died mid-attach; surface without injecting.
                          surface_notice(workspace, session, "resume failed: " + error);
                      });
                      return;
                  }
                  enqueue([this, workspace, session] {
                      // (MEDIUM-1, Rev 3) the workspace may have been evicted
                      // between the submit and this reply (daemon died; the
                      // reply is pumped, eviction runs on the UI thread, FIFO
                      // across drain batches). Never inject a phantom.
                      if (model_.workspaces.count(workspace) == 0) {
                          surface_notice(workspace, session,
                                         "session resumed in a workspace that is no longer open");
                          return;
                      }
                      model_.ensureSessionIn(workspace, session);  // workspace is modeled
                      model_.ensureCellIn(workspace, session);
                      activate_session(workspace, session);   // focus only; no session.activate
                  });
              });
}
```

`resume_from_history` is the single S3/S4 entry point: `handle_switcher`
(History) calls it on Enter, and the supervisor's startup path calls it (or
seeds `pending_resume_` + `attach_workspace`) for `options_.initial_resume`
(§6.1). **The success branch guards on `model_.workspaces.count(workspace)`**
and routes an evicted workspace through `surface_notice` + return; the failure
branch calls `surface_notice`, which itself never injects a workspace. Neither
**resume** branch can create a `WorkspaceModel` (H1/MEDIUM-1). **Invariant
SW25**, scoped to the resume path; the pre-existing unguarded `create_session`
reply handler is recorded as out-of-scope (§11.7) and is **not** covered by
SW25.

`session.resume` broadcasts `SessionCreated` via
`ProtocolServer::onSessionCreated` (`src/transport/protocol_server.cpp`), which
triggers the existing `refresh_sessions`/cell path in this and peer supervisors
(16-D4). That is a benign, already-handled side effect.

### 5.3 Failure modes (pinned)

- **Spawn failure.** `ensureRunning` throws `HostError`/`std::exception`
  (`HostLifecycle` rethrows `HostUnreachable` after the winner window,
  `src/host/workspace_host.cpp`). The worker captures the message, erases
  `pending_resume_`, and calls `surface_notice(workspace, resume, …)`; for an
  unmodeled workspace that is the status-bar ring (H1), so the notice is
  visible and **no** `WorkspaceModel` is created. (`SW-F1`)
- **Daemon exits during attach.** The connection transitions to `Dead`;
  `on_link_state` sets `live = false` and hides the workspace; a later
  `on_scan` evicts it. If the pending `session.resume` was already submitted,
  `submit_to` replies `"no supervisor connection"` or a transport error →
  `surface_notice`. (`SW-F2`)
- **Unknown/empty session.** `session.resume` replies
  `AppCode::UnknownSession` (`HostRuntime::map_agent_error`,
  `src/host/host_runtime.cpp`). Surface `"session not found in <workspace>"`;
  the workspace stays attached and usable. (`SW-F3`)
- **Workspace row missing.** `registry->findById` returns `nullopt` (row pruned
  between snapshot and selection). Surface `"workspace no longer registered"`;
  do not spawn. (`SW-F4`)
- **`lifecycle == nullptr`** (test/embedding): surface `"cannot start
  workspace"`; do not crash. (`SW-F5`)
- **Daemon restarted with a new `boot_id` (M4, Rev 2).** The scan reports a
  `boot_id` differing from `specs_.at(id).boot_id`; `on_scan` stops/erases the
  stale `Dead` connection and re-attaches. Without this the workspace stays
  hidden and `pending_resume_` is never consumed. (`SW-F13`)
- **Scan tick during an in-flight spawn (M5, Rev 2).** `evict_dead_workspaces`
  skips `ensure_in_flight_`, so `pending_resume_` survives until the attach
  completes. Without this the resume is silently dropped. (`SW-F14`)
- **App destroyed mid-spawn (H2, Rev 2).** `~SupervisorApp` sets the stop token
  and joins the worker before teardown; the worker checks `stop_requested()`
  before `enqueue`. (`SW-F15`)

### 5.4 C++ interface sketch (S3)

```cpp
// include/ymh/ui/supervisor.hpp (additive)
struct SupervisorRunOptions {
    ...
    HostLifecycle*     lifecycle{nullptr};
    WorkspaceRegistry* registry{nullptr};
    std::chrono::milliseconds ensure_running_timeout{12'000};      // S3 (documentation)
    std::chrono::milliseconds catalog_refresh_interval{15'000};    // S2
    // S4: consumed on the first Attached of the workspace (Rev 2: seeded as
    // pending_resume_ at startup, or delivered via resume_from_history).
    std::optional<std::pair<WorkspaceId, SessionId>> initial_resume;
};

// src/ui/supervisor.cpp (SupervisorApp, private)
std::set<WorkspaceId>              ensure_in_flight_;
std::map<WorkspaceId, SessionId>   pending_resume_;
std::jthread                       ensure_worker_;      // (H2)
std::mutex                         ensure_mutex_;
std::condition_variable            ensure_cv_;
std::deque<EnsureRequest>          ensure_requests_;
void ensure_workspace_running(const WorkspaceId& workspace, const SessionId& resume);
void resume_from_history(const WorkspaceId& workspace, const SessionId& session);
void resume_after_attach(const WorkspaceId& workspace, const SessionId& session);
void surface_notice(const WorkspaceId& workspace, const SessionId& session, std::string text);
std::optional<SupervisorWorkspace> workspace_spec_from_registry(const WorkspaceId& id) const;
```

---

## 6. S4 — `ymh --resume <id>` in TUI mode

### 6.1 Intended behaviour (pinned)

`add_common` registers `--resume` into `CliInvocation::session`
(`src/cli/cli.cpp`, `include/ymh/cli/cli.hpp`), but the `Tui` dispatch
(`run_supervisor_entry`, `src/cli/cli.cpp`) never reads it. Pin:

**Decision 22-D5.** `ymh --resume <id>` in TUI mode resumes the session in **the
session's own workspace**, not the cwd workspace. Resolution uses the S2
catalog machinery synchronously in the CLI (there is no TUI yet, so blocking is
fine): enumerate `listWorkspaces()`, read each `<ws>/.ymh/sessions.db` via
`SessionPersistence::openReadOnly`, and find the `SessionHeader` whose `id`
matches. If found in exactly one workspace `W`:

1. `root = W.canonicalPath`; `options.initial_workspace = W.canonicalPath`.
2. `find_or_register_workspace(W)` (`src/cli/cli.cpp`) yields the row (already
   registered; re-registering is idempotent).
3. `lifecycle.ensureRunning(row->id, identity)` — the shipped eager initial
   attach, now for `W` (Rev 3: **pinned**, not conditional). 22-A7 explicitly
   authorizes this eager spawn for the `--resume`-resolved non-cwd workspace;
   it is the same eager initial attach the CLI performs today, only with `W`
   instead of the cwd. S3's lazy `ensure_workspace_running` is not used here.
4. `options.initial_resume = {W.id, SessionId{id}}`.

The supervisor's `run()` seeds `pending_resume_[W.id] = session` before
attaching `W`'s connection, so `on_link_state(Attached)` consumes it through the
same mechanism as S3 (shared code path; **Invariant SW15**). It does **not** call
`resume_from_history` directly during startup, because the connection is not yet
attached at that point.

**`--workspace` interaction and `root` override mechanics (L7, Rev 2).** The
pre-dispatch flow in `run_cli` resolves `root = resolve_workspace(invocation)`
(`--workspace` or cwd) and runs `scaffold_for_invocation` +
`load_invocation_config(root)` **before** the `Tui` case
(`src/cli/cli.cpp`). Pin:

- `--resume` and `--workspace` may both be given. The **session's own workspace
  `W` wins for the supervisor's initial workspace and daemon target**; the
  `--workspace`/cwd `root` still governs config discovery/scaffolding and the
  workspace-layer config read (the daemon re-loads its own workspace layer from
  its cwd, `--config` carries the global path). If `W != root`, print no warning
  (the user asked to resume a specific session).
- `run_supervisor_entry` takes the resolved `root` **and** the resume id; when
  the resume id resolves to `W`, it overrides its internal `root`/`canonical`
  with `W.canonicalPath` for `find_or_register_workspace`, `options.initial_workspace`,
  and the eager `ensureRunning`. The config already loaded for the original
  `root` is unchanged (Rev 2: workspace-layer config is not re-loaded for `W`;
  the daemon does that itself).
- If `--resume` is absent, `--workspace`/cwd behaves exactly as today.

### 6.2 Unknown id (pinned)

If no registered workspace's `sessions.db` contains the id:
`err << "ymh: unknown session: " << id`; return **1**; the TUI is not started.
This matches the CLI's other session commands (`show`/`replay`/`fork` on an
unknown id are errors) and avoids opening a TUI that cannot do what was asked.
(`SW-F6`)

If the id is found in **more than one** workspace (possible only if the same
`SessionId` exists in two DBs — not expected, since ids are UUIDv4), pick the
workspace whose header has the newest `updatedAt`, tie-break `canonical_path`,
and print a warning to stderr. Deterministic, no failure. (`SW-F7`)

### 6.3 Interaction with S3 and `--new` (pinned)

- S4 reuses S3's `resume_after_attach` and the `pending_resume_` map; there is
  exactly one resume code path. S4 does **not** use S3's lazy
  `ensure_workspace_running`: its initial attach is the CLI's eager
  `lifecycle.ensureRunning` inside `run_supervisor_entry` (§6.1 step 3,
  `src/cli/cli.cpp`). **Invariant SW15.**
- `--new` (`CliInvocation::new_session`) is currently parsed and **never used**
  anywhere in `src/cli/cli.cpp`. Pin: `--new` takes precedence over `--resume`
  when both are given (start a fresh session; `--resume` ignored; warn to
  stderr). Otherwise `--new`'s behaviour is unchanged (out of scope; §11.3).
  **Invariant SW16.**

### 6.4 C++ interface sketch (S4)

```cpp
// src/cli/cli.cpp
// Returns the workspace whose sessions.db stores `session`, or nullopt.
std::optional<WorkspaceRecord> resolve_session_workspace(
    WorkspaceRegistry& registry, const std::string& session, std::ostream& err);

int run_supervisor_entry(const std::filesystem::path& root, const Config& config,
                         const std::filesystem::path& config_path, bool verbose,
                         const std::string& resume_session,   // NEW (S4)
                         std::ostream& err);
```

`resolve_session_workspace` is the CLI-side synchronous twin of the catalog
reader: it calls the shared free function
`read_workspace_history(const WorkspaceRecord&, bool live) -> WorkspaceHistory`
(`include/ymh/ui/session_catalog.hpp`, §4.6), passing
`registry.probeLiveness(record.id) == HostLifecycle::Live` for `live`; the
helper performs the same `openReadOnly`/`list()` call with the same
per-workspace catch taxonomy (§4.4). It must not duplicate the *UI* overlay,
only the read helper. On a workspace with a `note` (workspace missing / no db /
not a file / unreadable / corrupt / read-only / schema mismatch / unavailable),
that workspace is skipped for resolution; a workspace with no sessions is
skipped. `root` override mechanics are pinned in §6.1.

---

## 7. Invariants

| ID | Invariant |
|---|---|
| SW1 | In the Live source, every rendered `WorkspaceNode` has `WorkspaceModel::live == true` and `daemonStatus ∈ {Attached, Stopping}`. |
| SW2 | Every entry in `UiModel::workspaces` passes the scanOnce live predicate (`host.has_value() && probeLiveness == Live`) at the last completed `on_scan`, modulo entries whose connection is `Connecting`. |
| SW3 | Eviction never removes an entry whose connection is `Connecting`, nor one whose registry `probeLiveness` is `Live`. |
| SW4 | Eviction is skipped for a tick while `mode == UiMode::ExitConfirm`; `ExitConfirmState::orphaning` is never invalidated by eviction. |
| SW5 | A Live-source `SwitcherOverlayModel` never contains a node with `live == false` or a non-`Attached`/`Stopping` status, including immediately after a live-set change (re-snapshot on change). |
| SW6 | Eviction and the switcher perform no registry writes; the UI thread performs no registry I/O for eviction. |
| SW7 | `/sessions` never blocks the UI thread: all SQLite opens/reads happen on `SessionCatalogReader`'s worker. |
| SW8 (Rev 2) | `/sessions` opens `sessions.db` read-only (`SessionPersistence::openReadOnly`), takes no sidecar `flock`, and never writes table data. It **may** create/attach `-shm`/`-wal` sidecar files (documented SQLite side effect) when the directory is writable, and fails with `StoreError` (degraded, not fatal) when the directory is not writable. It never modifies durable session/event data. |
| SW9 | `/sessions` enumerates `registry.db` `workspaces` rows only; orphan `sessions.db` files are not discovered. |
| SW10 | A per-workspace read failure is contained: it produces a `note`, never an exception, never suppresses other workspaces, never crashes the TUI. |
| SW11 | `/sessions` reuses `SwitcherOverlayModel` + `render_switcher` (`SwitcherSource::History`); no second overlay/tree is added. |
| SW12 | `HostLifecycle::ensureRunning` is never called on the UI thread. |
| SW13 | `/sessions` selection issues explicit `session.resume`; Ctrl-S selection keeps the lazy `ensureAgent` path. `session.activate` is never called by either. |
| SW14 (Rev 2) | `pending_resume_` holds at most one target per workspace; the last selection wins; it is consumed exactly once per successful attach and erased on failure/eviction — but **not** while a spawn for that workspace is in flight (`ensure_in_flight_`, SW21). |
| SW15 | `ymh --resume` resolves the session's own workspace and reuses the S3 resume path; unknown id exits 1 without starting the TUI. |
| SW16 | `--new` beats `--resume`; a warning is emitted. |
| SW17 (Rev 3) | Both switcher sources order workspace entries by `title` ascending (case-insensitive), tie-break `canonical_path` ascending. Sessions are source-specific: **Live** keeps the daemon's `session.list` order (never re-sorted by the supervisor); **History** is `updated_at` descending, `id` ascending. Registry `ORDER BY canonical_path` is the grouping key/tie-break only. |
| SW18 | The spec-16 last-exit prompt and the owner watchdog are structurally independent of `OwnershipMark` and the switcher (see §3.4). |
| SW19 | A catalog snapshot carries `capturedAtMs` and `complete`; stale/partial snapshots are displayed with a footer marker, never blanked, and never block. |
| SW20 (Rev 2) | A `WorkspaceModel` with `daemonStatus ∈ {Connecting, Detached, Dead}` is never rendered in the Live source, even if `live == true` (the SW1/`Connecting` gate). |
| SW21 (Rev 2) | Eviction skips every workspace in `ensure_in_flight_` and never erases `pending_resume_` for a spawn in flight (M5). |
| SW22 (Rev 2) | `on_scan` replaces a connection whose pinned `boot_id` differs from the scan's `boot_id` (stop + erase + re-attach), so a restarted daemon is re-adopted and `pending_resume_` can be consumed (M4). |
| SW23 (Rev 2) | A failure notice for an unmodeled workspace/session is delivered via `UiModel::notices`; it never calls `ensureSessionIn`/`ensureCellIn`, so `model_.workspaces` is unchanged (H1). |
| SW24 (Rev 2) | The spawn worker is a single owned `std::jthread`; `~SupervisorApp` calls `request_stop()` + `join()` before member teardown, and the worker checks `stop_requested()` before any `enqueue`/`surface_notice` (H2). |
| SW25 (Rev 4) | **On the resume path**, the `resume_after_attach` **success** branch guards on `model_.workspaces.count(workspace) != 0` and, when the workspace is not modeled, routes through `surface_notice` and returns; the **failure** branch calls `surface_notice` directly, which never injects. So no **resume** reply path can create a `WorkspaceModel` (MEDIUM-1). Scope note: this invariant covers `resume_after_attach` only; `create_session`'s reply handler is a pre-existing unguarded path recorded as out-of-scope (§11.7). |
| SW26 (Rev 3) | `SupervisorApp` owns the `SessionCatalogReader`; `~SupervisorApp` stops+joins it before any other member teardown, and `catalog_`/`ensure_worker_` are declared after `action_mutex_`/`actions_`/`screen_` so reverse destruction also orders them first (MEDIUM-2). |

**Why `SW`.** `S` is claimed by spec 01; `SW` is unused by every spec (see the
naming note). The user's decision labels S1–S4 are kept as prose labels only.
Rev 2 added **`SW20`–`SW24`**; Rev 3 adds **`SW25`–`SW26`**.

---

## 8. Failure modes

Continue the repo's `F1–F12` convention with a spec-local `SW-F` prefix
(disjoint from `§54 F1–F12`; per the 16/20/21 naming convention). Rev 2 added
`SW-F13`–`SW-F16` (M4, M5, H2, M2); Rev 3 adds `SW-F17`–`SW-F19`
(MEDIUM-1, MEDIUM-2, MEDIUM-3).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| SW-F1 | `ensureRunning` throws (spawn/attach failed, `HostUnreachable`) | `"cannot start workspace: <err>"` notice; for an unmodeled workspace it is the status-bar ring, and `model_.workspaces` is unchanged (H1) | `pending_resume_` erased; user may retry; no phantom workspace |
| SW-F2 | Daemon dies during attach | workspace hidden (`live=false`), then evicted; pending resume replies "no supervisor connection"/transport error | `surface_notice`; next scan evicts; user may select another |
| SW-F3 | `session.resume` → `AppCode::UnknownSession` | `"session not found in <workspace>"` | workspace stays attached; snapshot refresh will drop the stale row |
| SW-F4 | Registry row pruned between snapshot and selection | `"workspace no longer registered"` | no spawn; catalog refresh removes the group |
| SW-F5 | `options_.lifecycle == nullptr` | `"cannot start workspace"` | no spawn; test/embedding only |
| SW-F6 | `ymh --resume <unknown>` | stderr `"ymh: unknown session: <id>"`, exit 1 | TUI not started |
| SW-F7 | Same session id in two DBs | stderr warning; newest `updatedAt` wins | deterministic; no failure |
| SW-F8 | `sessions.db` missing/unreadable | `(no stored sessions)`/`(unreadable)` leaf | `partial` footer; other groups unaffected |
| SW-F9 | `sessions.db` corrupt (FK/sequence) | `(corrupt)` leaf | `partial` footer; other groups unaffected |
| SW-F10 | `sessions.db` schema mismatch (app id / `user_version`) | `(schema mismatch)` leaf | `partial` footer; other groups unaffected |
| SW-F11 (Rev 4) | Workspace dir gone | **pre-flight 0** → `(workspace missing)` leaf (not `(no stored sessions)`; the `StoreOpenError` "workspace root…" branch is TOCTOU-only) | `partial` footer; other groups unaffected |
| SW-F12 | Exit prompt open while a daemon dies | eviction deferred one+ ticks | prompt resolves; next `on_scan` evicts |
| SW-F13 (Rev 2) | Daemon restarted with a new `boot_id` (M4) | stale `Dead` connection is never replaced; workspace hidden; `pending_resume_` never consumed | `on_scan` stops/erases the stale connection on `boot_id` mismatch and re-attaches |
| SW-F14 (Rev 2) | Scan tick during an in-flight spawn (M5) | without the guard, `pending_resume_` would be erased and the resume dropped | `evict_dead_workspaces` skips `ensure_in_flight_` |
| SW-F15 (Rev 2) | `SupervisorApp` destroyed mid-spawn (H2) | without lifetime management, the worker would `enqueue` on a destroyed object | `~SupervisorApp` sets the stop token and joins; the worker checks `stop_requested()` first |
| SW-F16 (Rev 2) | `sessions.db` in a non-writable directory (M2) | `PRAGMA journal_mode=WAL`/first query → `SQLITE_READONLY` → `StoreError` | `(read-only location)` leaf; `partial` footer; other groups unaffected |
| SW-F17 (Rev 3) | Workspace evicted between `session.resume` submit and reply (MEDIUM-1) | without the guard, the success handler would recreate a phantom `Attached`/`live=false` workspace | success branch guards on `model_.workspaces.count(workspace)`; else `surface_notice` + return |
| SW-F18 (Rev 3) | `SupervisorApp` destroyed mid-catalog-build (MEDIUM-2) | without a lifetime-managed reader, its sink would `enqueue` after `action_mutex_` is destroyed | `catalog_->stop()` (join) in `~SupervisorApp` before any member teardown; declaration order pinned |
| SW-F19 (Rev 3) | Non-DB/garbage `sessions.db` (MEDIUM-3) | `PRAGMA synchronous` → `SQLITE_NOTADB` → generic `StoreError`; labelled `(corrupt)` by pre-flight magic check, not `(read-only location)` | pre-flight 4 → `(corrupt)`; residual `SQLITE_NOTADB` past the check → `(unavailable: <msg>)` |

---

## 9. dsh mapping

The DeepSeek Harness (dsh) separates **session state** (durable, event-sourced)
from **client presentation** (ephemeral). S1–S4 stay inside that separation:

| dsh concept | ymh mapping here |
|---|---|
| Durable session store | `<workspace>/.ymh/sessions.db` (`SessionPersistence`); `/sessions` reads it directly read-only, never mutating table data (it may attach `-shm`/`-wal` sidecars, §4.1). |
| Open set / membership | shared `registry.db` `workspaces` (`WorkspaceRegistry`); `/sessions` groups by these rows; the Live switcher is the live subset. |
| Client-local focus | `UiModel::activeWorkspaceId`/`activeSessionId` (`03 R12`); never written to the registry. |
| Ownership / liveness | lock-primary (`03 R5`); the scanOnce predicate is the single liveness authority; the switcher is a projection, never a source of truth. |
| Control plane | no new RPC: `/sessions` is a read of the durable store; resume reuses `session.resume`. |
| Event stream is truth | the TUI remains one consumer; `/sessions` lists headers, it does not replay. |

No dsh-level invariant is weakened: the event log stays the durable source of
truth, the registry stays the open-set authority, and the UI stays a consumer.

---

## 10. Test plan

Strategy is `00 §44`: unit, integration (Fake LLM / fake FS / fake shell),
persistence/replay, golden render, and a separate opt-in live PTY layer. The
deterministic layers run offline; the live layer is opt-in (`YMH_LIVE_LLM=1`).
No `sleep`-based synchronisation beyond the injected clock/interval seams
(16 §8.4). ID scheme: `SW-U*` unit, `SW-I*` integration, `SW-G*` golden render,
`SW-P*` PTY/live. Existing affected tests are listed for amendment; spec 22 does
not edit tests (implementation phase does).

### 10.1 Unit

| ID | Test | Asserts |
|---|---|---|
| SW-U1 (Rev 2) | `ui_model_test.cpp` — live-only `SwitcherOverlayModel::open` | a `Detached`/`Dead`/`NotRunning` workspace is absent; `Attached`/`Stopping` present; a **`Connecting` workspace with `live == true` is absent** (SW20 gate) and renders no `[unreachable]`. **Amends** `UiModel.SwitcherNodesCarryOwnershipMark` (currently expects a `Dead` node) and `UiRenderGolden.SwitcherShowsOwnershipMarks` (currently expects `[not running]`/`[unreachable]`). `UiModel.SwitcherNavigatesAcrossWorkspaces` uses two `Attached` workspaces and passes unchanged. |
| SW-U2 | `ui_model_test.cpp` — `UiModel::eraseWorkspace` | removes the workspace + its sessions; repairs `activeWorkspaceId`; no-op for an unknown id; aggregate marked dirty. |
| SW-U3 | supervisor unit (fake registry + scanner) — `on_scan` eviction | a workspace absent from the live set, not `Connecting`, and not in `ensure_in_flight_` is erased from `model_.workspaces`, `connections_`, `specs_`, `pending_*`; a `Connecting` entry is retained; an in-flight spawn is retained (M5/SW21). |
| SW-U4 (Rev 4) | `session_catalog_test.cpp` (new) — `read_workspace_history` | maps `SessionHeader` → `SessionHistoryEntry`; History sessions sorted `updatedAt` desc, `id` asc; **the §4.4 pre-flight + catch taxonomy yields exactly one pinned `note` per real input** — **workspace dir gone → `"workspace missing"` (pre-flight 0)**, missing db → `"no sessions.db"`, not-a-file → `"not a file"`, `chmod 0000` → `"unreadable"`, **non-DB/garbage → `"corrupt"` (bad magic)**, `chmod 0555` dir → `"read-only location"`, valid DB with broken FK/sequence → `"corrupt"`, foreign app id → `"schema mismatch"`, app-id `0` accepted (L8) — and never throws. The two permission-dependent cases (`chmod 0000`, `chmod 0555`) are `GTEST_SKIP`ped when `::geteuid() == 0` (LOW-5). |
| SW-U5 | `session_catalog_test.cpp` — `SessionCatalogReader` | delivers a snapshot via the sink; `refreshNow()` coalesces; `generation` increments; a throwing `read_workspace_history` does not kill the thread. |
| SW-U6 (Rev 3) | `ui_model_test.cpp` — `SwitcherOverlayModel::openHistory` | builds nodes from `model.catalog`; non-live workspace has `historyOnly == true`, `note` propagated; **History sessions `updated_at` desc / `id` asc**; **workspace groups ordered title-ascending (case-insensitive), canonical_path tie-break** (M1/MEDIUM-4). |
| SW-U7 | `command_registry` / supervisor — `/sessions` dispatch | `/sessions` sets `switcher.source == History` and `mode == Switcher`; with `catalog.loaded == false` the render shows the loading placeholder; `CommandContext::sessions` absent → no crash. |
| SW-U8 | `ui_model_test.cpp` — `ownership_mark` unchanged | all four mappings still hold (`Owned`/`NotRunning`/`Stopping`/`Unreachable`), so 22-R1 is provably untouched. |
| SW-U9 | supervisor unit — `on_link_state(Dead)` | sets `live = false` and the workspace is hidden from a Live `open()` before any eviction. |
| SW-U10 | CLI — `resolve_session_workspace` | finds the session's own workspace; `nullopt` for unknown; newest-`updatedAt` tie-break for a duplicate. |
| SW-U11 (Rev 2, H1) | `ui_model_test.cpp` — notice ring | `pushNotice` appends, bounds at `kMaxNotices` (drops oldest), marks `Status` dirty; `surface_notice` for an **unmodeled** workspace/session calls `pushNotice` and leaves `model_.workspaces` **unchanged** (asserts no `WorkspaceModel` with `daemonStatus == Attached` is created). |
| SW-U12 (Rev 2, H2) | supervisor unit — worker lifetime | destroying a `SupervisorApp` with a spawn in flight sets the stop token, joins the worker, and does not crash; the worker's completion is not enqueued after the stop request (inject a slow fake `HostLauncher`). |
| SW-U13 (Rev 2, M4) | supervisor unit — boot_id restart | `on_scan` with a spec whose `boot_id` differs from `specs_.at(id).boot_id` stops/erases the stale connection and re-attaches; a subsequent `Attached` consumes `pending_resume_`. |
| SW-U14 (Rev 2, M5) | supervisor unit — eviction vs in-flight | with `ensure_in_flight_.count(ws) != 0`, `evict_dead_workspaces` leaves the workspace and `pending_resume_[ws]` intact. |
| SW-U15 (Rev 3, M1/MEDIUM-4) | `ui_model_test.cpp` — ordering | two workspaces with the same title order by `canonical_path`/`cwd`; case-insensitive title compare (`"beta"` before `"Gamma"`); both sources use the same **workspace** rule; **Live source preserves the input `session.list` order** (assert it is not re-sorted), History applies `updated_at` desc. |
| SW-U16 (Rev 2, L1) | `ui_model_test.cpp` — source reset | after `openHistory`, `UiModel::openSwitcher()` sets `switcher.source == Live`; `SwitcherOverlayModel::close()` leaves `source` unchanged. |
| SW-U17 (Rev 2, L3) | `ui_model_test.cpp` — cursor validation | a cursor on a workspace filtered out by the display predicate is reset to the active/first node, and a stale `cursor.session` is cleared, on the next `open`. |
| SW-U18 (Rev 3, MEDIUM-1) | supervisor unit — resume reply after eviction | with the workspace evicted between the `session.resume` submit and its reply, the success handler calls `surface_notice` and returns; `model_.workspaces` is unchanged (no phantom `Attached`/`live=false` workspace) and no `ensureSessionIn`/`ensureCellIn` call occurs. |
| SW-U19 (Rev 3, MEDIUM-2) | supervisor unit — catalog reader lifetime | destroying a `SupervisorApp` with a catalog build in flight stops+joins the reader before member teardown and does not crash (ASan); a slow fake read blocks the build. |
| SW-U20 (Rev 4, MEDIUM-3) | `session_catalog_test.cpp` — pre-flight classification | **a deleted workspace directory is `"workspace missing"` (pre-flight 0), not `"no sessions.db"`**; a garbage/non-DB file is `"corrupt"` (bad magic); a missing file is `"no sessions.db"`; a `chmod 0000` file is `"unreadable"`; a `chmod 0555` dir is `"read-only location"`; a valid-magic DB with a broken FK/sequence is `"corrupt"`. Each real input renders exactly one pinned leaf and never throws. Permission-dependent cases (`chmod 0000`/`0555`) are `GTEST_SKIP`ped when `::geteuid() == 0` (LOW-5). |

### 10.2 Integration

| ID | Test | Asserts |
|---|---|---|
| SW-I1 | spawn-then-resume | a registered, **not-running** workspace with an on-disk session; select it in `/sessions`; the daemon is spawned (`probeLiveness == Live`), `session.resume` is issued, the session activates, and its transcript replays (subscribe from `Beginning`). |
| SW-I2 | dead-daemon eviction | kill a daemon; within one `scan_interval` the workspace is gone from `model_.workspaces` and the Live switcher; other workspaces unaffected. |
| SW-I3 | `/sessions` lists a stopped workspace from disk | with **no** daemon, `/sessions` shows the workspace's stored sessions (History order: `updated_at` desc) read from `sessions.db`. |
| SW-I4 (Rev 4) | degradation without crash | for each of **workspace dir gone → `(workspace missing)` (pre-flight 0)** / missing db → `(no stored sessions)` / **non-DB garbage file → `(corrupt)`** / corrupt db → `(corrupt)` / schema-mismatched db → `(schema mismatch)` / **non-writable dir → `(read-only location)` (non-root)**: `/sessions` renders exactly the one pinned marker, the TUI stays up, and the other groups still list. Permission-dependent cases are `GTEST_SKIP`ped when `::geteuid() == 0`. |
| SW-I5 | `ymh --resume <id>` in TUI mode | resolves the session's own workspace (not cwd) and resumes it; the TUI's active session id equals the requested id. |
| SW-I6 | `/sessions` reuse | selecting in History mode uses the same `SwitcherOverlayModel`/`render_switcher`; no second overlay is instantiated (assert on `model_.switcher.source` and the render output). |
| SW-I7 (Rev 4, M2) | file-access behaviour of `/sessions` | a read of a writable WAL `sessions.db` succeeds and may create `-shm`/`-wal`; a read of a `chmod 0555` directory yields `note == "read-only location"` (non-root; `GTEST_SKIP` when `::geteuid() == 0`) and does not crash; the session/event rows are byte-identical before and after the read (no data write). |
| SW-I8 (Rev 2, M4) | daemon restart with a new `boot_id` | kill a daemon and let the supervisor respawn/restart it (new `boot_id`); within one `scan_interval` the workspace is re-attached, reappears in Ctrl-S, and a `/sessions` selection consumes `pending_resume_` (resume happens). |
| SW-I9 (Rev 2, H2) | destroy app mid-spawn | with a fake `HostLauncher` that blocks `spawn`, destroy `SupervisorApp`; assert no crash/ASan error and that the worker is joined. |
| SW-I10 (Rev 3, MEDIUM-2) | destroy app mid-catalog-build | with a reader whose per-workspace read blocks, destroy `SupervisorApp`; assert no crash/ASan error and that the reader is joined before `action_mutex_`/`screen_` teardown. |

### 10.3 Golden render

| ID | Test | Asserts |
|---|---|---|
| SW-G1 (Rev 3) | `ui_render_golden_test.cpp` — Live switcher live-only | `[owned]`/`[stopping]` present; `[not running]` and `[unreachable]` **absent**; with a `Connecting` workspace in the model the output is deterministic and still contains no `[unreachable]` (SW20; amends `UiRenderGolden.SwitcherShowsOwnershipMarks`). Live **window title `"workspaces"`** (shipped, `src/ui/ui_render.cpp:462`) and content header `"Switcher"` (L2, Rev 3). |
| SW-G2 (Rev 3) | `ui_render_golden_test.cpp` — History overlay | workspace groups ordered **title-ascending (case-insensitive), canonical_path tie-break** (M1; **changes** from the Rev 1 "registry order" expectation); History sessions `updated_at` desc; non-live group shows `[history]`; session leaves show title/kind/model/relative-updated/fork origin; a partial catalog shows the `partial` footer; window title `"sessions"` with content header `"sessions"`. |
| SW-G3 | `ui_render_golden_test.cpp` — empty/loading History | loading placeholder before the first snapshot; `(no stored sessions)` for an empty workspace. |

### 10.4 PTY / live

| ID | Test | Asserts |
|---|---|---|
| SW-P1 | `ui_supervisor_pty_test.cpp` — `/sessions` → spawn → resume | drive the real binary under a PTY; open `/sessions`, select a stored session in a stopped workspace, assert the daemon spawns and the session transcript renders. Hermetic `XDG_STATE_HOME`/`HOME` (the harness already sets them, `host_harness.hpp`). |
| SW-P2 | `ui_supervisor_pty_test.cpp` — `ymh --resume <id>` | the resumed session's title is on the header line (RB-10 slot) after startup. |
| SW-P3 (Rev 2, L5) | `ui_live_pty_test.cpp` (opt-in `YMH_LIVE_LLM=1`) | against real DeepSeek: resume a stored session and send a follow-up; assert a **unique, server-generated, unpredictable marker** (a fresh nonce the prompt asks the model to echo) appears in the reply **within a single frame**, not merely as an echo of the prompt. The test **must fail with a bogus `DEEPSEEK_API_KEY`** (mirrors the corrected `UiLivePty.StreamsAssistantReply` discipline). Do not match the prompt text. |
| SW-P4 | `ui_supervisor_pty_test.cpp` — Ctrl-S live-only | with a stopped workspace registered, Ctrl-S does **not** show it; `/sessions` does. |

**Amended existing tests (implementation phase).** `UiRenderGolden.SwitcherShowsOwnershipMarks`
(`tests/unit/ui_render_golden_test.cpp`) and `UiModel.SwitcherNodesCarryOwnershipMark`
(`tests/unit/ui_model_test.cpp`) currently assert `NotRunning`/`Unreachable`
switcher nodes; SW-U1/SW-G1 replace those assertions. `UiModel.OwnershipMarkDerivesFromDaemonStatus`
is **kept** unchanged (SW-U8). SW-G2 changes the Rev 1 History grouping assertion
from registry order to title order (M1). SW-P3 follows the corrected
`UiLivePty.StreamsAssistantReply` nonce discipline (L5).

---

## 11. Out-of-scope and recorded risks

### 11.1 Orphan / unregistered `sessions.db` files

Explicitly out of scope: `/sessions` does not scan the filesystem for
`<dir>/.ymh/sessions.db` files whose workspace row is absent from `registry.db`.
Consequence: a pruned registry row (`removeWorkspace`, `src/registry/registry.cpp`)
loses its history from this surface even if the DB survives. Re-importing is a
future action, not S2.

### 11.2 Test-isolation defect: `XDG_STATE_HOME` (recorded)

Some live-test paths are not hermetic with respect to `XDG_STATE_HOME` and can
leak real workspace rows into the developer's `~/.local/state/ymh/registry.db`.
Evidence: `tests/support/host_harness.hpp` pins `HOME`/`XDG_STATE_HOME`/
`XDG_CONFIG_HOME`/`XDG_CACHE_HOME` for harness children; `ui_live_pty_test.cpp`
and `integration_live_e2e_test.cpp` set them explicitly; but other live paths
(e.g. `headless_live_test.cpp`, which calls `run_headless` in-process and does
not spawn a daemon, so it is likely safe today) have no such pin, and the
in-process PTY/supervisor tests rely on `::setenv` at test start rather than a
fixture-level guarantee. This is **out of scope for spec 22** but is recorded
because a leaked row is exactly the kind of stale workspace that S1's switcher
used to retain forever, and because `/sessions` will enumerate leaked rows. A
separate test-infra fix should pin `XDG_STATE_HOME` in a process-wide fixture.
Pointer: `tests/support/host_harness.hpp`, `tests/unit/ui_supervisor_pty_test.cpp`,
`tests/integration_live_e2e_test.cpp`.

**Related live-test vacuity (L5, Rev 2, recorded).** The gate found
`UiLivePty.StreamsAssistantReply` **vacuous**: it matched its own echoed prompt
and passed with a bogus API key; it has since been fixed to use an unpredictable
nonce and to count occurrences within a single frame. Spec 22 does **not** edit
tests (including that file); SW-P3 mirrors the corrected discipline so no new
live test reintroduces a forgeable echo match. A live test must **fail** with a
bogus `DEEPSEEK_API_KEY`.

### 11.3 `--new` is parsed but unused (recorded)

`CliInvocation::new_session` is set by `add_common` (`src/cli/cli.cpp`) and read
nowhere. S4 only pins its precedence over `--resume` (SW16). Implementing `--new`
in TUI mode is out of scope.

### 11.4 16-D2 spawn scope/triggers (amended by 22-A7) and the shipped eager cwd spawn

`16-daemon-ownership.md` §3.2.1 (16-D2) pins **lazy** spawn for *the cwd
workspace* on *a prompt / `session.create` / workspace activation*, and
supersedes the eager `ensureRunning` at `cli.cpp`. **22-A7 (Rev 2; extended in
Rev 3)** amends 16-D2's scope and trigger list: S3 adds `session.resume` as a
spawn trigger and extends the spawn scope beyond the cwd workspace to **any
registered workspace selected from `/sessions`**; S4's resolved session
workspace is covered by the **same** amendment (it is a `session.resume`
trigger). `/sessions` selection **is** a workspace activation in 16-D2's sense,
so the lazy policy is retained; what changes is (a) the workspace need not be
the cwd, and (b) `session.resume` joins the trigger list.

**Resolved (Rev 3, no longer "recorded but unresolved").** The shipped
`run_supervisor_entry` (`src/cli/cli.cpp`) calls
`lifecycle.ensureRunning(row->id, identity)` eagerly for its initial workspace
(cwd, or the `--resume`-resolved workspace). S4 **retains that eager spawn by
decision** — the initial workspace is attached before the TUI starts, matching
the shipped CLI — and 22-A7 now explicitly authorizes the eager spawn for the
`--resume`-resolved non-cwd workspace as well. S3's on-demand
`ensure_workspace_running` covers every other workspace. There is therefore no
remaining 16-D2-vs-CLI discrepancy for the initial workspace within this spec's
scope: the eager initial attach and the lazy on-demand attach coexist under
22-A7. (A future spec that moves the initial attach to the lazy path would
simply delete the eager call; that is an optimization, not a correctness gap.)

### 11.5 Proposals (explicitly flagged, not pinned)

- A `session.listAll` RPC (or a daemon-side catalog) — **rejected** for S2; disk
  reads need no daemon and are strictly more available. Raise separately if
  cross-daemon consistency becomes a requirement.
- A lighter read-only session open that skips `verify_consistency` — a possible
  optimisation if the 15 s catalog cadence proves heavy (§4.2).
- Adopting the `AttachResult` connection in `attach_workspace` to avoid the
  extra `host.hello` (§5.1).
- Additional `/sessions` columns (e.g. `serverProfile`) beyond the pinned §4.1
  list.

### 11.6 Residual uncertainty (Rev 5)

- **Bounded quit wait.** The `std::jthread` join in `~SupervisorApp` can block
  quit for up to the lifecycle's own bound (~15 s worst case) because
  `HostLifecycle::ensureRunning` has no cancellation hook. This is documented
  and bounded, not a UAF; a future cancellation hook (§11.5) would remove it.
  The catalog reader's join is bounded by one per-workspace read (the 15 s
  cadence is not a factor: `stop()` interrupts the wait, not an in-flight read).
- **`immutable=1` gating.** The optional fast path is safe only when the
  workspace is **non-live** AND no non-empty `-wal` exists (§4.1); if an
  implementation uses it unconditionally it would silently miss uncheckpointed
  committed frames. The baseline (plain `openReadOnly`) is the safe choice and
  is what SW-I7 asserts; dropping the fast path is always acceptable.
- **Live-workspace disk reads.** S2 reads disk for live workspaces too, to keep
  the full `SessionHeader` field set. This is WAL-snapshot-safe but does touch
  the file; if a future requirement forbids any file access for a live
  workspace, the `session.list` field-parity gap must be resolved first.
- **Case-insensitive compare scope.** 22-D1 pins an ASCII `tolower` compare; a
  non-ASCII title falls back to a bytewise compare. This is deterministic but
  not locale-aware.
- **`specs_`/`connections_` sync.** The M4 re-attach depends on both maps being
  maintained together (by `attach_workspace`/eviction). A future refactor that
  splits them must preserve this.
- **MEDIUM-3 indistinguishability.** A `chmod 0000` file and a missing file both
  surface `SQLITE_CANTOPEN`; the pre-flight `exists`/`access` checks separate
  them. A `SQLITE_NOTADB` that slips past the magic check is `(unavailable)`,
  not `(corrupt)`. The `(corrupt)` row for a valid-magic DB is reachable only via
  `verify_consistency` (`CorruptionError`). This is pinned and accepted.
- **Pre-flight race.** A file can change between the pre-flight and the open
  (TOCTOU); the residual **`"unavailable"` arm** (which carries the raw SQLite
  message) covers that case and never crashes. The `"read-only location"` arm
  is a fixed note and does not carry the message.

### 11.7 Pre-existing unguarded `create_session` reply path (recorded, out of scope; LOW-6)

`SupervisorApp::create_session`'s reply handler calls
`model_.ensureSessionIn(workspace, SessionId{session})` and
`model_.ensureCellIn(workspace, SessionId{session})` with **no** workspace
guard (`src/ui/supervisor.cpp:854-859`). If the workspace were evicted between
the `session.create` submit and its reply, that path could inject a phantom
`WorkspaceModel` — the same defect class as MEDIUM-1, but **pre-existing**
(present before spec 22) and on the `create_session` path, not the
`resume_after_attach` path. Spec 22 does **not** fix it (it is outside S1–S4)
and **SW25 is scoped to the resume path only**; this entry records the gap so
SW25's coverage is not overstated. A follow-up should apply the same
`model_.workspaces.count(workspace)` guard to the `create_session` handler.

---

## 12. Revision log

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-09-17 | Initial draft: S1 live-only switcher + eviction; S2 `/sessions` disk catalog; S3 spawn-then-resume; S4 `--resume` in TUI. Supersession map §1.3, register §2, invariants SW1–SW19, failure modes SW-F1–SW-F12, tests SW-U/I/G/P. |
| 2 | 2026-09-17 | Gate fix pass. **H1**: workspace-independent `UiModel::notices` + `pushNotice`, `surface_notice` never injects a workspace (§3.6, §5.1, SW23, SW-F1, SW-U11). **H2**: single owned `std::jthread` spawn worker joined in `~SupervisorApp` (§5.1, SW24, SW-F15, SW-U12, SW-I9). **M1**: one ordering rule — title case-insensitive, `canonical_path` tie-break (§3.7, §4.3, SW17, SW-U6/SW-U15, SW-G2, 22-A5). **M2**: corrected SW8/§4.1 sidecar honesty, disk read for all workspaces, `StoreError` + read-only-location degradation, empirical SQLite evidence (§4.1, §4.4, SW-F16, SW-U4, SW-I4/SW-I7, 22-D6). **M3**: 16-D2 scope/trigger amendment (§11.4, 22-A7, 22-R7). **M4**: boot_id restart re-attach (§3.2 rule 9, SW22, SW-F13, SW-U13, SW-I8). **M5**: eviction skips `ensure_in_flight_` (§3.2 rule 8, SW21, SW-F14, SW-U14). **SW1/Connecting**: display gate `live && Attached/Stopping`, `live` set only on `Attached` (§3.1, SW1/SW20, SW-U1, SW-G1). **L1**–**L8** folded into the sections above. Register: Rev 2 rows + 22-A7–A9; failure modes SW-F13–SW-F16; invariants SW20–SW24; tests SW-U11–SW-U17, SW-I7–SW-I9. |
| 3 | 2026-09-17 | Re-gate fix pass. **MEDIUM-1**: `resume_after_attach` success branch guards on `model_.workspaces.count(workspace)`; evicted workspace → `surface_notice` + return (§5.2, SW25, SW-F17, SW-U18). **MEDIUM-2**: `SupervisorApp` owns `catalog_`; `~SupervisorApp` stops+joins it before other teardown; declaration order pinned (§4.2, §5.1, SW26, SW-F18, SW-U19, SW-I10). **MEDIUM-3**: §4.4 pre-flight classification + aligned catch order (non-DB → `(corrupt)`, residual → `(unavailable: <msg>)`, `.ymh`-writability → `(read-only location)`); no `SessionPersistence` API change (§4.4, SW-F19, SW-U4, SW-U20, SW-I4). **MEDIUM-4**: source-specific session ordering — Live keeps `session.list` order, History `updated_at` desc (§3.7, §4.3, SW17, 22-D1, 22-A13, SW-U6/SW-U15/SW-G2). **LOWs**: naming note → SW1–SW26/SW-F1–SW-F19; `immutable=1` gated on non-live + no non-empty `-wal`; History overlay rebuilt on each snapshot; 22-R3/§4.5 registry handle corrected; S4 eager initial attach folded into 22-A7 (no longer unresolved); L2 window-title evidence corrected (`"workspaces"` window title, `"Switcher"` content header). Register: Rev 3 rows + 22-A10–A13; failure modes SW-F17–SW-F19; invariants SW25–SW26; tests SW-U18–SW-U20, SW-I10. |
| 4 | 2026-09-17 | Third-gate fix pass. **MEDIUM**: §4.4 gains **pre-flight step 0** (`is_directory(record.canonicalPath) == false → "workspace missing"`); the "workspace dir gone" row and SW-F11 now reference step 0, the catch-order `StoreOpenError` "workspace root…" branch is marked **TOCTOU-only**, and SW-U4/SW-U20/SW-I4 list the workspace-gone case — **every real input maps to exactly one pinned note** (§4.4, §6.4). **LOW-1**: SW25 reworded (success branch guards; failure branch uses `surface_notice`). **LOW-2**: naming note decisions → `22-D1`–`22-D6`. **LOW-3**: `read_workspace_history(const WorkspaceRecord&, bool live)`; liveness supplied by the caller (§4.2, §4.6, §6.4). **LOW-4**: one `eraseWorkspace` promotion rule (first renderable, else first by map key, else empty); §3.2 rule 7 and §3.5 step 4 agree. **LOW-5**: pre-flight pinned under a **non-root** assumption; permission-dependent tests `GTEST_SKIP` under root. **LOW-6**: SW25 scoped to the resume path; pre-existing unguarded `create_session` reply handler recorded out-of-scope (§11.7). Register: Rev 4 rows; no new invariants/failure modes. |
| 5 | 2026-09-17 | Post-PASS documentation/test-setup accuracy pass (no pinned decision, invariant, failure mode, interface, or S1–S4 contract changed). **LOW-A**: §6.3 no longer says S4 reuses `ensure_workspace_running`; it reuses `resume_after_attach`/`pending_resume_` (S4's initial attach is the CLI's eager `ensureRunning`, §6.1). **LOW-B**: 22-S4 and the header `Supersedes:` now also cover `16 §7.7 :2138-2139` ("the cwd workspace shows `NotRunning` if no daemon"). **LOW-C**: §4.4's non-root sentence names SW-I7. **LOW-D**: the `"read-only location"` row/prose state the sidecar-absent condition (an existing `-shm`/`-wal` makes the read succeed). **LOW-E**: no distinct item labelled E exists in the file (grouped into F/G/H by the gate). **LOW-F**: the raw-message claim is qualified to the `"unavailable"` arm only (the `"read-only location"` arm drops it). **LOW-G**: §11.6 retitled "(Rev 5)"; the Rev 4 log citation `§6.1` → `§6.4`. **LOW-H**: the "Unreadable" row is scoped to a `chmod 0000` **file** in a traversable directory (a `chmod 0000` directory makes `exists(db_path)` false → `"no sessions.db"`); the Rev 4 register row's anchor is corrected to "pre-flight step 0". Register: Rev 5 rows. |
