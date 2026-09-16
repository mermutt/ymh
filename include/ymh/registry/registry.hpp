#pragma once

// Workspace registry (component 03). Pinned by
// docs/design/03-workspace-registry.md §2–§10 and 00-architecture.md §9.10:
// `WorkspaceRegistry` over the shared `registry.db`, the canonical-path ↔
// `WorkspaceId` index, the ordered session junction (`workspace_sessions`), the
// host (daemon) registration, the `pending_mutation` crash marker, and the
// one-time bootstrap `initialized` marker (`registry_meta`).
//
// The registry is the durable membership/ordering authority for the open set.
// It is deliberately outside the event log (D21) and outside any single daemon:
// session events/leases live in each workspace's `sessions.db` (spec 02), and
// the focused session is supervisor-local (spec 10, R12). It holds no events,
// leases, permissions, or UI state (R1).
//
// Cross-process writer serialization is an exclusive `flock(LOCK_EX)` on
// `registry.lock` held per mutation (D22, §5); readers take no lock and see a
// WAL snapshot (R2, R15).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/registry/supervisor.hpp"

namespace ymh {

// ---------------------------------------------------------------------------
// Identifiers and value types (§2.1)
// ---------------------------------------------------------------------------

// UUIDv4; stable; never a path (§9.7). Distinct from 01's `SessionId`.
struct WorkspaceId {
    std::string value;

    auto operator<=>(const WorkspaceId&) const = default;
};

// UUIDv4 minted once at daemon startup (§9.7, §9.10); a PID-reuse guard.
struct HostBootId {
    std::string value;

    auto operator<=>(const HostBootId&) const = default;
};

// > 0 always; 0 is never stored (§3.3, R3).
using HostPid = std::int32_t;

// The host-registration half of a `workspaces` row (§6.1).
struct HostClaim {
    WorkspaceId           workspace;
    HostPid               pid = 0;  // > 0 (R3)
    HostBootId            bootId;
    std::filesystem::path socketPath;  // Unix socket for IPC (§9.6, spec 05)
};

struct WorkspaceRecord {
    WorkspaceId                 id;             // UUIDv4; never the path
    std::filesystem::path       canonicalPath;  // realpath (DIV-8, §9.7)
    std::string                 displayTitle;
    std::int64_t                createdAt = 0;  // epoch ms
    std::int64_t                updatedAt = 0;  // epoch ms
    std::optional<HostClaim>    host;           // nullopt ⇔ host_pid IS NULL
    std::optional<std::int64_t> heartbeatAt;    // epoch ms; nullopt ⇔ never
    std::optional<std::string>  metadata;       // opaque JSON; no schema commitment
};

struct WorkspaceSessionRecord {
    WorkspaceId  workspace;
    SessionId    sessionId;
    std::int64_t ordinal = 0;   // ordering key; strictly increasing, NOT dense (R8)
    bool         archived = false;
    std::int64_t createdAt = 0; // epoch ms
};

// 11-m2-errata §5.3 (D16): a store session reduced to the fields the
// `session.list` join needs.
struct SessionRef {
    SessionId    sessionId;
    std::int64_t createdAt = 0;  // epoch ms
};

// 11-m2-errata §5.3 (D16): one entry in the daemon's `session.list` order.
// `inJunction == false` marks a store-only session (the crash window between
// the store commit and the junction write) that is surfaced after the
// junction set and flagged for reconcile.
struct SessionOrderEntry {
    SessionId    sessionId;
    std::int64_t ordinal    = 0;
    bool         archived   = false;
    bool         inJunction = true;
};

enum class MutationType : std::uint8_t { Create, Delete, Reorder };

[[nodiscard]] std::string_view mutation_type_name(MutationType type) noexcept;
[[nodiscard]] std::optional<MutationType> parse_mutation_type(std::string_view name) noexcept;

struct PendingMutation {
    WorkspaceId  workspace;  // PK: at most one marker per workspace (§8)
    MutationType type = MutationType::Create;
    std::string  payload;    // opaque JSON; schema pinned in §8.2
    std::int64_t timestamp = 0;  // epoch ms
};

// Host liveness as observed by the sidecar-flock probe (§6.3, R5). The probe
// never mutates; a read-only registry reports `Stale` and leaves clearing to a
// writer via `reapHost` (§6.4).
enum class HostLiveness : std::uint8_t {
    Absent,  // no claim row
    Live,    // claim row and the sidecar `sessions.lock` is held
    Stale,   // claim row but the sidecar lock is absent
};

// ---------------------------------------------------------------------------
// Error taxonomy (§2.2)
// ---------------------------------------------------------------------------

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
    MutationInProgress,  // a pending_mutation marker already exists
    NotWriteLockHolder,  // a mutation attempted without the D22 lock
};

class RegistryError final : public std::runtime_error {
public:
    RegistryError(RegistryErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] RegistryErrorCode code() const noexcept { return code_; }

private:
    RegistryErrorCode code_;
};

// ---------------------------------------------------------------------------
// Configuration (§4.1)
// ---------------------------------------------------------------------------

struct RegistryConfig {
    std::filesystem::path db_path;    // <state>/ymh/registry.db
    std::filesystem::path lock_path;  // <state>/ymh/registry.lock

    // Writer serialization (§5).
    std::chrono::milliseconds busy_timeout{5'000};
    std::chrono::milliseconds lock_retry_budget{2'000};  // total wait for flock(LOCK_EX)
    std::chrono::milliseconds lock_retry_interval{25};

    // Host heartbeat (§6.2). Staleness is a HINT only; it never clears a claim.
    std::chrono::milliseconds heartbeat_interval{5'000};
    std::chrono::milliseconds heartbeat_stale_hint{15'000};  // 3 missed ticks

    // One-time bootstrap (§7.1). Consumed from the resolved §37 config.
    std::vector<std::filesystem::path> workspace_roots;  // default {"$HOME/prjs"}
    int                                bootstrap_depth{4};
    bool                               allow_process_scan_fallback{true};
};

inline constexpr int          kRegistrySchemaVersion = 2;
inline constexpr std::int32_t kRegistryApplicationId = 0x594D4802;  // 'Y','M','H', family 2
inline constexpr std::string_view kInitializedMarker  = "initialized";
inline constexpr std::string_view kLegacyMigratedMarker = "legacy_migrated";

// ---------------------------------------------------------------------------
// State-directory scaffolding (§3.1)
// ---------------------------------------------------------------------------

// `${XDG_STATE_HOME:-~/.local/state}/ymh`, never derived from getcwd (§9.7).
[[nodiscard]] std::filesystem::path default_state_dir();
[[nodiscard]] std::filesystem::path default_registry_db_path();
[[nodiscard]] std::filesystem::path default_registry_lock_path();

// Creates `dir` (and parents) with mode 0700 if absent.
void ensure_state_dir(const std::filesystem::path& dir);

// RegistryConfig wired to the default state dir with the pinned defaults.
[[nodiscard]] RegistryConfig default_registry_config();

// ---------------------------------------------------------------------------
// WorkspaceRegistry (§4.2)
// ---------------------------------------------------------------------------

class WorkspaceRegistry {
public:
    // Writer open: creates the DB/dir if absent, applies §3.2 pragmas, runs the
    // §3.4 version gate, then under the D22 write lock runs pending-mutation
    // resolution (§8) and, if the `initialized` marker is absent, bootstrap (§7).
    static std::unique_ptr<WorkspaceRegistry> open(const RegistryConfig& config);

    // Read-only open: SQLITE_OPEN_READONLY, no create, no flock, no migration,
    // no recovery write, no bootstrap. Coexists with the writer via WAL.
    static std::unique_ptr<WorkspaceRegistry> openReadOnly(const RegistryConfig& config);

    ~WorkspaceRegistry();

    WorkspaceRegistry(const WorkspaceRegistry&) = delete;
    WorkspaceRegistry& operator=(const WorkspaceRegistry&) = delete;

    // ---- reads (no flock; WAL snapshot) ------------------------------------
    [[nodiscard]] bool                                  isInitialized() const;
    [[nodiscard]] std::vector<WorkspaceRecord>          listWorkspaces() const;  // ORDER BY canonical_path
    [[nodiscard]] std::optional<WorkspaceRecord>        findById(WorkspaceId) const;
    [[nodiscard]] std::optional<WorkspaceRecord>        findByCanonicalPath(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::vector<WorkspaceSessionRecord>   listSessions(WorkspaceId) const;  // ORDER BY ordinal
    [[nodiscard]] std::optional<WorkspaceSessionRecord> findSession(WorkspaceId, SessionId) const;

    // 11-m2-errata §5.3 (D16): the `session.list` order — junction rows by
    // ordinal, then store-only sessions by `(createdAt, sessionId)`. The
    // Wave-2 `HostRuntime` calls this with the store's session refs.
    [[nodiscard]] std::vector<SessionOrderEntry> listSessionsOrdered(
        WorkspaceId workspace, const std::vector<SessionRef>& storeSessions) const;
    [[nodiscard]] std::optional<PendingMutation>        pendingMutation(WorkspaceId) const;

    // Number of rows with a host claim (supports §9.11's live-daemon cap; F8).
    [[nodiscard]] std::size_t liveHostCount() const;

    // Sidecar-flock liveness observation (§6.3, §6.4). Read-only; never clears.
    [[nodiscard]] HostLiveness probeLiveness(WorkspaceId) const;

    // ---- supervisor ownership set (16 §7.1) --------------------------------
    // All rows, `started_at` then `id` ascending. Returns `{}` when the
    // `supervisors` table is absent (read-only open of an un-migrated v1 DB).
    [[nodiscard]] std::vector<SupervisorRow> listSupervisors() const;

    // Fresh rows by `isFresh` (negative delta is stale), optionally excluding
    // one id. Returns 0 when the `supervisors` table is absent.
    [[nodiscard]] std::size_t freshSupervisorCount(
        std::int64_t now_wall_ms, std::chrono::milliseconds ttl,
        const std::optional<SupervisorId>& exclude = std::nullopt) const;

    // Convenience wrapper for non-daemon callers. False when the table is absent.
    [[nodiscard]] bool hasFreshSupervisor(std::int64_t now_wall_ms,
                                          std::chrono::milliseconds ttl) const;

    // ---- mutations (require the D22 write lock; §5) ------------------------
    WorkspaceRecord registerWorkspace(const std::filesystem::path& canonicalPath,
                                      std::string displayTitle,
                                      std::optional<std::string> metadata = std::nullopt);
    void            removeWorkspace(WorkspaceId);  // refuses if non-empty/hosted
    void            setDisplayTitle(WorkspaceId, std::string);

    // Host claim/heartbeat — the supervisor's ONLY write set (§9.10, R16).
    void            claimHost(HostClaim);
    void            heartbeat(HostClaim);  // no-op if the claim no longer matches
    void            releaseHost(WorkspaceId, HostBootId);

    // Supervisor ownership set (16 §7.1). Upsert by `id`; a new `bootId`
    // supersedes a recycled-pid predecessor. Heartbeat returns false when the
    // row is absent (pruned by a peer), so the caller re-registers (§2.6/C-M8).
    void registerSupervisor(const SupervisorRow& row);
    [[nodiscard]] bool heartbeatSupervisor(const SupervisorId& id,
                                           std::int64_t now_wall_ms);
    [[nodiscard]] bool deregisterSupervisor(const SupervisorId& id);  // false if absent

    // Deletes rows with `delta > ttl || delta < 0` (delta as in isFresh).
    // Returns the number deleted. Supervisor-only (16 §5.4/A9).
    std::size_t pruneStaleSupervisors(std::int64_t now_wall_ms,
                                      std::chrono::milliseconds ttl);

    // Clears a stale claim under the write lock, gated on lock-absence (R5).
    // Returns true iff a claim row was cleared. Never kills a process (R11).
    bool            reapHost(WorkspaceId);

    // Open-set junction — canonical writer is the workspace's daemon (R16).
    WorkspaceSessionRecord addSession(WorkspaceId, SessionId);
    void                   removeSession(WorkspaceId, SessionId);
    void                   archiveSession(WorkspaceId, SessionId, bool archived);
    void                   reorderSession(WorkspaceId, SessionId, std::int64_t newOrdinal);

    // ---- maintenance / introspection --------------------------------------
    [[nodiscard]] bool holdsWriteLock() const;  // did THIS instance acquire the flock?
    void               resolvePendingMutations();  // idempotent; also run by open()

    // One-time discovery walk over `config.workspace_roots` (§7.1); idempotent,
    // writes `registry_meta['initialized']` last.
    void bootstrap();

    // Imports a workspace's `sessions.db` rows newest-first (§7.1/§7.2).
    // Idempotent (junction PRIMARY KEY upserts); returns rows newly inserted.
    std::size_t importSessionsFromDisk(WorkspaceId, const std::filesystem::path& workspaceRoot);

    // Raw `registry_meta` access for diagnostics/tests (bookkeeping only).
    [[nodiscard]] std::optional<std::string> metaValue(std::string_view key) const;

private:
    struct Impl;
    explicit WorkspaceRegistry(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Free helpers shared with the process-scan fallback and tests
// ---------------------------------------------------------------------------

// Generates a UUIDv4 string. Exposed for deterministic id minting in callers.
[[nodiscard]] std::string generate_uuid_v4();

// 11-m2-errata §5.3 (D16): the daemon's `session.list` order. Junction rows
// come first in `ordinal` ascending order; store-only sessions follow, sorted
// by `(createdAt, sessionId)`, flagged `inJunction == false`. Never lexical id
// and never the store's `createdAt` for the junction set (R8).
[[nodiscard]] std::vector<SessionOrderEntry> orderSessions(
    const std::vector<WorkspaceSessionRecord>& junction,
    const std::vector<SessionRef>&             storeSessions);

} // namespace ymh

namespace std {

template <>
struct hash<ymh::WorkspaceId> {
    [[nodiscard]] std::size_t operator()(const ymh::WorkspaceId& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};

template <>
struct hash<ymh::HostBootId> {
    [[nodiscard]] std::size_t operator()(const ymh::HostBootId& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};

} // namespace std
