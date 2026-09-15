#pragma once

// FTXUI application + single-process wiring (10 §8, §12, §57 Step 11).
//
// Milestone 1: one workspace (the process cwd), the `AgentLoop` runs in-process,
// and the rendered event stream is live. The multi-workspace switcher and daemon
// attach are Milestone 2 and intentionally absent here.

#include <filesystem>

#include "ymh/config/config.hpp"

namespace ymh::ui {

struct UiRunOptions {
    std::filesystem::path workspace;
    Config                config;
    bool                  verbose = false;
};

// Opens the cwd session, runs the agent loop in-process, and blocks until the
// user quits. Returns a process exit code.
int run_tui(const UiRunOptions& options);

} // namespace ymh::ui
