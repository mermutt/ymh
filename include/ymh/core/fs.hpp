#pragma once

// Shared POSIX filesystem helpers (promoted from the `src/cli/cli.cpp`
// anonymous namespace by 80 sec.6.3 / "Amends" so the checkpoint store can
// reuse the same crash-safe JSON-state discipline). The helper is best-effort:
// an fsync failure is intentionally ignored, exactly as the JSON-state
// precedents do (`src/config/grant_store.cpp`, `src/skills/workspace_trust.cpp`).

#include <filesystem>

namespace ymh {

// fsync(2) the directory that contains `file` (or "." for a bare filename).
// A directory fsync makes a preceding rename durable; a failure is ignored.
void fsync_parent_directory(const std::filesystem::path& file);

} // namespace ymh
