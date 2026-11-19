#pragma once

// Multi-root workspaces. The workspace starts with one canonical primary root;
// the user may approve additional directories from the permission dialog, after
// which they are treated exactly like the primary root by the execution,
// permission, and sandbox layers.
//
// The registry is mutable and shared: the agent loop snapshots `all()` into each
// `PermissionRequest`, while `LocalEnvironment`/`LocalFilesystem` consult the
// live registry at execution time. Every stored root is canonical
// (weakly_canonical) and an existing directory; containment is always
// component-wise and lexical (never a string-prefix test).

#include <cstddef>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ymh {

// Lexical, component-wise containment of `candidate` in `root` (both must be
// absolute; the root itself is contained).
[[nodiscard]] bool path_is_under_root(const std::filesystem::path& root,
                                      const std::filesystem::path& candidate);

// The root that contains `candidate`, or nullopt. `roots` must be absolute and
// canonical.
[[nodiscard]] std::optional<std::filesystem::path> containing_root(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path&              candidate);

// The path of `candidate` relative to the containing root, or nullopt.
[[nodiscard]] std::optional<std::filesystem::path> relative_to_any_root(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path&              candidate);

// True when any component of `candidate` relative to its containing root is
// `.git`, or `.ymh` unless `allow_ymh`. A candidate outside every root is not
// protected by this predicate (callers reject it separately).
[[nodiscard]] bool path_is_protected_in_roots(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path&              candidate,
    bool                                      allow_ymh = false);

// Validate one additional root: must resolve to an existing directory, must be
// absolute (relative paths resolve against `primary`), must not be `/` or a
// system/home directory, must not be an ancestor of `primary` or of an existing
// root, must not be under the canonical `/tmp` scratch area, and must not be
// already covered by an existing root. Returns the canonical directory or a
// human-readable refusal.
[[nodiscard]] std::expected<std::filesystem::path, std::string>
validate_workspace_root(const std::filesystem::path&              candidate,
                        const std::filesystem::path&              primary,
                        const std::vector<std::filesystem::path>& existing);

// Pick the directory a permission dialog should offer for an outside target:
// the nearest existing directory (the target itself, or its parent for a file)
// then walk up to the nearest directory containing `.git`/`.ymh`, bounded by
// home and the canonical `/tmp`. Returns nullopt when the target is already
// covered or no safe candidate exists.
[[nodiscard]] std::optional<std::filesystem::path> suggest_workspace_root(
    const std::filesystem::path&              target,
    const std::filesystem::path&              primary,
    const std::vector<std::filesystem::path>& existing);

class WorkspaceRoots {
public:
    // `primary` is the canonical workspace root; `additional` are directories
    // previously approved and persisted. Invalid additional entries are dropped
    // (the caller may log them before constructing).
    WorkspaceRoots(std::filesystem::path              primary,
                   std::vector<std::filesystem::path> additional = {});

    WorkspaceRoots(const WorkspaceRoots&)            = delete;
    WorkspaceRoots& operator=(const WorkspaceRoots&) = delete;

    [[nodiscard]] std::filesystem::path primary() const;
    // Primary first, then the additional roots in approval order.
    [[nodiscard]] std::vector<std::filesystem::path> all() const;
    [[nodiscard]] bool contains(const std::filesystem::path& candidate) const;
    [[nodiscard]] std::optional<std::filesystem::path>
    relative(const std::filesystem::path& candidate) const;

    // Validates against the live set and, on success, registers the canonical
    // directory. Returns the canonical path or the refusal reason.
    [[nodiscard]] std::expected<std::filesystem::path, std::string>
    add(const std::filesystem::path& candidate);

    [[nodiscard]] std::expected<std::filesystem::path, std::string>
    validate(const std::filesystem::path& candidate) const;

private:
    mutable std::mutex                mutex_;
    std::filesystem::path             primary_;
    std::vector<std::filesystem::path> additional_;
};

} // namespace ymh
