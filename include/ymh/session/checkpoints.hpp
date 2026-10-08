#pragma once

// File checkpoint subsystem (spec 80). Snapshots the PRE-image of files that
// `write_file`/`edit_file` mutate, keyed by `(session, turn)`, under
// `<ws>/.ymh/checkpoints/` (content-addressed blobs + a JSON index). Restore is
// root-confined through `ExecutionEnvironment::resolve()` and symlink-refusing.
// The store is owned by the workspace daemon and never adds a row to the
// session DB (80-D3/80-D12).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/session/ids.hpp"
#include "ymh/core/event.hpp"

namespace ymh {

class ExecutionEnvironment;

// 80-D1: one captured file's PRE-state within a checkpoint.
enum class PreImageKind : std::uint8_t {
    Content,
    Absent,
    Symlink,
    Failed,
};

struct CheckpointFile {
    std::string  path;
    PreImageKind kind = PreImageKind::Content;
    std::string  blob;
    std::int64_t size{0};
    std::int64_t mtime_ms{0};
};

struct Checkpoint {
    SessionId                   session;
    TurnId                      turn{0};
    std::int64_t                created_at_ms{0};
    bool                        incomplete{false};
    std::vector<CheckpointFile> files;
};

// Per-file outcome of a restore (80-D6). This is the session-layer domain type;
// the wire DTO `protocol::RestoreReport` is distinct and bridged by HostRuntime.
struct CheckpointRestoreReport {
    std::int64_t restored{0};
    std::int64_t skipped{0};
    std::int64_t failed{0};
    std::int64_t changed{0};
    bool         expired{false};
    std::string  detail;
};

// NEW-M3: the store's index I/O error (CP-F14). The host maps it to
// `protocol::AppCode::CheckpointUnavailable`.
struct CheckpointUnavailableError : std::runtime_error {
    explicit CheckpointUnavailableError(std::string what)
        : std::runtime_error(std::move(what)) {}
};

// 80-D2: implemented by the daemon and injected into the ToolRegistry. The
// ToolContext supplies session/turn/environment, so the recorder needs none.
class CheckpointRecorder {
public:
    virtual ~CheckpointRecorder() = default;

    // Captures the PRE-state of `paths` before the tool mutates them. Idempotent
    // per (session, turn, path): the first capture wins (80-D1). Never throws.
    virtual void capture(const SessionId& session, TurnId turn,
                         const std::vector<std::filesystem::path>& paths,
                         const ExecutionEnvironment& env) = 0;
};

// 80-D3: blob id = lowercase-hex sha256 of the raw bytes.
[[nodiscard]] std::string checkpoint_blob_id(std::string_view bytes);

// 80-D5: the first symlink component of the UNRESOLVED `candidate`, or a leaf
// whose hard_link_count() > 1, or nullopt. Called before resolve().
[[nodiscard]] std::optional<std::filesystem::path>
first_symlink_component(const std::filesystem::path& candidate);

// 80-D1: `path`'s workspace-relative form, used as the manifest key.
[[nodiscard]] std::string workspace_relative(const std::filesystem::path& workspace_root,
                                             const std::filesystem::path& path);

class CheckpointStore final : public CheckpointRecorder {
public:
    // `workspace_root` is the canonical workspace root. `hash` defaults to
    // `checkpoint_blob_id` (injected for tests).
    explicit CheckpointStore(std::filesystem::path workspace_root,
                             std::function<std::string(std::string_view)> hash = {});

    void capture(const SessionId&, TurnId,
                 const std::vector<std::filesystem::path>&,
                 const ExecutionEnvironment&) override;

    // 80-D7/80-D8: distinct retained paths capturable at turns >= `turn`; 0 when
    // `turn` is older than the retained window or nothing is retained. A READ
    // projection: an index I/O error degrades to 0 (NEW-M5).
    [[nodiscard]] std::int64_t revertible_count(const SessionId&, TurnId) const;

    // 80-D5/D6/D7: root-confined, symlink-refusing restore. Never throws for a
    // per-file problem. Throws `CheckpointUnavailableError` only for an index
    // I/O error; a corrupt/unknown-version index loads as empty (CP-F5).
    CheckpointRestoreReport restore(ExecutionEnvironment&, const SessionId&, TurnId);

    // 80-D7/80-D12: drop a session's manifests and GC its blobs.
    void removeSession(const SessionId&);

    // 80-D7: count/age eviction + reconcile against existing sessions + blob GC.
    // A maintenance pass run at daemon startup; NEVER throws (NEW-M6).
    void sweep(const std::set<SessionId>& live_sessions,
               std::chrono::system_clock::time_point now);

private:
    void ensure_loaded_locked() const;
    [[nodiscard]] TurnId oldest_retained_turn_locked(const std::string& session) const;
    [[nodiscard]] std::optional<Checkpoint> find_locked(const std::string& session,
                                                        TurnId turn) const;
    void evict_locked(const std::string& session,
                      std::chrono::system_clock::time_point now);
    void persist_locked();
    void collect_garbage_locked();

    std::filesystem::path                        workspace_root_;
    std::filesystem::path                        store_dir_;
    std::function<std::string(std::string_view)> hash_;
    mutable std::mutex                           mutex_;
    mutable std::map<std::string, std::vector<Checkpoint>> sessions_;
    mutable bool                                 loaded_ = false;
    mutable bool                                 dirty_ = false;
};

} // namespace ymh
