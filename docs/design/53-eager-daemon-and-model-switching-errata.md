# 53 — Eager Daemon Creation and Mid-Flight Model Switching

```
Status: **verified (Rev 2)** — the five-reviewer adversarial gate PASSED (0 open
        HIGH / 0 MEDIUM); implementation may proceed (AGENTS.md, the rule).
Component: 53 (errata) — supersedes 49's lazy-spawn decision (49-D1/49-D2) and
            restores eager cwd daemon creation at supervisor start; adds the
            `/model` picker and the mid-flight `session.set_model` RPC.
Depends on: 00-architecture.md §54 (F1–F12), §44 (test strategy), §45 (Fake LLM);
            08-llm-provider.md (verified) §5.2 (L11, the explicit-model rule);
            23-session-lifecycle-errata.md (verified) §3 (D1′) / §6.2 (D31, the
            unprompted-root filter);
            16-daemon-ownership.md (verified) §2.3/§2.5/§2.6/§3.2.1/§4/§8.1
            (O1–O22); 22-switcher-sessions-errata.md (verified) §3/§5/§11.4;
            25-ui-ux-errata.md (verified) §3.2 (25-D1) / §3.3 (25-D2, the
            `session.set_mode` template); 45-ui-interaction-errata.md (verified)
            §4/§7/§11 (45-D2/45-D5/45-D9); 10-supervisor-tui.md (verified) §6/§7;
            52-endpoints-models-and-dsh-agent-presets.md (verified) §3
            (52-D1/52-D3, 52-I1–I4); 49-switcher-single-workspace.md (draft) §3
            (49-D1/49-D2).
Supersedes: 49-D1 (lazy workspace/daemon creation on the first prompt) and 49-D2
            (the empty state as the normal bare-`ymh` start) — both replaced by
            53-D1. Re-supersedes **16-D2's bare-cwd lazy-spawn policy** (which
            49-A1/49-A11 had reinstated) for the bare-cwd case only, restoring
            22 §11.4 / 22-A7's eager initial spawn. 22-A7's S3 scope/trigger
            extension (spawn may target a non-cwd workspace; `session.resume` is
            a spawn trigger) is **retained**.
Amends:     25-D1's `active == nullptr` status-line clause (53-D3); 52 §1.4's
            "UI out of scope — 10/25 own `/model`" (53-D4 now owns the picker);
            the transport method table and `TransportHost`/`HostRuntime`
            (53-D5/D7, mirroring 25-D5's `session.set_mode`); `01-session.md`'s
            durable event table (53-D6); **08 §5.2 (L11) / `01-session.md` §3** —
            `SessionHeader.model` immutability (53-D6/A12); 49-I1/I2/I3/I5 and
            49-F1/F4 (53-A1–A4). All recorded in §7.
Retained:   16 O1–O22 (ownership/teardown); the daemon's one-time `chdir`; the
            supervisor's read-only `sessions.db`; 45-D2/45-D5/45-D9 and 22-A2
            picker conventions; 25-D2's `session.set_mode`; 46-I13/23-D58/50-D2.1's
            attach auto-create (relied on, not amended); 52 Part A's schema (the
            `/model` list source).
Scope:       two halves: **(A)** eager cwd workspace/daemon creation at supervisor
            start (53-D1–D3), and **(B)** `/model` selection + the mid-flight
            `session.set_model` RPC with a durable event (53-D4–D8).
Verification status: **DRAFT (Rev 2) — not yet reviewed.**
```

---

## 1. Purpose, scope, and the user decisions

### 1.1 The original request (verbatim)

> "rework how a daemon created. It has to be created on start. Because now on start
> the bottom line does not show model at all and /model command does not work. So on
> start daemon shall be created and bottom line should show model name etc. Also
> /model does not show list of available models. It should be possible to pick from
> the list. Also it seems daemon does not accept change of model mid flight. It
> should be possible."

Decoded into four requirements:

| # | Requirement | Half |
|---|---|---|
| R1 | The daemon is created **on start**, not lazily | A |
| R2 | The bottom status line shows the **model name** (and the usual status content) from the first frame | A |
| R3 | `/model` **lists** the available models and lets the user **pick** one | B |
| R4 | The daemon accepts a **model change mid-flight** (while it owns a live session) | B |

### 1.2 What this changes in one sentence

A bare `ymh` eagerly registers the cwd workspace and spawns/attaches its daemon
before the TUI loop (reverting spec 49's lazy policy), which makes the resolved
model a live, displayable session property from the first frame; `/model` becomes a
keyboard picker over spec 52's named models, and the selected model is applied to
the live session through a new durable, event-sourced `session.set_model` RPC that
queues behind an open turn.

### 1.3 Supersession map

| ID | Spec / decision | What 53 does | Why |
|---|---|---|---|
| 53-A1 | 49-D1 (lazy workspace/daemon creation), 49-I1, 49-I3, 49-A2 | **Supersedes.** The bare-cwd case is eager again: the row and daemon exist before the loop. | R1/R2. |
| 53-A2 | 49-D2 (the empty state), 49-I2 | **Supersedes as the normal start state.** The empty screen survives only as the degraded fallback when the eager spawn fails. | R1/R2. |
| 53-A3 | 49-I5 (a zero-daemon supervisor is valid) | **Amends.** Still representable, but no longer the normal bare-`ymh` state; it is the 53-F1 fallback. | R1. |
| 53-A4 | 49-F1/F4 | **Amends.** The first-prompt spawn failure is no longer the primary failure path; the startup spawn failure is (53-F1). | R1. |
| 53-A5 | 16-D2's bare-cwd lazy-spawn policy (`16:529-551`), reinstated by 49-A1/49-A11 | **Re-supersedes for the bare-cwd case only**, restoring 22 §11.4/22-A7's eager initial spawn. 22-A7's S3 scope/trigger extension is retained. | R1. |
| 53-A6 | 25-D1 (`25:324-384`) `active == nullptr` clause | **Amends.** When a workspace is attached but no session is active, the left segment renders the resolved model instead of nothing (53-D3). | R2. |
| 53-A7 | 52 §1.4 (`52:121-143`) "any UI surface … out of scope (spec 10/25 own `/model`, …)" | **Amends the ownership assignment**: 53 owns the `/model` picker; 52 Part A remains the schema/source of the list. | R3. |
| 53-A8 | 01-session.md durable event table | **Adds** `SessionModelChanged` (`session/model`). | R4. |
| 53-A9 | 05-transport method table + `TransportHost`/`HostRuntime` (mirroring 25-D5's `session.set_mode`); 49-D10's "no new wire surface" (`49:408`) | **Adds** `session.set_model`. | R4. |
| 53-A10 | 45-D2/45-D5/45-D9, 22-A2 | **Retained (not amended).** The picker reuses the established keys (arrows/`j`/`k`, Enter, Esc/Ctrl+C). | Consistency. |
| 53-A11 | 46-I13 / 23-D58 / 50-D2.1 (attach auto-create) | **Retained (not amended).** 53 relies on it: the eager attach creates the first session. | R2. |
| 53-A12 | 08-llm-provider.md §5.2 (L11) (`08:687-689`, `:1027-1029`) | **Amends.** `SessionHeader.model` becomes foldable (the `title` precedent) and is materialized on `SessionModelChanged`. | R4. |
| 53-A13 | 23-D31 (`23:361`), 23-D1′ (`23:399`) | **Retained (not amended).** The unprompted-root catalog filter hides the 53-D2 session; 23-D1′'s clean-exit deletion is the intended disk cleanup but is **not yet shipped** (no `created_sessions_` in `src/ui/supervisor.cpp`). | R2. |

### 1.4 What this spec does **not** change

- 16 O1–O22, the `supervisors`/`workspaces` registry schema, the last-exit prompt,
  and the daemon-side owner watchdog — unchanged. Eager spawn changes *when* the
  daemon is created, not *who owns it*.
- The daemon's one-time `chdir` to its workspace root — unchanged; the supervisor
  never chdirs.
- The supervisor's read-only `sessions.db` handle (`src/ui/supervisor.cpp:1901`) —
  unchanged; every write goes through the owning daemon.
- 52 Part A's `llm.endpoints`/`llm.models`/`llm.active_model` schema and
  `resolve_model` — unchanged; they are the list source, not modified.
- 25-D2's `session.set_mode` — unchanged; it is the template for `session.set_model`.
- 45-D2/45-D5 (command list) and 22-A2 (switcher keys) — unchanged; reused.
- `10-supervisor-tui.md` §6 (the aggregate/status line) and §7 (the switcher) are
  **not directly amended**: the status line is owned by 25-D1 (which 53-D3 amends)
  and the switcher keys by 22-A2/45; 53 adds no new switcher node type and no new
  aggregate counter.

---

## 2. Current state (verified against the shipped tree)

### 2.1 The bare `ymh` start is lazy (spec 49)

`run_supervisor_entry` (`src/cli/cli.cpp:468`) computes
`explicit_activation = !resume_session.empty() || new_session` (`src/cli/cli.cpp:504`)
and registers/attaches the cwd workspace **only** for an explicit activation
(`src/cli/cli.cpp:500-533`):

```cpp
// src/cli/cli.cpp:500-533
// 49-D1/49-A1/49-A2: a bare `ymh` (no --resume/--new) starts with zero
// workspaces and spawns no daemon; the cwd row and daemon are created on the
// first prompt. `--resume`/`--new` are explicit activations and keep the
// eager register+attach.
const bool                     explicit_activation = !resume_session.empty() || new_session;
std::optional<WorkspaceRecord> row;
if (explicit_activation) {
    row = find_or_register_workspace(*canonical, err);
    if (!row.has_value()) {
        return 1;
    }
}
// … open registry …
if (row.has_value()) {
    const AttachResult attach = lifecycle.ensureRunning(row->id, identity);  // :528
    (void)attach;
}
```

The comment is explicit: *"49-D1/49-A1/49-A2: a bare `ymh` (no --resume/--new)
starts with zero workspaces and spawns no daemon"* (`src/cli/cli.cpp:500-503`).

`SupervisorApp::run()` then tolerates zero workspaces (`src/ui/supervisor.cpp:405-415`):

```cpp
// src/ui/supervisor.cpp:405-415
for (const SupervisorWorkspace& workspace : options_.workspaces) {
    attach_workspace(workspace);
}
// 49-D1/49-A3: zero workspaces is a valid start state (the empty screen).
if (!model_.workspaces.empty()) {
    model_.activeWorkspaceId = options_.workspaces.front().id;
    …
}
```

and `submit()` carries the lazy first-prompt path (`src/ui/supervisor.cpp:436-465`):
resolve/register the cwd row, queue the draft in `pending_creates_`, then
`ensure_workspace_running(*id, SessionId{})` (`src/ui/supervisor.cpp:456-464`).

> **Design-first note (verified, must be recorded).** The lazy behavior is present
> in the working tree, introduced by commit `f91ef1b68` *"ui: lazy workspace
> creation (spec 49)"* and expanded by `b74dfc433` *"WIP: in-flight specs 49/50 -
> NOT green (516/1867 failing), preserved for re-planning"*. `DESIGN_STATUS.md`
> still lists spec 49 as **draft (Rev 3) — not yet reviewed**, i.e. the lazy
> behavior shipped ahead of its gate. This spec supersedes the decision regardless;
> whether the pre-existing 49 code should be reverted or amended is an
> implementation choice, not a product decision (§10.1 implementation note).

### 2.2 The empty start renders no model (R2)

With zero workspaces, `build_ui` finds no active session (`src/ui/ui_render.cpp:1319-1333`)
and calls `render_status(model, /*active=*/nullptr, …)` (`src/ui/ui_render.cpp:1369`).
The `active == nullptr` branch renders only a left string and the aggregate
(`src/ui/ui_render.cpp:610-624`):

```cpp
// src/ui/ui_render.cpp:613-623
if (notice.empty()) {
    if (model.workspaces.empty()) {
        // 49-D2: the empty screen's left segment prompts for the first prompt…
        return ftxui::hbox(
            {ftxui::text("no workspace attached — type a prompt to start") | ftxui::dim,
             ftxui::filler(), aggregate});
    }
    return ftxui::hbox({ftxui::text(""), ftxui::filler(), aggregate});
}
```

The model segment (segment 3) is only rendered when an active session exists
(`src/ui/ui_render.cpp:640-647`, `:721-722`) and only when `status.model` is
non-empty. `StatusModel.model` (`include/ymh/ui/ui_model.hpp:224`) is populated from
`effective_model(options_.config)` on session creation/listing
(`src/ui/supervisor.cpp:1544-1546`, `:1653-1654`) and from the stored header on
resume (`src/ui/supervisor.cpp:1281-1282`). **With no session there is nothing to
render.** This is exactly R2.

### 2.3 `/model` exists but does not list or switch (R3)

The `/model` command is registered in `CommandRegistry::builtin()`
(`src/ui/command_registry.cpp:174-194`):

```cpp
// src/ui/command_registry.cpp:174-194
registry.add(Command{
    "model", "show or set the model (applies to new sessions)",
    [](CommandContext& context, const std::string& args) {
        if (args.empty()) {
            const std::string current =
                context.session == nullptr ? std::string{} : context.session->status.model;
            append_system(context, "model: " + (current.empty() ? std::string{"(default)"} : current));
            return;
        }
        if (context.set_model) {
            context.set_model(args);
        }
        if (context.session != nullptr) {
            context.session->status.model = args;
            context.model.dirty.mark(context.session->id, UiDirtyFlag::Status);
        }
        append_system(context,
                      "model set to " + args +
                          " (the daemon applies it at session.create; there is no "
                          "live model switch)");
    }});
```

- `/model` with **no args** prints the current `status.model` or `(default)` — it
  does **not** list the configured models.
- `/model <name>` calls `context.set_model`, which only records a **preference**:
  `context.set_model = [this](const std::string& name) { preferred_model_ = name; };`
  (`src/ui/supervisor.cpp:2260`). `preferred_model_` is consumed only by
  `create_session` (`src/ui/supervisor.cpp:1602-1604`).
- The command's own text states the gap: *"the daemon applies it at session.create;
  there is no live model switch"* (`src/ui/command_registry.cpp:191-193`).
- There is **no validation** of the argument against `config.llm.models`, and no
  picker overlay. `CommandContext` has a single `set_model` callback
  (`include/ymh/ui/command_registry.hpp:23`).
- **This is the "does not work" the user saw.** `/model <name>` mutates
  `status.model` **optimistically** (`src/ui/command_registry.cpp:186-189`) while
  the daemon does nothing: the value changes on screen but no session is
  re-modeled and no subsequent request changes. The optimistic write is removed
  by 53-D8.

### 2.4 The daemon does not accept a mid-flight model change (R4)

- `AgentConfig.model` is baked once at daemon startup by `to_agent_config`
  (`src/cli/wiring.cpp:229-234`) and copied into the `AgentLoop`
  (`include/ymh/agent/agent.hpp:109-126`). `AgentLoop::buildRequest` reads
  `config_.model` on every step (`src/agent/agent_loop.cpp:537`, `:552`), but there
  is **no public setter**: the only accessor is
  `const AgentConfig& config() const` (`include/ymh/agent/agent_loop.hpp:134`).
- `SessionHeader.model` is written at create (`src/session/session_manager.cpp:61`)
  and appended as `payload::SessionStarted{model, …}` (`src/session/session_manager.cpp:74`,
  `include/ymh/session/events.hpp:33-37`); the provider "never re-resolves"
  (`include/ymh/llm/llm_provider.hpp:88-89`).
- The only mid-session composition change is the **agent preset**
  (`AgentPresetRoster::select`, `src/agent/preset.cpp:857-873`), and it **fails
  closed** after the first message: `CompositionFixed` ("session composition is
  fixed after the first message", `src/agent/preset.cpp:861-862`). There is no
  model analogue.
- The daemon registers **one** provider route from the single resolved endpoint
  (`src/agent/workspace_runtime.cpp:374-390`; `to_provider_config`,
  `src/cli/wiring.cpp:86-100`), so it currently cannot route a model bound to a
  different endpoint (see §3 53-D7's endpoint restriction / OQ-53-3).
- **08 §5.2 pins the immutability that R4 contradicts:** *"`SessionHeader.model`
  is immutable (`01 §3`); switching models means a new session (or an explicit
  per-request override for auxiliary calls such as compaction), **not a header
  mutation**"* (`08:687-689`; L11 at `08:1027-1029`). 53's half B **amends** that
  clause (53-A12): `model` becomes foldable, following the `title` precedent.

### 2.5 The templates that already exist (verified)

- **Mid-flight change with pending semantics.** `session.set_mode`
  (`include/ymh/transport/protocol.hpp:534`) → `ProtocolServer`
  (`src/transport/protocol_server.cpp:406-412`) → `HostRuntime::setSessionMode`
  (`src/host/host_runtime.cpp:727-751`): `turn_open = agentStatus(id) != "Idle"`
  (`:743`), `PlanModeSetResult` (`:745`), reply `SetModeResult{session, active,
  pending}` (`:750`; `include/ymh/transport/host.hpp:65-72`). The controller
  (`include/ymh/agent/plan_mode_controller.hpp`) appends when idle, queues when a
  turn is open, and applies the pending value at step start / turn end
  (`src/agent/agent_loop.cpp:854-856`, `:951-953`, `:68`).
- **Durable per-session state as an event.** `payload::PlanMode`
  (`include/ymh/session/events.hpp:249-251`) with `EventType::PlanMode`
  (`include/ymh/core/event.hpp:74`), `wire_name "plan/mode"`
  (`src/core/event.cpp:41`), projection-invisible to `deriveMessages`
  (`src/session/session.cpp:393-398`), delivered to the UI as `PlanModeChanged`
  (`src/ui/ui_event_adapter.cpp:160-163`) → `status.plan_active`
  (`src/ui/ui_model.cpp:1071-1074`). 25-D2 (`25:385-…`) pins the whole pattern.
- **Header materialization in the same transaction.** `SessionRenamed` is
  materialized into `sessions.title` inside the append transaction
  (`src/session/session_persistence.cpp:611-616`) and folded into `header_.title`
  (`src/session/session.cpp:625-626`); resume/fork restore it by folding the log
  (`src/session/session.cpp:569-570`, `:684-685`).
- **Attach auto-create.** On `Attached`, `refresh_sessions`
  (`src/ui/supervisor.cpp:1479`, `:1511`) auto-creates the first session when the
  active workspace has no live session (`src/ui/supervisor.cpp:1556-1564`),
  per 46-I13/23-D58/50-D2.1.
- **The `/sessions` catalog hides unprompted root sessions**
  (`src/ui/session_catalog.cpp:138-142`, 23 §6.1/23-D31), so an auto-created empty
  session is **not** listed by `/sessions`.

### 2.6 The named-model config already exists (verified)

`llm.endpoints.<name>` / `llm.models.<name>` / `llm.active_model` are shipped
(`include/ymh/config/config.hpp:182-196`, `:200-214`, `:246-249`), resolved by
`resolve_model` (`src/config/config.cpp:2364-2387`) into `ResolvedModel`
(`include/ymh/config/config.hpp:489-508`); `effective_model` is a thin wrapper
returning `model_id` (`src/config/config.cpp:2272-2274`). The schema, precedence
(`agent.model` → `llm.active_model` → `llm.model` → builtin), name grammar, and
failure modes are pinned by 52-D1/52-D3 and 52-I1–I4 (`52:351-…`, `:435-…`,
`:647-…`). **The list source for `/model` therefore needs no new config.**

### 2.7 Verdict

| Requirement | Current state | Deciding evidence |
|---|---|---|
| R1 daemon on start | **Not met** (lazy) | `src/cli/cli.cpp:500-533`; `src/ui/supervisor.cpp:405-415` |
| R2 model on the bottom line | **Not met** (no session ⇒ no segment) | `src/ui/ui_render.cpp:613-623`, `:640-647` |
| R3 `/model` lists + picks | **Not met** (prints one value; no list/picker) | `src/ui/command_registry.cpp:174-194` |
| R4 mid-flight switch | **Not met** (no setter; `CompositionFixed` precedent only) | `src/agent/agent_loop.hpp:134`; `src/agent/preset.cpp:861-862` |

---

## 3. Decision (53-D)

### 53-D1 — Eager cwd workspace and daemon creation on start (R1)

A bare `ymh` (no `--resume`, no `--new`) **registers the cwd workspace row and
spawns/attaches its daemon before the TUI loop**, reverting 49-D1 for the bare-cwd
case. `run_supervisor_entry` (`src/cli/cli.cpp:468`):

1. Canonicalize the root (unchanged; `src/cli/cli.cpp:490-496`).
2. `find_or_register_workspace(*canonical, err)` **unconditionally** (not gated on
   `explicit_activation`). A registry/canonicalize failure stays fatal (exit 1),
   as today (`src/cli/cli.cpp:478-499`).
3. `lifecycle.ensureRunning(row->id, identity)` **unconditionally**.
4. **Failure semantics differ by mode** (the shared `if (row.has_value())` path
   currently returns 1 on attach failure, `src/cli/cli.cpp:526-533`):
   - **Bare `ymh`** (no `--resume`/`--new`): do **not** exit. Record the error as
     an initial notice and continue with the **cwd workspace unmodeled**; the lazy
     `submit()` path (`src/ui/supervisor.cpp:436-465`) remains the retry fallback
     (53-F1).
   - **Explicit activation** (`--resume`/`--new`): fatal, exit 1, as today.
5. `DaemonSetScanner::scanOnce()` (`src/cli/cli.cpp:536-551`) then seeds
   `options.workspaces` with **every** live daemon the registry reports (not just
   the cwd one), so `SupervisorApp::run()` attaches it exactly as it does for
   `--new`/`--resume` today.

`SupervisorApp::run()` keeps its zero-workspace branch, but only as the **degraded
fallback** (`SupervisorRunOptions::initial_notice`, 53-D3). The
`SupervisorApp::submit()` lazy path is retained for the fallback and for a
workspace that is not running (e.g. after 53-F2).

**Justification.** R1 and R2 are the user's explicit request. The daemon is the
component that owns the resolved model, the session, and every write; without it
there is no live session and therefore no model to display, no session to switch,
and no writer to accept the switch. Eager creation is the smallest change that
makes R1/R2/R4 coherent.

**What lazy spawn was protecting against, stated honestly (what is lost).** Spec 49
chose lazy creation deliberately (`49:210-275`, `49:676-693`) and its rationale is
recorded as 49 §10.1. This spec gives up all four of its protections:

1. **No daemon before the first prompt.** A bare `ymh` now forks a daemon process
   at launch even if the user never types anything. That process is idle but
   resident until the supervisor exits.
2. **No registry row before the first prompt.** 49-A2 deferred the cwd `workspaces`
   row; 53 restores it at startup (the same row 22 §11.4/22-A7 authorized and 49
   removed).
3. **The empty start screen (49-D2).** It is no longer the normal bare-`ymh`
   state; it survives only as the 53-F1 fallback.
4. **The Ctrl-S "nothing to switch to" case (49-D5/49-D6).** The cwd workspace is
   live from the start, so Ctrl-S now has at least one live workspace. 49-D5's
   session-aware predicate is unchanged and still applies once a second workspace
   exists.

**Ownership is unaffected (16 O1–O22), but the eager daemon IS in the orphaning
set.** The daemon is spawned under the same `HostLifecycle::ensureRunning`
presence model as the explicit activation path (`src/cli/cli.cpp:526-533`), so it
is supervisor-owned from the first moment. With one supervisor attached to its
eagerly-spawned daemon, `is_orphaning_view` is **true**
(`src/ui/supervisor_presence.cpp:17-19`: `live_supervisors == 1`, no `ymh run`
owner, no other fresh owner), so `compute_orphaning_set()` returns `[id]`, **not**
empty (`src/ui/supervisor.cpp:556-569`). Ctrl+Q (`begin_exit(allow_prompt=true)`,
`:3246`) therefore **opens the last-exit modal** (`:544-549`), and the confirmed
exit tears the daemon down through `teardown_daemons([id])` (`confirm_exit`
`:694-703`, `teardown_daemons` `:713-721`) — exactly 16 §4.2's rule: the prompt
opens **iff the orphaning set is non-empty** (i.e. no other owner). `requestExit()`
(`:520`) and `--yes`/`options_.no_prompt` bypass the prompt and tear down that same
non-empty set. This is the intended behaviour: the user is the last owner, so they
are asked before the daemon they own is stopped. The one genuinely-empty case is a
**quit between `fork` and `attach_workspace`**: `connections_` is not yet populated,
`compute_orphaning_set()` is empty, `confirm_exit(empty)` tears down nothing, and
only the **16 §5.1 daemon-side owner watchdog** reaps the daemon after
`owner_grace` (`ShutdownReason::NoOwners`). No 16 invariant is weakened; eager
spawn only moves the daemon into the ownership set earlier.

**Two-supervisor startup race.** If two supervisors start concurrently for the
same workspace, `HostLifecycle::ensureRunning`'s D-F1 winner-attach path
(`src/host/workspace_host.cpp:1175-1186`) hands the loser to the winner's daemon
instead of failing, and both supervisors attach the one daemon. Pinned: the eager
start must go through `ensureRunning` (step 3) so this path applies; 53-U17 covers
two eager supervisors.

### 53-D2 — The eager attach creates the first session (R2)

On a successful eager attach, the existing attach auto-create path runs
(`src/ui/supervisor.cpp:1556-1564`, per 46-I13/23-D58/50-D2.1): when the active
workspace has no live session, `create_session(workspace, {})` creates one. The
created session's model is `preferred_model_` when set, else the resolved config
model (`src/ui/supervisor.cpp:1602-1604`, `src/host/host_runtime.cpp:637`).

**H5 — the create path must resolve the name to a wire id.** `preferred_model_`
is an `llm.models` NAME (set from a picker row). `HostRuntime::createSession`
stores the raw `model` param string verbatim into `header.model` and
`payload::SessionStarted.model` (`src/host/host_runtime.cpp:637`;
`src/session/session_manager.cpp:61`, `:74`), and a fresh session emits no
`SessionModelChanged` — so the agent would either ignore the choice (session runs
the daemon default) or send the NAME as `request.model` (an invalid wire id).
Pin: **`session.create` resolves the requested name through the daemon
`ModelCatalog` to the entry's `model_id` (+ `parameters`/`profile`) BEFORE writing
the header/event**, mirroring `setSessionModel`; `effective()` therefore always
returns a wire id (53-D7). An unknown name is `InvalidParams` (53-F5).

Consequences, pinned:

- The active session exists within one `refresh_sessions` round-trip, so
  `status.model` is populated (`src/ui/supervisor.cpp:1653-1654`) and segment 3
  renders — R2 is satisfied with **no renderer change** in the normal path.
- The auto-created session is an **unprompted root session**, which
  `src/ui/session_catalog.cpp:138-142` (23 §6.1/23-D31) hides from `/sessions`
  until it is prompted. The disk cleanup owner is **23-D1′** (`23:399`), not 49
  §10.1: at a clean supervisor exit the supervisor deletes each unprompted root
  session it created. **23-D1′ is not yet shipped** (no `created_sessions_` in
  `src/ui/supervisor.cpp`), so empty sessions accumulate on disk today; the
  catalog filter only hides them. Recorded as a risk and OQ-53-1.
- The daemon (not the supervisor) owns the session create and the `SessionStarted`
  event; the supervisor still never writes `sessions.db` (its handle remains
  read-only, `src/ui/supervisor.cpp:1901`).
- **Failure mode.** If the eager auto-create fails *after* a successful attach
  (store unavailable, lease lost), 53-I2 is violated: degrade to the 53-D8
  no-session path — push the error as a notice, keep the workspace attached, and
  let the first prompt retry the create (53-F13). The attach itself is not undone.

### 53-D3 — The status line shows the model from the first frame (R2)

Two layers:

1. **Normal path.** The 53-D2 session makes `render_status` render segment 3
   (`src/ui/ui_render.cpp:640-647`, `:721-722`) with no *renderer* change; the
   value source changes per 53-D3.1 (`display_model`).
2. **Fallback path (amend 25-D1).** When a workspace is attached but no session is
   active (the transient window before auto-create lands, or a 53-F1/F2 degraded
   start), the `active == nullptr` branch (`src/ui/ui_render.cpp:610-624`) gains a
   **resolved-model left segment**. Pin the composition:

   - The left segment is `build · <model>` where `<model>` is the resolved model's
     **entry name** when `resolve_model(config).model_name` is non-empty, else its
     `model_id` (`include/ymh/config/config.hpp:489-508`). `build` mirrors segment 1
     (`25-D1`, `src/ui/ui_render.cpp:634`) so the fallback is visually consistent.
   - When a workspace is attached, the segment is `build · <model> · no session`;
     when no workspace is attached (the 53-F1 fallback), it is the 49-D2 string
     `no workspace attached — type a prompt to start` plus the model:
     `no workspace attached — type a prompt to start · <model>`.
   - The aggregate stays the right widget; the fallback is ellipsized with the same
     width discipline as segment 3 (`25-D1` step 3, `:366-384`).

   This requires one new `UiModel` field, `resolved_model` (a display string), set
   in `run()` from `options_.config` (53-D3.1) and refreshed on config-bearing
   events. It is never persisted and never written by the daemon.

   **53-D3.1 — `UiModel::resolved_model` and the canonical `display_model`.**
   Set once at startup: `model_.resolved_model = display_model(resolve_model(options_.config))`.
   `display_model` is ONE pinned function with a declared home:

   ```cpp
   // include/ymh/ui/ui_model.hpp (53-D3)
   // The canonical status-line model value: the llm.models entry name when named,
   // else the wire id. Used at create, refresh, apply_create_reply, resume, and
   // catalog hydration so the segment never flips between name and id.
   [[nodiscard]] std::string display_model(const ResolvedModel& model);           // name ?: id
   [[nodiscard]] std::string display_model(const Config& config,
                                           const std::string& model_id);          // name for id, else id
   ```

   **Correction (this replaces the Rev 1 claim that `/status` "already computes
   the same value"):** `effective_model` returns the **wire id only**
   (`src/config/config.cpp:2272-2274`). The initial `status.model` is therefore the
   wire id on the normal path (`src/ui/supervisor.cpp:1545`, `:1654`) but the entry
   name after a switch (`model_name ?: model`, 53-D6) — so one session could
   render `Muse-Glimmer-30B` at frame 1 and `balanced` after switching to the same
   model. **Pin the canonical value:** every write to `status.model` (create,
   refresh, `apply_create_reply`, resume, catalog hydration, switch) goes through
   `display_model`, so the segment shows the entry name when named, else the wire
   id — one value, no flipping. OQ-53-5 is corrected accordingly (now a pinned
   choice, not an open question).

### 53-D4 — `/model` is a picker over spec 52's named models (R3)

`/model` with **no args** opens a new overlay, `UiMode::ModelPicker`. `/model
<name>` (the argument form) remains and now validates + applies directly
(backward compatible with the current invocation shape). The overlay:

- **List source (pinned).** The rows are exactly the `config.llm.models` entries
  (`include/ymh/config/config.hpp:246-247`), sorted by name ascending,
  case-insensitive, with an `id` tie-break (mirroring 22 SW17's ordering rule).
  Additionally, when `resolve_model(config).model_name` is empty (the literal-id or
  builtin path, 52-D3 step 3/4), one synthetic row is prepended for the current
  effective literal id, labelled with the id and the default endpoint. **No new
  config and no daemon RPC are needed to build the list.** (Cosmetic: on the
  default/builtin path `resolve_model(config).endpoint.name == ""`, so the
  synthetic row's endpoint column renders empty — recorded in §10.1.)
- **Row content.** `name` (the `llm.models` key), `models.<name>.model` (the wire
  id), and `models.<name>.endpoint` (the endpoint name). This is the provider-
  grouped information dsh shows (`§8`), rendered on one line per row.
- **Current marker.** The row whose name/id matches the active session's display
  model (`StatusModel.model`, or `resolved_model` when no session) is marked (e.g.
  `●`), matching the switcher's current-node convention (`22 §3.4`).
- **Interaction (pinned; reuses 45-D2/45-D5 and 22-A2).**
  - `ArrowUp`/`ArrowDown` and `j`/`k` move the cursor; the move wraps modulo the
    row count (45-D1.2, `src/ui/supervisor.cpp:2369-2380`).
  - `Enter` applies the highlighted model (53-D5) and closes the overlay.
  - `Esc`/`Ctrl+C` dismiss without applying (45-D5, `src/ui/supervisor.cpp:2601-2604`).
  - Type-to-filter is **not** pinned in Rev 1; it is OQ-53-4 (the switcher's
    `SwitcherOverlayModel::filter`, `include/ymh/ui/ui_model.hpp:407`, is the
    precedent if adopted).
- **No session.** When no session is active, `Enter` sets `preferred_model_` (the
  next `session.create`, 53-D2) and updates `resolved_model`; it issues no RPC.
- **Read-only session.** On a session in a non-live workspace (the 45-D10
  unmodeled-session lockout), `/model` is refused with the existing lockout notice;
  no RPC is issued (53-F9).
- **Unknown name (argument form).** No RPC; an error conversation entry
  `unknown model: <name> (see /model)` is appended (53-F5).

### 53-D5 — The mid-flight switch RPC: `session.set_model` (R4)

A new transport method mirroring `session.set_mode` (25-D5):

- **Method:** `session.set_model` (`kSessionSetModel`, next to `kSessionSetMode`,
  `include/ymh/transport/protocol.hpp:534`).
- **Profile:** **Interactive only**, like `agent.select`. Pinned: `kSessionSetModel`
  is added to the `is_method_allowed` Automation exclusion list
  (`src/transport/protocol.cpp:667-673`, alongside `kSessionActivate`,
  `kSessionSuspend`, `kSessionCompact`, `kHostShutdown`, `kAgentSelect`); `ymh run`
  sets its model at `session.create`. Recorded as OQ-53-7.
- **Protocol version.** `session.set_model` is additive, so `kProtocolVersion`
  stays `1` (`include/ymh/transport/protocol.hpp:153`): both ends are the same
  binary and the handshake has no capability negotiation, so version skew surfaces
  as `MethodNotFound`, never a silent misparse. `kMethodCatalog` grows 37 → 38
  (`src/transport/protocol.cpp:635`).
- **Params:** `{ "session": <SessionId>, "model": <name-or-literal-id> }`. `model`
  is a non-empty string; an `llm.models` name is preferred. A literal wire id is
  accepted and resolves through `ModelCatalog::find` to the matching entry's
  endpoint/parameters/profile (the literal path is pinned in §4); an unmatched
  literal is `InvalidParams` (53-F5).
- **Reply:** `{ "session": <id>, "model": <effective wire id>, "model_name": <entry
  name or "">, "pending": <bool> }` (`SetModelResult`; homed in `host.hpp` next to
  `SetModeResult`, `include/ymh/transport/host.hpp:67`).
- **Semantics (pinned; mirrors `session.set_mode`):**
  1. `entry = runtime_.model_catalog().find(model)` (53-D7). Unknown ⇒
     `InvalidParams` (53-F5). A target whose `ResolvedEndpoint` identity
     (`name`/`base_url`) is not the daemon's registered endpoint's ⇒
     `EndpointNotRouted` (53-F6).
  2. `turn_open = agentStatus(session) != "Idle"` (`src/host/host_runtime.cpp:743`).
  3. `ModelSelection selection{entry.model_id, entry.name, entry.parameters,
     entry.profile, entry.endpoint.provider};`
     `ModelSetResult r = controller.set(session, turn_open, std::move(selection));`
     - `Unchanged` (already the effective model) ⇒ no append, `pending=false`.
     - `Committed` (idle) ⇒ one durable append now, `pending=false`.
     - `Queued` (turn open) ⇒ held in memory, `pending=true`.
  4. Reply with the effective id/name and `pending = (r == Queued)`.
- **A running step keeps its model.** The pending value is applied at the next
  **step start** (like plan mode, `src/agent/agent_loop.cpp:854-856`, `:951-953`)
  and flushed at turn end (`:68`); an in-flight request is never mutated. This is
  exactly dsh's rule (*"a running step keeps the model and effort it started
  with"*, `§8`).
- **Scope: per-session.** The change is a session event and a session-header
  update; it does **not** rewrite `llm.active_model` or any config. The daemon
  never writes config. Whether a switch should also become the workspace default is
  OQ-53-2.
- **Writer.** Only the owning daemon issues the append; the supervisor issues the
  RPC and never touches the store (its `sessions.db` handle is read-only,
  `src/ui/supervisor.cpp:1901`).

### 53-D6 — The durable event: `SessionModelChanged`

Following the 25-D2 pattern exactly:

```cpp
// include/ymh/session/events.hpp (namespace ymh::payload)
// The whole-value-replace session model selection. The last one wins; a log with
// none keeps the SessionStarted.model. `model` is the wire id; `model_name` is the
// `llm.models` entry name ("" for a literal-id selection).
struct SessionModelChanged {
    std::string model;
    std::string model_name;
};

// include/ymh/core/event.hpp — EventType gains one enumerator
SessionModelChanged,   // wire: session/model   (53-D6)

// include/ymh/session/events.hpp — registration (both directions)
template <> struct SessionEventMap<EventType::SessionModelChanged> {
    using type = payload::SessionModelChanged;
};
template <> struct EventTraits<payload::SessionModelChanged> {
    static constexpr EventType type = EventType::SessionModelChanged;
};

// to_json:   json = {{"model", value.model}, {"model_name", value.model_name}};
// from_json: value.model = json.value("model", ""); model_name = json.value("model_name", "");
```

Pinned properties:

- `wire_name(EventType::SessionModelChanged) == "session/model"` and
  `parse_event_type("session/model")` round-trips (`src/core/event.cpp:18-50`);
  `all_event_types()` includes it (`include/ymh/core/event.hpp:95`). Adding the
  enumerator also bumps the wire table `std::array<WireEntry, 30>` → `31`
  (`src/core/event.cpp:18`).
- **Projection-invisible:** `deriveMessages` ignores it, exactly like `PlanMode`
  (`src/session/session.cpp:393-398`).
- **Header materialization in the same transaction:** the append path sets
  `sessions.model = ?` alongside `sessions.updated_at` (the `SessionRenamed` →
  `sessions.title` precedent, `src/session/session_persistence.cpp:611-616`), and
  `Session::appendEventLocked` folds it into `header_.model` (the `SessionRenamed`
  precedent, `src/session/session.cpp:625-626`; the load/fork folds at `:569-570`,
  `:684-685`). This keeps `stored_session_model` (`src/ui/supervisor.cpp:1248-1262`)
  and `/sessions` correct after a switch. `stored_session_model` returns the **wire
  id**; the status line applies the canonical `display_model` (53-D3.1) when it
  hydrates from the catalog, so a reconnect cannot flip the segment between the
  entry name and the id.
- **No schema change:** the generic `(type, payload)` table stores it; no
  migration.
- **UI projection:** the adapter maps it to a new `ModelChanged{session, model,
  model_name}` `UiEvent` (`src/ui/ui_event_adapter.cpp:155-163` is the
  `SessionRenamed`/`PlanMode` precedent) and `UiModel::apply` sets
  `state.status.model = display_model(...)` — the canonical entry-name-else-id
  value (53-D3.1), **not** a second name/id rule — (the `PlanModeChanged`
  precedent, `src/ui/ui_model.cpp:1071-1074`), so the switch is reflected even when
  the reply is lost (53-F7).

### 53-D7 — The daemon-side model controller and the agent seam

Mirror `PlanModeController` (`include/ymh/agent/plan_mode_controller.hpp`) exactly:

- A `ModelSelectionController` owned by `WorkspaceRuntime` (next to `plan_mode_`,
  `src/agent/workspace_runtime.cpp:173-180`), with an `AppendFn` that appends
  `payload::SessionModelChanged` to the resident session and a `ResolveFn` that
  rebuilds a full selection from a durable wire id through the catalog (H5).
- `set(session, turn_open, ModelSelection)` → `Unchanged | Committed | Queued`
  (no `Cancelled` — unlike plan mode there is no `request_exit` cancel path);
  `apply_pending_at_step_start(Session&)`; `flush_pending_at_turn_end(Session&)
  noexcept`; `erase(SessionId) noexcept`.
- **H4: the selection carries `parameters` + `profile` + `provider`, not just the
  id.** A switch must change the whole request shape. `set`/`effective` move a
  `ModelSelection` (see §4) and `buildRequest` applies every field via the pinned
  §4 field map; `ModelSelection.profile` drives the **loop-side** profile
  behaviour — `force_first_tool_call` (47-D2) and any other `AgentLoop`-side
  profile branch — for the selected entry. Without this, a switch would send the
  new wire id with the OLD model's
  `reasoning_effort`/`temperature`/`max_tokens`/`profile`. **Scope (Rev-1
  limitation, OQ-53-10):** profile shaping on the **provider/adapter** side does
  *not* follow a switch. `LlmCallConfig` has no profile field (§4) and the
  adapter's `LLMProviderConfig.profile` is baked once at startup from
  `resolve_model(config).profile` (`src/cli/wiring.cpp:104`), so it keeps the
  STARTUP model's profile for `normalize_tool_arguments`
  (`src/llm/openai_adapter.cpp:568-569`), `forbidden_stop_tokens`
  (`src/llm/openai_adapter.cpp:1055-1057`), and the capability gates
  (`src/llm/provider_registry.cpp:131`).
- **H5: `effective()` always returns a wire id.** A named create is resolved to
  the wire id at `session.create` (53-D2); on a cold/resumed/forked session
  `effective()` rebuilds the selection from the folded `header_.model` wire id via
  `ResolveFn`; only when nothing resolves does the caller keep its own model.
- The `AgentLoop` consults the controller when building a request AND when
  stamping `AssistantMessage.source`. Pin the seam:

```cpp
// include/ymh/agent/agent_loop.hpp
struct AgentServices {
    // 53-D7: null => the model never changes mid-session (pre-53 behavior).
    ModelSelectionController* model_selection = nullptr;
};

class AgentLoop {
public:
    // 53-D7/H4: the single source of the effective selection, used by
    // `buildRequest` AND the `AssistantMessage` provenance stamp (`:1089`).
    // Falls back to the configured model when the controller is null/empty.
    [[nodiscard]] ModelSelection effective_model_selection() const;
};

// include/ymh/agent/agent_loop.cpp — effective_model_selection()
ModelSelection AgentLoop::effective_model_selection() const {
    const ModelSelection configured{config_.model, "", config_.parameters,
                                    config_.profile, config_.provider};
    if (services_.model_selection == nullptr) {
        return configured;
    }
    if (const std::optional<ModelSelection> selected =
            services_.model_selection->effective(session_);
        selected.has_value()) {
        return *selected;
    }
    // No durable selection: keep the session's OWN model (the folded header wire
    // id) with the configured parameters/profile. Never `config_.model`, which can
    // differ from the session's model on resume/fork (53-D7).
    ModelSelection own = configured;
    if (!session_.header().model.empty()) {
        own.model = session_.header().model;
    }
    return own;
}

// include/ymh/agent/agent_loop.cpp — buildRequest() (exact field mapping)
const ModelSelection selected = effective_model_selection();
request.model      = selected.model;         // was config_.model       (:537)
request.parameters = selected.parameters;    // was config_.parameters  (:542)
// … the force_first_tool_call guard now reads the SELECTED profile:
//   if (selected.profile.force_first_tool_call && turn_step == 1 && …)   (:545)
LlmCallConfig config;
config.provider         = selected.provider;                     // was config_.provider     (:551)
config.model            = selected.model;                        // was config_.model        (:552)
config.reasoning_effort = selected.parameters.reasoning_effort;  // :553
config.temperature      = selected.parameters.temperature;       // :554
config.max_tokens       = selected.parameters.max_output_tokens; // :555
config.stop             = selected.parameters.stop;              // :556
config.top_p            = selected.parameters.top_p;             // :557
config.top_k            = selected.parameters.top_k;             // :558
config.seed             = selected.parameters.seed;              // :559
config.tool_choice      = request.parameters.tool_choice;        // :560
// `LlmCallConfig` has no profile field, so only LOOP-side profile shaping
// follows the selection: it reads `selected.profile` (47-D2
// `force_first_tool_call` above). PROVIDER-side shaping does NOT follow a
// switch: the adapter's `LLMProviderConfig.profile` is baked at startup
// (`src/cli/wiring.cpp:104`), so `normalize_tool_arguments`
// (`src/llm/openai_adapter.cpp:568-569`), `forbidden_stop_tokens`
// (`:1055-1057`), and the capability gates (`src/llm/provider_registry.cpp:131`)
// keep the startup model's profile. Rev-1 limitation (OQ-53-10).

// include/ymh/agent/agent_loop.cpp:1089 — provenance must follow the selection:
const ModelSelection stamped = effective_model_selection();
assistant.source = model_message_source(stamped.provider, stamped.model);
```

> **H4 field map (pinned).** On every request, `request.model` and
> `LlmCallConfig.model` take the selected entry's wire id; `request.parameters`
> and the nine `LlmCallConfig` parameter fields take the selected entry's
> `GenerationParameters`; `LlmCallConfig.provider` takes the selected entry's
> endpoint `ProviderId`; the `force_first_tool_call` guard and every other
> **loop-side** profile-driven behaviour take the selected entry's `ModelProfile`.
> The selected entry's `profile` is resolved from its `ModelSettings.profile` via
> `find_model_profile` at catalog-build time. Nothing is copied from the previous
> model's config. **Provider/adapter-side** profile shaping is *not* part of this
> map and does not follow a switch: `LlmCallConfig` carries no profile and the
> adapter's `LLMProviderConfig.profile` is baked at startup
> (`src/cli/wiring.cpp:104`), so it keeps the startup model's profile (53-D7/H4
> scope; OQ-53-10).

- `AgentLoop::runTurn`/`runStep` call
  `services_.model_selection->apply_pending_at_step_start(session_)` beside the
  plan-mode call (`src/agent/agent_loop.cpp:854-856`, `:951-953`). The turn-end
  flush currently rides `PlanFlushGuard`, which is typed to `PlanModeController`
  (`src/agent/agent_loop.cpp:60-69`, used at `:844`, `:916`); **generalize the
  guard to run both flushes** (or add a second guard) so a queued model change is
  committed on every turn exit — normal end, error, cancellation, lease failure,
  step limit. The `AssistantMessage.source` stamp at `:1089` must use
  `effective_model_selection()` (above), not `config_.provider`/`config_.model`.
- **Endpoint routing (pinned restriction).** The daemon registers one adapter
  keyed by the resolved endpoint's `ProviderId`
  (`src/agent/workspace_runtime.cpp:374-390`) and bakes that endpoint's
  `base_url`/key/profile from the single `resolve_model(config)` result
  (`to_provider_config`, `src/cli/wiring.cpp:86-104`). The route lookup matches on
  `ProviderId` alone (`resolve_adapter`, `src/llm/llm_runtime.cpp:340-359`), so
  the guard MUST compare the **full `ResolvedEndpoint` identity** — its `name`
  and `base_url` — not the provider id: two distinct endpoints can share a
  `ProviderId` (the common `openai-compatible` case) and would otherwise be
  admitted while the request is sent to the registered endpoint's base_url/key.
  A model whose endpoint identity is not the registered endpoint's is **rejected
  up front** with `EndpointNotRouted` (53-F6) so the failure is visible at switch
  time, never a silent `NoProviderRoute` on the next request. Same-endpoint model
  switches (the common case: one endpoint, many `llm.models` entries) work in
  Rev 1; cross-endpoint switching requires multi-endpoint routing (OQ-53-3).
- **`ModelCatalog`.** Because `WorkspaceRuntime::Impl` consumes `Config` and does
  not retain it (verified: no `config_` member; `src/agent/workspace_runtime.cpp:129-180`),
  build a `ModelCatalog` once at startup: `name → {model_id, ResolvedEndpoint,
  GenerationParameters, ModelProfile}`, plus the default entry. Expose it via
  `WorkspaceRuntime::model_catalog()` (mirroring `plan_mode()`,
  `include/ymh/agent/workspace_runtime.hpp:162`) and the controller via
  `WorkspaceRuntime::model_selection()`; both `HostRuntime::setSessionModel` and
  the `session.create` model resolution go through `find`. The catalog never
  stores literal secrets beyond what `to_provider_config` already holds.

### 53-D8 — Errors, notices, and the no-session path

- **Unknown model** (argument form or RPC) ⇒ `InvalidParams` (RPC) / an error
  conversation entry (UI). No event, no state change.
- **Cross-endpoint** ⇒ `EndpointNotRouted` (new `AppCode = -32021`), surfaced as an
  error entry. No event.
- **No live session** ⇒ no RPC; the picker sets `preferred_model_` and updates
  `resolved_model`.
- **Read-only session** ⇒ the 45-D10 lockout notice; no RPC.
- **Daemon dies / reply lost** ⇒ the durable event is the source of truth; the
  supervisor surfaces a notice and does not optimistically change `status.model`
  (53-F7). The Rev 1 `/model` handler's optimistic `status.model = args`
  (`src/ui/command_registry.cpp:186-189`) is **removed**; the value changes only
  from a reply or a projected event.
- **Queued change lost to daemon death** ⇒ if the daemon replied `pending=true`
  and died before the step boundary, the in-memory change is gone; on reconnect
  the effective model reverts to the last durable value and a notice renders
  (53-F12). Recovery: re-issue `/model`.
- **Eager auto-create fails after attach** ⇒ degrade to this no-session path: a
  notice, the workspace stays attached, the first prompt retries the create
  (53-F13).

---

## 4. C++ interface sketches (pinned)

```cpp
// ── src/cli/cli.cpp (run_supervisor_entry) ────────────────────────────────
// 53-D1: a bare `ymh` registers the cwd row and spawns/attaches its daemon
// BEFORE the TUI loop. A spawn failure degrades to the 49-D2 empty state with a
// notice and is NOT fatal in the bare case (53-F1); for an explicit activation
// (`--resume`/`--new`) it stays fatal (exit 1).
std::optional<WorkspaceRecord> row = find_or_register_workspace(*canonical, err);
if (!row.has_value()) {
    return 1;                                   // registry / canonicalize failure
}
std::optional<std::string> startup_notice;
try {
    const AttachResult attach = lifecycle.ensureRunning(row->id, identity);
    (void)attach;
} catch (const std::exception& error) {
    startup_notice = "cannot start workspace daemon: " + std::string{error.what()};
}
// … scanner seeds `attached`; options.workspaces = attached (may be empty on 53-F1)
options.initial_notice = std::move(startup_notice);

// ── include/ymh/ui/supervisor.hpp (SupervisorRunOptions) ──────────────────
std::optional<std::string> initial_notice;   // 53-D1/53-D3 fallback notice

// ── src/ui/supervisor.cpp (SupervisorApp::run) ────────────────────────────
if (options_.initial_notice.has_value()) {
    model_.pushNotice(*options_.initial_notice);   // 53-F1
}
model_.resolved_model = display_model(resolve_model(options_.config));   // 53-D3.1
// … the existing attach loop and the `if (!model_.workspaces.empty())` guard stay.

// ── include/ymh/ui/ui_model.hpp ───────────────────────────────────────────
// 53-D3.1: the resolved model for the no-session fallback segment.
std::string resolved_model;   // entry name when named, else the wire id
// 53-D3.1: the canonical display helper (declared here, defined in the .cpp);
// the SINGLE name-else-id rule used by every status.model write.
[[nodiscard]] std::string display_model(const ResolvedModel& model);
[[nodiscard]] std::string display_model(const Config& config, const std::string& model_id);

// 53-D4: the `/model` picker overlay.
struct ModelPickerRow {
    std::string name;       // llm.models key; "" for the synthetic literal row
    std::string model_id;   // wire id
    std::string endpoint;   // endpoint name
};
struct ModelPickerModel {
    // H3: the flag is `visible` (NOT `open` — C++ forbids a data member and a
    // member function sharing a name; the `open(...)` method below).
    bool                       visible = false;
    std::vector<ModelPickerRow> rows;
    std::size_t                selected = 0;
    void open(const Config& config, const std::string& current_display);
    void close();
    void moveUp();
    void moveDown();
};
// UiModel gains: ModelPickerModel model_picker;
// UiMode gains:  ModelPicker,

// ── include/ymh/ui/command_registry.hpp ───────────────────────────────────
// 53-D4: `/model` with no args opens the picker; with args validates + applies.
std::function<void()> open_model_picker;                    // NEW
std::function<void(const std::string&)> apply_model;        // replaces set_model
// (`set_model` is retired or aliased to `apply_model`; 53-A7.)

// ── include/ymh/transport/protocol.hpp ────────────────────────────────────
inline constexpr std::string_view kSessionSetModel = "session.set_model";  // 53-D5
// AppCode gains:
EndpointNotRouted = -32021,   // 53-F6: the target model's endpoint has no route
// kMethodCatalog grows 37 -> 38 (src/transport/protocol.cpp:635);
// is_method_allowed excludes it from Automation (protocol.cpp:667-672).

// ── include/ymh/transport/host.hpp (next to SetModeResult, :67) ───────────
struct SetModelResult {          // 53-D5; mirrors SetModeResult (host.hpp:65-72)
    SessionId   session;
    std::string model;           // effective wire id
    std::string model_name;      // llm.models entry name; "" for a literal id
    bool        pending = false; // queued behind an open turn
};
virtual SetModelResult setSessionModel(const nlohmann::json& params) = 0;   // 53-D5

// ── include/ymh/agent/model_selection.hpp (new; mirrors PlanModeController) ─
// 53-D7: the resolved selection carried end-to-end. `model` is the WIRE ID.
// H4: the entry's resolved parameters/profile/provider travel WITH it, so a
// switch applies the new model's effort/temperature/max_tokens/profile — not the
// previous model's (the pre-review draft swapped only the id).
struct ModelSelection {
    std::string          model;        // wire id (NEVER an llm.models name)
    std::string          model_name;   // llm.models entry name; "" for a literal
    GenerationParameters parameters;   // selected entry's resolved params
    ModelProfile         profile;      // selected entry's profile
    ProviderId           provider;     // selected entry's endpoint provider
};

enum class ModelSetResult : std::uint8_t { Unchanged, Committed, Queued };

class ModelSelectionController {
public:
    using AppendFn = std::function<void(const SessionId&, payload::SessionModelChanged)>;
    // 53-D7/H5: rebuilds the full selection from a durable wire id (the folded
    // `header_.model`) on a cold/resumed/forked session. The daemon supplies the
    // catalog lookup.
    using ResolveFn = std::function<std::optional<ModelSelection>(const std::string& wire_id)>;
    ModelSelectionController(AppendFn append, ResolveFn resolve);

    ModelSetResult set(const Session& session, bool turn_open, ModelSelection selection);
    bool apply_pending_at_step_start(Session& session);
    bool flush_pending_at_turn_end(Session& session) noexcept;
    // H4/H5: the full selection, never a bare id. Pending wins; else the durable
    // selection (the folded `header_.model` wire id) is rebuilt through
    // `resolve_`; nullopt only when the session has no model or it does not
    // resolve. `effective().model` is ALWAYS a wire id.
    [[nodiscard]] std::optional<ModelSelection> effective(const Session& session) const;
    void erase(const SessionId& session) noexcept;
private:
    // Local nested memo type: `ProjectionMemo` is a PRIVATE nested type of
    // `PlanModeController` (`include/ymh/agent/plan_mode_controller.hpp:53`) and
    // is not visible here.
    struct Memo {
        bool           valid = false;
        ModelSelection selection;
    };
    AppendFn                                  append_;
    ResolveFn                                 resolve_;
    mutable std::mutex                        mutex_;
    std::mutex                                commit_mutex_;   // 25-D2 H1 TOCTOU
    std::map<SessionId, ModelSelection>       pending_;
    mutable std::map<SessionId, Memo>         memo_;
};

// ── include/ymh/agent/workspace_runtime.hpp (accessors, mirroring plan_mode()) ─
[[nodiscard]] ModelCatalog&             model_catalog() noexcept;     // 53-D7
[[nodiscard]] ModelSelectionController& model_selection() noexcept;   // 53-D7

// ── src/agent/workspace_runtime.cpp (ModelCatalog) ────────────────────────
// 53-D7: built once at daemon startup (Config is not retained by Impl).
struct ModelCatalogEntry {
    std::string       name;          // llm.models key; "" for the default entry
    std::string       model_id;      // wire id
    ResolvedEndpoint  endpoint;      // carries the ProviderId
    GenerationParameters parameters; // resolved for THIS entry
    ModelProfile      profile;       // find_model_profile(entry.profile)
};
class ModelCatalog {
public:
    static ModelCatalog build(const Config& config);
    // 53-D5 literal path (pinned): an entry `name` match wins; else the literal
    // is matched against each entry's `model_id` (sorted order, first match) and
    // yields that entry's endpoint/parameters/profile. No match => nullopt (the
    // caller replies InvalidParams, 53-F5). A literal never invents an endpoint
    // or parameters. `find(default_entry().model_id)` returns the default entry.
    [[nodiscard]] std::optional<ModelCatalogEntry> find(std::string_view name_or_id) const;
    [[nodiscard]] const ModelCatalogEntry&         default_entry() const;
    [[nodiscard]] std::vector<ModelCatalogEntry>   entries() const;   // sorted
};

// ── src/host/host_runtime.cpp (setSessionModel) ───────────────────────────
protocol::SetModelResult HostRuntime::setSessionModel(const nlohmann::json& params) {
    // validate {session, model}; entry = runtime_.model_catalog().find(model)
    //   (nullopt => InvalidParams, 53-F5);
    // reject a different ResolvedEndpoint IDENTITY than the daemon's registered
    //   endpoint (`model_catalog().default_entry().endpoint`): compare `.name`
    //   AND `.base_url` — NOT merely `.provider`. Two distinct endpoints can
    //   share a ProviderId (the common `openai-compatible` case), and the route
    //   lookup matches on ProviderId alone (`resolve_adapter`,
    //   `src/llm/llm_runtime.cpp:340-359`), so a provider-id-only guard would
    //   wrongly admit them while the request still goes to the registered
    //   endpoint's base_url/key (EndpointNotRouted, 53-F6);
    // ModelSelection selection{entry->model_id, entry->name, entry->parameters,
    //                          entry->profile, entry->endpoint.provider};
    // turn_open = agentStatus(id) != "Idle";
    // r = runtime_.model_selection().set(*session, turn_open, std::move(selection));
    // return {id, entry->model_id, entry->name, r == ModelSetResult::Queued};
}

// ── src/host/host_runtime.cpp (createSession; H5) ─────────────────────────
// The `session.create` `model` param is an `llm.models` NAME (or a literal id).
// It MUST be resolved through the catalog BEFORE `options.model` reaches the
// header / `SessionStarted` / `SessionManager::createSession`; otherwise the raw
// NAME is stored in `header.model` (a wire-id slot) and a fresh session emits no
// `SessionModelChanged`, so `effective()` would send a NAME as `request.model`.
if (object.contains("model")) {
    const std::string requested = object.at("model").get<std::string>();
    const std::optional<ModelCatalogEntry> entry = runtime_.model_catalog().find(requested);
    if (!entry.has_value()) {
        throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                               "InvalidParams"});
    }
    options.model = entry->model_id;   // wire id, mirroring setSessionModel
}
// else: options.model = runtime_.agent_config().model (already a wire id).
// The controller's `resolve_` (the catalog lookup by wire id) then rebuilds the
// full parameters/profile for this session, so a named create applies the named
// entry's effort/temperature/profile — not the daemon default's.
```

---

## 5. Invariants

| ID | Invariant |
|---|---|
| 53-I1 | A bare `ymh` registers the cwd `workspaces` row and spawns/attaches its daemon **before** the TUI loop, unless canonicalization/registration fails (fatal) or the spawn throws (53-F1 degraded fallback). 49-I1/49-I3 no longer hold for the bare-cwd case. |
| 53-I2 | After a successful eager attach, the active workspace has a live session within one `refresh_sessions` round-trip (53-D2), and `StatusModel.model` is non-empty, so segment 3 renders from the first frame. |
| 53-I3 | The eagerly created daemon is supervisor-owned from spawn and **is** in the orphaning set while the eager supervisor is the only owner (`is_orphaning_view` true, `compute_orphaning_set() == [id]`): Ctrl+Q opens the last-exit modal, and the confirmed exit (`requestExit`/`--yes`, or the modal) tears it down via `teardown_daemons([id])`. The prompt opens **iff the set is non-empty** (i.e. no other owner) — 16 §4.2. A quit before `attach_workspace` (empty `connections_`) tears down nothing and is backstopped by the 16 §5.1 daemon-side watchdog after `owner_grace`. (16 O1/O10/O15, §4.) |
| 53-I4 | The `/model` picker enumerates exactly the `llm.models` entries (sorted) plus the synthetic current-literal row when `model_name` is empty; it issues `session.set_model` only for a live session, and sets `preferred_model_` otherwise. |
| 53-I5 | `session.set_model` is the sole mid-flight model-change path. The supervisor never writes `sessions.db` or config; only the owning daemon appends. |
| 53-I6 | Every **committed** model change appends exactly one `SessionModelChanged` event, and a **queued** change appends exactly one when it is applied at the step boundary; the last one in the log wins on fold/resume/fork; `sessions.model` is materialized in the same transaction (53-D6). A change that is only queued and then lost to daemon death is not durable (53-F12). |
| 53-I7 | A model change never mutates an in-flight request. An open turn queues it (`pending=true`) and it applies at the next step boundary; a running step keeps the model it started with. |
| 53-I8 | A model change is per-session. `llm.active_model`, `agent.model`, and every config layer are untouched by the daemon and by the RPC. |
| 53-I9 | A target model whose `ResolvedEndpoint` identity (`name`/`base_url`) differs from the daemon's registered endpoint is rejected (`EndpointNotRouted`) before any append: no event, no header change, no pending entry. A shared `ProviderId` alone does NOT satisfy the guard. |
| 53-I10 | The picker keymap is `ArrowUp`/`ArrowDown`/`j`/`k` (wrapping) + `Enter` (apply) + `Esc`/`Ctrl+C` (dismiss), matching 45-D1.2/45-D5 and 22-A2; `Tab` is not a picker key. |
| 53-I11 | The status line's model segment (25-D1 segment 3) is never silently dropped while ≥4 columns remain; the 53-D3 fallback segment uses the same width discipline. |
| 53-I12 | The 53-D2 auto-created session is a root unprompted session and is hidden from `/sessions` by the 23-D31 filter until prompted. |
| 53-I13 | `/model` is refused on a read-only session (45-D10 lockout) with the existing notice; no RPC is issued. |

---

## 6. Failure modes (53-F)

Continue the repo's `F1–F12` convention with the spec-local `53-F` prefix (disjoint
from `§54 F1–F12`).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 53-F1 | Eager `ensureRunning` throws at startup | a startup notice `cannot start workspace daemon: <err>`; the TUI still starts with the **cwd workspace unmodeled** — other live daemons the scanner reports are still modeled (49-D2 empty state). The cwd `workspaces` row was already registered in step 2 before `ensureRunning`, so a durable row may exist with no daemon — the row-vs-model distinction, **not** a phantom workspace | the first prompt retries via the retained lazy `submit()` path; `/sessions` still works |
| 53-F2 | The eager daemon dies immediately after spawn | the daemon-death eviction path removes the workspace (22 §3.2) and a notice renders; no model segment until a new attach | the next prompt re-spawns (53-D1 fallback) |
| 53-F3 | cwd cannot be canonicalized / registry unavailable | `ymh` exits 1 (unchanged, `src/cli/cli.cpp:478-499`) | user fixes the path / `XDG_STATE_HOME` |
| 53-F4 | `session.set_model` on an unknown session | `UnknownSession` (`AppCode`) | picker targets the active session; a stale picker is re-opened |
| 53-F5 | Unknown model name/id | `InvalidParams` (RPC) / error entry (UI); no event | user picks a listed row |
| 53-F6 | Target model's `ResolvedEndpoint` identity (`name`/`base_url`) differs from the daemon's registered endpoint (a shared `ProviderId` alone is not enough) | `EndpointNotRouted` (`-32021`); no event, no pending entry | pick a same-endpoint model (Rev 1); cross-endpoint waits on OQ-53-3 |
| 53-F7 | `session.set_model` reply lost / daemon dies before the reply | the supervisor surfaces a notice; `status.model` is **not** optimistically changed; the durable event reconciles on reconnect | re-issue `/model` |
| 53-F8 | The session is deleted while a model change is pending | `ModelSelectionController::erase` drops the pending entry (the `plan_mode().erase` precedent, `src/host/host_runtime.cpp:866`); no append | none needed |
| 53-F9 | `/model` on a read-only (non-live-workspace) session | the 45-D10 lockout notice; no RPC | activate the workspace, then switch |
| 53-F10 | A pending model change is applied at step start while a compaction request is in flight | compaction is a `request_override` (`include/ymh/llm/llm_provider.hpp:91-98`) and outranks the session model; the conversation request uses the new model, the compaction keeps its override | none needed; recorded for clarity |
| 53-F11 | Two supervisors issue `session.set_model` for the same session concurrently | the daemon serializes both through `commit_mutex_`; the last append wins on fold; both replies echo their own effective value | last-writer-wins is the pinned semantics (same as `plan/mode`) |
| 53-F12 | The daemon dies after replying `pending=true` but before the step boundary applies the change | the in-memory pending is lost; on reconnect `effective()` reverts to the last durable `SessionModelChanged`/`SessionStarted.model`; the daemon-death eviction notice renders | re-issue `/model`; the durable event is the source of truth (53-I6) |
| 53-F13 | The eager auto-create fails after a successful attach (store unavailable / lease lost) | 53-I2 is violated: no live session, so segment 3 does not render; the error is surfaced as a notice and the workspace stays attached | the first prompt retries the create through the retained lazy `submit()` path; the picker's no-session path (53-D8) applies until then |

---

## 7. Amendment register

| ID | Amended / superseded clause | Verified code anchor | New behaviour |
|---|---|---|---|
| 53-A1 | 49-D1 (`49:210-275`), 49-I1 (`49:533`), 49-I3 (`49:535`), 49-A2 (`49:577`) | `src/cli/cli.cpp:500-533`; `src/ui/supervisor.cpp:405-415` | **Superseded.** The bare-cwd workspace row and daemon are created before the loop (53-D1). |
| 53-A2 | 49-D2 (`49:276-309`), 49-I2 (`49:534`) | `src/ui/ui_render.cpp:613-623` | **Superseded as the normal start.** The empty screen is the 53-F1 fallback only. |
| 53-A3 | 49-I5 (`49:537`) | `src/ui/supervisor.cpp:539-570` (`begin_exit`/`compute_orphaning_set`), `:694-721` (`confirm_exit`/`teardown_daemons`) | **Amended.** A zero-daemon supervisor remains representable, but only as the fallback. The eager daemon **is** orphaned while the supervisor is the last owner, so its clean exit opens the last-exit modal (53-I3). |
| 53-A4 | 49-F1 (`49:558`), 49-F4 (`49:561`) | `src/ui/supervisor.cpp:1093-1130` | **Amended.** The startup spawn failure (53-F1) is now the primary path; the first-prompt failure is the fallback. |
| 53-A5 | 16-D2 (`16:529-551`), reinstated by 49-A1/49-A11 | `src/cli/cli.cpp:528` | **Re-superseded for the bare-cwd case.** 22 §11.4/22-A7's eager initial spawn is restored; 22-A7's S3 scope/trigger extension is retained. |
| 53-A6 | 25-D1's `active == nullptr` clause (`25:366-369`) | `src/ui/ui_render.cpp:610-624` | **Amended.** A resolved-model left segment renders when a workspace is attached without a session (53-D3). |
| 53-A7 | 52 §1.4 (`52:127-131`) "any UI surface … out of scope" | `src/ui/command_registry.cpp:174-194` | **Amended ownership.** 53 owns the `/model` picker; 52 Part A remains the schema/source. |
| 53-A8 | 01-session.md durable event table | `include/ymh/core/event.hpp:51-82`; `src/core/event.cpp:18` (`std::array<WireEntry, 30>` → `31`) | **Adds** `SessionModelChanged` (`session/model`). |
| 53-A9 | 05-transport method table; `TransportHost`/`HostRuntime`; 49-D10 (`49:408`) | `include/ymh/transport/protocol.hpp:504-543`; `src/transport/protocol.cpp:635-672`; `src/transport/protocol_server.cpp:406-412` | **Adds** `session.set_model` (mirrors 25-D5's `session.set_mode`). |
| 53-A10 | 45-D2/45-D5/45-D9, 22-A2 | `src/ui/supervisor.cpp:2500-2530` (dialog arrows/Return), `:2591-2647` (switcher arrows/`j`/`k`/Esc/Return), `:2369-2380` (wrapping) | **Retained (not amended).** The picker reuses the established keys. |
| 53-A11 | 46-I13 / 23-D58 / 50-D2.1 | `src/ui/supervisor.cpp:1556-1564` | **Retained (not amended).** The eager attach triggers the auto-create. |
| 53-A12 | 08 §5.2 (L11) (`08:687-689`, `:1027-1029`) — 08 misattributes the rule to `01 §3`, which does not pin it | `src/session/session_manager.cpp:61`; `src/session/session_persistence.cpp:611-616`; `src/session/session.cpp:625-626` | **Amended.** `model` becomes foldable (the `title` precedent) and is materialized on `SessionModelChanged`. |
| 53-A13 | 23-D31 (`23:361`), 23-D1′ (`23:399`) | `src/ui/session_catalog.cpp:138-142` | **Retained (relied on), gap recorded.** The filter hides the unprompted session; 23-D1′'s clean-exit deletion is **not yet shipped** (`created_sessions_` absent), so empty sessions accumulate on disk. |

---

## 8. dsh (DeepSeek Harness) mapping

Verified against the installed packages
(`/home/yury/.dsh/profiles/node_modules/@deepseek-ai/`).

| dsh | Evidence | ymh 53 mapping |
|---|---|---|
| `/model` popup and the composer model seat share **one per-session directory**; both submit through `session.selectModel` over `session.models`. | `dsh-client-ui-model-selection/README.md` (*"Two entries over ONE per-session directory owned by `ModelDirectoryResolver` (`ctx.modelDirectories`): the `/model` popupSelect contribution … and the composer's named `conversation.input.model` seat … submit through `session.selectModel`"*) | `/model` opens one picker over the per-session model; the status-line model segment is the read-only "seat". The directory source is spec 52's `llm.models` (host-reported), mapped to the daemon's `ModelCatalog` + the supervisor's `config.llm.models`. |
| Models are **grouped by provider**; the popup shows provider names and catalog descriptions. | same README (*"Models stay grouped by provider. The composer menu shows model and effort names only. The `/model` popup shows provider names and catalog descriptions"*) | ymh groups by **endpoint** (the connection), the nearest analogue; rows show `name`, wire `model`, `endpoint` (53-D4). |
| **A complete selection applies to the next request; a running step keeps the model and effort it started with.** | same README (Summary) | Pinned as 53-D5/53-I7: idle ⇒ append now; open turn ⇒ queue and apply at step start; in-flight request untouched. |
| Reasoning effort is chosen with the model; a model without effort metadata has no Effort row. | same README (*"The popup applies the selected model's default effort … An adapter without reasoning metadata leaves the Effort row absent"*) | ymh takes `reasoning_effort` from the selected `llm.models` entry (`include/ymh/config/config.hpp:206`); no separate effort UI in Rev 1 (recorded in OQ-53-8). |
| The default model is **process-wide** (`dsh-agent-default-model`), configured at composition; per-session selection is the entry point's job; `saveSelection()` persists a default. | `dsh-agent-default-model/README.md` (*"gives newly created agents a shared default provider and model when their sessions do not specify one … The default is process-wide; per-session model selection remains the responsibility of the entry point that creates the agent"*; `currentSelection()`/`saveSelection()`) | ymh's process/workspace default is `llm.active_model` (52-D3); per-session selection is the `SessionModelChanged` event. ymh does **not** write config from the daemon, so a mid-flight switch does not call `saveSelection` — whether it should update the workspace default is OQ-53-2. |
| An unroutable session blocks the composer until routing is available. | same README (*"When the Host reports that no adapter serves the session's route, this plugin raises a composer block"*) | ymh rejects the switch up front with `EndpointNotRouted` (53-F6) rather than blocking the composer; a fuller parity (a blocked/disabled state) is deferred with multi-endpoint routing (OQ-53-3). |

---

## 9. Test plan

`ctest` is **NOT parallel-safe** and must not be run during a build (repo
convention). Live tests are opt-in (`YMH_LIVE_LLM=1` + `DEEPSEEK_API_KEY`) and skip
otherwise.

### 9.1 Fixtures

- `SupervisorHarness` (`tests/support`, used by `tests/unit/errata46_ui_test.cpp`)
  with `seed_active_workspace`, `seed_workspace`, `activate_session`,
  `on_catalog_snapshot`, `dispatch_key`. **New seams:** a fake `HostLifecycle`
  recording `ensureRunning` calls, and an `options.initial_notice` reader.
- `tests/support/host_harness.hpp` (hermetic `XDG_STATE_HOME`/`HOME`) for the
  spawn/attach integration tests.
- `tests/support/fake_transport_host.hpp` for the `session.set_model` handler tests.
- Golden: `build_model()` and `normalize(render_to_ansi(...))`
  (`tests/unit/ui_render_golden_test.cpp`).
- Existing spec-49 tests (`tests/unit/errata49_ui_test.cpp`, 460 lines) assert the
  lazy behavior and **must change** (see 9.5).

### 9.2 Unit (hermetic)

| ID | Test | Asserts |
|---|---|---|
| 53-U1 | `UI53_D1_EagerSpawnAtStartup` | a bare start invokes `ensureRunning` exactly once before the loop; the workspace is modeled/attached (53-I1). |
| 53-U2 | `UI53_D1_SpawnFailureDegrades` | `ensureRunning` throws ⇒ the TUI still starts, a notice is pushed, the cwd workspace is unmodeled (the durable cwd row may exist; no phantom model), (53-F1). |
| 53-U3 | `UI53_D2_AttachAutoCreatesSession` | `on_link_state(Attached)` with no live session issues `session.create`; `status.model` becomes non-empty (53-I2). |
| 53-U4 | `UI53_D3_StatusFallbackShowsModel` | workspace attached, no session ⇒ the left segment contains the resolved model; `active == nullptr` (53-D3). |
| 53-U5 | `UI53_D4_PickerListsModels` | the picker rows equal `config.llm.models` sorted, plus the synthetic literal row when `model_name` is empty (53-I4). |
| 53-U6 | `UI53_D4_PickerKeys` | arrows/`j`/`k` wrap; `Enter` applies; `Esc`/`Ctrl+C` dismiss; `Tab` is a no-op (53-I10). |
| 53-U7 | `UI53_D4_NoSessionSetsPreference` | with no session, `Enter` sets `preferred_model_`, updates `resolved_model`, issues no RPC. |
| 53-U8 | `UI53_D4_UnknownNameErrors` | `/model bogus` appends an error entry and issues no RPC (53-F5). |
| 53-U9 | `UI53_D5_SetModelCommitted` | idle session ⇒ `setSessionModel` appends once, `pending=false` (53-I6). |
| 53-U10 | `UI53_D5_SetModelQueued` | open turn ⇒ `pending=true`, no append until step start (53-I7). |
| 53-U11 | `UI53_D6_EventFoldsHeader` | a `SessionModelChanged` event materializes `sessions.model` and folds `header_.model` (53-I6, 53-A12). |
| 53-U12 | `UI53_D6_AdapterUpdatesStatus` | the adapter emits `ModelChanged`; `apply` sets `status.model` to the entry name (53-D6). |
| 53-U13 | `UI53_D7_CrossEndpointRejected` | a model on an unrouted endpoint ⇒ `EndpointNotRouted`, no event (53-I9/53-F6). |
| 53-U14 | `UI53_D7_ControllerEraseOnDelete` | deleting a session drops a pending change (53-F8). |
| 53-U15 | `UI53_I3_LastExitPromptEager` | a lone eager supervisor's Ctrl+Q computes `[id]` and opens the last-exit modal; confirm tears down via `teardown_daemons([id])`; `requestExit()`/`--yes` bypass the modal and tear down the same set (53-I3). |
| 53-U16 | `UI53_D4_ReadOnlyRefused` | `/model` on a read-only session shows the 45-D10 notice, no RPC (53-I13). |
| 53-U17 | `UI53_I3_TwoSupervisorLastExit` | with two eager supervisors on one workspace, the first exit computes an empty set (the other owner is fresh) and opens no modal / tears down nothing; the last exit computes `[id]` and tears down (53-I3). |
| 53-U18 | `UI53_D3_DisplayModelCanonical` | `display_model` returns the `llm.models` entry name when named, else the wire id, and is the value used at create/refresh/`apply_create_reply`; `/model` with no session updates the next session's displayed model (53-D3, OQ-53-5). |
| 53-U19 | `UI53_D7_SelectionCarriesParams` | `ModelSelectionController::effective` returns the selected entry's `parameters` + `profile` + `provider`, not just the id (53-D7). |
| 53-U20 | `UI53_D5_CreateResolvesWireId` | `session.create` resolves the `llm.models` NAME through the daemon catalog to the wire id (+params/profile) before writing the header/event; `effective()` is always a wire id (53-D5). |
| 53-U21 | `UI53_D6_FoldLastWins` | a two-`SessionModelChanged` log folds to the last event on load/resume/fork; `header_.model` is the wire id (53-I6). |
| 53-U22 | `UI53_I5_NoStoreWrite` | the supervisor issues only the RPC and performs no `sessions.db`/config write during a switch (53-I5). |
| 53-U23 | `UI53_F11_ConcurrentSetModel` | two concurrent `session.set_model` serialize through `commit_mutex_`; the last append wins on fold; both replies echo their own effective value (53-F11). |
| 53-U24 | `UI53_F10_PendingDuringCompaction` | a pending change applies at step start while compaction is in flight; the conversation request uses the new model, the compaction keeps its `request_override` (53-F10). |
| 53-U25 | `UI53_F4_UnknownSessionRejected` | `session.set_model` on an unknown/stale session returns `UnknownSession` and appends no event (53-F4). |
| 53-U26 | `UI53_F12_PendingLostOnDaemonDeath` | a queued change is lost when the daemon dies after replying `pending=true` but before the step boundary; `effective()` reverts to the last durable `SessionModelChanged`/`SessionStarted.model`; the daemon-death eviction notice renders (53-F12). |
| 53-U27 | `UI53_F13_EagerCreateRetryFallback` | the eager auto-create fails after a successful attach (store unavailable / lease lost); the 53-D8 no-session path applies — `resolved_model` is shown, `/model` sets `preferred_model_`, and the next prompt retries the create (53-F13). |

### 9.3 Golden render

| ID | Test | Asserts |
|---|---|---|
| 53-G1 | `G53_StatusWithModel` | the status line renders `build · <model> · …` with the model segment in position 3 (25-D1). |
| 53-G2 | `G53_StatusFallbackNoSession` | `build · <model> · no session` when a workspace is attached without a session (53-D3). |
| 53-G3 | `G53_ModelPickerOverlay` | the picker overlay renders rows (`name`, wire id, endpoint), the current marker, and a footer hint. |
| 53-G4 | `G53_StatusNarrowKeepsModel` | at a narrow width the model segment is kept while tps is dropped before the note/notice (mirrors `StatusNarrowDegradationDropsTpsBeforeNoteAndNotice`) (53-I11). |

### 9.4 Integration (FakeLLM) / PTY live

> Test IDs here use the `53-T#` prefix (integration **T**ests) so they do not
> collide with the `53-I#` invariants in §5. The old `53-I1..I3` integration IDs
> are renamed `53-T1..T3`.

| ID | Test | Asserts |
|---|---|---|
| 53-T1 | `I53_SetModelAppliesNextTurn` | after `session.set_model`, the next turn's request carries the new model; the event is in the log. |
| 53-T2 | `I53_RunningTurnKeepsModel` | a switch during an open turn does not change the in-flight request; the next step uses the new model. |
| 53-T3 | `I53_ResumeForkFoldsModel` | resume/replay **and fork** of a session with a `SessionModelChanged` yields the switched (last-wins) model in the header and `/sessions`. |
| 53-T4 | `I53_EagerDaemonDeath` | the eager spawn succeeds, the daemon then exits: the 22 §3.2 eviction removes the workspace and a notice renders; the next prompt re-spawns it (53-F2). |
| 53-T5 | `I53_SetModelReplyLost` | a fake transport drops the `session.set_model` reply: `status.model` is **not** optimistically changed; on reconnect the durable `SessionModelChanged` event reconciles `status.model` (53-F7). |
| 53-T6 | `I53_ConfigUntouched` | after a switch, `llm.active_model`/`agent.model` and every config layer are byte-identical (53-I8). |
| 53-T7 | `I53_SessionsHidesUnprompted` | the 53-D2 auto-created unprompted root session does not appear in `/sessions`; it appears after it is prompted (53-I12). |
| 53-T8 | `I53_PreAttachQuitWatchdog` | a quit before `attach_workspace` (empty `connections_`) tears down nothing; the daemon-side owner watchdog reaps the daemon after `owner_grace` (53-I3, 16 §5.1). |
| 53-T9 | `I53_ParametersFollowSelection` | the next turn's request carries the selected entry's model **and** `parameters` (reasoning_effort/temperature/max_output_tokens/stop/top_p/top_k/seed/tool_choice), `profile`, and provider — not the previous model's (53-D7). |
| 53-T10 | `I53_ProvenanceFollowsSelection` | `AssistantMessage.source` stamps the effective provider/model, not the stale `config_.provider`/`config_.model` (`src/agent/agent_loop.cpp:1089`) (53-D7). |
| 53-P1 | `P53_StartShowsModel` (PTY) | `ymh` start shows the model on the bottom line without any prompt. |
| 53-P2 | `P53_ModelPickerSwitch` (PTY) | `/model` opens the list, arrows+Enter switch, the bottom line updates. |
| 53-P3 | `P53_LiveSwitch` (`YMH_LIVE_LLM=1`) | a live DeepSeek session switches model between two turns and both turns succeed. |

### 9.5 Existing tests that MUST change

- `tests/unit/errata49_ui_test.cpp` — the `UI49_D1_*` tests assert the superseded
  lazy behavior: `UI49_D1_NoWorkspaceAtStartup` (`:317`),
  `UI49_D1_EmptyStateNoticeOnCtrlS` (`:327`),
  `UI49_D1_EmptyStateComposerTypeable` (`:338`),
  `UI49_D1_NoRegistryRowBeforePrompt` (`:349`),
  `UI49_D1_FirstPromptSpawnsWorkspace` (`:375`),
  `UI49_D1_SpawnFailureNoPhantom` (`:419`), and `UI49_I5_ZeroDaemonCleanExit`
  (`:442`). They are re-pointed at the 53 fallback (a *failed* eager spawn) or
  retired; the new eager expectations live in 53-U1/53-U2/53-U3/53-U15/53-U17.
- `tests/unit/ui_supervisor_pty_test.cpp` — the PTY tests directly assert the
  superseded lazy behavior and would leave the suite red:
  - `UI49_P3_NoDaemonBeforePrompt` (`:1765`) asserts a bare `ymh` creates neither
    a cwd host socket nor a `workspaces` registry row. **Re-point**: the bare
    `ymh` now creates both before the loop; a no-daemon start is the *failed*
    eager spawn (53-F1).
  - `UI49_P4_FirstPromptRendersSession` (`:1817`) asserts the first prompt is what
    attaches. **Re-point**: the session exists before any prompt (53-D2); assert
    the first prompt renders in the already-attached session.
  - `UI49_P1_ResumeThenCtrlSShowsNotice` (`:1861`) asserts no cwd workspace so
    Ctrl-S shows the notice. **Retire/re-point**: the eager cwd workspace is live,
    so this must become the *two-workspace* switcher case (or drive a failed
    spawn).
  - The stale comment at `:444` ("the eager cwd spawn is removed for a bare
    `ymh` …") must be rewritten; the eager cwd spawn is restored.
- `tests/unit/ui_render_golden_test.cpp` — `UI49_G5_LazyFirstSubmit` (`:1924`)
  golden-encodes the lazy first-submit transition. **Retire or re-point** at the
  eager start; `G53_StatusWithModel`/`G53_StatusFallbackNoSession` supersede it.
  Any golden embedding the 49-D2 empty-state status string is updated for the
  53-D3 fallback composition (`UI49_G4` at `:1907`).
- **Intentionally inherited, not re-tested:** 53-F3 (cwd cannot be canonicalized /
  registry unavailable → exit 1) is unchanged by 53 and remains covered by the
  existing spec-49/CLI tests (`src/cli/cli.cpp:478-499`).

---

## 10. Out of scope, recorded risks, and open questions

### 10.1 Recorded risks

- **This supersedes a (draft) decision and changes verified-spec behavior** (25-D1)
  and the transport surface, so it must pass the independent gate before any code.
- **A daemon now runs from launch** even if the user never prompts (53-D1; the 49
  §10.1 protection is knowingly given up). Bounded by 16's ownership/watchdog: a
  supervisor's clean exit tears it down, and the daemon-side watchdog backstops a
  crash.
- **An unprompted session is created per launch** (53-D2). It is hidden from
  `/sessions` by the 23-D31 filter (53-I12) and deletable, but it still exists on
  disk. Recorded; OQ-53-1.
- **Cross-endpoint switching is not supported in Rev 1** because the daemon
  registers one provider route keyed by the resolved endpoint's `ProviderId`
  (`src/agent/workspace_runtime.cpp:374-390`); it is rejected up front rather than
  failing later (53-F6). OQ-53-3.
- **Provider-side profile shaping does not follow a mid-session switch (Rev 1).**
  The loop-side profile behaviour (`force_first_tool_call`, 47-D2) follows the
  selected entry, but the adapter's profile is fixed at startup from
  `resolve_model(config).profile` (`src/cli/wiring.cpp:104`), so
  `normalize_tool_arguments` (`src/llm/openai_adapter.cpp:568-569`),
  `forbidden_stop_tokens` (`:1055-1057`), and the capability gates
  (`src/llm/provider_registry.cpp:131`) keep the STARTUP model's profile.
  Recorded; OQ-53-10.
- **A point-in-time picker** — the row list is a snapshot of `config.llm.models`;
  a config reload mid-session is not observed (mirrors 46-F5/49-F9).
- **The synthetic default row's endpoint column is empty** on the
  default/builtin path (`resolve_model(config).endpoint.name == ""`, 52-D3);
  cosmetic, recorded not fixed.
- **Implementation note (not a product decision).** The lazy behavior shipped
  before spec 49 was verified (`f91ef1b68`, `b74dfc433` self-reported 516/1867
  failing; `DESIGN_STATUS.md` still says draft). This spec supersedes the decision
  regardless; whether the implementation reverts the 49 diff or amends it in place
  is left to the implementer (was OQ-53-9; reclassified).

### 10.2 Open questions

1. **OQ-53-1 — Initial session vs. config-model-only display.** Does a bare `ymh`
   create an unprompted session (53-D2, reusing the shipped auto-create), or should
   the startup attach suppress auto-create and show the resolved config model with
   no session? 53 pins the former (it reuses shipped behavior and makes the model a
   live property); the latter is the smaller UX change. **Display-only question**:
   disk cleanup is owned by 23-D1′ and catalog hiding by 23-D31; neither is decided
   here.
2. **OQ-53-2 — Does a mid-flight switch become the workspace default?** Should
   selecting a model in a session also update `llm.active_model` (for new
   sessions), or stay per-session? ymh's daemon must not write config, so a
   config-writing option needs an explicit owner (the supervisor? a new RPC?).
   53 pins per-session only.
3. **OQ-53-3 — Multi-endpoint routing.** Cross-endpoint switching requires the
   daemon to register a route per `llm.endpoints` entry (a spec-52 Part A
   follow-on). Is that in scope for 53 Rev 2 or a separate errata? Until then,
   `EndpointNotRouted` (53-F6).
4. **OQ-53-4 — Type-to-filter.** Should the picker filter by typed prefix (the
   switcher's `filter` precedent, `include/ymh/ui/ui_model.hpp:407`) or stay
   arrows-only? 53 pins arrows-only in Rev 1.
5. **OQ-53-5 — Display name (RESOLVED by 53-D3.1).** Should the status line show
   the `llm.models` entry name (e.g. `balanced`) or the wire id (e.g.
   `Muse-Glimmer-30B`)? 53 pins ONE canonical value — the entry name when named,
   else the wire id — applied uniformly via `display_model` at create/refresh/
   resume/switch (53-D3.1, 53-D6), so it cannot flip between the two. Kept as a
   record; no longer open.
6. **OQ-53-6 — Endpoint on the status line.** The user said "model name etc.".
   Should the endpoint (and/or workspace) also render as a segment? 53 does not add
   one; the header already shows the workspace cwd.
7. **OQ-53-7 — Automation profile.** Should `session.set_model` be allowed in the
   Automation profile (`ymh run`)? 53 pins Interactive-only (like `agent.select`).
8. **OQ-53-8 — Reasoning effort.** Should `/model` also expose the model's
   `reasoning_effort` (dsh shows effort beside the model)? 53 does not.
9. **OQ-53-9 — Subagent model inheritance.** Does a subagent spawned mid-session
   inherit the parent session's effective (possibly pending) model, or the
   process/workspace default? 53 does not pin it: the subagent is a distinct
   session and the `session.create` resolution (53-D2) applies to it. Recorded as
   an OQ (this slot previously held the spec-49 implementation note, now moved to
   §10.1).
10. **OQ-53-10 — Provider-side profile switching.** Should a mid-flight switch
    also re-shape the **adapter's** profile (`normalize_tool_arguments`,
    `forbidden_stop_tokens`, capability gates)? The adapter is registered once
    with `LLMProviderConfig.profile` baked from the startup model
    (`src/cli/wiring.cpp:104`) and `LlmCallConfig` has no profile field, so Rev 1
    pins **loop-side-only** profile switching (53-D7/H4). A full fix needs a
    per-request profile on the adapter seam or per-endpoint adapters.

---

## 11. Revision log

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-09-23 | Initial draft. Four requirements from the verbatim request. Half A: eager cwd workspace/daemon creation at start (53-D1), the attach auto-create (53-D2), and the status-line model segment with a 25-D1 amendment (53-D3). Half B: the `/model` picker over spec 52's named models (53-D4), the `session.set_model` RPC (53-D5), the durable `SessionModelChanged` event (53-D6), the daemon-side `ModelSelectionController` + `ModelCatalog` + agent seam (53-D7), and the error/no-session paths (53-D8). Supersedes 49-D1/49-D2 and re-supersedes 16-D2's bare-cwd lazy policy; amends 25-D1, 52 §1.4, the transport table, and the 01 event table. Invariants 53-I1–53-I13; failure modes 53-F1–53-F11; amendment register 53-A1–53-A12; 9 open questions. Verification status: DRAFT — not yet reviewed. |
| 2 | 2026-09-23 | Rev 2 — five-reviewer finding set applied (8 HIGH, all MEDIUM, all LOW). **H1** ownership: the eager daemon **is** in the orphaning set (`is_orphaning_view` true, `compute_orphaning_set() == [id]`), so Ctrl+Q opens the last-exit modal and `requestExit`/`--yes` tear it down via `teardown_daemons([id])`; a pre-attach quit is backstopped by the 16 §5.1 watchdog (53-D1 paragraph, 53-I3, 53-U15/U17, 53-T8). **H2** added 08 §5.2 (L11) as an amended verified spec (53-A12, `08:687-689`; header Depends/Amends). **H3** `ModelPickerModel::open` collision fixed (flag renamed `visible`). **H4** the switch now carries the entry's `parameters`+`profile`+`provider` end-to-end (`ModelSelection`), with a pinned `buildRequest`/`LlmCallConfig` field map and `:1089` provenance. **H5** `session.create` resolves the name to a wire id through `ModelCatalog` before the header/event; `effective()` is always a wire id. **H6** §9.5 now names the PTY tests (`UI49_P3`/`P4`/`P1`), the stale `:444` comment, and `UI49_G5_LazyFirstSubmit`, and corrects the `UI49_D1_*` IDs. **H7** 53-T4 (53-F2). **H8** 53-T5 (53-F7). MEDIUM: §2.1/§7 anchors re-pinned (`:504`/`:500-533`/`:528`); §2.3 optimistic-update prose; 53-D2 cites 23-D1′/23-D31 (not shipped) and gains 53-F13; 53-D3 pins canonical `display_model` (OQ-53-5 resolved); 53-D5 literal path + Automation exclusion + protocol version; 53-I6 qualified; new 53-F12; `ProjectionMemo` replaced by a local memo; `WorkspaceRuntime::model_catalog()`/`model_selection()` pinned; `effective()` cold-session semantics; `:1089` provenance; §4 literal `find`; tests 53-U17–U24, 53-T4–T10, 53-G4. LOW: `std::array<WireEntry, 30>`→`31`; `PlanFlushGuard` generalization; §2.4/§2.5 cross-refs; 49-D10 named; placeholders filled; vocabulary aligned; OQ-53-9 reclassified (implementation note) and its slot reused for subagent inheritance; 53-F3 inherited; §11 counts reconciled; `ModelSetResult::Cancelled` dropped; `SetModelResult` homed in `host.hpp`; synthetic-row endpoint note; `display_model` home; F1 row-vs-model; D-F1 winner-attach cited; `kProtocolVersion` decision. Integration test IDs renamed `53-I#`→`53-T#` to stop colliding with the invariants. Invariants 53-I1–53-I13; failure modes 53-F1–53-F13; amendment register 53-A1–53-A13; 8 open questions (OQ-53-5 resolved, kept as a record). Verification status: DRAFT (Rev 2) — awaiting independent review. |
