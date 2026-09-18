# 23 — Session Lifecycle Errata: Unprompted-Session Cleanup & `session prune` (RB-19)

```
Status: verified (adversarial Oracle gate, PASS — zero open HIGH/MEDIUM) · verified: 2026-09-18 · reviewer: Oracle
Revision: Rev 13 (Rev 12 retained. The Rev 12 gate confirmed the production
          design correct and reachable end-to-end and returned 0 HIGH, 0
          MEDIUM, 4 LOW — all wording/completeness. Rev 13 fixes exactly those
          four: the §12.1/23-D65 active-workspace justification is corrected
          (no harness-reachable path sets `activeWorkspaceId` without also
          setting `activeSessionId`; `run()` sets it alone but is unreachable,
          and `apply_resume_success` → `UiModel::focusSession` sets both);
          SL-U13/SL-U7 now seed a workspace whose `activeSessionId` is the
          known id so `submit()` takes the direct `prompt()` path; §12.1 pins
          the connection prerequisite (`connections_` entry) before
          `seed_workspace`/`focus_workspace`; and §3.8 states the extracted
          `on_create_reply` calls `track()` after releasing `cleanup_mutex_`.
          No creation-path, wire, protocol, or TUI-state-machine change — see
          §2.1, §3.8, §12.1, §14)
Component: 23 (errata) — amends 10-supervisor-tui.md, 05-transport.md,
           22-switcher-sessions-errata.md, 21-config-jsonc-errata.md,
           01-session.md, 19-session-rename-errata.md, 02-persistence.md,
           03-workspace-registry.md, 04-workspace-host-daemon.md,
           06-agent-loop.md, 16-daemon-ownership.md
Depends on: 01-session.md (verified), 02-persistence.md (verified),
            03-workspace-registry.md (verified), 04-workspace-host-daemon.md
            (verified), 05-transport.md (verified), 06-agent-loop.md (verified),
            10-supervisor-tui.md (verified), 16-daemon-ownership.md (verified),
            19-session-rename-errata.md (verified),
            21-config-jsonc-errata.md (verified),
            22-switcher-sessions-errata.md (verified)
Scope: (D1′) The user's single new requirement: **a session that was never
       prompted must not survive the user's exit — it is deleted from history.**
       Cleanup runs at a clean supervisor exit, is scoped to the sessions *this
       supervisor created*, uses the same unprompted predicate as prune, and is
       best-effort (`session prune --empty` is the backstop). **The session
       creation path is not changed at all**: `session.create` and the TUI's
       eager creates stay exactly as shipped.
       (D2) `ymh session prune` — the previously-verified destructive CLI, kept
       whole, scoped to `--empty` only in v1 (`--older-than` deferred).
       (D3) the subagent-kind fix — a PRE-SHIP blocker for the root-only
       cleanup/prune filter.
       (D4) legacy empties are hidden from every enumeration surface but not
       auto-deleted; the filter lives at the `/sessions` catalog consumer
       (store-only reconcile entries excepted, §6.5).
       (D5) the orphan-junction sweep, restricted to flock-held contexts and
       guarded against a freshly created store.
       (Rev 7) The Rev 2–5 draft lifetime model and the Rev 6 pivot are both
       **withdrawn**; the pre-existing lifetime/teardown defects are recorded as
       an independent hardening effort (RB-21, §12) and are not load-bearing.
       (Rev 8) Correctness amendment over Rev 7, no new behaviour: cleanup
       excludes every session this supervisor has **prompted** (a prompt can be
       accepted and queued before its `UserMessage` is durable), the TUI
       auto-create is gated on the **unfiltered registry junction set** (never on
       the D4-filtered `session.list` reply), `force` is re-justified as
       defence-in-depth (the TUI never sets `HostRuntime::active_session_`), the
       Ctrl-S cell set is reconciled against the filtered reply, and the
       created-set is **per-process** (§3.3/§3.5/§6.5).
       (Rev 9) Correctness amendment over Rev 8, no new behaviour: the whole
       reply-driven update of `refresh_sessions` (cell add, Ctrl-S reconcile and
       its `UiModel::eraseSession`, and the auto-create decision) runs only on a
       **successful** `session.list` reply (`reply.ok && reply.result.is_array()`),
       so a failed/timed-out reply removes nothing and creates nothing; both
       cleanup bookkeeping sets are recorded under one mutex and the queued-create
       path binds the returned id to the already-captured prompt in the reply
       handler, so no accepted prompt can be overtaken; and the auto-create gate
       is the **union** of the registry junction set and the reply, with the D4
       skip scoped to **junction-backed** entries so store-only entries surface
       (§3.3/§6.1/§6.5).
```

## 1. Purpose, scope, and supersession map

### 1.1 The problem

**The user's requirement (verbatim, 2026-09-18).**

> "Let session be created as is today. Change code less. Only thing I want - if
> user never entered any prompts in a session and exited. The session shall be
> cleaned up/deleted from history."

This is the whole requirement. It is deliberately smaller than every prior
revision of this spec: it does **not** ask for a creation gate, a lazy session,
a wire change, or a TUI state-machine change. It asks only that an unprompted
session not survive a clean exit, plus the repeatable `ymh session prune`
command the user asked for earlier (for legacy/history cleanup).

**A session row is created at `session.create`, never at the first prompt — and
that is retained.** `SessionManager::createSession`
(`src/session/session_manager.cpp:47-71`) calls `store_->create(header)` and
then appends `payload::SessionStarted`, both before any user input.
`SessionPersistence::create` (`src/session/session_persistence.cpp:711-750`)
runs the `INSERT INTO sessions` and the lease acquisition in one transaction.
This matches the verified spec `01-session.md` §9.1 and is **unchanged** by this
spec.

**The TUI eagerly auto-creates a session on startup and on idle input.** Three
live call sites in `SupervisorApp` (`src/ui/supervisor.cpp`) submit
`session.create` with the placeholder title `"tui"` before the user has typed a
prompt:

1. `refresh_sessions`' reply handler (`src/ui/supervisor.cpp:1209-1218`; the
   create call is at `:1215`) — when the daemon reports **zero** stored sessions
   for an attached workspace, it calls `create_session(workspace, "")` (the
   attach and spawn paths both converge here). **Rev 8 (MEDIUM-2) + Rev 9
   (MEDIUM-3):** this "zero" test must be taken from the **union** of the
   unfiltered registry junction set and the reply, never from the D4-filtered
   `session.list` reply alone, or attaching to a workspace whose only sessions
   are store-only (pre-M2 / crash-window) unprompted roots spuriously creates a
   new empty (§6.5).
2. `new_session` (`src/ui/supervisor.cpp:1422-1427`; the create call is at
   `:1425`) — the `/new` command calls `create_session(workspace, "")`.
3. `handle_input`'s first-keystroke path (`src/ui/supervisor.cpp:1900-1907`; the
   create call is at `:1905`) — when there is no active `SessionUiState` and
   `activeSessionId` is empty, the first keystroke calls
   `create_session(workspace, "")`.

A fourth, effectively **dead** path already exists in `submit()`
(`src/ui/supervisor.cpp:367-392`; the create call is at `:388`): when
`activeSessionId` is empty it calls `create_session(workspace_id, text)` and the
queued prompt is sent by `apply_create_reply` once the daemon confirms. **It is
reached only for non-empty `text`** — `submit()` returns immediately on empty
text (`:368-370`), before the create call — so it is **not** an empty-create
driver. It never fires today because the eager paths always populate
`activeSessionId` first.

**Observed damage.** The repo workspace's `sessions.db` held **21 sessions, 11
of them empty** — exactly one `session/start` event each — created by TUI
startup alone. Every empty row consumes a registry junction row, a lease row, a
session cell, and a switcher leaf; on a long-lived supervisor they accumulate
without bound. `session.create` is also available to any client, so an
unprompted row can be created outside the TUI.

**Empty is cleanly identifiable and is not the title.** An unprompted session's
own event log is exactly `[SessionStarted]`; the first accepted turn appends
`UserMessage` then `TurnStarted` (`AgentLoop::runTurn`,
`src/agent/agent_loop.cpp:502-525`). The predicate is therefore a query on the
session's **own** events for a `user/message` event. The **title is not a valid
predicate**: spec `19-session-rename-errata.md` §4.3 pins that the daemon's
advisory auto-name runs on the first `agent.prompt` **before** the agent loop
appends `UserMessage`, so an auto-renamed session can have a non-placeholder
title while still having no user message.

**The event-log predicate alone is not sufficient for cleanup (Rev 8, MEDIUM-1).**
`agent.prompt` is asynchronous at two layers: `HostRuntime::agentPrompt`
(`src/host/host_runtime.cpp:725-756`) only queues a lambda onto the `TurnExecutor`
(`turns_.submit`, `:747-754`), and `AgentLoop::send`
(`src/agent/agent_loop.cpp:100-142`) only enqueues the inbox — the `UserMessage`
is appended later, inside `AgentLoop::runTurn` (`src/agent/agent_loop.cpp:502-525`,
via `appendUserMessage`). So between an accepted prompt and the durable
`user/message` there is a window in which `isUnprompted(id)` is still `true` and
`AgentRegistry::hasPendingWork` is `false` (it is blind to the `TurnExecutor`
queue — RB-21 #3, §12). A delete in that window (`only_if_empty:true,
force:true`) would erase the row, lease, junction and events **and drop the
   queued turn**. **Cleanup therefore also consults a supervisor-side prompted-set
   (§3.3), not the event-log predicate alone.** The daemon-side `only_if_empty`
   stays the hard backstop for prompts that have already become durable.
   **Rev 9 (MEDIUM-2):** the recording point for both sets is pinned so that
   window cannot be *observed* at all: the queued prompt is captured on the UI
   thread (the thread that stores it), the `session.create` reply handler binds
   the returned id to that already-captured prompt and records
   `created_sessions_` **and** `prompted_sessions_` in the **same locked
   critical section** before it enqueues `apply_create_reply`, and both sets
   share one mutex (they are written on the pump thread and read on the UI
   thread; §3.3). **Rev 10 (MEDIUM-2, remaining ordering):** the recording point
   also covers a prompt captured **after** the reply — `cleanup_created_sessions`
   snapshots `pending_creates_` under the same mutex and skips every created id
   whose workspace has a non-empty pending entry, so the accepted prompt is not
   dropped before `apply_create_reply` dispatches it (§3.3, SL-U16).

**No list path filters empty sessions today.** `session.list`
(`HostRuntime::listSessions`, `src/host/host_runtime.cpp:450-469`), the
`/sessions` disk catalog (`read_workspace_history`,
`src/ui/session_catalog.cpp:97-153`), and `ymh list` (`session_list`,
`src/cli/session_cli.cpp:170`) all enumerate `sessions` rows directly.

**Deletion exists but is API-only.** `session.delete` (confirm-gated,
`src/transport/protocol_server.cpp:445`) → `HostRuntime::deleteSession`
(`src/host/host_runtime.cpp:666-679`) → `SessionManager::deleteSession`
(`src/session/session_manager.cpp:138`) → `SessionPersistence::erase`
(`src/session/session_persistence.cpp:770-810`). There is **no CLI or TUI
delete/prune command**. `WorkspaceRegistry::archiveSession` and the `archived`
column of `registry.db`'s `workspace_sessions` table exist but have **no
callers**.

### 1.2 What this changes, in one sentence

**Nothing about how sessions are created**: at a clean supervisor exit, the
supervisor deletes each session *it* created that is still unprompted (no
`user/message` in its own log), using the guarded `session.delete`; every list
surface hides sessions that have never been prompted (store-only reconcile
entries excepted, §6.5); and a new `ymh session prune --empty` command deletes
old/empty sessions across live or stopped registered workspaces (removing the
registry junction row too).

### 1.3 Supersession map

#### 1.3.1 Superseded / withdrawn

| ID | Prior text | Change |
|---|---|---|
| 23-S5 (Rev 6) | The entire Rev 2–5 **draft lifetime model**: `Session::draft`, `createWithEvents`-on-first-append, `MaterializedHook`, `kMaxDraftsPerWorkspace`, the draft-aware seam set, the `isDraft` guards, the draft delete branch, and §3.9 (shared ownership, the `dispose` completion barrier, `TurnExecutor` join/park, the `SessionEnded` forwarding fast-path). | **Withdrawn (Rev 7).** The draft design is not pursued. |
| 23-S6 (Rev 7) | The **Rev 6 pivot**: `agent.prompt`-creates-the-session, `SessionStore::createWithFirstEvents`, the atomic birth transaction, `AgentLoop::startPrecommittedTurn`, the wire `session.create` retirement, `TransportHost::createSession` removal, the `agent.prompt` `{session, created}` result-type change, the TUI no-session steady state / `pending_input` composer / eager-create removal, the `kProtocolVersion` bump, and every invariant/failure-mode/test that depends on them. | **WITHDRAWN (Rev 7).** The user rejected the pivot and chose the smaller requirement in §1.1. **The creation-gate work is NOT pursued.** The pivot's own hazards proved non-convergent under a **5-lens adversarial review (hyperplan)** plus **five Oracle gate rounds**: a **self-deadlock** in the completion latch combined with `whenIdle` deferral; an **unimplementable `void drain(grace)`** that cannot express the join-vs-park distinction; a **self-cycle leak** from strong `shared_ptr` captures; a **missing `EventBus` publish that killed the live stream**; and an **unmarshalled turn**. Rather than work around these, Rev 7 drops the pivot entirely. `session.create` and the TUI eager creates are **retained exactly as shipped**. |
| 23-S7 (Rev 7) | Any reading of Rev 6 that made a newly created session never unprompted (the birth prelude always carried a `UserMessage`). | **Withdrawn.** Under Rev 7 a freshly created session **is** unprompted until the user sends a prompt; that is the condition cleanup-on-exit acts on. |

#### 1.3.2 Amended

| ID | Amended clause | Change |
|---|---|---|
| 23-A1 (Rev 2) | `05-transport.md` §7.4 `session.delete params: { session, confirm: true }`; `TransportHost::deleteSession` (`include/ymh/transport/host.hpp:94`, pure virtual) | **Normative** (was a Rev 1 proposal): `session.delete` gains `only_if_empty: bool` and `force: bool` (both default `false`). The `TransportHost` seam and the `protocol_server.cpp` parser are amended; both files are in scope. **No other wire change**: no method added, none retired, no `kProtocolVersion` bump. |
| 23-A13 (Rev 6) | `02-persistence.md` §4.7; `03-workspace-registry.md` R16; prune ordering | **Dependent-children precheck before touching the junction.** Prune (both paths) and `HostRuntime::deleteSession` must call a new `hasDependents(id)` store check **before** `WorkspaceRegistry::removeSession`; a session with children is skipped (`reason: "has dependent sessions"`) with the junction untouched. (§5.3/§5.4, SL10.) |
| 23-A14 (Rev 6) | `WorkspaceHost::Impl` startup (`src/host/workspace_host.cpp:587-598`); prune stopped-path apply | **Orphan-junction sweep** restricted to flock-held contexts, **plus a fresh-store guard** (23-D56). (§7, SL23.) |
| 23-A16 (Rev 7) | `16-daemon-ownership.md` §4.3 teardown ordering (steps 1–4, `:841-852`) | **Prepend a best-effort cleanup step (step 0) before `deregisterSupervisor` and before any `host.shutdown`.** Cleanup runs while the daemon is still serving and this supervisor is still an owner; it does **not** deregister, signal, or otherwise alter the spec-16 teardown. (§3.2, SL3.) |
| 23-A17 (Rev 7) | `01-session.md` §9.1 create timing (as amended by Rev 6) | **Restored.** A session row is created at `session.create` (eager, before any prompt), exactly as shipped; `SessionStarted` is the first own event, written by `createSession`. The Rev 6 timing amendment is withdrawn. (§1.1.) |
| 23-A18 (Rev 7) | `06-agent-loop.md` §7 / `SubagentRunner` | **Subagent-kind fix (23-D53).** `SessionOptions` carries `kind`/`parentSession`; `SessionManager::createSession` honours them; `SubagentRunner` passes `kind = Subagent, parent = parent_.id()`. A behavior change (subagents flip Root→Subagent). (§4, SL7.) |
| 23-A19 (Rev 8) | `10-supervisor-tui.md` — supervisor cleanup bookkeeping | **Distinct, per-process bookkeeping.** Cleanup scope is a distinct `created_sessions_` record populated **only** by `session.create` replies (never `SupervisorConnection::track`, which is a **union** of listed + created ids), **minus** a distinct `prompted_sessions_` set populated by `prompt()`/`submit()` (including a prompt queued behind an in-flight create). A session created by a previous run or left by a crash is not covered and falls to `prune`. (§3.3; MEDIUM-1 + the created-set gap.) |
| 23-A20 (Rev 8) | `10-supervisor-tui.md` — `refresh_sessions` | **Auto-create gated on the unfiltered registry junction set** (`WorkspaceRegistry::listSessions`, `include/ymh/registry/registry.hpp:220`), **not** the D4-filtered `session.list` reply; **and** the workspace's Ctrl-S cell set is reconciled against the filtered reply (cells absent are removed, except the active session). No wire or protocol change. (§6.5; MEDIUM-2 + L1.) **Gate widened to the junction∪reply union and the reconcile gated on `reply.ok` by 23-A22/23-A24 (Rev 9).** |
| 23-A21 (Rev 8) | `04-workspace-host-daemon.md` / `10-supervisor-tui.md` — `active_session_` reachability | **Correction, not a new behaviour.** `HostRuntime::active_session_` is set only by `HostRuntime::activateSession` (`src/host/host_runtime.cpp:681-694`), whose only caller is the `session.activate` RPC (`src/transport/protocol_server.cpp:424-425`), which no production client sends. The TUI's `activate_session` (`src/ui/supervisor.cpp:1161-1170`) is supervisor-local and sets `WorkspaceModel::activeSessionId` only. `force` is therefore **defence-in-depth for external clients**, not required for the TUI eager empty. (§3.5/§5.4; MEDIUM-3.) |
| 23-A22 (Rev 9) | `10-supervisor-tui.md` — Ctrl-S cell reconciliation | **The reply-driven update runs only on a successful reply.** `refresh_sessions` adds/reconciles cells and takes the auto-create decision only when `reply.ok && reply.result.is_array()`; a failed/timed-out `session.list` reply removes nothing and creates nothing (no cell, no `SessionUiState`, no input draft is erased; no `session.create`). The same predicate that builds `sessions` gates the whole block. (§6.5, SL14; MEDIUM-1.) |
| 23-A23 (Rev 9) | `10-supervisor-tui.md` — cleanup bookkeeping recording point | **Recording point + synchronization pinned.** `created_sessions_` and `prompted_sessions_` share one mutex; for the queued-create path the reply handler binds the returned id to the already-captured prompt and records both sets in the same critical section before enqueueing `apply_create_reply`; `prompt()`/`submit()` record the direct path. No accepted prompt can be overtaken. (§3.3, SL4/SL27; MEDIUM-2.) **Extended by 23-A25 (Rev 10): cleanup also snapshots `pending_creates_` for a prompt queued after the reply but before `apply_create_reply` is drained.** |
| 23-A24 (Rev 9) | `04-workspace-host-daemon.md` — `session.list` D4 filter scope | **The D4 skip is scoped to junction-backed entries.** `HostRuntime::listSessions` skips an unprompted root only when `entry.inJunction` is true; a store-only entry (`inJunction == false`, the crash-window/pre-M2 reconcile class) surfaces so the supervisor's union gate can see it. The reply shape is unchanged (no wire change). (§6.1/§6.5, SL28; MEDIUM-3.) |
| 23-A25 (Rev 10) | `10-supervisor-tui.md` — cleanup bookkeeping snapshot | **Cleanup also snapshots `pending_creates_` (MEDIUM-2, remaining ordering).** A prompt queued **after** the `session.create` reply but **before** `apply_create_reply` is drained is captured in `pending_creates_` by `create_session`'s dedup branch; the reply handler has already recorded the id in `created_sessions_` with `prompted_sessions_` empty, so a `created_sessions_ \ prompted_sessions_` delete would drop the accepted prompt. `cleanup_created_sessions` therefore snapshots `pending_creates_` under `cleanup_mutex_` **in the same critical section** as the two sets and **skips every created id whose workspace has a non-empty pending entry**; `apply_create_reply` then sends the prompt once drained. The §3.3 invariant is corrected to state this actual condition. (§3.3/§3.8, SL4/SL27, SL-F28, SL-U16; MEDIUM-2.) |

| 23-A26 (Rev 11) | `10-supervisor-tui.md` — cleanup mutex + test/harness pins | **No new behaviour; spec-internal correctness/clarity pins.** (a) `cleanup_mutex_` is **leaf-only** (LOW-2): it guards only container operations and is never held across `submit`, the reply-cv wait, or `connection->stop()`. (b) SL-U16's empty-create driver is corrected to a real production empty-create — `handle_input` first-keystroke (`:1900-1907`), `new_session` (`:1425`), or the harness `create_session(ws,"")` seam — because `submit()` no-ops on empty text (`:368-370`) before its create (`:388`), so the Rev 10 driver never created a session and the sole falsification test was vacuous (MEDIUM-1). (c) The SL-U15/SL-U16 harness seams (`submit`, `create_session`, `run_create_reply`, `confirm_exit`, `take_delete_requests`) are pinned in §12.1 (LOW-3). (d) §3.1 names the `pending_creates_` **snapshot**, not the live map (LOW-4). No creation-path, wire, protocol, or TUI-state-machine change. (§3.1/§3.3/§3.8, §12.1, SL4, SL-U16; MEDIUM-1 + LOW-2/3/4.) |

#### 1.3.3 Retained (explicitly not changed)

- **`session.create`** persists a session row + lease + `SessionStarted` at
  create time (`SessionManager::createSession`,
  `src/session/session_manager.cpp:47-71`). Unchanged.
- **The three eager TUI creates** (`:1215`, `:1425`, `:1905`) and the dead
  `submit()` path (`:388`). Unchanged. With cleanup-on-exit they become
  self-healing (§3.6).
- **Spec 19 auto-name**: the advisory rename still fires on the first
  `agent.prompt` before `UserMessage` (`19` §4.3). Unchanged.
- **Spec 22 `/sessions`**: registered-only enumeration, failure surfacing, and
  the `live` classification (`22` SW9/SW10). Unchanged except for the added
  unprompted filter at the catalog consumer (§6).
- **Spec 16 ownership/teardown invariants**: the orphaning set, the daemon
  admission check, the last-owner watchdog. Unchanged; cleanup only prepends a
  bounded, best-effort step before them (§3.2).

### 1.4 Scope boundaries

- **In scope:** `src/session/session_manager.cpp`, `include/ymh/session/session_manager.hpp`,
  `src/session/session.cpp`, `include/ymh/session/session.hpp`,
  `src/session/session_persistence.cpp`, `include/ymh/session/session_persistence.hpp`
  (guarded delete + `isUnprompted`/`hasDependents`/`eraseWithEvent`, the
  `SessionOptions` kind/parent fields).
- **In scope:** `src/host/host_runtime.cpp`, `include/ymh/host/host_runtime.hpp`
  (`deleteSession` guards/precheck/order, `listSessions` filter).
- **In scope:** `include/ymh/transport/host.hpp` and `src/transport/protocol_server.cpp`
  (the `session.delete` params — 23-A1, normative). **No other wire change.**
- **In scope:** `src/ui/supervisor.cpp` (cleanup-on-exit bookkeeping; the
  `refresh_sessions` auto-create gate and Ctrl-S cell reconciliation, §6.5 — the
  latter calls the existing `UiModel::eraseSession`, so `src/ui/ui_model.cpp` is
  unchanged), `src/ui/session_catalog.cpp`,
  `include/ymh/ui/session_catalog.hpp` (the catalog-consumer filter). **No
  creation-path or TUI state-machine change.**
- **In scope:** `src/cli/cli.cpp`, `src/cli/session_cli.cpp`,
  `include/ymh/cli/session_cli.hpp` (`ymh session prune`).
- **In scope:** `src/agent/subagent.cpp`, `include/ymh/agent/subagent.hpp`
  (the subagent kind/parent fix — 23-A18 only). Nothing else in the agent
  lifetime model is in scope (§12).
- **In scope:** `src/registry/registry.cpp`, `include/ymh/registry/registry.hpp`,
  `src/registry/liveness.cpp`, `include/ymh/registry/liveness.hpp` — the
  dependent precheck, junction-first prune ordering, the orphan-junction sweep
  scope + fresh-store guard, and the per-mutation `flock` rule (§5.3/§5.6/§7).
- **Out of scope:** `ui::run_tui` (`src/ui/ui_application.cpp`) is dead code
  (spec 17 §1); it is not touched.
- **Out of scope:** orphan/unregistered `<dir>/.ymh/sessions.db` files — prune
  enumerates `registry.db` `workspaces` rows only (mirrors spec 22 §4.5).
- **Out of scope:** `WorkspaceRegistry::archiveSession` / the `archived` column
  (still unused); prune **deletes**, it does not archive.
- **Out of scope:** the placeholder-title header change (RB-20, §8).
- **Out of scope:** the lifetime/teardown defects recorded in §12 (RB-21); they
  are independent of this spec and must not be relied upon.
- **Out of scope:** the `XDG_STATE_HOME` test-isolation defect (spec 22 §11.2).
- `requirements_draft.txt` is untracked user notes and is not touched.

### 1.5 Terminology (pinned)

- **Unprompted session** — a durable session whose **own** event log contains no
  `user/message` event. Equivalent to `EXISTS(SELECT 1 FROM events WHERE
  session_id = :id AND type = 'user/message')` being false. Never the title.
- **Prompted session** — a durable session with at least one own `user/message`.
- **Cleanup-on-exit** — the Rev 7 D1′ behaviour: at a clean supervisor exit,
  delete the unprompted sessions **this supervisor created** (§3).
- **Created session** — a session id returned by a `session.create` reply to
  *this* supervisor's connection, recorded in the supervisor-side
  `created_sessions_` set (§3.3). Not a session merely listed or resumed, and not
  a session created by another client or by a **previous** supervisor run. **The
  record is per-process.**
- **Prompted-set** — the supervisor-side set of session ids this supervisor has
  submitted an `agent.prompt` for (including a prompt queued behind an in-flight
  `session.create`), regardless of whether the resulting `user/message` is durable
  yet (§3.3; MEDIUM-1).
- **Tracked set** — `SupervisorConnection::track`/`untrack`
  (`src/ui/supervisor_connection.cpp:61-79`). It is a **union** populated from
  both `refresh_sessions` (listed sessions, `src/ui/supervisor.cpp:1193`) and
  `create_session`'s reply (`:1252`), so it is **not** the created-set and must
  not be used for cleanup scope.
- **Guarded delete** — `SessionManager::deleteSession(id, only_if_empty)`:
  checks the unprompted predicate **before any durable write**, then erases via
  the lease-exempt single-transaction `eraseWithEvent`.
- **Administrative delete** — a delete performed by cleanup/prune (not by an
  agent), allowed to be lease-exempt and to bypass the active-session guard via
  `force`; never a mid-turn delete.
- **Junction** — a `registry.db` `workspace_sessions` row linking a workspace to
  a session id.
- **Flock-held context** — a code path that holds the workspace `flock` via a
  writable `SessionPersistence::open` (§7).

---

## 2. Amendment register

**Rev 1–6 rows are retained for history.** The Rev 2–5 rows that describe the
daemon-side draft are **withdrawn as normative text by 23-S5**; the Rev 6 rows
that implement the `agent.prompt`-creates pivot are **withdrawn as normative
text by 23-S6**. They are kept only so the revision history is auditable; do not
re-pin them. **Rev 7 rows** implement the user's actual requirement
(cleanup-on-exit + prune) and are the current normative text. **Rev 8 rows
(23-D57–23-D59)** are correctness amendments over Rev 7 (gate MEDIUM-1/2/3 plus
the created-set gap); they add no creation-path, wire, or protocol change.
**Rev 9 rows (23-D60–23-D62)** are correctness amendments over Rev 8 (gate
MEDIUM-1/2/3 plus four LOWs); they likewise add no creation-path, wire, or
protocol change. **Rev 10 row (23-D63)** is a correctness amendment over Rev 9
(gate MEDIUM-2 remaining ordering plus two LOWs); it likewise adds no
creation-path, wire, or protocol change. **Rev 11 row (23-D64)** is a
spec-internal correction over Rev 10 (gate MEDIUM-1 vacuous-test driver plus
LOW-2/3/4: leaf-only mutex, pinned harness seams, snapshot wording); it adds
**no behaviour at all** — no creation-path, wire, protocol, or TUI-state-machine
change. **Rev 12 row (23-D65)** is a spec-internal correction over Rev 11 (gate
MEDIUM-1 unreachable-driver precondition plus LOW-1/LOW-2: the §12.1 harness
inventory wording and the §3.8 `track()` preservation); it likewise adds **no
behaviour at all** — no creation-path, wire, protocol, or TUI-state-machine
change. **Rev 13 row (23-D66)** is a spec-internal correction over Rev 12 (the
gate **PASS** returned four LOW wording/completeness nits: LOW-1 the false
active-workspace justification, LOW-2 the unseeded `activeSessionId` in
SL-U13/SL-U7, LOW-3 the unpinned connection prerequisite, LOW-4 the
`track()`/`cleanup_mutex_` boundary); it likewise adds **no behaviour at all** —
no creation-path, wire, protocol, or TUI-state-machine change.

### 2.1 Current normative rows (Rev 7–Rev 13)

| ID | Amended clause | Verified code anchors | New behaviour |
|---|---|---|---|
| 23-D5 (Rev 1) | Predicate | `wire_type`/`wire_name` (`src/core/event.cpp`); `SessionPersistence::append` | `isUnprompted` own-log `user/message` query (§3.4/§5.2) |
| 23-D6 (Rev 1) | Prune surface | `session.delete`; `SessionPersistence`; `WorkspaceRegistry` | `ymh session prune`; hybrid live-RPC/stopped-disk (§5) |
| 23-D7 (Rev 1) | Backward compatibility | `SessionPersistence::list`, `read_workspace_history`, `session_list` | Legacy empties hidden + prunable; no schema change (§6) |
| 23-D8 (Rev 2, HIGH-1) | Stopped-path junction | `WorkspaceRegistry::removeSession` (`src/registry/registry.cpp:1261`); `HostRuntime::deleteSession` | The stopped path removes the `workspace_sessions` row (§5.3) |
| 23-D10 (Rev 2, MEDIUM-4) | Mid-turn guard | `AgentRegistry::hasPendingWork` (`src/agent/agent_registry.cpp:196-206`); `Agent::state()` | Guard uses `hasPendingWork` OR blocked states (§5.4) |
| 23-D11 (Rev 2, MEDIUM-5) | Guarded delete | `SessionManager::deleteSession`; `SessionPersistence::erase` | Predicate checked before `SessionEnded`; guarded `SessionManager::deleteSession(id, only_if_empty)` (§5.4) |
| 23-D13 (Rev 2, L1) | Registry open mode | `WorkspaceRegistry::open`/`openReadOnly` | Dry-run/enumeration use `openReadOnly` (§5.6) |
| 23-D14 (Rev 2, L2) | Selection guard | `PruneOptions` | `--keep` alone is invalid; an inclusion flag (`--empty` in v1) is required (§5.1) |
| 23-D15 (Rev 2, L3) | Stopped-path `SessionEnded` | `SessionManager::deleteSession` | Stopped prune uses the manager delete (`eraseWithEvent`: `SessionEnded` + erase) (§5.3) |
| 23-D16 (Rev 2, L4) | Post-prompt title | `UiModel::apply` `SessionTitleChanged` (`src/ui/ui_model.cpp`); `ui_event_adapter.cpp` | Title updates from the streamed `SessionRenamed`; no `refresh_sessions` dependency (§6.4) |
| 23-D19 (Rev 3, H3) | Lease-exempt guarded delete | `Session::appendEventLocked`/`isLeaseHolder` (`src/session/session.cpp`); `SessionPersistence::erase` | New `SessionStore::eraseWithEvent(id, event)` (single transaction, lease-exempt) used by both delete paths (SL11) |
| 23-D20 (Rev 3, M1) | Active-session reset | `HostRuntime::deleteSession`; `suspendSession` (`src/host/host_runtime.cpp:696-707`) | A delete that removes `active_session_` clears it (SL13) |
| 23-D30 (Rev 5, H-4) | Prune junction ordering + orphan sweep | `WorkspaceRegistry::removeSession` (`src/registry/registry.cpp:1261-1282`); `removeWorkspace` (`:1017-1043`); `orderSessions` (`:609-644`) | `removeSession` runs **before** the store erase on both paths, **after** the dependent precheck; an orphan-junction sweep repairs pre-existing leaks in flock-held contexts (§5.3/§7, SL10/SL23) |
| 23-D31 (Rev 5, M-1) | Shared-helper filter | `read_workspace_history` (`src/ui/session_catalog.cpp:97-153`, declared `include/ymh/ui/session_catalog.hpp:70`); `registry_catalog_source` (`:155-165`); `resolve_session_workspace` (`src/cli/cli.cpp:369-404`, called at `:380`) | The unprompted filter moves to the catalog consumer; the shared helper stays unfiltered (`include_unprompted` defaults true) so `ymh --resume <hidden-id>` resolves (§6.2, SL15) |
| 23-D39 (Rev 5, L-1) | Delete PTY close | `HostRuntime::deleteSession` (`src/host/host_runtime.cpp:666-679`) | The PTY close is a delete step, **after all guards** (§5.4, SL9) |
| 23-D40 (Rev 5, L-2) | Dry-run filesystem effects | `probeWorkspaceLock` (`src/registry/liveness.cpp:16-53`); `SessionPersistence::openReadOnly` (`src/session/session_persistence.cpp:705`) | The dry run skips liveness probing; "no mutation" means no logical mutation (SL24) |
| 23-D41 (Rev 5, L-3) | Inclusion kind scope | `PruneOptions`; `SessionPersistence::list` | Every inclusion flag selects `kind == 'root'` only (§5.2, SL18) |
| 23-D46 (Rev 6) | Dependent precheck | `SessionPersistence::erase` (`src/session/session_persistence.cpp:770-810`); `WorkspaceRegistry::removeSession` (`src/registry/registry.cpp:1261-1282`) | New `hasDependents(id)`/`canErase(id)` check **before any side effect**; a dependent session is skipped with the junction and PTY untouched (§5.4, SL10) |
| 23-D47 (Rev 6) | Sweep scope | `WorkspaceHost::Impl` startup (`src/host/workspace_host.cpp:587-598`); prune stopped-path apply | The orphan sweep runs only between `claimHost` and `setState(Serving)` and in the stopped-path apply; never on the live RPC path (§7, SL23) |
| **23-D50 (Rev 7)** | **Cleanup-on-exit** | `SupervisorApp::confirm_exit` (`src/ui/supervisor.cpp:595-609`); `create_session` reply (`:1244-1258`); `apply_create_reply` (`:1265-1299`) | At a clean exit, before spec-16 deregister/teardown, delete each unprompted root session this supervisor created (§3, SL2/SL3) |
| **23-D51 (Rev 7)** | **Cleanup scope** | `SupervisorConnection::track` (`src/ui/supervisor_connection.cpp:61-79`); `SupervisorApp::create_session` (`src/ui/supervisor.cpp:1225-1259`) | Cleanup targets only ids returned by this supervisor's `session.create` replies, **minus the ids it prompted** (23-D57); never a listed/resumed/foreign session, never a prompted session, never a fork/subagent/dependent (§3.3, SL4/SL27) |
| **23-D52 (Rev 7)** | **Creation path frozen** | `SessionManager::createSession` (`src/session/session_manager.cpp:47-71`); `HostRuntime::createSession`; `SupervisorApp` eager creates (`:1215`,`:1425`,`:1905`) | `session.create` and the eager creates are unchanged; no wire retirement, no protocol bump, no TUI no-session state (§3.6, SL1) |
| **23-D53 (Rev 7)** | **Subagent kind (pre-ship)** | `SessionManager::createSession` (`src/session/session_manager.cpp:58`); `SessionOptions` (`include/ymh/session/session_manager.hpp:28-33`); `SubagentRunner::run` (`src/agent/subagent.cpp:18`); schema CHECK (`src/session/session_persistence.cpp:49-50`) | `SessionOptions` gains `kind`/`parentSession`; `createSession` honours them; `SubagentRunner` passes `Subagent`/parent (§4, SL7) |
| **23-D54 (Rev 7)** | **`--older-than` deferred** | `PruneOptions` | v1 accepts `--empty` only; `--older-than` is not implemented (unknown flag → exit 2). Deferred because it selects every root regardless of prompt and so is exposed to the subagent-kind hazard (23-D53) (§5.8, §13.2) |
| **23-D55 (Rev 7)** | **Guard-order fix** | `HostRuntime::deleteSession` (`src/host/host_runtime.cpp:666-679`) | **All guards precede all side effects** (today PTY close and agent dispose run before any dependent guard). Pinned order: mid-turn → active → only_if_empty → hasDependents → PTY close → agent dispose → junction → erase → active reset (§5.4, SL9) |
| **23-D56 (Rev 7)** | **Fresh-store sweep guard** | `SessionPersistence::open` (`src/session/session_persistence.cpp:640-641` uses `SQLITE_OPEN_CREATE`) | The orphan-junction sweep runs only if the `sessions.db` **pre-existed** the writable open; a fresh/missing DB would otherwise classify every junction as an orphan (§7, SL23) |
| **23-D57 (Rev 8)** | **Cleanup bookkeeping (MEDIUM-1 + created-set gap)** | `SupervisorConnection::track` (`src/ui/supervisor_connection.cpp:61-79`); `SupervisorApp::create_session` reply (`src/ui/supervisor.cpp:1244-1258`); `prompt()` (`:1301-1304`); `submit()` (`:367-392`) | Cleanup targets `created_sessions_ \ prompted_sessions_`: a **distinct per-process** `created_sessions_` populated only by `session.create` replies, minus a distinct `prompted_sessions_` set populated whenever this supervisor submits `agent.prompt` (including the prompt queued behind an in-flight create). `track()` is a union and is **not** used. A session from a previous run/crash is not covered; `prune` is the backstop (§3.3, SL2/SL4/SL27) |
| **23-D58 (Rev 8)** | **Refresh auto-create + cell reconciliation (MEDIUM-2 + L1)** | `SupervisorApp::refresh_sessions` (`src/ui/supervisor.cpp:1172-1220`); `WorkspaceRegistry::listSessions` (`include/ymh/registry/registry.hpp:220`); `UiModel::eraseSession` (`src/ui/ui_model.cpp:541-556`) | The "no sessions → `create_session`" decision is taken from the **unfiltered registry junction set**, not the D4-filtered `session.list` reply; the Ctrl-S cell set is reconciled against the filtered reply (absent cells removed, active exempt). No wire change (§6.5, SL14/SL28) |
| **23-D59 (Rev 8)** | **`active_session_` reachability / `force` rationale (MEDIUM-3)** | `HostRuntime::activateSession` (`src/host/host_runtime.cpp:681-694`); `protocol_server.cpp:424-425` (`session.activate`); `SupervisorApp::activate_session` (`src/ui/supervisor.cpp:1161-1170`) | `HostRuntime::active_session_` is set only via the `session.activate` RPC (no production client sends it); the TUI never sets it. Cleanup's `force:true` is **defence-in-depth for external clients**, not required for the TUI eager empty (§3.5/§5.4, SL5/SL22) |
| **23-D60 (Rev 9)** | **Reconcile gated on a successful reply (MEDIUM-1)** | `SupervisorApp::refresh_sessions` (`src/ui/supervisor.cpp:1179-1220`); `UiModel::eraseSession` (`src/ui/ui_model.cpp:541-556`) | The whole reply-driven update (cell add, Ctrl-S reconcile/erase, auto-create) runs **only** when `reply.ok && reply.result.is_array()` (the predicate that builds `sessions`, `:1181`). A failed/timed-out reply removes no cell and no `SessionUiState`/input draft, and creates no session (§6.5, SL14) |
| **23-D61 (Rev 9)** | **Cleanup bookkeeping recording point + mutex (MEDIUM-2)** | `SupervisorApp::create_session` reply (`src/ui/supervisor.cpp:1244-1258`); `prompt()` (`:1301-1304`); `submit()` (`:367-392`); `SupervisorConnection::process_requests` (the reply callback is invoked on the pump thread at `src/ui/supervisor_connection.cpp:362`; `process_requests` is defined at `:339` and called from `pump()` at `:443`) | `created_sessions_` and `prompted_sessions_` share one mutex; the queued prompt is captured on the UI thread, and the reply handler binds the returned id to it and records both sets in the same critical section before `enqueue(apply_create_reply)`. No window where created is recorded but the accepted prompt is not; no unsynchronized cross-thread access (§3.3, SL4/SL27). Extended by 23-D63 (Rev 10) for a prompt queued after the reply |
| **23-D62 (Rev 9)** | **Union auto-create gate + D4 filter scope (MEDIUM-3)** | `HostRuntime::listSessions` (`src/host/host_runtime.cpp:450-469`); `WorkspaceRegistry::listSessions` (`src/registry/registry.cpp:853-865`); `orderSessions` (`:609-644`); `registry.hpp:91-98` | The gate is `!registry->listSessions(ws).empty() \|\| !sessions.empty()`; the daemon's D4 skip is scoped to `entry.inJunction`, so store-only entries (crash window / pre-M2) surface. No wire change (§6.1/§6.5, SL28) |
| **23-D63 (Rev 10)** | **Cleanup snapshots `pending_creates_` (MEDIUM-2, remaining ordering)** | `SupervisorApp::create_session` (`src/ui/supervisor.cpp:1225-1258`, dedup branch `:1230-1236`); `SupervisorConnection::process_requests` reply (`src/ui/supervisor_connection.cpp:362`); `apply_create_reply` (`:1265-1299`, reads/erases `pending_creates_` `:1268-1272`, calls `prompt()` `:1293-1295`); `drain()` (`:1326-1334`, reached from `Event::Custom` at `:2115-2116`) | `cleanup_created_sessions` snapshots `pending_creates_` in the same `cleanup_mutex_` critical section as `created_sessions_`/`prompted_sessions_` and **skips every created id whose workspace has a non-empty pending entry**. This closes the ordering where the prompt is captured **after** the create reply (so the reply handler saw an empty pending map and did not record `prompted_sessions_`) but before `apply_create_reply` is drained. The §3.3 invariant is corrected to this actual condition; no creation-path/wire/protocol change (§3.3/§3.8, SL4/SL27, SL-F28, SL-U16) |
| **23-D64 (Rev 11)** | **Cleanup mutex leaf-only + test-driver/harness pins (MEDIUM-1, LOW-2/3/4)** | `SupervisorApp::submit` (`src/ui/supervisor.cpp:367-392`, empty guard `:368-370`, create `:388`); `handle_input` (`:1900-1907`, create `:1905`); `new_session` (`:1425`); `create_session` (`:1225-1259`); reply handler (`:1244-1258`); `confirm_exit` (`:595-609`); `SupervisorHarness` (`include/ymh/ui/supervisor_harness.hpp:34-92`; impl `src/ui/supervisor.cpp:2285-2417`) | **No new behaviour.** (a) `cleanup_mutex_` is **leaf-only**: container operations only; never held across `submit`, the reply-cv wait, or `connection->stop()` (LOW-2). (b) SL-U16's empty-create driver is a real production empty-create — first-keystroke `handle_input` (`:1905`), `new_session` (`:1425`), or harness `create_session(ws,"")` — because `submit()` no-ops on empty text (`:368-370`) before `:388`, so the Rev 10 driver never created and the test was vacuous (MEDIUM-1). (c) The harness gains `submit`, `create_session`, `run_create_reply`, `confirm_exit`, `take_delete_requests`, pinned in §12.1 (LOW-3). (d) §3.1 reads the `pending_creates_` snapshot, not the live map (LOW-4). No creation-path/wire/protocol/TUI-state-machine change (§3.1/§3.3/§3.8, §12.1, SL4, SL-U16) |
| **23-D65 (Rev 12)** | **Active-workspace test prerequisite + harness-inventory/`track()` wording (MEDIUM-1, LOW-1/LOW-2)** | `UiModel::focusWorkspace` (`include/ymh/ui/ui_model.hpp:474`; `src/ui/ui_model.cpp:885-891`); `SupervisorApp::run` (`src/ui/supervisor.cpp:343`); `submit()` (`:367-392`, `activeWorkspace()` `:371`); `handle_input` (`:1900-1907`, `activeWorkspace()` `:1903`, create `:1905`); `seed_workspace` (`:2349-2351`); `SupervisorHarnessImpl` ctor (`:2287-2291`); reply handler `track()` (`:1249-1253`); `SupervisorConnection::track` (`src/ui/supervisor_connection.cpp:61-70`) | **No new behaviour.** (a) §12.1 pins a `focus_workspace(workspace)` seam invoking `UiModel::focusWorkspace`, and every test that drives `submit()`/`dispatch_key` (SL-U7's durable-prompt step, SL-U13, SL-U15, SL-U16) must `seed_workspace(ws)` then `focus_workspace(ws)` first: no harness-reachable path sets `activeWorkspaceId` without also setting `activeSessionId` (`run()` `:343` sets it alone but the harness never calls it; harness-exposed `apply_resume_success` `:1005-1016` → `UiModel::focusSession` sets both), so without the seam `model_.activeWorkspace()` is null and the driver no-ops (the Rev 11 gate, MEDIUM-1; premise corrected Rev 13, LOW-1). (b) §12.1's harness inventory is reworded to "of the seams these tests need, only …" (LOW-1). (c) §3.8 step 1 states the `on_create_reply` extraction preserves the shipped `connection->track(SessionId{session})` call (`:1249-1253`) verbatim before `enqueue` (LOW-2). No creation-path/wire/protocol/TUI-state-machine change (§3.8, §12.1) |
| **23-D66 (Rev 13)** | **Gate-PASS LOW wording/completeness (LOW-1/LOW-2/LOW-3/LOW-4)** | `UiModel::focusSession` (`src/ui/ui_model.cpp:893-908`, `activeWorkspaceId` `:903`); `SupervisorApp::apply_resume_success` (`:1005-1016`); `create_session` early-return (`:1226-1229`); `submit()` (`:367-392`, create `:388`, prompt `:391`); `SupervisorHarness` (`include/ymh/ui/supervisor_harness.hpp:34-92`, `ensure_workspace_running`/`on_scan` `:39-40`); §3.8 step 1 `track()` (`:1249-1253`) | **No new behaviour.** (a) The 23-D65/§12.1 premise "`activeWorkspaceId` is set only in `run()`" is corrected: no harness-reachable path sets `activeWorkspaceId` **without also setting `activeSessionId`** — `apply_resume_success` (harness-exposed) → `focusSession` sets both, while `run()` sets it alone but is unreachable (LOW-1). (b) SL-U13/SL-U7 seed a workspace whose `activeSessionId` is the known id, so `submit()` takes the direct `prompt()` path (`:391`) and actually records `prompted_sessions_`, not the create path (`:388`) (LOW-2). (c) §12.1 pins the connection prerequisite: the workspace's `connections_` entry must exist before `seed_workspace`/`focus_workspace`, else `create_session` early-returns (`:1226-1229`) and SL-U13/SL-U15/SL-U16 are vacuous (LOW-3). (d) §3.8 step 1 states the extracted `on_create_reply` performs its container inserts under `cleanup_mutex_`, releases it, then calls `connection->track(...)` (which takes `SupervisorConnection::mutex_`) and `enqueue` (leaf-only rule) (LOW-4). No creation-path/wire/protocol/TUI-state-machine change (§3.8, §12.1) |
| 23-P1 (Rev 2) | `session.delete` params | `TransportHost::deleteSession`; `protocol_server.cpp` | **Normative**: `only_if_empty` + `force`; seam + parser amended (§5.4/§5.7) |

### 2.2 Withdrawn rows (history only)

- **Rev 2–5 draft rows:** `23-D1`–`23-D4`, `23-D12`, `23-D18`, `23-D21`–`23-D23`,
  `23-D24`'s forwarding fast-path, `23-D26`–`23-D29`, `23-D32`–`23-D36`,
  `23-D38`, `23-D42`, and the `23-A3`/`23-A4`/`23-A7`-draft clauses. Withdrawn by
  23-S5.
- **Rev 6 pivot rows:** `23-D43` (create-path wire contract), `23-D44`
  (synchronous birth), `23-D45` (TUI no-session state), `23-D48` (hardening
  separation — kept as an RB-21 pointer, see §12), `23-D49` (pre-committed first
  turn), and `23-A11`/`23-A12`/`23-A15`. Withdrawn by 23-S6.

---

## 3. D1′ — Unprompted-session cleanup on exit

### 3.1 Decision (pinned)

**At a clean supervisor exit, the supervisor deletes every session it created
that is still unprompted.** This is the whole D1′ behaviour. It does not create,
recreate, gate, or defer any session; it only removes the empties the user's own
TUI left behind when they quit without typing a prompt.

```text
cleanup-on-exit(S):
  for each attached workspace W of S:
    # Rev 10 (MEDIUM-2): a non-empty pending_creates_[W] is an accepted prompt
    # whose apply_create_reply has not yet run; skip W's created ids so it is
    # never deleted before its prompt is dispatched.
    # Rev 11 (LOW-4): this reads the §3.3/§3.8 *snapshot* taken under
    # cleanup_mutex_, never the live map (which is written on the pump thread).
    if the snapshot's pending_creates_[W] is empty:               # §3.3, Rev 10/11
      for each id in (created_sessions_[W] \ prompted_sessions_):
        # daemon-authoritative; best-effort, bounded by cleanup_grace
        session.delete { session: id, confirm: true,
                         only_if_empty: true, force: true }
```

**Claim (soft).** After a **clean** exit with the daemon reachable, no
unprompted root session created by that supervisor survives in any of its
workspaces. It is **best-effort, not a guarantee**: a crash, a lost clean-exit
path, or an unreachable daemon leaves the row, and `ymh session prune --empty`
is the backstop (§3.7).

### 3.2 WHEN: at clean exit, before spec-16 teardown

`SupervisorApp::confirm_exit` (`src/ui/supervisor.cpp:595-609`) is the single
clean-exit path (reached by the confirm key and by `--yes`). Its current pinned
order is: `presence_->deregister()` (`:600-603`) → `teardown_daemons()`
(`:604`, spec 16 §4.3 steps 2–3) → `quit_`/screen exit.

**Pinned (23-A16): cleanup is a new step 0, before deregister and before any
`host.shutdown`.**

```text
confirm_exit(orphaning):
  0. cleanup_created_sessions()      # NEW (23-D50); bounded by cleanup_grace
  1. presence_->deregister()         # spec 16 §4.3 step 1 (unchanged)
  2. teardown_daemons(orphaning)     # spec 16 §4.3 steps 2-3 (unchanged)
  3. quit_ / screen exit
```

- **Why before deregister.** Spec 16 makes zero *live owners* a daemon shutdown
  trigger (§5.1) and the last-owner exit an explicit shutdown (§4). If cleanup
  ran after deregister, the daemon could begin tearing down (or the owner
  watchdog could fire) before the delete RPCs arrive. Running first keeps the
  daemon serving and this supervisor an owner.
- **Bounded.** Cleanup is bounded by a new `cleanup_grace` (default **2 s**,
  independent of `teardown_grace`); on expiry the supervisor proceeds to
  deregister/teardown. It never blocks the exit indefinitely.
- **No change to spec 16.** Cleanup does not deregister, signal, or admit-check;
  it only issues `session.delete` on the existing connections. The spec-16
  orphaning set, admission check, and watchdog are untouched (SL3).
- **Shared daemon (another live supervisor).** Cleanup runs for **every**
  attached workspace, whether or not it is in the orphaning set, because it only
  touches sessions this supervisor created. If the daemon is shared, it is not
  torn down, and the other supervisor is unaffected (SL4). If this supervisor is
  the last owner, spec-16 teardown follows cleanup as usual.
- **Daemon already gone / unreachable.** The delete RPC fails; cleanup is a
  no-op for that workspace; the supervisor proceeds (SL6).

### 3.3 WHICH: only the sessions *this supervisor created*

**Chosen: the sessions this supervisor created and that are still unprompted.**
Not "every unprompted session in its workspaces".

**How the set is identified (Rev 10, 23-D57/23-D61/23-D63).** `SupervisorApp::create_session`
(`src/ui/supervisor.cpp:1225-1259`) is the only creator on the supervisor side.
Its reply handler (`:1244-1258`) already has the returned id. Cleanup uses three
**distinct** supervisor-local records, all owned by `SupervisorApp` and all
guarded by a single `cleanup_mutex_`:

1. `created_sessions_` — e.g. `std::map<WorkspaceId, std::set<SessionId>>`,
   populated **only** in `create_session`'s reply handler (and carried into
   `apply_create_reply`, `:1265-1299`) when the reply carries a non-empty id.
   `SupervisorConnection::track`/`untrack`
   (`src/ui/supervisor_connection.cpp:61-79`) is **not** the source: `track()` is
   also called for every listed session (`src/ui/supervisor.cpp:1193`) and for
   the created id (`:1252`), so it is a **union** of listed + created ids and
   cannot answer "did *this* supervisor create it".
2. `prompted_sessions_` — e.g. `std::set<SessionId>`, recorded under the same
   mutex at the **earliest point the id is knowable**:
   - **Direct path:** `submit()` with a known `activeSessionId` calls `prompt()`
     (`src/ui/supervisor.cpp:1301-1304`); the id is recorded there, at
     submission time, **before** any `user/message` can be durable.
   - **Queued-create path:** `submit()` with an empty `activeSessionId` calls
     `create_session(workspace, text)` (`:388`), which stores the text in
     `pending_creates_` (`:1230-1237`) — the UI thread captures the queued prompt
     there. This is reached only for **non-empty** `text`; `submit()` returns at
     `:368-370` on empty text, so an empty-create driver must instead be the
     eager paths (`:1215`/`:1425`/`:1905`) or the harness `create_session` seam
     (§12.1, SL-U16). When the `session.create` reply arrives on the connection pump thread
     (`SupervisorConnection::process_requests` invokes the reply callback on the
     pump thread at `src/ui/supervisor_connection.cpp:362`; `process_requests` is
     defined at `:339` and called from `pump()` at `:443`), the handler reads the
     pending prompt under `cleanup_mutex_` and, in the **same critical section**
     that inserts the returned id into `created_sessions_`, inserts it into
     `prompted_sessions_` **before** it `enqueue`s `apply_create_reply`
     (`:1255`). `apply_create_reply` then sends the prompt (`:1293-1295`); that
     later record is idempotent. All `pending_creates_` access sites —
     `create_session` (`:1230-1237`), the workspace-eviction path (`:1095`), this
     reply handler, and `apply_create_reply` (`:1268-1272`) — take
     `cleanup_mutex_`, so the pump-thread read is synchronized with the UI-thread
     write/erase.
3. `pending_creates_` — the **existing** `std::map<WorkspaceId, std::string>`
   queued-prompt record, also guarded by `cleanup_mutex_`. Its entry for a
   workspace is non-empty exactly while an accepted prompt is waiting for
   `apply_create_reply` to dispatch it. Cleanup snapshots it (§3.8 step 3) and
   skips any created id whose workspace has a non-empty entry.

   **The pin (Rev 10, corrected).** Cleanup snapshots all three records in one
   `cleanup_mutex_` critical section and targets
   `created_sessions_ \ prompted_sessions_`, **excluding every id whose workspace
   has a non-empty `pending_creates_` entry**. The actual guaranteed condition is:
   there is **no observable state** in which cleanup issues a `session.delete` for
   a created id while an accepted-but-undurable prompt for that session is not
   already excluded — either the id is in `prompted_sessions_`, or its workspace
   has a non-empty `pending_creates_` entry. Two orderings cover the
   queued-create path:
   - **prompt captured before the reply** — the reply handler reads the pending
     prompt under the mutex and records both `created_sessions_` and
     `prompted_sessions_` in one critical section before `enqueue`ing
     `apply_create_reply` (SL-U15);
   - **prompt captured after the reply but before `apply_create_reply` is
     drained** — the reply handler saw an empty `pending_creates_` and recorded
     only `created_sessions_`; `create_session`'s dedup branch then stores the
     prompt in `pending_creates_`, so cleanup's snapshot sees the non-empty entry
     and skips the workspace's created ids (SL-U16).

   If the reply has not yet arrived, `created_sessions_` does not contain the id,
   so cleanup cannot target it (the row survives, and `prune` is the backstop).
   The mutex also removes the container data race that Rev 8's wording created by
   writing `created_sessions_` on the pump thread and reading it on the UI thread
   with no lock.

   **The mutex is leaf-only (Rev 11, LOW-2; pinned).** `cleanup_mutex_` guards
   **only** the three container operations (insert/find/erase on
   `created_sessions_`/`prompted_sessions_`/`pending_creates_`, and the snapshot
   copy) and is **never** held across `submit`, the reply condition-variable
   wait, or `connection->stop()`. Cleanup copies its snapshot under the lock,
   releases the lock, and only then issues the bounded `session.delete`
   requests; the reply handler records under the lock and releases it before
   `enqueue`. This ordering rule is what keeps a leaf mutex from becoming a
   lock-order edge against the connection pump.

Cleanup targets `created_sessions_ \ prompted_sessions_`, excluding workspaces
with a non-empty `pending_creates_`, snapshotting all three records under
`cleanup_mutex_`; `only_if_empty` is still sent and remains the
daemon-authoritative backstop.

**The created-set is per-process (23-D57).** `created_sessions_` is in-memory
supervisor state, so it covers **only** sessions created by *this* supervisor
run. An unprompted session created by a **previous** supervisor run, or left
behind by a crash, is **not** in the set and is never targeted by cleanup; it
falls to `ymh session prune --empty` as the backstop (§3.7, SL6/SL27). This is
the intended safety boundary: the spec does not attempt cross-process
attribution.

**Why this scope (justification).**

1. **It satisfies the safety constraints by construction — with one recorded
   residual.** The requirement forbids deleting (a) a session another live client
   is using and (b) a prompted session. A session created by another client is
   not in `created_sessions_`, so restricting the target set removes that class.
   `prompted_sessions_` removes the **just-submitted** (not-yet-durable) prompted
   class, and `only_if_empty` (§3.5) removes the durable prompted class.
   **Residual (Rev 9, LOW):** "another client's" is *not* fully removed when a
   **second supervisor** `--resume`s (via `session.resume`, not
   `session.activate`) an empty **this** supervisor created and prompts it: the
   prompted-set is per-supervisor, so this supervisor's cleanup can still delete
   it before the foreign prompt becomes durable. This is a narrow
   cross-supervisor race (it requires that session to be resumed and prompted
   within the first supervisor's exit window); it is recorded in §13.2#2 and,
   once the foreign prompt is durable, `only_if_empty` refuses. Skipping recently
   `session.activate`d ids would not close it — `--resume` sends `session.resume`
   (`src/ui/supervisor.cpp:977-978`) and no production client sends
   `session.activate` (23-D59).
2. **It matches the user's requirement exactly.** The empties are TUI
   eager-create artifacts: `refresh_sessions` (`:1215`), `/new` (`:1425`), and
   the first keystroke (`:1905`). All three flow through `create_session`, so all
   three are recorded.
3. **It preserves D4.** Legacy empties in pre-existing `sessions.db` files were
   not created by this supervisor, so cleanup never touches them; D4's
   "not auto-deleted" rule stays true, the `/sessions` catalog and `ymh list`
   still hide them, and `prune --empty` is their cleanup path (§6). (A store-only
   legacy empty surfaces in `session.list` for the reconcile gate only, §6.5.)
4. **It is safe on a shared daemon for foreign-*created* sessions.** A foreign
   client's empty session is not in `created_sessions_`, so a supervisor exiting
   next to another live client cannot delete it. (A foreign client that resumes
   and prompts one of *this* supervisor's empties is the residual in item 1 and
   §13.2#2.)

**Rejected alternative: "every unprompted root in the workspace."** It needs no
bookkeeping, but it (i) would delete another client's unprompted session and
(ii) would auto-delete legacy empties, contradicting D4. Rejected.

### 3.4 The predicate

The cleanup predicate is the same one prune uses (§5.2): the session's **own**
log contains no `user/message`.

```sql
NOT EXISTS (SELECT 1 FROM events WHERE session_id = :id AND type = 'user/message')
```

The stored type is `wire_name(EventType::UserMessage) == "user/message"`
(`src/core/event.cpp`; `SessionPersistence::append` binds `wire_type(event)`).
The query is over **own** events, never the resolved view, so a fork's parent
prefix cannot mark the fork as prompted. The title is never consulted (§1.1,
SL8).

The predicate is evaluated **daemon-side at delete time** by
`HostRuntime::deleteSession` and again by `SessionManager::deleteSession` — not
from a supervisor snapshot — so a session whose prompt has **already become
durable** is kept (SL5). It does **not** cover the window before the prompt
becomes durable (MEDIUM-1, §1.1): that class is excluded supervisor-side by
`prompted_sessions_` (§3.3), because `only_if_empty` would still see the session
as unprompted and delete it (SL27, SL-F25).

### 3.5 The delete call

Cleanup issues the existing `session.delete` with the D2 additive params:

```json
{ "session": "<id>", "confirm": true, "only_if_empty": true, "force": true }
```

- **`only_if_empty: true`** — the daemon checks `store().isUnprompted(id)`
  **before any durable write**; a prompted session is refused with
  `InvalidParams` and nothing is written (no `SessionEnded`, no partial erase).
  This is the hard protection against deleting a prompted session (SL5).
- **`force: true` (Rev 8 correction, MEDIUM-3; 23-D59).** The earlier claim that
  "the eagerly created empty is normally the daemon's `active_session_`" is
  **false**. `apply_create_reply` calls the **supervisor-local** `activate_session`
  (`src/ui/supervisor.cpp:1161-1170`), which sets only
  `WorkspaceModel::activeSessionId` and sends no RPC. `HostRuntime::active_session_`
  is set **only** by `HostRuntime::activateSession`
  (`src/host/host_runtime.cpp:681-694`, `:692`), whose only caller is the
  `session.activate` RPC (`src/transport/protocol_server.cpp:424-425`); **no
  production client sends `session.activate`** (only `protocol.cpp`,
  `protocol_server.cpp`, and tests reference it). So in the TUI/headless flows
  `active_session_` is `nullopt` and the active guard never fires.
  **Chosen resolution (b): keep the guard, repurpose `force` as defence-in-depth.**
  `force:true` is retained for external/automation clients that *do* send
  `session.activate`; it bypasses **only** the active-session guard and never the
  mid-turn guard, `only_if_empty`, or `hasDependents`. Dropping it would not
  break the TUI path, but keeping it costs nothing and keeps cleanup correct for a
  client-activated empty. SL-U7 explicitly sends `session.activate` so the
  guard/`force` pair is genuinely exercised, not dead-tested.
- **Prompted-set exclusion (Rev 8, MEDIUM-1).** Cleanup never issues a delete for
  an id in `prompted_sessions_` (§3.3); this is the guard for a prompt that was
  accepted but is not yet durable, which `only_if_empty` cannot see.
- **Root-only.** The recorded ids come from `session.create`, which creates
  roots; the delete additionally enforces `kind == Root` and the
  `hasDependents` precheck, so a fork/subagent/dependent is never erased
  (SL4, §4).
- **Confirm gate.** `confirm: true` is unchanged; cleanup is a trusted in-process
  client.
- **Alternative considered — `session.suspend` then delete with `force: false`.**
  It would avoid `force` but costs an extra round-trip per session, still cannot
  make the two calls atomic, and does not improve the safety story (the scope in
  §3.3 plus `only_if_empty` already exclude foreign/prompted sessions). The
  simpler single delete with `force` is pinned.

### 3.6 The creation path is unchanged

**No creation-path change of any kind is pinned.** Specifically:

- `session.create` persists the row + lease + `SessionStarted` exactly as
  shipped (`SessionManager::createSession`, `src/session/session_manager.cpp:47-71`).
- The three eager TUI creates (`:1215`, `:1425`, `:1905`) and the dead
  `submit()` path (`:388`) are retained. They are what creates the empties, and
  cleanup-on-exit makes them **self-healing**: the user does not have to change
  how they start a session for the empties to disappear on exit.
- `agent.prompt` is unchanged: it requires a non-empty known `session`, exactly
  as today.
- No wire retirement, no `kProtocolVersion` bump, no TUI no-session state, no
  `pending_input` composer, no `startPrecommittedTurn`, no
  `createWithFirstEvents` (SL1).

The only code added to the creation path is **bookkeeping**: recording the id a
`session.create` reply already returns (and the ids this supervisor prompts), so
cleanup knows what this supervisor created and what it prompted. The creation
mechanism, timing, and wire shape are untouched.

### 3.7 Failure handling (best-effort)

- **Cleanup delete fails** (RPC error, daemon gone, timeout): the row survives.
  `ymh session prune --empty` reclaims it (§5). Not fatal to the exit.
- **Supervisor crashes without a clean exit**: `confirm_exit` never runs, so
  cleanup never runs; the row survives. Prune is the backstop.
- **A session is prompted (durably) between create and exit**: `only_if_empty`
  refuses; the session is kept. The refusal writes nothing (SL5).
- **A prompt is accepted but not yet durable at exit** (queued on the
  `TurnExecutor`, `UserMessage` not yet appended, or captured in
  `pending_creates_` and not yet dispatched by `apply_create_reply`):
  `only_if_empty` cannot see it; cleanup excludes the id via `prompted_sessions_`
  (direct path / prompt-before-reply) or via a non-empty `pending_creates_`
  snapshot (prompt-after-reply), so the queued turn is not dropped (SL27,
  SL-F25/SL-F28).
- **A session gains a dependent (fork) before exit**: `hasDependents` refuses;
  the junction and PTY are untouched; the parent survives (SL10).
- **Cleanup is a no-op on an already-deleted or unknown id**: reported and
  ignored; not an error.

**The requirement is best-effort, not a guarantee.** The spec does not claim
that no unprompted session can ever survive; it claims the normal clean-exit
path removes the supervisor's own empties, and prune covers everything else.

### 3.8 C++ interface sketch (D1′, Rev 11)

```cpp
// src/ui/supervisor.cpp (Rev 11, additive; 23-D50/23-D51/23-D57/23-D61/23-D63/23-D64)
//
//  Supervisor-local cleanup bookkeeping (per-process), all under ONE mutex:
//    std::mutex                                 cleanup_mutex_;
//    std::map<WorkspaceId, std::set<SessionId>> created_sessions_;
//    std::set<SessionId>                        prompted_sessions_;
//    std::map<WorkspaceId, std::string>         pending_creates_;   // existing
//
//  cleanup_mutex_ is LEAF-ONLY (Rev 11, LOW-2): it guards only container
//  operations and is never held across submit, the reply-cv wait, or
//  connection->stop(). Copy the snapshot under the lock, release, then issue
//  deletes.
//
//  1. create_session's reply handler (connection pump thread) is extracted into
//     a named private `on_create_reply(workspace, session, error)` so the test
//     harness can invoke the pump-side path directly (§12.1). The extraction
//     preserves the shipped handler's existing `connection->track(SessionId{
//     session})` call (`src/ui/supervisor.cpp:1249-1253`; `track` sets
//     `subscribe_pending_`, `src/ui/supervisor_connection.cpp:61-70`) verbatim,
//     before enqueue; the new inserts below are added alongside it (Rev 12,
//     LOW-2 — dropping `track()` would change production behaviour). The
//     container inserts run under cleanup_mutex_ (read `pending_creates_` via
//     `find`, never `operator[]`, so an evicted workspace is not re-inserted):
//       const auto pending = pending_creates_.find(workspace);
//       const bool prompt_queued =
//           pending != pending_creates_.end() && !pending->second.empty();
//       if (!session.empty()) {
//           created_sessions_[workspace].insert(session);   // de-duplicated
//           if (prompt_queued) {
//               prompted_sessions_.insert(session);          // SAME critical
//           }                                               // section
//       }
//     cleanup_mutex_ is released before `connection->track(...)` (Rev 13,
//     LOW-4): `track` acquires SupervisorConnection::mutex_, and cleanup_mutex_
//     is leaf-only, so `on_create_reply` calls `track()` **after** releasing
//     `cleanup_mutex_`, then enqueue(apply_create_reply). Do NOT derive the
//     created-set from SupervisorConnection::track (a union of listed + created
//     ids).
//
//  2. Record prompted ids on the direct path at prompt-submission time, under
//     cleanup_mutex_, in prompt() (and submit() when the active id is known):
//       prompted_sessions_.insert(session);
//     The queued-create path with a prompt captured before the reply is covered
//     by (1); a prompt captured after the reply is covered by the step-3
//     pending_creates_ skip. The later record inside apply_create_reply's
//     prompt() call is idempotent.
//
//  3. cleanup_created_sessions() snapshots all three records under
//     cleanup_mutex_ (Rev 10: `created_sessions_`, `prompted_sessions_`, and
//     `pending_creates_`; Rev 11 LOW-4: the skip below reads this snapshot, not
//     the live map) and runs as new step 0 in confirm_exit, before
//     presence_->deregister():
void SupervisorApp::cleanup_created_sessions() {
    // copy created_sessions_ \ prompted_sessions_ and pending_creates_ into
    // locals under cleanup_mutex_, then RELEASE the lock (leaf-only, Rev 11
    // LOW-2) before any submit / cv wait
    // For each (workspace, ids) with a live connection:
    //   skip the whole workspace if the snapshot's pending_creates_[workspace]
    //     is non-empty
    //     (Rev 10, MEDIUM-2: an accepted prompt not yet dispatched by
    //     apply_create_reply);
    //   for each id, skipping any id in prompted_sessions_:
    //     submit session.delete { session, confirm:true,
    //                             only_if_empty:true, force:true }
    //   bounded by options_.cleanup_grace; collect replies on a shared cv.
    // On timeout/error: log and continue (best-effort).
}

// include/ymh/cli/session_cli.hpp (D2, additive) — see §5.7.
```

---

## 4. D3 — Subagent-kind fix (PRE-SHIP blocker)

### 4.1 The defect (verified)

`SessionManager::createSession` hardcodes the kind:

```cpp
// src/session/session_manager.cpp:58
header.kind = SessionKind::Root;
```

`SessionOptions` (`include/ymh/session/session_manager.hpp:28-33`) has **no**
`kind` and **no** `parentSession` field:

```cpp
struct SessionOptions {
    std::filesystem::path cwd;
    std::string           serverProfile;
    std::string           model;
    std::string           title;
};
```

`SubagentRunner::run` (`src/agent/subagent.cpp:18`) creates its child session by
routing through `AgentRegistry::create(options_)`
(`src/agent/agent_registry.cpp:72-88`, which calls
`services_.sessions->createSession(options)`), and the `SessionOptions` it holds
(`include/ymh/agent/subagent.hpp:19-22`) carries no kind/parent either.

**Consequence:** every subagent row is `kind = 'root'` with
`parent_session = NULL`. `SessionKind::Subagent` is assigned **nowhere** in
`src/` or `include/` (only parsed/serialized in `src/session/session.cpp:161-184`),
so the schema arm at `src/session/session_persistence.cpp:49-50`
(`CHECK ((kind='subagent' AND parent_session IS NOT NULL …))`) is **dead code**.
`SubagentRunner` is exercised today only by
`tests/unit/agent_registry_test.cpp:214`, but the defect is a **pre-ship
blocker**: the moment subagents ship, their rows are misclassified.

### 4.2 Why it blocks this spec

1. **The root-only filter would select subagent rows.** Cleanup (§3) and prune
   (§5.2) select `kind == 'root'` only, precisely to avoid dangling
   `SubagentSpawned`/`SubagentFanIn` payloads. With every subagent row labelled
   `root`, the filter does not exclude them.
2. **`hasDependents` cannot protect them.** The dependent check is
   `parent_session`-only (the inline check in `SessionPersistence::erase`,
   `src/session/session_persistence.cpp:777-784`). With `parent_session = NULL`
   on every subagent row, a subagent looks like an independent root, so
   pruning/cleanup can erase it and leave the parent's
   `SubagentSpawned`/`SubagentFanIn` payloads dangling.

Therefore D3 is **not a nit**; it must land before (or with) the cleanup/prune
code (SL7).

### 4.3 The fix (pinned)

```cpp
// include/ymh/session/session_manager.hpp (additive; 23-D53)
struct SessionOptions {
    std::filesystem::path      cwd;
    std::string                serverProfile;
    std::string                model;
    std::string                title;
    SessionKind                kind = SessionKind::Root;      // NEW
    std::optional<SessionId>   parentSession = std::nullopt;  // NEW
};
```

`SessionManager::createSession` copies them into the header (replacing the
hardcoded `Root`) and `validateHeader` enforces the existing kind/parent/seed
matrix (`01` I9; the CHECK at `src/session/session_persistence.cpp:49-50`):

```cpp
header.kind          = options.kind;
header.parentSession = options.parentSession;
validateHeader(header);   // throws std::invalid_argument on a bad matrix (S9)
```

`SubagentRunner` sets them before `registry_.create(options_)`
(`src/agent/subagent.cpp:18`):

```cpp
options_.kind          = SessionKind::Subagent;
options_.parentSession = parent_.id();
const std::expected<AgentId, AgentError> created = registry_.create(options_);
```

### 4.4 Behavior change and blast radius

- **Subagents flip `Root` → `Subagent`.** Every subagent row gains
  `parent_session = <parent id>`.
- **No registry junction for subagents (Rev 8, L2).** `SubagentRunner::run`
  (`src/agent/subagent.cpp:18`) creates its child through
  `AgentRegistry::create` (`src/agent/agent_registry.cpp:72-88`) →
  `SessionManager::createSession` (`src/session/session_manager.cpp:47-71`),
  which does **not** add a `workspace_sessions` row; only
  `HostRuntime::createSession` (`src/host/host_runtime.cpp:539-588`, `:578`) and
  the fork path do. A subagent therefore has **0 junctions** — it is a child
  session row, not a workspace session. Tests and the persistence-assertion rule
  must expect **0** junctions for a subagent, never 1.
- **The unprompted predicate** (§3.4/§5.2) is unaffected in itself (it is a
  `user/message` query), but the root-only gate now correctly excludes subagent
  rows.
- **Every list filter** (`session.list`, `ymh list`, `/sessions`) that gains the
  root-only predicate (SL14/SL18) now behaves correctly for subagents.
- **`hasDependents`** now protects subagents (their parent is set), so the
  dependent precheck is sound for the subagent case too.
- Existing databases with mislabelled subagent rows are legacy data; D4's
  hidden-not-deleted rule means they are not auto-migrated.   A future migration
  is out of scope (§13.2).

---

## 5. D2 — `ymh session prune`

### 5.1 Command surface (pinned; 23-D14)

A new CLI namespace subcommand, consistent with `ymh workspace add|list|stop`:

```text
ymh session prune --empty [--keep <N>]
                  [--workspace <path> | --all] [--yes] [--force] [--json]
```

| Flag | Meaning |
|---|---|
| `--empty` | Select unprompted **root** sessions (§3.4/§5.2). **Required in v1** (the only inclusion flag). |
| `--keep <N>` | **Exclusion modifier only** (applied last): never select the `N` most-recently-updated sessions per targeted workspace (`updated_at` desc, `id` asc). `--keep 0` excludes nothing. |
| `--workspace <path>` | Target one registered workspace by path (canonicalized). Default: the cwd workspace. |
| `--all` | Target every registered workspace. Mutually exclusive with `--workspace`. |
| `--yes` | Apply. Without it the command is a **dry run**. |
| `--force` | Allow pruning the daemon's `active_session_`. Never allows a mid-turn delete. |
| `--json` | Machine-readable output. |

**Selection is opt-in.** `--empty` is required. `--keep` alone is **invalid**
(exit 2), because `--keep 0 --yes` would otherwise wipe every root session while
bypassing the guard. Error text:
`"session prune: pass --empty (--keep is a modifier only)"`.

**`--older-than` is DEFERRED (23-D54).** It is not accepted in v1; passing it is
an unknown flag (exit 2). It is deferred because it selects **every** root
regardless of prompt, so it is exposed to the subagent-kind hazard (23-D53): if a
subagent row is still mislabelled `root`, `--older-than` would select it. Once
D3 has shipped and been observed in the field, a later revision may re-pin it
with the same root-only rule (§5.8, §13.2).

**Selection semantics.** Inclusion = `--empty`. Then `--keep N` excludes the N
most-recent per workspace. Then the always-skip set (§5.4) is removed.

### 5.2 Which sessions are eligible

- **`--empty` selects `kind == 'root'` only (23-D41).** Forks/subagents are
  **never** selected, because pruning a fork/subagent would leave dangling
  session-id references in parent logs (a fork child is referenced by its
  parent's seed/lineage, and subagent ids are routed parent-side, `01` I17).
  SL18/SL-F20. This is sound only once 23-D53 lands (§4).
- A session with dependent children cannot be erased
  (`SessionPersistence::erase` throws `DependentSessionError`). Prune **skips it
  before touching the junction** and reports `reason: "has dependent sessions"`;
  no recursion (§5.3/§5.4, SL10).
- Already-absent sessions are reported `reason: "already gone"`; not an error.

### 5.3 Live vs stopped vs unregistered workspace (23-D8/23-D30/23-D47)

Prune is **hybrid**, decided per targeted workspace by liveness. Liveness is read
via `registry.probeLiveness(id) == HostLiveness::Live` (spec 22 predicate).

- **Candidate enumeration is always from disk (Rev 5 M-2).** For **every**
  targeted workspace, prune opens `<canonical_path>/.ymh/sessions.db` with
  `SessionPersistence::openReadOnly` and calls `list()`, then applies the
  `--empty`/`--keep` filters. It **never** enumerates live candidates via the
  daemon's `session.list`, because §6.1 makes `session.list` hide
  **junction-backed** unprompted roots (store-only entries surface only for
  reconcile, §6.5) — so the reply is not the complete set `--empty` targets. This
  is the primary use case (the 11 observed empties) and must not silently select
  nothing. SL19.
- **Live workspace:** for each selected session, connect over the daemon's Unix
  socket and issue `session.delete{ session, confirm: true,
  only_if_empty: true, force: <options.force> }`. The daemon runs the full
  cleanup in the pinned order (§5.4): mid-turn guard, active guard,
  `only_if_empty` guard, `hasDependents` precheck, PTY close, agent dispose,
  `registry_.removeSession` (junction first — idempotent, crash-safe via its
  `pending_mutation` marker), then the guarded `SessionManager::deleteSession`
  → `eraseWithEvent` (`SessionEnded{Deleted}` + erase, lease-exempt). No new RPC.
  SL10/SL11.
- **Stopped workspace:** open the DB **writable** with `SessionPersistence::open`
  (acquires the workspace `flock`) and, per selected session, run the **same**
  order: `hasDependents` precheck, then `registry_.removeSession(workspace, id)`
  **first** (23-D8/HIGH-1; 23-D30), then
  `SessionManager::deleteSession(id, only_if_empty)` → `eraseWithEvent`
  (`SessionEnded{Deleted}` + erase, lease-exempt). The junction removal is
  mandatory and must precede the erase: without it the `workspace_sessions` row
  leaks and `WorkspaceRegistry::removeWorkspace` refuses while any junction row
  exists (`src/registry/registry.cpp:1017-1043`), making a fully-pruned
  workspace permanently un-removable. SL10.
- **Unregistered workspace** (`--workspace <path>` with no registry row): exit 2.
  Orphan `sessions.db` files are not scanned (mirrors spec 22 §4.5/SW9).
- **No `sessions.db`:** reported `reason: "no sessions.db"`; zero selections; not
  an error.

**Dependent-children precheck (23-D46; SL10).** Before either path calls
`registry_.removeSession`, it calls `store.hasDependents(id)`. If true, the
session is skipped (`reason: "has dependent sessions"`) and **the junction is not
touched**. This closes the defect where junction-first ordering removed the
junction and then `eraseWithEvent` threw `DependentSessionError`, leaving a
**permanently store-only** session. A residual race remains (a fork created
between the precheck and the erase): the erase's inline dependent check still
refuses, but the junction may already be gone; this is recorded (§13.2) and is
best-effort, like the rest of the delete.

### 5.4 The active, mid-turn, and guarded delete (23-D10/D11/D19/D20/D46; MEDIUM-4/5)

The protection is **daemon-authoritative**; the supervisor's focused session is
supervisor-local (`10 §9.10`) and invisible to a separate CLI.

- **Mid-turn (always, any workspace):** `HostRuntime::deleteSession` refuses a
  session for which `AgentRegistry::hasPendingWork(id)` is true
  (`src/agent/agent_registry.cpp:196-206`), or whose `Agent::state()`
  (`include/ymh/agent/agent.hpp`) is a blocked/running state. **Corrected
  classification (MEDIUM-4):** `hasPendingWork()` covers queued-but-not-yet-running
  triggers that `state()` reports as `Idle`; the blocked states
  (`WaitingForPermission`, `WaitingForInput`) are **mid-turn** here even though
  `activationAllowed()` (`agent.hpp`) treats them as activatable — the two
  predicates answer different questions, and this spec must not conflate them.
  The guard is: refuse iff `hasPendingWork(id) || state ∈ {Thinking, CallingTool,
  WaitingForPermission, WaitingForInput, Cancelling}`. `Error` and `Idle` are not
  mid-turn. Prune reports `reason: "turn in progress"`. **Known gap (not
  repaired here, RB-21):** `hasPendingWork()` does not account for a prompt
  already queued in the `TurnExecutor`; a close can drop a just-submitted prompt.
- **Daemon active session (`HostRuntime::active_session_`):** refused unless
  `force: true`; prune reports `reason: "active session"`. `--force` maps to the
  wire `force`. The check is inside `deleteSession` at delete time, so there is
  no CLI-snapshot TOCTOU. **Reachability (Rev 8, MEDIUM-3; 23-D59):** this guard
  fires only when `HostRuntime::active_session_` is set, which happens **only**
  via the `session.activate` RPC (`src/transport/protocol_server.cpp:424-425` →
  `HostRuntime::activateSession`, `src/host/host_runtime.cpp:681-694`). No
  production client sends `session.activate`, and the TUI's `activate_session`
  (`src/ui/supervisor.cpp:1161-1170`) is supervisor-local (it sets only
  `WorkspaceModel::activeSessionId`), so for TUI/headless flows the guard never
  fires. It remains a **real guard for external/automation clients**, and
  `--force` is their opt-out; it is not required for the TUI eager empty. The
  "cannot be raced" claim is **softened**: the
  `Agent::state()`/`hasPendingWork()` reads are ordinary non-atomic reads; the
  guard is best-effort at the boundary and the dry run is the user-facing safety
  net. **M1 (Rev 3):** when a delete (forced or not) removes the session named by
  `active_session_`, `HostRuntime::deleteSession` **resets `active_session_`**
  (mirroring `suspendSession`, `src/host/host_runtime.cpp:703-705`). Without this
  the daemon would keep pointing at a session that no longer exists. SL13.
- **`only_if_empty` (23-D11, MEDIUM-5):** the predicate is checked **before**
  any durable write, via `SessionManager::deleteSession(id, only_if_empty)`. A
  prompted session is refused with `InvalidParams` and nothing is written (no
  `SessionEnded`, no partial erase). This closes the Rev 1 gap where
  `SessionEnded` was appended before the check and `SessionPersistence::erase`
  opened its own transaction. SL5/SL11.
- **Lease handling for the delete (Rev 3, H3).** `SessionManager::deleteSession`
  appends `SessionEnded{Deleted}` through `Session::appendEventLocked`, which
  throws `LeaseLost` unless `store_->isLeaseHolder(id)`
  (`src/session/session.cpp`; `SessionPersistence::isLeaseHolder`). Prune's
  targets are frequently **non-resident** (candidates are enumerated from disk),
  and the stopped path never acquires a session lease. **Pinned:** a
  **lease-exempt, single-transaction** store operation
  `SessionStore::eraseWithEvent(id, event)` appends `SessionEnded{Deleted}`
  **and** deletes the session's snapshots, leases, events, and row in one
  transaction (keeping the `DependentSessionError` guard). The guarded delete
  uses it for **both** paths; `SessionEnded` is kept, but it is written by
  `eraseWithEvent`, not by `Session::append`, so no session lease is required.
  This is an **administrative delete**, not an agent append; it mirrors the
  existing lease-exempt `SessionPersistence::erase` and is an explicit,
  documented exception to I5 (01 §10.1) scoped to `eraseWithEvent`. SL11.
  `eraseWithEvent` is a `SessionStore` method, so it cannot publish; the event is
  a **fully-stamped `Event`** (`include/ymh/core/event.hpp`) built by
  `SessionManager::deleteSession`, which **publishes it to `bus_` after the
  `eraseWithEvent` commit**. The `eraseWithEvent` signature remains
  `(SessionId, Event)`, never a payload.
  **Observation caveat (23-A10):** because the erase removes the row in that same
  transaction, `HostRuntime::handleCommittedEvent`'s store re-read
  (`src/host/host_runtime.cpp:350-358`) no longer sees it, so the bus publish
  alone does not deliver the event to transport subscribers. That defect is
  **out of scope here** and recorded in RB-21 (§12); this spec pins only the
  publish-after-commit, not a forwarding repair.
- **`HostRuntime::deleteSession` order (pinned; 23-D55; all guards precede all
  side effects).** Order matters:
  1. mid-turn guard → `InvalidParams`
  2. `active_session_` guard (unless `force`) → `InvalidParams`
  3. `only_if_empty` guard (`store().isUnprompted(id)`) → `InvalidParams`
     (evaluated **before any side effect**)
  4. `hasDependents(id)` precheck → `DependentSessionError` with **no side
     effect** (the PTY is not closed and the agent is not disposed)
  5. `pty().closeSession(id)` (no-op if no PTY)
  6. dispose the agent if resident (`AgentRegistry::dispose`)
  7. `registry_.removeSession(workspace, id)` — junction FIRST, idempotent
  8. `SessionManager::deleteSession(id, only_if_empty)` → `eraseWithEvent`
  9. reset `active_session_` when it names the deleted id

  **Fix vs Rev 6.** Rev 6 put `hasDependents` at step 6, *after* the PTY close
  (4) and agent dispose (5), so a refused dependent session was already torn
  down. Rev 7 moves the dependent precheck to step 4, before any side effect
  (23-D55). The current shipped `HostRuntime::deleteSession`
  (`src/host/host_runtime.cpp:666-679`) closes the PTY and disposes the agent
  before anything else; Rev 7 reorders it.

  On the live path steps (1)–(9) run inside `HostRuntime::deleteSession`; on the
  stopped path step (1)/(2) are vacuous (no daemon) but steps (3)–(9) are
  executed by prune through the manager + registry directly.

### 5.5 Output and exit status (pinned)

- **Default (no `--yes`): dry run.** Prints `would prune N` and one line per
  candidate (`id`, `kind`, `updated_at`, `reason`); performs no logical mutation;
  opens the registry with `openReadOnly`; does **not** probe liveness (SL24).
- **`--yes`: apply.** Prints `pruned N` / `skipped M` and per-session reasons.
  Exit 0 if every selected session was deleted or already gone; exit 1 if any
  delete failed (partial success is reported, never hidden).
- **Exit 2** for usage errors: no inclusion flag, `--keep` alone, malformed
  `--keep`, `--workspace` + `--all`, unregistered `--workspace`.
- **`--json`:** a single JSON array of `{id, kind, updated_at, status, reason}`
  (dry run and apply alike); exit codes unchanged.

### 5.6 Config and registry access (23-D13/L1)

- Prune enumerates workspaces with `WorkspaceRegistry::openReadOnly` for the dry
  run and the enumeration pass; only the **apply** path opens the registry
  read-write for `removeSession`, per mutation under `flock` (spec 03 §5.3).
- Prune does **not** load the global config (contrast `ymh list`, which does);
  it is a pure store/registry operation. (Config-gate test SL-I17.)
- `session prune` runs in the same CLI process; no daemon is required.

### 5.7 C++ interface sketch (D2, Rev 7)

```cpp
// include/ymh/cli/session_cli.hpp (additive; 23-D6/23-D14)
struct PruneOptions {
    bool                        empty = false;   // v1: the only inclusion flag
    std::optional<std::size_t>  keep;            // exclusion modifier
    std::optional<std::filesystem::path> workspace;
    bool                        all   = false;
    bool                        yes   = false;
    bool                        force = false;
    bool                        json  = false;
};
int session_prune(const PruneOptions& options, std::ostream& out, std::ostream& err);

// include/ymh/session/session.hpp (SessionStore, additive; non-pure defaults so
// in-memory fakes keep compiling; the durable overrides are authoritative):
class SessionStore {
    // ...
    // 23 §3.4: true iff the session's OWN log contains no `user/message`.
    [[nodiscard]] virtual bool isUnprompted(SessionId id) const;
    // 23 §5.3: true iff any sessions row has parent_session == id.
    [[nodiscard]] virtual bool hasDependents(SessionId id) const;
    // 23 §5.4: append `event` and erase snapshots/leases/events/row in ONE
    // lease-exempt transaction. Default (fakes): append + erase.
    virtual void eraseWithEvent(SessionId id, Event event);
};

// include/ymh/session/session_persistence.hpp (additive overrides)
[[nodiscard]] bool SessionPersistence::isUnprompted(SessionId id) const override;
[[nodiscard]] bool SessionPersistence::hasDependents(SessionId id) const override;
void SessionPersistence::eraseWithEvent(SessionId id, Event event) override;

// include/ymh/session/session_manager.hpp (additive)
void SessionManager::deleteSession(const SessionId& id, bool only_if_empty = false);

// src/host/host_runtime.cpp (23-D55; pinned order per §5.4):
//   1. mid-turn guard -> InvalidParams
//   2. active_session_ guard (unless force) -> InvalidParams
//   3. only_if_empty guard -> InvalidParams (before any side effect)
//   4. hasDependents(id) -> DependentSessionError (no side effect)
//   5. pty().closeSession(id)
//   6. dispose the agent if resident
//   7. registry_.removeSession(workspace, id)
//   8. sessions().deleteSession(id, only_if_empty)
//   9. reset active_session_ when it names id
void HostRuntime::deleteSession(const SessionId& id,
                                bool only_if_empty = false,
                                bool force = false);

// include/ymh/transport/host.hpp (23-A1, normative — amended pure virtual)
virtual void deleteSession(const SessionId& id, bool only_if_empty, bool force) = 0;
// src/transport/protocol_server.cpp: parse `only_if_empty` and `force` (both
// optional bools, default false) alongside the existing `confirm` gate and pass
// them through. No other wire change; no kProtocolVersion bump.
```

### 5.8 Proposals (explicitly flagged)

- **23-P1 (Rev 2 — NORMATIVE).** `session.delete.only_if_empty` and
  `session.delete.force` are required behaviour. The `TransportHost` seam and the
  `protocol_server.cpp` parser are amended (§5.7), and both files are in scope
  (§1.4). The confirm gate is unchanged.
- **23-P2 — a `session.prune` wire method — REJECTED** (§5.3).
- **23-P3 — daemon-side idle-empty reaper — REJECTED.** A timer introduces
  nondeterminism; cleanup-on-exit and prune are user-controlled.
- **23-P4 — `--all-kinds` for prune — DEFERRED** (not pinned).
- **23-P5 (Rev 7) — `--older-than` — DEFERRED** (23-D54, §5.1). It selects all
  roots regardless of prompt and is therefore exposed to the subagent-kind
  hazard until 23-D53 is observed in the field.

---

## 6. D4 — Enumeration filters and legacy empties

### 6.1 Filter points (pinned)

| Surface | Code anchor | Change |
|---|---|---|
| `session.list` (daemon) | `HostRuntime::listSessions` (`src/host/host_runtime.cpp:450-469`) | skip a header when **`entry.inJunction`** and `store.isUnprompted(header.id)` and `header.kind == Root`; a store-only entry (`entry.inJunction == false`, the crash-window/pre-M2 reconcile class) always surfaces so the auto-create gate can see it (§6.5, 23-D62) |
| Ctrl-S Live switcher | `SwitcherOverlayModel::open` (`src/ui/ui_model.cpp`); `SupervisorApp::refresh_sessions` (`src/ui/supervisor.cpp:1172-1220`) | `session.list`'s filter applies; `refresh_sessions` reconciles the workspace cell set against the **successful** filtered reply (`reply.ok && reply.result.is_array()`; absent cells removed, active exempt) so a superseded empty stops rendering — a failed/timed-out reply removes nothing and creates nothing (§6.5, 23-D60) |
| `/sessions` catalog | `registry_catalog_source`'s `read` lambda (`src/ui/session_catalog.cpp:155-165`) | pass `include_unprompted = false` to `read_workspace_history`; skip unprompted root headers |
| `ymh list` | `session_list` (`src/cli/session_cli.cpp:170`) | skip unprompted root headers after `store->list()` |

### 6.2 The shared helper stays unfiltered (23-D31; SL15)

`read_workspace_history` (`src/ui/session_catalog.cpp:97-153`, declared
`include/ymh/ui/session_catalog.hpp:70`) is shared: `resolve_session_workspace`
(`src/cli/cli.cpp:369-404`, called at `:380` by `run_supervisor_entry`) uses it
to locate the workspace owning a `--resume <id>`. Filtering inside the helper
would make `ymh --resume <hidden-id>` fail with "unknown session" / exit 1,
contradicting SL16 (while `ymh show/replay/fork <id>`, which load directly, keep
working). The filter is therefore applied **only** at the `/sessions` catalog
consumer, via a new `include_unprompted` parameter that defaults to **true** and
is passed `false` only from the catalog path. `read_workspace_history` itself
never filters.

```cpp
// include/ymh/ui/session_catalog.hpp (23-D31, additive parameter)
WorkspaceHistory read_workspace_history(const WorkspaceRecord& record, bool live,
                                        bool include_unprompted = true);
// registry_catalog_source.read passes false; resolve_session_workspace uses the
// default true.
```

**Explicit-id surfaces are unaffected.** `ymh show <id>`, `session.show`,
`session.resume`, `session.replay`, and `ymh --resume <id>` still operate on a
hidden durable session by id.

**The active session is never hidden.** The filter applies to enumeration, not to
the active `SessionUiState`.

### 6.3 Legacy empties are hidden, not auto-deleted (23-D7; SL16)

Existing `sessions.db` files already contain unprompted rows. They are **hidden**
from every enumeration surface (§6.1) but **not** auto-deleted — in particular,
cleanup-on-exit does **not** touch them (it is scoped to this supervisor's own
creates, §3.3). They remain reachable by explicit id and are reclaimed only by
the explicit `ymh session prune --empty`. **Exception (Rev 9, 23-D62):** a
**store-only** unprompted root (a pre-M2 row with no junction) surfaces in the
daemon's `session.list` reply so the auto-create gate can see it (§6.5); the
`/sessions` catalog and `ymh list` still hide it, and it is still never
auto-deleted.

### 6.4 Interaction with specs 19 and 22

- **Spec 19 (auto-name).** Policy untouched. The auto-rename still fires on the
  first `agent.prompt` before `UserMessage`; the filter uses the `user/message`
  predicate, not the title. After creation, the cell title is updated by the
  streamed `SessionRenamed` (`UiModel::apply`'s `SessionTitleChanged` branch,
  `src/ui/ui_model.cpp`; `ui_event_adapter.cpp`), so the title does not depend on
  an unrelated `refresh_sessions`.
- **Spec 22 (`/sessions`).** SW9 (registered-only) and SW10 (failure surfacing)
  are unchanged; the filter only removes unprompted root entries from an already
  registered-only set. An empty workspace still reports its normal note.

### 6.5 Refresh auto-create and the Ctrl-S cell set (Rev 9; 23-D58/23-D60/23-D62)

**MEDIUM-2 + MEDIUM-3: the D4 filter must not drive session creation, and the
existence test must include store-only sessions.** `session.list` is filtered by
D4 (§6.1), but `refresh_sessions` decides "no sessions → `create_session`" from
the reply (`src/ui/supervisor.cpp:1209-1216`) and, on `Attached`, runs *before*
the pending resume (`:1147` then `:1150-1155`). A workspace whose only sessions
are unprompted roots therefore returns an empty list and the TUI eagerly creates
a new empty; `apply_resume_success` (`src/ui/supervisor.cpp:1005-1016`) then
overrides focus via `UiModel::focusSession` (`src/ui/ui_model.cpp:893-908`),
leaving a stray session. That silently changes session-creation behaviour via the
read path and breaks the pinned `--resume <hidden-id>` behaviour (SL-I4).

**Why the Rev 8 junction-only gate was incomplete (Rev 9, MEDIUM-3).**
`WorkspaceRegistry::listSessions` (`src/registry/registry.cpp:853-865`) returns
**only** `workspace_sessions` (junction) rows. Junctions are written only by
`HostRuntime::createSession` (`src/host/host_runtime.cpp:578`) and the fork path
(`:643`); there is **no startup backfill**. `orderSessions`
(`src/registry/registry.cpp:609-644`; `include/ymh/registry/registry.hpp:91-98`)
explicitly documents a **store-only** session — a store row with no junction,
produced by the crash window between the store commit
(`src/host/host_runtime.cpp:571`) and the junction write (`:578`), or by pre-M2
`sessions.db` rows. The Rev 8 claim that "every durable session has a junction"
was therefore **false**: a workspace whose only sessions are store-only
unprompted roots still reports zero → spurious create, and a
`--resume <store-only-id>` still leaves a stray.

**Pinned mechanism (no wire change).** The auto-create decision is taken from the
**union** of the unfiltered registry junction set and the reply, and the daemon's
D4 skip is scoped to **junction-backed** entries so store-only entries always
surface:

```cpp
// src/host/host_runtime.cpp — HostRuntime::listSessions: the D4 skip is scoped
// to junction-backed entries; a store-only entry (entry.inJunction == false, the
// reconcile class flagged by orderSessions) always surfaces. The reply shape is
// unchanged (SessionSummary carries no in-junction field; the flag is internal).
if (entry.inJunction && store.isUnprompted(header.id) && header.kind == Root) {
    continue;
}
```

```cpp
// src/ui/supervisor.cpp — the reply handler computes `sessions` only under
// `reply.ok && reply.result.is_array()` (:1181) and captures the predicate; the
// reply-driven update inside the enqueue runs only on success:
const bool reply_ok = reply.ok && reply.result.is_array();
enqueue([this, workspace, sessions, reply_ok] {
    if (!reply_ok) {
        return;   // failed/timed-out list: no cell add, no cell erase, no create
    }
    // ... populate cells from `sessions`, then the auto-create gate:
    const bool registry_known = options_.registry != nullptr;
    const bool workspace_has_sessions =
        registry_known
            ? (!options_.registry->listSessions(workspace).empty() || !sessions.empty())
            : !sessions.empty();             // registry unavailable: reply only
    if (it->second.activeSessionId.value.empty() && !workspace_has_sessions) {
        create_session(workspace, std::string{});
    }
});
// `sessions` now includes store-only entries (the scoped D4 skip above), so the
// union covers junction-backed AND store-only sessions. If the reply is empty
// but junctions exist, do NOT create. If the registry handle is unavailable, the
// reply alone is used.
```

**Why not move the D4 filter to the supervisor (the other offered option).** The
supervisor has no store access and `SessionSummary`
(`include/ymh/transport/protocol.hpp:355-362`) carries no prompt flag, so it
cannot evaluate `isUnprompted`; exposing that flag would be a wire change.
Scoping the daemon skip to `entry.inJunction` is daemon-internal —
`SessionOrderEntry.inJunction` is already computed by `orderSessions` and is not
in the reply — so the reply shape and `kProtocolVersion` are untouched.

**D4 carve-out (recorded).** `session.list` still hides **junction-backed**
unprompted roots; a store-only entry surfaces (it is the crash-window/pre-M2
reconcile class, not the junction-backed legacy-empty class D4 targets), so the
Ctrl-S live switcher may render a store-only root. That is the price of making
the gate complete without a wire change; it is recorded in §13.2#14. The
`/sessions` catalog (`read_workspace_history` with `include_unprompted = false`)
and `ymh list` still hide unprompted roots, so D4's catalog behaviour is
unchanged.

**Citation note (Rev 9, LOW).** `options_.registry` is held by `SupervisorApp`
(`include/ymh/ui/supervisor.hpp:48`). It is *not* read on the supervisor thread
via `workspace_spec_from_registry`: that helper is called from
`ensure_worker_loop` on the **ensure-worker** thread (`std::jthread` spawned at
`src/ui/supervisor.cpp:363`; the call is at `:941`). The gate's `listSessions`
call runs on the supervisor thread inside the `enqueue` lambda;
`WorkspaceRegistry` methods are internally synchronized (`impl_->mutex`,
`src/registry/registry.cpp:854`), so the cross-thread use is safe (a single
indexed read). A stale/leaked junction errs toward "do not create", which is safe
(§13.2).

**L1: reconcile the Ctrl-S cell set — successful replies only (Rev 9,
MEDIUM-1).** `refresh_sessions` currently only adds cells (`ensureCellIn`,
`src/ui/ui_model.cpp:443-456`) and sets titles; nothing removes a cell, so a
superseded `/new` empty keeps its `SessionCell` and still renders in the Ctrl-S
switcher (`SwitcherOverlayModel::open` iterates `workspace.sessions`,
`src/ui/ui_model.cpp:910-929`). **Pinned:** only when the reply is successful —
the same `reply.ok && reply.result.is_array()` predicate that builds `sessions`
(`src/ui/supervisor.cpp:1181`) — `refresh_sessions` removes every cell of that
workspace whose id is **absent from the filtered reply** and is **not** the
workspace's active session (§6.2's active-session exemption), via
`UiModel::eraseSession` (`src/ui/ui_model.cpp:541-556`). A session that is merely
unprompted but active is therefore never removed; a superseded empty is. This
makes SL14 hold for the live switcher after the next successful refresh.

**On a failed or timed-out reply (`!reply.ok`) the reply-driven update is
skipped entirely.** `sessions` is empty (it is built only under `reply.ok &&
reply.result.is_array()`), and the Rev 8 code — which `enqueue`d the reconcile
unconditionally (`src/ui/supervisor.cpp:1196`) — would erase every non-active
cell *and* the global `SessionUiState` (`UiModel::eraseSession` also erases
`model_.sessions[id]`, `src/ui/ui_model.cpp:542`), discarding input drafts on a
transient list failure. Rev 9 gates the whole reply-driven block — cell add, cell
reconcile/erase, **and** the auto-create decision — on the same predicate as the
parse. The auto-create part matters too: a failed/timed-out list is **not**
evidence of "zero sessions", so treating it as empty would spuriously create
(the union gate's fallback `!sessions.empty()` is false on a failure). The
workspace remains usable because the first-keystroke path
(`src/ui/supervisor.cpp:1900-1907`) still creates on the user's first input.

---

## 7. D5 — Orphan-junction sweep

**Junction-first ordering on both delete paths** (23-D8/23-D30; SL10): the
`workspace_sessions` row is removed **before** the store erase, and only after
the `hasDependents` precheck. This is what makes a fully-pruned workspace
removable (`WorkspaceRegistry::removeWorkspace` refuses while any junction row
exists, `src/registry/registry.cpp:1017-1043`).

**Sweep scope (23-D47; SL23).** A pre-existing leak (an erase that succeeded but
whose junction removal was lost to a crash/SIGKILL between the two DBs) is
repaired by a sweep restricted to **flock-held contexts only**:

1. **Prune's stopped-path apply** — it already holds the writable store's
   `flock`.
2. **Daemon startup** — strictly between `WorkspaceRegistry::claimHost`
   (`src/host/workspace_host.cpp:587`) and
   `host_runtime_->setState(protocol::HostState::Serving)` (`:598`).

The sweep is **never** invoked on the live RPC path: a concurrent create's
junction (row written before the session's own row is visible to the sweep) must
not be deleted.

**Fresh-store guard (23-D56; SL23).** `SessionPersistence::open` uses
`SQLITE_OPEN_CREATE` (`src/session/session_persistence.cpp:640-641`), so opening
a **missing** `sessions.db` creates an empty one. An unguarded sweep would then
see zero sessions and classify **every** junction as an orphan, deleting them
all. Pinned: the sweep runs only if the `sessions.db` **pre-existed** the
writable open — capture `bool db_preexisted =
std::filesystem::exists(db_path)` **before** `SessionPersistence::open` and skip
the sweep when it is false.

**What the sweep does.** For each junction `(workspace, session_id)` whose
session id is absent from the store (`store->load(id)` is `nullopt`), call
`registry_.removeSession(workspace, id)` (idempotent). It never touches a session
that exists in the store.

---

## 8. Placeholder titles (out of scope here)

The placeholder-title header change (`tui`/`headless`/`main`) is **RB-20**, a
UI-local, gate-free item. It is recorded but not pinned by this spec (SL25).

---

## 9. Invariants

Numbered `SL1`–`SL28`; testable and cited. (The Rev 7 set is a fresh numbering;
prior revisions' `SL` numbers are historical. Rev 8 adds `SL27`–`SL28` and
amends `SL2`/`SL4`/`SL5`/`SL14`/`SL22`; Rev 9 amends `SL4`/`SL14`/`SL27`/`SL28`
and adds failure modes `SL-F27`–`SL-F29` and tests `SL-U15`, `SL-I29`, `SL-I30`;
Rev 10 amends `SL4`/`SL14`/`SL27`/`SL-F28` and adds test `SL-U16`; Rev 11 pins
`SL4`'s `cleanup_mutex_` as leaf-only (LOW-2) and corrects the `SL-U16`
empty-create driver (MEDIUM-1).)

| ID | Invariant |
|---|---|
| SL1 | **Creation frozen.** `session.create` persists at create time and the TUI eager creates (`src/ui/supervisor.cpp:1215`,`:1425`,`:1905`) and the dead `submit()` path (`:388`) are retained. No creation-path change, no wire retirement, no protocol bump, no TUI no-session state. |
| SL2 | **Cleanup-on-exit.** At a clean supervisor exit, before spec-16 deregister/teardown, the supervisor deletes every session **it created and did not prompt** that is still an unprompted root. |
| SL3 | **Cleanup ordering.** Cleanup is step 0 of `confirm_exit` (`src/ui/supervisor.cpp:595-609`), before `presence_->deregister()` and `teardown_daemons`; it does not deregister, signal, or alter the spec-16 orphaning set/admission/watchdog; it is bounded by `cleanup_grace`. |
| SL4 | **Cleanup scope.** Cleanup targets `created_sessions_ \ prompted_sessions_`, **excluding every created id whose workspace has a non-empty `pending_creates_` entry** — three distinct, **per-process** records, all guarded by one **leaf-only** `cleanup_mutex_` and snapshotted in one critical section (Rev 11 LOW-2: the lock guards only container operations and is released before any `submit`, reply-cv wait, or `connection->stop()`). `created_sessions_` is populated only by this supervisor's `session.create` replies; `prompted_sessions_` records the returned id in the **same critical section** when a prompt was queued behind that create (plus the direct path in `prompt()`), and the `pending_creates_` snapshot covers a prompt queued **after** the reply but before `apply_create_reply` is drained — so no accepted prompt can be overtaken in either ordering (SL-U15/SL-U16). Cleanup never deletes a session it did not create, a prompted session, a fork/subagent, or a session with dependent children. `SupervisorConnection::track` is a union and is not the source. (A second supervisor that resumes+prompts one of this supervisor's empties is a recorded cross-supervisor residual, §3.3/§13.2#2.) |
| SL5 | **Guarded delete.** Cleanup/prune use `session.delete{only_if_empty:true}`; the predicate is checked before any durable write; a refusal writes no `SessionEnded` and no partial erase. Cleanup passes `force:true` as **defence-in-depth for external clients** that call `session.activate`; the TUI never sets `HostRuntime::active_session_`. |
| SL6 | **Best-effort.** A cleanup delete failure or a crash without a clean exit leaves the row; `ymh session prune --empty` is the backstop. Cleanup is not a guarantee. |
| SL7 | **Subagent kind (pre-ship).** `SessionManager::createSession` honours `SessionOptions::kind`/`parentSession`; `SubagentRunner` passes `Subagent`/parent; no subagent row is `root`. |
| SL8 | **Predicate.** Unprompted = own-log absence of `user/message`; never the title; evaluated daemon-side at delete time. |
| SL9 | **Delete order.** `HostRuntime::deleteSession` runs **all guards before all side effects**: mid-turn → active → only_if_empty → hasDependents → PTY close → agent dispose → junction → erase → active reset. |
| SL10 | **Dependent precheck.** Both delete paths call `hasDependents(id)` before touching the junction; a dependent session is skipped with the junction **and** the PTY/agent untouched. |
| SL11 | **Lease-exempt erase.** `eraseWithEvent` appends `SessionEnded{Deleted}` and removes snapshots/leases/events/row in one lease-exempt transaction; `SessionEnded` is published to `bus_` after the commit. |
| SL12 | **PTY close.** The delete closes the session PTY (step 5) after all guards. |
| SL13 | **Active reset.** A delete that removes `active_session_` resets it. |
| SL14 | **No-surface-empties (except the active session).** No enumeration surface listed in §6.1 returns or renders an unprompted **junction-backed** `root` session, **except the workspace's active session** — the Ctrl-S reconcile exempts it (§6.5) and `SwitcherOverlayModel::open` renders every `workspace.sessions` cell, so an active unprompted junction-backed root does render in Ctrl-S. A store-only entry (`inJunction == false`) surfaces (the reconcile class, §6.5/§13.2#14). `refresh_sessions` reconciles the Ctrl-S cell set against the **successful** filtered reply (`reply.ok && reply.result.is_array()`), removing a superseded empty's cell while exempting the active session; a failed/timed-out reply removes nothing and creates nothing (§6.5). |
| SL15 | **Shared helper unfiltered.** The filter lives at the `/sessions` catalog consumer; `read_workspace_history` gains `include_unprompted` defaulting to true; `resolve_session_workspace` uses the default. |
| SL16 | **Legacy hidden, not deleted.** Legacy unprompted sessions are hidden, never auto-deleted (including by cleanup); explicit-id access works, including `ymh --resume <hidden-id>`. |
| SL17 | **Prune opt-in.** `--empty` is required; `--keep` alone is invalid (exit 2); `--keep 0` excludes nothing; apply requires `--yes`. |
| SL18 | **Prune root-only.** `--empty` selects `kind == 'root'` only; forks/subagents are never selected. |
| SL19 | **Candidates from disk.** Live candidates are enumerated from disk (`openReadOnly`), never via the filtered `session.list`. |
| SL20 | **Hybrid paths.** Live workspaces delete via `session.delete`; stopped workspaces delete via the manager + registry; both follow the pinned order. |
| SL21 | **Registered only.** Prune targets registered workspaces only; an unregistered `--workspace` errors (exit 2). |
| SL22 | **Active/mid-turn.** Prune never deletes the daemon's `active_session_` unless `--force`, and never a mid-turn session. `active_session_` is set only by the `session.activate` RPC (no production client sends it); the guard is real for external clients and vacuous for TUI/headless flows (23-D59). |
| SL23 | **Sweep scope + fresh-store guard.** The orphan-junction sweep runs only in flock-held contexts (prune stopped-path apply; daemon startup between `claimHost` and `setState(Serving)`) and only if the `sessions.db` pre-existed the writable open; never on the live RPC path. |
| SL24 | **Dry-run safety.** The dry run performs no logical mutation, opens the registry `openReadOnly`, and does not probe liveness; SQLite sidecars are not logical mutations. |
| SL25 | **No schema change.** `kSchemaVersion` is unchanged; no migration; the placeholder-title item (RB-20) is out of scope. |
| SL26 | **RB-21 separation.** The pre-existing lifetime/teardown defects (§12) are recorded as an independent hardening effort and are not load-bearing for this spec. |
| SL27 | **Prompted-set exclusion + recording point (Rev 8/9/10, MEDIUM-1/2).** Cleanup never targets an id in `prompted_sessions_`, **and never targets any created id whose workspace has a non-empty `pending_creates_` entry**, so a prompt accepted but not yet durable is never deleted and its queued turn is not dropped. The id is recorded under `cleanup_mutex_` at the earliest knowable point: in `prompt()` for the direct path, and in the `session.create` reply handler (the same critical section as `created_sessions_`) when a prompt is queued **before** the reply. For a prompt queued **after** the reply but before `apply_create_reply` is drained, the accepted text is in `pending_creates_`; cleanup snapshots that record in the same critical section and skips the workspace's created ids (SL-U16). There is therefore no window, in either ordering, in which created is recorded but the accepted prompt is unaccounted for (§3.3). |
| SL28 | **Refresh gate (Rev 8/9, MEDIUM-2/3).** `refresh_sessions` decides auto-create from the **union** of the unfiltered registry junction set and the reply, never from the D4-filtered `session.list` reply alone; the daemon's D4 skip is scoped to `entry.inJunction`, so store-only entries surface and are counted. Attaching to a workspace whose only sessions are unprompted roots — junction-backed **or** store-only (pre-M2 / crash window) — creates nothing (§6.1/§6.5). |

---

## 10. Failure modes

Prefix `SL-F#` (trigger / symptom / recovery).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| SL-F1 | Cleanup delete RPC fails (daemon gone, timeout) | An unprompted row survives | Best-effort: log and continue; `session prune --empty` is the backstop (SL6) |
| SL-F2 | Supervisor crashes without a clean exit | No cleanup runs; rows survive | Prune backstop (SL6) |
| SL-F3 | A created session is prompted **durably** between create and exit | Cleanup would delete a started session | `only_if_empty` refuses with `InvalidParams`; nothing written (SL5) |
| SL-F4 | Supervisor exits next to another live supervisor on a shared daemon | A foreign client's empty session could be deleted | Cleanup is scoped to this supervisor's `created_sessions_`; foreign ids are never targeted (SL4) |
| SL-F5 | An external client calls `session.activate` on a created empty | The active guard would refuse the delete | Cleanup passes `force:true` (bypasses only the active guard); the TUI never sets `active_session_` (SL5, 23-D59) |
| SL-F6 | A created empty session gained a fork | Erasing the parent dangles the fork's lineage | `hasDependents` precheck before any side effect; skip, junction/PTY untouched (SL10) |
| SL-F7 | A subagent row is still `kind='root'` (pre-23-D53) | The root-only filter selects it; `hasDependents` cannot protect it | **23-D53 is a pre-ship blocker**; it must land with cleanup/prune (SL7) |
| SL-F8 | Prune races a daemon start/stop | `flock` open or RPC connect fails | Re-classify liveness and retry the other path once |
| SL-F9 | Prune selects a session that becomes mid-turn | Would delete a running turn | Daemon refuses via `hasPendingWork`/state (SL22); reported `skipped` |
| SL-F10 | Prune selects a session an external client activated via `session.activate` | Would delete the active session | Refused unless `--force` (SL22); reported `skipped: active session` |
| SL-F11 | `only_if_empty` on a now-prompted session | Would delete a started session | Refused with `InvalidParams`; nothing written (SL5) |
| SL-F12 | Prune selects a session with dependent forks | Junction removed, then `eraseWithEvent` throws `DependentSessionError`; a permanent store-only session | `hasDependents` precheck before `removeSession`; skip + report `has dependent sessions`; junction untouched (SL10) |
| SL-F13 | Prune a stopped workspace, junction removal fails | A leaked `workspace_sessions` row makes the workspace un-removable | Junction-first ordering; a failure aborts before the erase and a retry completes (SL10) |
| SL-F14 | Partial prune: erase succeeds, `removeSession` fails/SIGKILL between DBs | Permanent invisible orphan junction | Junction-first ordering + orphan sweep in flock-held contexts (SL23) |
| SL-F15 | `--keep` alone, or a malformed `--keep` | Ambiguous selection | Exit 2, no mutation (SL17) |
| SL-F16 | `--empty` on a fork | Would hide/prune an intentional session | Forks are exempt (SL18, §5.2) |
| SL-F17 | Unregistered `--workspace` | Would target an unregistered path | Exit 2, no mutation (SL21) |
| SL-F18 | `session.delete` without `confirm: true` | Accidental delete | `InvalidParams`; unchanged confirm gate |
| SL-F19 | Forced delete of the active session | `active_session_` points at a deleted session | `deleteSession` resets it (SL13) |
| SL-F20 | `--empty` selects a subagent (pre-23-D53) | Dangling parent-side session-id references | 23-D53 (SL7) + root-only selection (SL18) |
| SL-F21 | Sweep runs against a freshly created store | Every junction looks like an orphan and is deleted | Fresh-store guard: skip the sweep unless `sessions.db` pre-existed (SL23) |
| SL-F22 | Dry run `O_CREAT`s `sessions.lock` / WAL `-shm` | "read-only" dry run writes files | Dry run skips liveness probing; SL24 narrows the wording |
| SL-F23 | A repair relies on the lifetime/teardown code | Latent UAF / dropped prompt / unobserved `SessionEnded` | Out of scope: recorded as RB-21 (SL26, §12) |
| SL-F24 | `--older-than` passed in v1 | Unknown flag | Exit 2 (deferred, 23-D54) |
| SL-F25 | A prompt is accepted but its `UserMessage` is not yet durable at exit (queued on the `TurnExecutor`) | `only_if_empty` still sees the session as unprompted; cleanup would erase it and drop the queued turn | `prompted_sessions_` excludes the id from cleanup (SL27, MEDIUM-1) |
| SL-F26 | A workspace's only sessions are unprompted roots (junction-backed or store-only); `refresh_sessions` reads the D4-filtered reply | The TUI spuriously auto-creates a stray empty (and breaks `--resume <hidden-id>`) | Auto-create is gated on the **union** of the registry junction set and the reply, with the D4 skip scoped to `inJunction` entries (SL28, §6.5, MEDIUM-2/3) |
| SL-F27 | A `session.list` reply times out or fails (`!reply.ok`) on attach | The reply is empty; an unconditional cell reconcile erases every non-active cell and its `SessionUiState`/input draft, and the empty-list fallback can spuriously create | The whole reply-driven update (cell add, reconcile/erase, auto-create) runs only under `reply.ok && reply.result.is_array()`; a failed reply removes nothing and creates nothing (SL14/SL28, §6.5, MEDIUM-1) |
| SL-F28 | A prompt is queued behind an in-flight `session.create` and the exit key runs before the UI drains `apply_create_reply` — either **before** the reply (reply not yet arrived) or **after** the reply but before `apply_create_reply` runs (the accepted text sits in `pending_creates_`) | Cleanup sees the id in `created_sessions_` but not `prompted_sessions_`, deletes the row, and drops the queued prompt | All orderings are closed: (i) reply-not-yet-arrived — neither set contains the id, so cleanup cannot target it; (ii) prompt-before-reply — the reply handler records both sets in the same locked critical section before enqueueing `apply_create_reply`; (iii) prompt-after-reply — cleanup snapshots `pending_creates_` in the same critical section and skips every created id whose workspace has a non-empty entry (SL4/SL27, §3.3, SL-U15/SL-U16, MEDIUM-2) |
| SL-F29 | A store-only root exists (crash between the store commit and the junction write, or a pre-M2 row) and the gate reads only registry junctions | The junction set is empty → spurious create; `--resume <store-only-id>` leaves a stray | The D4 skip is scoped to `entry.inJunction`, so store-only entries surface in the reply and the union gate counts them (SL28, §6.5, MEDIUM-3) |

---

## 11. dsh mapping

- **Event-sourced truth.** The unprompted predicate is a projection over the
  durable event log; hiding is a read-side projection; cleanup and prune are
  command effects that append the terminal `SessionEnded` and erase the
  aggregate. The log never contains a session without a user-visible event after
  a clean exit.
- **Durable aggregate.** `session.create` still creates the durable aggregate
  eagerly (the shipped model). Cleanup-on-exit is an explicit compensating
  command, not a lifetime model; it does not require an unsaved-aggregate
  concept.
- **Explicit cleanup.** No auto-reaper timer; cleanup is a deterministic
  end-of-exit step, and prune is the explicit, user-controlled backstop.
- **Ownership.** Cleanup respects spec 16's ownership: it runs while the
  supervisor is still an owner and never competes with the daemon's shutdown.

---

## 12. Test plan

Concrete IDs; extend existing files where named. Deterministic tests use the Fake
LLM / in-memory stores; live tests are opt-in (`YMH_LIVE_LLM=1`).

**Persistence-assertion rule (mandatory).** Every persistence assertion must
count **rows AND leases AND junctions** for the affected ids (and events where
relevant), not just `sessions` rows. A test that asserts only "the row is gone"
is incomplete. **Exception (Rev 8, L2):** a subagent session has **0 junctions**
by construction (`AgentRegistry::create` → `SessionManager::createSession` adds
no `workspace_sessions` row, §4.4), so subagent assertions count **0**.

### 12.1 Unit / integration — cleanup-on-exit

| ID | Test | Asserts |
|---|---|---|
| SL-U1 | cleanup deletes an unprompted created session | after a clean exit, the created id has 0 rows, 0 leases, 0 junctions, 0 events |
| SL-U2 | cleanup keeps a prompted session | a created session with a `user/message` survives: 1 row, 1 lease, 1 junction, events intact |
| SL-U3 | cleanup keeps a fork/subagent | a fork/subagent created during the session survives regardless of the parent's unprompted state |
| SL-U4 | cleanup keeps a dependent parent | a created empty parent with a fork is refused by `hasDependents`; parent row/lease/junction/events intact |
| SL-U5 | cleanup scope | a foreign session (created by another connection) and a legacy empty are **not** deleted |
| SL-U6 | cleanup ordering | `cleanup_created_sessions` is invoked before `presence_->deregister` and `teardown_daemons` (spy/ordering assertion); a shared daemon (not in the orphaning set) still receives the deletes |
| SL-U7 | cleanup uses force+only_if_empty (Rev 8, MEDIUM-3) | the harness **explicitly sends `session.activate`** for the created empty first (so `HostRuntime::active_session_` is actually set), then cleanup's `force:true` delete succeeds and `active_session_` is reset (SL13); a session prompted durably between create and exit (via `submit()` against a workspace that is active per §12.1 **and seeded with `activeSessionId` = the known id**, so `submit()` takes the direct `prompt()` path `:391` and records `prompted_sessions_` rather than the create path `:388` — Rev 13, LOW-2) is refused and kept. Falsifiable: without the explicit `session.activate` the guard is never exercised (the Rev 7 assertion was vacuous); without `force` the active guard returns `InvalidParams` and the row survives |
| SL-U8 | cleanup best-effort | with the daemon unreachable, exit still completes; the row survives; no crash |
| SL-U9 | crash without clean exit | a simulated crash path leaves the unprompted row; `prune --empty` then removes it |
| SL-U13 | queued-prompt race (Rev 8, MEDIUM-1; Rev 12 active-workspace prerequisite) | with the workspace made active (`seed_workspace(ws)` then `focus_workspace(ws)`, §12.1) **and seeded with `activeSessionId` = the known id** (so `submit()` takes the direct `prompt()` path `:391` and records `prompted_sessions_`, not the create path `:388` — Rev 13, LOW-2), and a `TurnExecutor` that has not yet run the queued turn (no durable `user/message`), `submit()`/`prompt()` records the id in `prompted_sessions_`; on clean exit cleanup issues **no** `session.delete` for that id and the row/lease/junction/events survive. Falsifiable: the Rev 7 behaviour (created-set only, no prompted-set) deletes it — 0 rows/leases/junctions — dropping the queued turn |
| SL-U14 | created-set is distinct and per-process (Rev 8) | with a foreign prompted session `B` listed by `refresh_sessions` and a created empty `A`, a recording test double asserts cleanup sends a delete request for **`A` only**; `B` receives none, and a pre-existing unprompted root from a prior run is likewise never targeted. Falsifiable: a `SupervisorConnection::track` **union** source would include `B` (listed) and send a delete for it |
| SL-U15 | queued-create-then-immediate-exit (Rev 9, MEDIUM-2; Rev 12 active-workspace prerequisite) | with the workspace made active (`seed_workspace(ws)` then `focus_workspace(ws)`, §12.1; `submit()` calls `model_.activeWorkspace()` at `:371`), drive `submit()` with an empty `activeSessionId` and text so `create_session` queues the prompt; let the pump-side `session.create` reply handler run via the harness `run_create_reply` seam (§12.1) (recording `created_sessions_` **and** `prompted_sessions_` under `cleanup_mutex_`) but **do not** drain the UI queue, then invoke `confirm_exit` via the harness; assert cleanup issues **no** `session.delete` for the created id (observed via `take_delete_requests`, §12.1) and the row/lease/junction/events survive. Falsifiable: Rev 8 records `prompted_sessions_` only in the UI-thread `apply_create_reply`, so cleanup sees created-but-not-prompted and deletes (0 rows/leases/junctions), dropping the queued turn. **Synchronization assertion:** if the suite has a ThreadSanitizer configuration, the same test run under TSan must be race-free; Rev 8's write-on-pump-thread / read-on-UI-thread access to `created_sessions_` is a reported data race, while Rev 9's `cleanup_mutex_` is clean |
| SL-U16 | prompt-queued-after-create-reply (Rev 11, MEDIUM-1 driver fix; Rev 12 active-workspace prerequisite; Rev 10 ordering) | **Empty-create driver (Rev 11).** With an attached workspace whose `activeSessionId` is empty and no `SessionUiState`, **made active via `seed_workspace(ws)` then `focus_workspace(ws)` (§12.1; without it `handle_input`'s `model_.activeWorkspace()` at `:1903` and `submit()`'s at `:371` are null and the test is vacuous — Rev 12, MEDIUM-1)**, drive a **real** empty-create: dispatch a printable key so `handle_input`'s first-keystroke path (`src/ui/supervisor.cpp:1900-1907`) calls `create_session(ws, "")` at `:1905` (equivalently `new_session` at `:1425`, or the harness `create_session(ws, "")` seam, §12.1). A real production empty-create is required to reach `pending_creates_[ws]=""`; the `submit()` path cannot: it returns immediately on empty text (`:368-370`) **before** its create call (`:388`), so a `submit()`-with-empty-text driver is a no-op, creates no session, runs no reply, and the test passes without the `pending_creates_` snapshot (Rev 10 gate, MEDIUM-1). Then let the **pump-side** `session.create` reply handler run via the harness `run_create_reply` seam (§12.1) — it sees the empty pending map and records the returned id in `created_sessions_` **only**, enqueueing `apply_create_reply`; **do not drain**. Then, without draining, drive `submit()` with non-empty text (the `:388` create path; the dedup branch `:1230-1236` stores it in `pending_creates_[ws]` and sends no second create). Then invoke `confirm_exit` via the harness (§12.1) before any `Event::Custom` drain (`:2115-2116`). Assert cleanup issues **no** `session.delete` for the created id (`take_delete_requests`, §12.1) and the row/lease/junction/events survive. Falsifiable: without the `pending_creates_` snapshot in `cleanup_created_sessions`, cleanup targets `created_sessions_ \ prompted_sessions_` = the id (the reply handler recorded no prompted entry because the prompt arrived after the reply) and deletes it — 0 rows/leases/junctions — dropping the accepted prompt. This is the **opposite ordering** to SL-U15 (SL-U15: prompt before reply; SL-U16: prompt after reply, before drain) |

**Harness surface (Rev 11, LOW-3; active-workspace seam added Rev 12, MEDIUM-1;
pinned).** SL-U15/SL-U16 drive internal `SupervisorApp` paths that
`SupervisorHarness` (`include/ymh/ui/supervisor_harness.hpp:34-92`, impl
`SupervisorHarnessImpl` at `src/ui/supervisor.cpp:2285-2417`) does not expose
today — of the seams these tests need, it exposes only `apply_create_reply`,
`drain_actions`, and `dispatch_key` (it also exposes unrelated seeds/observers,
e.g. `seed_workspace`). **Extend that header and its impl** (test-support surface
only; production never calls it) with these seams and semantics:

- `focus_workspace(workspace)` — invokes `UiModel::focusWorkspace`
  (`include/ymh/ui/ui_model.hpp:474`; `src/ui/ui_model.cpp:885-891`), setting
  `model_.activeWorkspaceId` for the workspace. **Active-workspace prerequisite
  (Rev 12, MEDIUM-1; premise reworded Rev 13, LOW-1):** no harness-reachable path
  sets `activeWorkspaceId` **without also setting `activeSessionId`** — `run()`
  (`src/ui/supervisor.cpp:343`) sets it alone but the harness never calls it
  (`SupervisorHarnessImpl` ctor `:2287-2291` only spawns the ensure worker), and
  the harness-exposed `apply_resume_success` (`:1005-1016`) → `UiModel::
  focusSession` (`src/ui/ui_model.cpp:893-908`, `activeWorkspaceId` `:903`) sets
  both. `seed_workspace` (`:2349-2351`) only inserts into `model_.workspaces`,
  and `focusWorkspace` no-ops unless the workspace already exists
  (`src/ui/ui_model.cpp:886-888`), so a test calls `seed_workspace(ws)` **then**
  `focus_workspace(ws)`. **Connection prerequisite (Rev 13, LOW-3):** the
  workspace's `connections_` entry must exist before the driver runs (the
  harness already exposes `ensure_workspace_running`/`on_scan`,
  `include/ymh/ui/supervisor_harness.hpp:39-40`), because `create_session`
  early-returns when `connections_` has no entry
  (`src/ui/supervisor.cpp:1226-1229`). **Every test that drives `submit()` or
  `dispatch_key`** (SL-U7's durable-prompt step, SL-U13, SL-U15, SL-U16) must
  establish both first; otherwise `model_.activeWorkspace()` is null (`submit()`
  `:371`, `handle_input` `:1903`), no `create_session` runs, and the test passes
  vacuously (the Rev 11 gate, MEDIUM-1).
- `submit(text)` — invokes `SupervisorApp::submit` (`src/ui/supervisor.cpp:367-392`).
  An empty `text` is a no-op (`:368-370`); a non-empty value with an empty
  `activeSessionId` reaches the create path (`:388`), otherwise the direct prompt
  path (`:391`). SL-U16 uses a non-empty value only for the post-reply prompt.
- `create_session(workspace, prompt)` — invokes the private
  `SupervisorApp::create_session` (`:1225-1259`) directly; this is the
  empty-create driver (`prompt == ""`) used by `refresh_sessions` (`:1215`),
  `new_session` (`:1425`), and `handle_input` (`:1905`).
- `run_create_reply(workspace, session, error)` — invokes the **pump-side**
  `session.create` reply handler (`:1244-1258`), extracted as a named private
  `SupervisorApp::on_create_reply(...)` (§3.8 step 1). Semantics: under
  `cleanup_mutex_` it records the returned id in `created_sessions_` and, when a
  prompt is already pending, `prompted_sessions_`, then `enqueue`s
  `apply_create_reply`; it does **not** drain. Distinct from the existing
  `apply_create_reply` seam, which is the UI-thread action.
- `confirm_exit(orphaning)` — invokes `SupervisorApp::confirm_exit`
  (`:595-609`), running `cleanup_created_sessions` as step 0 (§3.2).
- `take_delete_requests()` — returns and clears the `session.delete` requests
  cleanup issued since the previous call, each recorded as
  `(workspace, session, params)`. The recorder is a test-only observer in
  `SupervisorApp`, appended by `cleanup_created_sessions` immediately before it
  submits each delete (the `submit_to`/`SupervisorConnection::submit` boundary
  the deletes already use). Cleanup runs on the UI thread, so the recorder needs
  no extra synchronization; this is the assertion surface for "cleanup issues
  **no** `session.delete` for id X".

`dispatch_key` (already exposed) is the first-keystroke driver for SL-U16 and
requires the workspace active (above). The seams are additive; they change no
production behaviour.

### 12.2 Unit — subagent-kind fix (23-D53)

| ID | Test | Asserts |
|---|---|---|
| SL-U10 | `SubagentRunner` kind/parent | the child session row has `kind='subagent'` and `parent_session=<parent>`; **1 lease, 0 junctions** (`AgentRegistry::create` → `SessionManager::createSession` adds no `workspace_sessions` row; §4.4, L2) |
| SL-U11 | `SessionOptions` default | a default `SessionOptions` still creates a `root` with `parent_session=NULL` (no regression) |
| SL-U12 | dependent protection | after the fix, `hasDependents(parent)` is true for a spawned subagent, so the parent cannot be pruned/cleaned |

### 12.3 Unit / integration — filtering (D4)

| ID | Test | Asserts |
|---|---|---|
| SL-I1 | `host_runtime_test.cpp` — `session.list` filter | an unprompted durable root is absent; a prompted root and any fork/subagent are present |
| SL-I2 | `session_cli_test.cpp` — `ymh list` filter | an unprompted root is not printed; `ymh show <id>` still prints it |
| SL-I3 | `session_catalog_test.cpp` — `/sessions` filter | the catalog consumer omits unprompted roots; `read_workspace_history` called directly (default `include_unprompted=true`) still returns them; SW9/SW10 markers unaffected |
| SL-I4 | `ymh --resume <hidden-id>` | an unprompted durable session hidden from `/sessions`/`ymh list` still resolves through `resolve_session_workspace` and resumes |
| SL-I27 | refresh auto-create gate (Rev 8, MEDIUM-2) | attach to a workspace whose only durable sessions are unprompted roots (legacy empties): `refresh_sessions` issues **no** `session.create`, so no stray session is created; with the same setup plus `--resume <hidden-id>`, the resume succeeds and the workspace still has exactly the original session count. Falsifiable: the Rev 7 gate reads the D4-filtered `session.list` reply (empty), calls `create_session`, and leaves a stray empty |
| SL-I28 | Ctrl-S cell reconciliation (Rev 8, L1) | `/new` creates an empty (cell added), then `/new` again supersedes it; after the next `refresh_sessions` the superseded id has **no** `SessionCell` (`UiModel::workspaces[ws].sessions`), while the **active** unprompted session's cell is retained. Falsifiable: the Rev 7 code only calls `ensureCellIn` and never removes, so the superseded cell remains and renders in `SwitcherOverlayModel::open` |
| SL-I29 | failed `session.list` reply leaves cells intact (Rev 9, MEDIUM-1) | with a **non-active** `SessionCell` (carrying a non-empty input draft in its `SessionUiState`) and no registry handle (the fallback path), inject a **failed/timed-out** `session.list` reply for the workspace; after `refresh_sessions` the cell is still in `UiModel::workspaces[ws].sessions`, the draft is intact, and **no** `session.create` request is issued. Falsifiable: the Rev 8 code enqueues the reconcile unconditionally, `sessions` is empty, `UiModel::eraseSession` removes the non-active cell **and** `model_.sessions[id]` (cell gone, draft lost), and the empty-`sessions` fallback calls `create_session` |
| SL-I30 | store-only auto-create gate (Rev 9, MEDIUM-3) | seed a workspace whose store has exactly one unprompted root but with **no** `workspace_sessions` row (a pre-M2 / crash-window store-only session); attach: `refresh_sessions` issues **no** `session.create` and the workspace has exactly the original session count (the store-only entry surfaces via the scoped D4 skip). Repeat with `--resume <store-only-id>`: the resume succeeds and still no stray is created. Falsifiable: the Rev 8 junction-only gate sees zero junctions and creates a stray |

### 12.4 Unit / integration — `session prune` (D2)

| ID | Test | Asserts |
|---|---|---|
| SL-I5 | dry-run | no mutation; opens the registry with `openReadOnly`; prints `would prune N`; exit 0; row/lease/junction counts unchanged |
| SL-I6 | `--empty --yes` on a stopped workspace | deletes only unprompted roots via `SessionManager::deleteSession` (`eraseWithEvent`: `SessionEnded` + erase) with no lease; prompted roots/forks/subagents survive; for each pruned id: 0 rows, 0 leases, 0 junctions, 0 events |
| SL-I7 | junction removal (HIGH-1) | after `--empty --yes` on a stopped workspace, `WorkspaceRegistry::findSession`/`listSessions` has no row for the pruned ids; `removeWorkspace` then succeeds |
| SL-I8 | live candidate enumeration (Rev 5 M-2) | `--empty` on a live workspace selects the on-disk unprompted roots (not `session.list`), and deletes them via the daemon |
| SL-I9 | `--empty --yes` on a live workspace | issues `session.delete{confirm:true, only_if_empty:true, force:false}` per session; the active session is skipped unless `--force` |
| SL-I10 | `--keep N` | the N most-recent roots survive; the rest are selected; `--keep 0` excludes nothing; `--keep` alone exits 2 |
| SL-I11 | no inclusion flag / `--older-than` | exit 2, no mutation (v1) |
| SL-I12 | unregistered `--workspace` | exit 2, no mutation |
| SL-I13 | dependent children | a fork/subagent parent is skipped **before** `removeSession`; the junction is intact and the PTY survives; the session is not store-only; reported `has dependent sessions` |
| SL-I14 | mid-turn race | a session that becomes mid-turn between snapshot and delete is refused (`hasPendingWork`/state) and reported `skipped` |
| SL-I15 | stopped→live race | a workspace whose daemon starts after enumeration is retried over the RPC |
| SL-I16 | `--json` | a single valid JSON array; exit codes unchanged |
| SL-I17 | config gate | `session prune` runs with a missing global config (contrast `ymh list`) |
| SL-I18 | partial prune recovery | if `removeSession` fails (or the process is killed) after the junction removal but before the erase, a retry completes; no permanent orphan; `removeWorkspace` succeeds afterwards |
| SL-I19 | sweep scope | the orphan-junction sweep removes a store-absent junction in the stopped-path apply and at daemon startup between `claimHost` and `setState(Serving)`; it is **not** invoked on the live RPC path (a concurrent create's junction is never deleted) |
| SL-I20 | fresh-store sweep guard | starting a daemon whose `sessions.db` did not exist creates no junction deletions (every junction would otherwise be classified as an orphan) |
| SL-I21 | dry run writes no lock | a prune dry run creates no `sessions.lock` (liveness not probed); no logical mutation; exit 0 |

### 12.5 Unit / integration — delete order and guards (23-D55)

| ID | Test | Asserts |
|---|---|---|
| SL-I22 | guard order (the guard-order test) | deleting a session with dependent children throws `DependentSessionError` and leaves the **PTY open** and the **agent resident** (all guards precede all side effects); the junction is intact |
| SL-I23 | active-session reset | after `HostRuntime::deleteSession` removes `active_session_`, `active_session_` is empty; `host.status` reflects it |
| SL-I24 | lease-exempt delete | `SessionManager::deleteSession(id, …)` succeeds on a **non-resident** and on a **never-leased** durable session (no `LeaseLost`); `eraseWithEvent` writes a fully-stamped `SessionEnded` `Event` and removes snapshots/leases/events/row in one transaction; a session with dependent children still throws `DependentSessionError` |
| SL-I25 | `SessionEnded` is emitted | the delete publishes exactly one fully-stamped `SessionEnded{Deleted}` to `bus_` after the commit (transport observation is RB-21, not asserted here) |
| SL-I26 | PTY close | the delete closes the session PTY on the success path, after the guards |

### 12.6 PTY / live

| ID | Test | Asserts |
|---|---|---|
| SL-P1 | PTY — prompt-then-exit keeps the session | start the TUI, send one prompt, quit; the session has 1 row, 1 lease, 1 junction and a `user/message` |
| SL-P2 | PTY — no-prompt exit cleans up | start the TUI, type nothing, quit; the eagerly created session is gone: 0 rows, 0 leases, 0 junctions |
| SL-P3 | PTY — `/new` then exit | `/new` creates an empty session; quitting with no prompt removes it; the previously prompted session is unchanged |
| SL-P4 | live `session prune --empty` | against a real workspace with legacy empties, dry-run lists them and `--yes` removes exactly them (and their junction rows/leases), leaving prompted sessions |
| SL-P5 | live headless | `ymh run` in a fresh workspace exits 0 and creates one prompted session; a legacy empty in the same workspace is untouched by the run and reclaimed only by prune |

---

## 13. Out-of-scope, recorded risks, and the separate hardening effort

### 13.1 The separate hardening effort (RB-21) — NOT load-bearing for spec 23

The following defects are **real and pre-existing**. They were surfaced while
reviewing the Rev 2–5 draft; the Rev 7 rescope neither fixes nor depends on them.
They are recorded here as a **separate hardening effort** (backlog item
**RB-21**) that is **independent of spec 23** and must not be relied upon by any
requirement in this spec.

1. **`AgentLoop::dispose()` does not join the in-flight turn body.**
   `AgentLoop::dispose()` (`src/agent/agent_loop.cpp:227-242`) sets `disposed_`,
   cancels, clears `inbox_`, sets `running_ = false`, but never waits for the
   in-flight `runTurn` body to return. `AgentRegistry::dispose`
   (`src/agent/agent_registry.cpp:147-160`) then calls
   `services_.sessions->closeSession(sessionId)`, which erases the
   `std::unique_ptr<Session>` from `SessionManager::sessions_`, freeing it —
   while a `TurnExecutor` worker may still be inside `Session::append`. This is a
   **latent use-after-free** today, independent of this spec.
2. **Daemon teardown detaches workers past `shutdown_grace`.**
   `TurnExecutor::drain` (`include/ymh/agent/turn_executor.hpp:118-143`) detaches
   a still-running worker when the grace deadline passes (`:138-140`);
   `WorkspaceHost::Impl::coordinator` (`src/host/workspace_host.cpp:610-663`)
   then destroys `host_runtime_`/`runtime_` (`:693`, `:698`), which the detached
   body may still reference through captured `this`.
3. **`hasPendingWork()` is blind to the `TurnExecutor` queue.** A prompt queued
   in the `TurnExecutor` but not yet picked up by the agent is not counted, so a
   close/delete can drop a just-submitted prompt (the mid-turn guard in §5.4
   inherits this gap).
4. **`HostRuntime::handleCommittedEvent` re-reads the store.** It re-reads
   `runtime_.store().readAfter(...)` (`src/host/host_runtime.cpp:350-358`)
   instead of forwarding the bus event. For an event whose row was erased in the
   same transaction (the administrative delete's `SessionEnded`), the re-read is
   empty and the event is dropped, so `01 I16` / `05 §7.4` observation is not
   satisfied at the transport boundary.

**Consequence for spec 23:** §5.4's delete publishes `SessionEnded` after the
commit but does **not** repair the forwarding; the guards are best-effort; and
none of the above may be treated as fixed. **Rev 8 (MEDIUM-1):** #3's gap is
mitigated **only** on the cleanup path by the supervisor-side `prompted_sessions_`
exclusion (§3.3); `prune` and the daemon mid-turn guard remain blind to the
`TurnExecutor` queue and are unchanged. The fix does not repair RB-21.

### 13.2 Other recorded risks (in scope of spec 23 unless noted)

1. **Cleanup scope is supervisor-local.** A supervisor can only clean up sessions
   it created; an unprompted session created by a client that never runs a
   clean-exit cleanup (e.g. an automation that calls `session.create` and exits)
   is reclaimed only by prune.
2. **Cleanup `force:true` residual.** `force` bypasses the active-session guard.
   The safety comes from the created-set scope + `only_if_empty` + root-only +
   `hasDependents`; the exotic case (another client resumes an unprompted session
   created by this supervisor by id and activates it without prompting) is a
   recorded residual. A related **cross-supervisor** race (Rev 9): a **second
   supervisor** can `--resume` (via `session.resume`) and **prompt** one of this
   supervisor's created empties before the foreign prompt becomes durable; the
   per-supervisor `prompted_sessions_` does not cover it, so this supervisor's
   cleanup can delete the row. Once the foreign prompt is durable,
   `only_if_empty` refuses; `prune` is the backstop (§3.3#1).
3. **Best-effort mid-turn guard.** `Agent::state()`/`hasPendingWork()` are
   non-atomic reads; the guard is boundary-best-effort, not a lock. The dry run
   is the user-facing safety net.
4. **Dependent-precheck race.** A fork created between the `hasDependents`
   precheck and the erase still makes the erase throw, but the junction may
   already be gone. Recorded; the sweep repairs the junction, and prune is
   best-effort.
5. **Supervisor-local focus** is invisible to the CLI (§5.4).
6. **`--all-kinds` and `--older-than`** are deferred (23-P4/23-P5).
7. **`archiveSession`** remains unused; prune deletes.
8. **Subagent rows created before 23-D53** are mislabelled `root` and are legacy
   data; D4's hidden-not-deleted rule means they are not auto-migrated. A
   migration is out of scope.
9. **Test isolation (`XDG_STATE_HOME`)** is spec 22 §11.2's open item; SL-P1/SL-P4
   must pin it in-process.
10. **Prune of a live workspace is N round-trips** (one `session.delete` per
    session); a batch RPC is 23-P2 (rejected).
11. **Orphan-junction sweep scope** covers only **registered** workspaces
    (prune's own target set, mirroring §1.4/SW9); the sweep is limited to
    flock-held contexts (§7, SL23).
12. **Dry run is not byte-for-byte read-only.** `openReadOnly` still runs
    `PRAGMA journal_mode = WAL` and may create `-shm`/`-wal`; SL24 narrows the
    claim to "no logical mutation" and the dry run skips liveness probing.
13. **Cleanup prompted-set is in-memory and per-process (Rev 8, MEDIUM-1).** A
    crash between an accepted prompt and its durable `user/message` loses the
    `prompted_sessions_` record. The crashed run never executes cleanup, so the
    row is not deleted by it; a later supervisor run's cleanup does not know the
    id (per-process) and leaves it to `only_if_empty`/`prune`. The record does
    not survive across supervisor runs, by design (§3.3).
14. **Refresh gate existence proxy: junction set and reply (Rev 8/9,
    MEDIUM-2/3).** A stale/leaked junction for a session with no store row makes
    `refresh_sessions` skip auto-create; the error direction is "do not create",
    which is safe, and the orphan-junction sweep repairs the leak. Conversely, a
    **store-only root** is a durable row with **no** junction, produced by the
    crash window between the store commit (`src/host/host_runtime.cpp:571`) and
    the junction write (`:578`/`:643`) — the state documented at
    `include/ymh/registry/registry.hpp:91-98` — or by a pre-M2 `sessions.db` row
    (there is no startup junction backfill). Rev 8's junction-only gate therefore
    under-counted this class and could spuriously create on `--resume`. Rev 9
    fixes it by scoping the daemon's D4 skip to `entry.inJunction`, so store-only
    entries surface in the reply and the union gate counts them (§6.5, SL28). The
    residual is the deliberate D4 carve-out: a store-only root is rendered by the
    Ctrl-S live switcher (the `/sessions` catalog and `ymh list` still hide it).
15. **`active_session_` guard is untested in production (Rev 8, MEDIUM-3).** No
    production client sends `session.activate`; the guard and `force` are
    exercised only by tests (SL-U7) and external/automation clients. If
    `session.activate` were ever removed, `force` becomes inert but harmless.

---

## 14. Revision log

- **Rev 1 (2026-09-18).** Initial write. Pinned approach (a) (lazy TUI creation),
  the workspace draft composer, `/new` as an unsaved composer, the compensating
  delete, the `user/message` predicate, the list-surface filter, the hybrid
  `ymh session prune`, and the no-schema-change migration story. **Superseded by
  Rev 2.**
- **Rev 2 (2026-09-18).** Implements the user's decision that the guarantee is
  **airtight at `session.create`**: the daemon holds an in-memory draft and
  materializes it atomically on the first triggering append. Resolved the first
  gate's HIGH-1, MEDIUM-1..6, and L1..L4. **Superseded by Rev 6.**
- **Rev 3 (2026-09-18).** Resolved the second gate's H1 (headless lease only on
  the resume path), H2 (`showContext` draft-aware), H3 (lease-exempt
  `eraseWithEvent`), M1 (`active_session_` reset), and the draft-bound item.
  **Superseded by Rev 6.**
- **Rev 4 (2026-09-18).** Resolved the third gate: H1 (`createSession` performs
  no lease/junction), H2 (the draft cap refuses rather than evicts), M1
  (fully-stamped `SessionEnded`), L-1..L-3. **Superseded by Rev 6.**
- **Rev 5 (2026-09-18).** Continued the draft design and took the pre-existing
  lifetime bugs in scope: H-1 (terminal `SessionEnded` forwarding fast-path),
  H-2 (shared `shared_ptr` ownership + serialized registry maps + an
  `AgentLoop::dispose` completion barrier + a `closeSession` mid-turn draft
  guard), H-3 (`TurnExecutor::drain` joins or parks, never detach-then-destroy),
  H-4 (junction-first prune ordering + orphan-junction sweep), M-1..M-9,
  L-1..L-4. **Superseded by Rev 6** (the lifetime model is withdrawn; the
  underlying defects move to RB-21).
- **Rev 6 (2026-09-18) — THE PIVOT.** The user approved a pivot away from the
  in-memory draft: **`agent.prompt` creates the session**. Five gate rounds plus a
  three-lens parallel audit found the Rev 5 lifetime rework was **not
  converging**, with concrete, independently verified breaks: a **self-deadlock**
  in the completion latch combined with `whenIdle` deferral; an
  **unimplementable `void drain(grace)`**; a **self-cycle leak** from strong
  `shared_ptr` captures; an **underspecified "park"**; a **wrong-branch fix** for
  the `SessionEnded` forwarding; and a **close-guard blind to the `TurnExecutor`
  queue**. Pinned the `agent.prompt` create-path wire contract, the atomic birth
  transaction, the pre-committed first turn, the TUI no-session state, the
  retirement of `session.create`, the dependent-children precheck, and the
  flock-scoped orphan-junction sweep. **Superseded by Rev 7.**
- **Rev 7 (2026-09-18) — RESCOPE TO THE USER'S ACTUAL REQUIREMENT.** The user
  rejected the Rev 6 pivot outright:

  > "Let session be created as is today. Change code less. Only thing I want - if
  > user never entered any prompts in a session and exited. The session shall be
  > cleaned up/deleted from history."

  **Withdrawn (23-S6):** the entire Rev 6 pivot — `agent.prompt`-creates,
  `SessionStore::createWithFirstEvents`, the atomic birth transaction,
  `AgentLoop::startPrecommittedTurn`, the wire `session.create` retirement,
  `TransportHost::createSession` removal, the `agent.prompt` `{session,created}`
  result-type change, the TUI no-session steady state / `pending_input` composer
  / eager-create removal, the `kProtocolVersion` bump, and every
  invariant/failure-mode/test that depends on them. **The creation-gate work is
  NOT pursued.** The reason is recorded in §1.3.1: the pivot's own hazards were
  established as **non-convergent** by a 5-lens adversarial review (hyperplan)
  plus five Oracle gate rounds — a self-deadlock, an unimplementable drain, a
  cycle leak, a missing `EventBus` publish that killed the live stream, and an
  unmarshalled turn — and the user then chose the smaller requirement.

  **Pinned (Rev 7):** **D1′** unprompted-session cleanup on exit (§3) — a clean
  exit, before spec-16 deregister/teardown, deletes each unprompted root session
  *this supervisor created* via `session.delete{only_if_empty:true, force:true}`,
  best-effort, with prune as the backstop; the **creation path is frozen**
  (23-D52/SL1); **D2** `ymh session prune` is kept whole but scoped to `--empty`
  only (`--older-than` deferred, 23-D54); **D3** the subagent-kind fix is a
  pre-ship blocker (23-D53); **D4** legacy empties are hidden, not auto-deleted,
  with the filter at the `/sessions` catalog consumer (23-D31); **D5** the
  orphan-junction sweep is flock-scoped with a fresh-store guard (23-D56). All
  prior history is retained. Invariants SL1–SL26; failure modes SL-F1–SL-F24;
  tests SL-U1–SL-U12, SL-I1–SL-I26, SL-P1–SL-P5. **Not verified; no code.**
- **Rev 8 (2026-09-18) — GATE-FAIL FIX (3 MEDIUM, 0 HIGH).** The Rev 7 gate
  returned FAIL with three MEDIUM and zero HIGH; the rest of Rev 7 verified
  clean. Rev 8 fixes exactly those plus two LOW and a supervisor-bookkeeping gap,
  with **no creation-path, wire, or protocol change**:

  - **MEDIUM-1 (queued-prompt race).** `agent.prompt` is two-stage async
    (`HostRuntime::agentPrompt` → `TurnExecutor` queue → `AgentLoop::send` inbox →
    `UserMessage` appended in `runTurn`), so a just-submitted prompt can be
    invisible to both `isUnprompted` and `hasPendingWork`; cleanup's `force:true`
    delete would erase the row and drop the turn. **Fixed (23-D57):** the
    supervisor records a `prompted_sessions_` set at prompt-submission time and
    cleanup targets `created_sessions_ \ prompted_sessions_` (SL27, SL-F25,
    SL-U13).
  - **MEDIUM-2 (D4-filtered auto-create).** `refresh_sessions` decided
    auto-create from the D4-filtered `session.list` reply, so a workspace whose
    only sessions were unprompted roots got a spurious empty and
    `--resume <hidden-id>` broke. **Fixed (23-D58):** the decision comes from the
    **unfiltered registry junction set** (`WorkspaceRegistry::listSessions`);
    `session.list` keeps its exact shape and filter (SL28, SL-F26, SL-I27).
  - **MEDIUM-3 (`active_session_` false call chain).** The TUI's
    `activate_session` is supervisor-local; `HostRuntime::active_session_` is set
    only by the `session.activate` RPC, which no production client sends. The
    `force` rationale is corrected: **keep the guard, repurpose `force` as
    defence-in-depth** for external clients; SL-U7 now sends `session.activate`
    explicitly so the guard is genuinely exercised (23-D59, SL5/SL22,
    §3.5/§5.4).
  - **L1 (Ctrl-S cell).** `refresh_sessions` now reconciles the cell set against
    the filtered reply (absent removed, active exempt) via `UiModel::eraseSession`
    (§6.5, SL14, SL-I28).
  - **L2 (subagent junction).** SL-U10 corrected: a subagent has **0** junctions
    (`AgentRegistry::create` → `SessionManager::createSession` adds none; §4.4).
  - **Bookkeeping gap.** The created-set is a **distinct per-process** record
    populated only by `create_session` replies; `SupervisorConnection::track` is a
    union and is not the source; a previous-run/crash empty is not covered and
    falls to `prune` (23-D57, SL4, SL-U14).

  All prior history retained.
- **Rev 9 (2026-09-18) — GATE-FAIL FIX (3 MEDIUM, 0 HIGH).** The Rev 8 gate
  returned FAIL with three MEDIUM and zero HIGH; the rest of Rev 8 verified
  clean. Rev 9 fixes exactly those plus four LOWs, with **no creation-path, wire,
  or protocol change**:

  - **MEDIUM-1 (failed `session.list` wipes cells).** `refresh_sessions` built
    `sessions` only under `reply.ok && reply.result.is_array()`
    (`src/ui/supervisor.cpp:1181`) but `enqueue`d the reconcile unconditionally
    (`:1196`), so a timed-out reply yielded an empty vector and the Rev 8 erase
    removed every non-active cell **and** its `SessionUiState`/input draft
    (`UiModel::eraseSession` also erases `model_.sessions[id]`,
    `src/ui/ui_model.cpp:542`); the empty-list fallback could also spuriously
    create. **Fixed (23-D60):** the whole reply-driven update (cell add,
    reconcile/erase, auto-create) is gated on the same success predicate; a
    failed reply removes nothing and creates nothing (§6.5, SL14, SL-F27,
    SL-I29).
  - **MEDIUM-2 (prompted-set recording point + data race).** Rev 8 recorded
    `created_sessions_` on the connection pump thread and `prompted_sessions_` in
    the UI-thread `apply_create_reply`; a queued prompt could be overtaken (exit
    before the UI drained the reply → created known, prompted not → delete drops
    the queued turn), and the cross-thread access was an unsynchronized container
    race. **Fixed (23-D61):** one `cleanup_mutex_` guards both sets; the queued
    prompt is captured on the UI thread, and the reply handler binds the returned
    id to it and records both sets in the same critical section before
    `enqueue(apply_create_reply)` (§3.3/§3.8, SL4/SL27, SL-F28, SL-U15).
  - **MEDIUM-3 (store-only gate).** Rev 8 gated on registry junctions only, but
    `WorkspaceRegistry::listSessions` returns only `workspace_sessions` rows and
    store-only sessions (crash window / pre-M2) have none; the gate could
    spuriously create and `--resume <store-only-id>` left a stray. **Fixed
    (23-D62):** the gate is the **union** of the junction set and the reply, and
    the daemon's D4 skip is scoped to `entry.inJunction` so store-only entries
    surface — no wire change (§6.1/§6.5, SL28, SL-F29, SL-I30).
  - **LOW (cross-supervisor).** §3.3#1's "removes that class entirely" is
    softened: a second supervisor can resume+prompt an empty this supervisor
    created before the foreign prompt is durable; recorded as a residual
    (§3.3/§13.2#2).
  - **LOW (§6.5 citation).** `workspace_spec_from_registry` is called from
    `ensure_worker_loop` on the ensure-worker thread (`src/ui/supervisor.cpp:363`,
    call at `:941`), not on the supervisor thread; and `apply_resume_success` is
    `src/ui/supervisor.cpp:1005-1016`, not `ui_model.cpp` (§6.5).
  - **LOW (§13.2#14 attribution).** Store-only comes from the store-commit /
    junction-write crash window (`include/ymh/registry/registry.hpp:91-98`) and
    pre-M2 rows, not "the defect the dependent precheck closes" (§13.2#14).
  - **LOW (§2.1 heading).** "Current normative rows (Rev 7)" → "(Rev 7–Rev 9)".
  - **LOW (SL-I13 typo).** "the junction and PTY survive" → "the junction is
    intact and the PTY survives".

  All prior history retained. Invariants SL1–SL28 (`SL27`–`SL28` from Rev 8; Rev 9
  amends `SL4`/`SL14`/`SL27`/`SL28`); failure modes SL-F1–SL-F29 (`SL-F27`–`SL-F29`
  new); tests SL-U1–SL-U15, SL-I1–SL-I30, SL-P1–SL-P5. **Not verified; no code.**
- **Rev 10 (2026-09-18) — GATE-FAIL FIX (1 MEDIUM, 0 HIGH).** The Rev 9 gate
  returned FAIL with exactly one MEDIUM plus two LOWs; MEDIUM-1, MEDIUM-3, and
  all other LOWs verified resolved and are not touched. Rev 10 fixes exactly
  those, with **no creation-path, wire, or protocol change**:

  - **MEDIUM-2 (remaining ordering: prompt queued after the create reply but
    before `apply_create_reply` is drained).** Rev 9 recorded both
    `created_sessions_` and `prompted_sessions_` in the reply handler only when
    the prompt was already captured **before** the reply. But `create_session`'s
    dedup branch (`src/ui/supervisor.cpp:1230-1236`) also stores a prompt that
    arrives **after** the reply; the reply handler had already read an empty
    `pending_creates_` and recorded `created_sessions_` alone, and
    `apply_create_reply` (`:1265-1299`) — which reads/erases `pending_creates_`
    and only then calls `prompt()` (`:1293-1295`) — has not run until `drain()`
    executes on the next `Event::Custom` (`:2115-2116`). So an exit key processed
    before that drain could delete the created id and drop the accepted prompt.
    **Fixed (23-D63):** `cleanup_created_sessions` snapshots `pending_creates_`
    in the same `cleanup_mutex_` critical section as the other two records and
    **skips every created id whose workspace has a non-empty pending entry**; the
    §3.3 invariant is corrected to this actual condition (§3.1/§3.3/§3.7/§3.8,
    SL4/SL27, SL-F28, SL-U16). The test mirrors SL-U15 with the reply handler run
    **before** `submit()`, then `confirm_exit` **before** drain.
  - **LOW-1 (SL14 absolute claim).** SL14's first sentence ("no §6.1 surface
    returns or renders an unprompted junction-backed root") contradicted the
    active-session exemption: `SwitcherOverlayModel::open` renders every
    `workspace.sessions` cell (`src/ui/ui_model.cpp:924`) and the Ctrl-S
    reconcile exempts the active session, so an **active** unprompted
    junction-backed root does render. Qualified with "except the active session"
    (§9).
  - **LOW-2 (reply-execution thread citation).** 23-D61 and §3.3 cited
    `SupervisorConnection::submit`'s range in `src/ui/supervisor_connection.cpp`
    as where the `session.create` reply runs; `submit` only queues the request.
    The reply callback is invoked in
    `SupervisorConnection::process_requests` at
    `src/ui/supervisor_connection.cpp:362` (function at `:339`, called from
    `pump()` at `:443`). Corrected.

  All prior history retained. Invariants SL1–SL28 (Rev 10 amends
  `SL4`/`SL14`/`SL27`); failure modes SL-F1–SL-F29 (Rev 10 amends `SL-F28`);
  tests SL-U1–SL-U16 (SL-U16 new), SL-I1–SL-I30, SL-P1–SL-P5. **Not verified; no
  code.**
- **Rev 11 (2026-09-18) — GATE-FAIL FIX (1 MEDIUM, 0 HIGH).** The Rev 10 gate
  confirmed the Rev 10 design fix **sound** — it independently walked every
  interleaving (reply-not-arrived, prompt-before-reply,
  prompt-after-reply-before-drain, and after `apply_create_reply` drains) and
  found no ordering that leaves an accepted prompt's id in `created_sessions_`
  with all three records empty; all mutex access sites are covered; the
  workspace-level skip can only *retain* — and returned exactly one MEDIUM plus
  three LOWs, all spec-internal. Rev 11 fixes exactly those, with **no
  creation-path, wire, protocol, or TUI-state-machine change**:

  - **MEDIUM-1 (SL-U16's first driver is a no-op; the test is vacuous).** Rev 10
    drove SL-U16 by calling `submit()` with an empty `activeSessionId` **and
    empty text** "so `create_session` sends the create with
    `pending_creates_[ws]=""`". But `SupervisorApp::submit` returns immediately
    on empty text (`src/ui/supervisor.cpp:368-370`) and never reaches its create
    call (`:388`). The spec's own §1.1 and §3.3 already say the empty-text eager
    creates are `refresh_sessions` (`:1215`), `new_session` (`:1425`), and
    `handle_input`'s first keystroke (`:1905`), and that the `submit()` create
    path is for **non-empty** text only. Implemented literally, the test created
    no session, ran no reply, left `created_sessions_` empty, and cleanup issued
    no delete — so it **passed without the `pending_creates_` snapshot**.
    **Fixed:** SL-U16's first step is now a real production empty-create —
    dispatch a printable key through `handle_input` (`:1900-1907`, create at
    `:1905`), or `new_session` (`:1425`), or the harness `create_session(ws,"")`
    seam — then the pump-side reply handler, then the non-empty `submit()`, then
    `confirm_exit` before any drain. The rest of the sequence and the
    falsifiability assertion are unchanged (§12.1, SL-U16).
  - **LOW-2 (pin the mutex as leaf-only).** `cleanup_mutex_` must never be held
    across `submit`, the reply condition-variable wait, or `connection->stop()`;
    it guards container operations only. Stated explicitly in §3.3, §3.8, and
    SL4.
  - **LOW-3 (SL-U15/SL-U16 harness seams unpinned).** `SupervisorHarness`
    (`include/ymh/ui/supervisor_harness.hpp:34-92`; impl
    `src/ui/supervisor.cpp:2285-2417`) exposed no submit/create/reply-handler/
    confirm/delete-observer seam and §12 named no file to extend. §12.1 now pins
    the added surface and its semantics: `submit`, `create_session`,
    `run_create_reply` (the extracted pump-side `on_create_reply`), `confirm_exit`,
    and `take_delete_requests`; `dispatch_key` is the first-keystroke driver.
  - **LOW-4 (§3.1 reads the live map).** §3.1's `if pending_creates_[W] is empty`
    is corrected to read the §3.3/§3.8 **snapshot** taken under `cleanup_mutex_`,
    never the live map.

  All prior history retained. Invariants SL1–SL28 (Rev 11 pins `SL4` leaf-only);
  failure modes SL-F1–SL-F29 (unchanged); tests SL-U1–SL-U16 (SL-U16 driver
  corrected), SL-I1–SL-I30, SL-P1–SL-P5. **Not verified; no code.**
- **Rev 12 (2026-09-18) — GATE-FAIL FIX (1 MEDIUM, 0 HIGH; 2 LOW).** The Rev 11
  gate confirmed the production design **correct and reachable end-to-end** —
  the deletion-gap design, the `pending_creates_` snapshot, and the production
  empty-create path (`dispatch_key` → `handle_input` no-state →
  `create_session(ws,"")` at `src/ui/supervisor.cpp:1905`) — and returned
  exactly one MEDIUM plus two LOWs, all spec-internal. Rev 12 fixes exactly
  those, with **no creation-path, wire, protocol, or TUI-state-machine change**:

  - **MEDIUM-1 (the pinned harness surface cannot set the active workspace).**
    Both pinned drivers require `model_.activeWorkspaceId == ws` —
    `handle_input` reads `model_.activeWorkspace()` (`:1903`) and `submit()`
    reads it (`:371`) — but `activeWorkspaceId` is set only in `run()` (`:343`),
    which the harness never calls (`SupervisorHarnessImpl` ctor `:2287-2291`
    only spawns the ensure worker); `seed_workspace` (`:2349-2351`) inserts into
    `model_.workspaces` but does not set it, and `model()` is const. Followed
    literally, `dispatch_key`/`new_session`/`submit()` no-op,
    `pending_creates_[ws]` stays empty, and SL-U16 either fails or passes
    vacuously; the same prerequisite silently affected SL-U13/SL-U15/SL-U7.
    **Fixed (option (a)):** §12.1 pins a `focus_workspace(workspace)` seam
    invoking `UiModel::focusWorkspace` (`include/ymh/ui/ui_model.hpp:474`;
    `src/ui/ui_model.cpp:885-891`), and every test driving `submit()`/
    `dispatch_key` must `seed_workspace(ws)` then `focus_workspace(ws)` first;
    SL-U13/SL-U15/SL-U16 (and SL-U7's durable-prompt step) now state the
    prerequisite explicitly (§12.1). This keeps the pinned production
    empty-create driver (the `dispatch_key` → `handle_input` path the gate
    confirmed reachable) rather than substituting the `create_session(ws,"text")`
    seam pair of option (b).
  - **LOW-1 (§12.1's harness inventory parenthetical).** "it has
    `apply_create_reply`, `drain_actions`, and `dispatch_key` only" was
    factually wrong — the header exposes ~20 methods. Reworded to "of the seams
    these tests need, it exposes only …", noting the unrelated seeds/observers
    (§12.1).
  - **LOW-2 (§3.8 step 1's extraction omits `track()`).** The shipped
    `session.create` reply handler calls `connection->track(SessionId{session})`
    (`src/ui/supervisor.cpp:1249-1253`) before `enqueue`; the extraction
    pseudocode omitted it. §3.8 step 1 now states the `on_create_reply`
    extraction preserves `track()` verbatim (it sets `subscribe_pending_`,
    `src/ui/supervisor_connection.cpp:61-70`), with the new inserts added
    alongside (§3.8).

  All prior history retained. Invariants SL1–SL28 (unchanged); failure modes
  SL-F1–SL-F29 (unchanged); tests SL-U1–SL-U16 (SL-U13/SL-U15/SL-U16
  preconditions made explicit; no new test), SL-I1–SL-I30, SL-P1–SL-P5. **Not
  verified; no code.**
- **Rev 13 (2026-09-18) — GATE PASS + 4 LOW wording/completeness fixes.** The
  Rev 12 adversarial Oracle gate **passed**: it verified the spec against the
  real code (including that SL-U16 is genuinely falsifiable and that the frozen
  creation path is not re-introduced normatively) and returned **zero HIGH, zero
  MEDIUM, four LOW**, all wording/completeness. Rev 13 fixes exactly those four,
  with **no creation-path, wire, protocol, or TUI-state-machine change**:

  - **LOW-1 (false active-workspace justification).** The claim that
    "`activeWorkspaceId` is otherwise set only in `run()`" (23-D65, §12.1) is
    false: the harness-exposed `apply_resume_success`
    (`src/ui/supervisor.cpp:1005-1016`) → `UiModel::focusSession`
    (`src/ui/ui_model.cpp:893-908`) also sets it (`:903`). Reworded to "no
    harness-reachable path sets `activeWorkspaceId` without also setting
    `activeSessionId`" (`run()` sets it alone but is unreachable); the
    conclusion — a dedicated `focus_workspace` seam is needed — is unchanged.
  - **LOW-2 (SL-U13/SL-U7 do not seed `activeSessionId`).** With only
    `focus_workspace`, `activeSessionId` stays empty, so `submit()` takes the
    create path (`:388`) and `prompted_sessions_` is never recorded. SL-U13 and
    SL-U7 now seed a workspace whose `activeSessionId` is the known id (direct
    `prompt()` path `:391`).
  - **LOW-3 (connection prerequisite unpinned).** `create_session` early-returns
    when `connections_` has no entry (`:1226-1229`), so §12.1 now pins
    establishing the workspace's connection (via the already-exposed
    `ensure_workspace_running`/`on_scan`,
    `include/ymh/ui/supervisor_harness.hpp:39-40`) before `seed_workspace`/
    `focus_workspace`; otherwise SL-U13/SL-U15/SL-U16 are vacuous.
  - **LOW-4 (`track()`/`cleanup_mutex_` boundary).** §3.8 step 1 now states the
    extracted `on_create_reply` performs its container inserts under
    `cleanup_mutex_`, releases it, then calls `connection->track(...)` (which
    takes `SupervisorConnection::mutex_`) and `enqueue`, preserving the
    leaf-only rule.

  All prior history retained. Invariants SL1–SL28 (unchanged); failure modes
  SL-F1–SL-F29 (unchanged); tests SL-U1–SL-U16 (SL-U13/SL-U7 seed
  `activeSessionId`; SL-U16 driver unchanged), SL-I1–SL-I30, SL-P1–SL-P5.
  **Verified** (adversarial Oracle gate, PASS — zero open HIGH/MEDIUM); the
  scope is the user's frozen-creation-path requirement: cleanup-on-exit of
  unprompted sessions, `ymh session prune --empty`, the subagent-kind pre-ship
  fix, the legacy-empty filters, the guard-order fix, and the flock-scoped
  orphan sweep.
