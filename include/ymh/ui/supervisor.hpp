#pragma once

// Multi-workspace supervisor TUI (docs/design/10-supervisor-tui.md §2.3, §3.3;
// docs/design/11-m2-errata.md §10.2). One `SupervisorConnection` per attached
// workspace; the FTXUI loop only reads `UiModel` and never blocks on a daemon.

#include <filesystem>
#include <string>
#include <vector>

#include "ymh/config/config.hpp"
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
};

int run_supervisor(const SupervisorRunOptions& options);

} // namespace ymh::ui
