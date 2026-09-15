# 02 — Persistence & Write Lease

**Component 02 of 10.** Persistence is the durable store seam beneath `Session`
(component 01). It owns the SQLite database that holds the append-only event log,
the cross-process write lease that serializes writers to that log, and the
flush/checkpoint/crash-recovery machinery that makes the log the source of truth
(D2, §54). This document pins the schema DDL, the required connection pragmas,
the `SessionPersistence` / `SessionHandle` interfaces, lease semantics, snapshot
handling, schema migration, and the crash-recovery protocol. It follows
`00-architecture.md` (cited inline as `§n`) and `01-session.md` (cited as
`01 §n`); where it cannot follow them it records the conflict under §13 rather
than choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
                     Session  (spec 01)
                        │  append / read / create / erase
                        ▼
   ┌───────────────────────────────────────────────────────────┐
   │  SessionPersistence  (this spec)                            │
   │    SessionStore impl over SQLite                            │
   │    lease acquire/renew/steal/release                        │
   │    flush · checkpoint · snapshot · crash recovery           │
   └───────────┬─────────────────────────────┬─────────────────┘
               │                             │
               ▼                             ▼
   <workspace>/.ymh/sessions.db     <workspace>/.ymh/sessions.lock
   sessions · events ·               sidecar flock (kernel-released
   session_leases ·                  on process death) — the liveness
   session_snapshots                 anchor for every lease (§9.7, §9.10)
```

The store is the only component that touches the session database. `Session`
(spec 01) depends on the abstract `SessionStore` seam and never opens SQLite
itself (01 §7). `SessionHandle` is the IPC-addressable teardown capability that
owns the lease (§9.7, §9.8); this spec pins its persistence-side behavior.

The store does **not** own the open-set, the ordered session list, the
`ordinal`/`archived` junction, host registration, or the focused session. Those
live in the shared `registry.db` (§9.10, D21, spec 03) and in supervisor-local
state (§20.22) respectively. §3.5 records the boundary explicitly.

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 02)

This spec pins:

- The `SessionPersistence` seam and its `SessionStore` overrides (01 §7).
- `SessionHandle` as the lease-owning teardown capability (01 §7, §9.8).
- The `sessions` / `events` / `session_leases` schema for
  `<workspace>/.ymh/sessions.db`, exactly as fixed by §9.2, plus the derived
  `session_snapshots` cache (§3.3, §6).
- Required connection pragmas (`foreign_keys`, `journal_mode=WAL`,
  `busy_timeout`) and the open/close policy (§9.2, §3.2, §4.3).
- Lease semantics: acquire / renew / steal / release, `holder_pid`,
  `holder_boot_id`, `expires_at` (TTL), the `flock` sidecar, the `kill(pid,0)`
  hint, and the SIGSTOP/split-brain guard (§9.7, §5).
- Flush / commit policy and `AssistantChunk` producer coalescing (01 §16.1(c),
  §6.1).
- Snapshot/checkpoint creation, staleness detection, and pruning (01 §6.4, I21,
  S12; §6).
- Crash recovery: WAL replay, torn-append handling, half-written rows, and the
  open-time consistency decision (§7).
- Migration / schema versioning (`user_version`) and the forward/backward policy
  (§3.4, §7.1).

### 1.3 Boundaries — deferred to other specs

| Concern | Owner | Why |
|---|---|---|
| Event taxonomy, payload shapes, `deriveMessages()`, turn/step rules | 01 | 01 §4, §6.3 |
| Session create/resume/fork/replay orchestration | 01 | 01 §9 |
| Which agent-loop step emits which event | 06 | §11 |
| Open-set, `ordinal`, `archived`, host liveness/registration, bootstrap | 03 | §9.10, D21 |
| Daemon spawn/setsid, socket, attach/detach, orphan reaping | 04 | §9.6, D23 |
| Wire framing, JSON-RPC methods, `SessionEnvelope` | 05 | §9.6, §20.22 |
| Payload JSON shapes inside `events.payload` | 01/06 | 01 §4.5 |
| `ExecutionEnvironment::resolve()` implementation | 07 | §9.7, §18 |
| Logging sinks/format | 10 (spdlog config) | §40 |

The store never `chdir()`s and never resolves a path from `getcwd()`: `db_path`
and `lock_path` are derived from the workspace root (already canonicalized by
spec 01 at create, 01 §10.2, I8) and passed in by the daemon (04). This is the
persistence half of I15 / F1 (§9.7).

### 1.4 Seam ownership relative to `01-session.md`

01 §7 declares `SessionStore` abstract and `SessionPersistence` its SQLite
implementation, and states "02 pins: DB path, pragmas, open/close policy,
flush/checkpoint, lease TTL/boot-nonce/steal, crash recovery." This spec is the
implementation of that delegation. The abstract `SessionStore` signature set is
**frozen by 01** and is reproduced here unchanged (§4.2); this spec adds only
concrete, non-virtual members to `SessionPersistence` and a `LeaseManager` seam
that 01 does not see.

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

These build on the strong types pinned in 01 §2.1 and §4.6 and add the
persistence-local types. `Sequence`, `SessionId`, `EventId`, `Event`, and
`EventRange` are defined by 01 and are **not** redefined here.

```cpp
namespace ymh {

// A daemon's boot nonce: UUIDv4 minted once by the daemon at startup and
// supplied to the store via PersistenceConfig::boot_id (§4.1); the store never
// mints it. Mirrors the registry's host_boot_id (§9.10) and every lease row's
// holder_boot_id (§9.2). It is NOT part of SessionHeader (HANDOFF §5 item 3).
struct BootId {
    std::string value;
    auto operator<=>(const BootId&) const = default;
};

// The identity that a lease row grants write access to (§9.7).
struct HolderIdentity {
    std::int32_t pid;          // holder_pid
    BootId       boot_id;      // holder_boot_id (PID-reuse guard)
    auto operator<=>(const HolderIdentity&) const = default;
};

// The observable state of a session's lease (see §5.3).
enum class LeaseState : std::uint8_t {
    HeldByMe,      // this process holds the lease and may append
    HeldByOther,   // a different live holder owns it; do not write
    Expired,       // row exists but expires_at < now (a hint, not a steal right)
    Absent,        // no row
};

} // namespace ymh
```

`HolderIdentity` is the only key under which a lease may be renewed or released
(§5.3, P4). Timestamps stored in SQLite are epoch milliseconds (`INTEGER`), per
§9.2; the C++ API uses `int64_t` for `created_at` / `updated_at` / `expires_at`
to match the column type and 01 §3's `SessionHeader` field types.

### 2.2 Store error taxonomy

01 §10.1 pins `LeaseLost` by name ("a non-holder gets `LeaseLost`") and 01 §13.2
S11 pins a store-open failure. This spec fixes the exception hierarchy so that
callers and tests share names:

```cpp
namespace ymh {

class StoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The database file is missing/locked/unopenable (01 S11), or the sidecar
// lock cannot be established (§5.1, P-F14).
class StoreOpenError : public StoreError { /* ... */ };

// This process is not the lease holder (01 I5, S4; §9.7).
class LeaseLost : public StoreError { /* ... */ };

// events.event_id UNIQUE violation (01 S1), a non-monotonic sequence (01 S2),
// or an undecodable stored row (01 S3).
class CorruptionError : public StoreError { /* ... */ };

// user_version is newer than this binary, or a migration failed (§3.4, P-F10/11).
class SchemaVersionError : public StoreError { /* ... */ };

// A payload/metadata blob exceeds the configured cap (01 S10; §9.11).
class PayloadTooLarge : public StoreError { /* ... */ };

// A whole-session erase would orphan fork/subagent children (§4.7, P-F13).
class DependentSessionError : public StoreError { /* ... */ };

} // namespace ymh
```

`LeaseLost`, `CorruptionError`, `SchemaVersionError`, and `DependentSessionError`
are **fail-loud**: they propagate and are never swallowed. Only the lease-loss
path degrades gracefully (read-only, §5.7).

---

## 3. Database layout and schema (pinned)

### 3.1 Location and files

```text
<workspace>/.ymh/sessions.db     SQLite database (the event log + leases)
<workspace>/.ymh/sessions.db-wal WAL file (SQLite-managed)
<workspace>/.ymh/sessions.db-shm shared-memory file (SQLite-managed)
<workspace>/.ymh/sessions.lock   sidecar flock + holder diagnostic (§5.1)
```

The database is **per workspace** and owned by that workspace's `WorkspaceHost`
daemon (§9.2, §9.6). One daemon owns one workspace and one cwd (D18, F1, §9.7),
so the daemon holds exactly one `sessions.db` open and one `sessions.lock` flock
for its lifetime. The `.ymh/` directory is created with mode `0700` if absent;
the DB and lock files are created with mode `0600`.

The store never creates `<workspace>/.ymh/sessions.db` outside a workspace root
that was canonicalized by spec 01 (01 I8, §10.2). It does not invent a path from
`getcwd()` (I15, §9.7).

### 3.2 Connection pragmas (required)

§9.2 mandates `foreign_keys`, `journal_mode=WAL`, and a busy timeout on **every
connection that opens this database**, and explicitly delegates the concrete
values to this spec. The pinned set, applied immediately after every
`sqlite3_open_v2` (write connection and read connections alike):

```sql
PRAGMA foreign_keys = ON;         -- required by §9.2; SQLite default is OFF
PRAGMA journal_mode = WAL;        -- required by §9.2; readers never block the writer
PRAGMA busy_timeout = 5000;       -- required by §9.2; 5 s, then SQLITE_BUSY
PRAGMA synchronous = NORMAL;      -- WAL-recommended; see the durability note below
PRAGMA wal_autocheckpoint = 1000; -- SQLite default; explicit so it is pinned
PRAGMA application_id = 0x594D4801; -- 'Y','M','H',schema-family 1; see §3.4
```

`foreign_keys=ON`, `journal_mode=WAL`, and `busy_timeout` are non-negotiable
(§9.2). `synchronous=NORMAL` is the pinned default: under WAL it guarantees that
a committed transaction survives an **application/process crash** (which is the
boundary I4 / durable-before-observable needs, 01 I4). An **OS crash or power
loss** may roll back the most recent commit(s) unless `synchronous=FULL` is set.
If a deployment needs that stronger guarantee, `synchronous=FULL` is the
documented override; it is not the default because it defeats the
streaming-append throughput the log needs.
`application_id` is a fixed magic that lets bootstrap discovery and diagnostics
recognize a `sessions.db` and reject a foreign SQLite file (§9.10).

Pragmas are re-applied per connection because `busy_timeout`, `foreign_keys`,
and `synchronous` are **per-connection**; `journal_mode` and `application_id`
are persistent in the DB header but re-setting them is harmless and keeps the
open path uniform. On a read-only connection (`openReadOnly`, §4.3) the same set
is applied except that `application_id` is only **read** for the foreign-file
check, never written, and `journal_mode=WAL` is a no-op on a DB already in WAL.

### 3.3 DDL (pinned)

The three source-of-truth tables are reproduced **verbatim** from §9.2. The
`session_snapshots` table is the derived cache owned by this spec (§6); §3.5
records the §9.2 errata that admits it.

```sql
-- ---- source of truth (§9.2) ------------------------------------------------

CREATE TABLE sessions (
    id              TEXT PRIMARY KEY,       -- SessionId (UUIDv4, stable, never a path)
    cwd             TEXT NOT NULL,          -- canonical workspace root, immutable
    created_at      INTEGER NOT NULL,       -- epoch ms
    updated_at      INTEGER NOT NULL,       -- epoch ms of last appended event
    title           TEXT NOT NULL DEFAULT '',   -- display title ('' => derive)
    model           TEXT NOT NULL DEFAULT '',   -- default model id
    server_profile  TEXT NOT NULL DEFAULT 'interactive',
    kind            TEXT NOT NULL DEFAULT 'root',   -- root | fork | subagent
    parent_session  TEXT,                   -- fork/subagent parent, NULL if root
    seed_length     INTEGER,                -- # parent events copied; NULL for root/subagent; required for fork
    metadata        JSON,
    FOREIGN KEY(parent_session) REFERENCES sessions(id),
    CHECK (kind IN ('root','fork','subagent')),
    CHECK ((kind='root' AND parent_session IS NULL AND seed_length IS NULL) OR (kind='fork' AND parent_session IS NOT NULL AND seed_length IS NOT NULL) OR (kind='subagent' AND parent_session IS NOT NULL AND (seed_length IS NULL OR seed_length = 0)))
);

CREATE TABLE events (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    event_id TEXT NOT NULL UNIQUE,
    timestamp INTEGER NOT NULL,
    type TEXT NOT NULL,
    payload JSON NOT NULL,

    FOREIGN KEY(session_id) REFERENCES sessions(id)
);

CREATE INDEX idx_events_session
ON events(session_id, sequence);

CREATE TABLE session_leases (
    session_id     TEXT PRIMARY KEY,
    holder_pid     INTEGER NOT NULL,  -- host daemon that owns writes
    holder_boot_id TEXT NOT NULL,     -- holder's per-boot UUID (PID-reuse guard)
    acquired_at    INTEGER NOT NULL,
    expires_at     INTEGER NOT NULL,  -- acquired_at + TTL
    FOREIGN KEY(session_id) REFERENCES sessions(id)
);

-- ---- derived cache (this spec; §6, §3.5) -----------------------------------

CREATE TABLE session_snapshots (
    session_id     TEXT PRIMARY KEY,  -- one snapshot per session
    at_sequence    INTEGER NOT NULL,  -- resolved-view Sequence head covered (I21)
    header_json    TEXT NOT NULL,     -- SessionHeader at `at_sequence`
    messages_json  TEXT NOT NULL,     -- deriveMessages() at `at_sequence` (I21)
    event_count    INTEGER NOT NULL,  -- resolved event count at `at_sequence`
    created_at     INTEGER NOT NULL,  -- epoch ms
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
```

Schema notes, each traceable to a pinned source:

- The `sessions` column list is the finalized field set of `HANDOFF.md` §5 item
  3 and 01 §3: `id`, `cwd`, `created_at`, `updated_at`, `title`, `model`,
  `server_profile`, `kind`, `parent_session`, `seed_length`, `metadata`. There is
  **no boot nonce** in this table (HANDOFF §5 item 3; 01 §3) and **no
  `ordinal`/`archived`** (they stay in the registry junction, §9.10).
- The three-way `kind` CHECK and the parent/seed CHECK are §9.2's; 01 I9's
  `validateHeader` mirrors them exactly. The store treats a CHECK violation as
  corruption (01 S9).
- `events.sequence` is `INTEGER PRIMARY KEY AUTOINCREMENT`, i.e. a single
  **database-global** counter. It is strictly increasing but **not gapless**;
  gaps are expected and allowed (01 §16.1(b), I2). This spec must never present
  the sequence as dense (P6).
- `events.event_id TEXT NOT NULL UNIQUE` is 01 I3; a duplicate is a
  `CorruptionError` (01 S1).
- `session_leases.session_id` is a PK, so a session has at most one lease row;
  acquire is an `INSERT`, steal is a `REPLACE` (§9.7, §5.3).
- `session_snapshots` has **no** authority over `events`: it is always
  discardable and rebuildable (01 I21, P11).

### 3.4 Schema versioning and migration

The store uses SQLite's built-in `PRAGMA user_version` as the schema version
integer; `PRAGMA application_id` (0x594D4801) identifies the file family.

```text
SCHEMA_VERSION = 1     (the DDL in §3.3)

open:
  app_id := PRAGMA application_id
  if app_id != 0 && app_id != 0x594D4801        -> SchemaVersionError (foreign DB)
  v := PRAGMA user_version
  if v == 0                                     -> fresh DB: apply §3.3, set v=1
  if v == SCHEMA_VERSION                        -> ready
  if v < SCHEMA_VERSION                         -> migrate forward in order, in ONE
                                                   transaction; on any failure ROLLBACK
                                                   and raise SchemaVersionError
  if v > SCHEMA_VERSION                         -> SchemaVersionError (newer binary
                                                   wrote this DB); refuse to open
```

Forward policy (normative):

- Migrations are **additive first**: new tables, new nullable columns, new
  indexes. A migration **must not** `UPDATE` or `DELETE` existing `events` rows
  and must not rewrite `events.sequence`; the log is append-only (D2, 01 I1, P7).
  If a future shape needs a payload change, it is handled by the JSON payload's
  own evolution, not by mutating rows (§9.2: "the event payload can evolve
  independently").
- Each migration runs in its own transaction and bumps `user_version` inside
  that transaction, so an interrupted migration leaves the DB at the old version
  (P-F11).
- A destructive migration (e.g. dropping a table) is allowed only for
  derived-cache tables (`session_snapshots`), never for `sessions`/`events`/
  `session_leases`.

Backward policy (normative): **no backward compatibility is promised.** An older
binary that sees `user_version > SCHEMA_VERSION` refuses to open with
`SchemaVersionError`; it never attempts a downgrade. This is safe because the
registry (`registry.db`, §9.10) records the daemon version family for a
workspace, and a mismatched binary is a deployment error, not a data-loss event.

### 3.5 Relationship to §9.2's three source-of-truth concerns

§9.2 states the per-workspace DB holds three **source-of-truth** concerns:
`sessions`, `events`, and `session_leases`. Spec 02 adds a fourth table,
`session_snapshots`, because 01 §6.4 explicitly delegates snapshot persistence to
spec 02 ("A snapshot is a derived cache persisted by spec 02") and 01 §7 lists
"flush/checkpoint" as owned here.

This is **not** a fourth durable concern. §9.2 was amended by errata to read
"three source-of-truth concerns (`sessions`, `events`, `session_leases`) plus a
derived, discardable snapshot cache" (00-architecture §9.2). The errata is the
authority for admitting `session_snapshots` into `sessions.db`; this spec does
not assert the reconciliation on its own authority. `session_snapshots` is a
pure, discardable materialization of `events` (I21), it can be deleted at any
time without changing any projection, and the store treats a missing or stale
snapshot as the normal case. A sidecar `<workspace>/.ymh/snapshots.db` or
per-session snapshot files are rejected: they would reintroduce a cross-file
consistency problem that same-DB storage avoids (§6.6). The resolved decision is
recorded in §13.1(d).

---

## 4. The `SessionPersistence` interface (pinned)

### 4.1 Configuration

```cpp
namespace ymh {

struct PersistenceConfig {
    std::filesystem::path db_path;          // <workspace>/.ymh/sessions.db
    std::filesystem::path lock_path;        // <workspace>/.ymh/sessions.lock

    // The daemon's boot nonce (§5.2), minted by the caller (spec 04) *before*
    // open(). The writer open() stamps it into the sidecar-lock diagnostic
    // (§4.3 step 7, §5.1) and into every lease row's holder_boot_id; the store
    // never mints it.
    BootId                     boot_id;

    // Lease timing (§5.5). TTL is a freshness bound, not a steal right.
    std::chrono::milliseconds lease_ttl{15'000};
    std::chrono::milliseconds renew_interval{5'000};   // ~TTL/3

    // SQLite tuning (§3.2).
    std::chrono::milliseconds busy_timeout{5'000};
    int                       wal_autocheckpoint_pages{1'000};

    // Derived-cache policy (§6.4–§6.5).
    std::size_t snapshot_threshold_events{256};

    // Payload cap enforced before COMMIT (01 S10, §9.11).
    std::size_t max_payload_bytes{4u * 1024u * 1024u};

    // Chunk coalescing bounds (01 §16.1(c); §6.2).
    std::size_t               max_chunk_batch{32};
    std::chrono::milliseconds chunk_flush_interval{100};
};

} // namespace ymh
```

`db_path` / `lock_path` are supplied by the daemon (spec 04) and are rooted at
the canonical workspace root; the store does not compute them (I15, P16).
`boot_id` is likewise minted by the daemon and supplied here before `open()`; the
store never mints it (§5.2).

### 4.2 Class shape and `SessionStore` overrides

The abstract `SessionStore` interface is **frozen by 01 §7** and reproduced here
only to show the concrete override set. The concrete class adds non-virtual
persistence operations that 01 does not see.

```cpp
namespace ymh {

// ---- frozen by 01 §7 (do not change) ---------------------------------------
class SessionStore {
public:
    virtual ~SessionStore() = default;

    virtual SessionHeader                create(SessionHeader) = 0;   // §9.2
    virtual std::optional<SessionHeader> load(SessionId) const = 0;   // §9.3
    virtual std::vector<SessionHeader>   list() const = 0;
    virtual void                         erase(SessionId) = 0;        // §9.8

    virtual EventRange read(SessionId, Sequence after = 0) const = 0;
    virtual EventRange readRange(SessionId, Sequence from, Sequence to) const = 0;

    // Persists and returns the store-assigned Sequence. Throws LeaseLost when
    // this process does not hold the session's lease (§9.7, 01 I5).
    virtual Sequence append(SessionId, Event) = 0;

    virtual bool isLeaseHolder(SessionId) const = 0;
};

// ---- the SQLite implementation (this spec) ---------------------------------
class SessionPersistence final : public SessionStore {
public:
    // Opens/creates the DB, applies §3.2 pragmas, acquires the sidecar flock
    // (§5.1), runs the open-time recovery protocol (§7.1). Throws
    // StoreOpenError (01 S11) or SchemaVersionError (§3.4).
    static std::unique_ptr<SessionPersistence> open(const PersistenceConfig&);

    // Read-only open: SQLITE_OPEN_READONLY, **no** sidecar flock, no migration,
    // no recovery write. Coexists with a live writer via WAL (§4.3, §8). Throws
    // StoreOpenError (missing/unopenable DB) or SchemaVersionError.
    static std::unique_ptr<SessionPersistence> openReadOnly(const PersistenceConfig&);
    ~SessionPersistence() override;

    // ---- SessionStore overrides --------------------------------------------
    SessionHeader                create(SessionHeader) override;
    std::optional<SessionHeader> load(SessionId) const override;
    std::vector<SessionHeader>   list() const override;
    void                         erase(SessionId) override;
    EventRange                   read(SessionId, Sequence after = 0) const override;
    EventRange                   readRange(SessionId, Sequence from, Sequence to) const override;
    Sequence                     append(SessionId, Event) override;
    bool                         isLeaseHolder(SessionId) const override;

    // ---- 02 additions (pinned; not part of the 01 seam) --------------------
    // Atomic multi-event append for producer coalescing (01 §16.1(c), §6.2).
    // Commits all events in one transaction; returns their sequences ascending.
    std::vector<Sequence> appendBatch(SessionId, std::span<const Event> events);

    // Lease operations (§5). acquire() is the create/resume path; renew() is
    // called on the renew_interval; release() is the SessionHandle teardown.
    LeaseState leaseState(SessionId) const;
    bool       acquireLease(SessionId);
    void       renewLeases();          // all sessions held by this daemon
    bool       releaseLease(SessionId);

    // Derived cache (§6.4–§6.6).
    void                           checkpoint(SessionId);
    std::optional<SessionSnapshot> loadSnapshot(SessionId) const;
    bool                           snapshotIsCurrent(SessionId) const;  // at == head (I21)
    void                           discardSnapshot(SessionId);

    int  schemaVersion() const;        // PRAGMA user_version
    void close();                      // flush, checkpoint, release, close fd

private:
    class Impl;                        // pimpl: sqlite3*, flock fd, writer mutex
    std::unique_ptr<Impl> impl_;
};

} // namespace ymh
```

`SessionPersistence::read` resolves a fork's logical view (§4.6) and is `const`;
it uses a mutable read connection and never writes (P18). `append`/`appendBatch`
are the only mutating entry points. An instance from `openReadOnly` is read-only:
every mutating operation (`append`, `appendBatch`, `erase`, the lease operations,
and `checkpoint`) throws `StoreOpenError`; only the `const` read methods are
valid, and `isLeaseHolder` returns false (P18, §4.3).

### 4.3 Open / close policy

Open (`open`, the writer):

1. Verify the workspace `cwd` exists and is a directory; otherwise raise the
   path error for 01 S14 and abort (never `chdir`, never create the directory).
   This precedes any filesystem mutation so a missing workspace is not papered
   over by a stray `.ymh/`.
2. Create `<workspace>/.ymh/` (mode `0700`) if absent.
3. Acquire the sidecar flock `LOCK_EX|LOCK_NB` on `lock_path` (§5.1). Failure ⇒
   `StoreOpenError`; another daemon owns this workspace and this process must not
   write (§5.1, P-F14). **Only the writer takes `LOCK_EX`**; a reader never
   contends for the lease (§6.3, §8).
4. `sqlite3_open_v2(db_path, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|
   SQLITE_OPEN_FULLMUTEX)`.
5. Apply the §3.2 pragmas.
6. Run the version/migration gate (§3.4) and the recovery protocol (§7.1).
7. Write the holder diagnostic `{pid, boot_id, acquired_at}` into `lock_path`,
   stamping the caller-supplied `PersistenceConfig::boot_id` (best-effort;
   §4.1, §5.1). The store never mints the nonce; the daemon mints it and passes
   it in before `open()`.

Open (`openReadOnly`, a reader):

1. Verify the workspace `cwd` exists and is a directory (01 S14).
2. `sqlite3_open_v2(db_path, SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX)`; a
   missing file ⇒ `StoreOpenError` (a reader never creates the DB).
3. Apply the §3.2 pragmas.
4. Run the version gate (§3.4) but **never migrate**: a DB whose `user_version`
   is not `SCHEMA_VERSION` (including an uninitialized `v0`) ⇒
   `SchemaVersionError`. A reader never writes and never runs recovery writes.
5. **Take no sidecar flock.** A read-only open succeeds while a live writer holds
   `LOCK_EX`, so a second read-only process (CLI `ymh show`, supervisor bootstrap,
   WAL readers) is always allowed; the reader never contends for the write lease
   (§5.1, §8). Reads run inside a WAL read transaction and therefore observe only
   committed state (§4.6, §7.2).

Close (`close`, the writer):

1. Commit any in-flight transaction (there is none between calls; each append is
   its own transaction, §6.1).
2. `PRAGMA wal_checkpoint(TRUNCATE)` to fold the WAL back into the main DB and
   reset its size (§6.3).
3. Release all held leases (`DELETE ... WHERE holder_pid=me AND
   holder_boot_id=me`) and clear the lock-file diagnostic.
4. Release the sidecar flock (`flock(fd, LOCK_UN)` + `close`).
5. `sqlite3_close_v2`.

Crash/abnormal exit skips 1–4; SQLite recovers the WAL and the kernel releases
the flock (§7). The daemon must call `close()` on graceful shutdown (spec 04).
An `openReadOnly` instance holds no flock and no lease, so its destructor only
runs `sqlite3_close_v2` — no checkpoint, no lease release, no `flock` unlock.

### 4.4 `append` / `appendBatch` and transaction shape

The append path is the pinned write path from 01 §6.1 / §16.1(a): persist first,
publish after. The lease is checked **inside the same write transaction** that
inserts the event, immediately before `COMMIT` (§9.7, P15).

```text
append(session_id, event):
  BEGIN IMMEDIATE
    -- 1. lease gate (inside the write txn; §9.7, P15)
    row := SELECT holder_pid, holder_boot_id, expires_at
             FROM session_leases WHERE session_id = ?
    if row is NULL
       or (row.holder_pid, row.holder_boot_id) != (me.pid, me.boot_id):
        ROLLBACK; throw LeaseLost
    -- 2. durable insert
    INSERT INTO events(session_id, event_id, timestamp, type, payload)
      VALUES (?, ?, ?, ?, ?)                       -- sequence := last_insert_rowid()
    seq := last_insert_rowid()
    -- 3. header + lease bookkeeping in the SAME txn
    UPDATE sessions SET updated_at = ? WHERE id = ?          -- 01 I22, P17
    UPDATE session_leases
       SET expires_at = now + TTL
       WHERE session_id = ? AND holder_pid = ? AND holder_boot_id = ?
    -- 4. lease re-check immediately before COMMIT (§9.7, P15)
    if the lease row no longer names (me.pid, me.boot_id):
        ROLLBACK; throw LeaseLost
  COMMIT
  return seq
```

`appendBatch` is the same with a loop over events inside one transaction; it
returns all assigned sequences. It exists so a producer can coalesce streamed
deltas into one durable transaction (01 §16.1(c), §6.2).

Failure behavior:

- Any SQL error before `COMMIT` ⇒ `ROLLBACK`, nothing is published, the in-memory
  log is unchanged (01 §6.1). A unique-`event_id` violation is a
  `CorruptionError` (01 S1); a `SQLITE_BUSY` after `busy_timeout` is a transient
  store error (P-F6).
- The lease check failing ⇒ `LeaseLost`, no write (01 I5, S4).
- The `updated_at` update is in the same transaction as the event insert, so
  I22 holds even across a crash (P17).
- Sequence values are DB-global and may skip (P6); the caller must not assume
  density (01 §16.1(b)).

`append` never publishes on the bus; 01's `Session::appendEvent` publishes after
this call returns (01 §6.1). The store has no `EventBus` dependency (01 §16.1(a):
the bus subscription is observer-only).

### 4.5 Fork physical strategy (pinned)

01 §16.1(d) pins COW/shared-prefix as the default and defers the exact physical
choice to this spec. **Pinned: a fork stores no copied event rows.** A fork's
resolved view is computed at read time from the parent chain:

```text
resolve(session):
  if session.kind == Root or session.kind == Subagent:
      return SELECT * FROM events WHERE session_id = session.id ORDER BY sequence
  if session.kind == Fork:
      parentView := resolve(load(session.parent_session))
      prefix     := parentView[0, session.seed_length)     -- 01 I10
      own        := SELECT * FROM events WHERE session_id = session.id ORDER BY sequence
      return prefix ++ own
```

Consequences, all pinned:

- The child's `events` rows are only its **own** events (its first own event is
  its own `SessionStarted`, 01 I10); the inherited prefix physically remains
  under the parent's `session_id`.
- No `session_forks` / boundary column is added to §9.2's schema: `seed_length`
  plus the append-only parent log is sufficient. Because `events.sequence` is
  DB-global and the parent is append-only, `prefix` (smaller sequences) always
  precedes `own` (larger sequences), so the concatenation is ascending (01 §6.2).
- Resolution is **recursive** over the parent chain (fork-of-fork composes, 01
  §15.1) and terminates at a root/subagent. The depth is bounded by the session
  graph; the store may memoize within a single `read` call.
- Physically copying rows is reserved for export and cross-DB copies, not for the
  live store (01 §16.1(d)).
- Because `sessions.parent_session` has a plain `FOREIGN KEY` (§9.2), a parent
  with fork children cannot be deleted while the children exist; this is what
  keeps a fork's prefix resolvable. `erase` therefore refuses a parent with
  dependents (§4.7, P-F13).

### 4.6 Read side and resolved views

`read(session, after)` returns the resolved view in ascending `Sequence` order,
filtered to `seq > after` (01 §6.2). `readRange(session, from, to)` returns the
resolved view filtered to `from <= seq <= to` (inclusive). `Sequence` never
crosses the transport boundary; only `EventRecord` carries it (01 §4.6).

For a fork, `after`/`from`/`to` are interpreted over the **resolved** view's
sequence values (which are the parent's actual sequences for the prefix and the
child's for the suffix), not over a per-session counter. This is consistent with
01 I2 (within a session, sequences strictly increase; `events()` is ascending by
sequence) because the resolved view is exactly what `Session::events()` returns
(01 §6.2). Reads run inside a WAL read transaction, i.e. over a consistent
committed snapshot, so a read-only connection coexists with a live writer and
never observes a half-committed event (§4.3, §7.2).

`load` returns the header; `list` returns headers (used by 01's `SessionStore`
seam and, in MVP, by the daemon; the ordered open-set list is the registry's job,
§9.10, not this `list`). `list` does not acquire leases.

### 4.7 `erase`

`erase(session)` implements the physical removal behind 01 §9.8's destructive
delete (I16). It is a single transaction and refuses to orphan dependents:

```text
erase(session_id):
  BEGIN IMMEDIATE
    if EXISTS(SELECT 1 FROM sessions WHERE parent_session = ?):
        ROLLBACK; throw DependentSessionError        -- P-F13
    DELETE FROM session_snapshots WHERE session_id = ?   -- derived cache first
    DELETE FROM session_leases    WHERE session_id = ?
    DELETE FROM events            WHERE session_id = ?   -- only removal of log rows (I1)
    DELETE FROM sessions          WHERE id = ?
  COMMIT
```

01 I16 requires that delete append `SessionEnded` **before** removal; that append
is performed by `SessionManager::deleteSession` (01 §8), not by `erase`. `erase`
assumes the `SessionEnded` row is already committed and is the final physical
step. `erase` does **not** emit anything itself.

Because the FK on `events`/`session_leases` has no `ON DELETE` action, the order
above is mandatory under `foreign_keys=ON`; reversing it raises an FK violation
(P8). `session_snapshots` additionally has `ON DELETE CASCADE` as
defense-in-depth.

### 4.8 `SessionHandle`

`SessionHandle` is the IPC-addressable teardown capability that owns the lease
(§9.7, §9.8; 01 §7). The abstract shape is frozen by 01; this spec pins the
concrete implementation and its lease ownership.

```cpp
namespace ymh {

// Frozen by 01 §7.
class SessionHandle {
public:
    virtual ~SessionHandle() = default;
    virtual SessionId session() const = 0;
    virtual bool      holdsLease() const noexcept = 0;
    virtual void      release() = 0;   // release lease; daemon keeps or reaps
};

// Concrete handle returned by the persistence layer for a writable session.
class SqliteSessionHandle final : public SessionHandle {
public:
    SqliteSessionHandle(SessionPersistence& store, SessionId id, bool writable);
    ~SqliteSessionHandle() override;              // release() on destruction

    SessionId session() const override { return id_; }
    bool      holdsLease() const noexcept override { return writable_ && !released_; }
    void      release() override;                 // idempotent

private:
    SessionPersistence& store_;
    SessionId           id_;
    bool                writable_;   // false for replay (01 I19)
    bool                released_ = false;
};

} // namespace ymh
```

Rules:

- A handle created for create/resume is `writable_ == true` and owns the lease;
  `~SqliteSessionHandle` calls `release()`, so teardown is RAII-safe.
- A handle created for **replay** is `writable_ == false`: it never acquires the
  lease and `release()` is a no-op (01 I19, §9.5).
- `release()` deletes the lease row only if it still names this daemon's
  `(pid, boot_id)` (§5.3); it is idempotent and safe to call twice.
- Handles are addressed by `SessionId` over the transport (spec 05); a
  `host.shutdown` (04) releases every handle it owns.

---

## 5. Write lease (pinned)

The lease is the cross-process write-ownership mechanism (§9.7, DIV-3/RISK-1).
It exists so that two daemons can never interleave appends to one session's log.

### 5.1 The sidecar `flock`

The **authoritative** liveness anchor is an exclusive `flock` on
`<workspace>/.ymh/sessions.lock` (§9.7, §9.10). It is **not** an fcntl lock on
the SQLite file: SQLite's own locks are per-connection and unrelated to process
liveness (§9.7).

```text
open(lock_path, O_RDWR|O_CREAT|O_CLOEXEC, 0600)
flock(fd, LOCK_EX|LOCK_NB)     -- held for the daemon's whole lifetime
  on EWOULDBLOCK -> StoreOpenError; another daemon owns this workspace
```

- The kernel releases the flock automatically on process death (including
  `SIGKILL`), so a crashed daemon never leaves the workspace permanently locked
  (§9.7).
- Exactly one daemon per workspace holds the flock. It is the same lock the
  registry uses for host liveness (§9.10), so host liveness and lease liveness
  share one kernel object.
- On acquiring it, the writer `open()` writes `{"pid":…, "boot_id":"…",
  "acquired_at":…}` into `lock_path` as a **diagnostic**, stamping the
  `PersistenceConfig::boot_id` supplied by the daemon (§4.1). The store never
  mints the nonce. The diagnostic is best-effort and never authoritative:
  authority is the kernel flock, not the file contents.
- The store never steals based on `expires_at` alone; it requires the flock
  (§5.3, P2). This is the mechanism that prevents split-brain.

### 5.2 Boot nonce (`holder_boot_id`)

`BootId` is a UUIDv4 minted **once by the daemon at startup**, before the store
opens, and injected through `PersistenceConfig::boot_id` (§4.1). The writer
`open()` stamps it into the sidecar-lock diagnostic (§5.1) and the store writes
it into every `session_leases.holder_boot_id` the daemon holds (§9.2), as well as
the registry's `workspaces.host_boot_id` (§9.10). The store never mints the
nonce. It is a PID-reuse guard: PID alone
can be recycled across process restarts or OS boots, but `(pid, boot_id)` cannot
be mistaken for a different daemon's lease (P4). The boot nonce lives **only** in
`session_leases` and the registry — never in `SessionHeader` (HANDOFF §5 item 3,
01 §3).

### 5.3 acquire / renew / steal / release

The four operations of §9.7, with the pinned steal guard:

```text
acquire(session_id):                    -- create/resume path
  INSERT INTO session_leases(session_id, holder_pid, holder_boot_id,
                             acquired_at, expires_at)
    VALUES (?, me.pid, me.boot_id, now, now + TTL)
  ON CONFLICT(session_id) DO NOTHING
  if changes() == 0:                    -- a row exists; only a safe steal may replace it
      return steal(session_id)
  return HeldByMe

renew(session_id):                      -- on renew_interval (all held sessions)
  UPDATE session_leases
     SET expires_at = now + TTL
   WHERE session_id = ? AND holder_pid = me.pid AND holder_boot_id = me.boot_id
  if changes() == 0: return LeaseLost   -- row vanished or was stolen

steal(session_id):                      -- only callable by the flock holder
  row := SELECT holder_pid, holder_boot_id, expires_at FROM session_leases
           WHERE session_id = ?
  if row is NULL: return Absent
  if (row.holder_pid, row.holder_boot_id) == (me.pid, me.boot_id): return HeldByMe
  if NOT flockHeldByMe(): return HeldByOther          -- P2: never steal without the flock
  if NOT (row expired OR !processAlive(row.holder_pid)):
      return HeldByOther                              -- live holder (incl. SIGSTOP)
  REPLACE INTO session_leases(...) VALUES (?, me.pid, me.boot_id, now, now+TTL)
  return HeldByMe

release(session_id):
  DELETE FROM session_leases
   WHERE session_id = ? AND holder_pid = me.pid AND holder_boot_id = me.boot_id
```

`acquire` is an idempotent `INSERT … ON CONFLICT DO NOTHING`; a non-empty result
routes through `steal`. `release` is a no-op if the row is already gone or has
been stolen; it never deletes another holder's row (P4).

### 5.4 Liveness: flock primary, `kill` hint, SIGSTOP guard

```text
flockHeldByMe() -> bool        -- did THIS process acquire the sidecar flock? (§5.1)
processAlive(pid) -> bool      -- kill(pid, 0): true unless errno == ESRCH
```

Rules (P3):

1. **Primary test.** The sidecar flock is the liveness authority. If this process
   holds it, no other daemon for this workspace is alive, so any lease row not
   held by this process belongs to a dead daemon and may be stolen (§5.3).
2. **Hint only.** `kill(pid, 0)` is a secondary hint, **never the sole test**
   (§9.7). It cannot distinguish a live-but-frozen process: a `SIGSTOP`ped
   holder still returns `0`. `EPERM` means "exists but not ours" ⇒ treat as
   alive. Only `ESRCH` proves death.
3. **SIGSTOP / split-brain guard.** A `SIGSTOP`ped daemon still holds the
   sidecar flock (the kernel does not release a flock on `SIGSTOP`), so no other
   process can acquire it and therefore no other process may steal its leases —
   even though its `expires_at` has passed and `kill(pid,0)` cannot prove it
   alive. When it resumes (`SIGCONT`) it renews. TTL expiry alone never
   authorizes a steal (P2).
4. **PID reuse.** A row whose `holder_boot_id` differs from the current boot's
   nonce, or whose `(pid, boot_id)` does not match a live daemon, is stale. A
   recycled PID with a different boot nonce is never mistaken for the holder
   (§9.7, P4).

### 5.5 TTL and renewal

- `lease_ttl` default 15 s; `renew_interval` default 5 s (≈ TTL/3, two missed
  renewals before expiry). Both are `PersistenceConfig` fields (§4.1).
- The daemon renews **all** leases it holds on every `renew_interval` tick
  (`renewLeases()`, §4.2). A successful renew refreshes `expires_at`.
- `expires_at` is a **freshness bound and a liveness hint**, not a steal right:
  with the flock guard (§5.4) a live-but-frozen holder is never stolen regardless
  of expiry (P2). Expiry mainly bounds how long a crashed holder's row is
  considered "recent" and drives lazy cleanup.
- A holder that cannot renew (row missing, or DB error) has lost the lease and
  degrades to read-only (§5.7).
- Wall-clock time (`system_clock`) is used for `expires_at` because it is
  persisted and compared across processes; the renew timer itself uses a
  monotonic clock so a wall-clock jump does not spuriously stop renewals. Clock
  skew can only make a row look older, which is harmless because expiry alone
  does not authorize a steal (P2).

### 5.6 Lease check before `COMMIT`

§9.7 requires that "the daemon's `SessionPersistence` checks the lease before
each `COMMIT`" and 01 §10.1 repeats it. The pinned shape (§4.4):

- The lease row is read at the top of the write transaction (`BEGIN IMMEDIATE`
  takes the DB write lock, so no other process can mutate the row concurrently)
  and re-read immediately before `COMMIT`.
- Both reads must see `(holder_pid, holder_boot_id) == (me.pid, me.boot_id)`;
  otherwise `ROLLBACK` + `LeaseLost`.
- The same transaction renews `expires_at`, so a healthy holder never fails its
  own check. The check is about ownership, not freshness.

### 5.7 Lease-loss behavior

When `append`/`appendBatch` throws `LeaseLost`, or `renewLeases()` finds a row
lost (01 S4):

1. The session degrades to **read-only**: `isLeaseHolder` returns false,
   `append` keeps throwing, `read`/`deriveMessages`/replay keep working (01
   §10.1, I19).
2. The daemon notifies its supervisor (§9.7) and the UI shows the session as
   non-writable (spec 10).
3. The store never force-writes and never retries a lost lease on its own; a
   re-acquire must go through `acquireLease`/`steal` (§5.3), which requires the
   sidecar flock.

---

## 6. Flush, checkpoint, and snapshots

### 6.1 Commit / flush policy

- **Every `append`/`appendBatch` is its own transaction and commits before it
  returns.** This is what makes durable-before-observable (01 I4) true at the
  store: the caller (`Session::appendEvent`) publishes only after the call
  returns (01 §6.1, P5).
- There is **no deferred/buffered write** and no "flush on interval" for the log:
  an event is either committed or not. This keeps resume/replay exact (D2).
- `synchronous=NORMAL` under WAL means a returned commit survives an
  application/process crash (§3.2); an OS crash or power loss may lose recent
  commits unless `synchronous=FULL` is set. Power-loss durability is the
  documented `FULL` override.
- A crash between `COMMIT` and the in-memory log update (01 §6.1 step 4) is
  harmless: the committed event is in the DB, and resume re-reads it.

### 6.2 `AssistantChunk` producer coalescing

01 §16.1(c) keeps `AssistantChunk` durable but lets the producer coalesce deltas
into **bounded batches**; the exact cadence defers to specs 02 and 06. Pinned
bounds (spec 06 chooses a policy inside them):

- A chunk batch is flushed when any of: the batch reaches `max_chunk_batch`
  (default 32), `chunk_flush_interval` elapses (default 100 ms), or a non-chunk
  event is about to be appended (a **barrier**: ordering must be preserved, so
  the pending chunks commit first).
- A flush is one `appendBatch` call, i.e. one transaction, returning the batch's
  sequences. The caller publishes each committed chunk in order (01 I4/I6).
- Coalescing **never reorders** chunks and never merges distinct message ids;
  it only reduces transaction count. `AssistantChunk` remains durable so a
  streamed UI is replayable (01 §16.1(c), I14).
- On cancel/close, any pending chunk batch is flushed before the closing event
  (`TurnCancelled` / `SessionEnded`) so the log is ordered.

### 6.3 WAL checkpointing

- During normal operation, SQLite's `wal_autocheckpoint` (1000 pages, §3.2) folds
  WAL frames back into the main DB automatically; no application action.
- The store runs an explicit `PRAGMA wal_checkpoint(TRUNCATE)` on `close()`
  (§4.3) so a cleanly closed workspace has a small, self-contained DB.
- A `PASSIVE` checkpoint may be requested on daemon idle (spec 04); it never
  blocks readers/writers.
- WAL is the reason a second read-only process (CLI `ymh show`, the supervisor
  during bootstrap) can read without blocking the daemon's writer. Such a reader
  opens via `openReadOnly` (§4.3): it takes **no** sidecar flock and therefore
  never contends with the writer's `LOCK_EX` (§8, §9.10).

### 6.4 Snapshot creation

A snapshot is a derived cache (01 §6.4, I21). Creation is pinned as:

- **On demand:** `checkpoint(session)` computes `SessionSnapshot` from the
  resolved view and writes it in one transaction. It never mutates `events`.
- **Automatically:** on `close()`/session teardown and after a compaction event
  (spec 06), if `resolved_event_count >= snapshot_threshold_events` (default
  256).
- **Replacement:** `session_snapshots` has `session_id` as PK, so a new snapshot
  `REPLACE`s the old one atomically. There is exactly one snapshot per session.
- A snapshot records `at_sequence` = the current resolved head. It is **not**
  patched incrementally; any advance makes it stale (§6.5).

### 6.5 Snapshot staleness and pruning (S12)

01 I21: a snapshot is valid only while `at_sequence` equals the current log head;
otherwise it is stale and must be discarded and recomputed, never patched.

```text
snapshotIsCurrent(id):
  snap := loadSnapshot(id)
  return snap && snap->at == currentHead(id)

resume path:
  snap := loadSnapshot(id)
  if snap && snap->at == currentHead(id):
      use snap->messages                        -- I21 holds by construction
  else:
      discardSnapshot(id)                       -- never patch (S12)
      messages := deriveMessages(header, read(id))   -- spec 01 recomputes
```

Pruning rules:

- A snapshot with `event_count < snapshot_threshold_events` is deleted (cheaper
  to recompute than to load).
- `erase` deletes the session's snapshot (§4.7).
- A snapshot that fails to decode (`header_json`/`messages_json`) is treated as
  **absent**, deleted, and recomputed; it is never fatal (P-F12).
- The store never serves a snapshot as authority: `deriveMessages()` over
  `events` always wins, and replay tests assert equality (01 I21, §15.4).

### 6.6 Snapshot encoding

- `header_json` uses the same `SessionHeader` JSON as 01 §15.1's round-trip
  (every field, no boot nonce, no `ordinal`/`archived`).
- `messages_json` uses the `Message` JSON shape owned by spec 01 / spec 08.
- The snapshot payload carries a format tag (e.g. `{"v":1,…}`) so a future shape
  change is detected and the snapshot discarded rather than mis-decoded.
- Snapshots are stored in the same DB as the events so a crash cannot make the
  snapshot "ahead" of the log; if the log advanced, `at_sequence` catches it
  (§6.5). No cross-file consistency problem exists.

---

## 7. Crash recovery

### 7.1 Open-time consistency protocol

After the pragmas (§3.2) and the version gate (§3.4), `open` runs:

```text
1. WAL recovery: SQLite replays committed frames and discards uncommitted ones
   automatically on first access; the store does NOT hand-roll this (§7.2).
2. PRAGMA foreign_key_check            -- must be empty; else CorruptionError (P8)
3. PRAGMA integrity_check (quick)      -- optional, config-gated; failure -> CorruptionError
4. per-session sequence check          -- no duplicate/non-increasing seq (01 S2)
5. lease cleanup                       -- rows held by a dead daemon are left for
                                          lazy steal (§7.5); never cleared eagerly
6. snapshot check                      -- stale snapshots are NOT rewritten here;
                                          they are detected lazily at resume (§6.5)
```

The store decides a log is **consistent at open** when steps 2–4 pass. It does
**not** require a clean shutdown marker and does not add a dirty flag: SQLite's
transactional WAL already guarantees that a crash leaves every transaction
all-or-nothing, so a shutdown marker would be redundant state that can itself
desync (P12).

### 7.2 WAL replay

WAL recovery is delegated entirely to SQLite. On open, SQLite:
- replays all committed frames from `sessions.db-wal` into the DB image,
- discards frames from a transaction that never reached `COMMIT`,
- and truncates/keeps the WAL per checkpoint state.

The store must therefore never read the raw DB file bypassing SQLite/WAL (no
`cp`, hexdump, or `sqlite3` CLI on a live file), and must never attempt to
interpret the WAL itself. A SQLite read-only connection (`openReadOnly`, §4.3) is
allowed and sees only committed frames. This is the "crash recovery" contract:
after `open` returns, every row the store reads is from a committed transaction
(P12). This is the persistence-level counterpart of 01 I4: the store cannot
observe a half-committed event.

### 7.3 Torn append / half-written rows (S13)

- SQLite transactions are atomic at the page/WAL-frame level; a crash mid-append
  leaves either the whole `INSERT`+`UPDATE` transaction or none of it. A
  partially written `events` row is therefore **never visible** after WAL
  recovery (P12). 01 S13's "torn read" is prevented by the transaction boundary,
  not by application locking.
- If a stored `events.payload` fails to decode (e.g. a bug or bit rot), that is
  `CorruptionError` (01 S3) and replay fails loud on the offending row; the
  store never silently skips it (no lossy replay, 01 S3).
- A row whose `sequence` is not strictly greater than the session's previous
  committed sequence is `CorruptionError` (01 S2).

### 7.4 Incomplete transactions

- Each `append`/`appendBatch`/`erase`/`checkpoint` is exactly one transaction;
  there is no multi-transaction invariant that a crash could split.
- The `updated_at` update and the event insert share a transaction (P17), so
  I22 cannot be violated by a crash.
- The lease `expires_at` renewal shares the append transaction (§4.4), so a crash
  cannot leave the event committed with a stale lease timestamp from this
  transaction's perspective.

### 7.5 Lease rows after a crash

A crashed daemon's lease rows remain in `session_leases` with an old
`(holder_pid, holder_boot_id)` and a past `expires_at`. Recovery is **lazy**:

- On open, the new daemon acquires the sidecar flock (the kernel released the
  dead daemon's) and writes its own `(pid, boot_id)` diagnostic (§5.1).
- A lease row is cleaned only when the new daemon touches that session
  (`acquireLease`/`steal`, §5.3): it sees a non-self row, confirms it holds the
  flock, confirms the row is expired or its holder is not alive, and `REPLACE`s
  it.
- No eager sweep deletes rows: an eager sweep would risk deleting a live
  holder's row during a transient overlap. Lazy steal under the flock guard is
  the only mutation (P2).

### 7.6 Snapshot recovery

Snapshots need no special recovery: they are in the same DB and therefore
consistent with the log at the transaction level. A snapshot whose `at_sequence`
is behind the recovered head is stale and is discarded/recomputed at resume
(§6.5, S12). A snapshot from a newer/older format tag is discarded.

### 7.7 Corrupt database

If `PRAGMA foreign_key_check` is non-empty, `integrity_check` fails, or a decode
fails, the store raises `CorruptionError` and **refuses to serve writes**. The
daemon (04) surfaces the error, keeps the DB for inspection, and does not
silently recreate it (recreating would destroy the source of truth, D2). A
manual `ymh doctor`/export recovery path is deferred to v2; v1 fails loud and
preserves the DB, as recorded in decision §13.1(i).

---

## 8. Concurrency and threading

- **Cross-process:** exactly one writer per workspace, enforced by the sidecar
  flock (§5.1); exactly one lease holder per session, enforced by
  `session_leases` + the pre-`COMMIT` check (§5.6). Readers are WAL readers
  opened via `openReadOnly` (§4.3): they take no sidecar flock, never block the
  writer, and may run while a live writer holds `LOCK_EX` (§9.2, §9.10).
- **In-process:** `SessionPersistence` owns one **write connection** guarded by a
  writer mutex (SQLite `FULLMUTEX`), plus read connections. Because
  `append`/`appendBatch` already serialize through the writer mutex and 01 I18
  serializes per-session appends, a single session never has two in-flight
  transactions.
- `BEGIN IMMEDIATE` acquires the DB write lock up front, so a writer fails fast
  with `SQLITE_BUSY` (after `busy_timeout`) rather than deadlocking mid-statement
  (P-F6).
- `renewLeases()` is driven by the daemon's event loop (one main loop, §35) on a
  timer, not by a thread per session. `kill`/`flock` are cheap syscalls.
- The store's read methods (`read`, `readRange`, `load`, `list`, `loadSnapshot`)
  are `const` and never mutate durable state (P18); a snapshot is written only by
  `checkpoint`/`close`, never lazily on a read.

---

## 9. Invariants

Numbered `P1`–`P18` to avoid clashing with 01's `I1`–`I23`. Any code that can
violate one is a defect.

**P1 — Single writer per session.** At most one lease row exists per session
(`session_id` PK), and only its holder may commit events. (§9.2, §9.7, 01 I5)

**P2 — The sidecar flock is the steal authority.** A lease row is stealable only
by the process holding `<workspace>/.ymh/sessions.lock`; `expires_at` expiry
alone never authorizes a steal. This is the split-brain/SIGSTOP guard. (§9.7,
§5.3–§5.4)

**P3 — Liveness is flock-primary, `kill` is a hint.** `kill(pid,0)` is never the
sole liveness test; only `ESRCH` proves death; a SIGSTOPped holder is never
stolen. (§9.7)

**P4 — Boot-nonce identity.** Every lease row records `(holder_pid,
holder_boot_id)`; renew/release match that exact pair; a recycled PID is never
mistaken for the holder. (§9.2, §9.7)

**P5 — Durable-before-observable at the store.** `append`/`appendBatch` return
only after `COMMIT`; the store never publishes and never returns an uncommitted
sequence. (01 I4, §6.1)

**P6 — Store-only, DB-global, gap-tolerant sequences.** `SessionPersistence` is
the sole assigner of `Sequence`; `events.sequence` is `AUTOINCREMENT`, strictly
increasing within a session, and **not** dense. (01 I2, §9.2, 01 §16.1(b))

**P7 — Append-only physical log.** No `UPDATE`/`DELETE` of `events` rows except
the whole-session `erase` transaction; snapshots never mutate `events`. (01 I1,
D2)

**P8 — Foreign keys enforced.** Every connection sets `foreign_keys=ON`; no
orphan `events`/`session_leases`/`session_snapshots` rows exist after any
operation. (§9.2)

**P9 — WAL + busy timeout.** Every connection opens in WAL with a busy timeout;
readers never block the writer. (§9.2, §9.10)

**P10 — Fork is a logical shared prefix.** A fork stores no copied event rows;
its resolved view is `resolve(parent)[0, seed_length] ++ own`. (01 I10, §16.1(d),
§4.5)

**P11 — Snapshot is derived and discardable.** A snapshot never overrides
`events`; a stale one is discarded and recomputed, never patched. (01 I21, S12)

**P12 — Crash consistency.** After a crash, WAL recovery yields only
fully-committed transactions; no torn or half-written row is ever observable.
(§7.2–§7.3, 01 S13)

**P13 — Explicit schema version.** `user_version` gates open; a newer version
refuses; migrations are transactional, additive-first, and never rewrite the log.
(§3.4)

**P14 — Erase is atomic and referentially safe.** Whole-session delete removes
snapshot+lease+events+sessions in one transaction and refuses to orphan
dependents. (01 I1, I16, §4.7)

**P15 — Lease check inside the write transaction.** The lease is verified within
the same transaction as the event insert, immediately before `COMMIT`. (§9.7,
§4.4, §5.6)

**P16 — Identity is rooted, never `getcwd()`.** `db_path`/`lock_path` are derived
from the canonical workspace root; the store never resolves a path from
`getcwd()`. (01 I15, §9.7)

**P17 — `updated_at` is transactional.** The store advances `sessions.updated_at`
in the same transaction as the event insert. (01 I22, §9.2)

**P18 — Reads never write.** `read`/`readRange`/`load`/`list`/`loadSnapshot` are
`const` and mutate no durable state; replay never acquires the lease. (01 I19)

---

## 10. Failure modes

### 10.1 Shared findings (F1–F12, §54)

The persistence layer's responsibilities for the existing findings:

| F# | Finding | Persistence-layer handling |
|---|---|---|
| **F1** | path/process isolation | `db_path`/`lock_path` rooted at the canonical workspace; no `chdir()`; no `getcwd()` (P16, 01 §10.2) |
| **F2** | background permission | out of scope here; `PermissionDecision` is a durable event that this store persists like any other (01 §13.1, spec 09) |
| **F3** | late event after close | `erase` is the final physical step; it runs only after the closing `SessionEnded` is committed and the mailbox drained (01 I16/I20, §4.7) |
| **F4** | edge-triggered attention | out of scope; no timers or attention state in the store (§9.9, spec 10) |
| **F5** | output ring buffers | oversized `ToolResult.output` is rejected by the payload cap before commit or recorded honestly as truncated (01 S10, §9.11, §4.4) |
| **F6** | input/keybinding focus | out of scope (§20.24) |
| **F7** | per-session dirty flags | out of scope; no per-session UI state in the store (§20.23) |
| **F8** | resource caps | the store bounds payload size and DB growth; process/PTY caps are per-host (§9.11) |
| **F9** | cancellation scoping | `TurnCancelled` is persisted as a distinct event; `appendBatch` never mixes two sessions' events (01 I11, §34) |
| **F10** | resume-suspended | resume acquires the lease but appends nothing; no auto-resume burst (01 I18, §9.9, §9.3) |
| **F11** | subagent ID duality | subagents have their own `sessions` row/log; `parent_session` is a plain FK, so a subagent's durability is independent (01 I17, §20.25) |
| **F12** | flash clock in model | out of scope; no wall-clock UI state in the store (§9.9) |

**Explicitly out of scope for this component:** F2, F4, F6, F7, F9 (event shape),
F10 (policy), F12. The store supplies the durable substrate those components
project; it owns none of their logic.

### 10.2 Component-local failure modes (`P-F#`)

These are component-local to persistence and are not part of the top-level
F1–F12 set. They must be covered by tests (§12.6).

| P-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **P-F1** | Cannot acquire lease (live holder) | `acquire` sees a non-self, un-expired row; `flock` held | Do not write; return `HeldByOther`; caller surfaces read-only (01 S4, §5.3) |
| **P-F2** | Lease lost mid-session | `isLeaseHolder` false, renew finds row gone, or pre-`COMMIT` check fails | `ROLLBACK`, throw `LeaseLost`, degrade read-only, notify supervisor (§5.6–§5.7, 01 S4) |
| **P-F3** | Stale lease row from a dead holder | flock acquired; row's `(pid,boot_id)` non-self; `kill` ESRCH or expired | Safe `REPLACE` steal; never steal a self row (§5.3) |
| **P-F4** | PID reuse | row's `holder_boot_id` ≠ live holder's nonce | Treat row as stale; never match by PID alone (§5.2, P4) |
| **P-F5** | SIGSTOPped holder / split-brain | flock still held by the frozen daemon; `expires_at` passed; `kill` returns 0 | **No steal**; wait for `SIGCONT`/death; TTL alone insufficient (§5.4, P2) |
| **P-F6** | Concurrent writer `SQLITE_BUSY` | `BEGIN IMMEDIATE` fails after `busy_timeout` | Retry with bounded backoff; on exhaustion surface a transient store error; never force-write (§8) |
| **P-F7** | Disk full / I/O error on `COMMIT` | SQLite error at commit | `ROLLBACK`; nothing published; if persistent, degrade the session read-only (§5.7) |
| **P-F8** | Corrupt WAL/DB or failed `foreign_key_check` | `SQLITE_CORRUPT`, `integrity_check`, or `foreign_key_check` non-empty | `CorruptionError`; refuse writes; preserve the DB; do not recreate (§7.7, 01 S3) |
| **P-F9** | Undecodable stored row/payload | JSON decode fails on read | Fail loud on the offending record; never skip silently (§7.3, 01 S3) |
| **P-F10** | Newer/unknown schema version | `user_version > SCHEMA_VERSION` or foreign `application_id` | `SchemaVersionError`; refuse to open; never write (§3.4, P13) |
| **P-F11** | Migration interrupted | failure inside the migration transaction | `ROLLBACK`; DB stays at the old version; open fails loud (P13) |
| **P-F12** | Snapshot decode failure / staleness | JSON decode fails, or `at_sequence` ≠ head | Delete/discard snapshot; recompute from `events`; never fatal (§6.5, 01 S12) |
| **P-F13** | `erase` would orphan dependents | `EXISTS(sessions.parent_session = id)` | `DependentSessionError`; caller deletes children first (§4.7, P14) |
| **P-F14** | Sidecar lock missing/unwritable | `open(lock_path)`/`flock` fails | `StoreOpenError`; cannot establish liveness; do not write (§5.1, 01 S11) |
| **P-F15** | Payload/metadata exceeds cap | size check before `INSERT` | Reject with `PayloadTooLarge`; no commit (01 S10) |
| **P-F16** | Non-monotonic / duplicate sequence on read | sequence ≤ session head, or `event_id` collision | `CorruptionError`; refuse to project (01 S1/S2) |

---

## 11. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (§55). The persistence layer maps
onto it as follows.

| dsh concept | ymh persistence layer | Reference |
|---|---|---|
| Session event log (persisted) | `SessionPersistence` over `<workspace>/.ymh/sessions.db` | §9.2, §55 |
| Session store (SQLite) | `SessionStore` abstract seam + `SessionPersistence` impl | 01 §7, §55 |
| Session record | `sessions` table (`SessionHeader`) | §9.2 |
| Append-only trace | `events` table + DB-global `AUTOINCREMENT` | §9.2, §9.1 |
| Session/turn/step traceability | durable `turn/*`, `step/*` rows | §8.1, 01 §4 |
| Cross-process ownership | `session_leases` + sidecar `flock` | §9.7, §9.10 |
| Crash-recovery marker | per-workspace DB is transactional (WAL); the registry's `pending_mutation` marker is the registry's concern | §9.10, §7.1 |
| Cordis service seam | `SessionStore` as a capability interface | §4.3, §55 |
| Checkpoint / snapshot | `session_snapshots` derived cache | 01 §6.4, §6 |
| Storage plugin separation | store is a seam; engine is SQLite (§48) | §4.3, §48 |

**Deliberate omissions** (accepted for v1, §55): no Cordis-compatible
configuration, no plugin dependency graph, no hot module replacement, no
external storage backend. The store is a headless, provider-agnostic capability
(G1, G3; §2.1). The shared registry's `pending_mutation` marker is **not**
duplicated here: the per-workspace DB is transactional and needs no such marker
(§7.1), while the registry's two-write divergence is spec 03's problem (§9.10).

---

## 12. Test plan

Strategy is §44: unit, integration (fake LLM / fake FS / fake shell), golden,
replay, and a separate live PTY/real-LLM layer (§44, §45). The deterministic
layers run offline against the Fake LLM (§45); the live layer is opt-in and
API-key gated. Persistence tests use a temporary real SQLite file where the
subject is SQLite semantics, and a fake `SessionStore` where the subject is
spec-01 behavior (01 §7).

### 12.1 Unit tests

Against a temporary real DB file (and, where noted, a fake store):

- **Open / pragmas / version**
  - Every connection reports `foreign_keys=1`, `journal_mode=wal`,
    `busy_timeout=5000` (§3.2, P8/P9).
  - Fresh DB gets `user_version=1` and `application_id=0x594D4801`; a foreign
    `application_id` is rejected (P-F10).
  - `user_version > SCHEMA_VERSION` ⇒ `SchemaVersionError`, no write (P-F10);
    a simulated `v0 → v1` migration is transactional and bumps `user_version`
    only on success (P-F11, P13).
- **Schema**
  - The `sessions` CHECK rejects `kind='fork'` with a NULL `seed_length` and
    `kind='root'` with a parent; `kind='subagent'` accepts NULL/0 seed (01 I9,
    §9.2).
  - `events.event_id UNIQUE` rejects a duplicate (01 S1, P-F16).
  - `foreign_keys=ON` blocks deleting a `sessions` row with live `events` (P8,
    §4.7).
- **Append**
  - `append` returns a strictly increasing sequence; gaps are allowed and
    asserted (P6).
  - `appendBatch` assigns N sequences in one transaction and returns them
    ascending; a failure rolls back all N (P5).
  - `updated_at` advances to the last event timestamp (P17, 01 I22).
  - A non-holder `append` throws `LeaseLost` and leaves no row (P-F1/P-F2, P15).
  - A payload over the cap throws `PayloadTooLarge` with no commit (P-F15).
- **Fork resolution**
  - `read(fork)` equals `read(parent)[0, seedLength] ++ read(child)`, ascending
    (P10, 01 I10); fork-of-fork composes.
  - A parent with children cannot be erased (`DependentSessionError`, P-F13).
- **Erase**
  - `erase` removes snapshot+lease+events+sessions in one transaction and leaves
    no FK orphans (P14); a mid-transaction failure rolls back everything.
- **Snapshot**
  - `snapshotIsCurrent` is true only when `at == head`; a stale snapshot is
    discarded and never patched (P11, 01 I21, 01 S12).
  - A corrupt `messages_json` is treated as absent (P-F12).
- **Lease**
  - `acquire`/`renew`/`release` succeed only for the exact `(pid, boot_id)` (P4).
  - A self-row is never stolen; a non-self expired row with the flock held is
    stolen (P-F3).
  - `kill(pid,0)` returning 0 (simulated SIGSTOP) does not authorize a steal
    (P-F5, P2).
- **Failure modes P-F1–P-F16** each have a dedicated unit test (§12.6).

### 12.2 Integration tests (Fake LLM, §45)

Driven by the deterministic `FakeLLM` (§45) so the whole loop is offline:

- **Persistence round-trip**: append user → assistant → tool call → tool result →
  assistant through the real `SessionPersistence`, close, reopen, `read`, and
  assert identical `EventRange` and projection (01 §15.2; §44 "session
  persistence").
- **Resume after restart**: reconstruct after a simulated process restart;
  assert the projection matches and no event is appended on resume (01 F10,
  P5).
- **Crash recovery**: kill the writer mid-stream (child process), reopen, and
  assert every visible event is fully committed and the WAL was replayed
  (§7.2, P12). No torn row is ever projected (P12, 01 S13).
- **WAL readers**: a second process opens via `openReadOnly` (no flock, no
  migration) and sees committed events without blocking the writer (P9, §4.3,
  §9.10).
- **Lease contention**: a second process for the same workspace cannot acquire
  the flock and does not write (P-F14); a crashed holder's lease is stolen only
  after the flock is free (P-F3); a SIGSTOPped holder is not stolen (P-F5).
- **Fork + persist**: fork a persisted parent, append to the child, reopen, and
  assert the child's resolved view is the shared prefix plus its own events
  (P10, 01 I10).
- **Snapshot path**: force a checkpoint, append more, resume, and assert the
  stale snapshot is discarded and the recomputed projection is correct (P11,
  01 S12).
- **Fake FS**: the store is pointed at a Fake-FS workspace root (§44) and asserts
  it never calls `getcwd()` and roots all paths at the workspace (P16, 01 F1).
- **Payload cap**: a large `ToolResult.output` is either rejected or recorded
  with `truncated=true` and the flag survives reopen (01 F5, P-F15).

### 12.3 Golden tests

Given a deterministic event stream persisted to a real DB, render through the UI
projection and compare to the expected terminal representation (§44 "given event
stream → render → expected terminal representation"). The store supplies the
fixture DB; the renderers are spec 10. Golden fixtures use only durable events
so they are replayable, and the DB is rebuilt from a golden event list rather
than committed as a binary fixture.

### 12.4 Replay tests

Same input log ⇒ same projected state (§44), including:

- a log persisted and reopened with gaps in `sequence` (P6),
- a fork whose prefix is physically under the parent,
- a log ending in an open step (01 S8),
- a snapshot that is stale and must be recomputed (P11, 01 S12).

Replay tests assert `deriveMessages()` equals the snapshot's `messages` when the
snapshot is current, and that a stale snapshot is discarded (01 I21, 01 §15.4).

### 12.5 Live end-to-end tests (real LLM, PTY-driven, §44)

The live layer exercises the real product as a human would: spawn the real `ymh`
binary under a PTY, type a prompt, read rendered output, and assert observable
behavior (§44). Persistence-relevant assertions:

- A session created in a live run leaves a valid `<workspace>/.ymh/sessions.db`
  with the expected `sessions`/`events` rows and no FK orphans (P8).
- Killing the daemon mid-turn and restarting resumes the session with the
  committed prefix and no duplicate events (P5, P12).
- Two live workspaces each own their own DB and lock file; neither blocks the
  other's WAL readers (P9, §9.10).

Milestone gating: MVP live tests cover the single-process flow; the
two-workspace/daemon tests target Milestone 2 (§44, §57 Step 13, §58). The live
layer is skipped, not failed, without an API key (§44).

### 12.6 Failure-mode coverage matrix

| Failure | Test layer | Asserts |
|---|---|---|
| F1 | unit + integration | rooted `db_path`/`lock_path`, no `getcwd` (P16) |
| F3 | unit | `erase` runs only after the closing `SessionEnded`; mailbox drained (01 I16/I20) |
| F5 | unit + integration | oversized output rejected or `truncated` preserved through reopen (P-F15) |
| F8 | unit | payload cap bounds the row (P-F15) |
| F11 | unit | subagent `parent_session` FK keeps its own log independent (P8) |
| P-F1 | unit + integration | live holder blocks a second acquirer |
| P-F2 | unit | non-holder `append` throws `LeaseLost`, no row written (P15) |
| P-F3 | integration | dead holder's row stolen only after the flock is free |
| P-F4 | unit | boot-nonce mismatch ⇒ stale, PID alone never matches (P4) |
| P-F5 | integration | SIGSTOPped holder is **not** stolen; TTL alone insufficient (P2) |
| P-F6 | unit | `SQLITE_BUSY` retries with backoff; no force-write |
| P-F7 | unit | commit failure leaves no visible event, no publish (P5) |
| P-F8 | unit | `foreign_key_check`/`integrity_check` failure ⇒ `CorruptionError` |
| P-F9 | unit | undecodable payload fails loud, never skipped (01 S3) |
| P-F10 | unit | newer `user_version` refuses to open (P13) |
| P-F11 | unit | interrupted migration leaves old version, open fails loud |
| P-F12 | unit | corrupt/stale snapshot discarded and recomputed (P11, 01 S12) |
| P-F13 | unit | parent with children cannot be erased (P14) |
| P-F14 | unit | missing/unwritable lock file ⇒ `StoreOpenError` |
| P-F15 | unit | payload over cap rejected before commit (01 S10) |
| P-F16 | unit | duplicate `event_id` / non-monotonic sequence ⇒ `CorruptionError` (01 S1/S2) |

### 12.7 Invariant coverage

Every invariant P1–P18 maps to at least one test, or is marked
covered-by-construction (CBC) with the reason it cannot be violated.

| Invariant | Test layer | Coverage |
|---|---|---|
| P1 single writer per session | unit + integration | PK + `LeaseLost` on a second holder (P-F1) |
| P2 flock is steal authority | integration | SIGSTOP test: no steal without the flock (P-F5) |
| P3 flock-primary liveness | unit | `kill` hint cannot prove death; ESRCH only (P-F3/P-F4) |
| P4 boot-nonce identity | unit | renew/release match the exact `(pid,boot_id)` |
| P5 durable-before-observable | unit | commit-flag: no visible row/return before `COMMIT` |
| P6 store-only gap-tolerant sequences | unit | strictly increasing, gaps asserted (P-F16) |
| P7 append-only physical log | unit | no mutating API; `erase` is the only delete (P14) |
| P8 foreign keys enforced | unit | `foreign_key_check` empty; orphan blocked |
| P9 WAL + busy timeout | unit + integration | pragma assertions; concurrent reader test |
| P10 fork is a logical prefix | unit + integration | resolved view == parent prefix ++ own (P10, 01 I10) |
| P11 snapshot derived/discardable | unit + replay | stale ⇒ recompute, never patch (P-F12) |
| P12 crash consistency | integration | kill+reopen yields committed-only rows |
| P13 explicit schema version | unit | version gate + transactional migration (P-F10/P-F11) |
| P14 erase atomic/referential | unit | one transaction; `DependentSessionError` (P-F13) |
| P15 lease check in-txn before COMMIT | unit | pre-`COMMIT` failure ⇒ `ROLLBACK` (P-F2) |
| P16 rooted identity | unit + integration | Fake-FS rooted paths, no `getcwd` (F1) |
| P17 transactional `updated_at` | unit | crash between insert/update impossible (P5) |
| P18 reads never write | CBC + unit | `const` read API; replay acquires no lease (01 I19) |

---

## 13. Decisions and open questions

### 13.1 Decisions (pinned by this spec)

- **(a) The sidecar flock is the steal authority.** TTL expiry never authorizes a
  steal; the flock holder is the only process that may steal. This is the
  split-brain/SIGSTOP guard §9.7 demands (§5.3–§5.4, P2/P3).
- **(b) Fork is a logical shared prefix with no copied rows.** `seed_length` plus
  the append-only parent log resolves the view recursively; no `session_forks`
  table or boundary column is added (§4.5, P10, 01 §16.1(d)).
- **(c) The log is committed per `append`/`appendBatch`; there is no deferred
  flush.** `AssistantChunk` coalescing is a producer concern bounded here by
  `max_chunk_batch` / `chunk_flush_interval` / non-chunk barrier (§6.1–§6.2,
  01 §16.1(c)).
- **(d) One derived `session_snapshots` table lives in `sessions.db`.** It is a
  same-DB, discardable materialization of `events`; §9.2 is amended by errata to
  "three source-of-truth concerns plus a derived, discardable snapshot cache"
  (00-architecture §9.2; §3.5, §6.4). A sidecar `snapshots.db` or per-session
  snapshot files are rejected: they would reintroduce a cross-file consistency
  problem.
- **(e) `synchronous=NORMAL` under WAL is the pinned default**, with
  `synchronous=FULL` as the documented override. NORMAL survives an
  application/process crash; an OS crash or power loss may lose recent commits
  unless `FULL` is set (§3.2, §6.1).
- **(f) Recovery is lazy and marker-free.** No dirty flag or clean-shutdown
  marker; SQLite WAL atomicity is the recovery contract, and stale leases are
  cleaned lazily under the flock guard (§7.1, §7.5, P12).
- **(g) `user_version` is the schema gate; migrations are transactional and
  additive-first and never rewrite the log** (§3.4, P13).
- **(h) `erase` refuses to orphan fork/subagent children** rather than
  cascading durable logs (§4.7, P14, P-F13).
- **(i) Corrupt-DB recovery tooling is deferred to v2.** v1 fails loud
  (`CorruptionError`), refuses writes, and preserves the DB for inspection; no
  `ymh doctor`/export path ships in v1 (§7.7).
- **(j) Read-only open is flock-free.** `openReadOnly` uses
  `SQLITE_OPEN_READONLY`, takes no sidecar flock, and runs no migration or
  recovery writes, so any number of WAL readers coexist with the single writer
  (§4.3, §6.3, §8).

### 13.2 Open questions

None. The three questions previously listed here — snapshot storage, power-loss
durability, and corrupt-DB recovery tooling — are resolved and recorded as
decisions (d)/(e)/(i) above.

---

## 14. References

- `00-architecture.md` §4.2 (event stream is the runtime spine), §4.3 (services
  vs. events), §8.1–§8.3 (durable vs. live events, `EventBus`), §9.1 (session as
  event log), §9.2 (SQLite schema + required pragmas), §9.3–§9.5 (resume/fork/
  replay), §9.6 (supervisor/daemon topology), §9.7 (isolation, path safety, write
  lease), §9.8 (session lifecycle), §9.9 (background execution, F10), §9.10
  (shared registry, host liveness, bootstrap), §9.11 (resource limits), §18
  (execution environment), §20.22 (`SessionEnvelope`), §20.23–§20.25 (attention/
  switcher/subagent duality), §34 (cancellation), §35 (concurrency model), §40
  (logging), §43 (RPC transport), §44 (testing strategy), §45 (Fake LLM), §48
  (dependencies, SQLite), §49 (source tree), §54 (D1–D23, F1–F12), §55 (dsh
  comparison), §57 Steps 2/13, §58 (milestones).
- `01-session.md` §2.1 (identities), §3 (`SessionHeader`), §4.4–§4.6
  (`SessionEventMap`, `EventRecord`/`EventRange`), §6.1–§6.4 (append protocol,
  read side, `deriveMessages`, snapshot), §7 (`SessionStore`/`SessionPersistence`/
  `SessionHandle` seams), §8 (`SessionManager`), §9 (lifecycle), §10 (write lease
  and path safety), §11 (concurrency), §12 (I1–I23), §13.2 (S1–S14), §14 (dsh
  mapping), §15 (test plan), §16.1 (pinned decisions).
- `HANDOFF.md` §2 (the rule), §5 item 3 (`SessionHeader` finalization), §6 row 02
  (component plan), §7 (definition of verified).
- `DESIGN_STATUS.md` (written/verified tracker).
