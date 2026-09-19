# 27 — Session Locking Errata: The F3 Locking Contract (Session read accessors + `PlanModeController::erase`)

```
Status: written · verified: — · reviewer: —
Revision: Rev 4 — closes the `header()` half of follow-up 27-R1, the latent
          unlocked-reference API hazard recorded by Rev 3 §5.3. Minimal
          resolution chosen (option a): **27-D9** makes `Session::header()`
          return a by-value `SessionHeader` copy taken under `appendMutex_`,
          exactly like the four accessors pinned by 27-I1, and drops `noexcept`
          (a copying accessor that can allocate cannot be `noexcept`). No caller
          bodies change: every existing call site already consumes the result by
          value (a field read, or a `const SessionHeader&` parameter), so the
          blast radius is the return type plus the one body; `header()` now
          joins the §3.3/27-I2 no-re-entry set. `id()`/`kind()` stay safe
          (immutable fields); `nextTurnId()`/`nextStepId()` remain the narrowed
          27-R1 residual (a different unlocked-scalar-read defect class, out of
          scope here). Rev 2 pinned the no-re-entrancy verification artifact as
          an **audit-only** contract (§3.3, §7.1) and the `-DYMH_TSAN=ON`
          invocation 27-T1 requires. Rev 3 corrected the pinned consequence of a
          27-I2 violation so each trigger class names the mutex it actually
          self-deadlocks on. No new decision, no wire/schema/protocol change,
          and no behavior change beyond removing the hazard.
Component: 27 (errata) — amends 01-session.md, 25-ui-ux-errata.md
Depends on: 01-session.md (verified), 02-persistence.md (verified),
            25-ui-ux-errata.md (draft Rev 8; the F3 row it records is closed
            here)
Scope: (1) Pin that `Session::ownEvents()`, `Session::deriveMessages()`, and
       `Session::snapshot()` copy `header_`/`log_` under `appendMutex_`, exactly
       like `Session::events()`, and reconcile that with 01 I7/I18/I21 and §11.
       (2) Pin the controller lock order
       `commit_mutex_ → mutex_`, `commit_mutex_ → appendMutex_`, and
       `commit_mutex_ → SessionManager::mutex_`, never `mutex_` with
       `appendMutex_`, and amend the stale sentence in 25 §N12 that says
       `erase` takes `mutex_` alone.
       (3) Pin the load-bearing
       no-re-entrancy invariant for every handler `publishCommitted` invokes
       inline while `appendMutex_` is held, for the injected `ProjectionFn`,
       and for the injected `AppendFn`; pin its verification as an audit-only
       artifact (§7.1). (4) Record the `reload()` construction-only assumption
       and the `erase() noexcept` leaf requirement. (5) Close the `header()`
       part of the residual: `Session::header()` now returns a locked by-value
       copy (27-D9), and record `id()`/`kind()` as safe and
       `nextTurnId()`/`nextStepId()` as the narrowed residual.
```

This document is a **pin of shipped code**, not a proposal. Every claim below was
verified against the working tree (uncommitted fix) before it was written; the
code anchors are line-numbered to that tree. It changes no decision in `01`
beyond making explicit what the read accessors already do, and it closes the
`F3` follow-up in `25` §14.1.

---

## 1. Purpose, scope, and supersession map

### 1.1 The problem

The uncommitted fix (from the spec-25 review, finding **F3**) makes the three
session read accessors that previously read `log_`/`header_` without
synchronization take `appendMutex_`, and makes `PlanModeController::erase()`
take `commit_mutex_` before `mutex_`. `25-ui-ux-errata.md` §14.1 records the
pre-fix state as a deferred gap; §N12 pins the controller lock order but still
says "`request_exit` and `erase` take `mutex_` alone". The code is ahead of the
spec. This errata catches the spec up.

The defect class was:

- (a) `PlanModeController::erase()` took only `mutex_`, so a `deleteSession`
  could interleave with an in-flight `commit_locked_`
  (`invalidate → append → record`) and resurrect a `memo_` entry for a deleted
  id.
- (b) `Session::ownEvents()` / `deriveMessages()` read `log_` without
  `appendMutex_` while `events()` locked it; `snapshot()` composed several
  unlocked reads. A concurrent appender produced torn reads (a data race).

### 1.2 What this changes, in one sentence

`ownEvents()`, `deriveMessages()`, and `snapshot()` now acquire `appendMutex_`
for the whole read and return a value copy; `snapshot()` calls the **free**
`ymh::deriveMessages(header_, log_)` (never the now-locking member) to avoid a
recursive self-deadlock; and `erase()` acquires `commit_mutex_` before
`mutex_` under the already-pinned controller lock order.

### 1.3 Supersession map

#### 1.3.1 Superseded / withdrawn

| In | Text | Disposition |
|---|---|---|
| `25-ui-ux-errata.md` §14.1 row `F3` | "Pre-existing plan-mode data races … recorded; out of scope for Rev 6" | **Closed by this errata.** The two sub-defects (a) and (b) are fixed and pinned below. The spec-25 document should be updated to point at this errata; this document does not edit it (one-file rule). |
| `25-ui-ux-errata.md` §N12 | "`request_exit` and `erase` take `mutex_` alone" | **Superseded for `erase` only.** `erase()` now takes `commit_mutex_` **then** `mutex_` (§3.2). `request_exit` still takes `mutex_` alone. |
| `01-session.md` §11 bullet | "Reads (`events`, `deriveMessages`) are taken over an immutable snapshot of the in-memory log" | **Extended** to all five read accessors (§4.3; `header()` added by 27-D9). Not a reversal. |

#### 1.3.2 Amended

| In | Amendment |
|---|---|
| `01-session.md` §6 read-side comments (`:641-652`) | The comments on `events`/`ownEvents`/`deriveMessages` gain the `appendMutex_` acquisition and the committed-handler re-entrancy restriction (§3.1). |
| `01-session.md` I7 (`:1121-1123`) | The **free function** stays pure; the **member** acquires `appendMutex_` before delegating (§4.1). |
| `01-session.md` I18 (`:1172-1173`) | The single-appender serialization now covers every read accessor, not just `append` vs `events` (§4.3). |
| `01-session.md` I21 (`:1182-1184`) | Snapshot equivalence unchanged in force; `snapshot()` now derives under the same lock, so `at`/`eventCount`/`messages` observe one coherent log state (§4.2). |
| `01-session.md` §11 (`:1072-1088`) | The re-entrancy restriction is generalized from `append` to all five read accessors (`header()` added by 27-D9), every handler invoked by `publishCommitted`, the `ProjectionFn`, and the `AppendFn` (§3.3). |

#### 1.3.3 Retained (explicitly not changed)

- `appendEvent` / `appendEventLocked` / `appendAutoRename` / `appendBatch`
  serialization semantics (I18) — unchanged.
- The controller's `commit_mutex_` purpose (whole-commit serialization + H1
  TOCTOU compare-and-append atomicity, `25` §N12) — unchanged.
- `PlanModeController::set` / `apply_pending_at_step_start` /
  `flush_pending_at_turn_end` / `request_exit` bodies — unchanged.
- `Session::events()` — already locked before this fix; unchanged.
- `emit()` (live-only publish) — never takes `appendMutex_`; unchanged. But note
  that a handler registered by `subscribe()` is **not** live-only: it is also
  invoked by `publishCommitted` (see §3.3.1), so it is subject to the invariant
  in §3.3.

### 1.4 Scope boundaries

- **No wire, schema, protocol, dependency, or path-safety change.**
- **No behavior change** other than closing the data race and the memo
  resurrection window: lock acquisition adds no observable event, no ordering
  change to the durable log, and no new error surface.
- **Not in scope:** the remaining `noexcept` field accessors `id()`/`kind()`
  (immutable fields; safe) and `nextTurnId()`/`nextStepId()` (unlocked reads of
  the scalar counters, a different defect class), recorded under the narrowed
  follow-up ID **27-R1** in §5.3. `header()` is now in scope and closed by
  27-D9 (§3.1).

---

## 2. Amendment register

| ID | Change | Code anchor |
|---|---|---|
| 27-D1 | `ownEvents()`, `deriveMessages()`, `snapshot()` copy under `appendMutex_` | `src/session/session.cpp:542-557`, `:660-669` |
| 27-D2 | `snapshot()` calls the free `ymh::deriveMessages(header_, log_)`, not the member | `src/session/session.cpp:666` |
| 27-D3 | `PlanModeController::erase()` takes `commit_mutex_` then `mutex_` | `src/agent/plan_mode_controller.cpp:127-136` |
| 27-D4 | Load-bearing no-re-entrancy invariant (handlers + `ProjectionFn` + `AppendFn`); audit-only verification (§7.1) | §3.3 |
| 27-D5 | `reload()` is construction-only; its lock-free `log_` write is safe only under that assumption | `src/session/session.cpp:512-535`, `include/ymh/session/session.hpp:300` |
| 27-D6 | `erase()` stays a leaf (`noexcept` requires it) | §3.5 |
| 27-D7 | Residual (narrowed): `id()`/`kind()` safe; `nextTurnId()`/`nextStepId()` unlocked scalar reads remain (follow-up **27-R1**) | §5.3 |
| 27-D8 | Verification artifact for 27-I2 pinned as audit-only (audited sites, auditor, cadence, form) | §3.3, §7.1 |
| 27-D9 | `Session::header()` returns a by-value `SessionHeader` copy taken under `appendMutex_`; `noexcept` dropped; `header()` joins the 27-I2 no-re-entry set | `include/ymh/session/session.hpp:233`, `src/session/session.cpp:712-715` |

---

## 3. The locking contract (pinned)

### 3.1 `Session` read accessors

`appendMutex_` is a **non-recursive** `std::mutex` (`include/ymh/session/session.hpp:315`),
`mutable` so the `const` accessors can take it. It guards
`log_` / `header_` / `nextTurn_` / `nextStep_` mutations.

Pinned signatures (`include/ymh/session/session.hpp`):

```cpp
[[nodiscard]] SessionHeader header() const;               // :233 (27-D9)
[[nodiscard]] EventRange events() const;                  // :244
[[nodiscard]] EventRange ownEvents() const;               // :249
[[nodiscard]] std::vector<Message> deriveMessages() const; // :254
[[nodiscard]] SessionSnapshot snapshot() const;           // :289
```

Pinned bodies (`src/session/session.cpp`):

```cpp
SessionHeader Session::header() const {                   // :712
    std::lock_guard<std::mutex> lock(appendMutex_);       // :713
    return header_;                                       // :714
}

EventRange Session::events() const {
    std::lock_guard<std::mutex> lock(appendMutex_);   // :538
    return log_;                                       // :539
}

EventRange Session::ownEvents() const {
    std::lock_guard<std::mutex> lock(appendMutex_);   // :543
    // fork-prefix slice (:544-551) or full log_ (:551)
}

std::vector<Message> Session::deriveMessages() const {
    std::lock_guard<std::mutex> lock(appendMutex_);   // :555
    return ymh::deriveMessages(header_, log_);        // :556 (free function)
}

SessionSnapshot Session::snapshot() const {
    std::lock_guard<std::mutex> lock(appendMutex_);   // :661
    // reads header_, log_.back(), log_.size()          (:663-667)
    snapshot.messages = ymh::deriveMessages(header_, log_);  // :666 (free function)
    // ...
}
```

**Contract.** All five accessors acquire `appendMutex_` for the whole read and
return a **value copy** (`SessionHeader` / `EventRange` / `std::vector<Message>`
/ `SessionSnapshot`). No reference into `log_` or `header_` escapes the lock. A
reader therefore never observes a torn append (S13) and every derived value is
computed from one coherent `(header_, log_)` state. `header()` is the 27-D9
addition: it returns `SessionHeader` **by value**, so a later `append` that
mutates `header_.title`/`header_.updatedAt` cannot retroactively change an
earlier read, and a concurrent read cannot tear. It is no longer `noexcept`
(the copy can allocate), unlike the immutable-field accessors `id()`/`kind()`;
because it now takes `appendMutex_`, it joins the 27-I2 forbidden re-entry set
(§3.3).

`EventRange` is returned by value from `events()`/`ownEvents()`; the fork slice
uses `log_.begin() + prefix` on the local copy, so the iterator range stays
valid after the lock is released.

### 3.2 `PlanModeController` lock order

Locks: `mutex_` guards `pending_`/`pending_exit_`/`memo_`
(`include/ymh/agent/plan_mode_controller.hpp:65`); `commit_mutex_` serializes the
whole commit (`:72`).

**Pinned order: `commit_mutex_ → mutex_`, `commit_mutex_ → appendMutex_`, and
`commit_mutex_ → SessionManager::mutex_`; never `mutex_` together with
`appendMutex_`, and never `SessionManager::mutex_ → commit_mutex_`.**

The third edge is transitive through the injected `AppendFn`. The
`WorkspaceRuntime` lambda (`src/agent/workspace_runtime.cpp:120-127`) calls
`sessions_.sessionPtr(id)` (`:122`), which takes and **releases**
`SessionManager::mutex_` before returning
(`src/session/session_manager.cpp:147-153`, lock at `:148`), and then
`session->append(mode)` (`:123`), which takes the session's `appendMutex_`.
`commit_locked_` invokes `append_` while holding `commit_mutex_`
(`plan_mode_controller.cpp:148`), so under `commit_mutex_` a controller commit
takes `SessionManager::mutex_` and then `appendMutex_` **in sequence, not
nested** — the pinned edges are `commit_mutex_ → SessionManager::mutex_` and
`commit_mutex_ → appendMutex_`, and `SessionManager::mutex_` is never held
together with `appendMutex_`. No inversion exists today — the `erase()` caller
(`src/host/host_runtime.cpp:769`) runs after `deleteSession` has released
`SessionManager::mutex_` — but the edge
is pinned so that a future path taking `SessionManager::mutex_` before
`commit_mutex_` is recognized as an inversion.

Verified acquisition sites:

| Path | `commit_mutex_` | `mutex_` | `appendMutex_` | Anchor |
|---|---|---|---|---|
| `set()` | `:37` | `:40` (released `:59`) | via `active()`→`events()` `:19`, or via `commit_locked_`→`append_` `:148` | `plan_mode_controller.cpp:37-61` |
| `erase()` | `:131` | `:132` | — | `:127-136` |
| `commit_()` | `:139` | — | via `commit_locked_`→`append_` | `:138-141` |
| `commit_locked_()` (caller holds `commit_mutex_`) | — | `:145`, `:150` | via `append_` `:148` | `:143-152` |
| `active()` (may be called under `commit_mutex_` from `set()`) | — | `:13` (released `:18`), `:20` | via `project_(session.events())` `:19` | `:10-27` |
| `request_exit()` | — | `:65` | — | `:64-68` |
| `apply_pending_at_step_start()` | via `commit_` `:91` | `:75` (released `:89`) | via `commit_` | `:70-95` |
| `flush_pending_at_turn_end()` | via `commit_` `:119` | `:102` (released `:116`) | via `commit_` | `:97-125` |

`mutex_` is released before every session touch: `active()` releases at `:18`
before `project_` at `:19`; `set()` releases at `:59` before `commit_locked_` at
`:60`; `apply_pending_at_step_start` releases at `:89` before `commit_` at `:91`;
`flush_pending_at_turn_end` releases at `:116` before `commit_` at `:119`.
Therefore `mutex_` and `appendMutex_` are never held together.

**`erase()` is the change (27-D3).** Pre-fix it took `mutex_` alone
(`25` §14.1). Post-fix:

```cpp
void PlanModeController::erase(const SessionId& session) noexcept {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);  // :131
    std::lock_guard<std::mutex> lock(mutex_);                // :132
    pending_.erase(session);
    pending_exit_.erase(session);
    memo_.erase(session);
}
```

This closes the interleaving: a `deleteSession` can no longer slip between
`commit_locked_`'s invalidate (`:146`) and record (`:151`) and leave a `memo_`
entry for a deleted id. Because `erase()` acquires `commit_mutex_` **first**, it
does not introduce an order inversion against `set()`/`commit_`.

The caller side (`HostRuntime::deleteSession` → `runtime_.plan_mode().erase(id)`,
`src/host/host_runtime.cpp:769`) now takes `commit_mutex_` from the
`deleteSession` path; this is exactly the "must be checked against the N12 order"
question `25` §14.1 raised, and it is consistent with the pinned order.

### 3.3 The load-bearing no-re-entrancy invariant (27-D4)

**Every path into `appendEventLocked` holds `appendMutex_` and publishes
committed events inline, on the calling thread, while still holding it:**

| Write path | Lock | `publishCommitted` under the lock |
|---|---|---|
| `appendEvent` | `:560` | via `appendEventLocked` `:561` → `:584` |
| `appendAutoRename` | `:589` | via `appendEventLocked` `:614` → `:584` |
| `appendBatch` | `:618` | loop `:644-646` → `:645` |

`EventBus::publishCommitted` (`src/core/event_bus.cpp:240-288`) snapshots the
handler lists under `state_->mutex` (`:245-260`), **releases** that mutex, and
then invokes:

- global subscribers registered by `EventBus::subscribe(Handler)` (`:262-264`,
  registered `:176-181`),
- committed subscribers registered by `EventBus::subscribeCommitted(RecordHandler)`
  (`:265-267`, registered `:192-197`),
- session subscribers registered by `EventBus::subscribe(const SessionId&, Handler)`
  via `mailbox.drain` (`:273-287`), which calls the sink **inline** on the
  calling thread (`SessionMailbox::drain`, `:97-113`).

All three run on the appender thread while it holds `appendMutex_`.

**Invariant (pinned).** No handler invoked by `publishCommitted` — global
subscriber, session subscriber, or committed subscriber — and no injected
`ProjectionFn` (`include/ymh/agent/plan_mode_controller.hpp:29`) or injected
`AppendFn` (`:28`) may:

1. re-enter `PlanModeController` (`set`, `active`, `apply_pending_at_step_start`,
   `flush_pending_at_turn_end`, `request_exit`, `erase`),
2. call `Session::header()`, `events()`, `ownEvents()`, `deriveMessages()`,
   `snapshot()`, `append`, `appendEvent`, `appendAutoRename`, or `appendBatch`,
   or
3. (injected `AppendFn` only) re-enter the controller or the session's append
   path. `commit_locked_` calls `append_` under the non-recursive
   `commit_mutex_` (`plan_mode_controller.cpp:148`); a re-entrant `AppendFn`
   self-deadlocks on `commit_mutex_` and, if it re-enters the same session's
   append, on `appendMutex_`, or
4. re-enter `SessionManager`. `SessionManager::deleteSession` publishes a
   committed record while holding `SessionManager::mutex_`
   (`src/session/session_manager.cpp:196-202`), so a handler that calls back
   into `SessionManager` there self-deadlocks on `SessionManager::mutex_`. No
   production handler does this today (see §7.1).

**Consequence of violation: a recursive self-deadlock on the mutex the trigger
class re-acquires.** The pinned effect is **per trigger class**, not uniformly
`appendMutex_`:

- **Plain-append handler class → `appendMutex_`.** A handler invoked by
  `publishCommitted` from an ordinary session append runs on the appender thread
  while it holds the non-recursive `appendMutex_` (`appendEvent` `:560`,
  `appendAutoRename` `:589`, `appendBatch` `:618`, all reaching `:584`). A
  handler that re-enters a read accessor or the append path re-acquires
  `appendMutex_` and blocks forever.
- **Plan-mode-commit class → `commit_mutex_`.** When `publishCommitted` is
  reached through a controller commit, the appender already holds `commit_mutex_`
  (`set()` `:37` → `commit_locked_` `:60`; or `commit_()` `:139`), and
  `commit_locked_` calls `append_` at `:148` before `:584` runs. A handler that
  re-enters `set()`/`commit_`/`erase` re-acquires `commit_mutex_` and
  self-deadlocks there — **not** on `appendMutex_`. A handler that instead
  re-enters a read accessor or the append path still deadlocks on `appendMutex_`,
  and one that calls `active()` hits the §3.3.2 cold-memo case (deadlock on
  `appendMutex_` only when the memo is cold). `25` §N12 `:585-589` distinguishes
  exactly these two modes.
- **`ProjectionFn` → `commit_mutex_` for controller re-entry.** `project_` runs
  at `active()` `:19` **after** `session.events()` has released `appendMutex_`
  (see §3.3.2), so a `ProjectionFn` calling a read accessor or `append` does not
  self-deadlock on `appendMutex_`; its hazard is re-entering the controller,
  which self-deadlocks on `commit_mutex_` when `active()` is reached from `set()`
  (which holds it at `:37`).
- **Injected `AppendFn` → `commit_mutex_`** (and `appendMutex_` if it re-enters
  the same session's append path) — item 3 above.

There is no TSan report for a self-deadlock and no timeout — the daemon hangs.

**Verification status — audit-only (27-D8).** This invariant is **not**
runtime-enforced and cannot be made so by TSan: a violation is a recursive
self-deadlock, which TSan does not report. No debug guard is shipped — this
errata pins shipped code and adds no behavior change, and a thread-local guard
would itself be a code change. 27-I2 is therefore pinned as an **audit-only
contract**, and its verification evidence is the §7.1 audit artifact, not a
test. The artifact names every registration/injection site and must be
re-scanned whenever a site is added or moved and at each gate of this spec.

#### 3.3.1 `subscribe()` handlers are the sharp edge

`EventBus::subscribe(Handler)` (global, `:176-181`) and
`EventBus::subscribe(const SessionId&, Handler)` (session, `:183-190`) both
register handlers that are invoked by **both** `publish()` (live, no lock) and
`publishCommitted()` (under `appendMutex_`); only `subscribeCommitted` handlers
are exclusive to the committed path. A handler written for the live path that
reads session state is therefore safe when it fires from a live `emit()` and a
self-deadlock when it fires from a committed append. Handlers must not assume the
live path.

#### 3.3.2 The `active()` cold-memo nuance

`PlanModeController::active()` takes `mutex_` only to consult `memo_` and
releases it before `project_(session.events())` (`:13-19`). When the memo is
**warm**, `active()` never touches `appendMutex_`. When the memo is **cold**, it
calls `session.events()`, which takes `appendMutex_`. A committed handler calling
`active()` is therefore safe **only** on the warm path; if the memo is cold it
self-deadlocks. The invariant in §3.3 forbids calling `active()` from a committed
handler at all — the memo's warmth is not an observable guarantee at the call
site. (The `commit_mutex_`-vs-`mutex_` distinctness at `25` §N12 protects the
controller's own mutexes; it does **not** protect the session's `appendMutex_`.)

### 3.4 `reload()` assumption (27-D5)

`Session::reload()` (`src/session/session.cpp:512-535`) writes `log_` **without**
`appendMutex_` at `:513`, then calls the now-locking `ownEvents()` at `:518`, and
later iterates the same unlocked `log_` at `:526` (the max-turn/step scan).

This is safe **only because** `reload()` is `private`
(`include/ymh/session/session.hpp:300`) and its sole caller is the constructor
(`src/session/session.cpp:501`), before the object is published to any other
thread.

**Recorded assumption:** no other thread can hold or observe a `Session` while
`reload()` runs. If `reload()` ever becomes reachable on a live session (e.g. a
public refresh/rebuild), then (a) `:513`'s `log_ = store_->read(...)` and the
`:526` iteration must take `appendMutex_` (or the whole body must be locked), and
(b) the `:518` `ownEvents()` call must be replaced with a lock-free helper (or the
body must use an already-held-lock variant), otherwise it recursively re-acquires
`appendMutex_` and self-deadlocks.

### 3.5 `erase()` `noexcept` note (27-D6)

`void erase(const SessionId&) noexcept;` (`include/ymh/agent/plan_mode_controller.hpp:50`)
now takes two locks (`:131`, `:132`). `std::mutex::lock()` is not `noexcept`; a
lock failure (or any throw) escaping a `noexcept` function calls
`std::terminate`. `erase()` must therefore remain a **leaf**: it must not call
anything that can re-enter the controller, take a session lock, or throw. Today
it only erases from three maps (`:133-135`) and returns.

---

## 4. Reconciliation with `01-session.md`

### 4.1 I7 — purity of the projection

`01` I7 (`:1121-1123`) pins `deriveMessages` as pure/deterministic: "It reads only
`header()` and `events()`; no I/O, no clock, no randomness."

**Amendment (27-D1/27-D2).** Two distinct entities are now separated:

- The **free function** `ymh::deriveMessages(const SessionHeader&, const EventRange&)`
  (`include/ymh/session/session.hpp:218-219`) remains **pure** as pinned: no I/O,
  no clock, no randomness, a pure function of its two arguments.
- The **member** `Session::deriveMessages()` (`:554-557`) now additionally
  acquires `appendMutex_` before delegating to the free function. Acquiring a
  mutex is a **synchronization side effect, not I/O**, so I7's no-I/O pin holds;
  but the member is no longer a lock-free call. Its determinism is now the
  stronger, explicit statement: *two calls made against the same log head
  return identical messages; each call observes one coherent log state.* Two
  calls against different heads may differ — as they always could; the lock
  guarantees neither call sees a torn state.

This is a **semantics change to the pin's scope** (a blocking lock was added to a
previously lock-free member) and is recorded as such, not as a silent
clarification.

### 4.2 I21 — snapshot equivalence

`01` I21 (`:1182-1184`) pins: if `snapshot.at` equals the current log head, then
`snapshot.messages == deriveMessages()`.

**Amendment (27-D1/27-D2).** The invariant is unchanged in force and is now
stronger in derivation: `snapshot()` takes `appendMutex_` once and computes
`at`, `header`, `eventCount`, and `messages` from the same `(header_, log_)`
state (`src/session/session.cpp:661-668`). `snapshot.messages` equals the free
projection over the same `(header, log-at-`at`)`; it does **not** call the
member `deriveMessages()` (which would re-acquire `appendMutex_` and
self-deadlock). Equivalence with a later member `deriveMessages()` at the same
head holds because both use the same free function.

### 4.3 I18 / §11 — reader consistency

`01` I18 (`:1172-1173`) pins: "Concurrent `append` calls to one `Session` are
serialized; a reader never observes a torn append (S13)." `01` §11 (`:1081-1082`)
names only `events` and `deriveMessages`.

**Amendment (27-D1, extended by 27-D9).** The reader guarantee now covers all
five accessors: `header`, `events`, `ownEvents`, `deriveMessages`, `snapshot`.
The §11 sentence is extended to read: *Reads (`header`, `events`, `ownEvents`,
`deriveMessages`, `snapshot`) are taken under `appendMutex_` over the in-memory
log; a reader never observes a torn append (S13).*

`01` §11 also pins (`:1086-1088`): "`Session::append` is not reentrant: an event
handler that runs on `publish` must not call back into `append` for the same
session synchronously." **Amendment (27-D4).** This is generalized: no handler
invoked by `publishCommitted`, no `ProjectionFn`, and no injected `AppendFn` may
call back into any of the four read accessors, `append`, or the controller
(§3.3).

### 4.4 `25` §N12 amendment

`25` §N12 pins the controller lock order and the load-bearing re-entrancy
invariant, but (a) says "`request_exit` and `erase` take `mutex_` alone" — now
false for `erase` (§3.2), (b) enumerates only `Session::events()`/`append()` as
forbidden re-entry calls — now extended to `header()`, `ownEvents`,
`deriveMessages`, `snapshot`, the `ProjectionFn`, and the `AppendFn` (§3.3), and
(c) omits the
transitive `commit_mutex_ → SessionManager::mutex_` lock-order edge (§3.2). This
errata is the authority for those three points; the `25` document should be
updated to reference it.

---

## 5. Invariants

**27-I1 — Read accessors copy under `appendMutex_`.** `header()`, `events()`,
`ownEvents()`, `deriveMessages()`, and `snapshot()` acquire `appendMutex_` for
the whole read and return value copies; **for these five accessors** no
reference into `log_`/`header_` escapes the lock. `header()` returns
`SessionHeader` by value (27-D9); `id()`/`kind()` read only immutable fields and
stay `noexcept`. `nextTurnId()`/`nextStepId()` are not covered (narrowed 27-R1,
§5.3). (§3.1)

**27-I2 — No re-entry from an inline handler, projection, or `AppendFn`.** No
handler invoked by `EventBus::publishCommitted` (global, session, or committed),
and no injected `ProjectionFn` or `AppendFn`, may re-enter `PlanModeController`,
re-enter `SessionManager` from the `deleteSession` publish path
(`session_manager.cpp:196-202`), or call any of
`header`/`events`/`ownEvents`/`deriveMessages`/`snapshot`/`append`/
`appendEvent`/`appendAutoRename`/`appendBatch`. Violation ⇒ recursive
self-deadlock on the mutex the trigger class re-acquires: a handler firing from
an ordinary append on the non-recursive `appendMutex_`; a handler or
`ProjectionFn` re-entering `set()`/`commit_`/`erase` during a plan-mode commit on
`commit_mutex_`; an injected `AppendFn` on `commit_mutex_` (and on `appendMutex_`
if it re-enters the session append path). **Audit-only (27-D8):** the
verification evidence is the §7.1 audit artifact, not a test — the failure is a
silent hang TSan does not report. (§3.3)

**27-I3 — Controller lock order.** Every multi-lock controller path acquires
`commit_mutex_` before `mutex_` and before both `appendMutex_` and
`SessionManager::mutex_`; the latter two are acquired **sequentially**, via the
injected `AppendFn` (`SessionManager::mutex_` is released before `appendMutex_`
is taken), and are never held together. `mutex_` and `appendMutex_` are never
held together, and no path takes `SessionManager::mutex_` before
`commit_mutex_`. (§3.2)

**27-I4 — `reload()` is construction-only.** `reload()` runs before the `Session`
is published; its lock-free `log_` write (`:513`) and iteration (`:526`) are
valid only under that assumption. (§3.4)

**27-I5 — `erase()` is a `noexcept` leaf.** `erase()` takes `commit_mutex_` then
`mutex_` and performs only map erasures. Its body is non-throwing and both lock
acquisitions are uncontended leaves; the theoretical `std::system_error` from
`std::mutex::lock()` is accepted (see §3.5). It must not re-enter the controller
or a session lock. (§3.5)

**27-I6 — `snapshot()` uses the free projection.** `snapshot()` calls
`ymh::deriveMessages(header_, log_)`, never the member `deriveMessages()`, so it
never recursively acquires `appendMutex_`. (§3.1, §4.2)

### 5.3 Residual — follow-up **27-R1** (narrowed; `header()` closed by 27-D9)

**`header()` — CLOSED by 27-D9.** `header()` used to return an unlocked
`const SessionHeader&` while `header_.updatedAt` (`src/session/session.cpp:573`,
`:632`) and `header_.title` (`:576`, `:635`) are written under `appendMutex_`.
That was a **latent API hazard, not an active production race**: distinct memory
locations do not race, and today's callers read only immutable fields —
`Session::fork` reads `.cwd`/`.model`/`.serverProfile` (`:690/694/695`),
`compactor.cpp` reads `.model` (`src/agent/compactor.cpp:108-112`), and the free
`ymh::deriveMessages` ignores the header (`[[maybe_unused]]` at `:362`). The
hazard was that a future caller reads a mutated field through the escaped
reference. Rev 4 removes it in all builds: `header()` returns a **by-value
`SessionHeader` copy taken under `appendMutex_`** (27-D9), so no reference into
`header_` escapes the lock and a read can never tear or be mutated later.

The resolution is **minimal**: it reuses the already-pinned 27-I1 mechanism (a
locked value copy) instead of introducing a proxy type or an atomic
representation, and the blast radius is only the return type plus the one body
— every existing call site already consumes the result by value (a field read,
or a `const SessionHeader&` parameter), so no caller body changes. The added
cost is one `SessionHeader` copy per call at non-hot sites (session creation,
per-turn compaction, tests), which is acceptable. Because `header()` now takes
`appendMutex_`, it joins the 27-I2 forbidden re-entry set (§3.3).

**Remaining residual (narrowed 27-R1).**

- `id()` (`include/ymh/session/session.hpp:234`), `kind()` (`:235`) — read the
  immutable `header_.id`/`header_.kind`; **not** a race and safe today.
- `nextTurnId()` (`:291`), `nextStepId()` (`:292`) — genuine latent races:
  `nextTurn_`/`nextStep_` are written under `appendMutex_`
  (`src/session/session.cpp:580/582`, `:639/641`). These are a **different
  defect class** (unlocked scalar reads) and are **out of scope** of this
  amendment. Unlike `header()`, they already return by value, so the fix is not
  the return type but the read itself; and taking `appendMutex_` there would
  make them re-entry-unsafe exactly like the 27-I2 accessors, so the fix must
  first audit every caller. **27-R1** now tracks only these two counters.

Closing 27-R1 requires either an atomic representation or a locked by-value
accessor for the two counters, plus the caller audit above; **27-R1** tracks the
narrowed residual for a future spec.

---

## 6. Failure modes

| ID | Trigger | Effect | Handling |
|---|---|---|---|
| 27-F1 | A committed/global/session handler, `ProjectionFn`, or injected `AppendFn` calls a session read accessor, `append`, or the controller while `publishCommitted` runs inline. From an ordinary append the appender holds `appendMutex_`; from a plan-mode commit it holds `commit_mutex_` (and `appendMutex_` via `append_`) | Recursive self-deadlock on the mutex the trigger class re-acquires: `appendMutex_` for a handler re-entering a read accessor/append from an ordinary append; `commit_mutex_` for a handler or `ProjectionFn` re-entering `set()`/`commit_`/`erase` from a plan-mode commit, and for a re-entrant `AppendFn`. The daemon hangs (no exception, TSan cannot detect a self-deadlock) | Forbidden by 27-I2. Audit-only (27-D8): the §7.1 audit artifact must be re-scanned for every new or moved subscriber, projection, or `AppendFn` site. |
| 27-F2 | `deleteSession` calls `erase()` while `commit_locked_` is between invalidate (`:146`) and record (`:151`) | A `memo_` entry for a deleted session is resurrected (stale projection; benign only until the id reappears) | Closed by 27-D3: `erase()` blocks on `commit_mutex_` until the commit completes, then erases. Regression test `EraseDoesNotResurrectMemoAcrossCommitWindow`. |
| 27-F3 | `reload()` is called on a live session (future change) | Unlocked `log_` write races appends; `:518` `ownEvents()` recursively acquires `appendMutex_` | Forbidden by 27-I4; see §3.4 for the required change if it ever becomes live. |
| 27-F4 | A future multi-lock path takes `mutex_` then `appendMutex_` (or `appendMutex_` then `commit_mutex_`) | ABBA deadlock against `set()`/`commit_` | Forbidden by 27-I3; the `25` §N12 inversion note is retained. |
| 27-F5 | A throw (including a lock failure) escapes `erase()` | `std::terminate` (function is `noexcept`) | 27-I5: `erase()` stays a leaf and does not throw. |
| 27-F6 | A future change reverts `header()` to an unlocked reference, or adds an unlocked read of a header field that is mutated under `appendMutex_` | A read of `title`/`updatedAt` races the appender (latent data race) | Forbidden by 27-I1/27-D9; the `static_assert` of 27-T6 fails the build if the return type is a reference. |

---

## 7. Test plan

| ID | Level | What it pins | Location |
|---|---|---|---|
| 27-T1 | unit (**requires TSan**) | One reader thread exercising all five accessors (incl. `header()`) runs concurrently with an appender; each accessor returns a coherent, bounded copy | `tests/unit/session_test.cpp:955` `SessionLogConcurrency.ReadersSeeConsistentLogWhileAppenderRuns` |
| 27-T2 | unit | `erase()` blocks on `commit_mutex_` across the invalidate→record window; the memo is cold afterwards and re-folds exactly once | `tests/unit/plan_mode_test.cpp:295` `PlanModeControllerTest.EraseDoesNotResurrectMemoAcrossCommitWindow` |
| 27-T3 | unit (existing) | `set()` compare-and-append atomicity across the memo-invalidation window (the H1 fix this errata builds on) | `tests/unit/plan_mode_test.cpp:235` `PlanModeControllerTest.SetIsAtomicAcrossCommitInvalidationWindow` |
| 27-T4 | unit (existing) | `snapshot().messages` equals `deriveMessages()` at `snapshot.at` (I21) and `snapshot()` does not self-deadlock | `tests/unit/session_test.cpp:477` `Session.SnapshotMatchesDerivedMessages` (asserts `snapshot.messages.size() == deriveMessages().size()` and `snapshot.at == events().back().seq`); 27-T1 also exercises `snapshot()` concurrently |
| 27-T5 | build | `-Werror`; no new warnings from the lock additions | project build |
| 27-T6 | unit + compile-time | `header()` returns `SessionHeader` **by value** under `appendMutex_` (27-D9): a `static_assert` pins the non-reference return type (so a revert to `const SessionHeader&` fails the build), and a captured snapshot is unchanged by a later append while the live value advances | `tests/unit/session_test.cpp` `Session.HeaderReturnsStableSnapshot` |
| 27-A1 | audit (§7.1) | 27-I2 no-re-entrancy: every subscriber / projection / `AppendFn` registration site is scanned for the forbidden calls of §3.3 | §7.1 checklist |

27-T1 is the direct regression for the pre-fix race (sub-defect (b)); it fails
under TSan before the fix. 27-T2 is the direct regression for sub-defect (a); it
parks `commit_locked_` with the memo invalidated and asserts `folds == 2`.

**27-T1 requires the TSan build.** `YMH_TSAN` defaults **OFF**
(`CMakeLists.txt:49`), so under the default `ctest` invocation 27-T1 passes
vacuously — it exercises the concurrent reads, but TSan is not instrumenting, so
the pre-fix race is not detected. The pinned invocation (`CMakeLists.txt:47`):

```sh
cmake -S . -B build-tsan -G Ninja -DYMH_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j
ctest --test-dir build-tsan -R SessionLogConcurrency --output-on-failure
```

The test is a valid regression **only** under this command; a green
default-build `ctest` is not evidence for 27-T1.

### 7.1 Audit artifact for 27-I2 (27-D8)

Because 27-I2's violation is a silent self-deadlock TSan cannot detect, the
invariant is **audit-only** (§3.3) and its evidence is this artifact.

- **What is audited:** every registration/injection site whose code runs on the
  appender thread while a lock it may re-acquire is held — the global
  `EventBus::subscribe(Handler)` handlers, the session
  `EventBus::subscribe(const SessionId&, Handler)` handlers, and the committed
  `EventBus::subscribeCommitted(RecordHandler)` handlers (all inline under
  `appendMutex_`, and also under `commit_mutex_` on a controller commit), plus
  the injected `ProjectionFn` (under `commit_mutex_` when `active()` is reached
  from `set()`) and the injected `AppendFn` (under `commit_mutex_`, acquiring
  `appendMutex_` inside `append`). Each site is scanned for the forbidden calls
  of §3.3: re-entering `PlanModeController`; calling
  `header`/`events`/`ownEvents`/`deriveMessages`/`snapshot`/`append`/
  `appendEvent`/`appendAutoRename`/`appendBatch`; and, for `AppendFn`,
  re-entering the controller or the session append path. A committed handler
  additionally runs
  under `SessionManager::mutex_` when `SessionManager::deleteSession` publishes
  (`src/session/session_manager.cpp:196-202`), so a handler re-entering
  `SessionManager` there self-deadlocks on `SessionManager::mutex_`; no
  production handler does this today, and the site is recorded here so a future
  one is audited.
- **By whom:** the author of any change that adds or moves a registration or
  injection site performs the scan and fills the row; the reviewer at the next
  gate of this spec (or of the spec owning the site) re-checks it.
- **When:** on any new or moved registration/injection site, and at every gate of
  spec 27. The Rev 4 rows below are the current baseline (re-verified unchanged
  from Rev 3; adding `header` to the forbidden-call scan found no production
  handler call — no handler calls `header()`).
- **Artifact form and predicate:** this checklist. A row must be completed — or
  the kind explicitly marked "no production sites" — before a change touching
  these seams is accepted. **PASS** = every row completed with a clean
  forbidden-call scan (or explicitly marked "no production sites"); **FAIL** =
  any row missing, or any forbidden call found.

| Site | Kind | Anchor | Forbidden-call scan (Rev 4) |
|---|---|---|---|
| `HostRuntime` live subscriber | global `subscribe` | `src/host/host_runtime.cpp:298-302` | clean — reads `event` + `server_`; no controller/session call |
| `HostRuntime` committed subscriber | `subscribeCommitted` | `src/host/host_runtime.cpp:303-304` → `handleCommittedRecord` `:347-363` | clean — `forward_mutex_`, `forwarder_`, `server_` only; no controller/session call |
| `headless` live subscriber | global `subscribe` | `src/cli/headless.cpp:192` | clean — CLI output only |
| `WorkspaceRuntime` injected `AppendFn` | ctor injection | `src/agent/workspace_runtime.cpp:120-127` | clean as an append-path caller (runs under `commit_mutex_` when invoked by `commit_locked_` `:148`, and acquires `appendMutex_` inside `append`); it is the re-entry target 27-I2 forbids others to call |
| default `ProjectionFn` `plan_mode_active` | ctor injection | `include/ymh/agent/plan_mode_controller.hpp:31`, `src/session/plan_mode.cpp:7-20` | clean — folds an `EventRange` only; no controller/session call |
| session subscribers | `subscribe(const SessionId&, …)` | `src/core/event_bus.cpp:183-190` | none in production today; any future registration is audited here |

---

## 8. Revision log

| Rev | Date | Change |
|---|---|---|
| Rev 1 | 2026-09-19 | Initial authoring. Pins the F3 locking contract for the already-shipped fix: `Session::ownEvents()`/`deriveMessages()`/`snapshot()` copy under `appendMutex_` (27-D1), `snapshot()` uses the free `ymh::deriveMessages` (27-D2), `PlanModeController::erase()` takes `commit_mutex_` then `mutex_` (27-D3), the load-bearing no-re-entrancy invariant for inline `publishCommitted` handlers and `ProjectionFn` (27-D4), the `reload()` construction-only assumption (27-D5), and the `erase()` `noexcept` leaf requirement (27-D6). Reconciles 01 I7/I18/I21/§11 and amends 25 §N12 (supersedes "`erase` takes `mutex_` alone"). Closes 25 §14.1 row `F3`. Records the `noexcept` field accessors as an open residual (§5.3). No wire/schema/protocol/behavior change. Status **written** — pending independent review. |
| Rev 2 | 2026-09-19 | Rev 1 gate remediation. The task-stated Rev-1 gate was cited as 0 HIGH / 1 MEDIUM / 5 LOW; the on-disk gate report `/tmp/opencode/regate27.md` actually records **0 HIGH / 2 MEDIUM / 6 LOW**. Rev 2 addressed the task-stated set; the on-disk M-1 (wrong consequence for the controller trigger) remained open, fixed by Rev 3. **MEDIUM-1:** 27-I2 (no-re-entrancy) pinned as **audit-only** with a required verification artifact (§7.1 — what/by whom/when/form), since no debug guard ships and TSan cannot detect the self-deadlock; new amendment 27-D8. **27-T1:** pinned the `-DYMH_TSAN=ON` build/command and marked it as requiring TSan (default build passes vacuously). **LOW-1:** §5.3 corrected to a latent API hazard — `id()`/`kind()` immutable and safe, `header()`'s current callers read immutable fields, the free projection ignores the header — and given follow-up ID **27-R1**. **LOW-2:** added the transitive `commit_mutex_ → SessionManager::mutex_` edge to §3.2/27-I3. **LOW-3:** added the injected `AppendFn` to §3.3/27-I2. **LOW-4:** scoped 27-I1 to the four accessors and cross-referenced 27-R1. **LOW-5:** reconciled 27-I5 with §3.5 (theoretical lock throw accepted). No pinned code claim changed. |
| Rev 3 | 2026-09-19 | Rev 2 re-gate remediation (`/tmp/opencode/regate27-rev2.md`: 0 HIGH / 1 MEDIUM / 7 LOW). **MEDIUM (F-M1 = on-disk `regate27.md` M-1):** corrected the 27-I2 consequence claim so each trigger class names the mutex it actually self-deadlocks on — the plain-append handler class on `appendMutex_`, the plan-mode-commit class (a handler or `ProjectionFn` re-entering `set()`/`commit_`/`erase`) on `commit_mutex_` — in §3.3, 27-F1, and 27-I2; the prohibition is unchanged. **F-L1:** the `ProjectionFn` consequence no longer claims an `appendMutex_` self-deadlock for a direct accessor call (`project_` runs after `events()` releases the lock); its hazard is controller re-entry under `commit_mutex_` (§3.3, §3.3.2). **F-L2:** §3.4/27-I4 now name `reload()`'s second unlocked `log_` access (`:526`). **F-L3:** 27-T4 cites `Session.SnapshotMatchesDerivedMessages` (`tests/unit/session_test.cpp:476`). **F-L4:** 27-T1 reworded to one reader thread exercising all four accessors. **F-L5:** §3.2/27-I3 now state the `SessionManager::mutex_` and `appendMutex_` acquisitions are sequential under `commit_mutex_`, not nested. **F-L6:** §3.3/27-I2/§7.1 record the `deleteSession`-under-`SessionManager::mutex_` publish path (`session_manager.cpp:196-202`). **F-L7:** §7.1 gains an explicit PASS/FAIL predicate and the `AppendFn` row notes it runs under `commit_mutex_`. Every pinned `file:line` anchor re-verified against the uncommitted F3 tree; no pinned code claim changed. |
| Rev 4 | 2026-09-19 | Closes the `header()` half of follow-up 27-R1 (the latent unlocked-reference API hazard of Rev 3 §5.3). **Resolution (option a, minimal):** new amendment **27-D9** — `Session::header()` now returns a by-value `SessionHeader` **copy taken under `appendMutex_`** (`include/ymh/session/session.hpp:233`, `src/session/session.cpp:712-715`) and drops `noexcept` (a copying accessor that can allocate cannot be `noexcept`). Reuses the already-pinned 27-I1 locked-value-copy mechanism instead of a proxy/atomic type; blast radius is the return type plus the one body, because every existing call site already consumes the result by value, so no caller body changes. Cost: one `SessionHeader` copy per call at non-hot sites (session creation, per-turn compaction, tests) — accepted. **27-I1** now covers five accessors; **`header()` joins the 27-I2/§3.3/§7.1 no-re-entry set** (it now takes `appendMutex_`); **27-T1** exercises it concurrently. New **27-T6** pins the contract: a `static_assert` fails the build if the return type reverts to a reference, plus a stable-snapshot runtime check. New failure mode **27-F6** (revert to an unlocked reference) is caught by 27-T6. **27-R1 is narrowed** to `nextTurnId()`/`nextStepId()` (unlocked scalar reads, a different defect class, out of scope; they already return by value, and a locked fix must first audit callers). No wire/schema/protocol/behavior change beyond removing the hazard. Status: amendment pending independent re-gate. |
