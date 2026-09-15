# 12 — M1 Spec/Code Drift Errata

```
Status: written · verified: — · reviewer: —
Component: 12 (errata) — reconciliation register; amends 11 by reference only
Depends on: 11-m2-errata.md (normative), 00-architecture.md §9.6/§9.10/§57 Step 13/§58; specs 01–05
Scope: M1 spec-text ↔ code drift discovered during the M2 interface freeze (Wave-1 track F)
```

## 1. Purpose, choice, and precedence

This is the Wave-1 track F deliverable: it reconciles the **M1 spec text** with the
**code on disk at freeze time**. The freeze review already produced 26 defects
(`D1`–`D26`) and pinned their corrections in `11-m2-errata.md`; that document is
**normative** and this one adds **no new semantics**. What 12 adds is a single
place that quotes the stale spec text verbatim and maps each stale line to (a) the
code truth at freeze, (b) the authoritative amendment in 11, and (c) the wave that
lands the fix — so a Wave-1 coder reading a spec directly does not follow
superseded text.

**Choice of artifact (stated per the task).** The task allowed appending to
`11-m2-errata.md` or adding a short `12-m1-drift-errata.md`. This file takes the
**second** option. Reason: all five named drift items are *already pinned* in 11
(§3.3, §3.5, §3.4/§4.4, §6, §9.2), so appending would duplicate frozen content
and re-open a verified gate; a separate register keeps 11 byte-stable and records
the reconciliation explicitly.

**Precedence (pinned).** If this document and 11 disagree, **11 wins**. If the
stale spec text and 11 disagree, **11 wins**. M1 code that 11 declares wrong is
fixed in Wave 1, not preserved.

## 2. Drift register (DR1–DR6)

| ID | Spec § | M1 spec text | Code truth at freeze | Authority | Wave |
|---|---|---|---|---|---|
| DR1 | 03 §5.3 | "its own host claim/heartbeat" reads daemon-exclusive | `WorkspaceRegistry::claimHost`/`releaseHost`/`heartbeat` are the only host-column writers in M1; supervisor provisional claim is M2 | 11 §9.2 | W1F |
| DR2 | 04 §3.6 | SIGCHLD "handled … self-pipe … reaps all exited children" | `src/execution/process.cpp` reaps by specific pid; no SIGCHLD handler | 11 §3.3 | W1B |
| DR3 | 04 §3.3 step 4 | store opened in `create()`; no injection seam | `workspace_runtime.cpp:117-123` opens the store once; no fake-store seam | 11 §3.5 | W1C |
| DR4 | 05 §7.1 | mapping pinned; code has no wire mapping / no `WorkspaceBusy` | `workspace_runtime.hpp:53-58` lacks `WorkspaceBusy`; `session_persistence.cpp:588-591` conflates held-flock with open failure | 11 §3.4, §4.4 | W1B/W1D |
| DR5 | 02 §5.2 | UUIDv4, minted once | `wiring.cpp:43-48` `make_boot_id()` = pid+steady_clock+counter, per call, not threaded to both sinks | 11 §6 | W0/W1C |
| DR6 | 11 §7.2 | ctor took `Clock& clock` (inert) | a bound `Clock&` cannot change `clock.now()` (static `Clock::now()`), so timeout is not injectable | 11 §7.2, §7.3, E21 | W1E |

### DR1 — 03 §5.3 row-domain table (supervisor provisional vs daemon authoritative)

**Stale text (verbatim), `03-workspace-registry.md:558`:**

> `| WorkspaceHost daemon (canonical for its workspace) | its workspace_sessions rows; its own host claim/heartbeat at startup/shutdown | … |`

**Read ambiguity.** "its own host claim/heartbeat" can be read as daemon-exclusive
ownership of the `host_pid`/`host_boot_id`/`host_socket`/`host_heartbeat` columns,
contradicting line 559, which grants the supervisor `claimHost`/`releaseHost`/
`heartbeat` on those same columns. The columns are a **shared, provisional-first
domain**: the supervisor writes a **provisional** claim at spawn; the daemon's
claim, written after it holds the sidecar `sessions.lock` flock, is
**authoritative** and supersedes it on the first tick.

**Code truth at freeze.** M1's registry (`src/registry/registry.cpp`) implements
`claimHost`/`releaseHost`/`heartbeat` but has no supervisor provisional-claim path
(that is M2). There is no daemon-exclusive lock on the host columns; authority is
derived from the `sessions.lock` flock (03 R5).

**Authority.** 11 §9.2 restates the table so the daemon owns "the host columns of
**its own** workspace" within a shared/provisional-first domain; 11 §9.1 pins the
handshake and the supervisor mint-and-pass nonce. Follow 11 §9.1/§9.2, not the
bare line 558 phrasing.

### DR2 — 04 §3.6 SIGCHLD (no global reaper; process.cpp owns tool children)

**Stale text (verbatim), `04-workspace-host-daemon.md:416`, `:421-422`:**

> `| **SIGCHLD** | **handled, not ignored**; reaped with waitpid(WNOHANG) from an async-signal-safe self-pipe | … |`
>
> `… the loop drains it and reaps all exited children.`

**Correction (pinned).** There is **no global SIGCHLD reaper.** The daemon installs
no handler that calls `waitpid`; `SIG_IGN`/`SA_NOCLDWAIT` remains forbidden. Child
status is owned exclusively by `src/execution/process.cpp`, which reaps by specific
pid (`process.cpp:114-141`; freeze-time `:116`, `:132`, `:141`). This is the single
most dangerous M2 conflict: a global sweep can reap a tool child first and destroy
the exit status the tool layer needs (invariant E8, one reaper).

**Code truth at freeze.** `process.cpp` already reaps by specific pid and loops on
`EINTR`/`WNOHANG`; the daemon has no SIGCHLD handler.

**Authority.** 11 §3.3 amends 04 §3.6 and 04 decision (i) by reference. Wave-1
`reap_guard.hpp` / `signal_policy.hpp` implement this.

### DR3 — 04 §3.3 store-opened-exactly-once + injection seam

**Spec text (correct), `04-workspace-host-daemon.md:307-312`:** step 4
`store := SessionPersistence::open(persistence)` after `chdir`, before `claimHost`.

**Gap.** The spec is right but there is no seam to substitute a fake store in tests
without opening the real SQLite file, and no explicit statement that the store is
opened **exactly once** per daemon.

**Code truth at freeze.** `src/agent/workspace_runtime.cpp:117-123` opens the store
once in `WorkspaceRuntime::create` (freeze-time `:119`).

**Authority.** 11 §3.5 pins store-opened-exactly-once and the injection seam
(`WorkspaceRuntimeOptions` store factory). Follow 11 §3.5.

### DR4 — 05 §7.1 AlreadyRunning → InvalidRequest

**Spec text (already correct), `05-transport.md:757`:**

> `HostErrorCode::AlreadyRunning      -> InvalidRequest (data.kind="AlreadyRunning")`

**Gap (code-side).** The mapping is pinned in the spec but absent from M1 code:
`WorkspaceRuntimeErrorCode` (`workspace_runtime.hpp:53-58`) has no `WorkspaceBusy`,
and `SessionPersistence::open` throws the **same** `StoreOpenError` for a held
`sessions.lock` (EWOULDBLOCK) and every other open failure
(`session_persistence.cpp:568-609`, lock case `:588-591`).

**Correction (pinned).** Add `WorkspaceRuntimeErrorCode::WorkspaceBusy` (held
flock) and `StoreOpenErrorCode::Locked`; map `WorkspaceBusy` →
`HostErrorCode::AlreadyRunning` → `RpcCode::InvalidRequest` with
`data.kind="AlreadyRunning"`. Note `WorkspaceBusy` is **not** the same as
`StoreUnavailable`/`StoreOpenFailed`.

**Authority.** 11 §3.4 (error-code enum + mapping table) and §4.4 (typed error
mapping). 05 §7.1 itself needs no change.

### DR5 — 02 §5.2 UUIDv4 boot nonce

**Spec text (already correct), `02-persistence.md:799-800`:**

> `BootId` is a UUIDv4 minted **once by the daemon at startup**, before the store opens, and injected through `PersistenceConfig::boot_id`

**Gap (code-side).** `src/cli/wiring.cpp:43-48` `make_boot_id()` returns
`pid + "-" + steady_clock + "-" + counter`, minted **per call**; it is not a
UUIDv4, not minted once, and is not threaded to both `PersistenceConfig::boot_id`
and `claimHost`. The three mirrored types (`ymh::BootId`, `ymh::HostBootId`,
`protocol::HostBootId`) have no conversion at the seams.

**Correction (pinned).** `mint_boot_id()` mints a UUIDv4 **exactly once** per
process at daemon startup; store it in `HostIdentity.boot_id` and read it by
reference everywhere. Add one-way adapters `to_host_boot_id` / `to_boot_id` /
`to_protocol_boot_id`; delete `make_boot_id()`. Supervisor mint-and-pass: the
supervisor mints the nonce once at spawn and passes it to the daemon, which
**adopts** it (04 §3.3 step 3 amended by reference; E13 "mint once").

**Authority.** 11 §6 (and 11 §9.1 for the supervisor mint-and-pass handshake).

### DR6 — 11 §7.2 clock reader (post-freeze, errata-internal; track E)

**Stale text (verbatim), `11-m2-errata.md:804`:** `Clock& clock,` in the
`PermissionBroker` ctor.

**Defect.** `Clock&` is **inert**: `clock.now()` resolves to the static
`Clock::now()` regardless of the bound reference, so the broker's timeout is not
injectable and timeout tests cannot be made deterministic.

**Correction (pinned).** The broker takes an injectable clock reader
`using ClockReader = std::function<Clock::time_point()>` (default `Clock::now`);
timeout expiry is measured with `now()`, never `Clock::now()` directly.
`tests/support/manual_clock.hpp` provides `ManualClock::reader()`. This is an
**additive post-gate amendment to 11 §7.2** (invariant E21); it is recorded here
because 12 is the errata register, not because it is M1 spec/code drift.

**Authority.** 11 §7.2 (amended ctor), §7.3 D19.1, §14 E21, §16 (deterministic
timeout test).

## 3. Coverage of the remaining freeze drift (D1–D26)

The five M1 register entries (DR1–DR5) are exactly the spec-text reconciliations
in track-F scope; DR6 is the post-freeze errata-internal amendment. They map to
the freeze defect ids as:

| Register | Freeze defect | 11 section |
|---|---|---|
| DR1 | D21 | §9 |
| DR2 | D7 | §3.3 |
| DR3 | D9 | §3.5 |
| DR4 | D8, D14 | §3.4, §4.4 |
| DR5 | D17 | §6 |

The remaining freeze defects (`D1`–`D6`, `D10`–`D13`, `D15`, `D16`, `D18`–`D20`,
`D22`–`D26`) are **code-vs-spec gaps and interface pins**, not stale spec text, and
are owned by the other Wave-1 tracks. They are not re-listed here; the single
normative traceability table is **11 §13** (`D1–D26 → section → wave`), with the
invariants in 11 §14 and failure modes in 11 §15. This document deliberately adds
no new defect ids.

## 4. Wave-1 reading rule

1. When a spec's text and 11 conflict, **11 is authoritative**; code must
   implement 11.
2. When this register and 11 conflict, **11 is authoritative**.
3. The stale quotations in §2 are recorded for search/disambiguation only; do not
   implement them.
4. Freeze-time code citations describe the baseline the review saw. Wave-1 code is
   actively applying 11, so the working tree may already reflect a fix; the
   register records *why* the change is required, not the current diff.

## 5. References

- `11-m2-errata.md` — normative M2 interface freeze (this doc defers to it).
- `00-architecture.md` §9.6, §9.10, §57 Step 13, §58 — the M2 split.
- Specs `01-session.md`, `02-persistence.md`, `03-workspace-registry.md`,
  `04-workspace-host-daemon.md`, `05-transport.md` — the amended text.
- Code cited (freeze baseline): `src/execution/process.cpp`,
  `src/agent/workspace_runtime.cpp`, `src/cli/wiring.cpp`,
  `src/session/session_persistence.cpp`, `include/ymh/agent/workspace_runtime.hpp`,
  `include/ymh/session/errors.hpp`, `src/registry/registry.cpp`.
