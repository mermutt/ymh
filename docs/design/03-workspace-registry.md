# 03 — Workspace Registry

**Component 03 of 10.** The workspace registry is the shared, durable **open-set**
of workspaces and their sessions: the canonical-path ↔ `WorkspaceId` index, the
ordered session junction, and the host (daemon) registration. It is the one
database that multiple processes touch, and it deliberately sits *outside* the
event log (D21, §54) and *outside* any single daemon (§9.10). This document pins
the registry DDL, the `WorkspaceRegistry` interface, the single-writer rule
(D22), host heartbeat/liveness, one-time bootstrap discovery, the best-effort
process-scan fallback, the `pending_mutation` crash-recovery protocol (DIV-3),
invariants (`R1`–`R16`), failure modes (`F1`–`F12` plus local `R-F#`), the
DeepSeek Harness (dsh) mapping, and the test plan. It follows `00-architecture.md`
(cited inline as `§n`), `01-session.md` (cited as `01 §n`), and
`02-persistence.md` (cited as `02 §n`); where it cannot follow them it records
the conflict under §15 rather than choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
   Supervisor (TUI)              WorkspaceHost daemon A          CLI (ymh workspace add)
        │ read-only WAL               │ read + write                   │ write
        └──────────────┬──────────────┴───────────────┬───────────────┘
                       ▼                              ▼
        ┌────────────────────────────────────────────────────────────┐
        │  WorkspaceRegistry  (this spec)                             │
        │    registry.db  ·  registry.lock  (flock, D22)              │
        │    workspaces · workspace_sessions · pending_mutation · meta │
        └────────────────────────────────────────────────────────────┘
                       ▲                              ▲
                       │ logical refs (no cross-DB FK) │
        <workspace>/.ymh/sessions.db        <workspace>/.ymh/sessions.lock
        (spec 02: events · leases)          (spec 02: sidecar flock = liveness anchor)
```

The registry is the durable **membership and ordering** authority for the open
set. It is *not* the event store: session events, leases, and snapshots live in
each workspace's `sessions.db` (spec 02, §9.2). It is *not* UI state: the
focused/active session is supervisor-local (§20.22). One daemon owns one
workspace and one cwd (D18, §9.7), so the daemon is the natural canonical writer
for its own workspace's session-list rows, while the supervisor reads the whole
open set and writes only host claim/heartbeat rows (§9.10).

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 03)

This spec pins:

- The shared `${XDG_STATE_HOME:-~/.local/state}/ymh/registry.db` and its
  `registry.lock` sidecar (§9.10).
- The `workspaces`, `workspace_sessions`, and `pending_mutation` tables, exactly
  as fixed by §9.10, plus this spec's `registry_meta` bookkeeping table (§3.3).
- The `WorkspaceRegistry` interface (open / read-only open / mutations / reads).
- The single-writer rule (D22): `flock(LOCK_EX)` on `registry.lock`; WAL readers
  never block; per-row-domain write ownership (§5).
- Host registration, heartbeat, and **liveness** (§6): sidecar `flock` primary,
  `host_boot_id` match, `kill(pid, 0)` and heartbeat staleness as hints only.
- One-time **bootstrap discovery** from `workspace_roots` and the best-effort
  **process-scan fallback** (§7).
- The **`pending_mutation`** crash-recovery protocol (DIV-3, §8).
- The boundary between the registry and 02's per-workspace DBs (§3.5, §9).

### 1.3 Boundaries — deferred to other specs

| Concern | Owner | Why |
|---|---|---|
| Session events, leases, snapshots, per-workspace DDL/pragmas | 02 | `<workspace>/.ymh/sessions.db`; §9.2, §9.10 |
| `SessionHeader`, `SessionManager`, `SessionId` | 01 | 01 §2–§3, §8 |
| JSON-RPC framing, socket protocol, authenticated handshake | 05 | §9.6, §43 |
| `WorkspaceHost` daemon lifecycle (spawn, detach, shutdown) | 04 | §9.6, §9.8, D23 |
| `WorkspaceModel`, `UiModel`, focused/active session | 10 | §20.22 |
| Config loading of `workspace_roots` | 10/CLI | §37; this spec consumes the resolved list |
| Permission/LLM/subprocess/PTY caps | 06/07/09 | §9.11; registry only counts live daemons |

### 1.4 Seam ownership relative to 01/02

- 01 owns `SessionId` and `WorkspaceId` as strong UUID value types (01 §2.1).
  This spec **reproduces them as frozen** and adds registry-local value types
  (`HostBootId`, `HostClaim`, `WorkspaceRecord`, `WorkspaceSessionRecord`,
  `PendingMutation`).
- 02 owns the per-workspace store and the sidecar `sessions.lock` (02 §5.1).
  This spec **consumes** that lock as the liveness anchor and never opens,
  flocks, or writes it except to *probe* it (§6.3).
- The registry holds **no** session event, lease, snapshot, permission, or
  focused-session state (R1). A `workspace_sessions.session_id` is a **logical**
  reference, not a cross-database foreign key (§3.5, R9).

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId` and `WorkspaceId` are frozen by 01 §2.1; `WorkspaceId` is a UUIDv4
and is **never** the path (§9.7). Registry-local types:

```cpp
namespace ymh {

// Frozen by 01 §2.1 — reproduced for reference, do not redefine.
struct WorkspaceId {                     // UUIDv4; stable; never a path (§9.7)
    std::string value;
    auto operator<=>(const WorkspaceId&) const = default;
};

// Registry-local value types.
using HostPid = std::int32_t;            // > 0 always; 0 is never stored (§3.3, R3)

struct HostBootId {                      // UUIDv4 minted at daemon startup (§9.7, §9.10)
    std::string value;
    auto operator<=>(const HostBootId&) const = default;
};

struct HostClaim {                       // the host-registration half of a workspaces row
    WorkspaceId                workspace;
    HostPid                    pid;          // > 0 (R3)
    HostBootId                 bootId;
    std::filesystem::path      socketPath;   // Unix socket for IPC (§9.6, spec 05)
};

struct WorkspaceRecord {
    WorkspaceId                 id;             // UUIDv4; never the path
    std::filesystem::path       canonicalPath;  // realpath (DIV-8, §9.7)
    std::string                 displayTitle;
    std::int64_t                createdAt;      // epoch ms
    std::int64_t                updatedAt;      // epoch ms
    std::optional<HostClaim>    host;           // nullopt ⇔ host_pid IS NULL
    std::optional<std::int64_t> heartbeatAt;    // epoch ms; nullopt ⇔ never
    std::optional<std::string>  metadata;       // opaque JSON; no schema commitment
};

struct WorkspaceSessionRecord {
    WorkspaceId  workspace;
    SessionId    sessionId;
    std::int64_t ordinal;      // ordering key; strictly increasing, NOT dense (§11 R8)
    bool         archived;     // list/search concern, not a tab concern (§9.8)
    std::int64_t createdAt;    // epoch ms
};

enum class MutationType : std::uint8_t { Create, Delete, Reorder };

struct PendingMutation {
    WorkspaceId   workspace;   // PK: at most one marker per workspace (§8)
    MutationType  type;
    std::string   payload;     // opaque JSON; schema pinned in §8.2
    std::int64_t  timestamp;   // epoch ms
};

} // namespace ymh
```

`HostPid` is signed so that "no host" is represented by a **SQL NULL** and a
`std::nullopt`, never by `0` (R3). `metadata` is an opaque JSON string, exactly
as `SessionHeader.metadata` is opaque (01 §3); the registry never decodes it
into a typed schema.

### 2.2 Registry error taxonomy

```cpp
namespace ymh {

enum class RegistryErrorCode : std::uint8_t {
    OpenFailed,          // missing dir / unopenable DB / lock file unwritable
    SchemaVersion,       // foreign application_id, or user_version > SCHEMA_VERSION
    Corrupt,             // integrity/foreign-key failure, unmarked order mismatch
    LockUnavailable,     // could not take flock(LOCK_EX) after bounded retries
    Uninitialized,       // read-only open before the `initialized` marker (§7)
    UnknownWorkspace,    // no row for the id / canonical path
    DuplicatePath,       // canonical_path collision not reconcilable (R4)
    WorkspaceNotEmpty,   // removeWorkspace with junction rows remaining
    HostClaimed,         // claim/reap conflict with a live holder
    MutationInProgress,  // a pending_mutation marker already exists for the workspace
    NotWriteLockHolder,  // a mutation attempted without the D22 lock
};

class RegistryError final : public std::runtime_error { /* code + message */ };

} // namespace ymh
```

Every fallible registry operation throws `RegistryError` with a code from this
taxonomy; the process-scan fallback and bootstrap distinguish `Uninitialized`,
`OpenFailed`, and `Corrupt` because those are the three conditions §9.10 names as
fallback triggers (§7.4, R-F1/R-F2).

---

## 3. Database layout and schema (pinned)

### 3.1 Location and files

```text
${XDG_STATE_HOME:-~/.local/state}/ymh/registry.db      SQLite database
${XDG_STATE_HOME:-~/.local/state}/ymh/registry.db-wal  WAL (SQLite-managed)
${XDG_STATE_HOME:-~/.local/state}/ymh/registry.db-shm  shared memory (SQLite-managed)
${XDG_STATE_HOME:-~/.local/state}/ymh/registry.lock    D22 flock + holder diagnostic
```

The `ymh/` state directory is created with mode `0700` if absent; `registry.db`
and `registry.lock` are created with mode `0600`. The registry path is derived
from `XDG_STATE_HOME` (or `~/.local/state`), **never** from `getcwd()` (§9.7,
02 P16). This is separate from each per-workspace
`<workspace>/.ymh/sessions.db` (§9.10). It is small: workspace records plus the
session junction, a one-row-per-workspace crash marker, and a fixed-size
bookkeeping table.

### 3.2 Connection pragmas (required)

Mirroring 02 §3.2, applied immediately after every `sqlite3_open_v2` (writer and
read-only connections alike):

```sql
PRAGMA foreign_keys = ON;            -- workspace_sessions.workspace_id FK (§9.10)
PRAGMA journal_mode = WAL;           -- required by §9.10; readers never block the writer
PRAGMA busy_timeout = 5000;          -- 5 s, then SQLITE_BUSY (bounded retry)
PRAGMA synchronous = NORMAL;         -- survives an application/process crash (02 §3.2)
PRAGMA wal_autocheckpoint = 1000;    -- SQLite default; explicit so it is pinned
PRAGMA application_id = 0x594D4802;  -- 'Y','M','H', registry schema-family 2 (§3.4)
```

`foreign_keys=ON`, `journal_mode=WAL`, and `busy_timeout` are non-negotiable
(§9.10). `application_id` `0x594D4802` is distinct from the per-workspace
`sessions.db` id `0x594D4801` (02 §3.2) so that bootstrap discovery and
diagnostics can tell the two files apart. On a read-only connection the same set
is applied except that `application_id` is only **read** for the foreign-file
check and `journal_mode=WAL` is a no-op on a DB already in WAL.

### 3.3 DDL (pinned)

The DDL below is reproduced from §9.10, which now carries the same host-claim
`CHECK`s and `registry_meta`; the two are identical. `registry_meta` holds
schema/bootstrap bookkeeping only and carries no open-set state (decision (b),
§15.1).

```sql
CREATE TABLE workspaces (
    id              TEXT PRIMARY KEY,      -- WorkspaceId (UUIDv4, stable)
    canonical_path  TEXT NOT NULL UNIQUE,  -- realpath() of the workspace dir
    display_title   TEXT NOT NULL,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL,
    host_pid        INTEGER,               -- WorkspaceHost daemon; NULL if none, never 0
    host_boot_id    TEXT,                  -- daemon's per-boot UUID (PID-reuse guard)
    host_socket     TEXT,                  -- Unix socket path for IPC
    host_heartbeat  INTEGER,               -- last heartbeat (epoch ms)
    metadata        JSON,
    CHECK (host_pid IS NULL OR host_pid > 0),
    CHECK ((host_pid IS NULL) = (host_boot_id IS NULL)),
    CHECK ((host_pid IS NULL) = (host_socket IS NULL)),
    CHECK ((host_pid IS NULL) = (host_heartbeat IS NULL))
);

CREATE TABLE workspace_sessions (
    workspace_id    TEXT NOT NULL,
    session_id      TEXT NOT NULL,
    ordinal         INTEGER NOT NULL,      -- ordering key; strictly increasing, not dense
    archived        INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    PRIMARY KEY (workspace_id, session_id),
    FOREIGN KEY (workspace_id) REFERENCES workspaces(id),
    CHECK (archived IN (0, 1))
);

CREATE INDEX idx_ws_sessions ON workspace_sessions(workspace_id, ordinal);

CREATE TABLE pending_mutation (            -- dsh-style crash recovery marker (§8)
    workspace_id    TEXT PRIMARY KEY,      -- at most one marker per workspace
    mutation_type   TEXT NOT NULL,         -- 'create' | 'delete' | 'reorder'
    payload         JSON NOT NULL,
    timestamp       INTEGER NOT NULL,
    CHECK (mutation_type IN ('create','delete','reorder'))
);

CREATE TABLE registry_meta (               -- schema/bootstrap bookkeeping only
    key             TEXT PRIMARY KEY,      -- 'initialized' | 'legacy_migrated' | ...
    value           TEXT NOT NULL
);
```

Notes, all load-bearing:

- **`host_pid` NULL sentinel (R3).** A workspace with no live daemon stores
  `host_pid = NULL`, `host_boot_id = NULL`, `host_socket = NULL`,
  `host_heartbeat = NULL`. The value `0` is never stored and is rejected by the
  `CHECK`. The four host columns move together: a claim is either fully present
  or fully absent. `host_heartbeat` is written at claim time (so a claim always
  has a heartbeat) and then refreshed each tick (§6.2).
- **`canonical_path` is `realpath` (DIV-8, §9.7).** It is the unique key;
  `WorkspaceId` is never derived from it. A symlink to an owned directory
  collides with it by string equality of the canonical path (R4, R-F7).
- **`pending_mutation` has no foreign key** to `workspaces`, by design: the
  marker must remain resolvable while the workspace row is itself mid-mutation,
  and a marker may legitimately outlive a rolled-back `create` (§8).
- **`ordinal` is not dense (R8).** It is an ordering key, not an array index:
  `ORDER BY ordinal ASC` defines the open set; gaps are expected and allowed;
  no code may assume contiguity, and the registry never claims gapless ordinals
  (mirroring 01 §16.1(b) and 02 P6 for `Sequence`).
- **`archived` is junction state, not header state** (01 §3, §9.8). Bootstrap
  from a legacy per-workspace DB cannot recover it and defaults it to `0` (§7.1).

### 3.4 Schema versioning and migration

The registry uses SQLite's `PRAGMA user_version` as the schema version and
`PRAGMA application_id` (`0x594D4802`) as the file-family magic.

```text
SCHEMA_VERSION = 1     (the DDL in §3.3)

open:
  app_id := PRAGMA application_id
  if app_id != 0 && app_id != 0x594D4802     -> RegistryError{SchemaVersion} (foreign DB)
  v := PRAGMA user_version
  if v == 0                                  -> fresh DB: apply §3.3, set v=1
  if v == SCHEMA_VERSION                     -> ready
  if v < SCHEMA_VERSION                      -> migrate forward in order, in ONE
                                                transaction; on failure ROLLBACK and
                                                raise RegistryError{SchemaVersion}
  if v > SCHEMA_VERSION                      -> RegistryError{SchemaVersion} (newer
                                                binary wrote this DB); refuse to open
```

Forward policy (normative), mirroring 02 §3.4:

- Migrations are **additive first**: new tables, new nullable columns, new
  indexes. A migration **must not** `UPDATE`/`DELETE` `workspaces`,
  `workspace_sessions`, or `pending_mutation` rows, and must not rewrite
  `ordinal`. If a payload shape changes, it is handled by the JSON payload's own
  evolution.
- Each migration runs in its own transaction and bumps `user_version` inside
  that transaction, so an interrupted migration leaves the DB at the old version
  (R-F18).
- A destructive migration is allowed only for `registry_meta` bookkeeping, never
  for the three open-set tables.

Backward policy (normative): **no backward compatibility is promised.** An older
binary that sees `user_version > SCHEMA_VERSION` refuses with
`RegistryError{SchemaVersion}`; it never downgrades. A mismatched binary is a
deployment error, not a data-loss event.

### 3.5 Relationship to 01/02's per-workspace DBs

The two databases have disjoint authority:

| Concern | Authority | Notes |
|---|---|---|
| Session existence, events, leases, snapshots | `<workspace>/.ymh/sessions.db` (02) | §9.2; the event log is the durable source of truth (D2) |
| Workspace identity, canonical path, title | `registry.db` `workspaces` (this spec) | §9.10; `WorkspaceId` is never the path |
| Open-set membership, `ordinal`, `archived` | `registry.db` `workspace_sessions` (this spec) | §9.10, 01 §3 |
| Host pid/boot-id/socket/heartbeat | `registry.db` `workspaces` (this spec) | §9.10 |
| Focused/active session | supervisor-local `WorkspaceModel` (spec 10) | §20.22; **never** registry state |

`workspace_sessions.session_id` is a **logical** reference. SQLite cannot enforce
a foreign key across database files, and the registry must remain readable even
when a workspace's `sessions.db` is missing, corrupt, or on an unmounted path.
Therefore:

- The registry is authoritative for **membership**; the per-workspace DB is
  authoritative for **existence**. A junction row whose session is absent from
  its `sessions.db` is an inconsistency (`R-F12`), never a registry corruption.
- The **daemon** (canonical writer for its own session list) reconciles the two
  at startup: it imports missing `sessions` rows into the junction and removes
  dangling junction rows, using the `pending_mutation` protocol (§8). The
  supervisor never edits another workspace's junction rows (R16).
- Deleting a session removes its junction row only **after** the daemon has
  committed `SessionEnded` and erased the session in `sessions.db` (F3, §9.8);
  the junction removal is the last step, so a crash never leaves a registry row
  pointing at a session that no longer exists.

---

## 4. The `WorkspaceRegistry` interface (pinned)

### 4.1 Configuration

```cpp
namespace ymh {

struct RegistryConfig {
    std::filesystem::path db_path;    // <state>/ymh/registry.db
    std::filesystem::path lock_path;  // <state>/ymh/registry.lock

    // Writer serialization (§5).
    std::chrono::milliseconds busy_timeout{5'000};
    std::chrono::milliseconds lock_retry_budget{2'000};   // total wait for flock(LOCK_EX)
    std::chrono::milliseconds lock_retry_interval{25};

    // Host heartbeat (§6.2). Staleness is a HINT only; it never clears a claim.
    std::chrono::milliseconds heartbeat_interval{5'000};   // §9.10
    std::chrono::milliseconds heartbeat_stale_hint{15'000}; // 3 missed ticks

    // One-time bootstrap (§7.1). Consumed from the resolved §37 config.
    std::vector<std::filesystem::path> workspace_roots;    // default {"$HOME/prjs"}
    int                                bootstrap_depth{4};
    bool                               allow_process_scan_fallback{true};
};

} // namespace ymh
```

`db_path`/`lock_path` are derived from `XDG_STATE_HOME` (or `~/.local/state`),
never from `getcwd()` (R16, §9.7). `workspace_roots` is the resolved §37 list;
the registry does not parse config files itself.

### 4.2 Class shape

```cpp
namespace ymh {

class WorkspaceRegistry {
public:
    // ---- construction ------------------------------------------------------
    // Writer open: creates the DB/dir if absent, applies §3.2 pragmas, runs the
    // §3.4 version gate, then under the D22 write lock runs pending-mutation
    // resolution (§8) and, if the `initialized` marker is absent, bootstrap (§7).
    static std::unique_ptr<WorkspaceRegistry> open(const RegistryConfig&);

    // Read-only open: SQLITE_OPEN_READONLY, no create, no flock, no migration,
    // no recovery write, no bootstrap. Coexists with the writer via WAL.
    static std::unique_ptr<WorkspaceRegistry> openReadOnly(const RegistryConfig&);

    ~WorkspaceRegistry();

    // ---- reads (no lock; WAL snapshot) ------------------------------------
    bool                                   isInitialized() const;
    std::vector<WorkspaceRecord>           listWorkspaces() const;              // ORDER BY canonical_path
    std::optional<WorkspaceRecord>         findById(WorkspaceId) const;
    std::optional<WorkspaceRecord>         findByCanonicalPath(const std::filesystem::path&) const;
    std::vector<WorkspaceSessionRecord>    listSessions(WorkspaceId) const;     // ORDER BY ordinal ASC
    std::optional<WorkspaceSessionRecord>  findSession(WorkspaceId, SessionId) const;
    std::optional<PendingMutation>         pendingMutation(WorkspaceId) const;

    // ---- mutations (require the D22 write lock; §5) ------------------------
    WorkspaceRecord registerWorkspace(const std::filesystem::path& canonicalPath,
                                      std::string displayTitle,
                                      std::optional<std::string> metadata = std::nullopt);
    void            removeWorkspace(WorkspaceId);                     // refuses if non-empty/hosted
    void            setDisplayTitle(WorkspaceId, std::string);

    // Host claim/heartbeat — the supervisor's ONLY write set (§9.10, R16).
    void            claimHost(HostClaim);
    void            heartbeat(HostClaim);                             // no-op if claim no longer matches
    void            releaseHost(WorkspaceId, HostBootId);

    // Open-set junction — canonical writer is the workspace's daemon (R16).
    WorkspaceSessionRecord addSession(WorkspaceId, SessionId);
    void                   removeSession(WorkspaceId, SessionId);
    void                   archiveSession(WorkspaceId, SessionId, bool archived);
    void                   reorderSession(WorkspaceId, SessionId, std::int64_t newOrdinal);

    // ---- maintenance / introspection --------------------------------------
    bool            holdsWriteLock() const;      // did THIS call acquire the flock?
    void            resolvePendingMutations();   // idempotent; also run by open()
};

} // namespace ymh
```

Every mutation re-acquires `flock(LOCK_EX)` on `registry.lock` for its duration
(§5) and runs inside a `BEGIN IMMEDIATE` transaction. Reads take no flock and see
a WAL snapshot.

### 4.3 Open / read-only policy

- `open()` creates the state directory (`0700`) and DB if absent, applies the
  §3.2 pragmas, runs the §3.4 version gate, then takes the D22 write lock once
  to run **open-time maintenance**: `resolvePendingMutations()` (§8) followed by
  `bootstrap()` if `registry_meta['initialized']` is absent (§7). The lock is
  released before `open()` returns.
- `openReadOnly()` uses `SQLITE_OPEN_READONLY`, never creates a file, never
  takes the flock, runs no migration and no recovery write, and never
  bootstraps. If the DB is missing it throws `RegistryError{OpenFailed}`; if it
  is present but pre-`initialized` it throws `RegistryError{Uninitialized}`
  (which is the signal the supervisor uses to consider the process-scan fallback,
  §7.4).
- Both opens verify `application_id` and `user_version`; a foreign or newer file
  throws `RegistryError{SchemaVersion}` (R13).

### 4.4 Mutation semantics

- `registerWorkspace` canonicalizes `canonicalPath` with
  `std::filesystem::canonical()` before comparing; on a canonical-path collision
  it returns the existing `WorkspaceRecord` instead of creating a second row
  (R4, R-F7). `display_title` defaults to the basename of the canonical path
  when empty.
- `removeWorkspace` throws `WorkspaceNotEmpty` if any junction row remains and
  `HostClaimed` if a live claim exists; it never orphans sessions or reaps a
  live daemon. It is a CLI/supervisor action, not a daemon action.
- `claimHost` validates `pid > 0` and that `bootId`/`socketPath` are non-empty;
  it is the only way a claim is created, and it stamps `host_heartbeat` at claim
  time (so a claim always has a heartbeat, §3.3, §6.1). It is idempotent for the
  same `(workspace, pid, bootId)`.
- `heartbeat` updates `host_heartbeat` only when the stored
  `(host_pid, host_boot_id)` still equals the caller's; otherwise it is a no-op
  (the caller has been superseded and must re-claim).
- `releaseHost` deletes the claim only when `(host_pid, host_boot_id)` matches;
  it never clears another instance's claim.
- `addSession` assigns `ordinal = max(ordinal)+1` (or `0` for the first) and
  refuses if a `pending_mutation` marker already exists for the workspace
  (`MutationInProgress`). `removeSession` deletes the junction row and bumps the
  workspace `updated_at`. `reorderSession` rewrites the affected ordinals. All
  three write a `pending_mutation` marker first (§8).

---

## 5. Single-writer rule (D22)

### 5.1 The `registry.lock` `flock`

The registry's cross-process writer serialization is an exclusive `flock` on
`registry.lock`, exactly as D22/§9.10 require:

```text
open(lock_path, O_RDWR|O_CREAT|O_CLOEXEC, 0600)
for each mutation:
    flock(fd, LOCK_EX)                      -- held only for this mutation
    BEGIN IMMEDIATE; <mutate>; COMMIT;
    flock(fd, LOCK_UN)
```

- **Per-write critical section, not process-lifetime ownership.** §9.10 says
  "exactly one process holds `flock(LOCK_EX)`" *and* makes each daemon the
  canonical writer for its own session list *and* lets the supervisor write host
  claims. Those are only consistent if the lock is held for the duration of each
  write, so that exactly one process holds it **at any instant**. This reading is
  pinned in decision (a), §15.1; the alternative (a single lifetime writer
  process through which all writes are routed) is rejected because §9.10
  explicitly names the daemon as the canonical writer.
- The lock is acquired with a bounded retry budget (`lock_retry_budget`,
  `lock_retry_interval`); on exhaustion the operation throws
  `RegistryError{LockUnavailable}`. A mutation is **never** performed without the
  lock; `holdsWriteLock()` exposes the state for diagnostics/tests.
- `BEGIN IMMEDIATE` inside the lock is the DB-level backstop; the flock is the
  process-level serialization D22 mandates and the marker the supervisor takes
  before reaping an orphan (§7.3).
- `registry.lock` carries a best-effort diagnostic
  `{"pid":…, "boot_id":…, "acquired_at":…}` written on acquisition, mirroring 02
  §5.1. Authority is the kernel flock, never the file contents.

### 5.2 WAL readers never block

Readers (supervisor TUI, CLI `ymh list`/`ymh show`) use `openReadOnly()`, take no
flock, and issue plain read transactions. Under `journal_mode=WAL` a reader never
blocks the writer and the writer never blocks a reader (P9-equivalent); a reader
sees only committed state (R15). A long-running reader holds a WAL snapshot and
may pin the WAL; the registry's writes are small, so `wal_autocheckpoint=1000`
plus SQLite's checkpointing bounds the WAL in practice.

### 5.3 Who writes what (row-domain ownership)

| Writer | Rows it may mutate | Rows it must never mutate |
|---|---|---|
| `WorkspaceHost` daemon (canonical for its workspace) | its `workspace_sessions` rows; its own host claim/heartbeat at startup/shutdown | other workspaces' junction rows; `canonical_path`; `registry_meta` (except bootstrap if it is the first writer) |
| Supervisor | `workspaces.host_pid/host_boot_id/host_socket/host_heartbeat` via `claimHost`/`releaseHost`/`heartbeat`; `workspaces.display_title`; `removeWorkspace`; `registerWorkspace` | any `workspace_sessions` row of a live workspace |
| CLI `ymh workspace add` | `registerWorkspace` (+ its sessions, under the marker protocol) | host columns of a live workspace |
| Any first writer | `registry_meta` bootstrap keys (§7) | — |

The supervisor's write set is confined to host-registration and
workspace-identity columns; it never edits the ordered session list of a
workspace owned by a daemon (R16). If the supervisor needs a session-list change
it asks the daemon (spec 04), which writes it as the canonical writer.

---

## 6. Host registration, heartbeat, and liveness

### 6.1 Claim fields

A host claim is the tuple
`(workspace_id, host_pid, host_boot_id, host_socket, host_heartbeat)` in the
`workspaces` row; `host_heartbeat` is stamped at claim time (the `CHECK` ties it
to `host_pid`, §3.3). `HostBootId` is a UUIDv4 minted once at daemon startup
(02 §5.2), written both to `session_leases.holder_boot_id` (02) and to
`workspaces.host_boot_id` (this spec). It is a PID-reuse guard: a recycled PID
with a different boot nonce is never mistaken for the holder (R5, §9.7, §9.10).

### 6.2 Heartbeat registration

On startup the daemon canonicalizes its directory, acquires its sidecar
`sessions.lock` flock (02 §5.1), then calls `claimHost`; it refreshes
`host_heartbeat` every `heartbeat_interval` (default 5 s, §9.10). The supervisor
may also `claimHost` when it spawns a daemon (to record the spawn before the
daemon is up) and `releaseHost` when it reaps one; such a supervisor-written
claim is **provisional** and never by itself establishes liveness — liveness
still requires the sidecar flock (R5), and the daemon's own claim (written after
it holds the flock) supersedes it on the first tick. `host_heartbeat` is epoch ms.

`heartbeat_stale_hint` (default 15 s, three missed ticks) is a **hint** used to
raise suspicion and to schedule a liveness check; it is never sufficient to clear
a claim (R5).

### 6.3 Liveness: sidecar flock primary, `kill` hint

Liveness is established exactly as §9.7/§9.10 specify:

```text
probeWorkspaceLock(canonical_path) -> {Absent, HeldBy(pid, boot_id)}
    open(<workspace>/.ymh/sessions.lock, O_RDWR|O_CREAT|O_CLOEXEC, 0600)
    if flock(fd, LOCK_EX|LOCK_NB) succeeds:
        release immediately; return Absent
    if errno == EWOULDBLOCK:
        read the best-effort diagnostic {pid, boot_id}; return HeldBy(...)

killHint(pid) -> Alive | Dead | Unknown      # kill(pid, 0): ESRCH => Dead only
```

Rules (R5, R6, R-F4, R-F5, R-F6):

1. **Primary test — the sidecar flock.** The daemon holds
   `<workspace>/.ymh/sessions.lock` for its whole lifetime (02 §5.1); the kernel
   releases it automatically on process death, including `SIGKILL` (§9.7). The
   lock is the same kernel object that anchors the write lease, so host liveness
   and lease liveness share one authority (§9.10).
2. **Boot-nonce cross-check.** The recorded `host_boot_id` is compared against
   the live holder's boot nonce as reported by the sidecar lock diagnostic. The
   diagnostic is best-effort and non-authoritative (02 §5.1), but the *pairing*
   is what prevents PID reuse from matching a recycled PID to the claim.
3. **`kill(pid, 0)` is a hint, never the sole test.** Only `ESRCH` proves death;
   `EPERM` means "exists but not ours" ⇒ alive; a `SIGSTOP`ped daemon still
   returns `0` and, crucially, still holds the sidecar flock (the kernel does not
   release a flock on `SIGSTOP`). A frozen holder is therefore never stolen
   (02 §5.4, R-F6).
4. **Heartbeat staleness is a hint, never sufficient.** A stale heartbeat alone
   never clears a claim (§9.10).
5. **Claim clearing rule (pinned, R5).** A held sidecar lock always means the
   workspace is live; the diagnostic is never authority. A claim is cleared
   **only when the sidecar lock is absent AND the recorded `host_boot_id` no
   longer matches a live holder's boot nonce**:
   - Lock **absent** ⇒ no daemon holds the workspace ⇒ the recorded holder is
     dead and its claim is stale (the "nonce cannot match" condition is
     satisfied vacuously because there is no live holder to match).
   - Lock **held** and diagnostic `(pid, boot_id)` **matches** the row ⇒ live,
     same holder; the claim is valid.
   - Lock **held** but diagnostic `(pid, boot_id)` **differs** ⇒ the diagnostic
     is best-effort and non-authoritative (rule 2), so a held lock still means
     the workspace is live. The row is **not** re-claimed from the diagnostic;
     the flock holder's own `claimHost`/heartbeat (R6) updates it on its next
     tick. The stale row is re-claimed only after the lock is absent, or after
     an authenticated handshake (spec 05) confirms no holder — never from the
     diagnostic alone.
6. **Clearing/re-claiming is a mutation.** Any write that clears or re-claims is
   performed under the D22 write lock. A daemon always acquires its sidecar flock
   **before** it writes its own claim, so a daemon-written claim always names the
   current lock holder (R6); a supervisor-written spawn claim is provisional and
   never establishes liveness by itself.

### 6.4 Lazy clearing

Claims are cleared **lazily on next access** (§9.10): the supervisor's read path
observes a claim, probes the sidecar lock, and if the claim is stale it performs
`reapHost` under the write lock. There is no eager sweeper and no claim TTL that
mutates the registry on a timer. A read-only registry cannot reap; it reports the
claim as `Stale` and leaves the mutation to a writer.

### 6.5 Process-scan fallback (best-effort only)

Pinned verbatim to §9.10. The scan is attempted **only** when the registry is
missing, corrupt, or predates `initialized` (`RegistryError{OpenFailed|Corrupt|
Uninitialized}`):

```text
enumerate /proc/*/cmdline (or ps -eo pid=,args=)
keep a process iff:
    executable basename of argv[0] == "ymh"     (exact match, never a substring)
    argv contains the explicit host flag triple:
        --host --workspace <uuid> --socket <path>
seed candidate PIDs ONLY; never trust the self-reported --workspace/--socket
for each candidate:
    ws     := registry row if the registry is readable
              else the canonical workspace root on disk
    socket := registry.host_socket if present
              else derived independently from the workspace root by the fixed
              convention <workspace>/.ymh/host.sock (see below)
    confirm by connecting to the derived socket and completing the authenticated
        JSON-RPC handshake (spec 05), or by probing the workspace flock (§6.3)
    trust a candidate only after confirmation
```

Hard rules (R10, R11, R-F13):

- `argv` is used **only to seed candidate PIDs**; the DB/socket path is derived
  **independently** — from the registry row or from the workspace root — and
  **never** from the scanned `argv`. `ps` output is self-reported, spoofable, and
  racy.
- A process scan is **never a discovery source** for the open set: it only sees
  currently-running daemons and cannot reconstruct the durable ordered junction
  (§9.10, R10).
- A process scan **never kills** a daemon on `ps` evidence alone. Reaping an
  orphan requires **lock-absence on the independently derived DB/workspace** and
  a **`host_boot_id` mismatch**, and the supervisor **must take the D22 registry
  write lock before mutating anything** (R11).

**Socket-path convention.** The transport spec (05) owns the JSON-RPC protocol
and the final socket convention. For the fallback to derive the path
independently, this spec uses `<workspace>/.ymh/host.sock` as a **v1 default,
not a protocol commitment**, consistent with the per-workspace `.ymh/` directory
and with the `workspaces.host_socket` column (§9.10, §20.22). Cross-check: if
spec 05 changes the convention, this derivation (and §15.1(l)) must change with
it.

---

## 7. Bootstrap and discovery

### 7.1 One-time bootstrap (first run)

On first run the `registry_meta['initialized']` marker is absent and there is no
canonical-path index to walk, so the discovery source is the configured
`workspace_roots` list (§37). Pinned exactly as §9.10:

```text
bootstrap(config):                              # under the D22 write lock
  if registry_meta['initialized'] present: return   # idempotent no-op
  roots := config.workspace_roots
           if empty -> {"$HOME/prjs"}
  if a root does not exist: skip it (no auto-discovery for that root)
  for each root:
      walk to bounded depth (default 4)
      skip denylist directories: .git node_modules .cache Library target build .venv
      for every directory D containing D/.ymh/sessions.db:
          canonicalD := canonical(D)
          rows := read sessions table of D/.ymh/sessions.db   (spec 02 schema)
          order rows newest-first by (created_at DESC, updated_at DESC, session_id ASC)
          upsert workspace(canonical_path = canonicalD)
          insert workspace_sessions rows with contiguous ordinals from 0
          (archived defaults to 0: it is not recoverable from a legacy DB)
  optional one-time legacy central store import -> mark registry_meta['legacy_migrated']
  write registry_meta['initialized'] = <epoch ms>     # LAST
```

Rules:

- `workspace_roots` unset ⇒ default `["$HOME/prjs"]`; if that path does not
  exist there is no auto-discovery and the user runs
  `ymh workspace add <path>` (§9.10, R-F11).
- **Newest-first import.** A discovered DB's sessions are grouped by canonical
  `cwd` into per-directory workspaces and inserted newest-first (descending
  `created_at`, deterministic tie-break on `updated_at` then `session_id`),
  starting at ordinal `0` (R8 — the ordinals are contiguous here only because
  they are freshly assigned; density is still not guaranteed later).
- **`initialized` is written last**, so an interrupted bootstrap resumes safely:
  the walk is idempotent (`canonical_path` UNIQUE; junction `PRIMARY KEY`
  upserts), and a re-run completes it (R14, R-F10).
- **A process scan is NOT a discovery source** (§9.10). It only sees running
  daemons, never the durable ordered open-set.
- The optional **legacy central store** import runs at most once and is recorded
  in `registry_meta['legacy_migrated']`; it is deferred for v1 (decision (m),
  §15.1): the legacy format is either defined later or dropped, and the marker
  keeps the gate idempotent.

### 7.2 Manual registration: `ymh workspace add <path>`

`ymh workspace add <path>` calls `registerWorkspace` after
`std::filesystem::canonical(path)`. It also imports that workspace's
`sessions.db` rows (newest-first) if the DB exists, so a manually added
workspace joins the open set exactly as a bootstrap-discovered one does. It is
idempotent: re-adding an existing canonical path attaches to the existing
workspace and imports any new sessions (R-F16).

### 7.3 Legacy migration

An optional one-time legacy central store may be imported after the initial
bootstrap; once imported it is marked migrated in `registry_meta` and never
re-imported. The exact legacy schema is not specified by the architecture, so
the import is deferred for v1 (decision (m), §15.1) and gated behind the
explicit marker; it is best-effort and never blocks the open set.

### 7.4 Discovery vs. liveness

These are two different questions and are **never conflated** (§9.10):

- **Open-set discovery** — which workspaces and sessions exist, and in what order
  — is **durable state, not process state**. The `workspaces` rows plus the
  `workspace_sessions.ordinal` junction are authoritative, seeded by the one-time
  bootstrap above. Process inspection cannot reconstruct the ordered junction,
  so it is never the source of truth (R10).
- **Liveness** — is a host still alive? — is established by the sidecar `flock`
  plus the `host_boot_id` cross-check (§6.3). `kill(pid, 0)` and heartbeat
  staleness are hints. A `ps`/procfs scan is no stronger than `kill(pid, 0)` and
  is therefore never the sole liveness test (R5).

The process-scan fallback exists **only** for the degraded case where the durable
index is unavailable (missing/corrupt/pre-`initialized`); even then it is
best-effort, confirmation-gated, and never kills (§6.5).

---

## 8. Pending mutations (DIV-3)

### 8.1 Protocol

A junction mutation touches the session record and the workspace ordering. The
registry writes a `pending_mutation` marker **before** the record+order writes
can diverge, so that a crash between intent and effect is recoverable:

```text
T1  BEGIN; INSERT INTO pending_mutation(workspace_id, mutation_type, payload,
                                        timestamp) VALUES (...); COMMIT;
T2  BEGIN IMMEDIATE;
      <record + order writes>
      DELETE FROM pending_mutation WHERE workspace_id = ?;
    COMMIT;
```

- `T1` durably records the intent. `T2` applies the mutation **and clears the
  marker atomically**, so the marker is present after a crash **iff** the data
  writes did not commit.
- `pending_mutation.workspace_id` is the PK, so at most one marker exists per
  workspace; a new mutation for a workspace with an unresolved marker is refused
  with `MutationInProgress` (R7, R-F8).
- `T1` and `T2` both run under the D22 write lock (§5.1); the marker is never
  written or cleared without it.

### 8.2 Marker payload (pinned)

```json
// mutation_type = "create"
{ "workspace_id": "...", "session_id": "...", "new_ordinal": 7,
  "prev_ordinals": { "<session_id>": <ordinal>, "...": 0 } }

// mutation_type = "delete"
{ "workspace_id": "...", "session_id": "...",
  "prev_ordinals": { "<session_id>": <ordinal>, "...": 0 } }

// mutation_type = "reorder"
{ "workspace_id": "...", "session_id": "...", "from_ordinal": 3, "to_ordinal": 7,
  "prev_ordinals": { "<session_id>": <ordinal>, "...": 0 } }
```

`prev_ordinals` is the full pre-mutation ordinal map for the workspace, so
resolution is deterministic and idempotent without consulting any other state.
`payload` is stored as opaque JSON; the registry validates it against this schema
before use and rejects anything else as `Corrupt` (R-F8).

### 8.3 Startup resolution

On the writer's `open()` (and on demand via `resolvePendingMutations()`), exactly
the marked mutation is resolved:

```text
for each row in pending_mutation:                  # at most one per workspace
    parse payload; on failure -> RegistryError{Corrupt}
    switch type:
      create  -> ROLL BACK: delete any junction row for (workspace, session_id);
                             restore prev_ordinals; clear the marker
      delete  -> COMPLETE:  delete the junction row (idempotent); clear the marker
      reorder -> COMPLETE:  apply to_ordinal ordering from the payload; clear the marker
    clear the marker in the SAME transaction as the resolution write
if no marker exists but the junction violates its invariants
   (duplicate ordinals within a workspace, NULL ordinal, FK orphan)
      -> RegistryError{Corrupt}                     # fail loud, never patch
```

- **`create` rolls back, `delete` completes** exactly as §9.10 pins. A `create`
  is provisional until its marker clears, so a crash before `T2` leaves no
  junction row; a `delete` is a tombstone that must finish, so a crash before
  `T2` is completed on next open.
- **Unmarked mismatch fails loud as corruption** (R7): if the tables disagree
  with no marker to explain it, the registry refuses to proceed rather than
  guessing.
- Resolution is idempotent: re-running it after a crash produces the same state.

### 8.4 Interaction with the per-workspace reconciliation

The daemon uses the same protocol when reconciling its `workspace_sessions` rows
against its `sessions.db` at startup (§3.5, R-F12): imports and dangling-row
removals each write a marker before the divergent writes, so a crash mid-
reconciliation is recoverable exactly as above.

---

## 9. Concurrency and threading

- **Cross-process writes** are serialized by `flock(LOCK_EX)` on `registry.lock`
  (D22, §5.1). At most one process holds it at any instant; every mutation is
  bracketed by acquire/release and runs in `BEGIN IMMEDIATE`.
- **Cross-process reads** take no lock and run as WAL read transactions; they
  never block the writer and are never blocked by it (R2, R15).
- **In-process writes** are additionally serialized by a mutex, so a single
  process never issues two overlapping mutations; `open()`'s maintenance and any
  later mutation share that mutex.
- **SQLite backstop.** `busy_timeout=5000` bounds `SQLITE_BUSY`; on exhaustion
  the operation throws `RegistryError{LockUnavailable}` and performs no write.
- **No cross-session ordering guarantee is implied by the registry.** The
  junction orders sessions within a workspace; it says nothing about execution
  ordering, which is the daemon's concern (§9.9).
- **Read-your-writes.** A writer's own connection sees its committed writes;
  other readers see them at their next snapshot. A reader may therefore observe a
  workspace whose host claim is momentarily stale; that is expected and is
  resolved by the lazy reaping of §6.4, not by locking reads.

---

## 10. Crash recovery

- **SQLite WAL atomicity.** The registry inherits SQLite's transactional
  guarantees: a crash leaves every transaction all-or-nothing, and the WAL is
  replayed automatically on open (02 §7.2). The registry never reads the raw DB
  file bypassing SQLite.
- **Marker resolution.** A crash between `T1` and `T2` (§8.1) is recovered by
  `resolvePendingMutations()` on the next writer `open()`; a crash after `T2`
  needs no recovery (R7, R-F8/R-F9).
- **Open-time checks.** `open()` runs `PRAGMA foreign_key_check`; a non-empty
  result is `RegistryError{Corrupt}`. It also detects the unmarked junction
  mismatch described in §8.3.
- **Claims after a crash.** A crashed daemon's claim is **not** cleared eagerly
  at open. It is cleared lazily on next access under the liveness rule of §6.3
  (lock absent + nonce mismatch) and the D22 write lock. Eager clearing would
  race a daemon that is mid-start (it acquires its sidecar flock before claiming,
  R6).
- **No dirty flag.** Like 02, the registry adds no clean-shutdown marker; WAL
  atomicity plus the explicit `pending_mutation` marker are the recovery
  contract.
- **Corrupt registry.** v1 fails loud (`RegistryError{Corrupt}`), refuses
  writes, and preserves the file for inspection; there is no auto-repair and no
  `ymh doctor` in v1 (decision (k), §15.1). The process-scan fallback may still
  enumerate live hosts read-only (§6.5).

---

## 11. Invariants

Numbered `R1`–`R16` to avoid clashing with 01's `I#` and 02's `P#`. Any code that
can violate one is a defect.

**R1 — Registry is the open-set authority only.** `registry.db` holds workspace
identity, the ordered session junction, host registration, and the crash marker.
It holds **no** session event, lease, snapshot, permission, or focused-session
state. (§9.10, D21, §20.22)

**R2 — Single writer via `flock`, lock-free readers.** Every mutation holds
`flock(LOCK_EX)` on `registry.lock` for its duration; readers use WAL read
transactions and take no flock; at most one process holds the lock at any
instant. (§9.10, D22)

**R3 — NULL host sentinel, never 0.** A workspace with no host stores
`host_pid = NULL`; `0` is never stored; `host_boot_id`/`host_socket`/
`host_heartbeat` are NULL exactly when `host_pid` is NULL. (§9.10)

**R4 — Canonical-path uniqueness.** `canonical_path` is `realpath()` and UNIQUE;
`WorkspaceId` is a UUID never derived from the path; a symlink collision resolves
to one workspace. (§9.7, DIV-8)

**R5 — Liveness is lock-primary.** A claim is cleared only when the sidecar
`sessions.lock` is absent **and** the recorded `host_boot_id` cannot be matched to
a live holder's nonce. `kill(pid, 0)` and heartbeat staleness are hints and are
never sufficient on their own. (§9.7, §9.10)

**R6 — A daemon claim follows its lock.** A daemon acquires its sidecar flock
before writing its own claim, so a daemon-written claim always names the current
lock holder. A supervisor-written claim (spawn recording) is provisional and
never establishes liveness on its own; every liveness decision still requires the
sidecar flock (R5). (§9.7, §9.10)

**R7 — Marker precedes divergent writes.** A junction mutation writes its
`pending_mutation` marker before the record+order writes; startup resolves exactly
the marked mutation (`create` rolls back, `delete` completes); an unmarked
order/table mismatch fails loud as corruption. (§9.10, DIV-3)

**R8 — Ordinals are a strict, gap-tolerant order.** Within a workspace `ordinal`
is strictly increasing under the assigned order and is **not** dense; gaps are
expected and allowed; the ordered open set is `ORDER BY ordinal ASC`. (§9.10,
01 §16.1(b))

**R9 — No cross-database foreign key.** `workspace_sessions.session_id` is a
logical reference; the registry never requires the per-workspace DB to be
present, and the daemon reconciles membership against existence. (§3.5, §9.10)

**R10 — Discovery is durable-state-driven.** The open set is seeded from the
durable index (bootstrap + manual add); a process scan is never a discovery
source. (§9.10)

**R11 — Process scan never kills.** Reaping an orphan requires lock-absence and a
`host_boot_id` mismatch, and must take the D22 write lock first; `ps` evidence
alone never authorizes a kill. (§9.10)

**R12 — Focused session is supervisor-local.** The focused/active session is held
in `WorkspaceModel`/`UiModel` (§20.22) and is never written to `registry.db`.

**R13 — Explicit schema gate.** `application_id = 0x594D4802` and `user_version`
gate open; a foreign or newer file is refused; migrations are transactional,
additive-first, and never rewrite the open-set tables. (§3.4)

**R14 — Bootstrap is idempotent and `initialized` is last.** An interrupted
bootstrap leaves no `initialized` marker and resumes safely on the next writer
open. (§9.10)

**R15 — Committed-state visibility.** A reader observes only committed
transactions; a mutation is atomic and its marker clear is atomic with it. (§8.1,
§10)

**R16 — Row-domain write ownership.** The supervisor's write set is confined to
host-registration and workspace-identity columns; each daemon is the canonical
writer for its own workspace's `workspace_sessions` rows. (§9.10)

---

## 12. Failure modes

### 12.1 Shared findings (F1–F12, §54)

The registry's responsibilities for the existing findings:

| F# | Finding | Registry-layer handling |
|---|---|---|
| **F1** | path/process isolation | `canonical_path` is `realpath`; the registry never `chdir()`s and never resolves a path from `getcwd()`; uniqueness is canonical-path equality (R4, §9.7) |
| **F2** | background permission | out of scope; `PermissionDecision` is durable in the session log (01/02), never in the registry (§9.9) |
| **F3** | late event after close | junction removal is the **last** step, after the daemon commits `SessionEnded` and erases the session; the registry holds no events (§9.8, §3.5) |
| **F4** | edge-triggered attention | out of scope; the registry holds no `AgentState`, counters, or flash state (§9.9, §20.23) |
| **F5** | output ring buffers | out of scope; the registry holds no tool output or buffers (§9.11) |
| **F6** | input/keybinding focus | out of scope; the focused session is supervisor-local and never registry state (R12, §20.22) |
| **F7** | per-session dirty flags | out of scope; no per-session UI state in the registry (§20.23) |
| **F8** | resource caps | the registry supports §9.11's "cap the number of live daemons" by counting live host claims; it enforces no LLM/subprocess/PTY cap itself (§9.11) |
| **F9** | cancellation scoping | out of scope; the registry holds no cancellation state (§34) |
| **F10** | resume-suspended | the junction supplies the ordered open set that resume reopens `Idle`; the registry stores no runtime state and never auto-resumes (§9.9, §9.10) |
| **F11** | subagent ID duality | junction rows are per `SessionId`; a subagent keeps its own row/log, and the parent id is display routing only, never registry state (§20.25) |
| **F12** | flash clock in model | out of scope; no timers or wall-clock UI state in the registry (§9.9) |

**Explicitly out of scope for this component:** F2, F4, F5, F6, F7, F9, F12
(and the policy halves of F3/F8/F10/F11). The registry supplies the durable
open-set those components project; it owns none of their logic.

### 12.2 Component-local failure modes (`R-F#`)

These are component-local to the registry and are not part of the top-level
F1–F12 set. They must be covered by tests (§14.6).

| R-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **R-F1** | Registry missing | `openReadOnly` finds no file; `open` creates it | Writer creates + bootstraps; reader throws `OpenFailed`; fallback allowed only if uninitialized (§6.5, §7) |
| **R-F2** | Registry corrupt | `foreign_key_check`/`integrity_check` fails, or unmarked junction mismatch | `RegistryError{Corrupt}`; fail loud; preserve file; read-only fallback may enumerate hosts but never writes (§10) |
| **R-F3** | `flock` contention | `LOCK_EX` unavailable after the retry budget | Bounded retry then `LockUnavailable`; never force-write (§5.1) |
| **R-F4** | Stale host claim (dead daemon) | sidecar lock absent; `kill` may be `ESRCH` or a reused PID | Lazy clear under the write lock; re-claim only after lock-absence or an authenticated handshake, never on heartbeat or diagnostic alone (R5, §6.4) |
| **R-F5** | PID reuse | recorded `host_boot_id` ≠ the live holder's nonce | A held lock means the workspace is live; the live holder's own claim supersedes the stale row (R6); never match or re-claim by PID/diagnostic alone (R5, §9.7) |
| **R-F6** | `SIGSTOP`ped holder / split-brain | sidecar flock still held; heartbeat may look stale; `kill` returns `0` | Never clear or steal; the frozen holder is alive (§6.3, 02 §5.4) |
| **R-F7** | Duplicate canonical path (symlink) | `canonical_path` UNIQUE conflict after `canonical()` | Attach to the existing workspace; never create a second row (R4) |
| **R-F8** | Pending marker present at open | row in `pending_mutation` | Resolve by type (`create` rollback, `delete` complete, `reorder` complete); unknown type/bad payload ⇒ `Corrupt` (R7, §8.3) |
| **R-F9** | Unmarked order/table mismatch | duplicate ordinals in a workspace, NULL ordinal, or FK orphan with no marker | `RegistryError{Corrupt}`; fail loud; never patch silently (R7) |
| **R-F10** | Bootstrap interrupted | `initialized` marker absent after a partial walk | Resume idempotently on the next writer open (R14, §7.1) |
| **R-F11** | `workspace_roots` unset or nonexistent | resolved list empty or paths absent | No auto-discovery; user runs `ymh workspace add <path>`; not an error (§7.1) |
| **R-F12** | Dangling junction session | junction row names a session absent from the workspace `sessions.db` | Daemon reconciliation removes it under the marker protocol; never reported as open (R9, §3.5) |
| **R-F13** | Spoofed/racy `ps` output | candidate `argv` mismatches the independently derived socket/flock | Seed PIDs only; require handshake/flock confirmation; never kill (§6.5, R11) |
| **R-F14** | Lock/state path unwritable | `open`/`flock` on `registry.lock` fails with `EACCES`/`ENOENT` | `RegistryError{OpenFailed}`; no partial registry state (§4.3) |
| **R-F15** | Newer schema | `user_version > SCHEMA_VERSION` | `RegistryError{SchemaVersion}`; refuse to open; never downgrade (R13) |
| **R-F16** | Concurrent `ymh workspace add` | two processes canonicalize the same path | Single-writer serializes; the second sees the existing row and attaches idempotently (R2, R4) |
| **R-F17** | Heartbeat clock skew | `host_heartbeat` far in the past while the sidecar lock is held | Stale is a hint only; never clears a claim (R5, §6.2) |
| **R-F18** | Interrupted migration | `user_version` still the old value | Old version remains; open fails loud or resumes migration (R13, §3.4) |

---

## 13. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (§55). The registry maps onto it as
follows.

| dsh concept | ymh workspace registry | Reference |
|---|---|---|
| Workspace/session registry | `WorkspaceRegistry` over `registry.db` | §9.10, §55 |
| Durable open-set index | `workspaces` + `workspace_sessions` | §9.10, D21 |
| Host/daemon registration | `workspaces.host_pid/host_boot_id/host_socket/host_heartbeat` | §9.6, §9.10 |
| Cross-process service discovery | registry rows + authenticated JSON-RPC handshake | §9.6, §43 |
| Process/service liveness | sidecar `flock` + boot-nonce cross-check | §9.7, §9.10 |
| Crash-recovery marker | `pending_mutation` (dsh-style) | §9.10, DIV-3 |
| Cordis service seam | `WorkspaceRegistry` as a capability interface | §4.3, §55 |
| Ordered session list | `workspace_sessions.ordinal` (gap-tolerant) | §9.10, 01 §16.1(b) |
| Plugin lifecycle / hot reload | not modeled (accepted for v1) | §55 |

**Deliberate omissions** (accepted for v1, §55): no Cordis-compatible
configuration, no plugin dependency graph, no hot module replacement, no
distributed registry. The registry is a small, headless, provider-agnostic
membership store (G1, G3; §2.1). It deliberately does **not** hold the event log
(D2), the lease (02), or the focused session (§20.22).

---

## 14. Test plan

Strategy is §44: unit, integration (fake LLM / fake FS / fake shell), golden,
replay, and a separate live PTY/real-LLM layer (§44, §45). The deterministic
layers run offline against the Fake LLM (§45); the live layer is opt-in and
API-key gated. Registry tests use a temporary real SQLite file where the subject
is SQLite/flock semantics, a **fake FS** for the discovery walker, a **fake
process table** for the scan fallback, and an in-memory fake `WorkspaceRegistry`
where the subject is a consumer (spec 04/10).

### 14.1 Unit tests

Against a temporary real DB and a fake FS/process table:

- **Open / pragmas / version**
  - Every connection reports `foreign_keys=1`, `journal_mode=wal`,
    `busy_timeout=5000` (§3.2).
  - Fresh DB gets `user_version=1` and `application_id=0x594D4802`; a foreign
    `application_id` is rejected (R13, R-F15).
  - `openReadOnly` on a missing DB throws `OpenFailed`; on a pre-`initialized`
    DB throws `Uninitialized`; neither creates a file (§4.3).
- **Schema / sentinel**
  - `host_pid = 0` is rejected by the `CHECK`; a claim is all-NULL or all-present
    (R3).
  - `canonical_path` UNIQUE rejects a second row for the same realpath; a symlink
    to an owned dir collides (R4, R-F7).
  - `pending_mutation` accepts only `create|delete|reorder` (R-F8).
  - `registry_meta` carries only bookkeeping keys; the DDL has no event/lease
    columns (R1).
- **Mutations**
  - `addSession` assigns `max+1`; ordinals are strictly increasing and **not**
    asserted dense (R8).
  - `reorderSession` writes a marker first, then the order, then clears it;
    `MutationInProgress` when a marker exists (R7).
  - `heartbeat` with a mismatched `(pid, boot_id)` is a no-op; `releaseHost`
    only clears a matching claim (R5, R6).
  - `removeWorkspace` throws `WorkspaceNotEmpty`/`HostClaimed` (§4.4).
- **Bootstrap (fake FS)**
  - Walker honours the denylist (`.git`, `node_modules`, `.cache`, `Library`,
    `target`, `build`, `.venv`) and the depth bound (§7.1, R-F11).
  - Discovered `sessions.db` rows import newest-first with contiguous ordinals
    from 0; `archived` defaults to 0 (§7.1).
  - `initialized` is written last; an injected failure before it leaves the
    marker absent and the walk resumable (R14, R-F10).
  - Unset/nonexistent `workspace_roots` ⇒ no discovery, not an error (R-F11).
- **Pending mutation resolution**
  - `create` marker ⇒ junction row rolled back; `delete` marker ⇒ row completed;
    `reorder` marker ⇒ order applied; each clears the marker (R7, §8.3).
  - Unknown type / malformed payload ⇒ `Corrupt` (R-F8).
  - Unmarked duplicate-ordinal mismatch ⇒ `Corrupt` (R-F9).

### 14.2 Integration tests (Fake LLM, §45)

- **Daemon ↔ registry.** A fake daemon acquires the sidecar flock, calls
  `claimHost`, heartbeats, and `releaseHost` on shutdown; the row transitions
  all-NULL → claimed → all-NULL (R3, R6).
- **Supervisor read-only coexistence.** A writer holds the D22 lock for a
  mutation while an `openReadOnly` reader lists workspaces without blocking and
  sees only committed state (R2, R15).
- **Reconciliation.** With a real per-workspace `sessions.db` (02) and a junction
  missing one session plus one dangling row, the daemon reconciles both under the
  marker protocol (R9, R-F12).
- **Manual add.** `ymh workspace add <path>` on a workspace with an existing
  `sessions.db` imports newest-first and is idempotent (R-F16, §7.2).
- **Process-scan fallback.** Against a fake process table and a fake
  `sessions.lock`, a matching candidate is confirmed by handshake/flock; a
  spoofed `--socket` is ignored because the path is derived independently; no
  kill occurs on `ps` evidence alone (R10, R11, R-F13).

### 14.3 Concurrency / crash tests

- **Writer contention.** Two processes race to mutate; the flock serializes them;
  the loser retries within budget and then fails with `LockUnavailable` without
  writing (R2, R-F3).
- **Concurrent manual add.** Two processes run `ymh workspace add` on the same
  canonical path; the flock serializes them and exactly one `workspaces` row
  exists afterward, with the second attaching idempotently (R4, R-F16).
- **Kill mid-mutation.** `SIGKILL` between `T1` and `T2` leaves the marker; the
  next writer `open()` resolves it by type (R7, R-F8).
- **Kill after `T2`.** No marker remains; the mutation is fully committed (R15).
- **Daemon crash and reap.** `SIGKILL` the daemon, then the supervisor's read
  path observes a stale claim and reaps it under the write lock after
  lock-absence (R5, R-F4).
- **`SIGSTOP` guard.** A `SIGSTOP`ped daemon still holds the sidecar flock; a
  concurrent would-be reaper does **not** clear the claim even with a stale
  heartbeat (R5, R-F6).
- **PID reuse.** A recycled PID with a different boot nonce is never matched to
  the claim (R5, R-F5).
- **Interrupted migration.** A killed migration leaves the old `user_version`;
  open fails loud or resumes (R13, R-F18).
- **Power-loss durability** is out of scope for the default suite; the registry
  uses `synchronous=NORMAL` (02 §3.2) and any stronger assertion is a gated test.

### 14.4 Golden / replay tests

- **Bootstrap golden.** A fake FS tree → the exact expected `workspaces` and
  `workspace_sessions` rows (order, ordinals, titles), rendered as a stable
  fixture (§7.1).
- **Resolution replay.** A recorded sequence of marker states → the same resolved
  registry state on every replay (R7).
- **Read-model replay.** The supervisor's open-set projection from a fixed
  registry snapshot is byte-identical across runs (R1, R15).

### 14.5 Live end-to-end tests (real LLM, PTY-driven, §44)

Milestone-2 gated (§57 Step 13, §58):

- Spawn the real `ymh` binary under a PTY (or `tmux send-keys` + `capture-pane`),
  create two workspaces, and assert the registry contains both `workspaces` rows
  and their junctions with the expected ordering.
- Kill a daemon and assert the supervisor reaps its claim on next access without
  killing a live one.
- Drive the real LLM through the supervisor to create a session and assert the
  junction gains a row in the correct order.
- Skipped (not failed) without an API key plus an explicit opt-in flag; tolerant
  of model nondeterminism — assert structure and invariants, never prose.

### 14.6 Failure-mode coverage matrix

| Failure | Test layer | Asserts |
|---|---|---|
| F1 | unit + integration | canonical `canonical_path`, no `getcwd` (R4) |
| F3 | integration | junction removal is the last step after `SessionEnded` (R1) |
| F8 | unit | live-claim count supports the daemon cap, no cap logic in the registry (R1) |
| F10 | integration | junction supplies the ordered open set; nothing auto-resumes (R12) |
| F11 | integration | subagent junction rows keep their own `SessionId` (R1) |
| R-F1 | unit | missing registry ⇒ create+bootstrap vs. reader `OpenFailed` |
| R-F2 | unit | integrity/foreign-key failure ⇒ `Corrupt`, file preserved |
| R-F3 | unit + integration | bounded retry then `LockUnavailable`, no force-write (R2) |
| R-F4 | integration | dead daemon's claim cleared lazily under the write lock (R5) |
| R-F5 | unit | boot-nonce mismatch ⇒ different instance, PID alone never matches (R5) |
| R-F6 | integration | `SIGSTOP`ped holder is not reaped (R5) |
| R-F7 | unit | symlink collision ⇒ one workspace (R4) |
| R-F8 | unit | `create` rollback / `delete` completion / bad payload ⇒ `Corrupt` (R7) |
| R-F9 | unit | unmarked mismatch ⇒ `Corrupt` (R7) |
| R-F10 | unit | interrupted bootstrap resumes; `initialized` last (R14) |
| R-F11 | unit | unset/nonexistent roots ⇒ no discovery, no error |
| R-F12 | integration | dangling junction row reconciled, never reported open (R9) |
| R-F13 | unit + integration | `ps` seeds PIDs only; confirmation required; no kill (R10/R11) |
| R-F14 | unit | unwritable lock/state path ⇒ `OpenFailed` |
| R-F15 | unit | newer `user_version` refuses to open (R13) |
| R-F16 | integration | concurrent add ⇒ one workspace, idempotent attach (R2) |
| R-F17 | unit | clock skew is a hint, never a clear (R5) |
| R-F18 | unit | interrupted migration leaves the old version, open fails loud (R13) |

### 14.7 Invariant coverage

Every invariant R1–R16 maps to at least one test, or is marked
covered-by-construction (CBC) with the reason it cannot be violated.

| Invariant | Test layer | Coverage |
|---|---|---|
| R1 registry is open-set only | unit | DDL has no event/lease columns; no such API exists |
| R2 single writer / lock-free readers | unit + integration | flock contention + concurrent reader (R-F3) |
| R3 NULL host sentinel | unit | `CHECK` rejects 0; all-NULL/all-present (R-F4) |
| R4 canonical-path uniqueness | unit | symlink collision merges (R-F7) |
| R5 lock-primary liveness | integration | `SIGSTOP` + PID-reuse + heartbeat-skew tests (R-F4/R-F5/R-F6/R-F17) |
| R6 claim names a lock holder | integration | daemon acquires flock before claim; claim write order |
| R7 marker precedes writes | unit + crash | `create` rollback / `delete` completion / unmarked mismatch (R-F8/R-F9) |
| R8 gap-tolerant ordinals | unit | strictly increasing; no density assumed; gaps explicitly asserted |
| R9 no cross-DB FK | integration | reconciliation with a per-workspace DB (R-F12) |
| R10 discovery is durable-state-driven | unit | process scan cannot seed the open set (R-F13) |
| R11 process scan never kills | unit | spoofed `ps` cannot trigger a kill (R-F13) |
| R12 focused session is supervisor-local | CBC | no registry API accepts a focused session |
| R13 explicit schema gate | unit | foreign/newer `user_version` refused (R-F15/R-F18) |
| R14 idempotent bootstrap, `initialized` last | unit | interrupted walk resumes (R-F10) |
| R15 committed-state visibility | unit + integration | reader during writer sees only committed rows |
| R16 row-domain write ownership | integration | supervisor cannot edit a daemon's junction rows |

---

## 15. Decisions and open questions

### 15.1 Decisions (pinned by this spec)

- **(a) The D22 flock is a per-write critical section, not process-lifetime
  ownership (resolved).** §9.10 says "exactly one process holds
  `flock(LOCK_EX)`" *and* makes each daemon the canonical writer for its own
  session list *and* lets the supervisor write host claims. The only consistent
  reading — now recorded in §9.10 — is that the lock is held for the duration of
  each mutation, so exactly one process holds it at any instant. The alternative
  (a single lifetime writer process) is rejected because §9.10 explicitly names
  the daemon as the canonical writer (§5.1, R2, R16).
- **(b) `registry_meta` is added for schema/bootstrap bookkeeping.** It holds
  `initialized`, `legacy_migrated`, and nothing else; it carries no open-set
  state. §9.10's "small — workspace records plus the session junction only"
  constrains the open-set payload, not a one-row bookkeeping table; the marker
  §9.10 itself mandates ("`initialized` marker is written last") needs a home.
  The §9.10 errata now reproduces this table (§3.3, §7.1, R1).
- **(c) `host_pid` uses the NULL sentinel, never `0`.** The four host columns
  (pid, boot id, socket, heartbeat) move together and are `CHECK`-constrained;
  `host_heartbeat` is set at claim time (§3.3, R3).
- **(d) Liveness is sidecar-flock-primary with a boot-nonce cross-check.**
  `kill(pid, 0)` and heartbeat staleness are hints; a claim is cleared only when
  the lock is absent and the recorded `host_boot_id` cannot be matched to a live
  holder (§6.3, R5). This is exactly §9.7/§9.10.
- **(e) Ordinals are gap-tolerant.** `ordinal` is an ordering key, not a
  position; the registry never claims gapless ordinals and never assumes density
  (§3.3, R8), mirroring 01 §16.1(b) and 02 P6 for `Sequence`.
- **(f) The process scan is best-effort, confirmation-gated, and never kills.**
  `argv` seeds candidate PIDs only; paths are derived independently; reaping
  requires lock-absence + boot-nonce mismatch under the D22 write lock (§6.5,
  R10, R11).
- **(g) The `pending_mutation` protocol commits the marker first and clears it
  atomically with the mutation.** A marker present after a crash means the data
  writes did not commit; resolution is by type (`create` rollback, `delete`
  complete, `reorder` complete) and is idempotent (§8, R7).
- **(h) Bootstrap is idempotent, `initialized` last, and the process scan is not
  a discovery source.** `workspace_roots` defaults to `["$HOME/prjs"]`; manual
  `ymh workspace add <path>` covers the no-root case (§7, R10, R14).
- **(i) The daemon reconciles membership against existence.** The per-workspace
  DB is authoritative for session existence; the registry is authoritative for
  membership; the daemon performs the reconciliation under the marker protocol
  (§3.5, R9, R-F12).
- **(j) `application_id = 0x594D4802`** distinguishes the registry from the
  per-workspace `sessions.db` (`0x594D4801`, 02 §3.2) for discovery and
  diagnostics (§3.2, R13).
- **(k) Corrupt-registry recovery tooling is deferred to v2.** v1 fails loud
  (`RegistryError{Corrupt}`), refuses writes, and preserves the file; the
  read-only process-scan fallback remains available (§10, R-F2).
- **(l) `<workspace>/.ymh/host.sock` is a v1 default, not a protocol
  commitment.** The transport spec (05) owns the final socket convention; this
  spec uses the path only so the process-scan fallback can derive a socket
  independently of `argv` (§6.5). If spec 05 changes it, this derivation changes
  with it.
- **(m) Legacy central-store import is deferred.** An optional one-time import
  may be attempted, gated behind `registry_meta['legacy_migrated']`; the legacy
  format is either defined later or dropped for v1. It is best-effort and never
  blocks the open-set bootstrap (§7.1, §7.3).
- **(n) `display_title` defaults to the directory basename with a CLI
  override.** `registerWorkspace` fills an empty `display_title` with the
  basename of the canonical path; the exact CLI surface belongs to the CLI/UI
  spec (10) (§4.4).

### 15.2 Open questions

None. The prior D22-lock-lifetime, legacy-format, and `display_title` questions
are resolved above as decisions (a), (m), and (n).

---

## 16. References

- `00-architecture.md` §4.2 (event stream is the runtime spine), §8.1–§8.3
  (event system), §9.2 (`SessionHeader`, per-workspace schema), §9.6
  (supervisor/daemon topology), §9.7 (isolation, path safety, write lease),
  §9.8 (detach/delete/archive), §9.10 (shared workspace registry), §9.11
  (resource caps), §18 (execution environment), §20.22 (`WorkspaceModel`,
  focused session), §20.25 (subagent ID duality), §37 (`workspace_roots`), §43
  (RPC mode), §44 (testing strategy), §45 (Fake LLM), §48–§49 (deps/source
  tree), §54 (D1–D23, F1–F12), §55 (dsh comparison), §57 Step 11/13, §58
  (milestones).
- `01-session.md` §2.1 (`SessionId`, `WorkspaceId`), §3 (`SessionHeader`),
  §8 (`SessionManager`), §16.1(b) (gap-tolerant sequences).
- `02-persistence.md` §3.1–§3.2 (per-workspace DB, `application_id`), §5.1
  (sidecar `sessions.lock`), §5.2 (boot nonce), §5.4 (flock-primary liveness),
  §7 (crash recovery), P6/P9 (gap tolerance, WAL readers).
- `HANDOFF.md` §2 (the rule), §5 items 2–3 (bootstrap discovery; `SessionHeader`
  finalization), §6 row 03 (component plan), §7 (definition of verified).
- `DESIGN_STATUS.md` (written/verified tracker).
