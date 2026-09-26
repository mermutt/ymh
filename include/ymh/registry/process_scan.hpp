#pragma once

// Best-effort process-scan fallback for the workspace registry (03 §6.5,
// R10/R11, §7.4). It is attempted ONLY when the durable index is unavailable
// (missing / corrupt / pre-`initialized`). `argv` is used solely to seed
// candidate PIDs; every path is derived independently from the registry row or
// from the process's cwd, never from the scanned `argv`. A scan never kills.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ymh/registry/registry.hpp"

namespace ymh {

struct ProcessEntry {
    std::int32_t             pid = 0;
    std::vector<std::string> argv;  // argv[0], argv[1], ...
    std::filesystem::path    cwd;   // resolved via /proc/<pid>/cwd (may be empty)
};

struct ScannedHost {
    std::int32_t          pid = 0;
    WorkspaceId           workspace;
    std::filesystem::path workspaceRoot;
    std::filesystem::path socketPath;
    bool                  confirmed = false;
};

// Confirmation of a candidate: an authenticated JSON-RPC handshake (spec 05) or
// a sidecar-flock probe (§6.3). The scan never trusts `argv` for this.
using HostConfirmer =
    std::function<bool(const std::filesystem::path& workspaceRoot,
                       const std::filesystem::path& socketPath)>;

// Pure, testable core. Seeds candidates from `table` where the executable
// basename of `argv[0]` is exactly "ymh" and the argv contains the host flag
// triple `--host --workspace <uuid> --socket <path>`. For each candidate the
// workspace root comes from `lookup` (the registry) when available, otherwise
// from the independently-resolved `ProcessEntry::cwd`; the socket path comes
// from the registry row or the `<root>/.ymh/host.sock` convention. Only
// candidates confirmed by `confirm` are returned.
[[nodiscard]] std::vector<ScannedHost> scanHosts(
    std::span<const ProcessEntry> table,
    const std::function<std::optional<WorkspaceRecord>(const WorkspaceId&)>& lookup,
    const HostConfirmer& confirm);

// Real enumeration: `/proc/<pid>/cmdline` and `/proc/<pid>/cwd`.
[[nodiscard]] std::vector<ProcessEntry> readProcessTable();

// `true` iff the argv is a `ymh` host invocation carrying the flag triple.
[[nodiscard]] bool is_ymh_host_process(const std::vector<std::string>& argv);

} // namespace ymh
