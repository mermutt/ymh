# 49 — Lazy Workspace Creation on First Prompt and the Session-Aware Ctrl-S Notice

```
Status: **draft (Rev 4)** — awaiting independent review.
Component: 49 (errata) — reinstates 16-D2's lazy-spawn policy; supersedes 22
            §11.4/22-A7's eager-initial-spawn half; amends 16-D2's bare-cwd
            `NotRunning` listing clause and 46-D3; does not supersede any 22 §3
            invariant
Depends on: 00-architecture.md §54 (F1–F12), §44 (test strategy), §45 (Fake LLM);
            16-daemon-ownership.md (verified) §2.3/§2.6/§3.2.1/§7.7/§8.1 (O1–O22);
            22-switcher-sessions-errata.md (verified) §3.1/§3.4/§3.7 (SW1/SW5/SW17),
            §5.2 (S3 resume), §7, §11.4; 46-permissions-ui-errata.md (verified) §5
            (46-D3/46-I8); 45-ui-interaction-errata.md (verified) §6 (45-D3/45-D4);
            10-supervisor-tui.md (verified) §7.1/§7.2; 03-workspace-registry.md
            (verified) §9.10
Supersedes: 22 §11.4 (`:1785-1796`) and the 22-A7 row (`:147`, `:1776-1783`) **in the
            bare-cwd eager-spawn half only** — the clauses that "retain that eager
            spawn by decision" for the initial cwd workspace. 22-A7's S3
            scope/trigger extension (spawn may target a non-cwd workspace;
            `session.resume` is a spawn trigger) is **retained**, as is its
            `--resume`-resolved eager attach (an explicit activation).
Amends:     46-D3.2/46-D3.3/46-D3.5/46-I8 and the 46-D3 test row (as in Rev 1);
            **16-D2's bare-cwd `NotRunning`/browsable listing clause** (Rev 3:
            before the first prompt there is no row to list); registry-row timing
            (`run_supervisor_entry`); the `SupervisorApp::run()` zero-workspace
            guard; `SupervisorApp::submit()` (lazy path). All recorded in the
            amendment register §7.
Retained:   16-D2's **lazy-spawn policy** is reinstated (spec 16 already pins it;
            the drift is 22-A7), but its **listing clause is amended** for the
            bare-cwd case: 16-D2 requires the cwd workspace to be listed
            `NotRunning` and browsable, which presumes the row exists; 49-A2
            removes the row until the first prompt, so there is nothing to list
            yet (49-A11). 16 O1–O22 are otherwise unchanged. 22 §3.1/§3.4/§3.7;
            45-D3/45-D4; `MessageDialogModel` / `UiMode::Notice` / `render_notice`
            are unchanged. No wire/protocol change; `kProtocolVersion` stays 1.
Scope:      two changes on the supervisor path: **(A)** lazy workspace/daemon
            creation on the first prompt submission (primary), and **(B)** the
            session-aware Ctrl-S target predicate (secondary safety net).
Verification status: **DRAFT — not yet reviewed.**
```

---

## 1. Purpose, scope, and the user decisions

### 1.1 The original report (decoded)

The user starts `ymh` in directory A without submitting a prompt, runs `/sessions`,
selects a stored session in a different workspace (directory B), and presses
`<Enter>`. The session resumes and the cwd changes to B. Pressing `<Ctrl+S>`
afterwards shows the switcher with two entries (`louvre` with `(no live sessions)`,
`ymh` with `(current session hidden)`). The user expected the single-`<OK>` notice,
"since there is no other active workspace".

### 1.2 The user's new decision (primary; supersedes the Rev 1 framing)

> "I think the right solution is when ymh starts it does not create a workspace by
> default. It shows an empty screen. And only if user typed a prompt and his
> `<Enter>` then workspace is created. Not earlier than that"

The Rev 1 diagnosis found the two switcher entries were *legitimate* because of the
**startup eager cwd spawn** (`src/cli/cli.cpp:514`, authorized by 22 §11.4 / 22-A7)
plus the `/sessions` spawn. The user now rules that the eager spawn itself is wrong.
**Removing it removes the root cause of the reported popup**; the session-aware
switcher predicate from Rev 1 becomes a *secondary* safety net rather than the
primary fix.

### 1.3 What this spec changes in one sentence

A bare `ymh` starts with **zero workspaces and zero daemons** and renders an empty
screen; the cwd workspace's registry row and daemon are created lazily on the first
prompt submission (or an explicit activation), and — separately — the Ctrl-S
switcher shows the `<OK>` notice whenever it has no session to switch to.

### 1.4 What this spec does **not** change

- 16 O1–O22 (ownership/teardown) and the `supervisors` registry table — unchanged.
- 22 §3.1/§3.4/§3.7 (Live-only switcher, ownership marks, ordering) — unchanged.
- 45-D3/45-D4 (Live ⊆ History, focused-session exclusion, no node suppression) —
  unchanged.
- `/sessions`, the catalog reader, the transport, and `ymh --host` — unchanged.
- The daemon's one-time `chdir` to its workspace root (`src/host/workspace_host.cpp:467`)
  — unchanged; the supervisor still never chdirs.

---

## 2. Diagnosis (current state, verified against the shipped tree)

### 2.1 Root cause: the eager cwd spawn and the startup registry row

Two things happen before the TUI renders, unconditionally:

1. `run_supervisor_entry` registers the cwd workspace row:
   `find_or_register_workspace(*canonical, err)` (`src/cli/cli.cpp:495`).
2. It eagerly spawns/attaches the cwd daemon:
   `lifecycle.ensureRunning(row->id, identity)` (`src/cli/cli.cpp:513-519`, the call
   on `:514`). This is the `louvre` daemon in the report. It is authorized by spec
   22 §11.4 / 22-A7 (`docs/design/22-switcher-sessions-errata.md:1772-1796`), which
   explicitly **retains** the eager spawn "by decision".

So even with no prompt, the cwd workspace is registered and its daemon is live.
`/sessions` then spawns the second daemon (`ymh`) via `resume_from_history` →
`ensure_workspace_running` (`src/ui/supervisor.cpp:1083-1091`), producing the
two-entry switcher. The switcher is correct to the specs (§2.5); the *spawn* is the
root cause.

### 2.2 Spec 16 already pins lazy spawn — the shipped code is the drift

Spec 16 §3.2.1 (**16-D2**, `docs/design/16-daemon-ownership.md:529-551`) pins:

> "The supervisor does **not** spawn a daemon at startup. It spawns the cwd
> workspace's daemon on the first of: a submitted prompt (`agent.prompt`/
> `agent.followup`) to that workspace, or an explicit `session.create` / workspace
> activation. Until then the workspace is listed as **`NotRunning`** and is
> browsable read-only. This supersedes the eager `ensureRunning` at
> `cli.cpp:352`/`11 §10.2 :1045` …"

and §7.7 step 3 (`16:2132-2148`) rewires `run_supervisor_entry` with
"no eager `ensureRunning(row->id)`, `:352`". The decision table records
"**16-D2** | Eager or lazy spawn? | **Lazy on first prompt**" (`16:2417`), and the
test plan has "**Spawn-on-prompt (16-D2).** S1 starts with cwd A and **no daemon**;
assert no `host.sock` exists and the workspace shows `NotRunning`. Submit a prompt;
assert the daemon spawns and serves" (`16:2286-2288`).

**The user's decision is therefore spec 16's already-verified design.** Spec 22
§11.4 / 22-A7 later *amended* 16-D2 to authorize the eager initial spawn; spec 49
supersedes that amendment (bare-cwd half) and restores 16-D2.

### 2.3 The shipped supervisor cannot start with zero workspaces (required change)

`SupervisorApp::run()` (`src/ui/supervisor.cpp:402-431`) hard-requires a workspace:

```cpp
for (const SupervisorWorkspace& workspace : options_.workspaces) {
    attach_workspace(workspace);
}
if (model_.workspaces.empty()) {
    return 1;                                              // :406-408
}
model_.activeWorkspaceId = options_.workspaces.front().id; // :409
```

With lazy spawn and no live daemon, `options_.workspaces` is empty and the process
exits `1` before rendering. The empty screen is impossible until this guard is
removed (§4, §5 49-I1). `submit()` is the other blocker: it returns immediately
when there is no active workspace (`:437-440`) and never spawns on the prompt path
(`ensure_workspace_running` is only called from `resume_from_history`, `:1090`).

### 2.4 Zero-daemon supervisor is already representable (spec 16 verdict)

- **The `supervisors` table has no workspace dependency.** The shipped DDL
  (`src/registry/registry.cpp:91-103`) has no foreign key to `workspaces`; spec 16
  §2.3 states it explicitly: "There is no foreign key to `workspaces`: a supervisor
  owns the whole daemon set, and **a supervisor may exist before any daemon does**"
  (`16:250-252`). A supervisor row with zero owned workspaces is already legal —
  **no schema or interface change is required.**
- **Registration is workspace-independent.** The supervisor writes its row at
  startup via `register_presence()` (`src/ui/supervisor.cpp:426`, `:836-849` →
  `INSERT INTO supervisors(id, pid, boot_id, started_at, heartbeat, tty)`,
  `src/registry/registry.cpp:1134`). It does not reference any workspace.
- **The ownership invariants hold vacuously with zero daemons.** O1 ("No
  unsupervised daemon", `16:2170`) is trivially satisfied; O10 ("Orphaning-set
  correctness", `16:2179`) has an empty set; O15 ("Bounded unsupervised lifetime",
  `16:2184`) applies per daemon and is vacuous. The shipped exit path proves it:
  `compute_orphaning_set()` iterates `connections_` (`src/ui/supervisor.cpp:518-531`)
  → empty; `begin_exit` sees `orphaning.empty()` and skips the prompt
  (`:502-512`); `confirm_exit` deregisters presence and calls `teardown_daemons({})`
  (`:656-670`), whose loop body never executes (`:675-708`). **A zero-daemon
  supervisor's clean exit is trivially O1/O10/O15-compliant and never opens the
  last-exit prompt.**
- **The daemon's `chdir` is unaffected.** Only the daemon chdirs
  (`src/host/workspace_host.cpp:467`); there is no `chdir` in `src/ui` or `src/cli`.
  The supervisor holds the intended cwd as a **path** (`SupervisorRunOptions::
  initial_workspace`, set at `src/cli/cli.cpp:538`, read at
  `src/ui/supervisor.cpp:410-417`) and passes it to `HostLifecycle::ensureRunning`
  at spawn time.

### 2.5 The switcher and both sub-lines remain correct (Rev 1 diagnosis, retained)

Both entries are live-renderable (`live_switcher_renderable`,
`src/ui/ui_model.cpp:352-355`; applied at `:1015-1018`), render `o`/`[owned]`
(`src/ui/ui_render.cpp:122-123`, `:138-139`; `src/ui/ui_model.cpp:338-339`),
`(no live sessions)` is the honest empty-state (`src/ui/ui_render.cpp:806-807`), and
`(current session hidden)` marks the active workspace whose only session is the
focused one (`src/ui/ui_model.cpp:1056`; `apply_resume_success` →
`focusSession` → `focusSessionIn` sets `activeWorkspaceId`,
`src/ui/supervisor.cpp:1195`, `src/ui/ui_model.cpp:996`). The switcher opened
instead of the notice only because 46-D3.2 deliberately counts a sessionless live
workspace as a target (`src/ui/supervisor.cpp:938`; `46:1025-1030`). The full
Rev-1 evidence table is retained in §2.6.

### 2.6 Verdict, with the evidence that decided it

| Part | Verdict | Deciding evidence |
|---|---|---|
| Eager cwd spawn at startup | **The defect the user is ruling on** | `src/cli/cli.cpp:514`; authorized by 22 §11.4/22-A7; contradicted by 16-D2 (`16:529-551`) |
| Startup cwd registry row | **Also deferred by this spec** | `find_or_register_workspace` `src/cli/cli.cpp:495` |
| Both switcher rows live | Correct to spec | `live_switcher_renderable` `ui_model.cpp:352-355` |
| `(no live sessions)` / `(current session hidden)` | Correct to spec | `ui_render.cpp:806-807`, `ui_model.cpp:1056` |
| Switcher opens instead of `<OK>` | Correct to 46-D3.2; user wants it changed | `supervisor.cpp:938`; `46:1025-1030` |

**Overall (Rev 1, retained): the reported popup was a UX change, not a bug.** Rev 2
adds: the *root cause* the user identified (eager spawn) **is a defect against
spec 16** and is fixed by 49-D1.

---

## 3. Decision (49-D)

### 49-D1 — Lazy workspace and daemon creation (primary)

A bare `ymh` invocation (no `--resume`, no `--new`, no explicit `--workspace`)
starts with:

- **zero modeled workspaces** (`UiModel::workspaces` empty; `activeWorkspaceId`
  empty),
- **zero daemons** spawned or attached by this supervisor,
- **no cwd `workspaces` registry row** created,
- an **empty screen** (§49-D2), and
- a registered `supervisors` row (16-D3; unchanged).

The workspace and daemon are created **lazily on the first prompt submission**
(`<Enter>` with a non-empty draft). The ordering is pinned and **must never call
`attach_workspace` before the daemon has registered** (Rev 3 HIGH fix):

1. `submit(text)` is called with no active workspace
   (`src/ui/supervisor.cpp:433`, `:437-440`).
2. **Resolve/register the workspace row**: the supervisor resolves the cwd path
   through its **own** registry handle (`options_.registry`,
   `include/ymh/ui/supervisor.hpp:52`) — `options_.registry->findByCanonicalPath(
   canonical)` (`include/ymh/registry/registry.hpp:218-219`) then
   `options_.registry->registerWorkspace(canonical,
   canonical.filename().string())` (`:252-254`) — yielding a `WorkspaceId`. The row
   is required because the daemon is spawned as
   `ymh --host --workspace <uuid> --root <path>` (`src/cli/cli.cpp:229-271`).
   `find_or_register_workspace` (`src/cli/cli.cpp:398-419`) is **not** callable
   here: it lives in the anonymous namespace at `src/cli/cli.cpp:101-906`. The
   supervisor mirrors it in a private helper (`resolve_or_register_workspace`, §4)
   using `options_.registry` — the same handle `register_presence` writes through
   (`:836-849`).
3. **Queue the draft**: `pending_creates_[id] = text`. This reuses the existing
   per-workspace create/prompt queue (`src/ui/supervisor.cpp:1453-1490`), consumed
   on attach in step 6. **No new `pending_prompts_` member exists** (Rev 3).
4. **Spawn the daemon**: `ensure_workspace_running(id, {})`
   (`src/ui/supervisor.cpp:1008-1026`; never on the UI thread, 22 SW12), which
   enqueues an `EnsureRequest` for the owned worker.
5. **Model the workspace and attach — only after the daemon registers.** The worker
   (`ensure_worker_loop`, `src/ui/supervisor.cpp:1028-1079`) calls
   `options_.lifecycle->ensureRunning(...)` (`:1052`), **then** builds the spec via
   `workspace_spec_from_registry(id)` (`:1057`, `:1216-1232`) and enqueues
   `attach_workspace(*spec)` (`:1076`). `workspace_spec_from_registry` returns
   `nullopt` while `row->host` is absent (`:1222-1224`), so it can only succeed
   **after** the spawn; when it is `nullopt` the worker surfaces
   `cannot start workspace: daemon did not register` (`:1058-1059`, 49-F1) and no
   workspace is modeled. `attach_workspace` (`:769-834`) inserts the model
   (`daemonStatus = Connecting`, `live = false`, `:774-784`) and the `connections_`
   entry (`:833`); its `connections_.count(spec.id) != 0` guard (`:770`) makes a
   second pre-spawn attach a no-op — which is exactly why the supervisor must not
   attach early.
6. **Create the session and send the prompt** on `on_link_state(Attached)`
   (`src/ui/supervisor.cpp:1334-1361`): the Rev-3 consume site sets
   `model_.activeWorkspaceId = workspace` when it was empty, then calls
   `create_session(workspace, text)` (`:1453-1490`). Its reply calls
   `apply_create_reply` (`:1496-1532`), which sends the queued prompt via `prompt`
   (`:1526-1527`). `pending_creates_` de-duplicates per workspace (`:1458-1465`),
   so the draft is sent exactly once. This is the same attach branch 46-D7/46-I13
   governs (`46:1521-1535`, `46:3474`): with no live session and no resume in
   flight it would otherwise auto-create an empty session
   (`src/ui/supervisor.cpp:1415-1427`); the Rev-3 consume runs first and the
   de-dup makes that auto-create a no-op.

The draft is pushed to input history and cleared optimistically, as `submit` does
today (`:443-449`). If the spawn fails, the notice ring reports it and no phantom
workspace is retained (49-F1, 22 SW23).

### 49-D2 — The empty state (exact)

With zero workspaces the screen renders:

- **Header**: `ymh` with no cwd suffix (`render_header`, `src/ui/ui_render.cpp:868-873`;
  it already omits the workspace suffix when the id is absent).
- **Transcript**: empty. `build_ui` renders an empty pane for the zero-workspace
  state; it does **not** reuse `render_conversation(nullptr, …)`, which renders
  `(no active session)` for the workspace-without-session case (49-A14).
- **Prompt box**: `> _` (green `> `, empty draft, `_` caret; `render_input`,
  `src/ui/ui_render.cpp:385-400`). The draft is held in the zero-workspace
  composer `UiModel::pendingComposer` (49-A13), so typing works before any
  workspace exists.
- **Status bar**: left segment `no workspace attached — type a prompt to start`
  (dim), right segment the existing aggregate `0 active · 0 waiting`
  (`render_status`, `src/ui/ui_render.cpp:487-505`; the new left string replaces the
  current empty left segment when `model_.workspaces.empty()`).
- **No overlay** (no switcher, no notice).

Typing is fully functional; `<Enter>` on a non-empty draft triggers 49-D1. `/` shows
the command list (`/sessions`, `/quit`, …) as usual. The following commands are
well-defined in this state:

- **`/sessions`** opens the History source over registered workspaces; with zero
  rows the catalog is empty and renders `(no stored sessions)` (or
  `loading stored sessions…` until the first snapshot). Selecting a stored session
  in a non-running workspace spawns/attaches it and resumes it — the existing S3
  path (22 §5), which is how the user's original flow works without a cwd
  workspace.
- **`<Ctrl+S>`** evaluates `switcher_has_targets()` (§49-D5). With zero workspaces
  it is false, so the minimal `<OK>` notice renders with
  `No other workspaces available` (46-D3; §49-D6). This matches the user's earlier
  expectation that a switcher with nothing to switch to shows just `<OK>`.

### 49-D3 — Paths that still create/attach a workspace or daemon (each proven)

| Path | Spawns? | Evidence |
|---|---|---|
| Bare `ymh` startup | **No** (changed) | This spec; 16-D2 `16:529-551` |
| First prompt submission | **Yes** | `submit` `supervisor.cpp:433`; `create_session` `:1453`; spawn seam 16 §7.7 `16:2145-2146` |
| `/sessions` → select a stored session | **Yes** (retained) | `select_history` `:2420-2433` → `resume_from_history` `:1083-1091` → `ensure_workspace_running` `:1090`; 22 §5 |
| `/sessions` → select an already-attached workspace node | Attaches only (no spawn) | `select_history` `:2425-2431` (`focusWorkspace`) |
| `ymh --resume <id>` | **Yes** (retained; explicit activation) | `run_supervisor_entry` resolves the session's workspace `cli.cpp:466-488`; eager attach `cli.cpp:513-519`; `pending_resume_` seed `supervisor.cpp:422-424`. 22-A7's `--resume` eager authorization is retained. |
| `ymh --new` | **Yes** (explicit activation; open question OQ-49-6) | `cli.cpp:1198-1200`; 50-D2.4 |
| `ymh run "<task>"` | **No daemon spawned** (unchanged) | It attaches only to an already-live daemon (`cli.cpp:1214-1219` → `run_via_daemon`, whose `ensureRunning` at `:561` attaches), else runs in-process (`run_headless`, `src/cli/headless.cpp:97`; no `HostLifecycle`) |
| `ymh --host …` (daemon entry) | **Unaffected** | `run_host_command` `cli.cpp:229-325`; it is the spawned process and is only invoked by a spawner; it requires `--workspace`/`--root` (`:268-271`), which the lazy first-prompt path supplies |
| Explicit workspace activation (Ctrl+N, `/new`) | **No in the zero-workspace state** | `new_session` no-ops when there is no active workspace (`src/ui/supervisor.cpp:1676-1681`); Ctrl+N dispatch `:2813-2814`; 16-D2 lists `session.create` as a trigger (`16:534-535`), but the UI cannot issue one without an active workspace. See OQ-49-1. |

### 49-D4 — Workspace lifetime after lazy creation

Once a lazily-created daemon exists it is **owned by the supervisor for the TUI
session** and is **not** torn down when idle (16 §2.7, O1/O15: a daemon is stopped
only when it has no owner). It is torn down by the last supervisor's clean exit
(`confirm_exit` → `teardown_daemons`, `src/ui/supervisor.cpp:656-708`), by an
explicit `ymh workspace stop` (16 §4.6), or by the owner watchdog on crash
(16 §5.1). No new teardown is introduced.

### 49-D5 — The session-aware Ctrl-S target predicate (secondary safety net)

Retained from Rev 1, with the Rev-3 active-workspace fallback (49-D5.1).
`switcher_has_targets()` (`src/ui/supervisor.cpp:936-956`) returns `true` iff at
least one of:

1. There exists a live-renderable workspace `W` (`live_switcher_renderable`,
   `src/ui/ui_model.cpp:352-355`) with `W.id != activeWorkspaceId` whose session
   membership is **unknown**, or which has at least one **visible live session
   leaf** (`SessionCell c ∈ W.sessions` with `catalog_has_session(W.id, c.id)`).
2. The active workspace has at least one visible live session leaf after the
   focused-session exclusion (`c.id != focused`) that is either catalogued
   (`catalog_has_session(active.id, c.id)`) **or** belongs to the active workspace
   whose membership is **unknown** (49-D5.1).

**49-D5.1 — "Membership unknown" covers the active workspace too (Rev 3).**
"Membership unknown" for a workspace `W` = the catalog is not loaded
(`!catalog.loaded || catalog.generation == 0`), **or** `W` is absent from
`model.catalog.workspaces`, **or** `W`'s `WorkspaceHistory::note` is set
(`catalog_has_session`, `src/ui/ui_model.cpp:448-461`). Rev 2 applied the fallback
only to non-active workspaces (clause 1). With a pending catalog and a second
visible session in the **active** workspace, clause 2's `catalog_has_session` is
false and clause 1 excludes the active workspace, so the predicate was false and a
**false "nothing available" notice** rendered. Rev 3 extends the unknown fallback
to the active workspace's non-focused visible leaves: if the active workspace's
membership is unknown, any non-focused visible leaf is a target. This is the same
truthfulness rule (45-D3), applied symmetrically.

The conservative fallback prevents a false "nothing available" during the load
window (45-D3's truthfulness rule).

**Why it is still needed.** Lazy spawn eliminates the *original* repro (after the
resume there is exactly one live workspace, so the unmodified 46-D3.2 predicate
already yields the notice). But the two-workspace shape recurs once a cwd workspace
is created by a prompt and a second workspace is then resumed via `/sessions`:
workspace A is live with zero sessions, B is active with one focused session.
Without 49-D5 the switcher would again render the useless two-placeholder popup.

### 49-D6 — Notice texts, including the zero-workspace case

The notice is shown iff `!switcher_has_targets()`. Its text is:

- **`No other workspaces available`** when no live-renderable workspace other than
  the active one exists. This now also covers the **zero-workspace** case (no
  active workspace either) — 46-D3's text is reused verbatim; no third string.
- **`No other sessions available`** when other live-renderable workspace(s) exist
  but none has a visible live session, and the active workspace has no other
  visible session.

Both render identically: a centered `workspaces` window with the message, a
separator, and exactly one inverted `[ OK ]` row (`render_notice`,
`src/ui/ui_render.cpp:858-866`).

### 49-D7 — The open path

`SupervisorApp::openSwitcher` (`src/ui/supervisor.cpp:959-968`) evaluates the
predicate; if false, it sets the message text (49-D6), `message.open = true`,
`mode = UiMode::Notice`, marks dirty, and returns without touching
`model_.switcher`. Otherwise it behaves exactly as today. The Ctrl-S handler keeps
requesting a catalog refresh (`:2797-2804`).

### 49-D8 — Key handling

While the notice is up (`mode == UiMode::Notice && message.open`):
`<Enter>`/`<Esc>`/`<Ctrl+C>` dismiss it (`handle_notice`, `src/ui/supervisor.cpp:971-979`;
dispatch `:2791-2792`); every other key, **including `<Ctrl+S>`/`<Ctrl+P>`, is
swallowed** (matching the shipped notice and the switcher's own behaviour,
`:2475`, `:2794-2804`). Input blocking is unchanged (`modal_owns_input`,
`:1619`).

### 49-D9 — The switcher, when it opens, is unchanged

The active workspace node is still rendered and focusable; the focused session is
still hidden; leaves and ordering are unchanged (22 SW1/SW5/SW17; 45-D4).

### 49-D10 — No new mode, wire surface, or switcher model

`UiMode::Notice`, `MessageDialogModel` (`include/ymh/ui/ui_model.hpp:417-421`), and
`render_notice` are reused. The lazy path adds no protocol method; the daemon is
spawned exactly as today (`ymh --host …`) once the row exists. `kProtocolVersion`
stays 1. The change is confined to `src/cli/cli.cpp` (`run_supervisor_entry`),
`src/ui/supervisor.cpp` (`run`, `submit`, `switcher_has_targets`, `openSwitcher`),
and the empty-state status string in `src/ui/ui_render.cpp`.

One model field is added: `UiModel::pendingComposer` (49-A13), the zero-workspace
composer. It exists only because the shipped composer is session-scoped
(`handle_input` operates on `SessionUiState`); without it the empty screen would
not be typeable. It is not a switcher/notice model and adds no wire surface.

---

## 4. C++ interface sketch

```cpp
// ── src/cli/cli.cpp (run_supervisor_entry) ────────────────────────────────
// 49-D1: bare TUI startup registers NO cwd workspace and spawns NO daemon.
// The row is created on the first prompt (49-D3). `--resume`/`--new` are
// explicit activations and keep the eager attach.
int run_supervisor_entry(..., const std::string& resume_session, bool new_session, ...) {
    // resolve --resume to its own workspace (unchanged) ...
    const std::optional<std::filesystem::path> canonical = canonicalize(resolved_root);
    // 49-D1: do NOT call find_or_register_workspace() for the bare cwd case.
    // 49-D3: --resume/--new still resolve/register + attach (unchanged).
    std::optional<WorkspaceRecord> row;
    if (!resume_session.empty() || new_session) {
        row = find_or_register_workspace(*canonical, err);   // explicit activation
    }
    // ... open registry; scan; seed options ...
    options.initial_workspace = *canonical;   // the intended cwd, held as a PATH
    // The scanner seeds options.workspaces with any already-live daemons; with
    // none, options.workspaces is EMPTY and the TUI must still render.
}

// ── src/ui/supervisor.cpp (SupervisorApp::run) ────────────────────────────
int run() {
    for (const SupervisorWorkspace& workspace : options_.workspaces) {
        attach_workspace(workspace);
    }
    // 49-D1: zero workspaces is a valid start state (the empty screen).
    // REMOVED: if (model_.workspaces.empty()) return 1;
    if (!model_.workspaces.empty()) {
        model_.activeWorkspaceId = options_.workspaces.front().id;
        if (!options_.initial_workspace.empty()) { /* cwd-wins, unchanged */ }
    }
    if (options_.initial_resume.has_value()) {
        pending_resume_[options_.initial_resume->first] = options_.initial_resume->second;
    }
    register_presence();   // 16-D3: a supervisors row with zero workspaces is legal
    start_scanner();
    start_catalog();
    ensure_worker_ = std::jthread([this](std::stop_token s) { ensure_worker_loop(s); });
    return run_loop();
}

// ── src/ui/supervisor.cpp (SupervisorApp::submit) — the lazy path ─────────
void submit(const std::string& text) override {
    if (text.empty()) {
        return;
    }
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace == nullptr) {
        // 49-D1: first prompt with no workspace -> register the row, queue the
        // draft, and spawn. The workspace is modeled+attached by
        // `ensure_worker_loop` ONLY after the daemon registers; this function
        // must NOT call `attach_workspace`/`workspace_spec_from_registry` here.
        const std::optional<WorkspaceId> id =
            resolve_or_register_workspace(options_.initial_workspace);  // helper below
        if (!id.has_value()) {
            push_notice("cannot create workspace: registry unavailable");  // 22 H1, no phantom
            return;                                                   // 49-F3
        }
        pending_creates_[*id] = text;      // consumed on Attached (step 6)
        ensure_workspace_running(*id, {}); // :1008, worker thread
        return;
    }
    // ... existing behavior (push history, create_session / prompt) unchanged ...
}

// ── src/ui/supervisor.cpp (SupervisorApp) — the attach consume site ───────
// 49-D1 step 6: inside `on_link_state(Attached)` (:1334), BEFORE
// `refresh_sessions`. The draft rides `pending_creates_`; `create_session`
// re-queues it, so no `pending_prompts_` member exists.
if (const auto it = pending_creates_.find(workspace);
    it != pending_creates_.end() && !it->second.empty()) {
    std::string draft = std::move(it->second);
    pending_creates_.erase(it);                    // so create_session proceeds
    if (model_.activeWorkspaceId.value.empty()) {
        model_.activeWorkspaceId = workspace;      // first lazy workspace becomes active
    }
    create_session(workspace, std::move(draft));   // :1453 -> :1496 -> :1526 prompt
}

// ── src/ui/supervisor.cpp (SupervisorApp) — the registry helper ───────────
// Mirrors `find_or_register_workspace` (src/cli/cli.cpp:398-419), which is NOT
// reachable from this translation unit (anonymous namespace, cli.cpp:101-906).
// Uses `options_.registry` (supervisor.hpp:52): findByCanonicalPath
// (registry.hpp:218-219) then registerWorkspace (:252-254).
std::optional<WorkspaceId> resolve_or_register_workspace(
    const std::filesystem::path& canonical);

// ── src/ui/supervisor.cpp (SupervisorApp) — the switcher predicate ────────
// 49-D5: unchanged from Rev 1 (catalog_membership_unknown / has_visible_leaf /
// switcher_has_targets / other_live_workspace_exists); openSwitcher picks the
// text per 49-D6.
[[nodiscard]] bool switcher_has_targets() const;   // full definition in §49-D5
[[nodiscard]] bool other_live_workspace_exists() const;
void openSwitcher();                               // 49-D6/49-D7
```

No new member is required: the draft rides the existing `pending_creates_` map,
and `handle_notice` (`src/ui/supervisor.cpp:971-979`) is retained verbatim (49-D8).
The only signature change is `run_supervisor_entry` if `new_session` is threaded
through.

---

## 5. Invariants

| ID | Invariant |
|---|---|
| 49-I1 | A bare `ymh` starts with `UiModel::workspaces` empty and `activeWorkspaceId` empty, and creates **no** cwd `workspaces` registry row and **no** daemon before the first prompt. `SupervisorApp::run()` never returns because `workspaces.empty()`; it returns only on user quit. |
| 49-I2 | The empty state renders the header `ymh`, an empty transcript, the `> _` prompt box, and a status bar whose left segment is `no workspace attached — type a prompt to start` and whose right segment is `0 active · 0 waiting` (49-D2). |
| 49-I3 | The first non-empty prompt submission is the sole trigger that (a) registers the cwd workspace row, (b) queues the draft, (c) spawns the daemon, (d) models/attaches the workspace **only after** the daemon registers, and (e) creates the session and sends the prompt, in that order (49-D1). The supervisor never calls `attach_workspace` before the daemon registers. |
| 49-I4 | `/sessions` selection, `--resume`, and `--new` retain their spawn/attach paths; `ymh run` and `ymh --host` are unaffected (49-D3). |
| 49-I5 | A supervisor with zero daemons is valid: its `supervisors` row exists (16-D3), its clean exit deregisters and tears down zero daemons, and the last-exit prompt does not open (16 O1/O10/O15 vacuous). |
| 49-I6 | The supervisor never calls `chdir()`; the intended cwd is held as a path (`SupervisorRunOptions::initial_workspace`) and passed to `HostLifecycle::ensureRunning` at spawn time (49-D1). |
| 49-I7 | The Ctrl-S notice renders iff `!switcher_has_targets()` (the 49-D5 session-aware predicate). |
| 49-I8 | When the notice is up, `model_.switcher` is not rebuilt and `mode == UiMode::Notice`; no `(current session hidden)`, `(no live sessions)`, workspace row, or session row is rendered. |
| 49-I9 | The notice contains the message plus exactly one option row, `[ OK ]` (inverted), in the `workspaces` window (49-D6). |
| 49-I10 | `<Enter>`/`<Esc>`/`<Ctrl+C>` dismiss the notice; every other key, including `<Ctrl+S>`/`<Ctrl+P>`, is swallowed and the notice remains (49-D8). |
| 49-I11 | The Ctrl-S switcher, when it opens, still contains only `live && daemonStatus ∈ {Attached, Stopping}` nodes (22 SW1/SW5) ordered title-asc/`canonical_path` (22 SW17); the active workspace node is still rendered (45-I7). |
| 49-I12 | `/sessions` is unchanged: it enumerates every registered workspace and focuses an attached sessionless workspace node on `<Enter>` (`src/ui/supervisor.cpp:2420-2433`). |
| 49-I13 | `switcher_has_targets()` never returns `false` while (a) a live-renderable non-active workspace's session membership is unknown (catalog pending, absent from snapshot, or `note` set), or (b) the active workspace's membership is unknown and it has a non-focused visible session leaf (49-D5.1). |
| 49-I14 | The notice text is exactly `No other workspaces available` when `other_live_workspace_exists()` is false (including the zero-workspace state), and exactly `No other sessions available` otherwise. |
| 49-I15 | The zero-workspace empty screen is typeable: printable/editing keys operate on `UiModel::pendingComposer`, the draft renders in the prompt box, and `<Enter>` on a non-empty draft runs the 49-D1 lazy path. |

---

## 6. Failure modes (49-F)

Continue the repo's `F1–F12` convention with the spec-local `49-F` prefix
(disjoint from `§54 F1–F12`).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 49-F1 | First-prompt spawn fails (`HostLifecycle::ensureRunning` throws) | notice `cannot start workspace: <err>`; the workspace is not retained (no phantom, 22 SW23); the draft is preserved in the composer | user may retry; no daemon, no registry row change beyond the row already written by `find_or_register_workspace` |
| 49-F2 | cwd gone at first prompt | `find_or_register_workspace`/canonicalize fails; notice `workspace missing`; no spawn | user cds to a valid dir and restarts |
| 49-F3 | Registry unwritable at first prompt | notice `cannot create workspace: registry unavailable`; no spawn | user fixes `XDG_STATE_HOME` permissions |
| 49-F4 | User quits with zero daemons | `compute_orphaning_set()` is empty; `confirm_exit` deregisters presence and tears down nothing; no prompt | none needed (49-I5) |
| 49-F5 | `/sessions` selected while no cwd workspace exists | the target workspace spawns/attaches and is resumed; a new active workspace appears | unchanged S3 path (22 §5) |
| 49-F6 | A live session exists in another workspace but is not yet in the catalog snapshot (45-D3 subset), and the workspace is already in the snapshot without it | predicate false → notice although a live session exists | Ctrl-S already calls `catalog_->refreshNow()`; the next Ctrl-S opens the switcher (accepted residual) |
| 49-F7 | Another live workspace's catalog read failed (`note` set) | treated as unknown → switcher opens with the 45-D4.4 note leaf, never a false notice | none needed |
| 49-F8 | Catalog not yet loaded and another live workspace exists | fallback → switcher opens with `(loading live sessions…)` | first snapshot lands; the next Ctrl-S applies the strict predicate |
| 49-F9 | A new live workspace/session appears while the notice is up | notice stays until dismissed (point-in-time) | dismiss; the next Ctrl-S re-evaluates (mirrors 46-F5) |
| 49-F10 | The active workspace is evicted while the notice is up | `activeWorkspaceId` promoted by `UiModel::eraseWorkspace` (`src/ui/ui_model.cpp:604-629`); the rendered notice is unaffected | dismiss; the next Ctrl-S re-evaluates |
| 49-F11 | Only the active workspace is live, but it has a second visible session | predicate true → switcher opens | a valid switch is never hidden |

---

## 7. Amendment register

| ID | Amended clause | Verified code anchor | New behaviour |
|---|---|---|---|
| 49-A1 | 22 §11.4 (`22:1785-1796`) and 22-A7 (`22:147`, `22:1776-1783`) — the eager initial cwd spawn | `run_supervisor_entry` eager `lifecycle.ensureRunning` (`src/cli/cli.cpp:514`) | **Superseded (bare-cwd half).** 16-D2's lazy spawn is reinstated: no eager spawn for a bare `ymh`. 22-A7's S3 scope/trigger extension and its `--resume`-resolved eager attach are **retained**. |
| 49-A2 | `run_supervisor_entry` registry-row timing | `find_or_register_workspace` (`src/cli/cli.cpp:495`) | The cwd row is **not** written at startup for a bare `ymh`; it is written on the first prompt (49-D1). `--resume`/`--new` still resolve/register eagerly. |
| 49-A3 | `SupervisorApp::run()` zero-workspace guard | `if (model_.workspaces.empty()) return 1;` (`src/ui/supervisor.cpp:406-408`) | **Removed.** Zero workspaces is a valid start state (empty screen); `activeWorkspaceId` is set only when a workspace exists. |
| 49-A4 | `SupervisorApp::submit()` | `src/ui/supervisor.cpp:433-457` | Gains the lazy path: with no active workspace, register + model + spawn + queue the prompt (49-D1). |
| 49-A5 | 46-D3.2 (target predicate) | `switcher_has_targets` (`src/ui/supervisor.cpp:936-956`) | A live-renderable non-active workspace is a target only if it has a visible live session leaf **or** its membership is unknown; a sessionless live workspace is no longer a target (49-D5). |
| 49-A6 | 46-D3.3 (open path) and the notice text | `openSwitcher` (`src/ui/supervisor.cpp:959-968`) | Two notice texts (49-D6); the second covers "other live workspace(s) exist, no switchable session"; the first also covers the zero-workspace state. |
| 49-A7 | 46-D3.5 (dismissal) | `handle_notice` (`src/ui/supervisor.cpp:971-979`) | Retained; `<Ctrl+S>`/`<Ctrl+P>` explicitly pinned as swallowed (49-D8). |
| 49-A8 | 46-I8 | 46 §7 invariant table (`46:3469`) | The form "notice iff `switcher_has_targets()` is false" is retained; the predicate is redefined by 49-A5. |
| 49-A9 | 46 §5.3 test row | `UI46_D3_OtherLiveWorkspaceShowsSwitcher` (`tests/unit/errata46_ui_test.cpp:211-225`) | **Retained, not flipped.** It seeds no catalog snapshot, so it exercises the 49-F8 fallback and still expects `UiMode::Switcher`. The strict loaded-catalog case is the new `49-U1`. |
| 49-A10 | 22 §3.1/§3.4/§3.7; 45-D3/45-D4 | (no code change) | **Not amended.** The Live-only display predicate, ownership marks, ordering, the focused-session exclusion, and the placeholder leaves are unchanged. |
| 49-A11 | 16-D2 lazy-spawn policy / 16 O1–O22 | `16:529-551`; (no code change) | **Policy reinstated; listing clause amended.** The shipped drift is 22-A7; 16-D2's lazy-spawn policy and O1–O22 are restored. But 16-D2 also requires the cwd workspace to be "listed as **`NotRunning`** and … browsable read-only" (`16:537-538`), which presumes the registry row exists; 49-A2 removes the row until the first prompt, so for a **bare `ymh`** there is nothing to list and the `NotRunning` listing clause is **amended**: before the first prompt the workspace is absent, not `NotRunning`. Once the first prompt creates the row+daemon, 16-D2's listing semantics resume. A zero-daemon supervisor needs no schema change (`16:250-252`; `src/registry/registry.cpp:91-103`). |
| 49-A12 | 46-D7 / 46-I13 / 23-D58 / 50-D2 | `46:1521-1535`, `46:3474`; `23:375`, `23:1253-1293`; `50 §4.2 (50-D2)` | **Reconciled, not a conflict.** Rev 2 cited a "48-D3.3 cwd always-win" clause; **that reference was fabricated** — spec 48 has no `D3` decision and no "cwd always wins" text (48 §15 records that the old 48-D3, the Ctrl+C re-entry item, moved to spec 50 as 50-D2; 48's kept decisions are D2/D4–D8). The real overlap is with the fresh-launch create branch: **46-D7.1** retains `focus live.front()` (`46:1530`) and **46-I13** auto-creates an empty session on `Attached` (`46:3474`), while **23-D58** owns the refresh auto-create. Spec **50-D2.1** replaces the live-focus with `create_session` ("always create"), and 49-D1's lazy first prompt routes through `create_session` on that same attach branch (49-D1 step 6). 49-D1 and 50-D2 agree: on a bare launch there is no workspace until the first prompt, and the fresh-launch guarantee applies once it exists (50-OQ-6). No conflict; see spec 50 §14 (cross-spec reconciliation). |
| 49-A13 | `UiModel` (model surface) | `struct UiModel` (`include/ymh/ui/ui_model.hpp:523`) | **One field added:** `SessionUiState pendingComposer`, the zero-workspace composer. The shipped composer is session-scoped (`handle_input` takes a `SessionUiState*`), so without it the empty screen could not be typed into. `handle_input`/`render_input` fall back to it only while `workspaces` is empty. Never a session cell, never persisted, no wire surface (49-D10 amended). |
| 49-A14 | 49-D2 transcript rendering | `render_conversation` (`src/ui/ui_render.cpp:397-420`) | `build_ui` renders an **empty** pane for the zero-workspace state instead of `render_conversation(nullptr, …)`, which renders `(no active session)`. The workspace-without-session case is unchanged. |
| 49-A15 | `create_session` reply seam | `connection->second->submit` (`src/ui/supervisor.cpp:1487`) | Routed through `submit_to` so the `session.create` reply terminal is drivable by the 46-D7 canned-reply test seam; production behaviour is identical (no canned reply installed). |

---

## 8. dsh mapping

| ymh concept | dsh analogue | Note |
|---|---|---|
| Lazy workspace creation on first prompt | dsh's lazy session/host start on first input | Spec 16 already pins it (16-D2); 49 removes the shipped eager drift. |
| Empty screen with no workspace | dsh's idle/blank start state | Header, empty transcript, prompt box, `no workspace attached` status. |
| Ctrl-S Live switcher | the live session/workspace switcher | Live-only predicate retained (22 §3). |
| The `<OK>` notice | the "nothing to switch to" acknowledgement | 46-D3 for the single-workspace case; 49 extends it to the sessionless-other-workspace and zero-workspace cases. |
| `/sessions` History | the persisted session browser | Unchanged (22 §4); remains the way to resume stored work without a cwd workspace. |

---

## 9. Test plan

Strategy is `00 §44`. ID scheme: `49-U*` unit, `49-G*` golden render, `49-P*`
PTY/live. Spec 49 does not edit tests; the implementation phase does.

### 9.1 Fixtures

- `SupervisorHarness` (`tests/support`; used at `tests/unit/errata46_ui_test.cpp`)
  with `seed_active_workspace`, `seed_workspace`, `activate_session`,
  `open_switcher`, `on_catalog_snapshot`, `dispatch_key`. **New seams needed:**
  a registry-backed workspace counter (to assert "no row before the first prompt"),
  a spawn counter / fake `HostLifecycle`, and a `submit` entry point.
- `live_workspace(id, title)` (`tests/unit/errata46_ui_test.cpp:117-125`).
- Catalog-snapshot builder (`tests/unit/errata46_ui_test.cpp:244-255`).
- Golden: `build_model()` (`tests/unit/ui_render_golden_test.cpp:99`),
  `normalize(render_to_ansi(...))`, and an **empty** `UiModel` for the empty screen.
- PTY: `tests/support/host_harness.hpp` (hermetic `XDG_STATE_HOME`/`HOME`),
  `wait_for` / `wait_for_frame_absent`, plus a filesystem check for `host.sock`.

### 9.2 Unit (new)

| ID | Test | Asserts |
|---|---|---|
| 49-U1 | `UI49_D5_OtherLiveWorkspaceWithoutSessionsShowsNotice` | active `ws-a` focused `s1`; live `ws-b` zero sessions; **loaded** catalog → Notice, `No other sessions available`. |
| 49-U2 | `UI49_D5_OtherLiveWorkspaceWithSessionShowsSwitcher` | live `ws-b` with catalogued `s2` → Switcher. |
| 49-U3 | `UI49_D5_ActiveWorkspaceSecondSessionShowsSwitcher` | active `ws-a` with focused `s1` + catalogued `s2` → Switcher. |
| 49-U4 | `UI49_D6_OnlyLiveWorkspaceNoticeText` | active only → Notice, `No other workspaces available`. |
| 49-U5 | `UI49_D5_CatalogPendingFallsBackToSwitcher` | another live workspace + no catalog → Switcher (49-I13). |
| 49-U6 | `UI49_D5_NoteWorkspaceFallsBackToSwitcher` | other live workspace with `note` set → Switcher (49-F7). |
| 49-U7 | `UI49_D8_NoticeDismissal` | `enter`/`escape`/`ctrl+c` dismiss; `ctrl+s` swallowed. |
| 49-U8 | `UI49_D5_SessionlessLiveWorkspaceReachableViaSessions` | `/sessions` focuses `ws-b` (49-I12). |
| 49-U9 | `UI49_D8_NoticeBlocksComposer` | a character key does not reach the composer with the notice up. |
| 49-U10 | `UI49_D1_NoWorkspaceAtStartup` | `run()` with `options.workspaces` empty does **not** return 1; `mode == Conversation`, `workspaces` empty, `activeWorkspaceId` empty. |
| 49-U11 | `UI49_D1_EmptyStateNoticeOnCtrlS` | zero workspaces → `<Ctrl+S>` → Notice, text `No other workspaces available` (49-D2/49-D6). |
| 49-U12 | `UI49_D1_FirstPromptSpawnsWorkspace` | `submit("hi")` with no workspace → a workspace is registered/modeled, `ensure_workspace_running` is invoked once, and after `on_link_state(Attached)` a session is created and the prompt is sent exactly once. |
| 49-U13 | `UI49_D1_NoRegistryRowBeforePrompt` | before `submit`, the registry has **no** `workspaces` row for the cwd; after `submit`, exactly one. (Also asserts the `supervisors` row exists throughout.) |
| 49-U14 | `UI49_D1_SpawnFailureNoPhantom` | spawn throws → notice, `model_.workspaces` unchanged (no phantom), draft preserved (49-F1). |
| 49-U15 | `UI49_I5_ZeroDaemonCleanExit` | `begin_exit(true)` with zero daemons → no prompt, `confirm_exit` runs, presence deregistered, `teardown_daemons` called with an empty set. |

**Existing 46-D3 unit tests: no flip required** (retained from Rev 1).
`UI46_D3_OtherLiveWorkspaceShowsSwitcher` (`:211-225`) seeds no catalog snapshot,
so it exercises the 49-F8 fallback; `UI46_D3_OtherSessionShowsSwitcher` (`:227-261`)
uses clause 2; the single-workspace tests (`:144`, `:162`, `:178`, `:194`, `:263`,
`:283`) are unchanged. Add a comment to `:211` marking it as the fallback case.

### 9.3 Golden render

| ID | Test | Asserts |
|---|---|---|
| 49-G1 | `UI49_G1_NoticeNoOtherSessions` | `No other sessions available` + `[ OK ]`; no `Switcher`, no placeholder leaf, no workspace row. |
| 49-G2 | `UI49_G1_MultiWorkspaceSwitcherUnchanged` | two live workspaces each with a catalogued session → normal switcher (both rows `[owned]`, leaves, footer). **Regression guard.** |
| 49-G3 | `UI46_G1_NoticePopup` (amended) | cover both texts and the `[ OK ]`-only shape. |
| 49-G4 | `UI49_G4_EmptyScreen` | an empty `UiModel` (zero workspaces) renders header `ymh`, the `> _` prompt box, and the status bar `no workspace attached — type a prompt to start` / `0 active · 0 waiting`; no switcher, no notice, no workspace row. |
| 49-G5 | `UI49_G5_LazyFirstSubmit` | after the first submit, the modeled workspace and its session render (header gains the cwd, transcript shows the user message). |

### 9.4 PTY / live (`tests/unit/ui_supervisor_pty_test.cpp`)

| ID | Test | Asserts |
|---|---|---|
| 49-P1 | `UI49_P1_ResumeThenCtrlSShowsNotice` | start bare, `/sessions` → resume a session in workspace B, then `<Ctrl+S>` → `[ OK ]` notice, not the switcher. |
| 49-P2 | `UI49_P2_TwoWorkspacesWithSessionsShowsSwitcher` | after two workspaces each have a session, `<Ctrl+S>` shows the switcher. |
| 49-P3 | `UI49_P3_NoDaemonBeforePrompt` | bare `ymh` under a PTY: assert **no** `host.sock` for the cwd and **no** cwd `workspaces` row in the registry; then submit a prompt and assert the daemon spawns and serves (mirrors 16 §8.4 test 1, `16:2286-2288`). |
| 49-P4 | `UI49_P4_FirstPromptRendersSession` | after the first prompt the transcript renders the reply (Fake LLM; opt-in live layer per 22 SW-P3 discipline). |

No `sleep`-based synchronisation beyond the injected clock/interval seams (16 §8.4).

---

## 10. Out of scope, recorded risks, and open questions

### 10.1 Recorded risks

- **This changes verified-spec behaviour** (46-D3.2) and **supersedes** part of a
  verified spec (22-A7), so it must pass the independent gate before any code.
- **Cross-spec reconciliation with the fresh-launch create branch.** 46-D7.1 keeps
  `focus live.front()` (`46:1530`) and 46-I13 auto-creates an empty session on
  `Attached` (`46:3474`); 23-D58 owns the refresh auto-create; spec 50-D2.1 changes
  the live-focus to `create_session` ("always create"). 49-D1's lazy first prompt
  routes through the same `create_session` path on that attach branch, and 50-D2
  scopes its guarantee to "once the cwd workspace exists" (50-OQ-6). No conflict;
  see spec 50 §14. (Rev 2's "48-D3 conflict" was a fabricated reference and is
  removed — see 49-A12.)
- **A sessionless live workspace becomes unreachable from Ctrl-S** (49-D5); the
  capability remains via `/sessions` (49-I12).
- **The notice is a point-in-time decision** (49-F9).

### 10.2 Open questions

1. **Does an explicit `session.create` (Ctrl+N / `/new`) spawn without a prompt?**
   16-D2 lists it as a trigger (`16:534-535`), but the shipped UI cannot exercise it
   in the zero-workspace state: `new_session()` does nothing when
   `model_.activeWorkspace()` is null (`src/ui/supervisor.cpp:1676-1681`; Ctrl+N
   dispatch `:2813-2814`), so Ctrl+N/`/new` are **not** spawn triggers for a bare
   `ymh`. This spec retains 16-D2's `session.create` trigger only for an
   **already-modeled** workspace; the user's stricter reading ("only if user typed a
   prompt") holds for the zero-workspace state. No new spawn path is added.
2. **Exact empty-state status string.** `no workspace attached — type a prompt to
   start` is proposed; the user only said "empty screen". Confirm in review.
3. **`--new` at startup.** Treated as an explicit activation that spawns (49-D3);
   spec 50-D2.4 (which owns the former 48-D3.4 `--new` handling) forces
   `create_session` on attach. Reconciled (49-A12).
4. **`--workspace <dir>` flag.** Does an explicit `--workspace` count as user
   intent to create eagerly, or stay lazy until the first prompt? Not verifiable
   from the decision; this spec leaves it lazy (same as cwd).
5. **Zero-workspace `/sessions`.** With no cwd row, the catalog is empty; confirm
   the exact rendering (`(no stored sessions)` vs a new empty-state string).
6. **The `note` fallback (49-F7).** Treating a failed catalog read as
   "unknown → target" is conservative; confirm it is preferred over a notice.
7. **Fresh-launch create branch reconciliation** (49-A12). Rev 2's "48-D3
   conflict" was a **fabricated** reference: spec 48 has no `D3` and no "cwd always
   wins" clause. The real overlap is 46-D7.1/46-I13/23-D58 and spec 50-D2; it is
   reconciled by scoping — 49-D1 owns the lazy-creation timing and 50-D2's
   guarantee applies once the cwd workspace exists (50-OQ-6). No open question
   remains. The fabricated reference removal is also recorded in spec 50 §14.
8. **Supervisor row when zero workspaces.** Confirmed representable (§2.4); the
   exit-prompt count is empty and the badge shows no daemons. Confirm the peer
   badge / aggregate UX in a zero-workspace supervisor.

### 10.3 Scope boundary

No change to `SwitcherOverlayModel`, `render_switcher`, the catalog reader, the
registry schema, the daemon, the wire protocol, or `/sessions`. The diff is
`run_supervisor_entry` (`src/cli/cli.cpp`), `run`/`submit`/`switcher_has_targets`/
`openSwitcher` (`src/ui/supervisor.cpp`), and the empty-state status string
(`src/ui/ui_render.cpp`).

---

## 11. Revision log

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-09-21 | Initial draft. Diagnosis of the user's report: **UX CHANGE, not a BUG** — the two live entries, both placeholder leaves, the `o [owned]` marks, the two live daemons (eager cwd spawn + S3 resume spawn), and the switcher opening are all correct to 22 §3.1/§11.4 and 46-D3.2. Decision 49-D (Rev 1): make `switcher_has_targets()` session-aware, add the `No other sessions available` text, retain the 46-D3 dismissal, reuse `MessageDialogModel`/`render_notice`. Amendment register 49-A1–A7 (46-D3.2/46-D3.3/46-D3.5/46-I8/46 §5.3 amended; 22 and 45 not amended). Invariants 49-I1–I8; failure modes 49-F1–49-F7; tests 49-U/G/P; fixtures; open questions. |
| 2 | 2026-09-21 | **New user decision (primary): no workspace/daemon at bare `ymh` startup; created lazily on the first prompt.** Diagnosis extended: the eager cwd spawn (`src/cli/cli.cpp:514`, authorized by 22 §11.4/22-A7) is the root cause; spec 16 §3.2.1 (16-D2) **already pins lazy spawn**, so the shipped code is the drift. Verified a zero-daemon supervisor is representable (no FK in the `supervisors` DDL, `src/registry/registry.cpp:91-103`; `16:250-252`; O1/O10/O15 vacuous; `compute_orphaning_set`/`confirm_exit`/`teardown_daemons` handle an empty set). Found and recorded the shipped blockers: `run()` returns 1 on empty `workspaces` (`src/ui/supervisor.cpp:406-408`) and `submit()` never spawns (`:437-440`). Decisions renumbered: **49-D1** lazy creation, 49-D2 empty state, 49-D3 spawn-path enumeration, 49-D4 lifetime, 49-D5 session-aware predicate, 49-D6 texts (incl. zero-workspace), 49-D7 open path, 49-D8 keys, 49-D9 switcher unchanged, 49-D10 no new surface. Amendment register 49-A1–A12: **22 §11.4/22-A7 bare-cwd eager spawn superseded; 16-D2 reinstated**; 46-D3.2/3.3/3.5/I8 amended; 22 §3.1/§3.4/§3.7 and 45-D3/D4 not amended; **48-D3 conflict recorded (49-A12)**. Invariants 49-I1–I14; failure modes 49-F1–F11; tests 49-U1–U15, 49-G1–G5, 49-P1–P4. Eight open questions (incl. the 48-D3 reconciliation). Verification status: DRAFT — not yet reviewed. |
| 3 | 2026-09-21 | **Gate repair (adversarial gate FAIL: 1 HIGH + 7 MEDIUM).** (H) The lazy sequence could never connect: 49-D1 step 3 modeled the workspace via `workspace_spec_from_registry` before the daemon registered (`:1222-1224` returns `nullopt` while `row->host` is absent) and called `attach_workspace` pre-spawn, whose `connections_.count != 0` guard (`:770`) then made `ensure_worker_loop`'s post-spawn attach (`:1076`) a no-op. **Fixed:** the workspace is modeled/attached **only after** `lifecycle->ensureRunning` succeeds (`ensure_worker_loop`, `:1052`→`:1057`→`:1076`); `submit()` never attaches early; the draft rides the existing `pending_creates_` map and is consumed on `on_link_state(Attached)` (`:1334`) which sets `activeWorkspaceId` and calls `create_session` (`:1453`). (M) `find_or_register_workspace` is in cli.cpp's anonymous namespace (`:101-906`) and is unreachable from `supervisor.cpp`; replaced with a private `resolve_or_register_workspace` over `options_.registry` (`findByCanonicalPath` `registry.hpp:218-219`, `registerWorkspace` `:252-254`). (M) Removed the contradictory `pending_prompts_` (no consume site); the draft is pinned through `pending_creates_`/`create_session` (cites 46-D7/46-I13). (M) **49-A12's "48-D3.3 cwd always-win" reference was fabricated** — spec 48 has no `D3`; the real overlap is 46-D7.1/46-I13/23-D58 and spec 50-D2 (reconciled by scoping). (M) 16-D2's `NotRunning`/browsable **listing clause is amended** for the bare-cwd case (no row before the first prompt); its lazy-spawn policy is reinstated. (M) 49-D5.1 extends the unknown-membership fallback to the **active** workspace's non-focused visible leaves, fixing the false "nothing available" notice (49-I13). (M) Corrected the Ctrl+N/`/new` claim: `new_session()` no-ops with no active workspace (`:1676-1681`), so it is not a zero-workspace spawn trigger (49-D3, OQ-49-1). Cross-spec reconciliation recorded in spec 50 §14. Verification status: DRAFT — not yet reviewed. |
| 4 | 2026-09-21 | **Implementation-phase corrections (found while coding; build+suite not yet green at this revision).** (1) **49-A13/49-I15:** the shipped composer is session-scoped, so the empty screen was **not typeable** as 49-D2 claimed; added `UiModel::pendingComposer` and routed `handle_input`/`render_input` to it while `workspaces` is empty. 49-D10 amended accordingly. (2) **49-A14:** `render_conversation(nullptr, …)` renders `(no active session)`, not an empty pane; `build_ui` now renders an empty conversation for the zero-workspace state. 49-D2 corrected. (3) `run_supervisor_entry` gained the `new_session` parameter already sketched in §4, so `--new` stays an explicit eager activation (49-D3/49-I4); `--resume`/`--new` alone register+attach eagerly. (4) **49-A15:** `create_session` routed through `submit_to` so the 46-D7 canned-reply seam can drive the `session.create` terminal; production unchanged. (5) Test seams added to `SupervisorHarness`: `submit`, `register_presence`, `begin_exit`, `ensure_call_count`, `submitted_count`, `pending_creates`; new tests `tests/unit/errata49_ui_test.cpp` plus golden `UI49_G1/G2/G4/G5`, and an extra composer-typeable unit test. (6) The spawn-failure path restores the draft to `pendingComposer` (49-F1). Verification status: **DRAFT (Rev 4)** — awaiting independent review. |
