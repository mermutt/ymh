# 16 — Daemon Ownership and the Supervisor Collective

```
Status: written · verified: — · reviewer: —
Revision: 5 — rev 0 closed 6 HIGH, 22 MEDIUM ids (16 distinct findings), 13 LOW;
          rev 1a fixed C-M4 + O-L1; rev 2 closed R-H1/N-H1 + 5 MEDIUM + 13 LOW;
          rev 3 closed N2-H1/N2-M1 + N-M3/N2-M2 + N2-L1..N2-L7; rev 4 closed
          N3-H1/G1, G3, N3-M1/L2, N3-L1/G2, L1, L3..L6; rev 5 closes the round-5
          LOWs N4-L1/N4-L2/R4-L1..R4-L5. See §10.
Component: 16 (superseding errata) — amends 00/03/04/05/10/11 by reference
Depends on: 00-architecture.md §9.6–§9.10, §44–§45, §54, §57–§58; specs 01, 02,
            03, 04, 05, 09, 10, 11, 14
Scope: the ownership change in `requirements_draft.txt` item 4 — unsupervised
       daemons must not exist; N supervisors collectively own the daemon set;
       cross-supervisor visibility and switching; the last-supervisor-exit
       prompt and teardown
Supersedes: 04 §1.2/§3.2/§4.2/§6.5/§10 H8–H9/§14.1(f); 00 §9.8/§9.9/§54 D23;
            10 §2.1 (DaemonStatus comment); 11 §8.2 D20.3 (ClientInstanceId)
Amends:     03 §3.3 (additive table), §3.4 (schema version), §4.3 (read-only
            tolerance), §5.3 (row domains); 04 §3.5/§6.1/§6.2/§6.6;
            05 §7 (additive methods), T-F21, §14.5; 11 §11.2/§11.3 (reused)
```

This document is the design gate for a **reversal of the M2 daemon-lifetime
model**. The M2 model (frozen in spec 11) treats a `WorkspaceHost` daemon as a
detached peer that outlives every supervisor: zero attached clients is a normal,
long-lived state (04 H8, §6.5). The user has reversed that stance:

> *"I have a change of heart on unsupervised daemons. They should not exist. …
> It should be possible to exit from one of the 'ymh', that would end one of the
> supervisors since another supervisor can manage both daemons. But exiting from
> the last 'ymh' should prompt user that both daemons will terminate."*
> — `requirements_draft.txt` item 4

This spec does **not** rewrite the amended specs. It amends them by reference:
every superseded clause is quoted with `file:line`, and its replacement text is
given here. The convention matches spec 11 (the M2 freeze errata) and spec 12.

Naming note. Invariants local to this spec are **`O1`–`O22`**; failure modes are
**`O-F1`–`O-F16`**; decisions are **`16-D1`–`16-D13`**. The prefixes `H`, `T`,
`U`, `R`, `A`, `X`, `L`, `Q`, `P`, `S`, `C`, `M` are already taken by specs
01–15. Review-defect ids `D1`–`D26` (spec 11) and architecture decision ids
`D1`–`D23` (spec 00 §54) keep their existing meaning; where this spec pins a new
architecture-level decision it is written `16-Dn`, never a bare `Dn`.

---

## 1. Purpose, scope, and supersession map

### 1.1 The problem

M2 ships a daemon that is deliberately **supervisor-independent**:

```text
src/host/workspace_host.cpp:521-526
    Impl::serve() is `shutdown_cv_.wait(lock, [this]{ return shutdown_requested_; });`
    # no idle timeout, no supervisor-liveness dependency
```

Shutdown triggers today are only `host.shutdown` (RPC), SIGTERM/SIGINT/SIGHUP,
and a startup failure (`workspace_host.cpp:484`, `:629-643`, `:467-468`). A
supervisor that exits simply closes its sockets (`src/ui/supervisor.cpp:117-122`)
and never deregisters, never prompts, and never signals a daemon. The result is
exactly the state the user rejects: **daemons with no supervisor remain running
and accumulating sessions.** The heartbeat and lease-renewal timers swallow
exceptions and never exit (`workspace_host.cpp:645-660`, `:662-677`), so nothing
in the current code can detect a dead supervisor.

At the same time, the pieces needed for a *collective* model are already partly
present and were never connected:

| Existing capability | Where | Gap |
|---|---|---|
| Multi-workspace attach at supervisor start | `src/cli/cli.cpp:380-390` | one-time snapshot; no continuous discovery |
| `HostNoticeKind::SessionCreated` on the wire | `protocol.hpp:211`, `protocol.cpp:55`, `ui_event_adapter.cpp:332` | **never emitted by the daemon** (only `SessionClosed`, and only to already-subscribed clients: `protocol_server.cpp:768-773`) |
| Per-session `event.subscribe` fan-out | `protocol_server.cpp:726-743` | a client cannot subscribe to a session it has not learned about |
| `ClientInstanceId` supersede | `protocol_server.cpp:315-327` | minted from a **shared** file (`supervisor.cpp:40-60`), so two concurrent supervisors collide |
| `host.status.attached_clients` | `protocol.hpp:248`, `protocol_server.cpp:707` | diagnostics only; no role breakdown, never used for lifecycle |

### 1.2 What this spec changes, in one sentence

**A daemon is alive only while it is owned, and ownership is the collective of
live supervisors plus live attached clients; the last supervisor's clean exit
prompts and tears the daemons down, and every crash path is backstopped by a
daemon-side ownerless watchdog.**

### 1.3 Supersession map

Disposition legend: **superseded** = the old text is wrong and is replaced;
**amended** = the old text survives in part and gains/replaces a clause;
**retained** = quoted here only to record that it still holds.

#### 1.3.1 Superseded

| # | Superseded clause (file:line) | New text (this spec) |
|---|---|---|
| **X1** | 04 §3.2 `:269` — *"record child pid; do NOT wait (the daemon outlives this supervisor)"* and 04 §3.2 `:293-296` — *"the daemon is not a child to be reaped; it is a peer that outlives the supervisor (H8, §54 D23)"* | The parent still never **blocks** in `waitpid` (H13/§3.3 unchanged). But two claims are **false** under the new model: (a) "the daemon outlives this supervisor" — its lifetime is bounded by `K(d)` (§2.4), and a clean exit may terminate it; (b) "not a child to be reaped" — the supervisor is the `fork` parent and **does** reap a spawned daemon that exits first, via its SIGCHLD handler (C-L1, §7.3). A crash terminates the daemon after the watchdog grace (§5). |
| **X2** | 04 §6.5 `:785-792` — *"When the TUI exits … the daemon observes a client detach and continues. … The daemon exits **only** on `host.shutdown`, SIGTERM, or SIGINT (§3.5); zero attached clients is not a trigger."* | Replaced by §5.1: zero *live owners* for `owner_grace` **is** a shutdown trigger (`ShutdownReason::NoOwners`). §3.5's ordered graceful shutdown is retained as the teardown mechanism. |
| **X3** | 04 §10 **H8** `:1022-1024` — *"The daemon survives detach and TUI exit. Zero attached clients is a normal state; the daemon never exits because a supervisor left."* | **H8 is void.** Its replacement is **O1**: a daemon must always have an owner; it self-terminates when it has none (§5). |
| **X4** | 00 §54 **D23** `:4828-4830` — *"A workspace daemon survives TUI detach."* | **D23 is void.** Replacement decision **16-D1**: *"A workspace daemon's lifetime is the lifetime of its ownership set; it does not survive the departure of the last owner."* Detach of a *session* (Ctrl+W, §9.8) is unaffected — see §1.4. |
| **X5** | 11 §8.2 **D20.3** `:939-945` — *"`ClientInstanceId` … Minted once per supervisor process (UUIDv4) and persisted supervisor-local at `<state>/ymh/supervisor.json` … On restart the same id is reused so supersede works"* | **Superseded by 16-D8 (§7.5):** the id is minted **per process, in memory**, and never persisted. The shared file returns the same id to two concurrent supervisors (`supervisor.cpp:40-60`), which makes the daemon's supersede logic (`protocol_server.cpp:315-327`) drop the other supervisor's live connection (O17/O19). Per-process minting preserves supersede — it is only needed for a reconnect **within** one process — and eliminates the collision. |
| **X6** | 04 §1.2 `:71-72` — *"**Attach / detach** — multiple supervisors may attach; detach does **not** terminate the daemon; the daemon survives TUI exit (§54 D23, §9.8)."* | **Superseded by A12/O1.** Multiple supervisors still attach, but detach terminates the daemon when it is the last owner, and the daemon no longer survives TUI exit unconditionally. Replace with: *"multiple supervisors may attach; detach does not terminate the daemon **while another owner remains**; the daemon's lifetime is the ownership set (§2)."* |
| **X7** | 00 §9.9 `:1103-1104` — *"The daemon itself never suspends: it survives TUI detach and keeps any in-flight session running (D23)."* | **Superseded by A6/O1.** The daemon keeps in-flight sessions running **while owned**; when ownerless for `owner_grace` it stops, cancelling in-flight turns after `shutdown_grace` (OQ-2). "Never suspends" is retained only for the owned case. |

#### 1.3.2 Amended

| # | Amended clause (file:line) | Amendment |
|---|---|---|
| **A1** | 04 §10 **H9** `:1026-1028` — *"Only `host.shutdown`, SIGTERM, or SIGINT stops the daemon; detach only removes a client. Detach never emits `SessionEnded`."* | Add the watchdog trigger: *"…or the ownerless watchdog (§5). `host.detach` and a client disconnect still never emit `SessionEnded`; the watchdog's teardown is a daemon shutdown, which does close sessions (04 §3.5 step 2) but is **not** a session `Detach`."* The session-level detach/delete separation (§9.8) is retained. |
| **A2** | 04 §4.2 `:526-528` — `attachedClients()` comment *"Zero is a normal, long-lived state…"* | Comment replaced: *"Zero live **owners** is a transient state terminated by the watchdog after `owner_grace` (§5). Zero **attached clients** while an owner row is fresh is normal and long-lived."* |
| **A3** | 04 §14.1(f) `:1368-1369` — *"Detach never stops the daemon; only `host.shutdown`/SIGTERM/SIGINT do."* | *"Detach never stops the daemon while another owner remains; the last owner's exit (§4) or the ownerless watchdog (§5) does."* |
| **A4** | 00 §9.8 `:1055` — Detach row *"TUI detaches from the daemon. The daemon SURVIVES…"* | `Ctrl+W` remains a **session** detach (the daemon survives; re-attach returns current state). It is distinct from supervisor **process exit**, which follows §4. |
| **A5** | 00 §9.8 `:1063-1067` — *"Detach means 'the TUI disconnects', not 'the session stops' (D23). … only shutdown reaps the daemon."* | Session detach keeps its meaning; the *daemon* is reaped by shutdown **or** the last-owner exit **or** the watchdog (§4, §5). D23 is void per X4. |
| **A6** | 00 §9.9 `:1090-1092` — *"a workspace runs its active session whenever it has pending work, regardless of whether any Supervisor is attached or focused"* | Insert **while owned**: *"…whenever it has pending work **and the workspace is owned** (§2.4); focus is irrelevant."* An ownerless workspace stops after `owner_grace`. |
| **A7** | 10 §2.1 `:149-154` — `DaemonStatus::Detached` comment *"daemon survives (§54 D23)"* | Comment becomes *"connection closed while the supervisor still runs; the daemon survives only while another owner remains (§2.4)"*. `DaemonStatus` gains `Stopping` (§7.6). |
| **A8** | 11 §8.2 **D20.3** (`ClientInstanceId` persistence) | **Superseded — see X5** (per-process mint, 16-D8). |
| **A9** | 03 §5.3 `:554-561` (row-domain table) | Add a fourth writer row: **Supervisor** may mutate its own `supervisors` row (register/heartbeat/deregister) and must never mutate another supervisor's row. See §7.2. |
| **A10** | 03 §3.3 `:234-284` / 00 §9.10 `:1189-1232` (DDL) | **Additive** table `supervisors` (§7.1). The four existing tables are byte-identical. |
| **A11** | 03 §3.4 `:313` — `SCHEMA_VERSION = 1` | `SCHEMA_VERSION = 2`; migration 1→2 is additive (§7.2). |
| **A12** | 04 §6.6 `:794-803` — multiple supervisors | Promoted from "N supervisors × one daemon fan out" to the **primary** model: N supervisors × M daemons, every supervisor attached to every live daemon, collective ownership (§2.2, §3). |
| **A13** | 05 **T-F21** `:1351` — *"Half-open connection \| no frame for `idle_timeout` \| close the connection (detach); daemon continues (§4.5)"* | Amend the tail: *"…close the connection (detach); the daemon continues **while it remains owned**; if that was the last owner, the owner watchdog stops it after `owner_grace` (§5.1)."* The close behavior itself is unchanged (R10). |
| **A14** | 05 §14.5 `:1503-1505` (live test) — *"Kill and restart the TUI while the daemon survives; assert the daemon is still alive and the reconnecting supervisor resumes each session via cursor with no visible gap (04 §6.5, T8)."* | Rewrite the scenario: with a **second** supervisor attached, the daemon survives and the reconnecting supervisor resumes; with **no** other owner, the daemon is torn down by the last-exit/watchdog and the test asserts that instead. §8.4.3 lists the tests to rewrite (O-L6). |
| **A15** | 05 §7 / `include/ymh/transport/host.hpp:57-63` (`TransportHost`) | **Additive:** `freshOwnerSnapshot()` returns the daemon's published fresh-id snapshot as `std::shared_ptr<const std::vector<protocol::ClientInstanceId>>`; `requestShutdown(std::string)` (`:63`) becomes `requestShutdown(protocol::ShutdownReason)`. `protocol::ShutdownReason` is a new transport mirror in `protocol.hpp` (N-M3/N2-M2, §7.4). |
| **A16** | 05 §7.2 `:818-821` — *"`host.shutdown` requests the 04 §3.5 graceful path … The daemon replies **before** it stops accepting, then sends `HostNotice{DaemonShuttingDown}` and closes (T-F19)."* | **Conditional now (L6):** `host.shutdown` is admitted only when the daemon has no other observable owner, or when the reason is the §4.6 `workspace_stop` override; otherwise it is refused with `AppCode::NotLastOwner` and the daemon stays `Serving` (O12, §4.4). The reply-before-stop ordering (T-F19) is unchanged for an accepted request. |

#### 1.3.3 Retained (recorded, not changed)

| # | Clause | Why it still holds |
|---|---|---|
| **R1** | 04 H10 `:1030-1032` — liveness is lock-primary | Daemon liveness (is the process serving this workspace?) is still the sidecar flock. Ownership is a *second*, orthogonal question: is anyone keeping it? |
| **R2** | 04 H11 `:1034-1036` — never kill on `ps` evidence | Retained. The new teardown signals a daemon only through `host.shutdown`/SIGTERM, never from a process scan. |
| **R3** | 04 H12 `:1038-1040` — graceful shutdown ordered/bounded | Retained and reused verbatim for the watchdog and last-exit teardown (§4.3). |
| **R4** | 04 H13 `:1042-1043` — signal policy (SIGHUP ignored) | Retained. The watchdog is **not** a signal; it is an in-process timer. |
| **R5** | 03 R5/§6.3 `:597-651` — sidecar-flock liveness, `kill` hint | Retained for host claims. Ownership rows use their own freshness rule (§2.3) and never clear a host claim. |
| **R6** | 03 D22/§5.1-§5.2 `:511-552` — single writer, WAL readers | Retained. Supervisor presence writes serialize through the same `flock(LOCK_EX)`; daemons read the `supervisors` table as WAL readers. |
| **R7** | 00 §9.9 decision (m) / 04 §6.6 `:799-803` — one active session per workspace, daemon-serialized | Retained. Switching supervisors never transfers or activates a session (§3.5). |
| **R8** | 03 §3.5 `:356` / 10 §2.1 `:161-164` — focused/active session is supervisor-local | Retained. Focus remains supervisor-local state, never registry state (see §3.5). |
| **R9** | 01/02 — sessions are event-sourced; the append-only log is the source of truth | Retained. Teardown flushes/closes sessions through 02/01, never rewrites history. |
| **R10** | 05 — length-prefixed JSON-RPC over a Unix socket, Interactive/Automation profiles | Retained; §7.4 adds methods only. |

### 1.4 Scope boundaries

In scope (from item 4, plus the minimal UI its semantics require):

1. The ownership model and its registry representation (§2).
2. Cross-supervisor visibility and switching (§3).
3. The last-supervisor-exit prompt and teardown (§4).
4. Orphan prevention for every crash/failure path (§5).
5. Races and their atomic resolutions (§6).
6. Interfaces, the additive registry migration, and the `ClientInstanceId`
   per-process fix (§7).
7. Invariants, failure modes, dsh mapping, test plan (§8).
8. Open questions (§9).

**Out of scope** (triaged separately in the backlog, per the lead):

- The bottom status-line reformat (`N active · M waiting` instead of listing
  sessions) — item 11.
- The current-session name in the top line — item 10.
- `/skills`, `/context`, `/export`, `<tab>` completion, `config.jsonc`, and the
  transcript style items 1–3, 5–9.
- Remote/SSH/TCP transport (00 §47 Mode B) — the ownership model is
  transport-agnostic; a remote supervisor is just another row.

The **one** UI exception, required by item 4's own semantics, is specified in
§7.6: the last-exit confirmation modal, a `Stopping` daemon status, and the
cross-supervisor "another session exists" surfacing that switching depends on.

### 1.5 Terminology

| Term | Definition |
|---|---|
| **Supervisor** | An interactive `ymh` process (the FTXUI TUI) that owns the daemon set. Exactly one per OS process. Registers one `supervisors` row (§2.3). |
| **Automation client** | `ymh run "<task>"` when a live daemon exists (`11 §10.2 :1049-1057`). It attaches, runs, and disconnects. It **never** registers and never prompts. |
| **Observer** | `ymh workspace stop` (and any future read-only attach). Never registers, owns nothing, and is **not** counted by `K(d)` (R-M1, §4.6). |
| **Owner** | A client that keeps a daemon alive: a live `Supervisor`/`Automation` connection, or a fresh `supervisors` row — exactly the `K(d)` predicate (§2.4). **Ownership of record** = a fresh `supervisors` row (interactive supervisors only). |
| **Daemon** | A `WorkspaceHost` process for exactly one workspace. |
| **Daemon set** | Every workspace whose host claim is live (`host_pid IS NOT NULL` and the sidecar lock is held). |
| **Keepalive** | The predicate `K(d)` (§2.4) that decides whether daemon `d` may continue running. |
| **Ownerless** | `K(d)` has been false continuously for `owner_grace`. |
| **Orphaning set** | The daemons that would become ownerless if *this* supervisor left (§4.1). |

---

## 2. The ownership model

### 2.1 Decision 16-D1 — daemons do not outlive their owners

**16-D1 (pinned).** A `WorkspaceHost` daemon exists only to serve owners. It is
spawned by a supervisor, it serves that supervisor and any peers, and it
terminates when it has no live owner. This replaces 00 §54 D23 and 04 H8.

This is deliberately **collective**, not per-daemon. There is no "this daemon
belongs to supervisor A" assignment. Ownership is a property of the machine's
supervisor set: if any supervisor is live, every live daemon is owned. The user's
requirement makes this explicit — *"another supervisor can manage both
daemons"* — and it matches the existing multi-workspace attach
(`cli.cpp:380-390`), which already makes every supervisor aware of every live
daemon.

### 2.2 Why collective, and not per-daemon ownership

A per-daemon owner set would require choosing an owner at spawn time, and the
user's scenario contradicts every such choice:

- The daemon must survive the supervisor that spawned it, because a *second*
  supervisor is attached to it (`"another supervisor can manage both daemons"`).
- The daemon must die when the last supervisor leaves, regardless of which
  supervisor spawned it.
- Either supervisor must be able to drive any daemon, so ownership cannot gate
  RPC dispatch.

Therefore the only stable model is: **the daemon's lifetime is a function of the
machine's live supervisor set**, and any supervisor may use any daemon. The
`supervisors` table is that set.

### 2.3 The ownership set — `supervisors` table

Ownership lives in the shared `registry.db`, in a new additive table. It must
live in a **shared, durable** place, not in daemon memory, for three reasons:

1. **Crash survival.** A supervisor killed with SIGKILL cannot tell the daemon
   it left; the row's staleness is the signal (§5).
2. **Cross-supervisor discovery.** A supervisor must enumerate its peers to
   answer "am I the last?" and to display them (§3, §4).
3. **Atomic last-exit.** The registry's single-writer `flock` (03 D22) is the
   only cross-process mutex available; the last-exit decision is serialized
   through it (§6).

The daemon additionally tracks its **live attached connections** in memory. The
two views are combined by the keepalive predicate (§2.4); neither alone is
sufficient (§5.3 explains why).

Schema (pinned; migration in §7.2):

```sql
CREATE TABLE supervisors (
    id          TEXT PRIMARY KEY,   -- SupervisorId == ClientInstanceId (UUIDv4)
    pid         INTEGER NOT NULL,   -- getpid() of the supervisor process
    boot_id     TEXT NOT NULL,      -- per-process boot nonce (UUIDv4)
    started_at  INTEGER NOT NULL,   -- epoch ms, first registration
    heartbeat   INTEGER NOT NULL,   -- epoch ms, refreshed every heartbeat_interval
    tty         TEXT,               -- display only, e.g. "pts/3"; NULL allowed
    CHECK (pid > 0)
);

CREATE INDEX idx_supervisors_heartbeat ON supervisors(heartbeat);
```

- The table is **global**, not per-workspace: collective ownership (§2.2).
- There is no foreign key to `workspaces`: a supervisor owns the whole daemon
  set, and a supervisor may exist before any daemon does.
- `boot_id` is the supervisor's per-process nonce, used to distinguish a
  restarted supervisor that reuses a PID from the one that died (§6.5).
- `heartbeat` is epoch ms, like `workspaces.host_heartbeat` (03 §6.2).

### 2.4 The keepalive predicate

**Two clocks, never mixed (16-D13; fixes O-H1/C-M1).** Interval arithmetic
(`LIVE_CONN`'s frame age, the watchdog's grace) uses the **monotonic** clock `m`
(`std::chrono::steady_clock`). The durable heartbeat is an **epoch-ms wall**
value, so `FRESH_OWNER` uses the **wall** clock `w`
(`std::chrono::system_clock`). The two are never subtracted from each other; a
`steady_clock` value has no epoch and subtracting it from an epoch-ms field
yields a huge negative delta (which would make every row look fresh — the exact
bug O-H1 reports).

For a daemon `d`, with its connection table `C(d)` and the registry's supervisor
rows `S`:

```text
# m = monotonic (steady_clock); w = wall epoch ms (system_clock)
LIVE_CONN(d, m) :=
    ∃ c ∈ C(d) :
        c.hello_done
        ∧ c.role ∈ {Supervisor, Automation}
        ∧ ¬c.dropped ∧ ¬c.close_when_drained
        ∧ (m − c.last_frame_at) ≤ limits.idle_timeout          # 30 s, 05 T-F21

FRESH_OWNER(w) :=
    ∃ s ∈ S :
        let delta := w − s.heartbeat                          # both epoch ms
        in 0 ≤ delta ≤ owner_lease_ttl                        # negative ⇒ STALE

K(d, m, w) := LIVE_CONN(d, m) ∨ FRESH_OWNER(w)
```

- **`delta < 0` is stale, not fresh.** A backward wall-clock jump (NTP step,
  manual `date`) or a row stamped by a process with a clock ahead of ours yields
  `delta < 0`. The predicate treats that as **not fresh**, so the failure
  direction is toward termination, never toward a **persistent** phantom owner: a
  forward wall-clock step can transiently refresh a row, but once `w` settles the
  row ages out normally (O1/O15).
- **`delta == 0` is fresh** and `delta == owner_lease_ttl` is fresh (inclusive
  bound); `delta > owner_lease_ttl` is stale.
- `LIVE_CONN` never consults the wall clock, so a wall jump cannot keep a
  connection alive or kill one; the transport's own monotonic idle timer
  (`transport_server.cpp:85-97`) is unchanged (R10).

`K` is **true when either view says an owner exists** (an OR, not an AND). This
polarity is load-bearing and is defended in §5.3:

- **Connection-only would be unsafe** across a supervisor restart: a supervisor
  that exits and restarts leaves a window with no connection, and the daemon
  must not die in that window.
- **Registry-only would be unsafe** when the registry is unwritable or on a full
  disk: a live supervisor with a failing heartbeat would look dead, and the
  daemon would kill work that is still being driven.
- **OR** makes each mechanism a fallback for the other. A daemon dies only when
  *both* say "nobody is here", which is precisely the orphan condition.

Constants (pinned here; identical in §5.2 and §7.3 — see O-L7):
`owner_heartbeat_interval = 5 s`, `owner_lease_ttl = 15 s`
(`= 3 × owner_heartbeat_interval`, mirroring 03 §6.2's three-missed-tick rule),
`watchdog_interval = 2 s`, `owner_grace = 5 s`. The heartbeat cadence is
**independent** of the transport `kPingInterval` (10 s, `protocol.hpp:196`): the
ping keeps the socket alive, the heartbeat keeps the durable row fresh, and one
supervisor timer may drive both.

### 2.5 Daemon lifetime

```text
WorkspaceHost::run()
  1'. OWNERSHIP GUARD (first, before any state — L3):
        if (!config.foreground
            && !(config.require_owner && !config.watchdog_disabled))
            -> HostExitCode::StartupRejected        # fail-loud guard (N3-H1/G1)
        # tests only: non-foreground + require_owner=true + watchdog_disabled=true
        # is ALSO rejected (the rev-3 guard tested only require_owner).
  … §3.3 startup unchanged (chdir → store → claimHost → heartbeat) …
  9'. if (config.require_owner && !config.watchdog_disabled) arm the owner watchdog
 10. serve until a shutdown trigger:
       (a) host.shutdown              -> ShutdownReason::ClientRequest | LastSupervisor
       (b) SIGTERM / SIGINT           -> ShutdownReason::Signal
       (c) watchdog: K(d) false for owner_grace -> ShutdownReason::NoOwners
       (d) startup failure            -> ShutdownReason::StartupFailure
     then the §3.5 / 11 §11.3 ordered teardown
```

`owner_grace` (default **5 s**) is the hysteresis that absorbs a supervisor
restart and a transient reconnect. It is deliberately short: the user's
requirement is that unsupervised daemons *do not exist*, so a long grace would
be a long unsupervised window.

### 2.6 Supervisor lifetime

```text
ymh (no args)
  1. mint a per-process ClientInstanceId (16-D8, §7.5)   # NOT the shared file
  2. open the registry; registerSupervisor(row)          # idempotent by id
  3. discover the daemon set (all live host claims)      # §3.2
  4. attach to every live daemon with (my_id, Supervisor) # C-H2 identity
     cwd workspace shown as "not running" if no live daemon
  5. start: presence heartbeat (5 s), daemon-set scan (2 s), ping (10 s)
  6. run the TUI
  7. on exit: the last-exit flow (§4)
```

- Registration happens **before** any daemon is spawned, so a daemon spawned by
  this supervisor is immediately covered by `FRESH_OWNER` even before the socket
  handshake completes.
- **Every attach carries `(ClientInstanceId, ClientRole)`** (C-H2). The
  supervisor's identity is the same value as its `supervisors.id`, so the daemon
  can exclude it from `other_fresh_owners` and the last-exit prompt can fire
  (§4.1, §4.4). `ymh run` sends `ClientRole::Automation`; the supervisor sends
  `ClientRole::Supervisor`.
- **Re-register on a false heartbeat (C-M8).** A supervisor frozen (SIGSTOP)
  longer than `owner_lease_ttl` has its row pruned by a peer's scan
  (`pruneStaleSupervisors`, §5.4), and its socket closed at `idle_timeout`
  (30 s). On the next heartbeat tick, `heartbeatSupervisor` returns `false`; the
  supervisor **re-registers its row and reconnects every dropped daemon
  immediately** (not on the next 2 s scan). This makes O-F2 recoverable: a
  resumed supervisor regains ownership instead of racing a last-exit. A
  re-registration that observes the daemon already gone simply re-enters
  `ensureRunning` (spawn) on the next prompt.
- The supervisor **does not eagerly spawn** the cwd daemon (16-D2, §3.2). It
  spawns on the first prompt or an explicit activation, matching the user's
  narrative (*"issued a first prompt. It caused a new daemon to spawn"*). This
  also removes the current startup stall where `ensureRunning` blocks the TUI
  before it can render (`cli.cpp:352`). The spawn seam is pinned in §7.7 (C-H4).

### 2.7 Clean exit vs crash vs SIGKILL

**Owner definition (resolves O-M8/C-M11).** "Owner" in this spec means a **live
client that keeps the daemon alive** — a live `Supervisor` **or** `Automation`
connection, or a fresh `supervisors` row. `Automation` is therefore a *keepalive
owner* but **not** an *ownership-of-record* supervisor: it never registers
(O6), never appears in the peer badge, and is excluded from the last-exit prompt
set. The requirement "unsupervised daemons must not exist" is scoped to
**interactive supervision**: an `ymh run` task is an intentional, bounded,
foreground user action that must not be killed mid-turn (O13), so it holds the
daemon for the duration of its connection. The precise guarantee is stated in O1
and O15.

| Event | Supervisor row | Connection | Daemon response |
|---|---|---|---|
| Clean exit, other owners exist | deregistered (after confirm) | closed | keeps serving; `LIVE_CONN` still true for peers |
| Clean exit, last owner, confirmed | deregistered (after confirm) | closed | prompted `host.shutdown{LastSupervisor}`; daemon admission-checks and stops (§4.4) |
| Clean exit, last owner, cancelled | **not deregistered** (no state change) | stays open | keeps serving; `K(d)` stays true (§4.2) |
| `SIGKILL` | **stale** (row remains) | kernel EOF immediately | `LIVE_CONN` false; row stale after `owner_lease_ttl`; watchdog stops after `owner_grace` |
| `SIGSTOP` (frozen) | row stops refreshing, pruned at `owner_lease_ttl` | socket stays open but silent; `idle_timeout` (30 s) closes it | `K(d)` false ⇒ watchdog stops after `owner_grace`; on resume the supervisor **re-registers and reconnects** (§2.6, C-M8) |
| `SIGTERM` | stale (no handler) | kernel EOF | same as `SIGKILL` |
| Machine reboot | stale | none | no daemon survives the reboot; rows pruned lazily (§5.4) |
| `ymh run` disconnects, no other owner | n/a (never registered) | closed | `K(d)` false ⇒ watchdog stops after `owner_grace` (§4.5) |

**O1 — No unsupervised daemon.** There is no sequence of interactive-supervisor
exits, crashes, or signals after which a `WorkspaceHost` process serves without
an owner for longer than the worst case in §5.2. "Owner" is as defined above: a
live `Supervisor`/`Automation` connection or a fresh registry row. An
`Automation` client is a bounded, intentional hold, not an unsupervised state;
its own disconnect starts the watchdog (§4.5).

**O2 — Collective ownership.** Any live supervisor may drive any live daemon;
ownership never gates RPC dispatch. A supervisor's departure affects a daemon
only through the keepalive predicate, never through a per-daemon grant.

**O3 — Durable ownership.** The ownership set is a `supervisors` table in
`registry.db`, written under the 03 D22 `flock`, readable by daemons as WAL
readers. It survives any single process's death.

**O4 — Kernel-mediated fast path.** A live socket connection is sufficient
evidence of an owner; the daemon never needs a registry read to know a connected
owner exists. The registry is the durable fallback, not the primary signal.

**O5 — Watchdog arming is fail-safe.** The watchdog is active iff
`HostConfig.require_owner && !HostConfig.watchdog_disabled`. `require_owner`
defaults **true** (fail-closed); a non-`foreground` daemon is rejected at startup
unless `require_owner && !watchdog_disabled` (N3-H1/G1). In-process
(`foreground == true`) tests opt out explicitly. This is a test seam, not a
production mode (R-M2, §7.3).

**O6 — Automation is not ownership of record.** `ymh run` attaches with role
`Automation`, never registers, and never prompts. It keeps a daemon alive only
for the duration of its connection (§2.4).

### 2.8 Worked example — the user's scenario

Two repositories, `A` and `B`. The user runs `ymh` in `A` (supervisor `S1`),
submits a prompt; the cwd daemon `D_A` spawns. Then runs `ymh` in `B`
(supervisor `S2`), submits a prompt; `D_B` spawns.

```text
t0  S1 registers (row S1). Daemon set = {} (no live claims).
t1  S1's first prompt -> ensureRunning(A) -> spawn D_A.
      D_A startup: claimHost(A); watchdog armed; serves.
      S1 attaches to D_A.            K(D_A) = LIVE_CONN(S1) ∨ FRESH_OWNER(S1) = true
t2  S1 scan tick: no new daemons.
t3  S2 registers (row S2). S2 scan finds D_A live -> attaches. S2's cwd B has no
      daemon -> shown as "not running".
t4  S2's first prompt -> ensureRunning(B) -> spawn D_B. S2 attaches to D_B.
      S1 scan tick finds D_B live -> attaches.   # both supervisors now on both
t5  S1 displays: workspaces {A: D_A, B: D_B}; sessions: {A: s1, B: s2}.
    S2 displays the same. Each sees the other's session via SessionCreated (§3.3).
t6  S1 quits. It is not the last (S2's row is fresh, S2 is connected to both):
      no prompt; S1 deregisters, closes sockets.
      K(D_A) = LIVE_CONN(S2) ∨ FRESH_OWNER(S2) = true; K(D_B) = true. Both live.
t7  S2 quits. It is the last owner of both:
      prompt: "Exiting will terminate 2 workspace daemons (A, B). Continue? [y/N]"
      yes -> S2 deregisters, sends host.shutdown{LastSupervisor} to D_A and D_B.
      Each daemon re-checks (no other fresh row, no other live owner), then runs
      the ordered teardown. S2 waits (bounded), closes, exits.
```

If S2 is instead `kill -9`'d at t7: `LIVE_CONN` goes false immediately (AF_UNIX
EOF), `FRESH_OWNER` goes false at `owner_lease_ttl`, and each daemon's watchdog
requests shutdown within the §5.2 bound (**23 s** for a death, **38 s** for a
freeze) and then runs the ordered teardown.

---

## 3. Cross-supervisor visibility and switching

### 3.1 Supervisor scope is the daemon set, not the cwd

Today a supervisor's scope is a one-time snapshot: `ensureRunning(cwd)` plus
every *other* registered workspace with a live daemon
(`cli.cpp:352`, `cli.cpp:380-390`). That snapshot satisfies "both supervisors
display both daemons" only if both daemons already existed when the second
supervisor started. The user's scenario creates `D_B` *after* `S1` is running, so
a snapshot is insufficient.

**16-D3 (pinned).** A supervisor attaches to **every** live daemon in the daemon
set and keeps that set current with a periodic scan. The cwd workspace is merely
the initially focused one; it is not privileged for ownership.

**O7 — Scope completeness.** At any time after its first scan tick, a supervisor
is attached to every daemon whose host claim was live at the previous tick. A
daemon that appears between ticks is attached within one `scan_interval`.

### 3.2 Discovery

Discovery is a read-only registry poll. It reuses the existing liveness
primitive (`registry.probeLiveness`, `workspace_host.cpp:868`) and the existing
attach path; it does not add a registry table.

```text
DaemonSetScanner (supervisor-side, owns its own thread)
  every scan_interval (default 2 s):
    rows := registry.openReadOnly().listWorkspaces()      # 03 §4.3
    for r in rows where r.host.has_value():
        if probeLiveness(r.id) != Live:      # sidecar flock absent -> stale
            drop the connection if attached; mark DaemonStatus::Dead
            continue
        if not attached(r.id):
            attach via SupervisorConnection(r.host.socket, r.host.boot_id,
                                            options.identity)   # C-H2
            # identity cross-check unchanged (11 §8.2 D20.7)
```

- **Read-only.** The scan opens the registry read-only (03 §4.3, 11 E15) and
  never writes. It never takes the `flock`; WAL readers never block (03 §5.2).
  It works on an un-migrated v1 DB: the `supervisors` table is simply absent and
  freshness queries return zero (§7.2, C-M9).
- **No spawning in the scan.** The scan only attaches to daemons that already
  exist. Spawning is a user action (§3.2.1).
- **Bounded but not free (C-L3).** `scan_interval` (2 s) is supervisor-local; the
  scan is O(workspaces) and reuses one open read transaction — it is not a
  filesystem walk. But `probeLiveness` opens and `flock`s the sidecar lock per
  workspace (`src/registry/registry.cpp:869`), so each tick costs O(workspaces)
  open/flock syscalls. For the expected open set (tens of workspaces) this is
  negligible; if it grows, cache `probeLiveness` per workspace and re-probe only
  those whose `host_heartbeat` moved.
- **Convergence with the watchdog.** A daemon that is being torn down
  (Draining) refuses new handshakes (`AppCode::ShutdownInProgress`,
  `protocol_server.cpp:307-312`); the scanner records `Stopping` and retries on
  the next tick, by which time the claim is gone (reaped) or the daemon has
  restarted.

#### 3.2.1 Lazy spawn (16-D2)

**16-D2 (pinned).** The supervisor does **not** spawn a daemon at startup. It
spawns the cwd workspace's daemon on the first of:

- a submitted prompt (`agent.prompt`/`agent.followup`) to that workspace, or
- an explicit `session.create` / workspace activation.

Until then the workspace is listed as **`NotRunning`** and is browsable
read-only. This supersedes the eager `ensureRunning` at
`cli.cpp:352`/`11 §10.2 :1045` and matches the user's narrative. Rationale:

- It removes the startup stall (a blocking 10 s readiness poll,
  `workspace_host.cpp:877-891`) before the TUI can render.
- It removes the most common orphan source: a daemon spawned by a supervisor
  that then exits without ever using it.
- It makes the ownership window explicit — a daemon exists only because a prompt
  needed it.

When the first prompt arrives, the spawn path is the existing
`HostLifecycle::ensureRunning` (`workspace_host.cpp:917-935`) unchanged: attach
if live, else reap a stale claim and spawn. `ensureRunning` still never kills a
process (04 H11, R2).

### 3.3 Session visibility

Both supervisors must display each other's sessions. Two mechanisms are needed:
an initial list and a live announcement.

**Initial list.** On attach, the supervisor calls `session.list`
(`protocol.hpp:424`, `protocol_server.cpp:374-379`) and builds a `WorkspaceModel`
per session. This already works.

**Live announcement — two missing halves (C-H1/O-M5).** `HostNoticeKind::
SessionCreated` exists on the wire (`protocol.hpp:211`, wire name
`"session_created"`, `protocol.cpp:55`) and the adapter maps it
(`ui_event_adapter.cpp:332-337`), but the path is broken at **both ends**:

1. **The daemon never emits it.** The dispatch handles
   `session.create`/`session.fork` without broadcasting
   (`protocol_server.cpp:382-399`), and `onSessionClosed` emits `SessionClosed`
   **only to clients already subscribed to that session**
   (`protocol_server.cpp:768-773`). A second supervisor never learns of a session
   it has not subscribed to. Fixed by 16-D4 below.
2. **The UI drops it.** `WorkspaceEvent` carries no `SessionId`
   (`ui_event.hpp:58-62`) and `UiModel::apply` no-ops `SessionOpened`/
   `SessionClosed` with `break;` (`ui_model.cpp:610-611`). Even with the emission
   fixed, no session cell would appear. Fixed by §7.6.

Both halves are required; the daemon-side fix alone is insufficient and would
leave O8 false.

**16-D4 (pinned).** The daemon broadcasts `host.event` with
`HostNotice{SessionCreated}` to **every** hello-complete `Interactive` client
after a successful `session.create`, `session.fork`, and `session.resume`, and
broadcasts `HostNotice{SessionClosed}` to **every** hello-complete `Interactive`
client on close/delete (not only to subscribers).

```cpp
// include/ymh/transport/protocol_server.hpp (additive)
// Broadcast a workspace-scoped lifecycle notice to every hello-complete
// Interactive client. Callable only on the io thread (like onEventCommitted).
void onSessionCreated(const SessionId& session);
```

- The emission is a **new call**, not a new notification kind: `SessionCreated`
  is already in `HostNoticeKind`. No wire-format change beyond actually sending
  it.
- It is **not** gated on subscription. `host.event` is a workspace-scoped
  broadcast (`protocol_server.cpp:768-773`), distinct from the per-session
  `event.stream` (`notify::kEventStream`, `protocol.hpp:462`).
- The existing per-session `SessionClosed` unsubscribe notices
  (`notify::kEventUnsubscribed`) are unchanged; the *broadcast* `host.event`
  `SessionClosed` is added alongside them.
- The supervisor's `UiEventAdapter` already keys notices idempotently by
  `Event.id`/session identity (10 §5.3), so a duplicate announcement (a client
  that was both subscribed and broadcast to) is harmless.
- **Automation** clients do not receive `host.event` (profile gate: only
  `Interactive`, as today at `protocol_server.cpp:768`). `ymh run` does not need
  cross-supervisor visibility.

**O8 — Live visibility.** A session created or closed in a daemon is announced
to every attached Interactive client within one io-thread dispatch, carried with
its `SessionId`, and rendered by `UiModel::apply` (insert/erase a session cell).
No client polls `session.list` to notice a change; a single `session.show` fills
the new cell's title (§7.6).

### 3.4 Switching — the widget already exists

`docs/design/UI_SURFACE_INVENTORY.md` (code-verified) records that the switcher
already builds a workspaces → sessions tree across **all** attached workspaces
(`SwitcherOverlayModel`, `ui_model.hpp:243-255`; `open()`, `ui_model.cpp:649-668`)
and that Enter already focuses any session (`supervisor.cpp:595-604` →
`UiModel::focusSession`, `ui_model.cpp:632-647`). The CLI already attaches every
registered workspace with a live daemon (`cli.cpp:365-393`), and each connection
populates its session cells via `session.list` (`supervisor.cpp:280-329`).

Therefore **spec 16 does not build a visibility widget.** It owns only the
*semantics* on top of the existing widget:

1. Discovery is continuous, not a startup snapshot (16-D3, §3.2).
2. New sessions are announced (16-D4, §3.3).
3. The switcher rows gain an ownership/attach marker (§3.6).
4. Optional `/attach` and `/switch` command entries are registered in
   `CommandRegistry::builtin()` (`command_registry.cpp:93-158`); the command
   handler stays a thin caller and adds **no** keybindings
   (inventory Surface 5; RB-08 owns generic Tab wiring).

Switching itself is a **supervisor-local focus change**, not a state transfer.

```text
switch(workspace_id, session_id):
  model_.activeWorkspaceId = workspace_id
  model_.workspaces[workspace_id].activeSessionId = session_id   # supervisor-local
  if not subscribed(workspace_id, session_id):
      event.subscribe{ session: session_id, from: Beginning }    # D20.5
      # replay renders the full history, then live events flow
  redraw
```

- **No registry write.** Focus is never persisted to `registry.db`
  (03 §3.5 table row "Focused/active session"; R8). This directly reconciles
  with `AGENTS.md`: *"the focused/active session is supervisor-local state, not
  registry state."*
- **No lease transfer, no activation.** Switching does not call
  `session.activate`/`session.suspend` and does not touch the daemon's
  single-active-session invariant (R7). A prompt submitted to a non-active
  session is queued as pending work; the daemon activates it per decision (m)
  (00 §9.9, 04 §6.6 `:799-803`).
- **Continue working.** Because the session is daemon-owned and event-sourced
  (R9), the second supervisor can submit prompts, cancel, or answer permission
  requests in a session the first supervisor created. Both observe the same
  `event.stream` fan-out (`protocol_server.cpp:726-743`).
- **First view replays from `beginning`.** Cursors are per-supervisor in memory
  and empty on restart (11 D20.4); `CursorInvalid` falls back to `beginning`,
  never `now` (11 D20.5), so a switch never silently loses history.

### 3.5 Reconciliation with one-active-session and supervisor-local focus

| Concern | Authority | This spec |
|---|---|---|
| Which session is active/running | the **daemon**, serialized, one per workspace (decision (m)) | unchanged; switching never changes it (R7) |
| Which session the UI shows | the **supervisor**, in `WorkspaceModel` | unchanged; never in the registry (R8) |
| Which supervisors own the daemon set | the **registry**, `supervisors` | new; orthogonal to focus |
| Which sessions exist | the **daemon**, via `session.list` + `SessionCreated`/`SessionClosed` | announcement added (16-D4) |

**O9 — Focus isolation.** Two supervisors may focus different sessions of the
same workspace simultaneously; neither switch alters the other's focus, the
daemon's active session, or any registry row.

### 3.6 UI surface seams (binds inventory C1–C5)

`docs/design/UI_SURFACE_INVENTORY.md` is authoritative for current UI behavior.
Spec 16 binds to it as follows:

| Constraint | Binding in this spec |
|---|---|
| **C1** — header right slot belongs to RB-10 (session name) | The optional peer-supervisor indicator is a **count badge in the left header segment** (after `"ymh · <cwd>"`, `render_header` `ui_render.cpp:364-372`) or in the switcher title — **never** after RB-10's name. |
| **C2** — "display that there is one more" is a count, not a list | The requirement is satisfied by the **existing aggregate counts** (`AggregateStatus`, `ui_model.hpp:257-262`, `ui_model.cpp:225-241`) which already span all attached workspaces, plus a `"N supervisors"` badge. No per-session list is added; RB-11's counts-only collapse is untouched. |
| **C3** — `/exit` and Ctrl+D become conditional | The last-exit logic hooks **inside/around `requestExit`** (`supervisor.cpp:237-242`). Ctrl+D (`supervisor.cpp:713-715`) and `/exit` via `context.request_exit` (`supervisor.cpp:452`) stay thin callers. |
| **C4** — extend, do not duplicate | `WorkspaceNode`/`SessionNode` gain an ownership/attach marker (`ui_model.hpp:229`; `render_switcher`, `ui_render.cpp:314-360`). No second visibility widget. |
| **C5** — count scope is not redefined | `AggregateStatus` already spans all attached workspaces; spec 16 only **extends "attached"** to mean "connected to a daemon in the owned daemon set", including daemons discovered after startup (§3.2). It never narrows the scope to the active workspace. |

The new switcher marker (C4) is display-only and supervisor-local. It is never
written to the registry:

```cpp
// include/ymh/ui/ui_model.hpp (additive to WorkspaceNode / SessionNode)
enum class OwnershipMark : std::uint8_t {
    Owned,          // a live daemon this supervisor is attached to
    NotRunning,     // workspace registered, no live daemon (16-D2)
    Stopping,       // daemon draining after last-exit / watchdog
    Unreachable,    // live claim but handshake failed (stale socket)
};
```

**O14 — No duplicate visibility UI.** Cross-supervisor visibility reuses the
switcher (`ui_model.cpp:649-668`) and the aggregate counts
(`ui_model.cpp:225-241`). Spec 16 introduces exactly one new overlay: the
last-exit prompt (§4.2). No new always-visible panel, and nothing in the header's
right slot.

---

## 4. Last-supervisor exit

### 4.1 The orphaning set

On an exit request (`Ctrl+D`, `q`, or `/exit`), the supervisor computes which
daemons would become ownerless if it left:

```text
orphaning_set :=
  # candidates (R-L1): every attached daemon PLUS every live-daemon workspace
  # this supervisor discovered but has not attached to yet (scanner lag).
  candidates := attached_daemons
              ∪ { ws : live_daemon(ws) ∧ ¬attached(ws)
                     ∧ my_registry_read.freshSupervisorCount(exclude = my_id) == 0 }
  for each candidate d:
      view := conn(d).request("host.ownership")   # bounded 2 s (C-L5)
      if view.live_supervisors == 1               # RAW count incl. me => "just me" (R-M3)
         and view.live_automation == 0            # no ymh run holding it
         and view.other_fresh_owners == 0         # snapshot − me (R-H1)
      then d ∈ orphaning_set
```

- `host.ownership` is a new read-only RPC (§7.4). It returns the daemon's live
  connection table by role as **raw counts including the caller** (R-M3) plus
  `other_fresh_owners = |snapshot − caller|` from the watchdog snapshot
  (§5.1). **No SQLite call happens on the io thread** (C-M7).
- `live_automation` is why `ymh run` matters: a daemon held by a running
  `ymh run` is **not** in the orphaning set, so the exiting supervisor neither
  prompts for it nor shuts it down. The automation client's own disconnect later
  triggers the watchdog if no owner remains.
- **Fresh-owner-only candidates (R-L1/N-L10).** The `supervisors` table is
  **global** (§2.3), not workspace-scoped, so there is no
  `fresh_owner_row(ws, …)` predicate. A daemon this supervisor owns but has not
  attached to is found from the scanner's live-daemon list; for such a candidate
  the supervisor uses its **own** registry read
  `freshSupervisorCount(exclude = my_id)` to confirm it is the sole fresh owner,
  and treats the view as `live_supervisors == 1 ∧ live_automation == 0`. This
  prevents a discovered-but-unattached daemon from being silently
  watchdog-killed. For an unattached candidate the supervisor **cannot** observe
  `live_automation` (there is no connection), so the prompt may over-list a
  daemon that a concurrent `ymh run` holds (N2-L6). This is safe: the prompt is
  advisory, and the daemon's admission check (§4.4) rejects the shutdown while a
  live Automation client exists. Probing would require attaching, a side effect
  the exit path must avoid.
- **Bounded exit query (C-L5).** Each `host.ownership` call uses a short
  `ownership_query_timeout` (default **2 s**), not the 30 s idle default
  (`host_connection.hpp:40`; `:37` is the 5 s handshake default). A
  hung/unresponsive daemon is skipped for the prompt; its own watchdog still
  stops it, and the supervisor's exit cannot block. The prompt appears within
  `|candidates| × 2 s` worst case.

**O10 — Orphaning set correctness.** A daemon is in the orphaning set iff,
holding the requester's departure fixed, `K(d)` is false for every other
possible owner. The set is computed from live state (connections) plus the
watchdog cache (rows), never from a stale startup snapshot.

### 4.2 Prompt semantics

**Exit ordering (16-D9, fixed for C-H3/O-M9).** The supervisor stays registered
until the user confirms. Nothing is written to the registry while the prompt is
open; cancelling leaves the supervisor fully registered and owning. The order is:

```text
query the orphaning set (read-only)      # still registered
  ├─ empty     -> deregister -> close -> exit
  └─ non-empty -> show the prompt
        ├─ cancel (n / N / Esc / Ctrl+C)  -> NO state change; stay registered
        ├─ no answer (EOF / closed terminal / SIGHUP)
        │                                 -> the process dies; its row goes stale
        │                                    and its sockets EOF; the watchdog
        │                                    backstops (§5)
        └─ confirm (y / Y / Enter / --yes)
                                          -> deregister -> host.shutdown -> close
```

This resolves C-H3/O-M9: the earlier `deregister → query → prompt` order left a
cancelling user **deregistered**, so a transient disconnect would make `K(d)`
false and the watchdog would kill their daemons. **No path leaves a live
supervisor deregistered.** §4.3, §6.2, §6.3 and 16-D9 are aligned to this order.

If the orphaning set is **empty**, the supervisor deregisters and exits silently;
daemons with other owners are unaffected.

If the set is **non-empty**, the supervisor shows a modal. There is no
confirmation UI anywhere in the TUI today (`requestExit`, `supervisor.cpp:237-242`
just sets `quit_`); spec 16 reuses the existing modal overlay pattern — the
permission dialog `render_dialog` (`ui_render.cpp:286-312`, an
`ftxui::window(...) | center`) composed as a `dbox` overlay in `build_ui`
(`ui_render.cpp:416-421`) — and hooks it **inside/around `requestExit`**. Ctrl+D
(`supervisor.cpp:713-715`) and `/exit` via `context.request_exit`
(`supervisor.cpp:452`) remain thin callers (inventory C3, Surface 4).

```text
┌ Exiting ─────────────────────────────────────────────────────────┐
│ Exiting will terminate 2 workspace daemons:                      │
│   • A  (/home/u/prjs/A)  — 1 session, turn running               │
│   • B  (/home/u/prjs/B)  — 2 sessions, idle                      │
│                                                                  │
│ In-flight turns will be cancelled. Sessions are saved; the       │
│ daemons restart automatically the next time you use them.        │
│                                                                  │
│                    [ Terminate and exit ]  [ Cancel ]            │
└──────────────────────────────────────────────────────────────────┘
```

Input handling (pinned):

| Input | Effect |
|---|---|
| `y` / `Y` / Enter on "Terminate" | proceed: deregister, then teardown (§4.3) |
| `n` / `N` / Esc / Enter on "Cancel" | cancel; **no registry write**, still registered, stay in the TUI |
| `Ctrl+C` (SIGINT) at the prompt | cancel; stay (SIGINT is not a quit in the TUI, `supervisor.cpp:717-720`) |
| EOF on stdin / terminal closed | **no graceful teardown**: the supervisor is already dying (SIGHUP). The daemon watchdog is the backstop (§5). This is deliberate — never interpret "cannot ask" as "yes". |
| `--yes` / `no_prompt` (16-D6) | skip the prompt; proceed as if confirmed. For scripts/tests only. |

- The modal is **supervisor-local UI**; it writes nothing to the registry until
  the user confirms. Only on confirm does the supervisor deregister (§4.3).
- The session/turn counts in the modal are display-only and may race; they are
  never used for a decision. Only the orphaning set is authoritative.
- **O11 — No accidental termination.** The daemon set is terminated only after
  an explicit affirmative answer, or via `--yes`. EOF, SIGINT, a closed
  terminal, and a timeout all mean "do not terminate here"; the watchdog then
  guarantees no orphan.

### 4.3 Teardown ordering

On confirmation (and **only** on confirmation), the supervisor tears down:

```text
1. deregisterSupervisor(my_id)              # AFTER the user confirmed (16-D9)
2. for each d in orphaning_set (bounded):
     conn(d).request("host.shutdown", {reason: "last_supervisor"},
                     timeout = teardown_grace)
     # The daemon admission-checks on the io thread BEFORE latching Draining
     # (§4.4). Accept -> reply + DaemonShuttingDown enqueued before stop (11 E19).
     # Reject -> AppCode::NotLastOwner; the supervisor closes and moves on.
3. wait for each daemon to close, bounded by teardown_grace (default 5 s);
   on grace expiry, close the socket anyway (never SIGKILL; R2)
4. exit 0
```

Inside each daemon that accepts, the coordinator (11 §11.3) runs unchanged. The
admission check is an **io-thread pre-check, not a coordinator step** (O-H2):

```text
io thread (dispatch), BEFORE any state change:
  1. admission check (§4.4)
       Reject -> respond AppCode::NotLastOwner; NO Draining, NO notice; return
       Accept -> continue
  2. requestShutdown(ShutdownReason::LastSupervisor)
  3. auto-reply {accepted:true} + HostNotice{DaemonShuttingDown}   # 11 E19/E21
coordinator (non-io), 11 §11.3 steps 2-5 (unchanged):
  4. stop accepting; drain/close sessions                        # 04 §3.5 step 2
  5. PTY: LocalPtyService::closeAll()  -> terminate+reap every PTY   # 14 E-P2
     MCP: McpManager::shutdown()       -> close MCP child processes   # 15 §4.7
  6. waitForDrain(shutdown_grace)                                  # 11 E19
  7. requestWatchdogStop(); join the owner watchdog — an UNCONDITIONAL,
     untimed std::thread::join() (never join_for); it cannot post after this
                                                                        # §5.1
  8. TransportServer::stop()
  9. drain TurnExecutor; store.close(); releaseHost; unlink socket  # 04 §3.5
```

- **Admission precedes `Draining` (O-H2).** The old §4.3 put the admission check
  in the coordinator, after `requestShutdown` had latched `Draining` and the io
  thread had already replied success + `DaemonShuttingDown`. A daemon with
  another live owner would then be torn down (violating O12). The check is now a
  precondition of `requestShutdown` on the io thread; a reject leaves the daemon
  in `Serving` with no notice emitted.
- **Children before transport.** PTY sessions (spec 14 E-P2 `:647-672`) and MCP
  server children (spec 15 §4.7) are terminated and reaped **before**
  `TransportServer::stop()`, so no child outlives the socket that could report
  it. This is the audited ordering the brief requires.
- **Reply before stop.** Reused verbatim from 11 §11.3 (E19–E21): the shutdown
  reply and `DaemonShuttingDown` notice are enqueued before `stop()`, and the
  coordinator is never the io thread.
- **Watchdog stop/join before transport stop (N2-L3, N3-M1/L2, N4-L2/R4-L3).**
  At step 7 the coordinator calls `requestWatchdogStop()` (sets `watchdog_stop_`,
  `notify_all`) then performs an **unconditional, untimed
  `std::thread::join()`** — never `join_for`/a timed join. The handshake, not a
  timeout, is what makes it prompt: the loop's `cv.wait_for` wakes, sees
  `watchdog_stop_`, and returns. A timed join would be wrong — on timeout the
  coordinator could reach step 8 `TransportServer::stop()` while the watchdog
  thread is still alive. The join therefore **assumes the watchdog loop body is
  non-blocking or bounded**: its registry read (`listSupervisors`, spec:1063) is
  a bounded WAL read and the `cv.wait_for` is interruptible by `notify_all`. If
  that read could block indefinitely the coordinator would hang in `join()` and
  the `_Exit` escalation could not fire (the watchdog is the blocked thread);
  the bounded-read precondition is part of this ordering. The join runs after
  `waitForDrain` and before `TransportServer::stop()`, so the watchdog can never
  post after the transport is gone; steps 8-9 still run on the orphan path.
- **Bounded.** Every wait is bounded by `shutdown_grace` (04 H12); on expiry the
  daemon force-closes and still completes step 9 (04 D-F16). The supervisor's
  `teardown_grace` (5 s) is shorter than a daemon's `shutdown_grace` (10 s)
  because the supervisor does not need to observe the final unlink; it closes the
  socket and exits. A daemon still draining after `teardown_grace` completes via
  its own coordinator or watchdog escalation (§5.1).
- **No SIGKILL from the supervisor.** If a daemon ignores `host.shutdown`
  entirely (a bug), the supervisor closes the socket and exits; the daemon's own
  watchdog self-exit (§5.1) still stops it. The supervisor never signals a
  process on `ps` evidence (R2), and `requestStop` (SIGTERM) remains available
  only for the launcher's own child in the startup-failure path
  (`workspace_host.cpp:892`).

### 4.4 The daemon is the arbiter (16-D5, normative for O-H2)

**16-D5 (pinned).** A `host.shutdown` is a **request**, not a command. Except
for the one documented administrative override (§4.6, reason `"workspace_stop"`),
the daemon accepts it only if, at admission time **on the io thread and before
`requestShutdown` latches `Draining`**, it is genuinely
ownerless-without-the-requester:

```text
# io thread, inside the host.shutdown dispatch, BEFORE requestShutdown.
# ONE exclusion notion: the requester's ClientInstanceId `caller` (from
# conn.instance) and its ClientId `caller_conn` name the same connection.
admit_shutdown(caller, caller_conn, reason):
    if reason == "workspace_stop": return Accept         # §4.6 override (C-M4)
    if live_supervisors_excluding(caller_conn) > 0: return Reject
    if live_automation_count() > 0:                 return Reject
    if other_fresh_owners(caller) > 0:              return Reject   # snapshot − caller
    return Accept
```

- **Admission applies to every `host.shutdown`, not only `LastSupervisor`
  (C-M4).** The revision-0 text admitted only the `last_supervisor` reason, so
  any other reason — including `ymh workspace stop` (`cli.cpp:279-291`, which
  today sends the reason string `"workspace stop"` with a fresh random instance;
  this spec canonicalizes it to `"workspace_stop"`, §4.6/§7.4) — bypassed the
  check and could tear
  down an owned daemon (O12 violated). Admission is now the default for
  `ClientRequest`/`LastSupervisor`/unknown reasons; the only bypass is the
  explicit, user-confirmed `workspace_stop` override (§4.6).
- `Reject` → respond `AppCode::NotLastOwner`, change no state, emit no notice;
  the daemon stays `Serving`. The supervisor treats it as "not the last after
  all", closes its socket, and exits without teardown; `ymh workspace stop`
  reports "workspace in use" unless its override was confirmed (§4.6).
- `Accept` → `requestShutdown(reason)`; the reply and `DaemonShuttingDown`
  notice are enqueued as usual (11 E19/E21).
- **One exclusion notion (R-L4).** `caller` (the requester's `ClientInstanceId`,
  from `conn.instance`) and `caller_conn` (its `ClientId`) identify the same
  connection. The live counts exclude it by `ClientId`; the fresh snapshot
  excludes it by `ClientInstanceId`. No third notion is used anywhere.
- **The view reports raw counts including the caller (R-M3).**
  `OwnershipView.live_supervisors` / `live_automation` are raw counts that
  **include** the requester; `live_supervisors_excluding(caller_conn)` is the
  admission-local subtraction (raw minus the caller when the caller is a
  `Supervisor`, else the raw count). `other_fresh_owners` is the **only** view
  field that excludes the caller. This makes §4.1's `live_supervisors == 1` mean
  "just me" for a sole supervisor.
- **The fresh set is the watchdog snapshot (R-H1/N-H1, C-M7).**
  `other_fresh_owners(caller) = |{ id ∈ snapshot : id ≠ caller }|`, computed from
  the immutable snapshot the watchdog publishes each tick (§5.1); the io thread
  performs **no SQLite call**, preserving 04 §3.4 / 11 E6. The residual window (a
  peer that registered less than `watchdog_interval` ago and has not attached
  yet) is benign: the peer's handshake fails, `ensureRunning` sees the stale
  claim, and it spawns a fresh daemon (§6.2). O12 is worded to match
  ("observable" owner).
- **Registry unavailable** → the watchdog publishes an **empty** snapshot
  (§5.1 catch), so `other_fresh_owners == 0`; the live counts still apply, and
  the fail-safe direction is toward termination (O1).

**O12 — Daemon-arbitrated shutdown.** No client can terminate a daemon while
another owner is **observable** — a live connection, or a fresh registry row
within the watchdog cache interval — **except** the single sanctioned
`ymh workspace stop` administrative override (§4.6), which requires an explicit
user confirmation. The daemon's io-thread admission check is the single point of
truth for every other path; the supervisor's orphaning set only decides whether
to *ask*.

### 4.6 `ymh workspace stop` — the one sanctioned override (C-M4)

`ymh workspace stop <workspace>` (`cli.cpp:279-291`) is an **explicit user
administrative command** to stop one workspace's daemon. It is not a supervisor
and does not own the daemon set. Under O12 it must not silently kill an owned
daemon, so it is reconciled as the single documented exception:

```text
ymh workspace stop <ws>:
  1. connect; hello with ClientRole::Observer (it owns nothing)
  2. view := host.ownership()                      # bounded 2 s (C-L5)
  3. if view.live_supervisors + view.live_automation > 0 and not --force:
        print "workspace <ws> is in use by N live supervisors / M clients"
        prompt "Stopping will disconnect them. Continue? [y/N]"
        if not confirmed -> exit 1, NO shutdown sent
  4. host.shutdown{reason: "workspace_stop"}       # override: skips admission
  5. await reply; exit 0
```

- **Documented override.** The daemon's `admit_shutdown` returns `Accept` for
  `reason == "workspace_stop"` **without** the owner checks, and sets
  `ShutdownReason::WorkspaceStop`. This is the one and only bypass of O12.
- **Confirmation required.** The CLI refuses unless the user confirms
  interactively (`[y/N]`) or passes `--force` (non-interactive/scripts). The
  daemon cannot verify the confirmation (it is a same-UID trust boundary,
  O-L8/§7.4), so the confirmation is a CLI-side contract, not an authenticated
  token.
- **Rationale.** The user explicitly asked to stop this workspace; the daemon's
  owners are the same user; refusing outright would make the command useless
  exactly when a stale/hung supervisor holds a daemon. A confirmation prompt
  makes the disruption visible without requiring the user to exit every
  supervisor first. If the user declines, nothing is written and no shutdown is
  sent.
- **Non-override reasons are not "unchanged" (reconciles §7.4).** Every other
  `host.shutdown` reason now passes `admit_shutdown` and is refused with
  `NotLastOwner` while another owner is observable. Only `workspace_stop`
  bypasses.
- **Alternative rejected.** Routing `ymh workspace stop` through admission
  (option (b)) would make the command silently no-op whenever any supervisor is
  attached, which is a worse UX for an explicit administrative action; the
  confirmed override is the pinned choice.

### 4.5 `ymh run` (Automation)

`ymh run` (`11 §10.2 :1049-1057`) is unchanged except for ownership effects:

- If a live daemon exists: attach with role `Automation` **and profile
  `Automation`** (C-M6), run the task, disconnect. It never registers and never
  prompts.
- If no daemon exists: the in-process M1 path (`run_headless`) runs with no
  daemon at all; ownership is irrelevant.
- While attached, its `LIVE_CONN` keeps the daemon alive and keeps it out of any
  supervisor's orphaning set (O13). This is a **bounded hold**: it lasts only
  while the `ymh run` connection is open, and it is a foreground user action,
  not an unsupervised daemon (O-M8/C-M11, §2.7).
- **It ends its daemon only by disconnecting (O-M6).** `host.shutdown` is
  Interactive-only (`protocol.cpp:557-558`, 05 `:791`), so `ymh run` cannot and
  must not use the last-exit path. After it disconnects, if no owner remains, the
  watchdog stops the daemon after `owner_grace`. `ymh run` therefore never leaves
  an unsupervised daemon even though it does not participate in the last-exit
  prompt.

**O13 — Automation safety.** An `ymh run` task is never terminated mid-turn by a
supervisor's exit; it ends only by its own disconnect (the watchdog then stops
the daemon if no owner remains).

---

## 5. Orphan prevention

### 5.1 The daemon-side owner watchdog (16-D7)

**16-D7 (pinned).** When the watchdog is armed, the daemon runs an in-process
watchdog that stops it when `K(d)` (§2.4) has been false for `owner_grace`. The
watchdog is armed iff `config.require_owner && !config.watchdog_disabled`
(R-M2, §7.3).

```text
owner watchdog (dedicated joinable thread, not the io thread)
  loop until requestWatchdogStop() is observed:
      cv.wait_for(lock, watchdog_interval); if (watchdog_stop_) return;
      try:
          m := monotonic_clock()                      # steady_clock, intervals
          w := wall_clock_ms()                        # system_clock, epoch ms
          live  := live_owner_count_.load() > 0
                   ∧ (m − last_owner_frame_mono_.load()) ≤ limits.idle_timeout
          # Build an IMMUTABLE fresh-id snapshot (R-H1/N-H1). This is the single
          # producer of the fresh set, for both FRESH_OWNER and the admission
          # cache. A bare count cannot answer "fresh owners excluding caller".
          ids := [ s.id for s in registry.listSupervisors()
                   if isFresh(s, w, owner_lease_ttl) ]
          publish_snapshot(std::make_shared<const std::vector<SupervisorId>>(ids))
          fresh := !ids.empty()
          if live ∨ fresh:
              ownerless_since_ = unset
              escalation_armed_ = false
          else:
              if ownerless_since_ unset: ownerless_since_ = m
              if m − ownerless_since_ ≥ owner_grace ∧ ¬escalation_armed_:
                  escalation_armed_   = true
                  escalation_deadline_ = m + kOwnerGraceTeardownBudget   # R4-L4
                  escalate_to_stop()        # NON-BLOCKING: arm + return (N3-M1)
              if escalation_armed_ ∧ m ≥ escalation_deadline_:
                  std::_Exit(HostExitCode::Internal)   # bounded self-exit
      catch (...):
          # registry read failed: publish an EMPTY snapshot (fail-safe toward
          # termination) and CONTINUE. O-M2: an escaping exception must never
          # kill the watchdog thread, which would remove all orphan prevention.
          publish_snapshot(std::make_shared<const std::vector<SupervisorId>>())
          continue
```

`kOwnerGraceTeardownBudget` (R4-L4) bounds the coordinator's **whole** teardown,
not just one grace:

```text
kOwnerGraceTeardownBudget =
      3 * shutdown_grace            # step 4 session drain/close, step 6
                                    # waitForDrain, step 9 TurnExecutor::drain
    + 2 * watchdog_interval         # step 7 join handshake + one tick margin
    = 34 s (defaults: 10 s, 2 s)
```

- **Escalation cannot fire mid-teardown (R4-L4).** The rev-4 value
  (`shutdown_grace + watchdog_interval` ≈ 12 s) was smaller than the real
  orphan-path teardown (`waitForDrain(shutdown_grace)` **then**
  `TurnExecutor::drain`, each grace-bounded, `workspace_host.cpp:534-539`), so
  `_Exit` could fire **before** step 7 and contradict §4.3's "steps 8-9 still run
  on the orphan path". The deadline now covers the full teardown. Equivalent
  alternative: the coordinator publishes one absolute `teardown_started_mono_`
  at step 4 and the watchdog fires only after
  `teardown_started_mono_ + kOwnerGraceTeardownBudget`; either form keeps
  escalation out of the teardown window. The bound stays finite, so O1/O15 hold.
- **Two clocks, never mixed (O-H1/C-M1).** `m` is `steady_clock` and is used
  only for `last_owner_frame` and `ownerless_since_`; `w` is `system_clock`
  epoch ms and is used only for freshness filtering (`isFresh` over
  `listSupervisors`). `isFresh` treats a
  negative `w − heartbeat` as stale (fail-safe, §2.4). A backward wall jump
  makes rows stale, never fresh.
- **Two atomics, no cross-thread map access (O-M3).** `live_owner_count_` and
  `last_owner_frame_mono_` are updated only on the io thread and read by the
  watchdog. The pin: `ProtocolServer` gains a liveness sink
  (`set_owner_liveness_sink`, §7.4) invoked on hello completion, on every
  inbound frame, and on drop; `WorkspaceHost::Impl` stores the values into the
  two atomics. The watchdog never touches `connections_`
  (`protocol_server.hpp:106-124`; 11 §11.3 `:1150-1153`).
- **Fresh-owner snapshot, not a bare count (R-H1/N-H1).** The watchdog publishes
  `std::shared_ptr<const std::vector<SupervisorId>>` via an atomic swap
  (`std::atomic<std::shared_ptr<const std::vector<SupervisorId>>>`, or a
  mutex-guarded `shared_ptr`); the reader is the io thread. This is the **only**
  producer of the fresh set — the loop above builds it from
  `registry.listSupervisors()` filtered by `isFresh`. A bare count cannot answer
  "fresh owners **excluding** the caller", which is exactly what admission needs;
  with a stale count the primary exit path (deregister → then
  `host.shutdown{last_supervisor}`) would reject its own requester and leave the
  daemon `Serving` until the ~23 s watchdog. The snapshot is immutable once
  published, so a reader may hold it across a store without a lock.
  `other_fresh_owners(caller) = |{ id ∈ snapshot : id ≠ caller }|`, where
  `caller` is the requester's `ClientInstanceId` (from `conn.instance`).
- **The registry read is off the io thread (C-M7).** `registry.listSupervisors()`
  is a WAL read (03 §5.2) issued from the watchdog thread; the io-thread
  admission (§4.4) and `ownershipView` (§7.4) read only the published snapshot
  (a pointer load) and perform **no SQLite call**. It never blocks writers and
  never blocks the io thread.
- **Exception-guarded (O-M2).** The body is wrapped; any registry/IO failure
  yields `fresh = false` and the loop continues. The only exit is the daemon's
  own shutdown.
- **Grace cancels on re-attach.** Any hello or frame from a Supervisor/Automation
  role clears `ownerless_since_`, so a supervisor restart within `owner_grace`
  keeps the daemon and its in-flight turn.
- **Idempotent.** `requestShutdown` is idempotent (04 §4.2 `:509-510`); a racing
  `host.shutdown` and the watchdog both funnel into the one coordinator.
- **Bounded, non-blocking escalation (C-M10, N3-M1/L2, R4-L4).**
  `escalate_to_stop()` is **non-blocking**: it records `escalation_armed_` and
  `escalation_deadline_ = m + kOwnerGraceTeardownBudget` (34 s; the full teardown
  bound above), calls `coordinator.requestShutdown(ShutdownReason::NoOwners)`,
  and returns. The self-`_Exit` is a later loop step, not a wait inside
  `escalate_to_stop()`. This removes the circular join (the watchdog is never
  blocked while the coordinator joins it). If the deadline passes and the process
  has not exited (a wedged coordinator/io thread), the watchdog calls
  `std::_Exit(static_cast<int>(HostExitCode::Internal))` — a **self-exit of the
  daemon process**, never a signal to another process, so R2 (never kill on `ps`
  evidence) is preserved. It is what makes O1/O15 hold even against a deadlocked
  teardown.
- **Stop/join handshake (N3-M1/L2, N4-L2/R4-L3).** `requestWatchdogStop()` sets
  the `watchdog_stop_` atomic and `notify_all()`s the loop's condition variable;
  the loop's `cv.wait_for(watchdog_interval)` wakes, observes `watchdog_stop_`,
  and returns. The coordinator calls `requestWatchdogStop()` **before** an
  **unconditional, untimed `std::thread::join()`** at step 7 (§4.3) — never
  `join_for`; the handshake, not a timeout, bounds the wait. This ordering
  **assumes the watchdog loop body is non-blocking or bounded** (the
  `listSupervisors` read is a bounded WAL read; `cv.wait_for` is interruptible
  by `notify_all`). Steps 8-9 (transport stop, store close, releaseHost, unlink)
  still run on the orphan path because the watchdog returns rather than blocking
  the coordinator.
- **Not a signal.** The watchdog is a timer, so SIGHUP/SIGCHLD policy is
  untouched (04 H13, R4).
- **Disarmed for tests.** `config.require_owner == false` or
  `config.watchdog_disabled == true` disables the watchdog entirely: no timer, no
  registry read, no self-termination (16-D11, R-M2). `foreground` does **not**
  disarm it, so an in-process test can arm the watchdog with injected clocks.

The teardown the watchdog triggers is the **same** ordered coordinator as
`host.shutdown` (11 §11.3, §4.3): children (PTY/MCP) → drain → transport stop →
store close → releaseHost. `ShutdownReason::NoOwners` is recorded for
diagnostics but changes no ordering.

### 5.2 Failure matrix and worst-case unsupervised lifetime

Constants (pinned here; identical in §2.4 and §7.3 — C-M2/O-M1/O-L7):
`owner_heartbeat_interval = 5 s`, `owner_lease_ttl = 15 s`
(`= 3 × owner_heartbeat_interval`), `watchdog_interval = 2 s`,
`owner_grace = 5 s`, transport `idle_timeout = 30 s` (unchanged, R10),
`shutdown_grace = 10 s` (04 §4.1), `teardown_grace = 5 s` (§4.3).

**Watchdog fire latency (C-M3 quantization).** `ownerless_since_` is set on the
first tick after `K` becomes false (≤ `watchdog_interval` late), and the fire
condition `m − ownerless_since_ ≥ owner_grace` is evaluated only on later ticks.
The total watchdog latency is therefore

```text
watchdog_latency = watchdog_interval
                 + ceil(owner_grace / watchdog_interval) * watchdog_interval
                 = 2 + ceil(5/2)*2 = 2 + 6 = 8 s
```

and the bound below uses `ttl + watchdog_latency` (death) and
`idle_timeout + watchdog_latency` (freeze). These are **shutdown-request**
latencies; process exit follows the ordered teardown (≤ `shutdown_grace`) or the
escalation self-exit (≤ `kOwnerGraceTeardownBudget`, 34 s) — see O15.

| # | Failure | Detection | Outcome | Worst-case shutdown request |
|---|---|---|---|---|
| **O-F1** | Supervisor `SIGKILL` | AF_UNIX EOF is immediate; row stale at `owner_lease_ttl` | watchdog → `NoOwners` | `ttl + watchdog_latency` = **23 s** |
| **O-F2** | Supervisor `SIGSTOP` (socket open, silent) | transport `idle_timeout` (30 s) closes the connection (`transport_server.cpp:85-97`); row stale at 15 s; on resume the supervisor re-registers/reconnects (§2.6, C-M8) | watchdog → `NoOwners` | `idle_timeout + watchdog_latency` = **38 s** |
| **O-F3** | Clean last exit, confirmed | prompt → `host.shutdown` | ordered teardown | `teardown_grace` = **5 s** (typically < 1 s) |
| **O-F4** | Clean exit, other owners exist | orphaning set empty | no teardown; peers keep serving | n/a |
| **O-F5** | Registry unwritable / disk full, supervisor alive | heartbeat write fails; connection stays live | `K` stays true via `LIVE_CONN`; supervisor logs and retries; **no termination** | n/a |
| **O-F6** | Registry unwritable, supervisor then dies | EOF → `LIVE_CONN` false; row stale | watchdog → `NoOwners` | **23 s** |
| **O-F7** | Registry unreadable at runtime | `listSupervisors` throws | exception-guarded loop ⇒ empty snapshot, `fresh = false`; fall back to `LIVE_CONN`; fail-safe toward termination | **23 s** after last connection |
| **O-F8** | Machine reboot | all processes die | no daemon survives; rows pruned on next start | **0 s** |
| **O-F9** | Stale ownership rows after crash | `delta = w − heartbeat`; `delta > ttl` or `delta < 0` | ignored by `FRESH_OWNER`; pruned by the next supervisor scan | n/a |
| **O-F10** | Daemon spawned, supervisor dies before connect | row fresh for `ttl`, then stale | watchdog → `NoOwners` | **23 s** |
| **O-F11** | `ymh run` mid-turn, last supervisor exits | `host.ownership.live_automation > 0` | not in orphaning set; daemon survives until `ymh run` disconnects (bounded hold, §4.5) | n/a (intentional) |
| **O-F12** | Last-exit prompt unanswered (EOF/closed terminal) | terminal close ⇒ SIGHUP ⇒ the supervisor dies; its sockets EOF immediately | no graceful teardown; row stale ⇒ watchdog backstops | **23 s** (not 38 s: the death is immediate, not a freeze) |
| **O-F13** | Two supervisors exit concurrently | both detach (no confirmed teardown) | sockets EOF ⇒ watchdog | **23 s** |
| **O-F14** | New supervisor attaches during teardown | daemon admission check sees it | shutdown rejected; or handshake fails → `ensureRunning` respawns | n/a |
| **O-F15** | Clock skew / backward wall jump / monotonic wrap | two clocks (16-D13): `w` epoch ms for freshness, `m` monotonic for intervals; `delta < 0` ⇒ stale | freshness fail-safe toward termination; monotonic intervals unaffected by wall jumps | n/a |
| **O-F16** | Two supervisors share a `ClientInstanceId` | per-process mint (16-D8) | impossible by construction | n/a |

**O15 — Bounded unsupervised lifetime.** Under every failure mode above, a
`WorkspaceHost` with the watchdog armed has its shutdown **requested** within
`owner_lease_ttl + watchdog_interval + ceil(owner_grace/watchdog_interval) ·
watchdog_interval` = **23 s** of its last owner disappearing when the loss is a
process death, and within `idle_timeout + 8 s` = **38 s** when the loss is a
freeze. The process then exits after the ordered teardown (bounded by
`shutdown_grace`, 10 s) or, if the teardown wedges, via the watchdog's
self-exit escalation (bounded by `kOwnerGraceTeardownBudget`, 34 s).
There is no unbounded unsupervised state. The one intentional exception is an
`Automation` hold (`ymh run`), which is a bounded foreground task and whose own
disconnect starts the same watchdog (§4.5, O-M8/C-M11).

### 5.3 Why the OR polarity is safe in both directions

§2.4 defines `K = LIVE_CONN ∨ FRESH_OWNER`. The two failure classes it must
survive pull in opposite directions:

- **Connection-only would strand work across a restart.** A supervisor that
  exits and restarts (or a transient socket drop) has no connection for a
  moment. If the daemon trusted only connections, it would kill a live in-flight
  turn during a routine restart. The fresh row covers that window.
- **Registry-only would strand work when the registry fails.** A live supervisor
  on a full disk cannot write its heartbeat, but it is still driving turns over
  a perfectly healthy socket. If the daemon trusted only the registry, it would
  kill live work. `LIVE_CONN` covers that.
- **Both-false is the true orphan condition.** A daemon is unsupervised exactly
  when no socket is attached **and** no fresh row exists. The OR never reports
  "owned" when both are false, so it cannot create an orphan; it only widens the
  window during a genuine owner transition, which is what `owner_grace` bounds.

### 5.4 Reboot, stale rows, and pruning

- A reboot kills every process, so **no orphan can survive a reboot**. The
  registry's rows are simply stale data.
- Stale `supervisors` rows are ignored by `FRESH_OWNER` (`delta = w − heartbeat`
  with `delta > ttl` **or** `delta < 0`) and **pruned** by
  `pruneStaleSupervisors` during a supervisor's scan tick and at startup.
  Pruning is a normal write under the D22 `flock`.
- **Pruning a frozen peer is safe because it can recover (C-M8).** A SIGSTOPped
  supervisor is pruned at `owner_lease_ttl` and loses its socket at
  `idle_timeout`. On resume it detects `heartbeatSupervisor == false`,
  **re-registers its row, and reconnects every dropped daemon** (§2.6) — it does
  not silently lose ownership. A peer must never prune a row whose heartbeat is
  within `owner_lease_ttl`; `pruneStaleSupervisors` deletes only
  `delta > ttl || delta < 0` rows. The residual risk is a last-exit that
  completes during the frozen window; it is bounded by the same 38 s as O-F2 and
  is accepted (a frozen supervisor cannot answer a prompt anyway).
- **The daemon never writes `supervisors`.** Its registry write set remains its
  own host claim (03 §5.3, A9); it only reads the table. Pruning is a supervisor
  duty, because a stale row has no live owner to object to its removal.
- A stale row never clears a **host** claim: `reapIfStale`/`reapHost`
  (`workspace_host.cpp:864-872`) act on sidecar-lock absence and boot-nonce
  mismatch only (03 R5, 04 H11, R2). Ownership staleness and host liveness are
  independent (R1).

### 5.5 Interaction with the transport idle timer

The transport already closes a connection with no frame for `idle_timeout`
(`transport_server.cpp:85-97`, 05 T-F21). Spec 16 **reuses** this and adds no
new socket timer:

- A live supervisor pings every `kPingInterval` (10 s, `protocol.hpp:196`), so
  its connection never idles out.
- A frozen supervisor stops pinging; its connection is closed at 30 s, which
  drops `live_owner_count_` and starts the watchdog grace.
- `owner_heartbeat_interval` (5 s) is faster than the ping because it is a
  cheap registry row update and drives the durable freshness bound; the two
  cadences are independent.

**O16 — Reuse, not duplication.** Ownership liveness adds no new socket
timeout, no new signal, and no new child process. It adds one timer thread, two
atomics, one registry table, and one registry read per watchdog tick.

---

## 6. Concurrency and races

All registry mutations (register, heartbeat, deregister, prune) run under the
03 D22 single-writer `flock`; WAL readers never block. The daemon's connection
state is io-thread-confined. The races below are the ones the critic named; each
has a pinned resolution.

### 6.1 Two supervisors start simultaneously

- Each mints a **distinct** `ClientInstanceId` in memory (16-D8), so
  registration is two independent `INSERT`s with different primary keys. No
  conflict, no supersede, no lost connection.
- Both discover the same daemon set and attach. The daemon's fan-out
  (`protocol_server.cpp:726-743`) and `attachedClients` already support N
  clients; `04 §6.6 :796-797` is retained and promoted (A12).
- If both submit a prompt for the cwd workspace at the same instant, both may
  call `ensureRunning`; the existing D-F1 spawn race is unchanged — the loser's
  daemon exits `WorkspaceBusy` and the loser re-attaches to the winner
  (`workspace_host.cpp:874-915`). Ownership does not alter this path.

**O17 — Distinct identity.** Two live supervisors never share a
`ClientInstanceId`; the daemon never supersedes a peer supervisor's connection.

### 6.2 One supervisor exits while another attaches

```text
S1: host.ownership(d)           # READ-ONLY query, while S1 is still registered
S2: register(S2)                # flock  (may interleave here)
S1: not sole -> close, exit     # empty set: deregister + close (no prompt)
    sole     -> prompt; on confirm:
S1: deregister(S1)              # flock, AFTER confirmation (16-D9)
S1: host.shutdown{last_supervisor}
S2: attach                      # may race the shutdown
```

- If `S2` registers between S1's query and S1's confirm, S1's daemon admission
  (§4.4) sees S2's fresh row (cache) or live connection and rejects; S1 exits
  without teardown and S2 owns the daemon.
- If `S2` registers after S1's deregister but before `host.shutdown`, same.
- If `S2` attaches after the daemon committed to shutdown, its handshake gets
  `ShutdownInProgress` (`protocol_server.cpp:307-312`); its `ensureRunning` finds
  the claim stale once the sidecar lock drops and spawns a fresh daemon.
- If S1 **cancels** the prompt it never deregistered, so it remains an owner and
  a transient disconnect cannot orphan the daemons (C-H3).

Every branch is safe; the only cost is a respawn.

### 6.3 Two last-exits concurrently

- Both query **while still registered**, so each sees the other's live
  connection: `live_supervisors ≥ 2` and neither is sole → **neither prompts**.
- Each then exits by the empty-set path (deregister + close). The daemons see the
  connections EOF and the rows stale, and the watchdog stops them within the
  §5.2 bound.
- If one queries after the other's socket has closed, it may see
  `live_supervisors == 1`; the daemon's admission re-checks at shutdown time
  (the peer's row is gone) and accepts. At most one prompt appears.
- No path leaves a daemon ownerless beyond the bound, and no path leaves a live
  supervisor deregistered.

**16-D9 (pinned; fixes C-H3/O-M9).** The exit sequence is **query (while
registered) → prompt → deregister (only after confirm) → `host.shutdown` →
close**. For an empty orphaning set it is **query → deregister → close**. A
supervisor is never deregistered while it is still running and unconfirmed.

### 6.4 Daemon shutdown racing a new attach

Covered by §6.2. The daemon never needs to "cancel" a shutdown in progress: a
new attach either arrives before admission (and causes a reject) or after
`Draining` (and fails the handshake, leading to a respawn). No half-shutdown
state is observable: the coordinator's ordered teardown (11 §11.3) either has
not begun (reject) or runs to completion.

### 6.5 Stale ownership rows after a crash; `reapIfStale` reconciliation

| Stale artifact | Owner of its cleanup | Rule |
|---|---|---|
| `supervisors` row (dead supervisor) | any supervisor (`pruneStaleSupervisors`) | `delta = w − heartbeat`; `delta > ttl \|\| delta < 0`; safe because no live process owns it |
| `workspaces` host claim (dead daemon) | `HostLifecycle::reapIfStale` → `reapHost` (`workspace_host.cpp:864-872`) | sidecar-lock absence + boot-nonce mismatch only (03 R5, H11) |
| `session_leases` row | next daemon open (02 §5.3) | unchanged |

**O18 — Independence.** A stale `supervisors` row never clears or creates a
host claim, and a stale host claim never affects `FRESH_OWNER`. The two liveness
domains are separate: host liveness = sidecar flock (R1); ownership freshness =
heartbeat age.

### 6.6 `ClientInstanceId` collision (fixed)

Today `load_client_instance()` (`supervisor.cpp:40-60`) returns the same id to
every concurrent supervisor because it reads one shared
`<state>/ymh/supervisor.json`. The daemon's supersede logic
(`protocol_server.cpp:315-327`) would then drop S1's connection when S2
connects. **16-D8 (§7.5) removes persistence**; the id is per process, so the
collision is impossible. Supersede still works for its only real use — a
transient reconnect **within** one process — because the id is stable for that
process's lifetime.

**O19 — No self-supersede across peers.** Concurrent supervisors are mutually
invisible to the supersede path.

---

## 7. Interfaces and schema migration

This section pins only new or changed types and functions. Headers live under
`include/ymh/` in the namespace shown. No implementation code.

### 7.1 Registry additions

The ownership set is stored in the registry. The transport-facing identity is
`protocol::ClientInstanceId` (`protocol.hpp:47-51`); the registry declares its
own mirror type to avoid a registry → transport dependency (the same mirroring
convention `protocol.hpp:10-14` uses).

```cpp
// include/ymh/registry/supervisor.hpp (new)
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ymh {

// Wire-identical to protocol::ClientInstanceId (UUIDv4 text). A supervisor's
// durable identity, minted per process (16-D8).
struct SupervisorId {
    std::string value;
    auto operator<=>(const SupervisorId&) const = default;
};

// One row of the `supervisors` ownership set (§2.3).
struct SupervisorRow {
    SupervisorId                 id;
    std::int32_t                 pid{0};
    std::string                  bootId;       // per-process nonce (UUIDv4)
    std::int64_t                 startedAtMs{0};
    std::int64_t                 heartbeatMs{0};  // WALL epoch ms (system_clock)
    std::optional<std::string>   tty;          // display only
};

// FRESH_OWNER predicate (16-D13, O-H1/C-M1). `now_wall_ms` is WALL epoch ms
// (system_clock), never a steady_clock value. Pinned:
//   delta := now_wall_ms - row.heartbeatMs;
//   return delta >= 0 && delta <= ttl;          // delta < 0 => STALE (fail-safe)
[[nodiscard]] bool isFresh(const SupervisorRow& row, std::int64_t now_wall_ms,
                           std::chrono::milliseconds ttl) noexcept;

} // namespace ymh
```

```cpp
// include/ymh/registry/registry.hpp (additive to WorkspaceRegistry)
// All mutations run under the 03 D22 writer flock. Reads are WAL.
// `registerSupervisor` is an upsert by `id`. If a row with the same `id` exists
// but a different `bootId`, the new process supersedes it (C-L2: `bootId` is the
// PID-reuse guard, mirroring 03 R5) — a recycled pid with a different nonce is
// never mistaken for the same supervisor.
void registerSupervisor(const SupervisorRow& row);
// `heartbeatSupervisor` returns false when the row is absent (pruned by a peer
// while this supervisor was frozen, C-M8); the caller MUST re-register (§2.6).
[[nodiscard]] bool heartbeatSupervisor(const SupervisorId& id,
                                       std::int64_t now_wall_ms);
[[nodiscard]] bool deregisterSupervisor(const SupervisorId& id);      // false if absent
[[nodiscard]] std::vector<SupervisorRow> listSupervisors() const;     // all rows

// Count fresh rows, optionally excluding one id. Used by the SUPERVISOR's own
// registry read to confirm a fresh-owner-only candidate (§4.1, R-L1). The
// daemon's FRESH_OWNER uses the watchdog snapshot, not this method (§5.1,
// N2-L1).
[[nodiscard]] std::size_t freshSupervisorCount(
    std::int64_t now_wall_ms, std::chrono::milliseconds ttl,
    const std::optional<SupervisorId>& exclude = std::nullopt) const;

// Convenience wrapper for non-daemon callers. The daemon watchdog does NOT use
// this: it calls `listSupervisors()` + `isFresh` to build the fresh-id snapshot
// (R-H1, §5.1).
[[nodiscard]] bool hasFreshSupervisor(std::int64_t now_wall_ms,
                                      std::chrono::milliseconds ttl) const;

// Delete rows with `delta > ttl || delta < 0` (delta as in isFresh). Returns the
// number deleted. Supervisor-only (A9); the daemon never calls this.
std::size_t pruneStaleSupervisors(std::int64_t now_wall_ms,
                                  std::chrono::milliseconds ttl);
```

### 7.2 Schema migration (additive)

- `SCHEMA_VERSION` becomes **2** (03 §3.4 `:313`, A11).
- Migration **1 → 2** is purely additive and runs in one transaction, bumping
  `user_version` inside it (03 §3.4 `:335-337`):

```sql
-- migration 1 -> 2 (additive; touches no existing row)
CREATE TABLE supervisors (
    id          TEXT PRIMARY KEY,
    pid         INTEGER NOT NULL,
    boot_id     TEXT NOT NULL,
    started_at  INTEGER NOT NULL,
    heartbeat   INTEGER NOT NULL,
    tty         TEXT,
    CHECK (pid > 0)
);
CREATE INDEX idx_supervisors_heartbeat ON supervisors(heartbeat);
PRAGMA user_version = 2;
```

- **Existing `registry.db` with real rows** (the user's machine today): a
  **writable** open sees `user_version = 1 < 2` and runs the migration in one
  transaction; the four existing tables are untouched, and `supervisors` is
  created empty. No data loss, no rewrite of
  `workspaces`/`workspace_sessions`/`pending_mutation`/`registry_meta`
  (03 §3.4 forward policy `:328-339`).
- **Read-only open must tolerate an un-migrated DB (C-M9/O-M7).** Today
  `WorkspaceRegistry::openReadOnly` throws `RegistryError{SchemaVersion}` on any
  `version != kRegistrySchemaVersion`, and migration runs only in the writable
  `open()` (`src/registry/registry.cpp:735-738`, `:697-706`). But `ymh list`/
  `ymh show` (11 E15) and `DaemonSetScanner` (§3.2) use `openReadOnly` — so
  browse and discovery would fail until a supervisor happens to migrate the DB.
  **Pinned amendment to 03 §4.3/§3.4:** `openReadOnly` accepts
  `version <= SCHEMA_VERSION` and opens the **old schema view** without writing.
  Any query against a table introduced after `version` returns empty/zero — for
  this spec, `freshSupervisorCount`/`hasFreshSupervisor`/`listSupervisors`
  return `0`/`false`/`{}` when the `supervisors` table is absent. A writable
  `open()` still migrates to 2 first. The scanner therefore never depends on the
  migration having run.
- **Backward policy for a *newer* DB is unchanged:** a binary that sees
  `user_version > SCHEMA_VERSION` refuses with `RegistryError{SchemaVersion}`
  (03 §3.4 `:341-344`). This is a deployment constraint, not a data-loss event;
  a daemon and a supervisor must be the same build. See §9 OQ-3.
- **DDL ownership:** 03 §3.3 `:234-284` and 00 §9.10 `:1189-1232` gain this
  table **by reference** (A10). The four existing tables remain byte-identical.

### 7.3 Daemon and host additions

```cpp
// include/ymh/core/clock.hpp (new) — the two-clock seam (16-D13; O-H1/C-M1)
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace ymh {

// Monotonic: local interval math ONLY (frame age, grace, timeouts).
using MonotonicClock = std::function<std::chrono::steady_clock::time_point()>;

// Wall: durable heartbeat/freshness ONLY (epoch ms). Cf. `WallClock` in
// include/ymh/agent/compactor.hpp:106; same shape, shared here for the daemon
// and the supervisor.
using WallClockReader = std::function<std::chrono::system_clock::time_point()>;

[[nodiscard]] std::int64_t epoch_ms(std::chrono::system_clock::time_point) noexcept;
[[nodiscard]] MonotonicClock default_monotonic_clock();   // steady_clock::now
[[nodiscard]] WallClockReader default_wall_clock();       // system_clock::now

} // namespace ymh
```

```cpp
// include/ymh/core/ownership.hpp (new) — the single source for the ownership
// cadence constants (N2-L7). Both `HostConfig` (below) and
// `SupervisorPresence::Options` (§7.5) default from these; a static_assert pins
// the ttl >= 3x relation (O-L7).
#pragma once

#include <chrono>

namespace ymh {

inline constexpr std::chrono::milliseconds kOwnerHeartbeatInterval{5'000};
inline constexpr std::chrono::milliseconds kOwnerLeaseTtl{15'000};
inline constexpr std::chrono::milliseconds kOwnerWatchdogInterval{2'000};
inline constexpr std::chrono::milliseconds kOwnerGrace{5'000};

static_assert(kOwnerLeaseTtl >= 3 * kOwnerHeartbeatInterval,
              "owner_lease_ttl must cover three missed heartbeats");

} // namespace ymh
```

```cpp
// include/ymh/host/workspace_host.hpp (additive)
enum class ShutdownReason : std::uint8_t {
    ClientRequest,    // host.shutdown with no recognized reason (admission-checked)
    Signal,           // SIGTERM / SIGINT
    StartupFailure,   // a startup step failed after partial state was created
    LastSupervisor,   // the last owner's confirmed exit (§4.4)
    NoOwners,         // owner watchdog: K(d) false for owner_grace (§5.1)
    WorkspaceStop,    // `ymh workspace stop` administrative override (§4.6, C-M4)
};

struct HostConfig {
    // … existing fields (04 §4.1, 11 §10.1) unchanged …

    // Watchdog arming (R-M2, N2-H1/N2-M1): armed iff
    // `require_owner && !watchdog_disabled`. `require_owner` defaults TRUE
    // (fail-safe/fail-closed): a daemon built without explicit opt-out always
    // has a watchdog. The hermetic/in-process tests opt OUT explicitly
    // (`require_owner = false` or `watchdog_disabled = true`); the deterministic
    // watchdog test uses `require_owner = true, watchdog_disabled = false` with
    // injected clocks. `foreground` does not participate. A non-`foreground`
    // daemon that would arm no watchdog — i.e. `!(require_owner &&
    // !watchdog_disabled)`, equivalently `!require_owner || watchdog_disabled` —
    // is a hard startup failure (§7.3 guard below; N4-L1/R4-L2).
    bool require_owner{true};
    bool watchdog_disabled{false};

    std::chrono::milliseconds owner_heartbeat_interval{kOwnerHeartbeatInterval};
    std::chrono::milliseconds owner_lease_ttl{kOwnerLeaseTtl};          // FRESH_OWNER ttl
    std::chrono::milliseconds watchdog_interval{kOwnerWatchdogInterval}; // watchdog tick
    std::chrono::milliseconds owner_grace{kOwnerGrace};                 // ownerless hysteresis

    // Two-clock seam (O-M10). Defaults are the real clocks; tests inject fakes
    // so no test sleeps. Both are read only by the watchdog thread.
    MonotonicClock  owner_monotonic_clock{default_monotonic_clock()};
    WallClockReader owner_wall_clock{default_wall_clock()};
};

class WorkspaceHost {
    // … existing (04 §4.2, 11 §10.1) unchanged …

    // Live connection counts by role (io-thread snapshot; §2.4).
    [[nodiscard]] std::size_t attachedSupervisors() const noexcept;
    [[nodiscard]] std::size_t attachedAutomation() const noexcept;

    // True once the owner watchdog has fired (diagnostics/tests).
    [[nodiscard]] bool ownerless() const noexcept;
};
```

- `attachedClients()` (04 §4.2 `:526-528`) keeps its signature; only its
  comment changes (A2). `attachedSupervisors()`/`attachedAutomation()` are new.
- **Watchdog arming is fail-safe (R-M2, N2-H1/N2-M1).** Armed iff
  `config.require_owner && !config.watchdog_disabled`. `require_owner` defaults
  **true** (fail-closed): an unconfigured daemon always has a watchdog. The
  hermetic tests opt OUT explicitly (`require_owner = false` or
  `watchdog_disabled = true`); the deterministic watchdog test runs in-process
  with `require_owner = true`, `watchdog_disabled = false`, and injected clocks.
  `foreground` does not participate. A unit test pins the truth table (§8.4.1).
- **Fail-loud guard, complete (N3-H1/G1).** Startup rejects a non-`foreground`
  daemon unless the watchdog would actually be armed:
  `if (!foreground && !(require_owner && !watchdog_disabled)) ->
  HostExitCode::StartupRejected`. The rev-3 guard tested only `require_owner`, so
  `require_owner=true, watchdog_disabled=true` passed and armed nothing — an
  unsupervised daemon (O1 violated). The complete predicate closes both arms.
  The guard runs **first, before any state** (§2.5 step 1', L3), so no
  `claimHost` has happened and no H21 release is needed. Only the in-process
  (`foreground == true`) test host may opt out. A unit test pins the full truth
  table and asserts both production constructors set `require_owner = true` and
  `watchdog_disabled = false`: the `--host` entry (`cli.cpp:151-242`) and
  `HostLifecycle::configFor` (`workspace_host.cpp:852-862`). This is what makes
  O1 fail **closed** rather than open.
- **`owner_lease_ttl ≥ 3 × owner_heartbeat_interval` (O-L7, N2-L7).** The
  relation is a `static_assert` in `include/ymh/core/ownership.hpp` (the single
  source for these constants); `HostConfig` additionally validates it at startup
  (a violation is a hard `HostExitCode::StartupRejected`) and a unit test asserts
  the static_assert and the shared-constant defaults. `HostConfig` and
  `SupervisorPresence::Options` both default from the same constants, so they
  cannot drift.
- **Two clocks, injected (O-H1/C-M1/O-M10).** The watchdog reads
  `owner_monotonic_clock()` for `m` and `owner_wall_clock()` for `w`; it never
  calls `Clock::now()` or `system_clock::now()` directly. Tests advance a fake
  wall clock to cross `owner_lease_ttl` and a fake monotonic clock to cross
  `owner_grace`, then notify the watchdog's condition variable — no sleeping.
- The watchdog owns a dedicated joinable thread and two atomics
  (`live_owner_count_`, `last_owner_frame_mono_`); it is created after
  `claimHost` (startup step 9, §2.5) and joined before `TransportServer::stop()`
  in the coordinator (11 §11.3), so it cannot post after the transport is gone.
  Construction/destruction order stays inside `WorkspaceHost::Impl` (11 E18).

**Attach identity threading (C-H2; amends 04 §6.1/§6.2).** The supervisor's
identity must reach the daemon, but today `connect_checked` hardcodes
`ServerProfile::Interactive` and mints a fresh `generate_uuid_v4()`
(`src/host/workspace_host.cpp:143-153`), so the connection's `conn.instance`
never equals the supervisor's `supervisors.id` and the last-exit prompt cannot
fire.

```cpp
// include/ymh/host/host_launcher.hpp (changed)
struct AttachIdentity {
    protocol::ClientInstanceId client_instance;   // the caller's pinned id (16-D8)
    protocol::ClientRole       role{protocol::ClientRole::Supervisor};
};

class HostLifecycle {
    // … existing members unchanged …

    // C-H2: `ensureRunning(WorkspaceId)` is REPLACED. The identity is threaded
    // into `connect_checked`, which uses it verbatim (no fresh uuid, no hardcoded
    // profile) and derives the profile from the role:
    //   Supervisor -> ServerProfile::Interactive
    //   Automation -> ServerProfile::Automation     (C-M6, §7.4)
    [[nodiscard]] AttachResult ensureRunning(WorkspaceId, AttachIdentity);
};
```

- The supervisor passes `{its pinned ClientInstanceId, ClientRole::Supervisor}`
  for both spawn-and-attach and scanner attach; `ymh run` passes
  `{a per-run id, ClientRole::Automation}`. `SupervisorConnection` already sends
  `config_.client_instance` (`supervisor_connection.cpp:188`); its
  `SupervisorConnectionConfig.client_instance` is filled from the **in-memory**
  mint (16-D8), not the shared file.
- `connect_checked` keeps the D20.7 identity cross-check (`hello.workspace` /
  `hello.boot_id`) unchanged; only its identity argument changes.

### 7.4 Transport additions

```cpp
// include/ymh/transport/protocol.hpp (additive)
enum class ClientRole : std::uint8_t {
    Supervisor,   // interactive TUI; registers, owns the daemon set
    Automation,   // `ymh run`; attaches, never registers (§4.5)
    Observer,     // `ymh workspace stop`; owns nothing (§4.6)
};

struct HelloParams {
    // … existing fields (protocol.hpp:253-258) unchanged …
    ClientRole role{ClientRole::Supervisor};   // additive; default preserves v1
};

// host.ownership (new read-only method). No params: the daemon uses the
// calling connection's ClientInstanceId (set at hello), so it cannot be spoofed.
struct OwnershipView {
    struct ClientInfo {
        std::string client_instance;
        ClientRole  role{ClientRole::Supervisor};
        std::int32_t pid{0};
    };
    std::vector<ClientInfo> clients;             // live, hello-complete, owner roles
    std::size_t live_supervisors{0};             // RAW count INCLUDING the caller (R-M3)
    std::size_t live_automation{0};              // RAW count INCLUDING the caller (R-M3)
    std::size_t other_fresh_owners{0};           // |snapshot − caller|, the only exclusion (R-H1)
    bool        shutting_down{false};            // HostState::Draining
};

namespace method {
// … existing catalog unchanged (protocol.hpp:415-444) …
inline constexpr std::string_view kHostOwnership = "host.ownership";
} // namespace method

// Additive AppCode (next free after -32018).
enum class AppCode : int {
    // … existing codes unchanged …
    NotLastOwner = -32019,   // any non-override host.shutdown refused (§4.4)
};

// Transport mirror of ymh::ShutdownReason (workspace_host.hpp), alongside the
// existing HostState / HostErrorCode mirrors (protocol.hpp:85-107). The daemon
// maps it one-to-one (§7.4).
enum class ShutdownReason : std::uint8_t {
    ClientRequest, Signal, StartupFailure, LastSupervisor, NoOwners, WorkspaceStop,
};
```

```cpp
// include/ymh/transport/host.hpp (additive to TransportHost, :57-63)
class TransportHost {
    // … existing members unchanged, EXCEPT `requestShutdown(std::string)` at
    // `:63` is replaced by the typed entry below …

    // N-M3/N2-M2: the daemon's published fresh-id snapshot, typed in transport
    // terms. `HostRuntime` overrides it by converting the `ymh::SupervisorId`
    // snapshot (§5.1) to `protocol::ClientInstanceId` (wire-identical UUID
    // text). io thread only; returns a shared_ptr copy.
    [[nodiscard]] virtual std::shared_ptr<const std::vector<ClientInstanceId>>
    freshOwnerSnapshot() const = 0;

    // Typed shutdown, replacing the string-only path for admission-checked
    // reasons. `HostRuntime` maps it one-to-one to `ymh::ShutdownReason`.
    virtual void requestShutdown(ShutdownReason reason) = 0;
};
```

```cpp
// include/ymh/transport/protocol_server.hpp (additive)
class ProtocolServer {
    // … existing unchanged …

    // Broadcast a workspace-scoped lifecycle notice to every hello-complete
    // Interactive client (16-D4). io thread only; not gated on subscription.
    void onSessionCreated(const SessionId& session);

    // Role of a handshaken client; nullopt if unknown/not handshaken.
    [[nodiscard]] std::optional<ClientRole> roleOf(ClientId id) const;

    // Build the host.ownership view for the CALLING connection (io thread).
    // `caller` is the ClientId of the connection whose dispatch is running — it
    // is NOT a wire parameter. `host.ownership` takes NO request params: the
    // requester's ClientInstanceId is read from `conn.instance` (set at hello),
    // so it cannot be spoofed (O-L1).
    //
    // No clock/ttl parameters (N2-L5): the fresh set is the watchdog's
    // precomputed immutable snapshot (§5.1), so no freshness math happens here.
    [[nodiscard]] OwnershipView ownershipView(ClientId caller) const;
};
```

- `Connection` gains `ClientRole role{ClientRole::Supervisor}` (set from
  `HelloParams.role` at hello; `protocol_server.hpp:106-124`).
- **Role ↔ profile is pinned (C-M6).** `role == Automation ⇒ profile ==
  Automation`; `role ∈ {Supervisor, Observer} ⇒ profile == Interactive`. The
  daemon derives the profile from the role at hello and rejects a mismatch with
  `AppCode::InvalidParams`. Rationale: permission accounting filters
  `profile == Interactive` (`protocol_server.cpp:908`), so an `ymh run` client
  that kept `Interactive` could hold a daemon for the 5-minute permission
  timeout with nobody to answer it. This also means `ymh run` cannot call
  `host.shutdown` at all (Interactive-only, `protocol.cpp:557-558`, 05 `:791`) —
  it ends its daemon only by disconnecting (O-M6, §4.5).
- **Method catalog (O-M4).** `kMethodCatalog` is a fixed array
  (`constexpr std::array<std::string_view, 28> kMethodCatalog`,
  `src/transport/protocol.cpp:525`) and `is_known_method` rejects anything not
  in it. Pin: append `method::kHostOwnership` → **size 28 → 29**, and update
  `tests/unit/transport_protocol_test.cpp:41`
  (`EXPECT_EQ(protocol::all_methods().size(), 29u)`). Without this the new RPC
  is rejected as unknown.
- **Liveness sink (O-M3, R-M1).** Pin
  `ProtocolServer::set_owner_liveness_sink(
  std::function<void(std::size_t live_owners, std::chrono::steady_clock::time_point last_frame)>)`,
  invoked on the io thread at hello completion, on every inbound frame, and on
  drop. **The sink counts only `role ∈ {Supervisor, Automation}`** — the exact
  `LIVE_CONN` role set (§2.4). An `Observer` (e.g. `ymh workspace stop`, §4.6)
  is never counted and therefore **cannot hold `K(d)`**, matching §4.6's "it
  owns nothing" and O1. A unit assertion pins the filter (§8.4.1).
  `WorkspaceHost::Impl` stores the values into `live_owner_count_` and
  `last_owner_frame_mono_` (the two atomics the watchdog reads, §7.3). Without
  this seam the atomics have no pinned writer.
- **`host.ownership` takes no request params (O-L1).** The request shape is `{}`
  (empty object); there is **no `requester` wire field**. Identity is the calling
  connection's `conn.instance`, set at hello, so it cannot be spoofed.
  `ownershipView` receives the `ClientId` of the dispatching connection from the
  daemon — never from client-supplied data. (Revision 0's §4.1 sent
  `{requester: my_client_instance}`; that param is dropped, matching the §7.4
  signature.)
- **`host.ownership` never reads the registry on the io thread (C-M7, R-H1).**
  The io-thread admission (§4.4) uses the exact live-connection counts plus
  `other_fresh_owners = |snapshot − caller|`, where `snapshot` is the immutable
  fresh-id vector the watchdog publishes each tick (§5.1). `ownershipView` reads
  the snapshot pointer and performs **no SQLite call**. This respects 04 §3.4 /
  11 E6 (no blocking work on the io thread) and §5.1's off-loop registry read.
- **Transport-side seams for admission (N-M3, N2-M2).** `ProtocolServer` holds
  **`TransportHost&`** (`protocol_server.hpp:153`), not `HostRuntime&`. The two
  seams therefore live on `TransportHost`
  (`include/ymh/transport/host.hpp:57-63`), whose existing entry is
  `virtual void requestShutdown(std::string reason) = 0` (`:63`). Pin, as
  additive 05 amendments:
  1. a **snapshot accessor** typed in transport terms —
     `virtual std::shared_ptr<const std::vector<ClientInstanceId>> freshOwnerSnapshot() const = 0;`
     — `HostRuntime` overrides it by converting the `ymh::SupervisorId` snapshot
     published by `WorkspaceHost::Impl` (§5.1) to `protocol::ClientInstanceId`
     (the two are wire-identical UUID text, §7.1); and
  2. a **typed shutdown entry** —
     `virtual void requestShutdown(protocol::ShutdownReason) = 0;`
     — replacing the string-only `:63` entry. `protocol::ShutdownReason` is the
     transport mirror in `protocol.hpp` (alongside `HostState`/`HostErrorCode`).
     **Single conversion point (G3):** the `ProtocolServer` `kHostShutdown`
     handler parses the wire `reason` string to `protocol::ShutdownReason`
     (`parse_shutdown_reason`); `HostRuntime::requestShutdown(protocol::
     ShutdownReason)` maps it one-to-one to `ymh::ShutdownReason` and calls the
     daemon's typed `requestShutdown`. There is exactly one string→enum parse
     (transport) and one mirror→native map (adapter); no other layer sees a
     string.
  Both are io-thread-callable and non-blocking; the snapshot accessor returns a
  `shared_ptr` copy (the only allocation). This amends 05's `TransportHost`
  (§1.3 A15) and reconciles the seam comment at `host.hpp:12-14` ("everything
  semantic is delegated to a `TransportHost`" — these two are the transport's
  read-only ownership view and its typed shutdown request).
- **Seam retyping — the concrete edits (G3, R4-L1).** The spec does not
  overstate what exists today; these are the owed changes:
  `include/ymh/transport/host.hpp:63` (string → enum);
  `include/ymh/host/host_runtime.hpp:119` (`setShutdownHook` becomes
  `std::function<void(ymh::ShutdownReason)>`) and `:136` (typed override);
  `include/ymh/host/host_runtime.hpp:214` (the `shutdown_hook_` member, retyped
  to `std::function<void(ymh::ShutdownReason)>`);
  `src/host/host_runtime.cpp:244-245` (hook storage) and `:345-348` (map the
  enum and call the daemon — no `std::move(reason)` string);
  `src/host/workspace_host.cpp:467-468` (the hook must forward the typed reason,
  **not** hardcode `ClientRequest`); `src/transport/protocol_server.cpp:360`
  (parse the string once and pass the enum);
  `tests/unit/host_runtime_test.cpp:619` and
  `tests/support/fake_transport_host.hpp:54` (typed signatures). A test asserts
  that `last_supervisor`/`workspace_stop` do **not** degrade to `ClientRequest`.
  **Include note (R4-L1):** `host_runtime.hpp` has no include or forward
  declaration for `ymh::ShutdownReason` today (`host_runtime.hpp:29-49`), so it
  must add `#include "ymh/host/workspace_host.hpp"` (or forward-declare the
  scoped enum `enum class ShutdownReason : std::uint8_t;` — it is a fixed
  underlying type, so a forward declaration is legal).
- **`host.shutdown` reason mapping — one conversion point (O-L5, C-M4, G3).**
  Today the hook discards the reason (`workspace_host.cpp:467-468` always
  `ClientRequest`). Pin the mapping **at the `ProtocolServer` `kHostShutdown`
  parse**: `"last_supervisor"` → `protocol::ShutdownReason::LastSupervisor`;
  `"workspace_stop"` → `WorkspaceStop` (the §4.6 override, the only admission
  bypass); anything else (including a missing reason) → `ClientRequest`. The
  enum then flows unchanged through `HostRuntime::requestShutdown` to the
  daemon's typed `ymh::ShutdownReason`. **Every reason except `workspace_stop`
  passes `admit_shutdown` (§4.4)** — so a non-last `host.shutdown` is **not**
  "unchanged": it is refused with `NotLastOwner` while another owner is
  observable (C-M4). `ShutdownReason::NoOwners` is set only internally by the
  watchdog and never travels on the wire. **No reason may degrade to
  `ClientRequest`**; a unit test pins the `last_supervisor`/`workspace_stop`
  round-trip.
- `session.create`/`session.fork`/`session.resume` dispatch calls
  `onSessionCreated(session)` after the reply is enqueued
  (`protocol_server.cpp:382-399`); `onSessionClosed` gains the broadcast
  `SessionClosed` to all Interactive clients in addition to the per-subscriber
  unsubscribe notices (`:745-775`).
- **Same-UID trust (O-L8).** `ClientRole` is self-asserted and defaults to
  `Supervisor`, so any same-UID client that completes hello and sends frames
  holds `K(d)`. This is the existing 05 §4.2 same-UID trust boundary (the socket
  is `0600`, owner-only). Pin the assumption explicitly: **ownership liveness
  trusts same-UID peers**; role is advisory, not authenticated. A client that
  wants to be counted as an ownership-of-record supervisor must additionally
  hold a fresh `supervisors` row (which only `SupervisorPresence` writes). No
  cross-UID hardening is attempted in v1.

### 7.5 Supervisor additions

```cpp
// include/ymh/ui/supervisor_presence.hpp (new)
#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "ymh/core/ownership.hpp"      // kOwnerHeartbeatInterval/kOwnerLeaseTtl (L4)
#include "ymh/registry/registry.hpp"   // WorkspaceRegistry (registerSelf, L4)
#include "ymh/registry/supervisor.hpp" // SupervisorId / SupervisorRow

namespace ymh::ui {

// Owns this supervisor's `supervisors` row: register at start, heartbeat on a
// timer, deregister on clean exit. Never dereferences another supervisor's row.
class SupervisorPresence {
public:
    struct Options {
        // Default from include/ymh/core/ownership.hpp (N2-L7), the same source
        // as HostConfig; they cannot drift.
        std::chrono::milliseconds heartbeat_interval{kOwnerHeartbeatInterval};
        std::chrono::milliseconds owner_lease_ttl{kOwnerLeaseTtl};
        std::optional<std::string> tty;   // display only
    };

    // Registers (idempotent upsert). Must run before any daemon is spawned so
    // FRESH_OWNER covers the spawn window (§2.6).
    static SupervisorPresence registerSelf(WorkspaceRegistry&, SupervisorId,
                                           Options);

    // Timer-driven. `now_wall_ms` is WALL epoch ms (16-D10). Returns false when
    // the row was pruned by a peer (C-M8): the caller MUST re-register and
    // reconnect (§2.6).
    [[nodiscard]] bool heartbeat(std::int64_t now_wall_ms);
    void reRegister();                             // after a false heartbeat (C-M8)
    // Confirmed-exit path (§4.3). `noexcept` because it is best-effort: it
    // catches registry errors internally and **inspects the registry call's
    // return value** (`deregisterSupervisor` returns false if absent), never
    // throwing (N2-L4/L5). A failed deregister leaves the row to age out (ttl)
    // or be pruned.
    void deregister() noexcept;

    [[nodiscard]] const SupervisorId& id() const noexcept;
};

} // namespace ymh::ui
```

```cpp
// include/ymh/ui/supervisor.hpp (additive)
struct SupervisorRunOptions {
    // … existing fields {workspaces, initial_workspace, config, verbose} unchanged …
    bool no_prompt{false};                        // --yes; skip the exit prompt
    std::chrono::milliseconds scan_interval{2'000};

    // C-H4 spawn seam (borrowed, non-owning). Both MUST outlive the entire
    // `run_supervisor(options)` call (R-L3): the caller (`run_supervisor_entry`)
    // owns them for the whole TUI session, so a lazy spawn on the first prompt
    // is still valid. Today HostLifecycle is built in run_supervisor_entry
    // (cli.cpp:349) and never reaches run_supervisor, and SupervisorConnection
    // can only attach — so the supervisor cannot spawn and 16-D2 is
    // unimplementable. `run_supervisor_entry` supplies both; read-only and test
    // paths leave them null (the workspace stays NotRunning).
    HostLifecycle*     lifecycle{nullptr};   // caller-owned; outlives run_supervisor
    WorkspaceRegistry* registry{nullptr};    // caller-owned; outlives run_supervisor

    // This supervisor's pinned identity (16-D8) and clock seam (16-D10).
    AttachIdentity     identity;                  // {pinned id, ClientRole::Supervisor}
    MonotonicClock     monotonic_clock{default_monotonic_clock()};
    WallClockReader    wall_clock{default_wall_clock()};
};

// include/ymh/ui/supervisor_connection.hpp (additive)
// The connection sends ClientRole::Supervisor at hello and exposes the
// ownership query; `ymh run` sends ClientRole::Automation.
```

**16-D8 (pinned).** `ClientInstanceId` is minted **per process, in memory**, and
is never persisted. `load_client_instance()` (`supervisor.cpp:40-60`) is
replaced by an in-memory mint at `SupervisorApp` construction; the shared
`<state>/ymh/supervisor.json` is no longer read or written. Supersede still
works because the id is stable for the process's lifetime and is only needed to
replace *this* process's prior connection (`protocol_server.cpp:315-327`).
This supersedes 11 §8.2 D20.3 (A8) and reconciles with C-H2:
`SupervisorRunOptions.identity.client_instance` and
`SupervisorConnectionConfig.client_instance` (`src/ui/supervisor.cpp:148-155`) are both filled
from this one in-memory mint, and `HostLifecycle::ensureRunning` receives it.

**16-D10 (pinned; two clocks, O-H1/C-M1).** `SupervisorApp` runs two timers: the
presence heartbeat (5 s) and the daemon-set scan (2 s). Both use the **monotonic**
reader (`MonotonicClock`) for their intervals; the heartbeat **value** is stamped
from the **wall** reader (`WallClockReader`, epoch ms). Tests inject both fakes
(§7.3); no timer calls `Clock::now()` or `system_clock::now()` directly
(11 E21).

**16-D11 (pinned; arming, R-M2, N3-H1/G1).** The watchdog is armed iff
`HostConfig.require_owner && !HostConfig.watchdog_disabled`. `require_owner`
defaults **true** (fail-closed); a non-`foreground` daemon is rejected at startup
(`StartupRejected`) unless `require_owner && !watchdog_disabled` — both opt-out
arms are closed. The hermetic in-process tests opt out explicitly; the
deterministic watchdog test uses `require_owner = true`,
`watchdog_disabled = false`, and injected clocks. `foreground` does not
participate in arming. There is no runtime RPC to disable the watchdog.

**Child reaping (C-L1).** A daemon spawned by this supervisor is its `fork`
child, and under the new model a daemon may exit **while its spawner still
lives** (another supervisor kept it alive until it left, or the watchdog stopped
it). The supervisor installs `SA_NOCLDWAIT` on SIGCHLD — or an explicit
non-blocking `waitpid(-1, WNOHANG)` reaper — so a child that exits first is
reaped and never becomes a zombie. This supersedes the "not a child to be
reaped" half of 04 §3.2 (X1); the supervisor still never **blocks** in
`waitpid`, and `isAlive`/`requestStop` (`HostLauncher`) are unchanged.

### 7.6 UI additions (minimal; binds inventory C1–C5)

```cpp
// DaemonStatus lives in include/ymh/ui/ui_event.hpp:36-41 (add two values);
// OwnershipMark / ExitConfirmState are additive to include/ymh/ui/ui_model.hpp.
enum class DaemonStatus : std::uint8_t {
    Connecting,
    Attached,
    Detached,
    Dead,
    Stopping,       // new: draining after last-exit / watchdog
    NotRunning,     // new: registered, no live daemon (16-D2)
};

// Switcher marker (§3.6); display-only, never in the registry.
enum class OwnershipMark : std::uint8_t { Owned, NotRunning, Stopping, Unreachable };

// Exit confirmation state; reuses the existing dialog overlay (C3, Surface 4).
struct ExitConfirmState {
    bool                    open{false};
    std::vector<WorkspaceId> orphaning;   // daemons that will terminate
    int                     sessions{0};  // display-only aggregate
    int                     running{0};   // display-only aggregate
};
```

**UI consumption of the lifecycle notice (C-H1/O-M5).** The daemon-side emission
(16-D4) is only half the path: today `HostNoticeKind::SessionCreated` maps to
`WorkspaceEvent{workspace, SessionOpened}` (`ui_event_adapter.cpp:332-337`), but
`WorkspaceEvent` carries **no `SessionId`** (`ui_event.hpp:58-62`) and
`UiModel::apply` **no-ops** `SessionOpened`/`SessionClosed` with `break;`
(`ui_model.cpp:610-611`). So §3.3's "the UI already handles it" and O8 are
false, and test 8.4.2(2) is unachievable. Pinned fix:

```cpp
// include/ymh/ui/ui_event.hpp (CHANGED — C-H1/O-M5)
struct WorkspaceEvent {
    WorkspaceId                workspace;
    WorkspaceEventKind         kind{WorkspaceEventKind::DaemonAttached};
    std::optional<SessionId>   session;   // REQUIRED for SessionOpened/SessionClosed
};
```

- `ui_event_adapter.cpp:332-337` must copy `notice.session`
  (`protocol.hpp:220`) into the new field; today it drops it.
- `UiModel::apply` (`ui_model.cpp:600-613`) must implement both cases instead of
  `break;`:
  - `SessionOpened` → insert a `SessionCell` for `*event.session` if absent,
    then `dirty.markAggregate()`.
  - `SessionClosed` → erase the cell and any subscription, then
    `dirty.markAggregate()`.
- `HostNotice` carries only the `SessionId`, not the title/kind, so on
  `SessionCreated` the supervisor issues **one** `session.list`/`session.show`
  refresh to fill the cell. This is a single RPC per peer-created session, not a
  poll — it preserves O8 ("no client polls to notice a change").
- `host.event{SessionClosed}` is broadcast to all Interactive clients (§7.4), so
  a supervisor that never subscribed to a peer's session still removes the cell.

- `UiModel` gains `ExitConfirmState exitConfirm` and an optional
  `std::size_t peerSupervisors` count (badge, C2).
- `SupervisorApp::requestExit()` (`supervisor.cpp:237-242`) becomes: compute the
  orphaning set (§4.1); if empty, `quit_`/`screen_->Exit()` as today; else open
  `exitConfirm` and route key events to it. Ctrl+D (`:713-715`) and `/exit`
  (`:452`) stay thin callers (C3).
- The prompt reuses `render_dialog` (`ui_render.cpp:286-312`) with the `dbox`
  overlay (`:416-421`). It renders only the **counts and the workspace titles**;
  it is not a per-session list (C2).
- The optional peer indicator is a **count badge in the left header segment**
  (after `"ymh · <cwd>"`, `render_header` `:364-372`) or in the switcher title —
  **never** the right slot, which RB-10 owns (C1).
- New commands `/attach` and `/switch` are registered in
  `CommandRegistry::builtin()` (`command_registry.cpp:93-158`) as thin callers
  into the same focus logic the switcher uses. No keybindings inside handlers;
  RB-08 owns generic Tab wiring (Surface 5).
- `AggregateStatus` (`ui_model.hpp:257-262`) keeps its existing cross-workspace
  scope (`ui_model.cpp:225-241`); spec 16 only extends "attached" to include
  daemons discovered after startup (C5).

**O20 — One new overlay.** The last-exit prompt is the only new always-modal UI.
Cross-supervisor visibility reuses the switcher and the aggregate counts; no new
panel, no right-slot content, no per-session list in the bottom row.

### 7.7 `ymh --host` and CLI wiring

- `cli.cpp:151-242` (`--host` entry) sets `config.require_owner = true` (the
  fail-safe default, N3-H1) and `watchdog_disabled = false`, so the watchdog is
  armed (16-D11); `HostLifecycle::configFor` likewise sets `require_owner = true`
  and leaves `watchdog_disabled = false` for a spawned daemon. A non-`foreground`
  daemon that would arm no watchdog is rejected at startup (§7.3 guard). Nothing
  else about startup changes (04 §3.3 order is retained).
- `run_supervisor_entry` (`cli.cpp:329-397`) is rewired (C-H4/C-H2):
  1. mint the per-process `ClientInstanceId` (16-D8) — the same value is used
     for presence registration, `AttachIdentity`, and every
     `SupervisorConnectionConfig.client_instance` (`src/ui/supervisor.cpp:148-155`);
  2. register presence (`SupervisorPresence::registerSelf`);
  3. discover + attach to the live daemon set with
     `AttachIdentity{id, ClientRole::Supervisor}` (no eager
     `ensureRunning(row->id)`, `:352`); the cwd workspace shows `NotRunning` if
     no daemon;
  4. construct `HostLifecycle` + `WorkspaceRegistry` and pass **both** (borrowed,
     caller-owned for the whole `run_supervisor` call — R-L3) plus `identity`,
     `monotonic_clock`, and `wall_clock` into `SupervisorRunOptions` — this is
     the C-H4 spawn seam;
  5. the first prompt calls
     `options.lifecycle->ensureRunning(cwd_ws, options.identity)` (16-D2).
  The one-time multi-workspace attach loop (`:380-390`) becomes the first tick
  of `DaemonSetScanner`; it no longer spawns anything.
- `ymh run` (`cli.cpp` headless path, `11 §10.2 :1049-1057`) sends
  `ClientRole::Automation` **and** derives `ServerProfile::Automation`
  (C-M6); it never registers. It ends its daemon **only by disconnecting** —
  `host.shutdown` is Interactive-only (`protocol.cpp:557-558`, 05 `:791`), so
  Automation cannot (and must not) use the last-exit path (O-M6, §4.5).
- `ymh workspace stop <ws>` (`cli.cpp:279-291`) is rewired (C-M4): hello with
  `ClientRole::Observer` (it owns nothing); query `host.ownership` (bounded 2 s);
  if any live owner exists, require an interactive `[y/N]` confirmation or
  `--force`; only then send `host.shutdown{reason: "workspace_stop"}` (the §4.6
  override). If declined, send nothing and exit 1. It never registers.
- `ymh workspace list` / browse paths (`11 E15`) remain read-only and never
  spawn.

---

## 8. Invariants, failure modes, dsh mapping, test plan

### 8.1 Invariants

| ID | Invariant | Where |
|---|---|---|
| **O1** | **No unsupervised daemon.** No sequence of exits/crashes/signals leaves a `require_owner` daemon serving without an owner beyond §5.2's bound. | §2.7, §5 |
| **O2** | **Collective ownership.** Any live supervisor may drive any live daemon; ownership never gates RPC dispatch. | §2.2 |
| **O3** | **Durable ownership.** The ownership set is the `supervisors` table, written under the D22 flock, read by daemons as WAL. | §2.3 |
| **O4** | **Kernel-mediated fast path.** A live connection is sufficient owner evidence; the registry is the durable fallback. | §2.4 |
| **O5** | **Watchdog arming (fail-safe).** Armed iff `require_owner && !watchdog_disabled`; `require_owner` defaults **true**; a non-`foreground` daemon that would arm no watchdog (`!(require_owner && !watchdog_disabled)`, equivalently `!require_owner \|\| watchdog_disabled`) is `StartupRejected`. | §7.3, 16-D11 |
| **O6** | **Automation is a keepalive owner, not ownership of record.** `ymh run` attaches as `Automation`, never registers, never prompts; it ends its daemon only by disconnect. | §2.7, §4.5 |
| **O7** | **Scope completeness.** A supervisor attaches to every live daemon within one `scan_interval` of it appearing. | §3.1 |
| **O8** | **Live visibility.** Session create/close is announced to every attached Interactive client and rendered by `UiModel::apply` without polling. | §3.3, §7.6 |
| **O9** | **Focus isolation.** Switching never alters a peer's focus, the daemon's active session, or any registry row. | §3.5 |
| **O10** | **Orphaning-set correctness.** A daemon is in the set iff the requester's departure leaves it ownerless. | §4.1 |
| **O11** | **No accidental termination.** Teardown requires an affirmative answer or `--yes`; EOF/SIGINT/closed terminal do not terminate. | §4.2 |
| **O12** | **Daemon-arbitrated shutdown.** No client can terminate a daemon while another owner is observable — except the single user-confirmed `ymh workspace stop` override (§4.6). | §4.4, §4.6 |
| **O13** | **Automation safety.** An `ymh run` task is never killed by a supervisor's exit; it ends only by its own disconnect. | §4.5 |
| **O14** | **No duplicate visibility UI.** Visibility reuses the switcher and aggregate counts. | §3.6 |
| **O15** | **Bounded unsupervised lifetime.** Shutdown requested within 23 s (death) / 38 s (freeze); the process then exits after the ordered teardown or the watchdog self-exit. | §5.2 |
| **O16** | **Reuse, not duplication.** No new socket timeout, signal, or child process. | §5.5 |
| **O17** | **Distinct identity.** Two live supervisors never share a `ClientInstanceId`. | §6.1 |
| **O18** | **Independence.** Ownership staleness and host-claim liveness never affect each other. | §6.5 |
| **O19** | **No self-supersede across peers.** Concurrent supervisors are invisible to the supersede path. | §6.6 |
| **O20** | **One new overlay.** The last-exit prompt is the only new modal UI. | §7.6 |
| **O21** | **Two-clock separation.** Interval math uses `steady_clock`; heartbeat/freshness uses `system_clock` epoch ms; a negative wall delta is stale. | §2.4, 16-D13 |
| **O22** | **Attach identity.** Every attach carries the caller's registered `ClientInstanceId` and role, so the daemon can exclude the caller and route the profile. | §7.3, C-H2 |

### 8.2 Failure modes

The table in §5.2 is normative; this list is the coverage index. All are
component-local `O-F#` (not the top-level F1–F12 of 00 §54).

```text
O-F1  supervisor SIGKILL            O-F9   stale ownership rows
O-F2  supervisor SIGSTOP/freeze     O-F10  spawn, supervisor dies before connect
O-F3  clean last exit               O-F11  ymh run holds the daemon
O-F4  clean exit, peers remain      O-F12  prompt unanswered / terminal closed
O-F5  registry unwritable, alive    O-F13  two supervisors exit concurrently
O-F6  registry unwritable, then die O-F14  attach during teardown
O-F7  registry unreadable           O-F15  clock skew / monotonic wrap
O-F8  machine reboot                O-F16  shared ClientInstanceId (impossible)
```

### 8.3 dsh mapping

dsh is the architectural reference (00 §55). Ownership maps as follows.

| dsh concept | ymh daemon ownership | Reference |
|---|---|---|
| Service registry / discovery | `supervisors` table + `host.ownership` | §2.3, §7.1 |
| Reference-counted service teardown | keepalive predicate `K(d)` (OR of live conn + fresh row) | §2.4, §5.3 |
| Service heartbeat / lease | `supervisors.heartbeat` + `owner_lease_ttl` | §2.3, §5.2 |
| Graceful drain on last client | ordered coordinator + `ShutdownReason::NoOwners` | §4.3, §5.1 |
| Kernel-mediated liveness | sidecar flock (host) + AF_UNIX EOF (owner) | R1, §5.3 |
| Single-writer registry | D22 flock; WAL readers | R6 |
| Hot plugin reload / distributed registry | not modeled (accepted for v1) | 00 §55 |

**Deliberate omissions.** No distributed/remote ownership (a remote supervisor
is just another row when §47 Mode B lands); no per-daemon owner assignment
(collective by construction); no ownership history (only the live set); no
process-scan-based ownership (never on `ps` evidence, R2).

### 8.4 Test plan

Strategy is 00 §44: unit, hermetic integration (Fake LLM / fake FS / fake
shell), golden/replay, and a separate opt-in live layer. The deterministic
layers run offline; the live layer is opt-in (`YMH_LIVE_LLM=1`). **No test may
use `sleep`-based synchronisation** — the repo just fixed a global-`waitpid`
flake in `tests/integration_pty_test.cpp`, so all waits below are
event/condition-variable driven with an injected clock (11 §7.2 `:806-856`).

#### 8.4.1 Unit tests

| Test | Covers |
|---|---|
| `isFresh`: exact `ttl` (fresh), `delta == 0` (fresh), `delta < 0` (**stale**), `delta > ttl` (stale) | O3, O21, §2.4, O-H1 |
| `register`/`heartbeat` (false when absent)/`reRegister`/`deregister`/`list`/`freshCount(exclude)`/`prune` CRUD | §7.1, A9, C-M8 |
| Migration 1→2 on a fixture `registry.db` with real rows: four tables unchanged, `user_version == 2`, `supervisors` empty | §7.2, A10/A11 |
| Read-only open of an **un-migrated v1** DB: opens the old view; freshness queries return `0`/`false` (no throw) | §7.2, C-M9/O-M7 |
| `K(d)` truth table (live∧¬fresh, ¬live∧fresh, both, neither) with both clocks | O4, §5.3 |
| Watchdog arming truth table: armed iff `require_owner && !watchdog_disabled`; `require_owner` defaults **true**; `foreground` irrelevant; in-process test with `require_owner=true` is armed | O5, R-M2, §7.3 |
| Fail-loud guard truth table (both arms): non-`foreground` + (`require_owner=false` **or** `watchdog_disabled=true`) ⇒ `StartupRejected`; `foreground=true` exempt; production constructors (`--host` `cli.cpp:151-242`; `HostLifecycle::configFor` `workspace_host.cpp:852-862`) set `require_owner=true` **and** `watchdog_disabled=false` | O1, N3-H1/G1, §7.3 |
| Watchdog exception guard: a throwing `listSupervisors` ⇒ empty snapshot, loop continues | O-M2, §5.1 |
| `admit_shutdown` truth table: peer connection, automation, snapshot-exclude, reject leaves `Serving`; `workspace_stop` override accepts even with owners | O12, §4.4/§4.6, C-M4 |
| Liveness sink counts only `{Supervisor, Automation}`; an `Observer` never holds `K(d)` | O1, R-M1, §7.4 |
| Fresh snapshot: `other_fresh_owners(caller) = \|snapshot − caller\|`; a stale snapshot cannot reject its own requester | O10, R-H1, §5.1/§7.4 |
| `host.ownership` request shape is `{}` (no `requester` param); identity from `conn.instance` | O-L1, §7.4 |
| `OwnershipView` construction: raw counts incl. caller; `other_fresh_owners` is the only exclude; snapshot (no SQLite) | §7.4, C-M7/R-M3 |
| `ClientRole` parse/serialize + default `Supervisor` + role→profile derivation + mismatch reject | §7.4, C-M6 |
| `all_methods().size() == 29` and `is_known_method("host.ownership")` | O-M4, §7.4 |
| Per-process `ClientInstanceId` mint: two mints differ; one process is stable | O17, 16-D8 |
| `WorkspaceEvent` carries `SessionId`; `UiModel::apply(SessionOpened/SessionClosed)` inserts/erases a cell | O8, C-H1/O-M5 |
| `OwnershipMark` derivation from `DaemonStatus` | §3.6 |
| Orphaning set with fake views: peer row, automation, fresh-owner-only (no attachment), none; `live_supervisors == 1` is a raw count incl. caller | O10, §4.1, R-M3/R-L1 |
| `host.shutdown` reason mapping: `"last_supervisor"` → `LastSupervisor`; `"workspace_stop"` → `WorkspaceStop` (override); else → `ClientRequest` (admission applies) | O-L5, R-L2, §7.4 |
| `ensureRunning`/`connect_checked` use the supplied `AttachIdentity` (no fresh uuid; profile from role) | O22, C-H2, §7.3 |
| Supervisor reap: a daemon child that exits first is reaped by SIGCHLD, no zombie | C-L1, §7.3 |
| Shared ownership constants: `static_assert(ttl ≥ 3×heartbeat)`; `HostConfig` and `SupervisorPresence::Options` defaults come from `core/ownership.hpp` and are equal | O-L7, N2-L7, §7.3/§7.5 |
| `deregister() noexcept` swallows a registry failure (best-effort), never throws | N2-L4, §7.5 |
| Shutdown coordinator joins the owner watchdog before `TransportServer::stop()` | N2-L3, §4.3 |
| `ownershipView(ClientId)` takes no clock/ttl parameters (snapshot is precomputed) | N2-L5, §7.4 |

#### 8.4.2 Hermetic integration (the two-supervisor scenario)

Driven by the two-process harness (`tests/integration_two_process_test.cpp`) with
`ForkExecLauncher` and the Fake LLM.

**Clock injection and which tests are wall-clock (O-M10).** The daemon is
`exec`'d, so a `std::function` clock cannot cross the exec. Two layers:

- **Deterministic (no sleep):** watchdog/grace logic runs **in-process**
  (`foreground = true`, `require_owner = true`, `watchdog_disabled = false`) with
  the injected `HostConfig.owner_monotonic_clock` / `owner_wall_clock` (§7.3).
  The test advances the fake clocks and notifies the watchdog's condition
  variable; it asserts the exact `ShutdownReason` and tick count. This is the
  seam R-M2 makes constructable (`foreground` no longer disarms).
- **Two-process (wall-clock, eventual):** the end-to-end scenario below uses real
  clocks and asserts **eventual** process exit via event-driven waits with
  generous timeouts — never an exact latency, never `sleep`.

1. **Spawn-on-prompt (16-D2).** S1 starts with cwd A and **no daemon**; assert no
   `host.sock` exists and the workspace shows `NotRunning`. Submit a prompt;
   assert the daemon spawns and serves.
2. **Cross-supervisor visibility (16-D4, O7/O8, C-H1).** S1 on A; S2 on B; both
   submit a prompt. Assert both supervisors' models contain both workspaces and
   both sessions; assert the session S2 creates in D_A is announced to S1 via
   `host.event{SessionCreated}` **and** that S1's `UiModel` gains a session cell
   for it (the `UiModel::apply` path, not merely the notice). Close it and assert
   the cell is removed.
3. **Switching (O9).** S2 switches to S1's session; assert S2 replays from
   `beginning` and can submit a follow-up; assert S1's focus and the daemon's
   active session are unchanged; assert no registry row changed.
4. **Non-last exit (O11).** S1 quits while S2 is attached; assert no prompt and
   both daemons still serve; assert S1's row is gone.
5. **Last exit + prompt (O12, C-H2).** S2 quits alone; assert the prompt fires
   (this is the C-H2 regression: the connection's instance must equal the
   registered row, or `other_fresh_owners` is never 0). Assert it lists exactly
   the orphaning daemons; answer "no" → daemons still serve, **S2 still
   registered and running**; answer "yes" → both daemons run the ordered
   teardown and exit, S2 exits 0.
6. **Automation guard (O13).** S1 exits while `ymh run` holds D_A mid-turn;
   assert D_A is not in the orphaning set and the turn completes.
7. **`--yes` (O11).** Headless exit terminates without a prompt.
8. **Fresh-owner-without-attachment (C-L4).** S1 owns D_A but the scanner has not
   attached yet; assert D_A appears in the orphaning set (prompt) rather than
   being silently watchdog-killed.
9. **`ymh workspace stop` override (C-M4).** With S1 attached to D_A, run
   `ymh workspace stop A` non-interactively without `--force`: assert it exits 1
   and D_A still serves (no shutdown sent). With `--force`: assert D_A runs the
   ordered teardown and S1 observes `DaemonShuttingDown`. Also assert that a
   plain `host.shutdown` (empty/unknown reason) from an `Observer` connection
   while S1 is attached is refused with `NotLastOwner` and D_A stays `Serving`.

#### 8.4.3 Crash / failure injection

| Scenario | Method | Assert |
|---|---|---|
| `SIGKILL` a supervisor (O-F1) | kill(2) the harness child | shutdown requested ≤ 23 s of injected time; `ShutdownReason::NoOwners` |
| Freeze a supervisor (O-F2) | `SIGSTOP`; advance fake clocks past `idle_timeout`/`ttl` | shutdown requested ≤ 38 s; on resume the supervisor re-registers + reconnects (C-M8) |
| Wedged coordinator (C-M10) | inject a coordinator that never completes | watchdog self-exits via `std::_Exit` ≤ `kOwnerGraceTeardownBudget` (34 s); no process left |
| Registry unwritable (O-F5/F6) | chmod the registry dir / inject a failing writer | survives while connected; exits ≤ 23 s after disconnect |
| Registry unreadable (O-F7) | inject a failing reader | exception-guarded loop; connection-only fallback; exits ≤ 23 s |
| Stale + negative-delta rows (O-F9/O-F15) | insert old-heartbeat **and** future-heartbeat rows | both stale; pruned by the next scan; watchdog not fooled |
| Spawn-then-die (O-F10) | spawn, kill the supervisor before hello | exits ≤ 23 s |
| Concurrent last-exits (O-F13) | two supervisors quit together | at most one prompt; no daemon ownerless beyond bound |
| Attach during teardown (O-F14) | S2 attaches while S1 shuts down | rejected, or S2 respawns; never half a daemon |
| Reboot (O-F8) | unit-level: rows with old boot ids | no live process assumed; rows pruned |

**Existing M2 tests to rewrite (O-L6).** The following assert the old "daemon
survives supervisor exit" semantics and must be inverted or given a second
supervisor:

- `tests/integration_two_process_test.cpp` — its header/summary asserts the
  daemon keeps serving after the supervisor exits. Rewrite into (a) a
  two-supervisor survival case (S1 exits, S2 attached) and (b) a last-exit
  teardown case.
- `tests/integration_live_e2e_test.cpp` — the live PTY scenario kills/restarts
  the TUI "while the daemon survives" (05 §14.5 `:1503-1505`); rewrite to attach a
  second supervisor or expect the last-owner teardown.
- `tests/unit/workspace_host_test.cpp:66 base_config` — under the fail-safe
  default (`require_owner = true`) this must **opt out explicitly**
  (`require_owner = false` or `watchdog_disabled = true`) to stay a no-watchdog
  test. Add a separate in-process watchdog test with `require_owner = true`,
  `watchdog_disabled = false`, and injected clocks (the seam is constructable).
- Any test asserting `attachedClients() == 0` is a "normal long-lived state"
  (04 §4.2) must be re-scoped to "normal while a fresh owner row exists".

#### 8.4.4 Golden / replay tests

- Golden render of the last-exit prompt (`render_dialog` path) for 0/1/2
  orphaning daemons; assert counts-only text, no per-session list (C2/O20).
- Golden render of the switcher with `OwnershipMark` variants (O14).
- Replay: a `host.event{SessionCreated}` sequence applied twice is idempotent
  in `UiEventAdapter` (10 §5.3).

#### 8.4.5 Live end-to-end (opt-in)

`YMH_LIVE_LLM=1` + `DEEPSEEK_API_KEY`: two supervisors under PTYs against the
real binary, real DeepSeek, one prompt each; assert cross-visibility, switch,
and last-exit teardown against the real process tree (no orphan `ps` entries
after the last exit). This is the only layer allowed to use wall-clock timing,
and it asserts **eventual** process exit, not an exact latency.

#### 8.4.6 Failure-mode coverage matrix

| O-F | Unit | Integration | Crash | Golden | Live |
|---|---|---|---|---|---|
| O-F1 | ✓ | | ✓ | | ✓ |
| O-F2 | ✓ | | ✓ | | |
| O-F3 | ✓ | ✓ | | ✓ | ✓ |
| O-F4 | ✓ | ✓ | | | |
| O-F5 | ✓ | | ✓ | | |
| O-F6 | ✓ | | ✓ | | |
| O-F7 | ✓ | | ✓ | | |
| O-F8 | ✓ | | | | |
| O-F9 | ✓ | ✓ | | | |
| O-F10 | ✓ | | ✓ | | |
| O-F11 | ✓ | ✓ | | | |
| O-F12 | ✓ | | ✓ | | |
| O-F13 | ✓ | ✓ | ✓ | | |
| O-F14 | ✓ | ✓ | | | |
| O-F15 | ✓ | | | | |
| O-F16 | ✓ | ✓ | | | |

#### 8.4.7 Invariant coverage

| Invariant | Tests |
|---|---|
| O1/O15 | 8.4.3 (all rows) |
| O2/O3/O4 | 8.4.1 `K(d)`, CRUD |
| O5/O16 | 8.4.1 arming derivation + watchdog seam; 8.4.3 |
| O6/O13 | 8.4.2 (6) |
| O7/O8 | 8.4.2 (2) |
| O9 | 8.4.2 (3) |
| O10/O11/O12 | 8.4.2 (4,5,7,8), 8.4.3 |
| O14/O20 | 8.4.4 |
| O17/O19 | 8.4.1 mint test, 8.4.2 (1) |
| O18 | 8.4.3 (stale rows) |
| O21 | 8.4.1 `isFresh` negative-delta; 8.4.3 two-clock skew |
| O22 | 8.4.1 `ensureRunning` identity; 8.4.2 (5) |

---

## 9. Open questions and decisions

Each item states the question, the recommendation, and the consequence of the
opposite choice. Items already pinned are listed for auditability.

| # | Question | Recommendation | If rejected |
|---|---|---|---|
| **16-D1** | Does a daemon survive the last owner? | **No** (X3/X4). | Reverts to M2; the requirement is unmet. |
| **16-D2** | Eager or lazy spawn? | **Lazy on first prompt** (§3.2.1). | Eager spawn restores the startup stall and the spawn-without-use orphan. |
| **16-D3** | Supervisor scope? | **Whole live daemon set, continuously scanned.** | Snapshot scope misses daemons created after startup (the user's scenario). |
| **16-D4** | How do peers learn of new sessions? | **Emit `host.event{SessionCreated}` to all Interactive clients.** | Polling `session.list` adds latency and load and contradicts O8. |
| **16-D5** | Who decides last-owner shutdown? | **The daemon, via admission.** | A supervisor-only check has the attach race (§6.2). |
| **16-D6** | Non-interactive confirmation? | **`--yes` / `no_prompt`.** | Scripts cannot exit a last supervisor cleanly. |
| **16-D7** | Orphan backstop? | **Daemon-side watchdog + registry ownership.** | Connection-only strands restarts; registry-only strands on disk-full. |
| **16-D8** | `ClientInstanceId` persistence? | **Per-process, in memory** (A8). | Persistence collides for concurrent supervisors (O17 broken). |
| **16-D9** | Exit ordering? | **Query (registered) → prompt → deregister (on confirm) → shutdown → close.** | Deregister-before-query leaves a cancelling supervisor deregistered (C-H3 broken). |
| **16-D10** | Timer clocks? | **Injected `MonotonicClock` (intervals) + `WallClockReader` (heartbeat).** | `Clock::now()`/one clock makes tests nondeterministic or freshness always-fresh (O-H1, 11 E21). |
| **16-D11** | Watchdog switch? | **`require_owner && !watchdog_disabled`; `require_owner` defaults true (fail-safe); non-`foreground` opt-out is rejected.** | Fail-open defaults leave a daemon silently unsupervised (N2-H1); a bare `require_owner` check contradicts the test seam (R-M2). |
| **16-D12** | Ownership transport? | **Registry row, not daemon memory.** | A crashed supervisor cannot be observed (§2.3). |
| **16-D13** | Clock discipline? | **Two clocks, never mixed: `steady_clock` for intervals, `system_clock` epoch ms for heartbeat/freshness; a negative wall delta is stale.** | One clock makes `FRESH_OWNER` always-fresh or tests nondeterministic (O-H1/C-M1, §2.4). |

### 9.1 OQ-1 — Grace and TTL tuning

The worst-case **shutdown-request** latency is 23 s (death) / 38 s (freeze),
including the grace quantization (§5.2). This is bounded but not instant.
Tightening requires lowering the transport `idle_timeout` (30 s, shared with
generic clients) or adding an owner-specific silence timeout. **Recommendation:**
keep 23/38 s for v1; the freeze case dominates and is rare. **Revisit if** the
user wants faster orphan cleanup; the constants are `HostConfig` fields (§7.3)
and the change is local.

### 9.2 OQ-2 — Finish an in-flight turn before terminating?

**Recommendation: no.** Waiting for a turn would make the unsupervised lifetime
unbounded (a turn is bounded only by LLM timeouts). The clean-exit prompt warns
that in-flight turns are cancelled; sessions are persisted and restart intact.
**Alternative considered:** drain the turn up to `shutdown_grace` — rejected
because `shutdown_grace` (10 s) already bounds the drain and a longer wait
contradicts O1.

### 9.3 OQ-3 — Schema migration vs. an older binary

`SCHEMA_VERSION = 2` means an older `ymh` binary refuses the upgraded
`registry.db` (03 §3.4 backward policy). **Recommendation:** accept it — the
harness is pre-1.0 and daemon/supervisor are always the same build. The
forward-compat half is already fixed: read-only opens tolerate an **un-migrated
v1** DB (§7.2, C-M9/O-M7), so `ymh list`/`show` and the scanner never fail
before a supervisor migrates. **If** a mixed-version window is ever required, the
`supervisors` table can be created lazily on first supervisor registration
instead of at open time; that would keep `user_version = 1` for old readers.
Deferred.

### 9.4 OQ-4 — Peer-supervisor indicator placement

C1 forbids the header right slot (RB-10). **Recommendation:** a count badge in
the left header segment, or the switcher title. A list of peer supervisors is
**not** recommended (C2 counts-only). Deferred to RB-10/RB-11 coordination.

### 9.5 OQ-5 — Are `/attach` and `/switch` needed?

The switcher already focuses any session (inventory Surface 3).
**Recommendation:** register `/attach` (spawn/attach the cwd daemon without a
prompt) and `/switch <workspace>` (open the switcher focused) as thin command
entries, but treat them as optional polish; the switcher is the primary path.
RB-08 owns Tab wiring.

### 9.6 OQ-6 — Should a long `ymh run` register a presence row?

**Recommendation: no.** Registration is the interactive-owner concept; an
automation client's live connection already keeps the daemon alive (O-F11) and
its lifetime is bounded by the task. Registering it would make it appear in the
peer badge and the last-exit math as an "owner", which it is not (O6).

### 9.7 OQ-7 — Registry write contention

With N supervisors, each writes one heartbeat every 5 s plus scan prunes,
serialized on the D22 flock. **Recommendation:** accept for N ≤ ~8 (the team
bound); a missed heartbeat is harmless (`FRESH_OWNER` is an OR with
`LIVE_CONN`). **If** contention appears, batch heartbeats or lengthen the
interval; not a v1 concern.

### 9.8 OQ-8 — Watchdog clock source (two clocks, pinned)

**Recommendation (pinned as 16-D13, O-H1/C-M1):** two clocks, never mixed. The
watchdog uses the injected **monotonic** reader (`MonotonicClock`,
`steady_clock`) for `LIVE_CONN` frame age and `owner_grace`, and the injected
**wall** reader (`WallClockReader`, `system_clock` epoch ms) for
`FRESH_OWNER`/`isFresh`. A negative `delta = w − heartbeat` is **stale**
(fail-safe). Epoch-ms freshness across processes requires roughly synchronized
wall clocks; on a single machine (the v1 scope) this holds. Remote supervisors
(§47 Mode B) would need a server-time comparison; deferred with the transport.

### 9.9 OQ-9 — Clean detach via `host.detach`?

The supervisor currently just closes the socket. `host.detach`
(`protocol_server.cpp:352-355`) is an explicit close. **Recommendation:** on a
non-last clean exit, send `host.detach` and await the reply (bounded), so the
daemon's `live_owner_count_` drops deterministically before the socket close.
Optional; socket close is already correct.

### 9.10 OQ-10 — Remote supervisors

Out of scope (00 §47 Mode B). The model already accommodates them: a remote
supervisor is a `supervisors` row plus a connection, and `host.ownership` is
transport-agnostic. The only new concern is cross-host clock skew (OQ-8) and
socket-loss detection over TCP, both deferred.

### 9.11 OQ-11 — `owner_lease_ttl ≥ 3 × owner_heartbeat_interval` (O-L7)

The three-missed-tick rationale requires this relation. **Pinned:** `HostConfig`
validates it at startup and refuses to start with
`HostExitCode::StartupRejected` if violated (the `--host` entry uses the
defaults, so this only guards a misconfiguration). It is also asserted in a unit
test (§8.4.1).

### 9.12 OQ-12 — Role is self-asserted (O-L8)

`ClientRole` is not authenticated; a same-UID client can claim `Supervisor`. The
trust boundary is the same-UID `0600` socket (05 §4.2). **Pinned:** ownership
liveness trusts same-UID peers; only a fresh `supervisors` row makes a client an
ownership-of-record supervisor. No cross-UID hardening in v1. Revisit if a
shared-user or multi-user host becomes a target.

---

## 10. Revision log

Revision 1 closes the findings from the spec-critic and Oracle-gate reviews of
revision 0 (both FAIL: 6 HIGH, 22 MEDIUM ids covering 16 distinct findings, 13
LOW). Revision 1a closed `C-M4` and `O-L1`; revision 2 (§10.5) closes the new
findings the re-reviews raised. Every finding id below maps to its disposition
and the section(s) changed. "Fixed" means the spec text was changed; "retained"
means no change with justification.

### 10.1 HIGH

| ID | Disposition | Sections changed |
|---|---|---|
| **O-H1** | Fixed — two clocks pinned: `steady_clock` for intervals, `system_clock` epoch ms for heartbeat/freshness; `delta < 0` is stale | §2.4, §5.1, §5.2 (O-F15), §7.1, §7.3, §9.8; new 16-D13 and O21 |
| **O-H2** | Fixed — admission is an **io-thread pre-check before `requestShutdown`**; the coordinator no longer checks; reject ⇒ `NotLastOwner`, no `Draining`, no notice | §4.3 (deleted coordinator step 1), §4.4 (normative), §7.4 |
| **C-H1** | Fixed — `WorkspaceEvent` gains `std::optional<SessionId>`; `ui_event_adapter` copies `notice.session`; `UiModel::apply` inserts/erases the cell | §3.3, §7.6, O8; test 8.4.2(2) |
| **C-H2** | Fixed — `AttachIdentity{ClientInstanceId, ClientRole}` threaded through `HostLifecycle::ensureRunning`/`connect_checked`; `SupervisorConnection`/`ymh run` use the pinned id and role | §2.6, §7.3, §7.5, §7.7; new O22; §1.3 X1 |
| **C-H3** | Fixed — one ordering: **query (registered) → prompt → deregister only after confirm → shutdown → close**; empty set: query → deregister → close | §4.2, §4.3, §6.2, §6.3, 16-D9 |
| **C-H4** | Fixed — `SupervisorRunOptions` gains `HostLifecycle*` + `WorkspaceRegistry*` + `identity` + clock readers; the first prompt spawns | §7.5, §7.7, §2.6 |

### 10.2 MEDIUM

| ID | Disposition | Sections changed |
|---|---|---|
| **O-M1** | Fixed — one constant set everywhere: heartbeat 5 s, ttl 15 s, watchdog 2 s, grace 5 s | §2.4, §5.2 |
| **O-M2** | Fixed — watchdog body is exception-guarded; failure ⇒ `fresh=false`, loop continues | §5.1, test 8.4.1 |
| **O-M3** | Fixed — `ProtocolServer::set_owner_liveness_sink` pins the two atomics' writer | §5.1, §7.4 |
| **O-M4** | Fixed — `kMethodCatalog` 28 → 29 with `kHostOwnership`; size test updated | §7.4, test 8.4.1 |
| **O-M5** | Fixed — same as C-H1 (UI consumption), both ends | §3.3, §7.6 |
| **O-M6** | Fixed — `host.shutdown` is Interactive-only; Automation ends by disconnect only | §4.5, §7.4, §7.7 |
| **O-M7** | Fixed — `openReadOnly` tolerates an un-migrated v1 DB (old schema view, freshness queries return zero) | §7.2, §3.2 |
| **O-M8** | Fixed — "owner" redefined as keepalive owner (Supervisor / Automation / fresh row); Automation is a bounded, intentional hold; precondition stated in O1 | §2.7, §4.5, O1, O6, §5.2 |
| **O-M9** | Fixed — same as C-H3 (exit ordering) | §4.2, §6.3, 16-D9 |
| **O-M10** | Fixed — daemon clock seam pinned (`HostConfig.owner_monotonic_clock`/`owner_wall_clock`); deterministic vs wall-clock test layers stated | §7.3, §8.4.2 |
| **O-M11** | Fixed — §1.3 extended: X1 (child/peer clause), X6 (04 §1.2:71-72), X7 (00 §9.9:1103-1104), A13 (05 T-F21:1351), A14 (05 §14.5:1503-1505) | §1.3.1, §1.3.2 |
| **C-M1** | Fixed — same two-clock fix as O-H1; `WallClockReader` pinned | §2.4, §7.1, §7.3, 16-D10 |
| **C-M2** | Fixed — same constant unification as O-M1 | §2.4, §5.2 |
| **C-M3** | Fixed — grace quantization counted: `watchdog_latency = 2 + ceil(5/2)*2 = 8 s`; bounds 23 s / 38 s | §5.2, O15, OQ-1 |
| **C-M4** | Fixed — admission now applies to **every** `host.shutdown` (not only `LastSupervisor`); `ymh workspace stop` is the one documented override, requiring confirmation/`--force`; O12 names the exception | §4.4, §4.6, §7.3, §7.4, §7.7, O12; tests 8.4.1/8.4.2(9) |
| **C-M5** | Fixed — watchdog armed iff `require_owner && !foreground`; unit test pinned (rev 2 supersedes `!foreground` → `!watchdog_disabled`, §10.5 R-M2) | §7.3, 16-D11, test 8.4.1 |
| **C-M6** | Fixed — `role == Automation ⇒ profile == Automation`; mismatch rejected; `ymh run` sends Automation | §7.4, §7.7, test 8.4.1 |
| **C-M7** | Fixed — admission uses the watchdog's cached fresh count; no SQLite on the io thread (rev 2 supersedes the count cache → fresh-id snapshot, §10.5 R-H1) | §4.4, §7.4 |
| **C-M8** | Fixed — false heartbeat ⇒ re-register + reconnect; added to O-F2 | §2.6, §5.4, O-F2 |
| **C-M9** | Fixed — same as O-M7 (read-only tolerance) | §7.2, OQ-3 |
| **C-M10** | Fixed — bounded escalation: watchdog self-`_Exit`s after the escalation deadline if the coordinator wedges (rev 5 raises the deadline to `kOwnerGraceTeardownBudget`, R4-L4) | §5.1, test 8.4.3 |
| **C-M11** | Fixed — same as O-M8 (Automation bounded hold) | §2.7, §4.5 |

### 10.3 LOW

| ID | Disposition | Sections changed |
|---|---|---|
| **O-L1** | Fixed — `host.ownership` takes **no request params** (`{}`); identity is the connection's `conn.instance`; `ownershipView` takes `ClientId caller` (daemon-internal, not a wire field) | §4.1, §7.4 |
| **O-L2** | Fixed — O-F12/O-F13 rationale corrected (SIGHUP ⇒ immediate EOF ⇒ 23 s, not 38 s) | §5.2 |
| **O-L3** | Fixed — O15 now says shutdown is **requested** within the bound; process exit follows teardown or escalation | §5.2, O15 |
| **O-L4** | Fixed — `require_owner && !foreground` derivation (rev 2 supersedes `!foreground` → `!watchdog_disabled`, §10.5 R-M2) | §7.3, 16-D11 |
| **O-L5** | Fixed — `host.shutdown` reason-string → `ShutdownReason` mapping pinned | §7.4 |
| **O-L6** | Fixed — existing M2 tests to rewrite are inventoried | §8.4.3 |
| **O-L7** | Fixed — `owner_lease_ttl ≥ 3 × owner_heartbeat_interval` validated at startup | §7.3, OQ-11 |
| **O-L8** | Fixed — same-UID trust assumption stated explicitly; role advisory, row = ownership-of-record | §7.4, OQ-12 |
| **C-L1** | Fixed — supervisor installs `SA_NOCLDWAIT`/SIGCHLD reaper; "not a child to be reaped" superseded | §7.5, §1.3 X1, test 8.4.1 |
| **C-L2** | Fixed — `boot_id` used as the PID-reuse guard on re-register (mirrors 03 R5) | §7.1 |
| **C-L3** | Fixed — scanner cost corrected: `probeLiveness` opens+flocks per workspace per tick | §3.2 |
| **C-L4** | Fixed — orphaning set includes fresh-owner rows with no attachment | §4.1, test 8.4.2(8) |
| **C-L5** | Fixed — `ownership_query_timeout` (2 s) pinned for the exit-path query | §4.1 |

### 10.4 Disputed findings

None. Every delivered finding (including `C-M4` and `O-L1`, corrected in this
amendment) was accepted and is now **fixed**. No retained-with-justification
entries remain in §10.1–§10.3, and no not-delivered ids remain.

### 10.5 Revision 2 — re-review findings (rev 1a → rev 2)

Both re-reviews confirmed the rev-1 closure work (critic 40/41, Oracle 21/21).
The findings below are new defects introduced by rev 1a. All are **fixed**.

#### 10.5.1 HIGH

| ID | Disposition | Sections changed |
|---|---|---|
| **R-H1** = **N-H1** | Fixed — the watchdog now publishes an **immutable fresh-id snapshot** (`std::shared_ptr<const std::vector<SupervisorId>>`, atomically swapped) as the single producer; `other_fresh_owners(caller) = \|snapshot − caller\|`; admission and `ownershipView` read the snapshot, not a bare count. This fixes the primary exit path (deregister → `host.shutdown{last_supervisor}` no longer rejects its own requester from a stale count) and lets `other_fresh_owners` reach 0 so the last-exit prompt fires. Also folds in **R-L4** (one exclusion notion: `caller`/`caller_conn`). | §4.1, §4.4 (code + notes), §5.1 (loop builds/publishes the snapshot), §7.4 (`OwnershipView`, `ownershipView`, C-M7 bullet) |

#### 10.5.2 MEDIUM

| ID | Disposition | Sections changed |
|---|---|---|
| **R-M1** = **N-L9** | Fixed — the liveness sink counts only `role ∈ {Supervisor, Automation}` (the `LIVE_CONN` set); an `Observer` cannot hold `K(d)` | §7.4 (sink bullet), §2.4, test 8.4.1 |
| **R-M2** = **N-M2** | Fixed — one arming rule: `require_owner && !watchdog_disabled`; `foreground` no longer participates, so the deterministic in-process watchdog test is constructable (rev 3 supersedes the `require_owner` default polarity → fail-safe true, §10.6 N2-H1) | §2.5, O5, §5.1, §7.3 (`HostConfig` + bullet), §7.7, 16-D11, §8.4.1/§8.4.2/§8.4.3, §9 row |
| **R-M3** = **N-L3** | Fixed — `OwnershipView.live_supervisors`/`live_automation` are **raw counts including the caller**; only `other_fresh_owners` excludes; §4.1's `== 1` comment corrected | §4.1, §4.4, §7.4 |
| **N-M1** | Fixed — §9's 16-D9 row rewritten to the corrected order (query → prompt → deregister → shutdown → close) | §9 |
| **N-M3** | Fixed — pinned two io-thread seams: `HostRuntime::freshOwnerSnapshot()` and `HostRuntime::requestShutdown(ShutdownReason)` | §7.4 |

#### 10.5.3 LOW

| ID | Disposition | Sections changed |
|---|---|---|
| **N-L1** | Fixed — "phantom owner" softened to "**persistent** phantom owner" with the transient-refresh caveat | §2.4 |
| **N-L2** | Fixed — glossary `Owner`/`Observer` aligned with the `K(d)` keepalive definition and R-M1 | §1.5 |
| **N-L4** | Fixed — §1 ranges corrected to `O1`–`O22` and `16-D1`–`16-D13` | §1 |
| **N-L5** | Fixed — header/§10 counts now say "22 MEDIUM ids (16 distinct findings)" | header, §10 intro |
| **N-L6** | Fixed — `DaemonStatus` cited to `ui_event.hpp:36-41`, not `ui_model.hpp` | §7.6 |
| **N-L7** | Fixed — the 30 s idle default is `host_connection.hpp:40` (`:37` is the 5 s handshake default) | §4.1 |
| **N-L8** | Fixed — `ClockReader` replaced by `MonotonicClock`/`WallClockReader`; 16-D11 row updated | §9 |
| **N-L10** | Fixed — the ill-typed workspace-scoped `fresh_owner_row` removed; the global-table candidate rule is used | §4.1 |
| **N-L11** | Fixed — the wire string is `"workspace_stop"` everywhere; the old code's `"workspace stop"` is called out as renamed | §4.4, §4.6, §7.4, §7.7 |
| **R-L1** | Fixed — the candidate set is "live-daemon workspaces discovered but not attached", confirmed with the supervisor's own `freshSupervisorCount(exclude = my_id)` | §4.1 |
| **R-L2** | Fixed — the reason-mapping unit test now covers all three cases (`last_supervisor`, `workspace_stop`, else) | §8.4.1 |
| **R-L3** | Fixed — `HostLifecycle*`/`WorkspaceRegistry*` pinned as caller-owned and required to outlive the whole `run_supervisor` call | §7.5, §7.7 |
| **R-L4** | Fixed — folded into R-H1: one exclusion notion (`caller`/`caller_conn`) throughout admission | §4.4 |

#### 10.5.4 Disputed (round 2)

None. All round-2 findings were accepted and fixed. No retained-with-justification
entries and no not-delivered ids in this round.

### 10.6 Revision 3 — round-3 re-review findings (rev 2 → rev 3)

Both round-3 reviewers confirmed the rev-2 snapshot refactor (critic 7/7;
Oracle 14/15). The findings below are the remaining defects. All are **fixed**.

#### 10.6.1 HIGH

| ID | Disposition | Sections changed |
|---|---|---|
| **N2-H1** = **N2-M1** | Fixed — **fail-safe default**: `require_owner{true}` (was false/opt-in), so an unconfigured daemon always has a watchdog; hermetic tests opt out explicitly (`require_owner=false` or `watchdog_disabled=true`). Added a fail-loud startup guard on `require_owner == false`; the guard's **completeness** was fixed in rev 4 (N3-H1/G1, §10.7). O1 now fails **closed**. | §2.5, O5, §7.3 (HostConfig + guard bullet), §7.7, 16-D11, §8.4.1, §8.4.3 |

#### 10.6.2 MEDIUM

| ID | Disposition | Sections changed |
|---|---|---|
| **N-M3** = **N2-M2** | Fixed — the seams are retyped and re-homed: they live on **`TransportHost`** (`protocol_server.hpp:153` holds `TransportHost&`, not `HostRuntime&`); `freshOwnerSnapshot()` returns `std::shared_ptr<const std::vector<protocol::ClientInstanceId>>`; `requestShutdown(std::string)` becomes `requestShutdown(protocol::ShutdownReason)` with a new transport-mirror enum and a one-to-one daemon mapping. `SupervisorId`/`ClientInstanceId` are no longer mixed. | §7.4 (enum + TransportHost sketch + prose), §1.3 A15, `transport/host.hpp:12-14` reconciled |

#### 10.6.3 LOW

| ID | Disposition | Sections changed |
|---|---|---|
| **N2-L1** | Fixed — the `freshSupervisorCount` comment no longer claims to be the daemon's `FRESH_OWNER`; it is the supervisor's own registry read (R-L1); the daemon uses the snapshot | §7.1 |
| **N2-L2** | Fixed — `cli.cpp:148-155` → `src/ui/supervisor.cpp:148-155` | §7.5, §7.7 |
| **N2-L3** | Fixed — the coordinator now **joins the owner watchdog before `TransportServer::stop()`** (step 7); renumbered steps and added the ordering bullet | §4.3 |
| **N2-L4** | Fixed — `deregister() noexcept` pinned as best-effort with an internal catch; failure ages out/prunes | §7.5 |
| **N2-L5** | Fixed — `ownershipView(ClientId caller)` dropped the vestigial `now_wall_ms`/`owner_lease_ttl` params (snapshot is precomputed) | §7.4 |
| **N2-L6** | Fixed — the unattached-candidate case documents that `live_automation` is unobservable, the prompt may over-list, and admission rejects while a live Automation client exists | §4.1 |
| **N2-L7** | Fixed — one shared constant header `include/ymh/core/ownership.hpp` with a `static_assert`; `HostConfig` and `SupervisorPresence::Options` default from it | §7.3, §7.5 |

#### 10.6.4 Disputed (round 3)

None. All round-3 findings were accepted and fixed. No retained-with-justification
entries and no not-delivered ids in this round.

### 10.7 Revision 4 — round-4 re-review findings (rev 3 → rev 4)

Both round-4 reviewers confirmed the rev-3 work (critic 8/9; Oracle N2-L1..N2-L7
all fixed). The findings below are the remaining defects. All are **fixed**.

#### 10.7.1 HIGH

| ID | Disposition | Sections changed |
|---|---|---|
| **N3-H1** = **G1** | Fixed — the fail-loud guard is now **complete**: reject iff `!foreground && !(require_owner && !watchdog_disabled)` (both opt-out arms closed). Rev 3 tested only `require_owner`, so `require_owner=true, watchdog_disabled=true` passed and armed nothing. Guard runs **first, before any state** (L3), so no H21 release is needed. §8.4.1 pins the full truth table and asserts both production constructors set `require_owner=true` **and** `watchdog_disabled=false`. | §2.5 (step 1'), §7.3 (guard bullet), O5, §7.7, 16-D11, §8.4.1 |

#### 10.7.2 MEDIUM

| ID | Disposition | Sections changed |
|---|---|---|
| **G3** | Fixed — the seam retyping is pinned end-to-end: the **single string→enum parse** is at `ProtocolServer`'s `kHostShutdown`; `setShutdownHook`/`shutdown_hook_` become `std::function<void(ymh::ShutdownReason)>`; `HostRuntime::requestShutdown` does the mirror→native map; the concrete callers/tests to change are listed. `last_supervisor`/`workspace_stop` can no longer degrade to `ClientRequest`. | §7.4 (typed-shutdown bullet + new "concrete edits" bullet + reason-mapping bullet) |
| **N3-M1** = **L2** | Fixed — `escalate_to_stop()` is now **non-blocking** (arm + return); the self-`_Exit` is a later loop step gated on `escalation_deadline_`. Added the `requestWatchdogStop()`/`watchdog_stop_`+`notify_all` handshake the coordinator runs **before** `join()`, so the join is prompt and steps 8-9 still run on the orphan path (rev 5 corrects the "bounded by one `watchdog_interval`" wording to an unconditional untimed join, §10.8 N4-L2). | §5.1 (loop + escalation/stop-join bullets), §4.3 (step 7 + bullet) |

#### 10.7.3 LOW

| ID | Disposition | Sections changed |
|---|---|---|
| **G2** = **N3-L1** | Fixed — deleted the stale "(the default)" from the "Disarmed for tests" bullet | §5.1 |
| **L1** | Fixed — the O-M8 row's unescaped `\|` replaced with `/` | §10.2 |
| **L3** | Fixed — the guard moved to step 1' **before any state**, so no `claimHost`/H21 release is needed | §2.5, §7.3 |
| **L4** | Fixed — `supervisor_presence.hpp` now includes `core/ownership.hpp` and `registry/registry.hpp` | §7.5 |
| **L5** | Fixed — `deregister()` comment reworded to "inspects the registry call's return value" | §7.5 |
| **L6** | Fixed — added §1.3 **A16** (05 §7.2's `host.shutdown` is now conditional); renamed the round-3 HIGH to **N2-H1** so `N-H1` is no longer overloaded | §1.3, §10 |

#### 10.7.4 Disputed (round 4)

None. All round-4 findings were accepted and fixed. No retained-with-justification
entries and no not-delivered ids in this round.

### 10.8 Revision 5 — round-5 LOW-only polish (rev 4 → rev 5)

The spec passed the gate on rev 4 (zero open HIGH/MEDIUM). This pass fixes only
the five remaining LOW items; no verified content was restructured. All are
**fixed**.

| ID | Disposition | Sections changed |
|---|---|---|
| **N4-L1** = **R4-L2** | Fixed — the complete guard predicate `!(require_owner && !watchdog_disabled)` now appears in the `HostConfig` comment and the O5 row, matching §2.5/§7.3/§8.4.1 | §7.3 (HostConfig comment), §8.1 (O5 row) |
| **N4-L2** = **R4-L3** | Fixed — step 7 is pinned as an **unconditional, untimed `std::thread::join()`** (never `join_for`); the bound comes from the `requestWatchdogStop()` handshake, not a timeout. Added the non-blocking/bounded-loop-body assumption (bounded `listSupervisors` read; interruptible `cv.wait_for`) | §4.3 (step 7 + bullet), §5.1 (stop/join bullet) |
| **R4-L1** | Fixed — added `host_runtime.hpp:214` (the `shutdown_hook_` member) to the concrete-edits list and an include-or-opaque-enum note (`host_runtime.hpp:29-49`) | §7.4 |
| **R4-L4** | Fixed — escalation deadline raised from `shutdown_grace + watchdog_interval` (≈12 s) to `kOwnerGraceTeardownBudget = 3 × shutdown_grace + 2 × watchdog_interval` (34 s), covering the full orphan-path teardown; alternative one-absolute-deadline form noted. Escalation can no longer fire mid-teardown | §5.1 (constant + bullet + loop), §5.2 (O15), §8.4.3 |
| **R4-L5** | Fixed — header now names `N3-L1/G2, L1, L3..L6` (L2 is a MEDIUM; G2 is distinct); added the missing **16-D13** row to §9 | header, §9 |

#### 10.8.1 Disputed (round 5)

None. All five LOWs were accepted and fixed. No retained-with-justification
entries and no not-delivered ids in this round.



