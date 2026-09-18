#pragma once

// Stored-session catalog reader (22-switcher-sessions-errata.md §4, S2).
//
// The `/sessions` surface lists every *stored* session for every *registered*
// workspace, read from disk (`<workspace>/.ymh/sessions.db`) — no daemon is
// required. The reader owns one worker thread and delivers immutable
// `SessionCatalogSnapshot`s to the UI thread through a `Sink`; the UI thread
// never opens SQLite (§4.2, SW7).
//
// This header is deliberately UI-model-free: Wave C injects the reader into
// `SupervisorApp` (registry-backed source + `SupervisorApp::enqueue` sink) and
// stores the delivered snapshot in `UiModel.catalog`. The reader itself only
// needs the workspace source and a sink.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ymh/registry/registry.hpp"
#include "ymh/session/session.hpp"

namespace ymh::ui {

// §4.2: one stored session, projected from `SessionHeader` (the field set the
// History overlay shows). `createdAt`/`updatedAt` are epoch ms.
struct SessionHistoryEntry {
    SessionId                  id;
    std::string                title;
    std::string                kind;  // "root" | "fork" | "subagent"
    std::string                model;
    std::int64_t               createdAt = 0;
    std::int64_t               updatedAt = 0;
    std::optional<SessionId>   parent;
    std::optional<std::size_t> seedLength;
};

// §4.2: one registered workspace's stored sessions plus the per-workspace
// degradation note. `note == std::nullopt` means the read succeeded. `live` is
// supplied by the caller: the read helper has no registry.
struct WorkspaceHistory {
    WorkspaceId                      id;
    std::string                      title;
    std::string                      canonicalPath;
    bool                             live = false;
    std::optional<std::string>       note;
    std::vector<SessionHistoryEntry> sessions;  // updatedAt desc, id asc
};

// §4.2: an immutable catalog snapshot. `workspaces` is in registry storage
// order (canonical_path); the renderer sorts by title (22-D1). `complete` is
// false iff at least one workspace has a `note`.
struct SessionCatalogSnapshot {
    std::vector<WorkspaceHistory> workspaces;
    bool                          complete = false;
    std::int64_t                  capturedAtMs = 0;
    std::uint64_t                 generation = 0;
};

// §4.6: the shared per-workspace read helper. Used by `SessionCatalogReader`
// (worker) and by the CLI `--resume` resolver (S4). It performs the §4.4
// pre-flight classification and the pinned catch order, and NEVER throws: any
// failure becomes a `WorkspaceHistory::note` (SW10).
//
// 23 §6.2 (23-D31): `include_unprompted` defaults to **true**, so the shared
// helper stays unfiltered for the `--resume` resolver (`src/cli/cli.cpp:380`).
// Only the `/sessions` catalog consumer passes `false` to hide unprompted root
// headers; explicit-id surfaces (`--resume`/`show`/`replay`/`fork`) are
// unaffected.
[[nodiscard]] WorkspaceHistory read_workspace_history(const WorkspaceRecord& record, bool live,
                                                      bool include_unprompted = true);

// The reader's data source. Wave C uses the registry-backed source built by
// `registry_catalog_source`; tests inject a fake for deterministic cadence and
// failure injection. This is the injection seam the task requires.
struct WorkspaceCatalogSource {
    std::function<std::vector<WorkspaceRecord>()>                 list;
    std::function<bool(const WorkspaceId&)>                       is_live;
    std::function<WorkspaceHistory(const WorkspaceRecord&, bool)> read;
};

// §4.1/§4.5: enumerate every row of `WorkspaceRegistry::listWorkspaces()` and
// read each `<canonical_path>/.ymh/sessions.db` with `openReadOnly`. Reads take
// no `flock` and open no second registry handle.
[[nodiscard]] WorkspaceCatalogSource registry_catalog_source(WorkspaceRegistry& registry);

// §4.2: one worker thread builds the snapshot; the sink runs on that thread and
// must post to the UI thread itself (same idiom as `DaemonSetScanner`).
class SessionCatalogReader {
public:
    using Sink = std::function<void(SessionCatalogSnapshot)>;

    // Production constructor (pinned §4.2): registry-backed source, default
    // `read_workspace_history`.
    SessionCatalogReader(WorkspaceRegistry& registry, Sink sink,
                         std::chrono::milliseconds refresh_interval);
    // Injection seam: explicit source (Wave C may also use this).
    SessionCatalogReader(WorkspaceCatalogSource source, Sink sink,
                         std::chrono::milliseconds refresh_interval);
    ~SessionCatalogReader();

    SessionCatalogReader(const SessionCatalogReader&) = delete;
    SessionCatalogReader& operator=(const SessionCatalogReader&) = delete;

    void start();
    // Joins the worker. Idempotent: a second `stop()` (or a `stop()` after the
    // destructor already ran it) is a no-op (SW26).
    void stop();
    // Request an immediate rebuild (thread-safe). Coalesces: at most one
    // rebuild is in flight; a request during a build is consumed afterwards.
    void refreshNow();

private:
    void loop();
    // One full rebuild. Throws only if the source's `list()` throws (the
    // "Registry read fails" row: keep the last snapshot); the per-workspace
    // read never throws.
    [[nodiscard]] SessionCatalogSnapshot build() const;

    WorkspaceCatalogSource    source_;
    Sink                      sink_;
    std::chrono::milliseconds refresh_interval_;
    std::thread               thread_;
    mutable std::mutex        mutex_;
    std::condition_variable   cv_;
    bool                      running_{false};
    bool                      refresh_pending_{false};
    std::uint64_t             generation_{0};
};

} // namespace ymh::ui
