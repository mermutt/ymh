#pragma once

// Multi-workspace supervisor TUI (docs/design/10-supervisor-tui.md §2.3, §3.3;
// docs/design/11-m2-errata.md §10.2). One `SupervisorConnection` per attached
// workspace; the FTXUI loop only reads `UiModel` and never blocks on a daemon.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ymh/config/config.hpp"
#include "ymh/core/clock.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/ui/ui_event.hpp"

namespace ymh::ui {

struct SupervisorWorkspace {
    WorkspaceId id;
    std::string cwd;          // display only (E17)
    std::string title;        // display_title
    std::string socket_path;  // daemon socket
    std::string boot_id;      // registry host->bootId (identity cross-check, D20.7)
};

struct SupervisorRunOptions {
    std::vector<SupervisorWorkspace> workspaces;
    std::filesystem::path            initial_workspace;  // empty = first
    Config                           config;
    bool                             verbose = false;

    // 45-D7: the build version, threaded from the CLI (`YMH_VERSION` is not
    // visible to `ymh_ui`). Empty means "unknown".
    std::string version;

    bool                      no_prompt{false};       // --yes; skip the exit prompt
    std::chrono::milliseconds scan_interval{2'000};   // 16 §3.2 daemon-set scan

    // 16 §4.1 C-L5 / §4.3: the exit path's bounded `host.ownership` probe and
    // the per-daemon `host.shutdown` teardown grace. Tests shrink both.
    std::chrono::milliseconds ownership_query_timeout{2'000};
    std::chrono::milliseconds teardown_grace{5'000};

    // C-H4 spawn seam (borrowed, non-owning). Both MUST outlive the entire
    // `run_supervisor(options)` call (R-L3): the caller (`run_supervisor_entry`)
    // owns them for the whole TUI session. Null leaves the workspace NotRunning.
    HostLifecycle*     lifecycle{nullptr};
    WorkspaceRegistry* registry{nullptr};

    // This supervisor's pinned identity (16-D8) and clock seam (16-D10).
    AttachIdentity     identity;   // {pinned id, ClientRole::Supervisor}
    MonotonicClock     monotonic_clock{default_monotonic_clock()};
    WallClockReader    wall_clock{default_wall_clock()};

    // 22 §4.2 (S2): the `/sessions` catalog rebuild cadence. Tests shrink it.
    std::chrono::milliseconds catalog_refresh_interval{15'000};

    // 22 §5.1 (S3): documentation of the lazy-spawn bound. The actual deadline
    // lives in `HostLifecycle::spawnAndAttach` (~10 s + a ~5 s winner window);
    // the UI worker adds no timeout of its own. Tests shrink it.
    std::chrono::milliseconds ensure_running_timeout{12'000};

    // 22 §6.1 (S4): the session to resume once the workspace connection reaches
    // `Attached`. Seeded into `pending_resume_` by `run()` (shared code path
    // with S3's lazy spawn), or delivered through `resume_from_history`.
    std::optional<std::pair<WorkspaceId, SessionId>> initial_resume;

    // 53-D1/53-D3: the eager-spawn failure notice (53-F1); empty means none.
    std::optional<std::string> initial_notice;
};

int run_supervisor(const SupervisorRunOptions& options);

} // namespace ymh::ui
