#pragma once

// Multi-workspace supervisor TUI (docs/design/10-supervisor-tui.md §2.3, §3.3;
// docs/design/11-m2-errata.md §10.2). One `SupervisorConnection` per attached
// workspace; the FTXUI loop only reads `UiModel` and never blocks on a daemon.

#include <chrono>
#include <filesystem>
#include <string>
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

    bool                      no_prompt{false};       // --yes; skip the exit prompt
    std::chrono::milliseconds scan_interval{2'000};   // 16 §3.2 daemon-set scan

    // C-H4 spawn seam (borrowed, non-owning). Both MUST outlive the entire
    // `run_supervisor(options)` call (R-L3): the caller (`run_supervisor_entry`)
    // owns them for the whole TUI session. Null leaves the workspace NotRunning.
    HostLifecycle*     lifecycle{nullptr};
    WorkspaceRegistry* registry{nullptr};

    // This supervisor's pinned identity (16-D8) and clock seam (16-D10).
    AttachIdentity     identity;   // {pinned id, ClientRole::Supervisor}
    MonotonicClock     monotonic_clock{default_monotonic_clock()};
    WallClockReader    wall_clock{default_wall_clock()};
};

int run_supervisor(const SupervisorRunOptions& options);

} // namespace ymh::ui
