# 24 — Agent Lifetime Errata: Session Ownership, Detach, Turn Quiescence, and Terminal-Event Forwarding (RB-21)

```
Status: written · verified: — · reviewer: —
Revision: 7 — Rev 1 repaired the four RB-21 lifetime/teardown defects with a
          reference-counted ownership model, park-on-close / join-on-shutdown,
          a queue-aware pending predicate, and a committed-record forwarding
          channel. Rev 2 incorporates three user decisions and closed an
          independent Oracle gate (1 HIGH, 9 MEDIUM, 5 LOW). The substantive
          addition was 24-D10: **detach is real and daemon-owned** — a
          non-last-supervisor detach never cancels a turn; only the last
          supervisor's exit finalizes (flush + terminal event) and closes the
          active sessions. Rev 2 also pinned the registry/session-map
          synchronization contract, the MCP/PTY child-teardown ordering, and
          every interface/test detail the gate flagged. No wire, schema,
          dependency, or path-safety change.
          **Rev 3 closes the second independent gate (1 HIGH, 3 MEDIUM,
          2 LOW).** The HIGH: the map mutex protects the maps but not the raw
          `Agent*` they return, so a coordinator-thread `finalizeAll` can free
          an agent an already-dispatched io handler is dereferencing. Rev 3
          pins the **strong-reference** fix (`findShared`/`getShared` held for
          the handler's whole duration, plus `sessionPtr` on the session side;
          24-D14, `AL30`/`AL31`, `AL-F22`) and enumerates every io-thread
          raw-`Agent*`/`Agent&`/`Session&` site (§7.2.1 in Rev 3; now the
          §7.2.2 checklist, H1–H8 + S1–S4). It
          also pins the last-exit session close
          (`finalizeAll` calls `closeSession`; 24-D15, `AL33`, `AL-F23`), the
          permission-hook remediation as one synchronized mechanism (weak
          `Session` capture + a gate hook mutex; 24-D16, `AL34`, `AL-F24`),
          the `WorkspaceRuntime::mcp()` accessor (24-D9), the owner/admission
          invariant (`AL32`, `AL-F25`), and corrects two stale claims (the
          `Impl` member order and defect 0's framing). No wire, schema,
          dependency, or path-safety change.
          **Rev 4 closes the third independent adversarial gate (1 HIGH,
          2 MEDIUM, 1 LOW) structurally, not by enumeration.** The HIGH — which
          had already failed twice for the same reason — is that Rev 3's
          "exhaustive" io-thread inventory still omitted the registry paths
          (`AgentRegistry::activateSession`/`suspendSession`/
          `requestCompaction`/`hasPendingWork`), each of which did
          `agents_.find(...)` and then called a method on the raw
          `agent->second`: a coordinator-thread `finalizeAll` can free that
          agent between the lookup and the call. Rev 4 removes the hazard **by
          construction**: the raw accessors `Agent& get`/`Agent* find` and the
          `SessionManager` `AgentLookup` closure are **deleted**; every caller
          must hold a `std::shared_ptr<AgentLoop>` (`findShared`/`getShared`) or
          `std::shared_ptr<Session>` (`sessionPtr`) for its whole use, copied
          under `mutex_` and used outside it; and `AgentRegistry::mutex_` is
          pinned to one map-only lock-scope rule (24-D17/24-D18,
          `AL35`/`AL36`, `AL-F26`/`AL-F27`/`AL-F28`). The §7.2.2 checklist is
          retained only as a *verification aid*, never as the safety
          argument (the structural proof is §7.2.1). Rev 4 also fixes
          `SubagentRunner` (hold `getShared` for the whole `run`; it is
          test-only), reconciles the three inconsistent lock-scope statements
          (§7.2/§3.3/§7.4), and corrects the `W1–W6` table. No wire, schema,
          dependency, or path-safety change.
          **Rev 5 closes the fourth independent adversarial gate (1 HIGH,
          2 LOW).** The HIGH: Rev 4's §7.2.1 point 6 claimed "no cached raw
          member points at a map-owned agent/session," but the shipped
          `TuiApp::agent_` (`Agent& agent_;`, `src/ui/ui_application.cpp`) was
          bound from `AgentRegistry::get(*created)` — a raw cached map-derived
          handle that violates `AL35`. Rev 4 neither enumerated its callers
          (`src/cli/headless.cpp`, `tests/support/agent_test_env.hpp`,
          `tests/unit/skills_wiring_test.cpp`, and the other test TUs) nor
          widened `AL-U22`/§7.2.2 past `src/host/`, so the scoped check could not
          catch it. Rev 5 fixes the claim **and the scope**: the dead
          `run_tui`/`TuiApp` path is removed (verified dead — no caller in
          `src/`; the live entry is `run_supervisor_entry` → `ui::run_supervisor`,
          `src/ui/supervisor.cpp`), the live `src/cli/headless.cpp` path binds
          `getShared` to a named local held for the whole function, the
          test-support helpers return owning handles, and `AL-U22`/§7.2.2 become
          a **repo-wide** invariant over `src/`, `tests/`, and `include/`
          (24-D19, `AL37`, `AL-F29`). A check scoped to one directory is what let
          this HIGH through twice. Rev 5 also qualifies §7.4's "map-only" claim
          to lock 1 only (lock 2 is held across `deleteSession`'s erase+publish)
          and makes §7.2.2 H2 match §2.2/§13 item 4 (`closeSession` removes the
          PTY and session-close calls too). No wire, schema, dependency, or
          path-safety change.
          **Rev 6 closes the fifth independent adversarial gate (2 MEDIUM,
          2 LOW, 0 HIGH).** The gate confirmed the structural argument holds
          after the Rev 5 deletions; the remaining findings are
          documentation-consistency and enumeration defects, fixed precisely
          without changing the structure. (1) `SessionManager::session` is no
          longer described as deleted anywhere: it is **private** and retained
          for internal use (§3.1, `AL30`/`AL35`/`AL37`, 24-D17, §7.2.1 point 7);
          the deleted set is the public accessors only. (2) The non-host
          checklist is completed and re-verified against the tree:
          `host_runtime_test.cpp` has 14 raw `sessions().session(...)` sites
          (not 2), and `agent_registry_test.cpp`'s `DurableAgentEnv` helpers,
          the `SubagentRunner` parent sites, `session_test.cpp`'s 11
          `manager.session(...)` sites, and the `Agent*` aliases are added
          (X6/X8/X10). (3) §7.2.1 point 6 now states that
          `SubagentRunner::parent_` derives from an owning handle anchored by
          the parent agent. (4) §7.2.3's removal list gains the stale
          `run_tui`/`"main"` comments in `session.hpp` and
          `workspace_runtime.hpp`. The repo-wide check is **mechanized**: the
          build fails by construction on a named deleted/private accessor, and
          `AL-U22` is pinned as a runnable CTest tree scan for the residual
          `.get()`/`*` case (§7.2.1 point 7). No structural, wire, schema,
          dependency, or path-safety change. Header status stays
          `written · verified: —`.
Component: 24 (errata) — amends 01-session.md, 02-persistence.md,
           04-workspace-host-daemon.md, 05-transport.md, 06-agent-loop.md,
           11-m2-errata.md, 16-daemon-ownership.md,
           22-switcher-sessions-errata.md, 23-session-lifecycle-errata.md
Depends on: 00-architecture.md §8.3/§9.8/§9.9/§54; 01-session.md (verified),
            02-persistence.md (verified), 04-workspace-host-daemon.md (verified),
            05-transport.md (verified), 06-agent-loop.md (verified),
            11-m2-errata.md (verified), 16-daemon-ownership.md (verified),
            17-ui-transcript-errata.md (verified),
            19-session-rename-errata.md (verified),
            22-switcher-sessions-errata.md (verified),
            23-session-lifecycle-errata.md (verified)
Scope: (D1) The four verified RB-21 defects, repaired as one coherent lifetime
       model. (D2) The ownership model: reference-counted vs.
       quiescence-barrier `Session` ownership. (D3) The teardown contract:
       park vs. join, plus the user's detach decision. (D4) The
       `hasPendingWork` / `TurnExecutor` capacity predicate. (D5) The
       terminal-event forwarding path. (D6) The user's detach/close decision
       (24-D10): detach is non-destructive and daemon-owned; the last
       supervisor's exit finalizes and closes active sessions. **This spec is
       independent of spec 23**: it neither relies on nor changes spec 23's
       creation-path/cleanup/prune behavior. It *revives the concepts* of the
       withdrawn 23 Rev 5 lifetime work (§1.3.1) as a fresh, convergent design;
       the Rev 5 text itself is not re-pinned.
          **Rev 7 closes the independent test-adequacy review of the RB-21
          work (1 HIGH test-plan falsehood, 1 HIGH missing memory-safety CI,
          1 MEDIUM vacuous `AL-U20`, 1 MEDIUM weak guard, 3 LOW).** It rewrote
          §11.1–§11.7 to cite only tests that exist, implemented the named
          missing tests (`AL-U1`/`AL-U18`/`AL-U15`/`AL-U3`/`AL-U14`/`AL-U20`/
          `AL-S1`/`AL-I12`), added `YMH_ASAN` + a CI workflow (ASan/TSan), and
          recorded the residual gaps as accepted risks in §13.1. No structural,
          wire, schema, dependency, or path-safety change; see §15 for the
          full log.
Supersedes: none. No prior normative text is withdrawn by this spec.
Amends:     01 §5 (EventBus), §8 (SessionManager), §9.5, I16, I18, I20;
            02 §4.7 (`eraseWithEvent` return); 04 §3.5/H12; 05 §7.4, §8.4, T7;
            06 §3.4, A1, A14, A15, A-F12; 11 §3.2 (TurnExecutor sketch), E6, E7,
            §11.3; 16 §4.3 step ordering and step 5 (child teardown), §5.1
            budget; 22 §3/§4 (detached-session reachability); 23 §5.4
            (mid-turn guard)
```

This document is the design gate for the **RB-21 hardening effort**. It repairs
four defects that ship today in `src/` and are independent of every
session-lifecycle feature, plus the user's **detach** decision (Rev 2). The four
defects are individually small; they are one spec because they share a single
root cause: **the daemon destroys objects while a `TurnExecutor` worker body may
still be touching them, and it derives observation by re-reading a store it has
already mutated.** The detach decision is in scope because the shipped
`session.close` implementation is the same lifetime bug seen from the other
side: it cancels a turn the daemon owns.

This spec does **not** rewrite the amended specs. It amends them by reference:
every changed clause is quoted by symbol (and, where useful, `file:line`), and
its replacement text is given here. The convention matches specs 11, 16, and 23.

**Naming note.** Invariants local to this spec are **`AL1`–`AL37`**; failure
modes are **`AL-F1`–`AL-F29`**; decisions are **`24-D1`–`24-D19`**. The prefixes
`H`, `T`, `U`, `R`, `A`, `X`, `Q`, `P`, `S`, `C`, `M` are taken by specs 01–15;
`L` is taken by 08 (LLM provider); `O` by 16; `SL` by 23. This spec therefore
uses `AL` (agent lifetime). Review-defect ids `D1`–`D26` (spec 11) and
architecture decision ids `D1`–`D23` (spec 00 §54) keep their existing meaning;
where this spec pins a new decision it is written `24-Dn`, never a bare `Dn`.

**Citation rule (mandatory for this spec).** Symbols are cited by qualified
name (`Class::method`, member, file path). Line numbers are **hints only** and
must be re-derived before code; the gate rejects stale line citations. Where a
clause is quoted from a verified spec, the clause id is cited, not a line.

---

## 1. Purpose, scope, and supersession map

### 1.1 The defects (re-verified against the shipped tree)

The RB-21 audit (`REQUIREMENTS_BACKLOG.md` §RB-21) recorded four defects. Each
was re-verified in the tree while writing Rev 1 and again for Rev 2; defect 0
was re-verified and **reframed** for Rev 3. The line numbers below are the
audit's hints; the **symbols** are authoritative. Rev 2 adds defect 0 (the
detach bug), which the user's decision makes normative.

0. **The explicit `session.close` RPC cancels a turn the daemon owns (the detach
   bug).** `HostRuntime::closeSession` (`src/host/host_runtime.cpp`) closes the
   session PTY and calls `AgentRegistry::dispose`, which cancels the in-flight
   turn and erases the resident `Session`. Spec `05` §7.4 defines `session.close`
   as detach semantics — "the daemon keeps appending" — so the shipped
   cancel-on-close is the bug. The user's decision (Rev 2, 24-D10) makes detach
   authoritative. **Reframed in Rev 3 (LOW):** a *non-last supervisor exit* does
   **not** call `session.close` at all — `SupervisorApp::confirm_exit`
   (`src/ui/supervisor.cpp`) deregisters presence and calls
   `teardown_daemons(orphaning)` only for workspaces in its orphaning set, then
   quits; the connection drop is handled by `ProtocolServer::drop_client`
   (`src/transport/protocol_server.cpp`), which only clears that connection's
   subscriptions. The non-last-exit detach behavior is therefore **already
   correct** via spec 16's ownership model (`ProtocolServer::admit_shutdown`
   denies a non-last `host.shutdown{last_supervisor}`), and is not the bug. The
   bug is the *explicit* `session.close` RPC (and any `HostRuntime::closeSession`
   call) cancelling a daemon-owned turn. This spec keeps the detach behavior and
   fixes the framing; see `AL23`/`AL32`.
1. **`AgentLoop::dispose()` does not join the in-flight turn body.**
   `AgentLoop::dispose()` (`src/agent/agent_loop.cpp`) sets `disposed_`, cancels
   via `turn_cancel_.cancel()`, clears `inbox_`, sets `pending_maintenance_failure_ = false`,
   sets `state_ = AgentState::Idle`, sets `running_ = false`, and calls
   `flushIdleCallbacks()`. It never waits for the in-flight `runTurn` body to
   return. `AgentRegistry::dispose` (`src/agent/agent_registry.cpp`) then calls
   `services_.sessions->closeSession(sessionId)`, which erases the
   `std::unique_ptr<Session>` from `SessionManager::sessions_` and frees it —
   while a `TurnExecutor` worker may still be inside `Session::append` /
   `Session::appendEventLocked`. This is a **latent use-after-free**. A second
   symptom: `dispose()` sets `running_ = false` and `state_ = Idle` while the
   body still runs, so `hasPendingWork()` and `whenIdle()` observe false
   quiescence.
2. **Daemon teardown detaches workers past `shutdown_grace`.**
   `TurnExecutor::drain` (`include/ymh/agent/turn_executor.hpp`) waits on
   `state_->exited == max_workers_` until the grace deadline; when the deadline
   passes it calls `worker.detach()`. `WorkspaceHost::Impl::coordinator`
   (`src/host/workspace_host.cpp`) then destroys `host_runtime_`/`runtime_`,
   which a detached body may still reference through captured `this`.
3. **`AgentRegistry::hasPendingWork()` is blind to the `TurnExecutor` queue.**
   Between `submit()` and the body's first instruction, a prompt is pending only
   in the executor queue; `AgentRegistry::hasPendingWork`
   (`src/agent/agent_registry.cpp`) consults only `AgentLoop::hasPendingWork`,
   so `HostRuntime::deleteSession`'s mid-turn guard lets a delete through and
   the just-submitted prompt is dropped. Spec 23 §5.4's guard inherits this gap.
4. **`HostRuntime::handleCommittedEvent` re-reads the store.**
   `HostRuntime::handleCommittedEvent` (`src/host/host_runtime.cpp`) receives a
   bus `Event`, then re-reads `runtime_.store().readAfter(...)` to obtain an
   `EventRecord` (with `Sequence`). For the administrative delete,
   `SessionManager::deleteSession` calls `store_->eraseWithEvent(id, ended)` —
   the `SessionEnded` row is erased in the same transaction — then publishes the
   `Event`; the re-read is empty and the terminal event is **dropped at the
   transport boundary** (`01` I16 / `05` §7.4 not satisfied).

### 1.2 What this changes, in one sentence

The daemon stops owning `Session` and `AgentLoop` as immediately-destroyed
`unique_ptr`s and starts owning them as **reference-counted** objects whose last
reference is the in-flight turn body itself; **detach becomes non-destructive
and daemon-owned** (24-D10), close/delete **park**, daemon shutdown **finalizes
then joins** (never detaches) and hard-exits without destructors if the grace
expires, the pending predicate **sees the executor queue**, the registry/session
maps get a **mutex + total lock order**, and durable events **carry their store
`Sequence` on the bus** so the forwarder never re-reads a row it may have
erased.

### 1.3 Supersession map

#### 1.3.1 Superseded / withdrawn

| ID | Prior text | Change |
|---|---|---|
| 23-S5 (Rev 6) | The withdrawn Rev 2–5 **draft lifetime model**: `Session::draft`, the shared-ownership rework, the `AgentLoop::dispose` completion barrier, `TurnExecutor` join/park, and the `SessionEnded` forwarding fast-path (23 §1.3.1). | **Not re-pinned.** This spec revives the *concepts* (refcount, park, join, terminal forwarding) as a fresh design and rejects the specific mechanisms that made Rev 5 non-convergent (§2.6). |
| 23 §13.1 | "The separate hardening effort (RB-21) — NOT load-bearing for spec 23". | **Discharged by this spec.** Spec 23's own behavior is unchanged. |
| 05 §7.4, last sentence of the `session.close` bullet (as implemented) | The shipped `HostRuntime::closeSession` cancel-on-close behavior. | **The implementation was wrong; 05 §7.4's detach text is authoritative** (24-D10). `HostRuntime::closeSession` becomes detach-only. |

Nothing else is superseded. This spec does **not** touch the creation path,
`session.create`, the TUI eager-create sites, `ymh session prune`, or the
enumeration filters — all of spec 23 stays as verified.

#### 1.3.2 Amended

| ID | Amended clause | Verified anchor | New behavior |
|---|---|---|---|
| 24-D1 | 01 §8 `SessionManager`, §5 `EventBus` ownership | `SessionManager::sessions_`; `EventBus::publish` | `Session` is `shared_ptr`-owned; a committed-record bus channel carries `EventRecord` |
| 24-D2 | 06 §3.4 `dispose()`, A14 | `AgentLoop::dispose`; `AgentRegistry::dispose` | `dispose()` is a non-blocking **park**; `AgentLoop`/`AgentRegistry` are `shared_ptr`-owned |
| 24-D3 | 04 §3.5/H12, 16 §4.3 step 9 | `TurnExecutor::drain`; `Impl::coordinator` | `drain` never detaches; returns `Quiesced`/`TimedOut`; timeout hard-exits |
| 24-D4 | 11 §3.2/E6/E7 | `TurnExecutor::submit`/`inFlight` | Per-session task identity; queue-aware `inFlight(session)` |
| 24-D5 | 23 §5.4 mid-turn guard, 11 §12.4 | `HostRuntime::deleteSession` | Guard uses `HostRuntime::hasPendingWork` (agent OR executor queue) |
| 24-D6 | 05 §7.4/§8.4, 01 I16, 02 §4.7 | `Session::appendEventLocked`; `SessionManager::deleteSession` | Durable appends publish a committed `EventRecord`; forwarder never re-reads |
| 24-D7 | 01 I18, 06 A1 | `AgentLoop` control state | Atomics + leaf `control_mutex_`; no lock across blocking waits |
| 24-D8 | 16 §4.3 steps 8–9 | `Impl::coordinator` | Drain before `TransportServer::stop()` (shipped order) |
| 24-D9 | 04 §3.5 step 3, 16 §4.3 step 5 | `Impl::coordinator`; `WorkspaceRuntime` | Explicit PTY **and** MCP child teardown before transport stop; `WorkspaceRuntime::mcp()` accessor (Rev 3) |
| 24-D10 | 05 §7.4, 16 §2.4/§4.3, 22 §3/§4 | `HostRuntime::closeSession`; `AgentRegistry::finalizeAll` | Detach is non-destructive and daemon-owned; last exit finalizes + closes |
| 24-D11 | 01 §8, 06 §4.1 (new) | `AgentRegistry::mutex_`; `SessionManager::mutex_` | Registry/session maps are mutex-guarded; total lock order pinned (§7.4) |
| 24-D12 | 01 §5 (new) | `EventBus::Subscription` | Committed subscriptions carry a channel kind and unsubscribe correctly |
| 24-D13 | 01 §8 (new) | `SessionManager::loadInto` | Idempotent: never replaces a resident `Session` |
| 24-D14 | 06 §4.1, 11 §12.4 (Rev 3) | `AgentRegistry::findShared`; `HostRuntime` handlers | **io-handler strong reference:** every handler that dereferences an agent holds `findShared`'s `shared_ptr` for its whole duration; no raw `Agent*` escapes |
| 24-D15 | 05 §7.4, 16 §2.4/§4.3 (Rev 3) | `AgentRegistry::finalizeAll`; `SessionManager::closeSession` | **Last-exit close is explicit:** `finalizeAll` finalizes + parks + `closeSession`s each resident session; process exit is the final free |
| 24-D16 | 09 §4.2, 06 §3.4 (Rev 3) | `AgentLoop::executeToolCall`; `PermissionGate` hooks | **Permission hook:** weak `Session` capture + a gate hook mutex; `dispose()` never clears the hook |
| 24-D17 | 06 §4.1/A15, 01 §8 (Rev 4) | `AgentRegistry::findShared`/`getShared`; `SessionManager::sessionPtr` | **No raw map-derived handle exists.** `Agent& get`/`Agent* find` and `SessionManager::AgentLookup`/`setAgentLookup`/`findAgent`/`agent` are deleted, and the public `session()` accessor is **made private** (retained for internal use, §3.1); `shared_ptr` is the only handle and is held for the caller's whole use |
| 24-D18 | 06 §4.1, 01 §8 (Rev 4) | `AgentRegistry::mutex_` | **Single lock-scope rule:** the registry mutex is held only for map operations; never across a call into `AgentLoop`/`Session`/`SessionManager`/provider/permission/executor, a predicate/hook, or a `shared_ptr` destructor |
| 24-D19 | 06 §4.1/A15, 17 §1, 19 §1 (Rev 5) | `TuiApp::agent_`; `src/cli/headless.cpp`; test support | **Repo-wide structural scope + dead-path removal:** the no-raw-handle rule binds every translation unit, not just `src/host/`; the live `headless.cpp` caller binds `getShared` to a named local, test helpers return owning handles, and the dead `run_tui`/`TuiApp` path (the `Agent& agent_` cached member) is removed |

#### 1.3.3 Retained (explicitly not changed)

- **One active session per workspace** (04 (m), A8) and focus-independent
  activation (A7): unchanged.
- **The bounded `TurnExecutor` pool and queue** (`max_workers` =
  `ResourceCaps::max_llm_concurrency`, `queue_capacity` = same; E7): unchanged;
  the queue remains the only backpressure.
- **`session.close` emits no `SessionEnded`** (01 I16): retained.
- **`session.delete` = destructive, emits `SessionEnded{Deleted}`** (01 I16):
  retained; this spec makes the emission observable at the transport boundary.
- **No schema change** (`registry.db`/`sessions.db` stay at schema `1`), no wire
  change (`kProtocolVersion` unchanged), no new dependency.
- **Path safety** (`ExecutionEnvironment::resolve()`, never `getcwd()` in tool
  code): unchanged.

### 1.4 Scope boundaries

In scope: defects 0–4; the ownership/teardown/predicate/forwarding/detach
decisions; the map-synchronization contract; the child-teardown ordering; the
worker-side `this`-capture audit (§7); the amendments in §12.

Out of scope (do not design here): session creation, cleanup-on-exit,
`session prune`, enumeration filters (spec 23); remote transport (explicitly out
of the project); LSP tools; any UI/golden change beyond spec 22's existing
live-only switcher; any performance work on the event bus.

### 1.5 Terminology (pinned)

- **Turn body.** The `std::function<void()>` submitted to `TurnExecutor`; it
  runs synchronously on a worker and may call `AgentLoop::activate`.
- **Park.** Erase ownership references without waiting. The object stays alive
  until its last strong reference drops; the last drop frees it.
- **Join.** Block the coordinator until every worker has returned. Never
  `detach()`.
- **Quiesced.** `TurnExecutor` has no queued body and no active body; all
  workers have observed `stopping` and incremented `exited`.
- **Detach.** End observation of a session (explicit `session.close` or a
  supervisor connection drop). Never mutates the daemon's agent/session/turn
  state (24-D10).
- **Finalize.** Last-supervisor teardown step: cancel each in-flight turn so its
  cancel path flushes the pending chunk batch and appends exactly one terminal
  event ("saved into history"), then park the agent. No `SessionEnded`.
- **Committed record.** An `EventRecord{seq, event}` published on the committed
  channel after the store commit, carrying the store-assigned `Sequence`.
- **Live event.** An `Event` published with no store `Sequence` (e.g.
  `McpServerStatusChanged` via `Session::emit` / `McpManager`).
- **Owner (of a capture).** The object whose `this` a callback or body captures.

---

## 2. Decision record

The backlog and the user require the following decisions. `§2.5` gives the
reference graph; `§2.6` explains why this design converges where the withdrawn
23 Rev 5 model did not.

### 2.1 (a) `Session` ownership — **reference-counted** (24-D1)

**Chosen.** `SessionManager::sessions_` becomes
`std::unordered_map<std::string, std::shared_ptr<Session>>`. `AgentLoop` holds a
strong `std::shared_ptr<Session>` anchor for its whole life (in addition to its
existing `Session&` convenience reference). A `Session` is freed only when the
last strong reference drops; the last reference may be held by an in-flight turn
body (via its agent).

**Why.** The in-flight body reaches the `Session` through `AgentLoop::session_`.
If the agent holds a strong reference, the `Session` **cannot** be freed while
the agent (or a body holding the agent) is alive. This makes the use-after-free
impossible *by construction*: there is no interleaving in which the manager's
`erase` frees an object a body still reaches. A barrier would have to be
re-proved at every call site; a refcount is a single invariant (`AL1`).

**Rejected: quiescence-barrier ownership of `Session`.** Three fatal problems,
all observed in the withdrawn Rev 5 design (§2.6):

1. **io-thread deadlock.** `closeSession`/`deleteSession` run on the transport
   io thread. A worker can be blocked in `PermissionBroker::resolve` →
   `outcome.get()`, resolved by `HostRuntime::decidePermission` **on the io
   thread**. A barrier on the io thread would wait for a worker that waits for
   the io thread.
2. **Unbounded stall.** A provider/tool that ignores its `CancellationToken`
   would stall the io thread for the whole network timeout (M-F4).
3. **Self-deadlock.** The Rev 5 completion latch combined with `whenIdle`
   deferral produced a verified self-deadlock (23 Rev 6 log).

**Cost.** Internal only: a map value type, a constructor signature, and a mutex
(§7.4). `Session` is already non-copyable, non-movable, and heap-stable; no
`Session` interface changes. No wire, schema, or dependency change.

### 2.2 (b) Teardown contract — **detach ≠ close ≠ shutdown** (24-D3/24-D10)

**Chosen.** Three distinct operations, never overloaded (mirrors 01 §9.5):

1. **Detach** (explicit `session.close`, or a supervisor connection drop / a
   non-last supervisor's exit). Ends observation only. `HostRuntime::closeSession`
   performs **no** agent/PTY/session mutation; `ProtocolServer` already ends the
   calling client's subscription (`end_subscriptions(conn, session, "session_closed")`).
   The daemon is unaffected; the in-flight turn **keeps running** (24-D10).
2. **Close / disposal** (agent teardown, `session.delete`, daemon teardown).
   **Park.** `AgentLoop::dispose()` marks disposed, cancels, clears the inbox
   under the leaf lock, and returns immediately. `AgentRegistry::dispose` erases
   ownership references and the manager's session reference. No thread waits.
3. **Daemon shutdown** (the **last** supervisor's exit; spec 16 tears the
   orphaned daemons down). **Finalize then join.** `AgentRegistry::finalizeAll()`
   cancels each in-flight turn (its cancel path flushes the pending chunk batch
   and appends exactly one terminal event — "saved into history") and parks;
   `TurnExecutor::drain(grace)` joins (never detaches).
   - `Quiesced` → the coordinator continues the ordered teardown; destructors
     are safe.
   - `TimedOut` → best-effort socket unlink and `std::_Exit` **without running
     destructors**. No `unique_ptr` frees an object a live worker may touch.

**Why.** Detach must not mutate daemon state because the turn's lifetime is
daemon-owned (24-D10). Park makes close non-blocking (no io-thread deadlock).
Join at shutdown is required because the `Impl` members are about to be
destroyed; the hard exit is the only safe action when a worker cannot be joined,
and it matches spec 16's watchdog (`std::_Exit(HostExitCode::Internal)` after
`kOwnerGraceTeardownBudget`, 16 §5.1).

**Rejected: cancel-on-detach (the shipped behavior).** It contradicts 05 §7.4,
kills work the daemon owns, and makes a detached session uncontrollable.

**Rejected: detach-then-destroy.** The detached body touches freed state.

**Rejected: join forever with no bound.** One uninterruptible tool would hang
the daemon forever, violating spec 16's bounded unsupervised lifetime (O15).

**Rejected: park at shutdown too.** Parking at shutdown leaks every agent and
session into an exiting process and still makes `Impl` destruction unsafe if the
parked refs are never dropped before the members are destroyed.

#### 2.2.1 24-D10 — detach is real and daemon-owned

**24-D10 (pinned).**

- **The turn's lifetime belongs to the daemon, not the supervisor.** No
  supervisor detach cancels, disposes, or erases a resident `Session`.
- **A non-last supervisor's exit is a detach.** The daemon survives (spec 16
  collective ownership) and the active session continues to run in **detached
  mode**.
- **The last supervisor's exit is the only finalize+close point.** The
  coordinator's `finalizeAll()` flushes/finalizes in-flight turns and closes the
  resident sessions via `SessionManager::closeSession` (24-D15; no
  `SessionEnded`; close ≠ delete). Spec 23's unprompted-session cleanup runs
  before this and is unaffected.
- **A detached session is discoverable and controllable from a still-running
  supervisor.** It lives on a live daemon, so spec 22's live-only Ctrl-S
  switcher surfaces the workspace and `/sessions` lists the stored session;
  `session.resume` returns the resident agent (idempotent), and
  `session.activate` / `agent.prompt` drive it from the other supervisor.
  **One resident `Session` per id** (24-D13): `resumeSession` must not replace
  the running object.

**Interaction with spec 16.** Spec 16 already keeps the daemon alive while
another owner exists and only tears it down on the last exit / watchdog. 24-D10
adds the *turn-level* rule: teardown finalizes rather than discards, and detach
does not stop work. The owner watchdog is unchanged; an ownerless daemon still
tears down after `owner_grace`, and that teardown now finalizes active turns.

**Interaction with spec 22.** No new UI is required. Spec 22's Live switcher
already shows live daemons and evicts dead ones; a detached-but-running session
is on a live daemon, so it is reachable. `/sessions` reads stored history
directly. This spec only pins that the detached session's live status renders
like any other live session and that selecting it attaches + resumes idempotently.

### 2.3 (c) `hasPendingWork` / `TurnExecutor` capacity predicate — **queue-aware OR** (24-D4/24-D5)

**Chosen.** `TurnExecutor` tags each task with its `SessionId` and exposes
`inFlight(const SessionId&)` = queued-for-session + active-for-session. A new
`HostRuntime::hasPendingWork(id)` is:

```cpp
bool HostRuntime::hasPendingWork(const SessionId& id) const {
    return runtime_.agents().hasPendingWork(id) || turns_.inFlight(id) > 0;
}
```

`HostRuntime::deleteSession`'s mid-turn guard uses it (plus the blocked-state
check). The queue **capacity** predicate is unchanged: `submit` returns `false`
iff `queue.size() >= queue_capacity_` or `stopping`; the caller maps that to
`RpcCode::InternalError` / `AgentErrorCode::InboxFull`.

**Why.** A prompt is pending in two places: the agent inbox/turn (visible to
`AgentLoop::hasPendingWork`) and the executor queue/active slot (invisible
today). The OR closes the gap. Because RPC dispatch is single-threaded on the io
thread, the check-then-act in `deleteSession` is atomic with respect to
`agentPrompt`/`agentFollowup`/… — no concurrent submit can slip between the
guard and the delete. The only concurrency is the worker itself, which
`inFlight(id) > 0` covers for the **entire** body duration.

**Rejected: split `Agent::send` into an io-thread `enqueue` plus a worker
`activate`.** It changes the pinned `Agent` interface (06 §3), moves inbox
mutation onto the io thread (a new race), and has a much larger blast radius.

**Rejected: count only the executor queue.** Insufficient: a running turn with
an empty executor queue must also refuse a delete.

### 2.4 (d) Terminal-event forwarding path — **committed-record channel** (24-D6/24-D12)

**Chosen.** Add a record-carrying channel to `EventBus`:

- `EventBus::subscribeCommitted(RecordHandler)` — a global subscriber that
  receives `const EventRecord&`; its `Subscription` carries a channel kind so it
  unsubscribes correctly (24-D12).
- `EventBus::publishCommitted(const EventRecord&)` — delivers the `Event` to the
  existing `Event` handlers **and** the record to the committed handlers, and
  fans out to the per-session mailbox exactly as `publish` does today.
- `Session::appendEventLocked` / `Session::appendBatch` call
  `publishCommitted(EventRecord{seq, event})` (they already know `seq`).
- `SessionManager::deleteSession` calls `publishCommitted(EventRecord{seq, ended})`
  with the `Sequence` returned by `eraseWithEvent` (whose return type changes
  from `void` to `Sequence`; this is **source-breaking** for implementers of the
  `SessionStore` seam — the in-tree fakes are updated — but not a wire/schema
  change).
- `HostRuntime` subscribes via `subscribeCommitted` and forwards the record
  directly; it no longer calls `readAfter`. The live-only MCP branch moves to a
  separate `subscribe` on the live channel.

**Why.** The forwarder needs the store `Sequence` (the cursor is a durable
position; 05 §5.5/T23). `events.sequence` is a **global** `INTEGER PRIMARY KEY
AUTOINCREMENT`, so per-session sequences have gaps and `last + 1` is wrong.
Re-reading the store fails precisely when the row was erased in the same
transaction (defect 4). Carrying the record from the append site is exact.

**Rejected: `headSequence(id) + 1` synthesis.** Fragile and wrong for a
multi-writer store.

**Rejected: a side map `event_id → seq`.** Stateful, needs GC, duplicates
information the append site has.

### 2.5 The reference graph (no cycles) — AL3

```text
WorkspaceRuntime
  ├─ SessionManager ── shared_ptr ──► Session
  ├─ AgentRegistry  ── shared_ptr ──► AgentLoop ── shared_ptr ──► Session
  └─ EventBus (shared state)
TurnExecutor queue/active
  └─ task body ── shared_ptr ──► Agent (AgentLoop)   [captured at submit]
```

Every edge points **downward**: task → agent → session; registry → agent;
manager → session. There is **no** back-edge (no agent → registry, no
agent/session → executor, no `Session` → `Agent`). The strong graph is a DAG, so
no `shared_ptr` cycle can leak. This answers the Rev 5 "self-cycle leak from
strong `shared_ptr` captures" finding: the fix is not "avoid `shared_ptr`", it
is "never create an upward strong edge".

**Rev 4 removes the one raw back-edge that could outlive its target.**
`SessionManager::setAgentLookup` (a stored `std::function` capturing
`AgentRegistry*` **raw** and re-reading the registry map on every call) is
**deleted** together with `AgentLookup`/`findAgent`/`agent` (24-D17). It was the
only stored callback that could name a map-owned agent without the registry
`mutex_` and hand out a raw `Agent*` to a caller whose use could outlive the map
entry. The remaining raw back-edges are plain non-owning service pointers
(`AgentLoop::services_`; `Session`'s `SessionStore*`/`EventBus*`;
`ToolContext::session_`): each target outlives the body (join) or the process
hard-exits, and none is a map-derived agent/session handle.

### 2.6 Why this converges where 23 Rev 5 did not

| Rev 5 break | How 24 avoids it |
|---|---|
| Self-deadlock in the completion latch + `whenIdle` deferral | **No latch.** Park uses refcounts; `dispose` never waits, never calls `whenIdle`. |
| Unimplementable `void drain(grace)` | `drain` returns `DrainResult`; the timeout is representable (24-D3). |
| Self-cycle leak from strong `shared_ptr` captures | The strong graph is acyclic by construction (§2.5, `AL3`). |
| Underspecified "park" | "Park" is pinned: erase ownership refs; the last strong ref frees (§1.5, `AL4`). |
| Wrong-branch fix for `SessionEnded` forwarding | The terminal record is published at the erase site with the real `Sequence` (§2.4, `AL16`/`AL17`). |
| Close-guard blind to the `TurnExecutor` queue | The guard is queue-aware (§2.3, `AL6`/`AL7`). |

---

## 3. Defect 1 — Session ownership and the `dispose` park

**Invariant that makes it impossible:** `AL1` (reference-counted `Session`
ownership), `AL2` (reference-counted agent ownership with strong refs in
executor bodies), `AL4` (park-on-close), `AL5` (dispose does not falsify
quiescence), `AL24` (one resident `Session` per id).

**Exact seams to change.**

1. `SessionManager` (`include/ymh/session/session_manager.hpp`):
   - `sessions_` becomes `std::unordered_map<std::string, std::shared_ptr<Session>>`.
   - Add `std::shared_ptr<Session> sessionPtr(const SessionId&)` (throws
     `UnknownSession`), used by `AgentRegistry::registerAgent` and
     `SubagentRunner`. It is the **only public accessor** that yields a
     `Session` handle (24-D17).
   - `session(const SessionId&)` returns `Session&` but becomes **private**: it
     is used only inside the manager (under `mutex_`) or by a caller that
     already holds a `sessionPtr` strong reference for the object. No external
     code may hold a raw `Session&` across a call that can erase its map entry.
   - **Delete `AgentLookup`/`setAgentLookup`/`findAgent`/`agent` and the
     `agent_lookup_` member** (24-D17). This stored `[this]`-capturing closure
     re-read the registry map outside the registry `mutex_` and returned a raw
     `Agent*`; it has no production caller (`findAgent`/`agent` are used only by
     `tests/unit/agent_registry_test.cpp`), so deletion is safe and removes the
     last path by which a raw map-derived agent handle could escape.
   - `loadInto` becomes **idempotent** (24-D13): if `sessions_.find(id)` exists,
     return the resident `Session&` and do not replace it. This is required by
     24-D10 (a second supervisor must not create a second resident `Session`).
   - `closeSession`/`createSession`/`forkSession` adjust to `make_shared` / `erase`.
2. `AgentLoop` (`include/ymh/agent/agent_loop.hpp`, `src/agent/agent_loop.cpp`):
   - Constructor takes `std::shared_ptr<Session>`; add member
     `std::shared_ptr<Session> session_owner_` **declared before** `Session& session_`.
   - `dispose()` becomes a park: set `disposed_`, cancel the turn, clear the
     inbox under `control_mutex_`, clear `pending_maintenance_failure_`, and
     **return without waiting**. It must **not** set `running_ = false` or
     `state_ = Idle` while a body runs (`AL5`); the worker sets them when
     `activate()` exits.
   - **Delete the dead `AgentLoop::run(CancellationToken)`** (see §7.3 C7); it is
     not on the `Agent` interface and has no production caller.
   - `~AgentLoop` is only reached after the last body reference drops.
3. `AgentRegistry` (`include/ymh/agent/agent_registry.hpp`,
   `src/agent/agent_registry.cpp`):
   - `agents_` becomes `std::unordered_map<std::string, std::shared_ptr<AgentLoop>>`.
   - **Delete `Agent& get(AgentId)` and `Agent* find(SessionId)`** (24-D17). They
     are the raw map-derived handles the Rev 3 HIGH is about; no caller may hold
     one across a `mutex_` release. Every caller is migrated to the shared
     accessors below.
   - Add `std::shared_ptr<AgentLoop> findShared(SessionId) noexcept` (24-D2/24-D14):
     lock `mutex_`, copy the `shared_ptr`, unlock, return (null when absent). The
     returned reference outlives the map entry. Add the `AgentId`-keyed sibling
     `getShared(AgentId) noexcept` for the `create` path. These two are the
     **only** functions that yield an agent handle (24-D17).
   - Add `finalizeAll()` (24-D10/24-D11/24-D15): snapshot the `shared_ptr`s under
     `mutex_`, **release**, then `dispose`/`closeSession` each, then re-lock to
     erase (24-D18; §7.2/§7.4).
   - `registerAgent` resolves `std::shared_ptr<Session> session = services_.sessions->sessionPtr(sessionId)`
     **before** taking `mutex_`, then inserts both maps under `mutex_`; it never
     nests the registry lock over the manager lock (24-D18).
   - `dispose(id)`: copy the `shared_ptr` out under `mutex_`, release, then
     `agent->dispose()` (park) and `services_.sessions->closeSession(sessionId)`
     (drops the manager ref), then re-lock to erase `bySession_`/`agents_`/
     `leases_`. The local strong copy is what keeps the agent alive across the
     unlocked `dispose()`, and it means the erased object's destructor runs
     **after** `mutex_` is released (24-D18).
4. `HostRuntime` (`src/host/host_runtime.cpp`): every `turns_.submit` body that
   touches an agent holds a strong `std::shared_ptr<AgentLoop>` for the body's
   whole duration, obtained in exactly one of two ways:
   - the four message bodies (`agentPrompt`, `agentFollowup`, `agentSteer`,
     `agentInject`) call `findShared(id)` at the top of the body and call
     `agent->prompt(...)`/`followup`/`steer`/`inject` on the strong reference; or
   - `activateSession`/`compactSession` call the registry methods
     `AgentRegistry::activateSession(id)`/`requestCompaction(id)`, which acquire
     and hold their own strong reference internally (R1/R3, §7.2.2).
   The shipped bodies instead call `runtime_.agents().find(id)` and dereference
   the raw `Agent*` (W1–W6). No worker body holds a raw map-derived handle:
   each either holds a `shared_ptr` or delegates to a registry method that does
   (24-D17/24-D18). Worker threads still enter the registry only through
   `mutex_`-guarded accessors, so the contention `AL27`/§7.4 governs is
   unchanged in kind.
5. **io-handler strong-reference rule (24-D14; the Rev 3 HIGH).** Every
   `HostRuntime` RPC handler runs on the transport io thread through
   `HostRuntime::translate` (`include/ymh/host/host_runtime.hpp`), which only
   maps exceptions — there is **no io marshalling** — and
   `ProtocolServer::waitForDrain` (`src/transport/protocol_server.cpp`) counts
   only outbound bytes (`outstanding_total_`). A handler already dispatched
   therefore runs concurrently with the coordinator's `finalizeAll`, which
   erases `agents_` and calls `SessionManager::closeSession`. Each handler below
   dereferences an agent and MUST hold `findShared`/`getShared`'s strong
   reference for its whole body; handlers that dereference a session hold
   `sessionPtr` (the **verification checklist** is §7.2.2 — H1–H8, R1–R6,
   S1–S4, W1–W6; it is a checklist, not the safety argument; the structural
   proof is §7.2.1):
   - `HostRuntime::ensureAgent` — existence check `find(id) != nullptr`.
   - `HostRuntime::closeSession` — **removed** from this set: detach-only under
     24-D10, it no longer looks up an agent.
   - `HostRuntime::deleteSession` — the mid-turn `Agent* agent = find(id)` +
     `agent->state()` switch, and the later `find(id)` + `dispose(agent->id())`.
   - `HostRuntime::agentCancel` — `find(id)` + `hasPendingWork()` + `cancel()`.
   - `HostRuntime::agentStatus` — `find(id)` + `status()`.
   - `HostRuntime::createSession` — `get(*created).session()` (raw `Agent&`).
   - Session-side: `agentPrompt` (`maybeAutoName`), `renameSession`,
     `forkSession` hold `sessionPtr` across the manager call.
   - Registry paths (the sites Rev 3 omitted): `AgentRegistry::activateSession`,
     `suspendSession`, `requestCompaction`, `hasPendingWork`, `dispose`, and
     `finalizeAll` each hold a raw `agent->second`/`it->second` across the call
     in the shipped tree; Rev 4 makes each acquire a strong copy under `mutex_`
     and call on that copy (R1–R6).
   `HostRuntime::hasPendingWork` needs no strong reference **at the handler
   level**, but the registry method is now safe on its own:
   `AgentRegistry::hasPendingWork` copies the `shared_ptr` under `mutex_`,
   releases, then evaluates the predicate on the strong reference outside the
   lock (24-D18, R4). The predicate is therefore never called under `mutex_`;
   §7.2, §3.3, and §7.4 state this one rule identically.
6. `HostRuntime::closeSession` becomes **detach-only** (24-D10): it performs no
   agent/PTY/session mutation. `ProtocolServer` already ends the subscription.

**Failure mode if not fixed:** `AL-F1` (use-after-free on `Session::append`),
`AL-F5` (false quiescence), `AL-F15` (detach cancels a daemon-owned turn),
`AL-F21` (a second resident `Session`), `AL-F22` (a caller dereferences an agent
erased by `finalizeAll`), `AL-F26` (a worker body holds a raw `Agent&`/
`Session&`), `AL-F27` (a raw accessor or the deleted lookup closure is
reintroduced), `AL-F28` (`mutex_` held across a predicate/call).

### 3.1 Interface sketch — `SessionManager` (pinned)

```cpp
class SessionManager {
public:
    SessionManager(SessionStore& store, EventBus& bus);

    SessionId createSession(const SessionOptions& options);
    SessionId resumeSession(const SessionId& id);
    SessionId forkSession(const SessionId& parent, std::size_t seedLength);
    SessionId replaySession(const SessionId& id);

    // 24-D1: strong-reference accessor for the agent lifetime anchor.
    // Throws UnknownSession when the id is not resident.
    [[nodiscard]] std::shared_ptr<Session> sessionPtr(const SessionId& id);

    void closeSession(const SessionId& id);              // park: erase the manager ref
    void deleteSession(const SessionId& id, bool only_if_empty = false);

    [[nodiscard]] std::vector<SessionId> list() const;

    // 24-D17: no raw Session& escapes the manager. `session()` is private and
    // is used only under mutex_ or on a held sessionPtr; external callers use
    // sessionPtr. AgentLookup/setAgentLookup/findAgent/agent are DELETED — the
    // stored [this]-capturing closure re-read the registry map outside the
    // registry mutex_ and handed out a raw Agent* (24-D17).

private:
    // 24-D13: returns the resident Session unchanged when present.
    Session& loadInto(const SessionHeader& header);
    Session& session(const SessionId& id);  // private: under mutex_ or held sessionPtr

    SessionStore*                                             store_ = nullptr;
    EventBus*                                                 bus_   = nullptr;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;   // 24-D1
    mutable std::mutex                                        mutex_;      // 24-D11
};
```

### 3.2 Interface sketch — `AgentLoop` (pinned, amended surface)

```cpp
class AgentLoop final : public Agent {
public:
    // 24-D1: the loop owns a strong reference to its session for its whole life.
    AgentLoop(AgentId id, std::shared_ptr<Session> session,
              AgentServices services, AgentConfig config);
    ~AgentLoop() override;

    AgentId     id() const override;
    SessionId   session() const override;
    AgentStatus status() const noexcept override;
    AgentState  state() const noexcept override;
    bool        disposed() const noexcept override;
    bool        hasPendingWork() const noexcept override;

    InboxResult send(Message message) override;
    InboxResult followup(Message message) override;
    InboxResult steer(Message message) override;
    InboxResult inject(ContextMessage context) override;

    void cancel() override;
    void dispose() override;                  // 24-D2: non-blocking park
    void whenIdle(std::function<void()> callback) override;

    // AgentLoop-only surface used by HostRuntime/AgentRegistry bodies.
    // `activate()` and `suspend()` are PUBLIC in the shipped tree.
    // `run(CancellationToken)` is DELETED (dead; see §7.3 C7).
    std::expected<CompactionOutcome, AgentError> requestCompaction();
    void       activate();
    void       suspend();

    [[nodiscard]] const AgentConfig& config() const noexcept { return config_; }

private:
    AgentId                  id_;
    std::shared_ptr<Session> session_owner_;  // declared before session_
    Session&                 session_;
    AgentServices            services_;
    AgentConfig              config_;

    std::deque<InboxItem>               inbox_;              // control_mutex_
    std::vector<std::function<void()>>  idle_callbacks_;     // control_mutex_
    std::string                         cancel_reason_;      // control_mutex_
    bool                                pending_maintenance_failure_ = false;  // control_mutex_
    mutable std::mutex                  control_mutex_;      // leaf: no blocking op inside
    std::atomic<bool>                   disposed_{false};
    std::atomic<bool>                   running_{false};
    std::atomic<AgentState>             state_{AgentState::Idle};
    std::size_t                         compactions_this_turn_ = 0;  // worker-only
    CancellationSource                  turn_cancel_;
};
```

### 3.3 Interface sketch — `AgentRegistry` (pinned, amended surface)

```cpp
class AgentRegistry {
public:
    // ... constructors, create/resume/list/activeCount unchanged ...
    // 24-D17: `Agent& get(AgentId)` and `Agent* find(SessionId)` are DELETED.
    // The interface has no raw map-derived agent handle.

    // 24-D2/24-D14/24-D17: strong reference to the concrete loop
    // (activate/suspend/requestCompaction live on AgentLoop, not on Agent).
    // Returns null when absent. Locks mutex_ only long enough to copy the
    // shared_ptr; the returned strong reference keeps the agent alive after
    // mutex_ is released and after finalizeAll/dispose erase the map entry
    // (AL30/AL31/AL35). EVERY caller that dereferences an agent outside the
    // registry's own mutex_-held section — io-thread HostRuntime handlers,
    // worker bodies, SubagentRunner, and the registry's own activate/suspend/
    // requestCompaction/hasPendingWork/dispose/finalizeAll — must use this (by
    // SessionId) or getShared (by AgentId) and hold the result for the whole
    // use (24-D14).
    [[nodiscard]] std::shared_ptr<AgentLoop> findShared(SessionId id) noexcept;

    // 24-D14/24-D17: the AgentId-keyed sibling, for the create path
    // (`HostRuntime::createSession` uses `getShared(*created)->session()`).
    // Locks mutex_, copies, unlocks; null when absent.
    [[nodiscard]] std::shared_ptr<AgentLoop> getShared(AgentId id) noexcept;

    // 24-D18: each of these copies the shared_ptr under mutex_, RELEASES, then
    // calls the agent method outside the lock; the registry mutex is a
    // map-only lock. R1–R4 in §7.2.2.
    void activateSession(const SessionId& id);
    void suspendSession(const SessionId& id);
    std::expected<CompactionOutcome, AgentError> requestCompaction(const SessionId& id);

    void dispose(AgentId id);   // park: copy strong ref under mutex_, call
                                // dispose()/closeSession() outside it, re-lock
                                // to erase (24-D18)

    // 24-D10/24-D15: last-supervisor teardown. Cancels each in-flight turn (its
    // cancel path flushes the chunk batch and appends exactly one terminal
    // event) then parks each agent, calls `SessionManager::closeSession(id)`
    // (the explicit last-exit close), and erases the maps. Never waits; the
    // coordinator's drain joins afterwards. Snapshot-release-erase contract:
    // snapshot the shared_ptrs under mutex_, RELEASE, call dispose()/
    // closeSession() on each snapshot entry, then re-lock to erase; never hold
    // mutex_ across dispose()/closeSession() and never hold an iterator across
    // them (24-D18). A concurrent caller that took findShared keeps its agent
    // (and, via the agent's session_owner_, its Session) alive until it returns
    // (AL31).
    void finalizeAll();

    // 11 §12.4 / 04 §3.7 arbiter predicate. Copies the shared_ptr under mutex_,
    // RELEASES, then evaluates the predicate on the strong reference outside
    // the lock (24-D18). The registry mutex is a map-only lock; the predicate
    // is never called under it. The strong ref keeps the agent alive for the
    // predicate call even if finalizeAll erases the entry concurrently.
    [[nodiscard]] bool hasPendingWork(const SessionId& id) const noexcept;

private:
    std::expected<AgentId, AgentError> registerAgent(const SessionId& sessionId);

    AgentServices services_;
    AgentConfig   config_;
    // 24-D2: shared ownership; the last strong ref may be an executor task body.
    std::unordered_map<std::string, std::shared_ptr<AgentLoop>> agents_;
    std::unordered_map<std::string, AgentId>                    bySession_;
    std::unordered_map<std::string, std::unique_ptr<SessionHandle>> leases_;
    mutable std::mutex                                          mutex_;   // 24-D11
    // ... provider_, pool_ unchanged ...
};
```

---

## 4. Defect 2 — `TurnExecutor` teardown: join, never detach

**Invariant that makes it impossible:** `AL10` (never detach), `AL11` (bounded
hard exit), `AL12` (drain before transport stop), `AL13` (wake before join),
`AL28` (child teardown before transport stop).

**Exact seam to change:** `TurnExecutor` (`include/ymh/agent/turn_executor.hpp`)
and `WorkspaceHost::Impl::coordinator` (`src/host/workspace_host.cpp`).

- `drain` returns `DrainResult { Quiesced, TimedOut }` and **never calls
  `detach()`**. On `TimedOut` the workers remain joinable; the coordinator must
  not return to normal teardown.
- `~TurnExecutor` is only reachable after `Quiesced`. If it finds non-joined
  workers, it must `std::_Exit(HostExitCode::Internal)` (never `detach`, never
  `std::terminate` on a joinable `std::thread`).
- `coordinator()` order becomes:

```text
1. protocol_->waitForDrain(shutdown_grace)                 # flush client outbound queues
2. runtime_->agents().finalizeAll()                        # cancel in-flight (flush + terminal
                                                           #   event), park, closeSession, erase maps
3. broker_->denyAll("shutdown")                            # wake permission-blocked workers
4. const TurnExecutor::DrainResult r = turns_->drain(shutdown_grace);
5. if (r == DrainResult::TimedOut) {
       unlinkSocketIfOwned();                              # best-effort only
       std::_Exit(static_cast<int>(HostExitCode::Internal));  # no destructors
   }
6. # Quiesced: ordered teardown is now destructor-safe
   runtime_->shutdownChildren(shutdown_grace);             # PTY closeAll + MCP shutdown
   requestWatchdogStop(); watchdog_thread_.join();
   cancelTimersAndSignals(); transport_->stop();
   persistence->close(); releaseHost(); unlinkSocketIfOwned();
```

Step 2/3 are new (`finalizeAll` / `denyAll`). They must run **before** the join
or a worker blocked on `PermissionBroker::resolve` → `outcome.get()` deadlocks
the coordinator (`AL13`). `PermissionBroker::~PermissionBroker` already resolves
pending promises with `Deny "shutdown"`; extract that body into
`PermissionBroker::denyAll(std::string reason)` so it can run before the join.

**Child teardown (finding 9, 24-D9; corrected in Rev 3).** Spec 16 §4.3 step 5
places PTY `LocalPtyService::closeAll()` **and** MCP `McpManager::shutdown()` before
`TransportServer::stop()`, so no child outlives the socket. The shipped
`coordinator` calls `runtime_->environment().pty().closeAll()` after the drain
but has **no explicit MCP shutdown** — `~McpManager` runs inside
`~WorkspaceRuntime`, which is reached after `transport_->stop()`. (Rev 2 claimed
`runtime_` is the first-declared `Impl` member; **that was inaccurate** —
`Impl::config_` is declared first and `runtime_` is declared at
`src/host/workspace_host.cpp:353`, after `io_`/`executor_` and before
`registry_`/`host_runtime_`/`protocol_`/`transport_` at lines 354–360. The
conclusion still holds: `transport_` is declared **after** `runtime_`, so it is
destroyed **before** `~WorkspaceRuntime`, and the explicit
`transport_->stop()` also runs while `runtime_` is still alive.)
Rev 2 makes the intent explicit: add
`WorkspaceRuntime::shutdownChildren(std::chrono::milliseconds grace)` which calls
`environment().pty().closeAll()` and `mcp().shutdown(grace).get()`, and call it at
step 6 (after quiesce, before transport stop). **Rev 3 pins the missing
accessor:** `shutdownChildren` cannot call `mcp()` today — `WorkspaceRuntime`
exposes only `mcp_statuses()` (`include/ymh/agent/workspace_runtime.hpp`) and the
owned manager is a private `std::unique_ptr<McpManager> mcp_`
(`src/agent/workspace_runtime.cpp`). Rev 3 therefore pins

```cpp
// WorkspaceRuntime (include/ymh/agent/workspace_runtime.hpp), mirroring gate()
// (defined at src/agent/workspace_runtime.cpp:291).
class McpManager;                              // add to the forward decls; the
                                               // header includes mcp_client.hpp,
                                               // not mcp_manager.hpp
[[nodiscard]] McpManager& mcp() noexcept;      // returns *impl_->mcp_
```

(`McpManager` is defined in `include/ymh/mcp/mcp_manager.hpp`; its existing
`Task<void> shutdown(std::chrono::milliseconds grace)` is the seam, and `.get()`
is the synchronous wait. The `.cpp` already includes `mcp_manager.hpp`.)
`shutdownChildren` reaches the manager through this accessor, so no caller needs
to touch `impl_`. This satisfies 16 §4.3 step 5's ordering and is pinned by
`AL28`. The implicit `~McpManager` shutdown remains a backstop; it is idempotent
(`McpManager` guards `shutdown_`).

**Ordering amendment.** The shipped coordinator already drains before
`TransportServer::stop()`; spec 16 §4.3 lists `TransportServer::stop()` (step 8)
**before** `drain TurnExecutor` (step 9), which would allow a worker to
`post()` after the transport is gone. This spec amends 16 §4.3 to the shipped
order (`drain` before `stop`, matching 11 §11.3's preamble) and records it in
§12.

**Failure mode if not fixed:** `AL-F2` (detached worker touches destroyed
state), `AL-F6` (permission-blocked join deadlock), `AL-F12` (`std::terminate`
on a joinable thread), `AL-F19` (MCP/PTY child outlives the socket).

### 4.1 Interface sketch — `TurnExecutor` (pinned, amended)

```cpp
class TurnExecutor {
public:
    enum class DrainResult { Quiesced, TimedOut };

    explicit TurnExecutor(std::size_t max_workers = 4, std::size_t queue_capacity = 4);
    ~TurnExecutor();   // only after Quiesced; otherwise std::_Exit (never detach)

    TurnExecutor(const TurnExecutor&) = delete;
    TurnExecutor& operator=(const TurnExecutor&) = delete;
    TurnExecutor(TurnExecutor&&) = delete;
    TurnExecutor& operator=(TurnExecutor&&) = delete;

    // 24-D4: the task carries its session so the queue is observable.
    // Returns false when the bounded queue is full or the executor is draining.
    bool submit(const SessionId& session, std::function<void()> body);

    // 24-D3: stop accepting, run/drain queued + in-flight bodies, join every
    // worker within `grace`. NEVER detaches. Idempotent:
    //   * once Quiesced, every later call returns Quiesced;
    //   * after TimedOut, a later call retries the join and returns Quiesced
    //     once the workers have exited (the workers are still joinable).
    [[nodiscard]] DrainResult drain(std::chrono::milliseconds grace);

    // 24-D4: queued + active bodies for one session, and in total.
    [[nodiscard]] std::size_t inFlight(const SessionId& session) const noexcept;
    [[nodiscard]] std::size_t inFlight() const noexcept;

    [[nodiscard]] std::size_t workerCount() const noexcept { return max_workers_; }
    [[nodiscard]] std::size_t queueCapacity() const noexcept { return queue_capacity_; }

private:
    struct Task {
        SessionId             session;
        std::function<void()> body;
    };

    void workerLoop(const std::shared_ptr<State>& state);

    std::size_t             max_workers_;
    std::size_t             queue_capacity_;
    std::shared_ptr<State>  state_;
    std::vector<std::thread> workers_;
    bool                    quiesced_ = false;
};
```

`State` (shared, owns all queue accounting; the only place session counters
live) gains:

```cpp
struct State {
    std::mutex                                mutex;
    std::condition_variable                   cv;
    std::deque<Task>                          queue;
    std::unordered_map<std::string, std::size_t> queued_by_session;  // under mutex
    std::unordered_map<std::string, std::size_t> active_by_session;  // under mutex
    std::size_t                               active = 0;
    std::size_t                               exited = 0;
    bool                                      stopping = false;
};
```

`submit` increments `queued_by_session[session]`; `workerLoop` moves one from
queued to active when it pops and decrements active when the body returns;
`drain` clears both when it observes quiescence. `inFlight(session)` returns
`queued_by_session[session] + active_by_session[session]`. (Finding 10: the
per-session counter lives **only** in `State`, not duplicated on
`TurnExecutor`.)

### 4.2 Test seam for `TimedOut` (finding 8)

`TimedOut` must be observable without killing the test binary. Two pinned seams:

1. **In-process re-drain.** A test submits a body that blocks on a test latch;
   `drain(1 ms)` returns `TimedOut`; the test releases the latch; a second
   `drain(1 s)` returns `Quiesced`; the executor is then destroyed normally.
   This exercises `TimedOut` and the idempotent re-drain semantics with no
   hard exit. (`AL-U7`/`AL-U17`.)
2. **Subprocess hard exit.** The coordinator's `TimedOut → std::_Exit` path is
   tested in a child process: the child builds a `WorkspaceHost` with an
   uncancellable turn, triggers shutdown, and the parent asserts the child
   exited with `HostExitCode::Internal` and produced no sanitizer report.
   (`AL-S1`.) The test binary itself never constructs a non-quiesced
   `TurnExecutor` that reaches `~TurnExecutor`.

`~TurnExecutor` therefore never hard-exits in-process tests; it only does so on
the real coordinator path, where `std::_Exit` is the intended behavior.

---

## 5. Defect 3 — the queue-aware pending predicate

**Invariant that makes it impossible:** `AL6`, `AL7`, `AL19`.

**Exact seam to change:** `HostRuntime` (`src/host/host_runtime.cpp`) and the
`deleteSession` mid-turn guard; spec 23 §5.4's guard is amended to call it.

```cpp
// New: the single pending predicate for the daemon.
[[nodiscard]] bool HostRuntime::hasPendingWork(const SessionId& id) const {
    return runtime_.agents().hasPendingWork(id) || turns_.inFlight(id) > 0;
}

// deleteSession's mid-turn lambda becomes:
const bool mid_turn = [&] {
    if (hasPendingWork(id)) {
        return true;
    }
    // 24-D14: hold a strong reference for the whole lambda. The coordinator's
    // finalizeAll may erase the map entry concurrently; the shared_ptr keeps
    // the agent alive until the switch completes (AL30/AL31).
    std::shared_ptr<AgentLoop> agent = runtime_.agents().findShared(id);
    if (agent == nullptr) {
        return false;
    }
    switch (agent->state()) {
        case AgentState::Thinking:
        case AgentState::CallingTool:
        case AgentState::WaitingForPermission:
        case AgentState::WaitingForInput:
        case AgentState::Cancelling:
            return true;
        case AgentState::Idle:
        case AgentState::Error:
            return false;
    }
    return false;
}();
```

**Why io-serialization closes the race.** `deleteSession` and every
`agentPrompt`/`agentFollowup`/`agentSteer`/`agentInject` run on the transport io
thread through `HostRuntime::translate`. The guard reads `inFlight(id)` and the
delete's subsequent `dispose` + row erase execute in the same dispatch turn, so
no submit can interleave. A prompt submitted before the delete is either active
or queued (`inFlight > 0`) → the guard refuses. A prompt submitted after the
delete fails `sessionExists`/`ensureAgent`. There is no window.

**Close is no longer a dispose path.** With 24-D10, `HostRuntime::closeSession`
is detach-only, so it never drops a queued prompt; the turn (including a queued
prompt body) continues. A queued prompt body for a **deleted** session is
prevented by the guard. A queued prompt body for a **finalized** (last-exit)
session observes `disposed_` and returns `AgentDisposed`; this is the
documented last-exit behavior (see §14).

**Failure mode if not fixed:** `AL-F3` (a just-submitted, already-acked prompt
is dropped by a delete), and the spec 23 §5.4 gap.

### 5.1 Rejected predicate variants

- **Count the executor queue globally.** Wrong: a body for session B would
  refuse a delete of session A.
- **Count only queued, not active.** Insufficient: an active body in the
  `activate()` loop's idle gap would not be counted.
- **Put the OR inside `AgentRegistry::hasPendingWork`.** Layering inversion:
  `AgentRegistry` lives in `WorkspaceRuntime`; `TurnExecutor` is daemon-level.
  The OR belongs at `HostRuntime`, which owns both.

---

## 6. Defect 4 — committed-record forwarding

**Invariant that makes it impossible:** `AL16`, `AL17`, `AL18`, `AL29`.

**Exact seams to change.**

1. `EventBus` (`include/ymh/core/event_bus.hpp`, `src/core/event_bus.cpp`):
   - Add `using RecordHandler = std::function<void(const EventRecord&)>;`
   - Add `Subscription subscribeCommitted(RecordHandler)` — global only.
   - Add `void publishCommitted(const EventRecord&)` — invoke every `Event`
     handler with `record.event`, every committed handler with `record`, and fan
     out to the per-session mailbox (preserving today's ordering).
   - Keep `subscribe(Handler)` / `publish(Event)` unchanged for live events.
   - **Unsubscribe fix (24-D12, finding 4).** `Subscription` currently carries
     only `id_` + optional session; `EventBusState::unsubscribe` erases from
     `global` (no session) or a session channel. A committed handler stored in a
     new `committed` list would never be erased, so `~HostRuntime` would leak it
     and a later publish would call into a destroyed `HostRuntime`. Pin a
     channel discriminator on `Subscription`:

```cpp
class Subscription {
public:
    enum class Channel : std::uint8_t { Global, Session, Committed };
    // ...
private:
    std::weak_ptr<detail::EventBusState> state_;
    std::uint64_t                        id_ = 0;
    std::optional<std::string>           session_;
    Channel                              channel_ = Channel::Global;
};
```

   `EventBusState` gains a `std::vector<GlobalEntry> committed;` list and its
   `unsubscribe(id, channel, session)` erases from the matching list. `Session`
   (mailbox) subscriptions use `Channel::Session`; the existing global
   subscriptions use `Channel::Global`.

2. `Session` (`src/session/session.cpp`):
   - `appendEventLocked`: replace `bus_->publish(event)` with
     `bus_->publishCommitted(EventRecord{seq, event})`.
   - `appendBatch`: replace the per-event `bus_->publish(event)` loop with
     `publishCommitted(EventRecord{sequences[index], events[index]})`.
   - `emit` (live-only) stays `bus_->publish(std::move(event))`.
3. `SessionStore::eraseWithEvent` (`include/ymh/session/session.hpp`,
   `src/session/session_persistence.cpp`): return type changes from `void` to
   `Sequence` (the assigned terminal sequence). **Source-breaking** for
   implementers of the `SessionStore` seam; the in-tree fake(s) and the durable
   store are updated. No wire/schema change.
4. `SessionManager::deleteSession`: capture `const Sequence seq = store_->eraseWithEvent(id, ended);`,
   erase the map entry, then `bus_->publishCommitted(EventRecord{seq, ended});`
   (replacing `bus_->publish(ended)`; the committed publish still delivers to
   live `Event` handlers, so no regression).
5. `HostRuntime` (`src/host/host_runtime.cpp`):
   - `startForwarding` creates two subscriptions: a live `subscribe` that keeps
     only the `McpServerStatusChanged` branch, and a `subscribeCommitted` that
     calls `handleCommittedRecord`.
   - `handleCommittedRecord(const EventRecord&)` performs the per-session
     monotonic guard against `last_forwarded_` and forwards `record` directly.
     **No `readAfter`.**
   - `~HostRuntime` unsubscribes **both** (the committed handle now unsubscribes
     correctly, `AL29`).

```cpp
void HostRuntime::handleCommittedRecord(const EventRecord& record) {
    const SessionId session = record.event.session_id;
    {
        std::lock_guard<std::mutex> lock(forward_mutex_);
        const auto it = last_forwarded_.find(session.value);
        const Sequence last = (it == last_forwarded_.end()) ? 0 : it->second;
        if (record.seq <= last) {
            return;                          // duplicate / out-of-order guard
        }
        last_forwarded_[session.value] = record.seq;
    }
    if (forwarder_) {
        forwarder_(record);
    } else if (server_ != nullptr) {
        server_->onEventCommitted(record);
    }
}
```

**Why this satisfies I16/§7.4.** The delete appends `SessionEnded{Deleted}` and
erases the row in one transaction; the append site knows the `Sequence`; the
committed channel delivers `{seq, SessionEnded}` to the forwarder before the
erase transaction's side effects can hide it. The client receives the terminal
event with a valid cursor; a later `session.resume` of the erased id is
`UnknownSession`/`CursorInvalid`, as 05 §8.5/T16 specifies.

**Failure mode if not fixed:** `AL-F4` (terminal event dropped), `AL-F20`
(committed handler leak / UAF into a destroyed `HostRuntime`).

---

## 7. Concurrency model, map synchronization, and capture audit

### 7.1 Threading model

- **io thread.** Runs `AsioExecutor`; owns `TransportServer`, `ProtocolServer`
  dispatch, timers, signals. All `HostRuntime` RPC methods execute here. It
  never runs a turn body.
- **worker threads.** `TurnExecutor` pool (`max_llm_concurrency`). Each runs a
  submitted body synchronously; a body may run an entire `AgentLoop::activate`
  turn, including provider calls, tools, and permission waits.
- **coordinator thread.** The daemon's main thread (`WorkspaceHost::Impl::run` →
  `serve` → `coordinator`). Non-io. Owns `finalizeAll` and the join.
- **watchdog thread.** Dedicated; spec 16 §5.1. Calls `std::_Exit` on the
  bounded deadline.

### 7.2 The map-synchronization and pointer-escape contract (findings 1 + 14 / HIGH; 24-D11/24-D14)

`AgentRegistry::agents_`/`bySession_`/`leases_` and `SessionManager::sessions_`
have **no mutex** in the shipped tree, yet they are touched by more than one
thread:

- io thread: every `HostRuntime` RPC handler (`create`, `resume`, `dispose`,
  `delete`, `find`, `hasPendingWork`, `activateSession`, `suspendSession`, …).
- worker threads: `SubagentRunner::run` calls `AgentRegistry::create` and
  `AgentRegistry::dispose`, and `SessionManager::session`.
- coordinator thread: `finalizeAll` erases entries while the io thread may be
  mid-handler. `setState(Draining)` only rejects **new** requests
  (`ProtocolServer` dispatch returns `ShutdownInProgress` when
  `host_.hostState() != Serving`); a handler already dispatched keeps running.
  `ProtocolServer::waitForDrain` tracks **outbound bytes** (`outstanding_total_`),
  not handler completion. Therefore `finalizeAll` genuinely races in-flight
  handlers, and concurrent map read+write is UB.

**Pinned contract.**

- **One lock-scope rule (24-D18).** `AgentRegistry::mutex_` is a **map-only
  lock**: it is held exactly while `agents_`/`bySession_`/`leases_` are read or
  mutated (lookup, `shared_ptr` copy, insert, erase). It is **never** held
  across a call into `AgentLoop`, `Session`, `SessionManager`, `LLMProvider`,
  `PermissionBroker`/`PermissionGate`, or `TurnExecutor`; never across a
  predicate, hook, or callback invocation; and never across a `shared_ptr`
  destructor that could run user code. Every `erase` site holds a local strong
  copy of the erased `shared_ptr`, so the freed object's destructor runs after
  `mutex_` is released. `hasPendingWork`'s predicate, `activate`/`suspend`/
  `requestCompaction`, and `dispose`/`closeSession` are therefore all invoked
  **outside** `mutex_` on a strong reference copied under it. §3.3 and §7.4
  state this identical rule; no other clause may contradict it.
- `SessionManager` gains `mutable std::mutex mutex_;` guarding `sessions_`.
  `loadInto`/`createSession`/`forkSession`/`closeSession`/`deleteSession`/
  `sessionPtr`/`resumeSession` take it for the map check/insert/erase. It is
  **never** held across `store_->append`/`eraseWithEvent`/`bus_->publish` where
  avoidable; `deleteSession` holds it across the erase + publish so a concurrent
  `resumeSession` cannot resurrect a deleted session (the publish is
  non-blocking; see the lock order). `closeSession` copies the erased
  `shared_ptr` out under the lock so `~Session` runs after release.
- **The registry cannot hand out a raw map-derived handle (24-D17).**
  `Agent& get(AgentId)` and `Agent* find(SessionId)` are **deleted**;
  `findShared`/`getShared` (returning `shared_ptr<AgentLoop>`) are the only
  accessors. On the session side, `SessionManager::sessionPtr` is the only
  public accessor and `session()` is private; `AgentLookup`/`setAgentLookup`/
  `findAgent`/`agent` are deleted. A `shared_ptr` copied under `mutex_` and held
  for the caller's whole use is the only way to reach a map-owned
  agent/session. Because the agent holds a strong `shared_ptr<Session>`
  (`session_owner_`, `AL1`), `closeSession`'s erase of the manager reference
  cannot free a `Session` reached through a held agent either. **No caller can
  observe a freed `AgentLoop` or `Session`.**
- `finalizeAll` snapshots the `shared_ptr`s under `mutex_`, **releases**, then
  calls `agent->dispose()`/`services_.sessions->closeSession(id)` (24-D15) on
  each snapshot entry, then re-locks to erase. This satisfies the
  erase-while-iterating contract (finding 2) and means `mutex_` is never held
  across `dispose()`/`closeSession()` (24-D18).

**Chosen mechanism (24-D14/24-D17): delete the raw accessors; a strong
reference is the only handle.** Rejected alternatives:

- **(b) park-only `finalizeAll` + defer erasure until handler-completion
  quiescence.** There is no handler-completion barrier today:
  `HostRuntime::translate` (`include/ymh/host/host_runtime.hpp`) only maps
  exceptions, and `ProtocolServer::waitForDrain`
  (`src/transport/protocol_server.cpp`) counts outbound bytes
  (`outstanding_total_`), not in-flight handler bodies. Building such a barrier
  would add a new synchronization mechanism plus a shutdown-ordering dependency,
  and it still would not cover worker-thread `SubagentRunner` access to the same
  maps. Rejected.
- **(c) marshal `finalizeAll` onto the io thread and join.** `finalizeAll` runs on
  the coordinator thread (`WorkspaceHost::Impl::coordinator`,
  `src/host/workspace_host.cpp`), which is not the io thread. Marshalling would
  add an io round-trip and a join handshake to an already grace-bounded teardown;
  worse, an io handler can be blocked waiting on a worker that is blocked in
  `PermissionBroker::resolve` (the §2.2 io-deadlock analysis), so joining the io
  thread can deadlock. Rejected.
- **(d) keep `find`/`get` and merely audit every call site.** This is the Rev 3
  approach, and it failed twice: the "exhaustive" list omitted the registry's
  own `activateSession`/`suspendSession`/`requestCompaction`/`hasPendingWork`
  paths (`src/agent/agent_registry.cpp`), each of which held a raw
  `agent->second` across the call. An enumeration is a maintenance liability: a
  new call site silently reintroduces the hazard. Deleting the accessors makes
  the hazard unrepresentable — there is no expression that yields a raw
  `Agent*`/`Agent&` to a map-owned object. Rejected.
- The mutex (24-D11) is required under all options, so (a) adds no new mechanism
  beyond the refcounts already required by `AL1`/`AL2`.

#### 7.2.1 Structural proof — no raw map-derived handle can escape (24-D17/`AL35`)

The safety argument is this proof, not the §7.2.2 checklist. It is a case
analysis over every mechanism that can name a map-owned `AgentLoop` or
`Session`. Let *H* be the proposition "a raw `Agent*`/`Agent&`/`Session*`/
`Session&` to a map-owned object is reachable by code whose use can outlive the
object's map entry".

1. **Only two maps own agents/sessions.** `AgentRegistry::agents_`
   (`unordered_map<string, shared_ptr<AgentLoop>>`) and
   `SessionManager::sessions_` (`unordered_map<string, shared_ptr<Session>>`).
   `bySession_` stores only `AgentId` values; `leases_` stores
   `unique_ptr<SessionHandle>`. Neither stores an `Agent*`/`Session*`.
2. **Only two accessors yield an agent handle.** After deleting `get`/`find`,
   the registry's only functions returning a handle are `findShared(SessionId)`
   and `getShared(AgentId)`, both declared `-> std::shared_ptr<AgentLoop>` and
   both implemented as "lock `mutex_`, copy the `shared_ptr`, unlock, return".
   No function returns `Agent*`/`Agent&`.
3. **Only one public accessor yields a session handle.** `SessionManager::
   sessionPtr(id) -> std::shared_ptr<Session>`; `session()` is private and
   callable only where the manager holds `mutex_` or the caller holds a strong
   reference (its external callers `AgentRegistry::registerAgent` and
   `SubagentRunner` now use `sessionPtr`). No function returns
   `Session*`/`Session&` to a caller that can outlive the entry.
4. **The stored-lambda path is gone.** The only stored callback that re-read the
   registry map (`SessionManager::agent_lookup_`, installed by the
   `AgentRegistry` constructor via
   `services_.sessions->setAgentLookup([this](const SessionId& id){ return find(id); })`)
   is deleted with `find`. No closure captures `this` and re-reads
   `agents_`/`bySession_`; the registry's own methods are the only re-readers,
   and they use `findShared`/`getShared`.
5. **Every internal re-reader holds the strong copy.** `AgentRegistry::
   activateSession`/`suspendSession`/`requestCompaction`/`hasPendingWork`/
   `dispose`/`finalizeAll`/`activeCount`/`list` obtain their handle via
   `findShared`/`getShared` (or copy directly under `mutex_` for the
   `finalizeAll` snapshot) and hold the `shared_ptr` for the whole operation; no
   raw reference survives the `mutex_` release (24-D18). The local `shared_ptr`
   is the lifetime edge.
6. **No cached raw member points at a map-owned agent/session.** Verified over
   the whole tree, the only raw members that name an agent/session are
   `AgentLoop::session_` (kept alive by the co-owned `session_owner_`, `AL1`),
   `SubagentRunner::parent_` (a `Session&` bound from an **owning**
   `std::shared_ptr<Session>` resolved via `sessionPtr` — §7.3 C8 — and anchored
   by the spawning parent agent's strong `session_owner_`, `AL1`), and
   `ToolContext::session_` / the `ChunkCoalescer` session reference (used only
   inside a turn whose agent holds the session strong, `AL1`). None is a bare
   map-derived handle: each is either co-owned by an agent (`session_owner_`) or
   bound from an owning handle whose lifetime the owning agent bounds, and none
   is read after the owner's join.
   The one cached raw **map-derived** member that shipped — `TuiApp::agent_`
   (`Agent& agent_;`, `src/ui/ui_application.cpp`), bound from
   `AgentRegistry::get(*created)` — is **removed** with the dead
   `run_tui`/`TuiApp` path (24-D19); it was the counterexample that falsified
   Rev 4's unconditional claim. After removal no cached raw member is
   map-derived.
7. **The `shared_ptr` cannot be silently converted to a dangling raw pointer.**
   A caller may write `.get()`/`*`/`->`; the pinned usage rule is that such a
   result is consumed within the expression's full scope while the `shared_ptr`
   is alive, never stored in a local that outlives it. The rule is enforced
   **repo-wide**, not per-directory, by exactly two mechanisms:
   - **By construction — the build fails.** The deleted accessors
     (`AgentRegistry::get`/`find`;
     `SessionManager::AgentLookup`/`setAgentLookup`/`findAgent`/`agent`) do not
     exist, and `SessionManager::session()` is **private** (retained for
     internal use, §3.1). Any translation unit under `src/`, `tests/`, or
     `include/` that *names* one of them fails to compile (a deleted-function or
     private-member diagnostic). This is a C++ translation rule, not a check
     that must be run; it is why this spec may say "the build fails if a TU
     names a deleted accessor" — that claim is true by construction.
   - **Runnable repo-wide scan — `AL-U22`.** The compiler cannot see a raw
     handle deliberately extracted with `.get()`/`*` and stored past its
     `shared_ptr`'s life. `AL-U22` is therefore a CTest test
     (`tests/unit/no_raw_map_handle_test.cpp`, §11.1) that walks `src/`,
     `tests/`, and `include/` and fails on (a) any occurrence of a deleted
     accessor name and (b) a raw extraction (`.get()`/`*`) applied **directly to
     a call** of `findShared`/`getShared`/`sessionPtr` — the temporary-handle
     pattern that dangles once the temporary dies. Extraction from a **named**
     owning local (`agent.get()`, the X8 aliases) is allowed within that local's
     scope and is not flagged, matching the usage rule above. It is a **lexical
     guard** run on every build; the completeness argument remains the type
     system plus this proof, and §7.2.2 is the human checklist for cases the
     scan cannot parse.
   A `src/host/`-scoped check is what let the `TuiApp::agent_` HIGH through
   twice; the scope is now the whole tree (`AL37`/24-D19).

No case yields *H*: the only handle-producing functions return owning
`shared_ptr`s, the stored re-reader is deleted, and every re-reader holds the
copy across its use. QED.

#### 7.2.2 Verification checklist — raw-handle sites, repo-wide (24-D14; not the safety argument)

**This is a checklist, not the proof.** The safety argument is §7.2.1; this
table exists to confirm that the structural rule was applied at every shipped
site **in the whole tree**. A site missing from this list is a review defect, but
the *absence of a site from the list* is not what makes the design safe — the
deleted accessors (24-D17) are.

The tables below cover the io-thread `HostRuntime` handlers (H1–H8, S1–S4), the
registry's own methods (R1–R6), the worker bodies (W1–W6), and — Rev 5/6 — every
non-host caller (X1–X10): the live `src/cli/` path, the test-support helpers, and
every test TU. Each raw pointer/reference is replaced by a strong
`std::shared_ptr<AgentLoop>` (`findShared`/`getShared`) or
`std::shared_ptr<Session>` (`sessionPtr`) held for the caller's whole scope, or
by a registry method that does so internally. A check scoped to `src/host/` is
explicitly insufficient (`AL37`).

| # | Site (`src/host/host_runtime.cpp`) | Shipped raw use | Pinned |
|---|---|---|---|
| H1 | `HostRuntime::ensureAgent` | `runtime_.agents().find(id) != nullptr` then `resume(id)` | `findShared(id) != nullptr`; the strong ref is held across the check |
| H2 | `HostRuntime::closeSession` | `find(id)` + `dispose(agent->id())`, plus `runtime_.environment().pty().closeSession(id)` and `runtime_.sessions().closeSession(id)` (`src/host/host_runtime.cpp`) | **removed**: detach-only (24-D10) — no agent lookup, **no `pty().closeSession`**, **no `sessions().closeSession`** (matches §2.2 and §13 item 4; AL23) |
| H3 | `HostRuntime::deleteSession` (mid-turn) | `find(id)` + `agent->state()` switch | `findShared(id)` held for the lambda |
| H4 | `HostRuntime::deleteSession` (dispose) | `find(id)` + `dispose(agent->id())` | `findShared(id)` held across the dispose |
| H5 | `HostRuntime::agentCancel` | `find(id)` + `hasPendingWork()` + `cancel()` | `findShared(id)` held for the body |
| H6 | `HostRuntime::agentStatus` | `find(id)` + `status()` | `findShared(id)` held for the body |
| H7 | `HostRuntime::hasPendingWork` (new) | `runtime_.agents().hasPendingWork(id)` | registry copies a strong ref under `mutex_`, releases, calls the predicate outside (R4) |
| H8 | `HostRuntime::createSession` | `runtime_.agents().get(*created).session()` (raw `Agent&`) | `getShared(*created)` held while `.session()` is read |

Registry-method checklist (the sites Rev 3 omitted; `src/agent/agent_registry.cpp`):

| # | Method | Shipped raw use | Pinned |
|---|---|---|---|
| R1 | `AgentRegistry::activateSession` | `agents_.find` → raw `agent->second->activate()` | `findShared` internally: copy under `mutex_`, release, call `activate()` on the strong ref |
| R2 | `AgentRegistry::suspendSession` | `agents_.find` → raw `agent->second->suspend()` | `findShared` internally (this method runs **directly on the io thread**, `HostRuntime::suspendSession`) |
| R3 | `AgentRegistry::requestCompaction` | `agents_.find` → raw `agent->second->requestCompaction()` | `findShared` internally; may run a whole turn, so it must hold the strong ref |
| R4 | `AgentRegistry::hasPendingWork` | `agents_.find` → raw `agent->second->hasPendingWork()` | `findShared` internally; predicate called **outside** `mutex_` on the strong ref |
| R5 | `AgentRegistry::dispose` | `it->second->dispose()` + erase under one scope | strong copy under `mutex_`, call outside, re-lock to erase |
| R6 | `AgentRegistry::finalizeAll` | iterate `agents_` + `it->second->dispose()` | snapshot strong refs under `mutex_`, call outside, re-lock to erase |

Worker-body checklist (`src/host/host_runtime.cpp`):

| # | Body | Shipped raw use | Pinned |
|---|---|---|---|
| W1 | `HostRuntime::activateSession` body | `runtime_.agents().activateSession(id)` | registry method holds the strong ref (R1) |
| W2 | `HostRuntime::compactSession` body | `runtime_.agents().requestCompaction(id)` | registry method holds the strong ref (R3) |
| W3 | `HostRuntime::agentPrompt` body | `find(id)` + `agent->prompt(...)` | `findShared(id)` at body start, held for the body |
| W4 | `HostRuntime::agentFollowup` body | `find(id)` + `agent->followup(...)` | `findShared(id)` at body start, held |
| W5 | `HostRuntime::agentSteer` body | `find(id)` + `agent->steer(...)` | `findShared(id)` at body start, held |
| W6 | `HostRuntime::agentInject` body | `find(id)` + `agent->inject(...)` | `findShared(id)` at body start, held |

**Session-side rule (24-D14/`AL1`).** The same hazard exists for a `Session&`
obtained on the io thread, because `finalizeAll` now calls
`SessionManager::closeSession`, which erases `sessions_`. The fix is the pinned
`SessionManager::sessionPtr(id)` (24-D1): every io handler that dereferences a
session holds a `sessionPtr` for its whole body. The io handlers that use a
session indirectly through a `SessionManager` convenience method are:

| # | Site (`src/host/host_runtime.cpp`) | Use | Pinned |
|---|---|---|---|
| S1 | `HostRuntime::agentPrompt` | `runtime_.sessions().maybeAutoName(id, …)` (internally `session(id)`) | hold `sessionPtr(id)` (the agent's `ensureAgent` already guarantees one) across the call |
| S2 | `HostRuntime::renameSession` | `runtime_.sessions().renameSession(id, …)` (internally `session(id)`) | hold `sessionPtr(id)` across the call |
| S3 | `HostRuntime::forkSession` | `runtime_.sessions().forkSession(parent, …)` | hold `sessionPtr(parent)` across the call |
| S4 | `HostRuntime::createSession` | `get(*created).session()` | covered by H8 (`getShared`) |

The `SessionManager` convenience methods internally use the now-private
`session(id)`; because the handler holds `sessionPtr(id)` for the whole call,
the object cannot be freed under the method. If the entry was erased first, the
method throws `UnknownSession` (safe, no dangling).

**Non-host callers (Rev 5/6 — the sites Rev 4 omitted; that omission was the
HIGH).** The static rule is repo-wide; these are the shipped callers outside
`src/host/` that named a raw map-derived handle. Each is migrated to an owning
handle held for its whole use, or removed with its dead path. The one
**production** site outside `src/host/` — `src/agent/subagent.cpp` (`:27`
`registry_.get(*created)`, `:45` `sessions_.session(childSessionId)`) — is
enumerated as capture `C8` in §7.3 and pinned there; the table below is the
non-host **caller/helper/test** set:

| # | Site | Shipped raw use | Pinned |
|---|---|---|---|
| X1 | `src/cli/headless.cpp` `run()` (**live**) | `Agent& agent = registry.get(agent_id);` (`:169`), then `agent.session()`, `agent.send()`, `agent.cancel()` | `std::shared_ptr<AgentLoop> agent = registry.getShared(agent_id);` — a **named local held for the whole function** (not a temporary, not a member) |
| X2 | `src/ui/ui_application.cpp` `run_tui` (**dead**) | `Agent& agent = registry.get(*created);` (`:482`) + member `Agent& agent_;` (`:422`) | **path removed** (24-D19, §7.2.3): the `.cpp`, its header, its `CMakeLists.txt` entry, the unused `cli.cpp` include, the stale `run_tui` comments (below), and the `ui_model_test.cpp` `run_tui` case are deleted |
| X3 | `tests/support/agent_test_env.hpp` `createAgent()` | `return registry.get(*created);` (`:114`, `Agent&`) | returns `std::shared_ptr<AgentLoop>` via `getShared`; callers bind `auto agent = env.createAgent();` |
| X4 | `tests/support/agent_test_env.hpp` `sessionOf()` | `sessions.session(agent.session())` (`:117`, `Session&`) | returns `std::shared_ptr<Session>` via `sessions.sessionPtr(...)`; callers use `env.sessionOf(agent)->events()` |
| X5 | `tests/unit/skills_wiring_test.cpp` | `runtime.agents().get(*created).session()` (`:149`); `runtime.sessions().session(id)` (`:177`, `:198`); its `env.createAgent()`/`env.sessionOf()` uses (`:287`/`:290`, `:333`/`:336`, `:348`/`:359`) | `getShared(*created)->session()`; `sessionPtr(id)` held for the test body; `createAgent`/`sessionOf` return owning handles (X3/X4) |
| X6 | `tests/unit/agent_registry_test.cpp` | **two** raw sources: its local `DurableAgentEnv::createAgent()` `Agent&` (`:135`–`:140`, `registry.get(*created)`) and `DurableAgentEnv::sessionOf()` `Session&` (`:143`, `sessions.session(agent.session())`); raw `env.registry.get(*resumed)` (`:198`, `:237`); raw `env.sessions.session(id)` (`:201`, `:228`, `:239`); `env.sessions.findAgent(id)`/`env.sessions.agent(id)` (`:247`, `:248`, `:251`, `:252`); `env.sessionOf(parent)` handed to `SubagentRunner` (`:275`, `:301`, `:337`); the `env.createAgent()`/`env.sessionOf(...)` callers (`:176`, `:181`, `:187`, `:192`, `:206`, `:244`, `:257`, `:258`, `:265`, `:266`, `:273`, `:281`, `:293`, `:307`, `:334`, `:343`, `:401`, `:418`) | `DurableAgentEnv::createAgent()`/`sessionOf()` return owning `shared_ptr<AgentLoop>`/`shared_ptr<Session>` (mirroring X3/X4); `env.registry.get` → `getShared`; `env.sessions.session` → `sessionPtr`; the `findAgent`/`agent` delegate assertions are removed with the deleted delegate (§13 item 8); `SubagentRunner` binds `parent_` from the owning session handle (C8, §7.2.1 point 6) |
| X7 | `tests/unit/workspace_runtime_test.cpp` | `runtime.agents().get(*created).session()` (`:75`, `:149`, `:172`); `runtime.agents().find(session)` (`:78`); `runtime.sessions().session(session)` (`:93`) | `getShared(*created)->session()`; `findShared(session) != nullptr`; `sessionPtr(session)` |
| X8 | `tests/unit/agent_loop_test.cpp`, `agent_inbox_test.cpp`, `compaction_test.cpp`, `host_runtime_test.cpp`, `ui_driver_test.cpp` | raw `Agent&`/`Session&` from `env.createAgent()`/`env.sessionOf()` (e.g. `agent_loop_test.cpp:130`/`:136`, `agent_inbox_test.cpp:128`/`:133`, `compaction_test.cpp:524`/`:529`, `ui_driver_test.cpp:48`); the `Agent* agentPtr = &agent;` aliases (`agent_inbox_test.cpp:143`, `:173`, `:219`, `:245`, `:279`; `agent_loop_test.cpp:196`, assigned `:207`); `host_runtime_test.cpp`'s **14** raw `sessions().session(...)` `Session&` sites (`:269`, `:280`, `:566`, `:865`, `:959`, `:1010`, `:1017`, `:1021`, `:1026`, `:1029`, `:1049`, `:1053`, `:1080`, `:1081`) and `agents().find(parent.session)` (`:520`, `:539`) | mechanical: `auto agent = env.createAgent();` (owning), `env.sessionOf(...)` owning, `findShared`; the `Agent*` aliases bind the owning handle's `.get()` only within the handle's scope; the session sites use `sessionPtr` held for the test body |
| X9 | `tests/unit/ui_model_test.cpp` | `run_tui(options)` (`:278`, `UiApplication.RejectsMissingWorkspace`); `#include "ymh/ui/ui_application.hpp"` (`:21`) | **removed** with the dead path (24-D19); the include is dropped with it |
| X10 | `tests/unit/session_test.cpp` | raw `Session&` from `manager.session(id)` (`:462`, `:470` — `Session& rootSession`/`forkSession`; `:501`, `:691`, `:694`, `:721`, `:742`, `:746`, `:749`, `:774`, `:777`) | `manager.sessionPtr(id)` held for the test body |

**Rev 5/6 (24-D17/24-D19):** `AgentRegistry::find`/`get` and `SessionManager::
AgentLookup`/`setAgentLookup`/`findAgent`/`agent` are **deleted**, and
`SessionManager::session()` is **private** (retained for internal use, §3.1) —
they were the raw-handle paths. After Rev 5 no `HostRuntime` handler, worker
body, stored closure, non-host caller (X1–X10), or test helper can obtain a raw
`Agent*`/`Agent&`/`Session*`/`Session&`; the only handles are
`findShared`/`getShared`/`sessionPtr` (see §7.2.1). The `SessionManager::
findAgent`/`agent` delegates were production-unused (only
`tests/unit/agent_registry_test.cpp`); that case is removed (§13 item 8).

#### 7.2.3 Dead-path verdict — `ui::run_tui` / `TuiApp` (24-D19)

`ui::run_tui` (`include/ymh/ui/ui_application.hpp`, `src/ui/ui_application.cpp`)
is **dead in production**, verified against the shipped tree:

- **No caller in `src/`.** `src/cli/cli.cpp` includes the header (`:38`) but
  never calls `run_tui`; the interactive entry is `run_supervisor_entry`
  (`src/cli/cli.cpp`) → `ui::run_supervisor` (`src/ui/supervisor.cpp`).
- **The only call is a unit test.** `tests/unit/ui_model_test.cpp`'s
  `UiApplication.RejectsMissingWorkspace` calls `run_tui` with a missing
  workspace and asserts exit code 2 — it exercises the error path, not the app.
- **Prior verified specs already classify it dead.** `17 §1` and `19 §1` say so
  explicitly; `23` lists it as out of scope. This spec does not amend them; it
  removes the path they describe.

`TuiApp` (`src/ui/ui_application.cpp`) is the sole owner of the cached raw member
`Agent& agent_;`, bound from `AgentRegistry::get(*created)`. **Pinned
remediation: remove the path** — delete `src/ui/ui_application.cpp`,
`include/ymh/ui/ui_application.hpp`, the `src/ui/ui_application.cpp` entry in
`CMakeLists.txt`, the `#include "ymh/ui/ui_application.hpp"` in
`src/cli/cli.cpp`, and the `UiApplication.RejectsMissingWorkspace` case (and its
now-unused `#include "ymh/ui/ui_application.hpp"`) in
`tests/unit/ui_model_test.cpp`. The stale `run_tui` references left in
comments are removed with the path: the `"main"`/`run_tui` clause in the
`is_placeholder_title` doc comment (`include/ymh/session/session.hpp`) and the
`src/ui/ui_application.cpp` mention in the `WorkspaceRuntime` doc comment
(`include/ymh/agent/workspace_runtime.hpp`). Both are comment-only edits —
`is_placeholder_title` behavior is unchanged, since stored sessions may still
carry the legacy `"main"` placeholder. No production behavior is lost. The raw
member **ceases to exist** rather than being converted, which is what makes
§7.2.1 point 6 true structurally. If a future milestone revives an in-process
TUI, it must bind `getShared` to a named local for the whole run, never a member
(24-D19/`AL37`).

### 7.3 Capture audit (finding 6; AL14)

| # | Capture site | Captured | Runs on | Owner lifetime guarantee |
|---|---|---|---|---|
| C1 | `HostRuntime::activateSession`/`compactSession`/`agentPrompt`/`agentFollowup`/`agentSteer`/`agentInject` bodies | `[this, id, …]` + the strong ref the body resolves (`findShared`) or the registry method holds (R1/R3) | worker | the body holds a strong `shared_ptr<AgentLoop>` for its whole duration (`AL2`); `HostRuntime` outlives all bodies via join (`AL12`) |
| C2 | `AgentLoop` permission resolver installed by `Impl::startup` | `[this]` (`Impl`) + `broker_` | worker | `broker_` denied before join (`AL13`); `Impl` alive until join |
| C3 | `HostRuntime::startForwarding` committed handler | `[this]` (`HostRuntime`) | appending thread (worker or io) | drain-before-destroy (`AL12`); unsubscribed in `~HostRuntime` (`AL29`) |
| C4 | `HostRuntime` forwarder lambda passed to the ctor | `[this]` (`Impl`) | worker/io (via C3) | same as C3 |
| C5 | `Impl::forwardEvent` posted lambda | `[this, record]` (`Impl`) | io | `drain` before `transport_->stop()` (`AL12`); post checks `transport_->running()` |
| C6 | `PermissionBroker::State::resolve` cancellation/timeout callbacks | `weak_ptr<State>` + `request_id` | worker / io | weak, no dangling; `denyAll` before join |
| C7 | `AgentLoop::run`'s `sessionCancel.on_cancel([this]{ suspend(); })` | `[this]` (`AgentLoop`) | any canceller of the session token | **REPAIRED (finding 7):** `AgentLoop::run(CancellationToken)` is **deleted**. It is not on the `Agent` interface and has no production caller (verified: no `->run(`/`.run(` on an agent in `src/` or `tests/`). This removes the only capture that outlives its owner. If a session-cancel hook is ever needed, it must be installed by the daemon with a `std::weak_ptr` control block; that is out of scope. |
| C8 | `SubagentRunner::run` (`src/agent/subagent.cpp`) holds `Agent& child = registry_.get(*created)` across `child.send()`/`child.whenIdle()`/`child.session()`, then `Session& childSession = sessions_.session(childSessionId)` | raw `Agent&`/`Session&` | worker (inside a parent turn) | **REPAIRED (Rev 4):** hold `std::shared_ptr<AgentLoop> child = registry_.getShared(*created)` and `std::shared_ptr<Session> childSession = sessions_.sessionPtr(childSessionId)` for the whole `run` (AL2/AL35); `registry_.dispose(*created)` then drops only the registry reference. `parent_` is kept alive by the spawning turn's agent `session_owner_` (AL1). **This path is currently test-only** — the only callers are `tests/unit/agent_registry_test.cpp`; no production code constructs a `SubagentRunner`, but the contract is pinned so a future caller is safe. |
| C9 | `ToolContext` / `ChunkCoalescer` / `ContextAssembler` / `ContextCompactor` receive `Session&` | raw ref | worker | used only within the turn; the agent's strong ref holds the session (`AL1`). |
| C10 | ~~`SessionManager::setAgentLookup` stores `[this]` (`AgentRegistry`)~~ | — | — | **REMOVED (Rev 4, 24-D17):** `setAgentLookup`/`AgentLookup`/`findAgent`/`agent` are deleted, so no stored closure captures the registry and re-reads its map outside `mutex_`. |
| C11 | `AgentLoop::executeToolCall` installs `services_.gate->set_decision_hook(...)` into the long-lived, **workspace-shared** `PermissionGate` (`src/agent/agent_loop.cpp`; `WorkspaceRuntime` owns one `gate_`, `src/agent/workspace_runtime.cpp`) | **Rev 3: `std::weak_ptr<Session>`** (from `session_owner_`), never `[this]` | worker (the resolver thread; `emit_decision` runs inside `resolve`) | **REPAIRED (Rev 3, 24-D16/`AL34`):** pinned to **one** mechanism. The hook captures `std::weak_ptr<Session> weak = session_owner_` and does `if (auto s = weak.lock()) s->append(recorded);`. `PermissionGate` guards `decision_hook_`/`attention_hook_` with a leaf `hook_mutex_`; `set_decision_hook`/`set_attention_hook` write under it; `emit_decision` (`src/policy/permission_policy.cpp`, invoked from `resolve`) copies the hook under it and invokes the copy outside the lock. `AgentLoop::executeToolCall` installs the hook and clears it with an RAII scope guard; **`AgentLoop::dispose()` never touches the gate** (no stored `PermissionGate*`), so the Rev 2 race between a `dispose()` clear and `emit_decision` is gone. A stale hook copy can never dereference a destroyed loop: `weak.lock()` fails. Residual (recorded §13): because the gate is workspace-shared, a concurrent agent's install/clear can still overwrite another's hook; the mutex makes that non-UB and the weak capture makes it non-dangling, but per-agent routing is out of scope. |

`AL14` requires every capture in C1–C11 to be backed by a strong reference in
the body or by an owner that outlives the join (or the process hard-exits). C7
and C10 are **removed** (deleted callbacks); C8 is repaired to hold strong
`shared_ptr`s (24-D17); C11 is weakly owned and synchronized (24-D16/`AL34`);
no capture remains unguarded.

**Pinned C11 remediation (24-D16, Rev 3).** One mechanism, its synchronization,
and its test:

```cpp
// PermissionGate (include/ymh/policy/permission_policy.hpp): guard the hooks.
class PermissionGate {
    // ...
private:
    mutable std::mutex hook_mutex_;          // leaf (lock order §7.4 #8)
    DecisionHook       decision_hook_;       // guarded by hook_mutex_
    AttentionHook      attention_hook_;      // guarded by hook_mutex_
};

void PermissionGate::set_decision_hook(DecisionHook hook) {
    std::lock_guard<std::mutex> lock(hook_mutex_);
    decision_hook_ = std::move(hook);
}

void PermissionGate::emit_decision(const PermissionRequest& request,
                                   const PermissionOutcome& outcome) {
    DecisionHook hook;                        // copy under the lock ...
    {
        std::lock_guard<std::mutex> lock(hook_mutex_);
        hook = decision_hook_;
    }
    if (!hook) {
        return;
    }
    // ... invoke outside it. The hook is a weak-owner closure; a destroyed
    // session makes weak.lock() fail instead of dangling.
    payload::PermissionDecision decision;
    decision.call     = request.call;
    decision.decision = outcome.decision;
    decision.reason   = outcome.reason;
    hook(decision);
}

// AgentLoop::executeToolCall (src/agent/agent_loop.cpp): weak capture + RAII
// clear. dispose() never touches the gate.
std::weak_ptr<Session> weak = session_owner_;
services_.gate->set_decision_hook(
    [weak](const payload::PermissionDecision& recorded) {
        if (auto session = weak.lock()) {
            session->append(recorded);
        }
    });
struct HookClear {
    PermissionGate* gate;
    ~HookClear() { gate->set_decision_hook({}); }
} clear{services_.gate};
```

`emit_decision` runs on the resolver thread — the thread that called
`resolve` (the worker running `executeToolCall`); `decide` does not invoke it.
`resolve` likewise snapshots `attention_hook_` under `hook_mutex_` before the
`attention_hook_(id, request)` call, so both hooks share one synchronization
rule. Tests: `AL-U20` (weak capture, no append after the owner dies, no UAF) and
`AL-U21` (TSan concurrent set/clear/emit).

### 7.4 Total lock order (finding 1, finding 11; AL27)

A single total order; **no edge may point upward**:

```text
1. AgentRegistry::mutex_
2. SessionManager::mutex_
3. AgentLoop::control_mutex_
4. Session::appendMutex_
5. HostRuntime::forward_mutex_
6. TurnExecutor::State::mutex_
7. PermissionBroker::State::mutex_
8. PermissionGate::hook_mutex_
```

**Acquisition is sequential, not nested (24-D18).** Lock 1
(`AgentRegistry::mutex_`) is the **map-only** lock: it is held only across
`agents_`/`bySession_`/`leases_` operations and is released before any call that
could take another lock. Lock 2 (`SessionManager::mutex_`) is **not** map-only:
`deleteSession` holds it across `store_->eraseWithEvent` **and**
`publishCommitted` (the pinned 2→5 downward edge, §7.2), so a concurrent
`resumeSession` cannot resurrect a deleted session; the publish is non-blocking
and the committed handler never re-enters `SessionManager`. No edge below is
*held* from lock 1, and the only edges held from lock 2 are that pinned 2→5 bus
edge and lock 4's 4→5 edge (`appendEventLocked`). Where a clause writes "1 → 3"
or "1 → 2", the arrow means "acquired after 1 is released", not "held across".

Verified edges:

- `AgentRegistry::dispose`/`finalizeAll`: copy the strong ref under 1, **release
  1**, then call `agent->dispose()` (3) and `SessionManager::closeSession` (2),
  then re-lock 1 to erase. **Sequential; no 1→2 or 1→3 edge is held** (24-D18).
  This is the correction that makes §7.2, §3.3, and §7.4 agree.
- `AgentRegistry::findShared`/`getShared`: take 1 only to copy the `shared_ptr`;
  release before returning. No edge is held across the caller's use.
- `AgentRegistry::activateSession`/`suspendSession`/`requestCompaction`/
  `hasPendingWork`: take 1 only to copy the strong ref (R1–R4), release, then
  call the agent method outside. No lock is held across the call.
- `AgentRegistry::registerAgent`: resolves `sessionPtr` (2) **before** taking 1,
  then inserts under 1. Sequential; no 1→2 edge.
- `SessionManager::deleteSession`: 2 → 5 (`publishCommitted` →
  `handleCommittedRecord`). Downward. The publish does not take `mutex_` again
  (the committed handler never calls back into `SessionManager`).
- `Session::appendEventLocked`: 4 → 5 (`publishCommitted`). Downward. (This is
  the correction to Rev 1's reversed contract: `appendMutex_` is acquired
  **before** `forward_mutex_`, because the append publishes after committing.)
- `HostRuntime::hasPendingWork`: calls `AgentRegistry::hasPendingWork` (1,
  released) then `turns_.inFlight` (6). **Sequential**, never nested.
- `HostRuntime::forward_mutex_` → `Impl::forwardEvent` → `transport_->post`:
  transport-internal locks only; no edge back to 1–4.
- `TurnExecutor::State::mutex_` is never held across `body()`; `drain` holds it
  only across the cv wait.
- `PermissionBroker::State::mutex_` is a leaf; `resolve` never holds it across
  `transport.broadcast_permission_request`.
- `PermissionGate::hook_mutex_` (8) is a leaf: `set_decision_hook` /
  `set_attention_hook` hold it only to assign; `emit_decision` holds it only to
  **copy** the hook, then releases it before invoking the copy (which may call
  `Session::append` → 4/5). No edge from 8 points upward.

`AgentLoop::control_mutex_` (3) is a leaf relative to 4–8: no `Session::append`,
no `publish`, no `submit`, no `resolve` is called while it is held.

---

## 8. Invariants

Numbered `AL1`–`AL37`; testable and cited. Any code that can violate one is a
defect.

| ID | Invariant |
|---|---|
| AL1 | **Reference-counted session ownership.** `SessionManager` owns `std::shared_ptr<Session>`; `AgentLoop` holds a strong `std::shared_ptr<Session>` anchor. A `Session` is freed only when its last strong reference drops. |
| AL2 | **Reference-counted agent ownership.** `AgentRegistry` owns `std::shared_ptr<AgentLoop>`; every `TurnExecutor` body that touches an agent holds a strong `std::shared_ptr<AgentLoop>` for the body's whole duration — resolved via `findShared` at body start or held by the registry method it calls (`activateSession`/`requestCompaction`) — and every `HostRuntime` handler that dereferences an agent holds a strong reference from `findShared`/`getShared` for its whole body. An agent is freed only after its last referencing body/handler returns. |
| AL3 | **Acyclic strong ownership.** The strong graph is task→agent→session, registry→agent, manager→session; no back-edge and no `shared_ptr` cycle. |
| AL4 | **Park on close/delete.** `AgentLoop::dispose()` and `AgentRegistry::dispose()` never wait for a worker; they erase ownership references and return. The last strong reference frees. |
| AL5 | **Dispose does not falsify quiescence.** `dispose()` must not clear `running_` or force a terminal `state_` while a body is executing; the turn body is the sole writer of `running_` and the turn-terminal `state_`. |
| AL6 | **Queue-aware pending predicate.** `HostRuntime::hasPendingWork(id)` is true iff the agent has a queued trigger/in-flight turn **or** `TurnExecutor::inFlight(id) > 0`. |
| AL7 | **Delete refuses while busy.** `HostRuntime::deleteSession` refuses (`InvalidParams`, `"turn in progress"`) while `hasPendingWork(id)` holds; dispatch is io-serialized so the check-then-act is atomic w.r.t. other RPCs. |
| AL8 | **Dispose is terminal and idempotent.** After `dispose()`, every inbox op returns `AgentDisposed`; no new turn starts; a second `dispose()` is a no-op (A14 preserved). |
| AL9 | **In-flight turn observes dispose.** A disposed agent's in-flight body stops at the next cancellation checkpoint and appends no new turn. |
| AL10 | **Join on shutdown, never detach.** `TurnExecutor::drain` never calls `detach()`. |
| AL11 | **Bounded hard exit.** On `DrainResult::TimedOut`, the coordinator performs best-effort cleanup and `std::_Exit` with no destructors; it never returns to normal teardown with a live worker. |
| AL12 | **Drain before transport stop; destroy after drain.** The coordinator quiesces the executor before `TransportServer::stop()`, and destroys `Impl` members only after `Quiesced` (or not at all on hard exit). |
| AL13 | **Wake before join.** The coordinator cancels every in-flight turn and denies every pending permission before joining. |
| AL14 | **No worker-side `this` outlives its owner.** Every raw `this` captured into a body or callback (audit C1–C11) is owned by an object that outlives the join or the process hard-exit; C7 is deleted and C11 is weakly owned and synchronized (`AL34`). |
| AL15 | **Control-state synchronization.** `AgentLoop` control flags are atomics; `inbox_`, `idle_callbacks_`, `cancel_reason_`, `pending_maintenance_failure_` are guarded by the leaf `control_mutex_`; no lock is held across a blocking provider/tool/permission wait. |
| AL16 | **Committed events carry their `Sequence`.** Every durable append publishes an `EventRecord{seq,event}` on the committed channel; the daemon forwarder never re-reads the store for a `Sequence`. |
| AL17 | **Terminal event survives row erasure.** The delete's `SessionEnded` is forwarded exactly once with its real store `Sequence`, even though the row was erased in the same transaction (01 I16, 05 §7.4/§8.4). |
| AL18 | **Forwarding is monotonic and deduped.** A committed record is forwarded iff its per-session `seq` exceeds the last forwarded `seq`; equal/lower seqs are dropped. |
| AL19 | **Backpressure unchanged.** The bounded queue remains the only backpressure; `submit` returns false when full; no thread is spawned; the pending predicate does not change capacity (E7). |
| AL20 | **No wire/schema/dependency change.** The JSON-RPC surface, `kProtocolVersion`, `registry.db`/`sessions.db` schema `1`, and the dependency set are unchanged. |
| AL21 | **Path safety unchanged.** No new `getcwd()`; every tool/LSP/git/subprocess path still resolves via `ExecutionEnvironment::resolve()`. |
| AL22 | **Detach emits no `SessionEnded`.** `session.close`/detach emits no `SessionEnded` (01 I16); park never erases durable history. |
| AL23 | **Detach is non-destructive.** A supervisor detach (explicit `session.close`, connection drop, or non-last supervisor exit) never cancels, disposes, or erases a resident `Session`; `HostRuntime::closeSession` performs no agent/PTY/session mutation (24-D10). |
| AL24 | **Daemon-owned turn lifetime; one resident `Session` per id.** The daemon, not the supervisor, owns a running turn; `SessionManager::loadInto`/`resumeSession` never replace a resident `Session` (24-D10/24-D13). |
| AL25 | **Last-exit finalize + close.** On the last supervisor's exit, `AgentRegistry::finalizeAll()` cancels each in-flight turn so its cancel path flushes the pending chunk batch and appends exactly one terminal event, then parks each agent, calls `SessionManager::closeSession(id)` for each resident session, and erases the maps; no `SessionEnded` is emitted (24-D10/24-D15). |
| AL26 | **Detached-session reachability.** A detached-but-running session on a live daemon is surfaced by spec 22's live-only switcher and `/sessions`, and is controllable via `session.resume` (idempotent) + `session.activate`/`agent.prompt` from another supervisor (24-D10). |
| AL27 | **Registry/session map synchronization.** `AgentRegistry::agents_`/`bySession_`/`leases_` and `SessionManager::sessions_` are guarded by their `mutex_`; the total lock order of §7.4 holds and no edge points upward. The registry mutex is a **map-only** lock (24-D18): it is never held across a call, predicate, hook, or `shared_ptr` destructor (§7.2/§3.3/§7.4 state this one rule identically). |
| AL28 | **Child teardown before transport stop.** PTY (`LocalPtyService::closeAll()`) and MCP (`McpManager::shutdown(grace)`) are shut down after quiesce and before `TransportServer::stop()` (24-D9). |
| AL29 | **Committed subscriptions unsubscribe.** A `subscribeCommitted` handle carries a channel kind and `unsubscribe()` removes the handler; `~HostRuntime` leaves no committed handler behind (24-D12). |
| AL30 | **No raw map-derived handle.** `AgentRegistry::find`/`get` and `SessionManager::AgentLookup`/`setAgentLookup`/`findAgent`/`agent` are deleted, and `SessionManager::session()` is private (24-D17). Every `HostRuntime` handler, `TurnExecutor` body, and `SubagentRunner` use obtains an agent via `AgentRegistry::findShared`/`getShared` (or delegates to a registry method that holds one, R1–R6) and a session via `SessionManager::sessionPtr`, holding the returned `shared_ptr` for its whole duration. No raw `Agent*`/`Agent&`/`Session*`/`Session&` from a registry/manager accessor escapes (24-D14). |
| AL31 | **`finalizeAll`/`dispose` cannot dangle a concurrent user.** Erasing an `agents_` entry drops only the registry's strong reference; a caller's `findShared`/`getShared` reference keeps the agent alive until it returns, and the agent's `session_owner_` keeps its `Session` alive through `closeSession`; a caller that holds `sessionPtr` is covered independently of the agent. The registry's own methods hold such a reference across their call (24-D14/24-D18). |
| AL32 | **Non-last supervisor exit never shuts the daemon down.** `ProtocolServer::admit_shutdown` refuses a `host.shutdown{last_supervisor}` while another live supervisor, a live automation client, or another fresh owner exists; `WorkspaceHost::Impl::ownerWatchdogLoop` tears down only when no live owner and no fresh supervisor row remain (24-D10). |
| AL33 | **Last-exit close is explicit.** `AgentRegistry::finalizeAll` calls `SessionManager::closeSession(id)` for every resident session after parking its agent; the `Session` object is freed when its last strong reference drops and at the latest at process exit. "Saved into history and closed" is therefore two pinned steps, not an implicit side effect (24-D15). |
| AL34 | **Weakly owned, synchronized permission hook.** The `AgentLoop` decision hook captures `std::weak_ptr<Session>` (never `[this]`); `PermissionGate` guards its hooks with a leaf `hook_mutex_`; `emit_decision` copies the hook under the lock and invokes the copy outside it; `AgentLoop::dispose()` never clears the hook (24-D16). |
| AL35 | **Structural no-raw-pointer rule (repo-wide).** The only functions that yield an agent handle are `AgentRegistry::findShared`/`getShared`, both returning `std::shared_ptr<AgentLoop>`; the only public session accessor is `SessionManager::sessionPtr` (`session()` is private and not externally nameable). No accessor, stored closure, or cached member yields a raw `Agent*`/`Agent&`/`Session*`/`Session&` to a map-owned object, and the rule binds **every translation unit** — `src/` (including `src/cli/`, `src/ui/`, `src/agent/`), `tests/`, and test support headers — not just `src/host/` (24-D17/24-D19; proof §7.2.1). |
| AL36 | **Single map-only lock-scope rule.** `AgentRegistry::mutex_` is held only across `agents_`/`bySession_`/`leases_` container operations; it is never held across a call into `AgentLoop`/`Session`/`SessionManager`/provider/permission/executor, a predicate/hook, or a `shared_ptr` destructor. `finalizeAll` snapshots under the lock, releases, acts, then re-locks to erase (24-D18). |
| AL37 | **Repo-wide scope, mechanized enforcement, and no dead cached member.** The deleted accessors (`AgentRegistry::get`/`find`; `SessionManager::AgentLookup`/`setAgentLookup`/`findAgent`/`agent`) are removed from the type system and `SessionManager::session()` is private, so the build fails by construction if any translation unit under `src/`, `tests/`, or `include/` names one; the residual `.get()`/`*` case the compiler cannot see is covered by the runnable repo-wide scan `AL-U22` (proof §7.2.1 point 7). The dead `run_tui`/`TuiApp` path — whose `Agent& agent_` member was the last cached raw map-derived handle — is removed (24-D19); the only cached raw agent/session members are the non-map-derived, agent-anchored references of §7.2.1 point 6. A check scoped to `src/host/` is insufficient and is prohibited (24-D19). |

---

## 9. Failure modes

Prefix `AL-F#` (trigger / symptom / recovery). The **Shared F#** column maps to
the `00` §54 review-finding taxonomy where one applies; `—` means a
memory-safety/internal-consistency defect with no shared id.

| ID | Trigger | Symptom | Recovery | Shared F# |
|---|---|---|---|---|
| AL-F1 | `close`/`delete` while a worker is inside `Session::append` | Use-after-free of `Session`; crash or corruption | Refcount park (AL1/AL2/AL4) | — |
| AL-F2 | `drain` grace expires and the worker is detached, then `Impl` members are destroyed | Detached body touches destroyed `HostRuntime`/`WorkspaceRuntime`; crash | Never detach; hard exit (AL10/AL11) | — |
| AL-F3 | Delete passes the guard while a prompt is queued in `TurnExecutor` | An acked prompt is silently dropped | Queue-aware predicate (AL6/AL7) | — |
| AL-F4 | Delete's `SessionEnded` row is erased in the same transaction | Forwarder's `readAfter` is empty; terminal event never reaches the client; 01 I16 / 05 §7.4 violated | Committed-record forwarding (AL16/AL17) | F3 |
| AL-F5 | `dispose` clears `running_`/`state_` while the body runs | `whenIdle`/`hasPendingWork` report false quiescence; early free | AL5 | — |
| AL-F6 | Worker blocked in `PermissionBroker::resolve` at join | Coordinator deadlock; teardown never completes | `denyAll` before join (AL13) | — |
| AL-F7 | A worker body's raw `this` outlives its owner | Use-after-free in the body | Capture audit + join (AL14) | — |
| AL-F8 | A strong `shared_ptr` cycle forms (agent↔executor/registry) | Memory leak; agents never freed | Acyclic graph (AL3) | — |
| AL-F9 | Two bodies mutate `AgentLoop` control state concurrently | Data race; torn inbox/state | Atomics + leaf mutex (AL15) | — |
| AL-F10 | A worker `post()`s after `TransportServer::stop()` | Post to a stopped transport; dropped or UAF | Drain before stop (AL12) | — |
| AL-F11 | `last_forwarded_` is stale/out of order | A valid record is dropped | Monotonic guard + tests (AL18) | — |
| AL-F12 | `~TurnExecutor` runs with joinable workers | `std::terminate` (joinable `std::thread` destructor) | Hard exit on `TimedOut` (AL11) | — |
| AL-F13 | A detach (`session.close`) cancels or disposes a running turn | A daemon-owned turn is killed; a detached session becomes uncontrollable | Detach-only `closeSession` (AL23/AL24) | — |
| AL-F14 | A prompt is submitted between the delete guard and the erase | Racy delete of a just-prompted session | io-serialized check-then-act (AL7) | — |
| AL-F15 | The shipped cancel-on-close path is retained | `session.close` kills a running turn; 05 §7.4 violated | 24-D10 detach (AL23) | — |
| AL-F16 | Last supervisor exits while a turn is in flight and the daemon discards it | The turn's partial history is lost; no terminal event | `finalizeAll` before drain (AL25) | — |
| AL-F17 | A second supervisor resumes a detached session | `loadInto` replaces the resident `Session`; two appenders | Idempotent `loadInto` + `resume` (AL24) | — |
| AL-F18 | `finalizeAll`/worker `SubagentRunner` mutate maps while an io handler reads them | Concurrent `unordered_map` read+write; UB/crash | `mutex_` + total lock order (AL27) | — |
| AL-F19 | MCP/PTY children shut down after `TransportServer::stop()` | A child outlives the socket that reports it | `shutdownChildren` before stop (AL28) | — |
| AL-F20 | A committed handler is never unsubscribed | `~HostRuntime` leaks the handler; later publish calls into freed state | Channel-kind `Subscription` (AL29) | — |
| AL-F21 | A resident `Session` is replaced by a second `loadInto` | Two `Session` objects for one id; divergent logs | Idempotent `loadInto` (AL24) | — |
| AL-F22 | A caller — an io handler or a registry method (`activateSession`/`suspendSession`/`requestCompaction`/`hasPendingWork`/`dispose`) — holds a raw `Agent*`/`Agent&` from `find`/`get` (or a raw `Session&` from `session()`) while the coordinator's `finalizeAll` erases the entry | Use-after-free in the caller (`agent->state()`/`status()`/`cancel()`/`activate()`/`suspend()`, `session->append`) | `findShared`/`getShared`/`sessionPtr` strong ref held for the whole use (AL30/AL31) | — |
| AL-F23 | Last-exit `finalizeAll` parks agents but never calls `closeSession` | The resident session is never explicitly closed; the user's "saved into history and closed" is only half-satisfied | `finalizeAll` calls `SessionManager::closeSession` (AL33) | — |
| AL-F24 | The permission hook captures `[this]` and is cleared from another thread while `emit_decision` invokes it | Data race on the `std::function`; possible dangling `AgentLoop` | Weak `Session` capture + gate `hook_mutex_`; `dispose()` never clears (AL34) | — |
| AL-F25 | A non-last supervisor exit is admitted as a daemon shutdown | The daemon exits and kills the still-owned active turn instead of detaching | `admit_shutdown` + owner watchdog refuse (AL32) | — |
| AL-F26 | A worker body (`SubagentRunner::run`) holds a raw `Agent&`/`Session&` from `get`/`session` across `send()`/`whenIdle()`/`session()` while `finalizeAll` erases the entry | Use-after-free in the subagent worker; a child whose registry entry was finalized is dereferenced | `getShared` + `sessionPtr` held for the whole `run` (AL30/AL35) | — |
| AL-F27 | A raw accessor or the deleted lookup closure is reintroduced (`Agent& get`/`Agent* find`, or `AgentLookup`/`findAgent`/`agent`) and a caller holds the raw pointer past `mutex_` release | The Rev 3 HIGH returns: `finalizeAll` frees the agent between lookup and use | Only `findShared`/`getShared`/`sessionPtr` exist; structural rule AL35/§7.2.1 | — |
| AL-F28 | `AgentRegistry::mutex_` is held across a predicate/call (e.g. `hasPendingWork`'s predicate or `dispose`/`closeSession`) | Dangling call or lock-order inversion/deadlock; §7.2/§3.3/§7.4 disagree | Map-only lock-scope rule AL36/24-D18; `finalizeAll` snapshot-release | — |
| AL-F29 | A raw map-derived handle survives **outside `src/host/`** — the dead `TuiApp::agent_` member, a test helper returning `Agent&`/`Session&`, or the live `headless.cpp` local — so a `src/host/`-scoped check misses it | The Rev 4 HIGH: a cached/returned raw handle to a map-owned agent/session is used after `finalizeAll`/`dispose` frees it — use-after-free | Delete the accessors (24-D17) + repo-wide check (`AL-U22`/`AL-U24`) + migrate X1–X10 and remove the dead path (24-D19/`AL37`) | — |

---

## 10. dsh mapping

- **Event-sourced truth.** The committed-record channel does not change the
  source of truth: the store remains the durable log and the bus is the
  observation edge. Observation now carries the committed position (`Sequence`)
  from the commit site instead of re-deriving it, so a row-erasing terminal
  event is still observable.
- **Ownership.** `AgentRegistry` owns `shared_ptr<AgentLoop>` and
  `SessionManager` owns `shared_ptr<Session>`; a turn body is a short-lived task
  that borrows a strong reference. Reference counting is the harness's natural
  "task holds the agent alive" rule. The registry/session maps are the
  harness's service registry, so they are mutex-guarded like any shared service
  table. The map lookup returns an **owning `shared_ptr`**, never a raw pointer
  (24-D17); the strong reference, not an index, is the ownership edge, and every
  caller — production, test support, or test — holds it for as long as it uses
  the agent (`AL30`/`AL35`/`AL37`). The rule is a type-system property of the
  whole tree, not a per-directory convention.
- **Lifecycle split.** *Detach* ends observation; *close* parks the agent and
  explicitly closes the resident session; *shutdown* finalizes and joins. This
  mirrors dsh's plugin-lifecycle split: a plugin client disconnecting never stops
  the host; only host shutdown finalizes the run. The turn's lifetime is
  host-owned (24-D10). A non-last peer leaving is a detach, admitted as a
  shutdown only when the caller is the last owner (`AL32`).
- **Failure isolation.** A stuck turn cannot corrupt memory or leak into a freed
  daemon: the process hard-exits bounded by the grace. A detached turn keeps
  running on the live daemon and is reachable from any peer supervisor, which is
  the harness's collective-ownership model applied to turns.
- **No new substrate.** No new IPC, wire message, thread, or dependency: the
  change is internal ownership, one additive bus channel, and the map/hook
  mutexes.

---

## 11. Test plan

Concrete IDs; extend the named existing files. Deterministic tests use the Fake
LLM / in-memory stores; live tests are opt-in (`YMH_LIVE_LLM=1`).

**Memory-safety rule (mandatory).** Every test that exercises a close/delete
during an in-flight turn, a detach during a turn, or a daemon shutdown with a
live turn must run under **ASan and TSan** in at least one CI configuration; a
test that only asserts "the process exited 0" is incomplete for AL-F1/AL-F2/
AL-F18. This is now mechanized: CMake exposes `YMH_ASAN` and `YMH_TSAN`
(`-fsanitize=address` / `-fsanitize=thread`, both `-O0` to avoid GCC 16
optimization-dependent `-Wmaybe-uninitialized` false positives in libstdc++
`std::regex` and nlohmann `json::items()`; warnings stay errors), and
`.github/workflows/ci.yml` runs the suite under both.

**Rev 7 correction.** Rev 6's §11.1–§11.7 named ~40 test IDs of which ~24 did
not exist anywhere in `tests/`. Every table below now names **only tests that
exist**, by their real `Suite.TestName`; the planned-but-unimplemented IDs are
moved to §11.8 and recorded as accepted risks in §13.

### 11.1 Unit (implemented)

| ID | Test (`file`) | Assertion |
|---|---|---|
| AL-U1 | `SessionManager.AL_U1_SessionPtrKeepsSessionAliveAfterClose` (`session_test.cpp`) | `sessionPtr` keeps a `Session` alive after `closeSession` while an external `shared_ptr` is held (AL1). |
| AL-U2 | `AgentInbox.OperationsAfterDisposeReturnAgentDisposed` (`agent_inbox_test.cpp`) | After `dispose()`, every inbox op returns `AgentDisposed`; `disposed()` is set (AL4/AL8). |
| AL-U3 | `AgentLoop.AL_U3_PausedBodyReportsPendingWorkAndDefersIdleCallback` (`agent_loop_test.cpp`), `AgentInbox.WhenIdleFiresImmediatelyAndDeferred` | A paused body reports pending work and defers `whenIdle`; it fires only after the body returns (AL5). |
| AL-U4 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` (`agent_registry_test.cpp`) | `findShared` yields a strong `shared_ptr<AgentLoop>` (AL2). |
| AL-U5 | `TurnExecutor.AL_U5_InFlightIsPerSessionAndCapacityRejectsOverflow` (`agent_turn_executor_test.cpp`) | `inFlight(session)` counts queued+active per session; capacity still rejects overflow (AL6/AL19). |
| AL-U6 | `TurnExecutor.AL_U6_DrainQuiescesAfterBodiesFinish` (`agent_turn_executor_test.cpp`) | `drain` returns `Quiesced` after bodies finish; the destructor is then safe (no hard exit) (AL10). |
| AL-U7 | `TurnExecutor.AL_U7_TimedOutThenQuiescedAfterRelease` (`agent_turn_executor_test.cpp`) | A latch-blocked body yields `TimedOut`; after release a second `drain` returns `Quiesced` (AL10). |
| AL-U8 | `EventBusTest.AL_U8_CommittedRecordReachesCommittedAndLiveHandlers` (`event_bus_test.cpp`) | `subscribeCommitted` receives `EventRecord{seq,event}`; `publishCommitted` also delivers the `Event` to `subscribe` handlers (AL16). |
| AL-U9 | `Session.AL_U9_AppendPublishesCommittedRecordWithStoreSequence` (`session_test.cpp`) | `Session::append` publishes a committed record whose `seq` equals the store-assigned sequence (AL16). |
| AL-U10 | `SessionStoreSeam.AL_U10_EraseWithEventReturnsSequenceAndManagerPublishesCommitted` (`persistence_test.cpp`) | `eraseWithEvent` returns the terminal `Sequence`; `SessionManager::deleteSession` publishes `{seq, SessionEnded}` (AL16/AL17). |
| AL-U11 | `HostRuntimeTest.AL_U11_ForwardsErasedRowSessionEndedWithSequence` (`host_runtime_test.cpp`) | Forwards an erased-row `SessionEnded` with its real sequence, no `readAfter` (AL17). |
| AL-U12 | `HostRuntimeTest.AL_U12_MonotonicGuardDropsLowerAndEqualSequences` (`host_runtime_test.cpp`) | A lower/equal `seq` is not forwarded; a higher one is (AL18). |
| AL-U13 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` (`agent_registry_test.cpp`) | No `shared_ptr` cycle: after the held handles drop, agent and session are destroyed (`weak_ptr` expired) (AL3). |
| AL-U14 | `AgentLoop.AL_U14_ControlMutexIsNotHeldAcrossProviderStream` (`agent_loop_test.cpp`) | The provider stub re-enters `hasPendingWork()` from inside `stream()`; holding `control_mutex_` across it would deadlock (AL15). |
| AL-U15 | `AgentRegistry.AL_U15_ConcurrentCreateDisposeAndReadsAreRaceFree` (`agent_registry_test.cpp`) | Concurrent create/dispose/`hasPendingWork`/`list`/`activeCount` from N threads do not race the registry maps; TSan-clean (AL27/AL-F18). |
| AL-U16 | `EventBusTest.AL_U16_CommittedSubscriptionUnsubscribes` (`event_bus_test.cpp`) | A committed `Subscription` unsubscribes; after `unsubscribe()` the handler is not invoked (AL29/AL-F20). |
| AL-U17 | `TurnExecutor.AL_U17_DrainIsIdempotent` (`agent_turn_executor_test.cpp`) | `drain` idempotency: `Quiesced` stays `Quiesced` (AL10). |
| AL-U18 | `SessionManager.AL_U18_ResumeSessionKeepsResidentSessionIdentity` (`session_test.cpp`) | `resumeSession`/`loadInto` on a resident id returns the same `Session` object (pointer identity) (AL24/AL-F17/AL-F21). |
| AL-U19 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` (`agent_registry_test.cpp`) | `findShared`'s strong reference keeps the agent (and its `Session`) alive after `finalizeAll` erases the entry (AL30/AL31/AL-F22). |
| AL-U20 | `AgentLoop.AL_U20_RealDecisionHookCaptureIsWeak` (`agent_loop_test.cpp`), `PermissionGate.AL_U20_WeakSessionCaptureDoesNotDangle` (`permissions_test.cpp`) | The **real** production decision hook is snapshotted via `PermissionGate::decision_hook_for_test()`, its owner torn down, and the captured copy invoked: weak capture no-ops, `[this]` is an ASan heap-use-after-free (AL34/AL-F24). |
| AL-U21 | `PermissionGate.AL_U21_ConcurrentHookSetClearAndEmitAreRaceFree` (`permissions_test.cpp`) | Concurrent `set_*_hook` and `emit_decision` do not race the hook members; TSan-clean (AL34). |
| AL-U22 | `AgentLifetimeGuard.NoRawMapDerivedHandleEscapes` (`no_raw_map_handle_test.cpp`) | Repo-wide lexical guard: deleted accessor names, raw extraction from a handle-producing call, any function returning a raw `Agent`/`AgentLoop`/`Session` handle (allowlisted private/borrowed declarations only), and a raw handle extracted from an owning local then returned/stored into a member (AL35/AL37/AL-F27/AL-F29). |

### 11.2 Integration (implemented)

| ID | Test (`file`) | Assertion |
|---|---|---|
| AL-I2 | `HostRuntimeTest.AL_I2_DeleteRefusesWhilePromptIsQueued` (`host_runtime_test.cpp`) | Delete while a prompt is queued returns `InvalidParams "turn in progress"`; the prompt later runs (AL7/AL-F14). |
| AL-I3 | `HostRuntimeTest.AL_I3_SubscribedClientReceivesDeletedSessionEndedWithCursor` (`host_runtime_test.cpp`) | A subscribed client receives `SessionEnded{Deleted}` with a valid cursor (AL17). |
| AL-I4 | `WorkspaceHostLifecycle.ForegroundServingClaimAndGracefulShutdown` (`workspace_host_test.cpp`), `HostRuntimeTest.AL_I10_FinalizeAllFinalizesInFlightTurn` | Graceful shutdown drains before stop and returns `Ok`; the in-flight finalize variant closes with one terminal event (AL10/AL12/AL25). |
| AL-I6 | `PermissionBroker.DenyAllResolvesEveryPendingRequest` (`permission_broker_test.cpp`) | A permission-blocked worker is woken by `denyAll`; the wake-before-join seam resolves every pending Ask (AL13/AL-F6). |
| AL-I7 | `WorkspaceHostQueue.AL_I7_DeleteIdleWhileOtherRunsHasNoCrossSessionEffect` (`workspace_host_test.cpp`) | Two sessions; delete one while the other runs; no cross-session effect (AL6/AL7). |
| AL-I9 | `HostRuntimeTest.AL_I9_CloseDetachesWithoutCancellingTheTurn` (`host_runtime_test.cpp`) | `session.close` on a running turn does **not** cancel it: the turn completes, no `SessionEnded`, the session stays resident (AL22/AL23/AL-F13/AL-F15). |
| AL-I10 | `HostRuntimeTest.AL_I10_FinalizeAllFinalizesInFlightTurn` (`host_runtime_test.cpp`) | Last-supervisor exit finalizes an in-flight turn: exactly one `TurnCancelled` terminal event (no natural `TurnEnded`), no `SessionEnded` (AL25/AL-F16). |
| AL-I11 | `TwoProcess.SupervisorExitWithPeerKeepsDaemon` (`integration_two_process_test.cpp`) | Supervisor A exits (detach); the daemon survives and the session stays controllable by supervisor B (AL23/AL24/AL26/AL-F17). |
| AL-I12 | `WorkspaceHostTeardown.AL_I12_ChildTeardownPrecedesTransportStop` (`workspace_host_test.cpp`) | Source-order guard: the coordinator's `shutdownChildren` call precedes `transport_->stop()` (AL28/AL-F19). |
| AL-I13 | `HostRuntimeTest.AL_I13_DestructorUnsubscribesCommittedHandler` (`host_runtime_test.cpp`) | `~HostRuntime` leaves no committed handler behind (AL29/AL-F20). |
| AL-I15 | `HostRuntimeTest.AL_I15_FinalizeAllClosesResidentSessionsWithoutSessionEnded` (`host_runtime_test.cpp`) | Last-exit `finalizeAll` calls `closeSession` for each resident session; the manager map empties; no new terminal event and no `SessionEnded` (AL25/AL33/AL-F23). |
| AL-I16 | `TransportServer.HostShutdownRefusedWhenAnotherSupervisorIsLive`, `HostShutdownRefusedWhenAutomationIsLive`, `HostShutdownRefusedByFreshSnapshotOwnerExcludingCaller`, `ObserverShutdownRefusedWhileSupervisorOwns` (`transport_server_test.cpp`), `TwoProcess.LastSupervisorExitTearsDaemonDown` | A non-last supervisor exit is refused (`NotLastOwner`), the daemon survives; the last owner's exit is admitted and tears down (AL32/AL-F25). |

### 11.3 Subprocess / crash-injection (implemented)

| ID | Test (`file`) | Assertion |
|---|---|---|
| AL-S1 | `TurnExecutor.AL_S1_TimedOutDrainHardExitsTheProcess` (`agent_turn_executor_test.cpp`) | A forked child with an uncancellable body drains `TimedOut` and `~TurnExecutor` calls `std::_Exit(HostExitCode::Internal=17)`; the parent asserts the exit code (AL11/AL-F12). |

### 11.4 Golden / replay (implemented)

| ID | Test (`file`) | Assertion |
|---|---|---|
| AL-G1 | `RenderGolden.*` (`render_golden_test.cpp`, `ui_render_golden_test.cpp`) | No golden UI render changes (the fix is non-UI) (AL20). |

### 11.5 Live (opt-in) — not implemented

`AL-L1`/`AL-L2` were planned and are **not implemented**; see §11.8/§13.
The live suites (`LiveLlmTest`, `LiveHeadlessTest`, `UiLivePty`,
`UiLiveResumePty`, `LiveE2E`) exist and are skipped unless `YMH_LIVE_LLM=1`.

### 11.6 Invariant coverage

`ACCEPTED RISK` means the invariant has no runnable test; the residual gap and
its compensating argument are recorded in §13.

| Invariant | Tests |
|---|---|
| AL1 | `SessionManager.AL_U1_SessionPtrKeepsSessionAliveAfterClose`, `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` |
| AL2 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll`, `AgentRegistry.FinalizeAllRacesLiveWorkerWithoutCorruption` |
| AL3 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` (weak expiry) |
| AL4 | `AgentInbox.OperationsAfterDisposeReturnAgentDisposed` |
| AL5 | `AgentLoop.AL_U3_PausedBodyReportsPendingWorkAndDefersIdleCallback`, `AgentInbox.WhenIdleFiresImmediatelyAndDeferred` |
| AL6 | `TurnExecutor.AL_U5_InFlightIsPerSessionAndCapacityRejectsOverflow`, `AgentRegistry.AL_U15_ConcurrentCreateDisposeAndReadsAreRaceFree` |
| AL7 | `HostRuntimeTest.AL_I2_DeleteRefusesWhilePromptIsQueued`, `WorkspaceHostQueue.AL_I7_DeleteIdleWhileOtherRunsHasNoCrossSessionEffect` |
| AL8 | `AgentInbox.OperationsAfterDisposeReturnAgentDisposed` |
| AL9 | `HostRuntimeTest.AL_I10_FinalizeAllFinalizesInFlightTurn`, `AgentLoop.CancelMidStreamYieldsOneTurnCancelled` |
| AL10 | `TurnExecutor.AL_U6_DrainQuiescesAfterBodiesFinish`, `TurnExecutor.AL_U7_TimedOutThenQuiescedAfterRelease` |
| AL11 | `TurnExecutor.AL_S1_TimedOutDrainHardExitsTheProcess` (TurnExecutor); daemon-coordinator `_Exit` — ACCEPTED RISK (§13) |
| AL12 | `WorkspaceHostLifecycle.ForegroundServingClaimAndGracefulShutdown` |
| AL13 | `PermissionBroker.DenyAllResolvesEveryPendingRequest` |
| AL14 | `AgentLoop.AL_U20_RealDecisionHookCaptureIsWeak`, `TurnExecutor.AL_S1_TimedOutDrainHardExitsTheProcess`, capture audit (§7.3) |
| AL15 | `AgentLoop.AL_U14_ControlMutexIsNotHeldAcrossProviderStream`, `AgentRegistry.FinalizeAllRacesLiveWorkerWithoutCorruption` |
| AL16 | `EventBusTest.AL_U8_CommittedRecordReachesCommittedAndLiveHandlers`, `Session.AL_U9_AppendPublishesCommittedRecordWithStoreSequence` |
| AL17 | `SessionStoreSeam.AL_U10_EraseWithEventReturnsSequenceAndManagerPublishesCommitted`, `HostRuntimeTest.AL_U11_ForwardsErasedRowSessionEndedWithSequence`, `HostRuntimeTest.AL_I3_SubscribedClientReceivesDeletedSessionEndedWithCursor` |
| AL18 | `HostRuntimeTest.AL_U12_MonotonicGuardDropsLowerAndEqualSequences` |
| AL19 | `TurnExecutor.AL_U5_InFlightIsPerSessionAndCapacityRejectsOverflow` |
| AL20 | `RenderGolden.*` + build (no schema/wire/dependency diff) |
| AL21 | `PathSafety.*` (unchanged, green) |
| AL22 | `HostRuntimeTest.AL_I9_CloseDetachesWithoutCancellingTheTurn` |
| AL23 | `HostRuntimeTest.AL_I9_CloseDetachesWithoutCancellingTheTurn`, `TwoProcess.SupervisorExitWithPeerKeepsDaemon` |
| AL24 | `SessionManager.AL_U18_ResumeSessionKeepsResidentSessionIdentity`, `TwoProcess.SupervisorExitWithPeerKeepsDaemon` |
| AL25 | `HostRuntimeTest.AL_I10_FinalizeAllFinalizesInFlightTurn`, `HostRuntimeTest.AL_I15_FinalizeAllClosesResidentSessionsWithoutSessionEnded` |
| AL26 | `TwoProcess.SupervisorExitWithPeerKeepsDaemon` |
| AL27 | `AgentRegistry.AL_U15_ConcurrentCreateDisposeAndReadsAreRaceFree` |
| AL28 | `WorkspaceHostTeardown.AL_I12_ChildTeardownPrecedesTransportStop` (call-order guard; the runtime `TransportServer::stop()` boundary has no seam — §13) |
| AL29 | `EventBusTest.AL_U16_CommittedSubscriptionUnsubscribes`, `HostRuntimeTest.AL_I13_DestructorUnsubscribesCommittedHandler` |
| AL30 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` |
| AL31 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` |
| AL32 | `TransportServer.HostShutdownRefusedWhenAnotherSupervisorIsLive`, `HostShutdownRefusedWhenAutomationIsLive`, `HostShutdownRefusedByFreshSnapshotOwnerExcludingCaller`, `ObserverShutdownRefusedWhileSupervisorOwns`, `TwoProcess.LastSupervisorExitTearsDaemonDown` |
| AL33 | `HostRuntimeTest.AL_I15_FinalizeAllClosesResidentSessionsWithoutSessionEnded` |
| AL34 | `AgentLoop.AL_U20_RealDecisionHookCaptureIsWeak`, `PermissionGate.AL_U21_ConcurrentHookSetClearAndEmitAreRaceFree` |
| AL35 | `AgentLifetimeGuard.NoRawMapDerivedHandleEscapes`, capture audit (§7.3) |
| AL36 | ACCEPTED RISK (§13 item 11) |
| AL37 | `AgentLifetimeGuard.NoRawMapDerivedHandleEscapes`, build-by-construction (§7.2.1 point 7) |

### 11.7 Failure-mode coverage

| Failure mode | Tests |
|---|---|
| AL-F1 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll`, `HostRuntimeTest.AL_I10_FinalizeAllFinalizesInFlightTurn` |
| AL-F2 | `TurnExecutor.AL_S1_TimedOutDrainHardExitsTheProcess` |
| AL-F3 | `HostRuntimeTest.AL_I2_DeleteRefusesWhilePromptIsQueued` |
| AL-F4 | `HostRuntimeTest.AL_U11_ForwardsErasedRowSessionEndedWithSequence`, `HostRuntimeTest.AL_I3_SubscribedClientReceivesDeletedSessionEndedWithCursor` |
| AL-F5 | `AgentLoop.AL_U3_PausedBodyReportsPendingWorkAndDefersIdleCallback` |
| AL-F6 | `PermissionBroker.DenyAllResolvesEveryPendingRequest` |
| AL-F7 | `AgentLoop.AL_U20_RealDecisionHookCaptureIsWeak`, `TurnExecutor.AL_S1_TimedOutDrainHardExitsTheProcess`, capture audit (§7.3) |
| AL-F8 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` (weak expiry) |
| AL-F9 | `AgentLoop.AL_U14_ControlMutexIsNotHeldAcrossProviderStream` |
| AL-F10 | `WorkspaceHostLifecycle.ForegroundServingClaimAndGracefulShutdown` |
| AL-F11 | `HostRuntimeTest.AL_U12_MonotonicGuardDropsLowerAndEqualSequences` |
| AL-F12 | `TurnExecutor.AL_S1_TimedOutDrainHardExitsTheProcess` |
| AL-F13 | `HostRuntimeTest.AL_I9_CloseDetachesWithoutCancellingTheTurn` |
| AL-F14 | `HostRuntimeTest.AL_I2_DeleteRefusesWhilePromptIsQueued` |
| AL-F15 | `HostRuntimeTest.AL_I9_CloseDetachesWithoutCancellingTheTurn` |
| AL-F16 | `HostRuntimeTest.AL_I10_FinalizeAllFinalizesInFlightTurn` |
| AL-F17 | `SessionManager.AL_U18_ResumeSessionKeepsResidentSessionIdentity` |
| AL-F18 | `AgentRegistry.AL_U15_ConcurrentCreateDisposeAndReadsAreRaceFree` |
| AL-F19 | `WorkspaceHostTeardown.AL_I12_ChildTeardownPrecedesTransportStop` |
| AL-F20 | `EventBusTest.AL_U16_CommittedSubscriptionUnsubscribes`, `HostRuntimeTest.AL_I13_DestructorUnsubscribesCommittedHandler` |
| AL-F21 | `SessionManager.AL_U18_ResumeSessionKeepsResidentSessionIdentity` |
| AL-F22 | `AgentRegistry.AL_U19_HeldHandleSurvivesFinalizeAll` |
| AL-F23 | `HostRuntimeTest.AL_I15_FinalizeAllClosesResidentSessionsWithoutSessionEnded` |
| AL-F24 | `AgentLoop.AL_U20_RealDecisionHookCaptureIsWeak`, `PermissionGate.AL_U21_ConcurrentHookSetClearAndEmitAreRaceFree` |
| AL-F25 | `TransportServer.HostShutdownRefusedWhenAnotherSupervisorIsLive`, `HostShutdownRefusedWhenAutomationIsLive`, `HostShutdownRefusedByFreshSnapshotOwnerExcludingCaller`, `ObserverShutdownRefusedWhileSupervisorOwns` |
| AL-F26 | ACCEPTED RISK (§13 item 12) |
| AL-F27 | `AgentLifetimeGuard.NoRawMapDerivedHandleEscapes` |
| AL-F28 | ACCEPTED RISK (§13 item 13) |
| AL-F29 | `AgentLifetimeGuard.NoRawMapDerivedHandleEscapes` |

### 11.8 Planned tests not implemented (accepted risks)

These planned IDs have **no runnable test**. They are recorded here so no table
claims a test that does not exist; the compensating arguments are in §13.

- `AL-U23` (registry `mutex_` map-only probe), `AL-U24` (test-support owning
  handles / repo-wide raw-site sweep), `AL-U25` (headless `getShared` under a
  concurrent `finalizeAll`).
- `AL-I1` (delete during an in-flight `Session::append`), `AL-I8` (headless
  receives the delete's `SessionEnded`), `AL-I14` (io-handler vs
  `finalizeAll`), `AL-I17` (TSan N-handler sweep).
- `AL-R1` (replay delete tail), `AL-L1`/`AL-L2` (opt-in live).
- Partially implemented: the daemon-coordinator half of `AL-I5` is not exercised
  end to end (`AL-S1` covers the `TurnExecutor` half); the unit form of
  `AL-I11` is not written (`AL-I11`'s invariant is covered by
  `TwoProcess.SupervisorExitWithPeerKeepsDaemon`, cited in §11.6).

---

## 12. Supersedes / amends

**Supersedes: none.** No normative text is withdrawn. The one behavioral
reversal is that the shipped `HostRuntime::closeSession` cancel-on-close is
declared a bug and 05 §7.4's detach text becomes authoritative (24-D10). Rev 3
corrects the *framing*: a non-last supervisor exit does not call
`session.close` at all (it is a connection drop plus an admission refusal), so
that path is already correct; only the explicit `session.close` RPC cancels a
daemon-owned turn.

**Amends (by reference):**

- **01 §5 (EventBus).** Adds `subscribeCommitted`/`publishCommitted`, the
  `RecordHandler` seam, and the `Subscription::Channel` discriminator (24-D12).
  The mailbox ordering contract (I6) is preserved.
- **01 §8 (SessionManager).** `sessions_` becomes `shared_ptr`-owned and
  mutex-guarded; adds `sessionPtr`; `loadInto` is idempotent (24-D13).
  `closeSession`/`deleteSession` semantics are unchanged except that
  `deleteSession` publishes the committed terminal record. **Rev 4:** the
  `AgentLookup`/`setAgentLookup`/`findAgent`/`agent` surface (06 §4.1's thin
  delegate) is **deleted** and `session()` becomes private; `sessionPtr` is the
  only public session accessor (24-D17).
- **01 §9.5 (Close / detach / delete).** Detach is authoritative (24-D10);
  delete emits `SessionEnded` with its committed `Sequence` observable at the
  bus edge (I16 end-to-end).
- **01 I16.** Strengthened: "delete = end" includes transport-boundary
  observation even when the row is erased in the same transaction; detach emits
  no `SessionEnded`.
- **01 I18 (single in-process appender).** `appendEventLocked` publishes under
  `appendMutex_`; the committed channel preserves that ordering.
- **01 I20 (mailbox drain before teardown).** The committed publish drains the
  mailbox exactly as `publish` does.
- **02 §4.7 / `eraseWithEvent`.** Return type `void` → `Sequence`
  (source-breaking for the `SessionStore` seam; no wire/schema change).
- **04 §3.5 / H12 (graceful shutdown).** Step 3 becomes "finalize every
  in-flight turn and deny every pending permission"; the executor drain never
  detaches and may hard-exit; child teardown is explicit (24-D9).
- **05 §7.4 (`session.close`).** Detach semantics are authoritative;
  `HostRuntime::closeSession` no longer cancels/disposes (24-D10).
- **05 §8.4 (late-event-after-close / F3).** The delete stream carries the
  terminal event for the erased row; the tombstone sequence is unchanged.
- **05 T7 (durable-before-observable).** Preserved.
- **06 §3.4 / A14.** `dispose()` is a non-blocking park; it does not wait for
  the turn body. The join moves to daemon teardown. **Rev 3:** `dispose()` also
  never touches the `PermissionGate` decision hook (24-D16).
- **06 §4.1 / A15 (agent lookup).** **Rev 4:** `Agent& get(AgentId)` and
  `Agent* find(SessionId)` are **deleted**; `findShared`/`getShared` (returning
  `std::shared_ptr<AgentLoop>`) are the only accessors, and the
  `SessionManager::agent`/`findAgent` delegate is deleted with them and
  `SessionManager::session()` is made private. Any caller
  that uses an agent outside the registry's own `mutex_`-held section holds the
  strong reference for its whole use; no raw `Agent*` can escape any caller
  (24-D14/24-D17, `AL30`/`AL35`, proof §7.2.1). **Rev 5:** "any caller" is
  repo-wide — the live `src/cli/headless.cpp` path, the test-support helpers,
  and every test TU are enumerated (X1–X10, §7.2.2) and migrated; the dead
  `run_tui`/`TuiApp` path is removed rather than migrated (24-D19, `AL37`).
- **17 §1 / 19 §1 (dead `run_tui`).** No amendment: Rev 5 removes the path those
  clauses already classify as dead; their statements remain accurate as history.
  The removal is the remediation for the `TuiApp::agent_` raw member (24-D19,
  §7.2.3).
- **06 A1.** The single-driver invariant is enforced by `running_.exchange(true)`
  plus `control_mutex_` (AL15).
- **06 A-F12.** Dispose during a turn: cancel + flush + release lease; the turn
  body closes `TurnCancelled`; `whenIdle` fires when the body actually returns.
- **11 §3.2 (TurnExecutor sketch), E6, E7.** `submit(session, body)`;
  `drain` returns `DrainResult`; `inFlight(session)` added; per-session counters
  live in `State`.
- **11 §11.3 (shutdown coordinator).** Pins `drain` before
  `TransportServer::stop()`, matching the shipped order and 11 §11.3's preamble.
- **16 §2.4/§4.3 (ownership/teardown).** 24-D10: a non-last supervisor's exit is
  a detach; the daemon keeps the active turn running; the last exit finalizes
  and closes. Step ordering amended to "drain TurnExecutor, then
  `TransportServer::stop()`"; step 5's PTY+MCP child teardown is pinned before
  the transport stop and made explicit via `WorkspaceRuntime::shutdownChildren`,
  which reaches the manager through the new `WorkspaceRuntime::mcp()` accessor
  (24-D9). The last-exit close is `finalizeAll` → `SessionManager::closeSession`
  (24-D15, `AL33`).
- **16 §4.1/§4.4 (admission/ownership).** No behavior change; Rev 3 pins the
  existing invariant `AL32`: `ProtocolServer::admit_shutdown` and
  `WorkspaceHost::Impl::ownerWatchdogLoop` guarantee that a non-last supervisor
  exit never shuts the daemon down (the supervisor-side orphaning predicate is
  `SupervisorApp::compute_orphaning_set`, `src/ui/supervisor.cpp`, backed by
  `is_orphaning_view`, `src/ui/supervisor_presence.cpp`).
- **16 §5.1 (`kOwnerGraceTeardownBudget`).** Value unchanged; the TurnExecutor
  drain remains one of the three `shutdown_grace` terms; the watchdog `_Exit`
  remains the outer backstop and the coordinator's hard-exit the inner one.
- **22 §3/§4 (switcher/sessions).** No new UI. A detached-but-running session
  is on a live daemon, so the live-only Ctrl-S switcher surfaces it and
  `/sessions` lists it; selecting it attaches and `session.resume` returns the
  resident agent (idempotent). Pinned by AL26.
- **23 §5.4 (mid-turn guard).** The guard's first clause becomes
  `HostRuntime::hasPendingWork(id)` (agent OR executor queue); the blocked-state
  clause is unchanged. This closes the "known gap (not repaired here, RB-21)"
  note.

---

## 13. Out-of-scope, recorded risks

1. **`AgentLoop::run` is dead in production and is deleted.** If a session-cancel
   hook is ever needed, the daemon must install it with a `weak_ptr` control
   block; designing that hook is out of scope.
2. **`last_forwarded_` is never pruned on session delete.** Bounded by the
   number of sessions forwarded; a fresh UUID cannot collide. Pruning is
   deferred.
3. **A genuinely uninterruptible tool at shutdown** forces the bounded hard
   exit; the daemon does not attempt to kill the tool. Matches 16's "never
   SIGKILL a child" stance.
4. **`session.close` no longer closes the session's PTY.** The turn may keep
   using it while detached; the PTY is closed at delete/child teardown. This is
   a consequence of 24-D10, not a separate feature.
5. **Performance.** The committed channel adds one indirection per durable
   append and the map mutexes add contention; no batching change is proposed.
   Measure only if profiling shows a regression.
6. **Spec 23 interaction.** Last-exit finalize does not delete anything; spec
   23's unprompted cleanup still runs before teardown. A queued-but-not-yet-
   appended prompt is protected from a **delete** by AL7; on last-exit it is
   finalized by AL25. The precise ordering of spec 23 cleanup vs. `finalizeAll`
   remains spec 23's to own.
7. **Workspace-shared permission hook (Rev 3).** `WorkspaceRuntime` owns one
   `PermissionGate` shared by every `AgentLoop` in the workspace
   (`src/agent/workspace_runtime.cpp`), so two concurrent `executeToolCall`
   bodies can install/clear each other's `decision_hook_`. `AL34` makes this
   non-UB (the gate's `hook_mutex_`) and non-dangling (the weak `Session`
   capture), but a decision append can still be dropped or misrouted. Making
   the hook per-agent (e.g. passing it through `resolve`) would change the
   pinned `09` seam and is deliberately out of scope for this lifetime spec.
8. **The raw accessors are deleted; every caller is migrated repo-wide.**
   `AgentRegistry::get`/`find` and `SessionManager::findAgent`/`agent` are
   removed by 24-D17, and `SessionManager::session()` is made private. The live
   `src/cli/headless.cpp` path binds `getShared` to a named local; the
   test-support helpers (`createAgent`/`sessionOf`) return owning handles; the
   test TUs listed X5–X10 migrate mechanically.
   `tests/unit/agent_registry_test.cpp`'s `SessionManagerAgentDelegatesToRegistry`
   case asserts through the deleted delegate and is removed; its replacement
   (AL-U22/AL-U24) asserts the structural absence repo-wide instead. No
   production behavior is lost — the delegate had no production caller.
9. **`ui::run_tui`/`TuiApp` are dead and removed (24-D19).** Verified: no caller
   in `src/` (the include in `src/cli/cli.cpp` is unused; the live entry is
   `run_supervisor_entry` → `ui::run_supervisor`), only the missing-workspace
   unit case calls it, and specs 17/19/23 already classify it dead. Removing it
   deletes the `Agent& agent_` cached raw map-derived member and the two stale
   comment references to the path (`include/ymh/session/session.hpp`,
   `include/ymh/agent/workspace_runtime.hpp`; §7.2.3). If an in-process
   TUI is revived, it must use a named `getShared` local (AL37).

### 13.1 Rev 7 accepted risks (test-adequacy)

10. **Daemon-coordinator hard exit (AL11/AL-I5) is not exercised end to end.**
    The `TurnExecutor` half is tested by `TurnExecutor.AL_S1_...`
    (`_Exit(17)` on `TimedOut`), and the `drain`→`TimedOut` precondition by
    `TurnExecutor.AL_U7_...`. The `workspace_host.cpp` coordinator call site
    (`runtime_->shutdownChildren`/`_Exit` after `turns_->drain`) needs a
    blocking provider in a forked daemon plus an RPC-driven in-flight prompt;
    that harness does not exist and building it would duplicate the ownership
    crash-injection harness for one line. The call site is a two-line,
    straight-line consequence of `TimedOut`, verified by inspection.
11. **AL36 (`AgentRegistry::mutex_` is map-only) has no runnable probe.**
    `AgentRegistry` stores concrete `AgentLoop` (final) and `SessionManager`
    methods are non-virtual, so no test double can be injected to re-enter the
    registry from inside `dispose()`/`closeSession()` and detect a held lock.
    The rule is enforced structurally by §7.2/§3.3/§7.4 and the
    snapshot-release-erase body of `finalizeAll`/`dispose` (every registry
    method copies the `shared_ptr` under `mutex_` and releases before calling).
    `AL-U23` is therefore not implemented.
12. **AL-F26 (`SubagentRunner` raw-handle race vs `finalizeAll`) has no
    concurrent test.** The code was migrated to `getShared`/`sessionPtr`
    (`src/agent/subagent.cpp`), but the path is test-only and no test runs
    `SubagentRunner::run` concurrently with `finalizeAll`; `AL-U15` covers the
    registry-map half. The structural fix is the same as AL35 (no raw accessor
    exists).
13. **AL-F28 (registry lock held across a predicate/call) is covered only by
    the same structural argument as item 11.** No runtime probe can observe the
    lock scope; `AL-U23`/`AL-I17` are not implemented.
14. **`AL-U24`/`AL-U25` (test-support owning-handle sweep, headless concurrent
    teardown) are not implemented.** `AL-U22`'s repo-wide scan covers the
    temporary-extraction residual; the owning-handle migration itself is
    enforced by the type system (the raw accessors no longer exist, so the
    build fails by construction) and by `AgentLifetimeGuard`'s return-type
    scan.
15. **`AL-I1`/`AL-I8`/`AL-I14`/`AL-I17`/`AL-R1`/`AL-L1`/`AL-L2` are not
    implemented.** `AL-I10`/`AL-I15` cover the in-flight-finalize and close
    paths that `AL-I1` targeted; `AL-I17`'s TSan sweep is approximated by
    `AL-U15` plus the TSan CI job; the live IDs are opt-in and were not
    authored. See §11.8.
16. **`AL-U6` no longer claims a `joinable()` assertion.** `TurnExecutor`
    exposes no worker-handle accessor, so "no detach" cannot be asserted
    directly; the test asserts `drain` returns `Quiesced` and the destructor is
    safe (it hard-exits only when not quiesced), which is the observable
    consequence of joining. The Rev 6 table text was corrected.
17. **The full test suite is not run under sanitizers.** The sanitizer CI jobs
    run the §11.1/§11.2 lifetime set (34 tests, green under both ASan and
    TSan). A full-suite TSan run reports pre-existing noise in suites unrelated
    to RB-21 — `GlobalTestEnv`, `McpIntegrationTest`, `OwnershipTwoSupervisor`,
    `SessionPruneTest`, `UiSupervisorPty` (fork/PTY/two-process tests) — that
    predates this spec and is out of scope here. The lifetime tests themselves
    are TSan-clean; `MemorySessionStore`/`FakeStore` were made thread-safe to
    remove the one harness race the lifetime tests exposed.

---

## 14. OPEN QUESTIONS — RESOLVED (Rev 2)

All three Rev 1 questions were answered by the user (2026-09-18). They are kept
here as a resolution record; none remains open.

1. **`session.close` on a running turn: cancel (shipped) or keep running
   (05 §7.4 detach)?** **RESOLVED — detach wins; the shipped cancel is the
   bug.** User verbatim:

   > "When user exits from last supervisors active sessions should be saved into
   > history (if not yet) and closed. If user exit one supervisor while another
   > supervisor is still running the active session continues to run in
   > detached mode. It can then be controlled from the running instance of
   > supervisor"

   Designed as **24-D10** (§2.2.1): detach is non-destructive and daemon-owned;
   the last supervisor's exit finalizes (flush + terminal event) and closes the
   active sessions; a detached session is discoverable/controllable from a
   still-running supervisor. `HostRuntime::closeSession` becomes detach-only.
   **Rev 3 reconciles the verbatim text with the coordinator sequence:** "saved
   into history (if not yet)" is `finalizeAll`'s cancel → flush → exactly one
   terminal event; "and closed" is the explicit
   `SessionManager::closeSession(id)` in the same `finalizeAll` (24-D15/`AL33`),
   with process exit as the final free. "continues to run in detached mode" is
   the non-last exit: no `finalizeAll`, the daemon survives because
   `admit_shutdown` refuses the shutdown while another owner exists (`AL32`).

2. **On shutdown grace expiry, hard `std::_Exit` or block indefinitely?**
   **RESOLVED — hard exit** (the recommended default). Bounded, memory-safe,
   consistent with 16 §5.1; stale registry rows are handled by 16's
   `reapIfStale`.

3. **Notify the client when a close/delete drops an acked-but-unstarted
   prompt?** **RESOLVED — no new notification** (the recommended default). A
   delete now refuses while a body is pending (AL7); on last-exit a queued body
   is finalized (AL25). The wire surface does not change.

---

## 15. Revision log

- **Rev 1 (2026-09-18).** Initial write. Repaired RB-21 defects 1–4 with
  reference-counted `Session`/`AgentLoop` ownership, park-on-close /
  join-on-shutdown / bounded hard-exit teardown, a queue-aware pending
  predicate, and a committed-record forwarding channel. Added `AL1`–`AL22`,
  `AL-F1`–`AL-F14`, `24-D1`–`24-D9`.

- **Rev 2 (2026-09-18).** Incorporated the three user decisions and closed the
  Oracle gate (1 HIGH, 9 MEDIUM, 5 LOW).
  - **Part A — detach (24-D10).** Rewrote §2.2 into three operations
    (detach/close/shutdown); added §2.2.1 and decisions 24-D10 (detach),
    24-D13 (idempotent `loadInto`); rewrote `HostRuntime::closeSession` as
    detach-only; added `AgentRegistry::finalizeAll`; amended 05 §7.4, 16
    §2.4/§4.3, 22 §3/§4, 23 §5.4; resolved §14.
  - **HIGH-1 — map races.** Added §7.2 (map-synchronization contract),
    `mutex_` on `AgentRegistry`/`SessionManager`, `AL27`, `AL-F18`, tests
    AL-U15. Chose mutex + total lock order over io-marshalling; justified.
  - **MEDIUM-2 — `finalizeAll` missing from the sketch.** Added it to §3.3 with
    the erase-while-iterating contract.
  - **MEDIUM-3 — `findShared` return type.** Now returns
    `std::shared_ptr<AgentLoop>` (activate/suspend/requestCompaction live on
    `AgentLoop`); §3.3 updated.
  - **MEDIUM-4 — committed unsubscribe.** Pinned `Subscription::Channel` +
    committed list + `EventBusState::unsubscribe` dispatch (24-D12, AL29,
    AL-F20, AL-U16/AL-I13).
  - **MEDIUM-5 — `AgentLoop` sketch wrong.** §3.2 now matches the shipped
    surface: `activate()`/`suspend()`/`requestCompaction()` public, no invalid
    `run(...) override`; `run` is deleted.
  - **MEDIUM-6 — missing capture C11.** Added `set_decision_hook` capture to
    §7.3 and AL14; pinned a scope-guard/clear fix.
  - **MEDIUM-7 — C7 remediation unpinned.** Pinned deletion of the dead
    `AgentLoop::run` (verified no caller) as the single remediation.
  - **MEDIUM-8 — `TimedOut` test seam.** Added §4.2 (in-process latch re-drain
    + subprocess hard-exit) and AL-S1; pinned `drain` idempotency/re-drain.
  - **MEDIUM-9 — MCP/PTY ordering.** Added `WorkspaceRuntime::shutdownChildren`
    at coordinator step 6, amended 16 §4.3 step 5, added AL28/AL-F19/AL-I12.
  - **LOW-10 — duplicated counter.** Per-session counters now live only in
    `TurnExecutor::State`.
  - **LOW-11 — lock order backwards.** §7.4 now states `appendMutex_` →
    `forward_mutex_` and pins a total order.
  - **LOW-12 — "additively".** §2.4/§12 now call the `eraseWithEvent` signature
    change **source-breaking** (no wire/schema change).
  - **LOW-13 — misleading coverage.** AL-F13 now maps to AL-I9 (detach), not
    AL-I2; AL-F14 stays AL-I2; added AL-I9/AL-I10/AL-I11/AL-I13.
  - Extended `AL23`–`AL29`, `AL-F15`–`AL-F21`, `24-D10`–`24-D13`, and the test
    plan/coverage matrices accordingly.

- **Rev 3 (2026-09-18).** Closes the second independent adversarial gate
  (1 HIGH, 3 MEDIUM, 2 LOW) and pins the two items the gate could not verify.
  - **HIGH-1 — the mutex protects the maps, not the raw pointers they return.**
    Chose and pinned **option (a): a strong reference for every io-handler use**
    (24-D14). `findShared`/`getShared` copy the `shared_ptr` under `mutex_`;
    every io-thread `HostRuntime` handler holds it for its whole body, and
    session-side handlers hold `sessionPtr`, so `finalizeAll`'s erase drops only
    the registry's/manager's reference and cannot dangle a dispatched handler.
    Added `AL30`/`AL31`, `AL-F22`, `AL-U19`, `AL-I14`, the lock-order entry for
    the new accessors, and the **exhaustive io-thread raw-pointer inventory**
    (§7.2.1: agent sites H1–H8, session sites S1–S4, worker bodies W1–W6).
    Options (b) park-only + handler-completion quiescence and (c)
    marshal-`finalizeAll`-to-io-and-join are rejected with reasons (no handler
    barrier exists; `translate` does not marshal; `waitForDrain` counts bytes;
    joining the io thread can deadlock).
  - **MEDIUM-2 — `mcp()` does not exist.** Pinned
    `WorkspaceRuntime::mcp() noexcept -> McpManager&` (mirrors `gate()`), noted
    the private `std::unique_ptr<McpManager> mcp_` and the existing
    `McpManager::shutdown(ms) -> Task<void>` seam, and aligned the 16 §4.3
    step-5 amendment (24-D9).
  - **MEDIUM-3 — "closes the resident sessions" never happens.** Pinned the
    last-exit close explicitly: `AgentRegistry::finalizeAll` calls
    `SessionManager::closeSession(id)` after parking (24-D15, `AL33`,
    `AL-F23`, `AL-I15`), and reconciled it line-by-line with the user's verbatim
    requirement (§14 item 1).
  - **MEDIUM-4 — C11 remediation unpinned and unsynchronized.** Pinned **one**
    mechanism: a `std::weak_ptr<Session>` capture plus a gate-level leaf
    `hook_mutex_` (snapshot-then-invoke in `emit_decision`); `dispose()` no
    longer clears the hook, removing the Rev 2 race (24-D16, `AL34`, `AL-F24`,
    `AL-U20`/`AL-U21`). Corrected the stale symbol (`emit_decision`, not
    `notify`).
  - **LOW-5 — "`runtime_` is the first-declared `Impl` member" is inaccurate.**
    Corrected to cite the real declaration order
    (`src/host/workspace_host.cpp`: `config_` first, `runtime_` at 353, before
    `transport_`/`protocol_`); the conclusion is unchanged.
  - **LOW-6 — defect 0 framing.** Reframed: a non-last supervisor exit never
    calls `session.close` (it is a connection drop + admission refusal); the
    detach behavior is already correct via spec 16. Only the explicit
    `session.close` RPC is the bug.
  - **Gate-unverified item A — owner/admission logic.** Pinned `AL32` and cited
    `ProtocolServer::admit_shutdown`, `WorkspaceHost::Impl::ownerWatchdogLoop` /
    `Impl::requestShutdown`, `SupervisorApp::confirm_exit` /
    `compute_orphaning_set` / `is_orphaning_view`, and `ProtocolServer::drop_client`;
    added `AL-F25`/`AL-I16`.
  - **Gate-unverified item B — io-handler inventory.** Added §7.2.1 with every
    io-thread raw-`Agent*`/`Agent&`/`Session&` site (H1–H8, S1–S4) and the
    worker bodies (W1–W6), with its Rev 3 coverage.
  - Extended `AL30`–`AL34`, `AL-F22`–`AL-F25`, `24-D14`–`24-D16`, the test plan,
    the lock order, and the coverage matrices. Header status stays
    `written · verified: —` for the independent re-gate.

- **Rev 4 (2026-09-18).** Closes the third independent adversarial gate
  (1 HIGH, 2 MEDIUM, 1 LOW). The HIGH had already failed twice for the same
  reason; Rev 4 fixes it **structurally** rather than by extending the
  enumeration.
  - **HIGH — the enumeration never covered the registry paths, and enumeration
    is the wrong proof.** The shipped `AgentRegistry::activateSession`/
    `suspendSession`/`requestCompaction`/`hasPendingWork`
    (`src/agent/agent_registry.cpp`) each do `agents_.find`/`bySession_.find`
    and then call a method on the raw `agent->second`; `HostRuntime::
    suspendSession` (`src/host/host_runtime.cpp`) invokes `suspendSession`
    **directly on the io thread**, and a coordinator-thread `finalizeAll` can
    erase/free that agent concurrently. Fixed by deleting the raw accessors
    `Agent& get`/`Agent* find` and the `SessionManager::AgentLookup`/
    `setAgentLookup`/`findAgent`/`agent` delegate (24-D17); every caller —
    handlers, worker bodies, registry methods, `SubagentRunner` — must hold
    `findShared`/`getShared`/`sessionPtr` for its whole use (AL30/AL35). Added
    §7.2.1, a structural proof by case analysis that no accessor, stored
    closure, or cached member can yield a raw map-derived handle; §7.2.2 is
    retained only as a *verification checklist* (now including R1–R6 and the
    corrected W1–W6). Added AL-F27, AL-U22, AL-I17 (TSan: every registry path
    races `finalizeAll`).
  - **MEDIUM — `§7.3 C8` (`SubagentRunner`).** It held a raw
    `Agent& child = registry_.get(*created)` across `child.send()`/
    `whenIdle()`/`session()` and a raw `Session& childSession`. Fixed: hold
    `std::shared_ptr<AgentLoop> child = registry_.getShared(*created)` and
    `std::shared_ptr<Session> childSession = sessions_.sessionPtr(...)` for the
    whole `run` (AL2/AL35); noted the path is **test-only** (only
    `tests/unit/agent_registry_test.cpp` constructs a `SubagentRunner`). Added
    AL-F26.
  - **MEDIUM — three inconsistent lock-scope statements.** §7.2 allowed only
    map ops + `dispose()`/`closeSession()` under `mutex_`; §3.3/H7 said
    `hasPendingWork`'s predicate completes under `mutex_`; §7.4 said
    `finalizeAll` holds lock 1 across locks 2/3. Fixed by stating **one rule**
    (24-D18, AL36): `AgentRegistry::mutex_` is a map-only lock, and
    `finalizeAll` snapshots under the lock, releases, acts, then re-locks to
    erase. §3.3 and §7.4 now match §7.2; §7.4's edge list drops the held
    1→2/1→3 edges. Added AL-F28, AL-U23.
  - **LOW — `W1–W6` table.** Corrected: the shipped `activateSession`/
    `compactSession` bodies call `runtime_.agents().activateSession(id)`/
    `requestCompaction(id)` (`src/host/host_runtime.cpp`), not `find(id)`. The
    worker checklist now splits W1–W2 (registry-method bodies) from W3–W6
    (`findShared` bodies).
  - Kept intact: 24-D14/D15/D16, AL30–AL34, AL-F22–AL-F25 (the detach
    save-and-close decision, the permission-hook mechanism, the owner/admission
    invariant). Header status stays `written · verified: —`.

- **Rev 5 (2026-09-18).** Closes the fourth independent adversarial gate
  (1 HIGH, 2 LOW). The HIGH had failed twice because the check was scoped.
  - **HIGH — the "no cached raw member" claim was false, and the check was
    scoped to `src/host/`.** Rev 4 §7.2.1 point 6 asserted no cached raw member
    points at a map-owned agent/session, but the shipped `TuiApp::agent_`
    (`Agent& agent_;`, `src/ui/ui_application.cpp`) was bound from
    `AgentRegistry::get(*created)`. Rev 4 never enumerated its callers and
    `AL-U22` was scoped to `src/host/`, so the check could not catch it. Fixed
    in three parts: (1) every caller is pinned — `src/cli/headless.cpp` (**live**)
    binds `std::shared_ptr<AgentLoop> agent = registry.getShared(agent_id);` to a
    **named local held for the whole function**; `tests/support/agent_test_env.hpp`
    `createAgent()`/`sessionOf()` return owning handles; the test TUs
    `skills_wiring_test.cpp`, `agent_registry_test.cpp`,
    `workspace_runtime_test.cpp`, `agent_loop_test.cpp`, `agent_inbox_test.cpp`,
    `compaction_test.cpp`, `host_runtime_test.cpp`, `ui_driver_test.cpp` migrate
    mechanically (X1–X9, §7.2.2). (2) The `run_tui`/`TuiApp` path is **removed**,
    verified dead (no caller in `src/`; the include in `src/cli/cli.cpp` is
    unused; the live entry is `run_supervisor_entry` → `ui::run_supervisor`,
    `src/ui/supervisor.cpp`; only `ui_model_test.cpp:278` calls it; specs
    17 §1/19 §1 already classify it dead) — the raw member ceases to exist rather
    than being converted (§7.2.3, §13 item 9). (3) `AL-U22`/§7.2.2 are **widened
    to the whole tree** (`src/`, `tests/`, `include/`) and the safety argument is
    structural: the raw accessors no longer exist, so no caller anywhere can
    obtain a raw map-derived agent/session handle; a scoped check is explicitly
    prohibited. Added `24-D19`, `AL37`, `AL-F29`, `AL-U24`, `AL-U25`; rewrote
    §7.2.1 points 6–7; corrected the false claim.
  - **LOW — §7.4 "map-only" was imprecise for lock 2.** Qualified "map-only" to
    lock 1 (`AgentRegistry::mutex_`) only; lock 2 (`SessionManager::mutex_`) is
    held across `deleteSession`'s `eraseWithEvent` + `publishCommitted` (the
    pinned 2→5 edge), matching §7.2.
  - **LOW — §7.2.2 H2 was incomplete.** `HostRuntime::closeSession` also calls
    `pty().closeSession(id)` and `sessions().closeSession(id)`
    (`src/host/host_runtime.cpp`); H2 now states that all three are removed,
    matching §2.2 and §13 item 4.
  - Re-verified every citation against the shipped tree; no citation in this
    revision relies on a stale line number. No schema, wire, dependency, or
    path-safety change. Header status stays `written · verified: —`.

- **Rev 6 (2026-09-18).** Closes the fifth independent adversarial gate
  (2 MEDIUM, 2 LOW, 0 HIGH). The gate **confirmed the structural argument now
  holds** — after the Rev 5 deletions, no code in `src/`, `tests/`, or
  `include/` can obtain a raw map-derived `Agent*`/`Agent&`/`Session*`/
  `Session&`. The four findings are documentation-consistency and enumeration
  defects; the structural design (the deletions, `findShared`/`getShared`/
  `sessionPtr`, the repo-wide `AL-U22`/§7.2.2, and 24-D17/24-D18/24-D19) is
  unchanged.
  - **MEDIUM-1 — `SessionManager::session` self-contradiction.** Rev 5 listed
    `session` as a DELETED accessor in §7.2.1 point 7 and `AL37`, but §3.1 pins
    it **private** (retained), `AL30` omitted it, `AL35`/§7.2.2/`AL-U22` called
    `sessionPtr` the only public accessor, and `AL-U22` correctly said "no
    *public* session lookup". Fixed everywhere: the deleted set is the **public**
    accessors only (`AgentRegistry::get`/`find`;
    `SessionManager::AgentLookup`/`setAgentLookup`/`findAgent`/`agent`), and
    `session()` is described uniformly as **private / not externally nameable**
    in §7.2.1 point 7, `AL30`, `AL35`, `AL37`, the 24-D17 row, §7.2.2, §12, and
    §13 item 8. Verified against `include/ymh/session/session_manager.hpp`
    (`Session& session(const SessionId&)` at `:68`, public in the shipped tree,
    pinned private here).
  - **MEDIUM-2 — the X1–X9 exhaustiveness claim was false.** `grep` over the
    shipped tree found `tests/unit/host_runtime_test.cpp` has **14** raw
    `sessions().session(...)` `Session&` sites (`:269`, `:280`, `:566`, `:865`,
    `:959`, `:1010`, `:1017`, `:1021`, `:1026`, `:1029`, `:1049`, `:1053`,
    `:1080`, `:1081`), not the two Rev 5 cited; `agent_registry_test.cpp` has a
    **second** raw source (`DurableAgentEnv::createAgent()`/`sessionOf()` at
    `:135`–`:143`) plus `env.sessionOf(parent)` into `SubagentRunner` (`:275`,
    `:301`, `:337`); `session_test.cpp` has **11** `manager.session(...)` sites
    (`:462`, `:470`, `:501`, `:691`, `:694`, `:721`, `:742`, `:746`, `:749`,
    `:774`, `:777`); and the `Agent* agentPtr = &agent;` aliases were unlisted
    (`agent_inbox_test.cpp:143`/`:173`/`:219`/`:245`/`:279`;
    `agent_loop_test.cpp:196`). Rewrote X5–X8, added **X10**
    (`session_test.cpp`), noted the one production non-host site
    (`src/agent/subagent.cpp:27`/`:45`, enumerated as `C8`), and re-verified the
    whole list by grep. The list is still a *verification aid*, not the safety
    argument (§7.2.2).
  - **LOW-3 — §7.2.1 point 6 false clause.** It claimed "None is obtained from a
    registry/manager accessor", but `SubagentRunner::parent_` derives from
    `sessionOf` → `SessionManager::session()` in the shipped tree
    (`tests/support/agent_test_env.hpp:117`, `tests/unit/agent_registry_test.cpp:143`).
    Fixed: point 6 now states `parent_` is a `Session&` bound from an **owning**
    `std::shared_ptr<Session>` resolved via `sessionPtr` (`C8`) and anchored by
    the spawning parent agent's `session_owner_` (`AL1`); none of the cached raw
    members is a bare map-derived handle.
  - **LOW-4 — stale `run_tui` comment.** `include/ymh/session/session.hpp`
    (`:81`–`:82`, the `is_placeholder_title` doc comment) still cites the dead
    `run_tui`/`ui_application.cpp`; so does
    `include/ymh/agent/workspace_runtime.hpp:18`. §7.2.3's removal list (and
    §13 item 9) now includes both comment-only edits; `is_placeholder_title`
    behavior is unchanged (legacy `"main"` placeholders remain valid).
  - **Gate-unverified item — mechanize the repo-wide check.** Rev 5's
    `AL-U22`/`AL37` "static/compile check" was not mechanized. Pinned **two**
    mechanisms and no third (§7.2.1 point 7, `AL37`, `AL-U22`): (1) **by
    construction**, naming a deleted accessor or the private `session()` is a
    hard compile error in any TU under `src/`, `tests/`, or `include/` (the
    build fails — true by construction); (2) **`AL-U22` is a runnable CTest**
    (`tests/unit/no_raw_map_handle_test.cpp`) that walks the three roots and
    fails on a deleted-accessor name or a raw `.get()`/`*` extraction applied
    **directly to a call** of `findShared`/`getShared`/`sessionPtr` (the
    temporary-handle pattern; extraction from a named owning local stays legal,
    matching X8), covering the residual the type system cannot see. The claim
    now matches the mechanism; the scan is explicitly a lexical guard, with the
    type system plus §7.2.1 as the completeness argument.
  - Re-verified every citation in this revision against the shipped tree
    (symbols authoritative; line numbers re-derived). No structural, wire,
    schema, dependency, or path-safety change. Header status stays
    `written · verified: —` (not self-marked verified).

- **Rev 7 (2026-09-18).** Closes the independent test-adequacy review of the
  RB-21 work (1 HIGH test-plan falsehood, 1 HIGH missing memory-safety CI, 1
  MEDIUM vacuous `AL-U20`, 1 MEDIUM weak guard, 3 LOW).
  - **HIGH-1 — the Rev 6 coverage tables were largely false.** ~24 of ~40 cited
    test IDs did not exist. Rewrote §11.1–§11.7 to name **only real tests** by
    their `Suite.TestName`; moved the unimplemented IDs to §11.8; recorded each
    residual gap as an accepted risk in §13.1. Implemented the named gaps:
    - `SessionManager.AL_U1_...`, `AL_U18_...` (resident-session identity,
      `loadInto` — previously untested).
    - `AgentRegistry.AL_U15_...` (concurrent create/dispose/reads; TSan).
    - `AgentLoop.AL_U3_...` (paused-body quiescence), `AL_U14_...`
      (`control_mutex_` not held across `stream`), `AL_U20_...` (real capture).
    - `TurnExecutor.AL_S1_...` (forked hard-exit, `HostExitCode::Internal`).
    - `WorkspaceHostTeardown.AL_I12_...` (child-teardown-before-transport-stop
      source-order guard).
  - **HIGH-2 — memory-safety rule unmet.** Added `YMH_ASAN` (mutually exclusive
    with `YMH_TSAN`) and `.github/workflows/ci.yml` with `build-test`, `asan`,
    and `tsan` jobs. Sanitizer builds use `-O0` to avoid GCC 16
    optimization-dependent `-Wmaybe-uninitialized` false positives in libstdc++
    `std::regex` (`src/execution/filesystem.cpp`) and nlohmann `json::items()`
    (`src/mcp/schema_translation.cpp`); warnings remain errors and no warning
    is suppressed. Made the test stores `MemorySessionStore`/`FakeStore`
    thread-safe so the TSan job is clean.
  - **MEDIUM-3 — `AL-U20` was vacuous.** It installed its own hook, so
    reverting `agent_loop.cpp` to `[this]` did not fail it. Added
    `PermissionGate::decision_hook_for_test()` (a documented test seam) and
    rewrote the test to snapshot and invoke the **real** production capture
    after teardown. Proven non-vacuous: the `[this]` revert is an ASan
    `heap-use-after-free` at `src/agent/agent_loop.cpp:455`.
  - **MEDIUM-4 — the repo-wide guard was too weak.** `AL-U22` now also flags
    any function returning a raw `Agent`/`AgentLoop`/`Session` handle
    (allowlisting only the pinned private/borrowed declarations), `.get()` on a
    line naming an accessor (the cast form), and a raw handle extracted from a
    named owning local then returned or stored into a member. It remains a
    lexical guard; the precise residual (dynamic escapes through wrappers whose
    return type is not a raw handle) is stated in the test header and §13.1.
  - **LOW-5 — `AL-U6` false `joinable()` claim** corrected (see §13.1 item 16).
  - **LOW-6 — `AL-I10` terminal count** now asserts exactly one `TurnCancelled`
    and zero `TurnEnded`, so a natural completion fails.
  - **LOW-7 — `AL-I15` earlier-turn count** now captures the terminal count
    before `finalizeAll` and asserts it is unchanged afterwards, instead of
    re-reading a `TurnEnded` produced by the earlier turn.
  - Non-vacuity evidence (revert-then-fail): `AL_U18` (loadInto replace),
    `AL_S1` (no `_Exit`), `AL_U3` (session-blind predicate), `AL_U20`
    (`[weak]`→`[this]`, ASan), `AL_U15` (no registry lock, TSan), and the
    strengthened guard (probe file with a raw accessor, a wrapper, a
    cast-`.get()`, and a member escape).
  - No wire, schema, dependency, or path-safety change. Header status stays
    `written · verified: —` (not self-marked verified).
