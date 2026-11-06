#include "ymh/execution/environment.hpp"

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

#include "ymh/execution/errors.hpp"
#include "ymh/execution/path_safety.hpp"

namespace ymh {
namespace {

std::filesystem::path weakly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : result;
}

bool component_prefix(const std::filesystem::path& base,
                      const std::filesystem::path& candidate) {
    auto base_it = base.begin();
    auto candidate_it = candidate.begin();
    for (; base_it != base.end(); ++base_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *base_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

// 118-D1: link one real global git file into the throwaway home. The link is
// read-only in effect: Landlock grants RW only on the workspace root and the
// throwaway `home`/`tmpdir`, so a write that resolves through the link is denied
// and the real file is never touched. A missing target is skipped; an existing
// entry is never replaced.
void link_git_global(const std::filesystem::path& target,
                     const std::filesystem::path& link) {
    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        return;
    }
    ec.clear();
    if (std::filesystem::exists(link, ec) || std::filesystem::is_symlink(link, ec)) {
        return;
    }
    ec.clear();
    std::filesystem::create_directories(link.parent_path(), ec);
    ec.clear();
    std::filesystem::create_symlink(target, link, ec);
}

// 118-D1: the parent's git global config directory. Git resolves the XDG files
// against `$XDG_CONFIG_HOME` when set, otherwise `$HOME/.config`. The child has
// neither variable (the spawn env is minimal), so both map onto the throwaway
// home's `.config/git`.
std::filesystem::path git_xdg_dir(const std::filesystem::path& real_home) {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        const std::filesystem::path candidate{xdg};
        if (candidate.is_absolute()) {
            return candidate / "git";
        }
    }
    return real_home / ".config" / "git";
}

// 118-D1: the files git reads from `$HOME` before it reports `user.name` /
// `user.email` as unset. `.gitconfig` (legacy) plus the XDG trio; the last one
// wins in git's own precedence order, which the links preserve untouched.
void link_git_globals(const std::filesystem::path& real_home,
                      const std::filesystem::path& spawn_home) {
    link_git_global(real_home / ".gitconfig", spawn_home / ".gitconfig");
    const std::filesystem::path xdg = git_xdg_dir(real_home);
    for (std::string_view name : {"config", "attributes", "ignore"}) {
        link_git_global(xdg / name, spawn_home / ".config" / "git" / name);
    }
}

} // namespace

bool path_is_within(const std::filesystem::path& candidate,
                    const std::filesystem::path& base) {
    return component_prefix(weakly(base), weakly(candidate));
}

LocalEnvironment::LocalEnvironment(std::filesystem::path root,
                                   SandboxMode mode,
                                   ToolConfig config,
                                   PtyService* pty)
    : root_(std::move(root)),
      mode_(mode),
      confinement_status_(kernel_confinement().probe()),
      fs_(std::filesystem::path{}, config),
      process_(config.terminate_grace),
      pty_(pty != nullptr ? pty : &pty_fallback_),
      config_(config) {
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec) ||
        !std::filesystem::is_directory(root_, ec)) {
        throw std::invalid_argument("workspace root is not a directory: " +
                                    root_.string());
    }
    root_ = std::filesystem::canonical(root_, ec);
    if (ec) {
        throw std::invalid_argument("workspace root cannot be canonicalized: " +
                                    root_.string());
    }
    fs_ = LocalFilesystem(root_, config);
}

std::filesystem::path LocalEnvironment::resolve(std::string_view path) const {
    if (path.empty()) {
        throw ToolError{ToolErrorCode::PathEscape, "empty path"};
    }
    const std::filesystem::path raw{std::string(path)};
    const std::filesystem::path base = root_;
    std::filesystem::path candidate = raw.is_absolute() ? raw : base / raw;

    if (mode_ == SandboxMode::Unrestricted) {
        return candidate.lexically_normal();
    }

    const std::filesystem::path parent = weakly(candidate.parent_path());
    if (!path_is_within(parent, base)) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes workspace root: " + raw.string()};
    }

    std::filesystem::path final = parent / candidate.filename();
    std::error_code ec;
    if (std::filesystem::exists(final, ec)) {
        std::filesystem::path canonical = std::filesystem::canonical(final, ec);
        if (!ec) {
            final = std::move(canonical);
        }
    }
    if (!path_is_within(final, base)) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes workspace root: " + raw.string()};
    }
    return final;
}

SpawnConfinement::SpawnConfinement(const std::filesystem::path& workspace_root,
                                   SandboxMode                  sandbox) {
    static std::atomic<std::uint64_t> counter{0};
    const std::string unique =
        std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1));

    std::error_code ec;
    base_ = std::filesystem::temp_directory_path(ec) / ("ymh-spawn-" + unique);
    if (ec) {
        base_ = workspace_root / ".ymh" / "spawn" / unique;
    }
    home_ = base_ / "home";
    tmp_  = base_ / "tmp";
    std::filesystem::create_directories(home_, ec);
    std::filesystem::create_directories(tmp_, ec);

    // 118-D1: the throwaway home keeps *writes* throwaway, but git must still
    // see the identity the user configured. Without this, `git config user.name`
    // inside a sandboxed shell is empty and a commit has no author.
    if (const char* real_home = std::getenv("HOME");
        real_home != nullptr && *real_home != '\0') {
        link_git_globals(std::filesystem::path{real_home}, home_);
    }

    policy_.root               = workspace_root;
    policy_.home               = home_;
    policy_.tmpdir             = tmp_;
    policy_.workspace_writable = sandbox != SandboxMode::ReadOnly;
    const std::filesystem::path& tmp_root = canonical_tmp_root();
    ec.clear();
    const std::filesystem::path  canonical_root =
        std::filesystem::weakly_canonical(workspace_root, ec);
    const std::filesystem::path  effective_root =
        ec ? workspace_root.lexically_normal() : canonical_root;
    if (tmp_root.empty() || effective_root == tmp_root ||
        path_is_under_tmp_root(tmp_root, effective_root)) {
        policy_.tmp_root = std::nullopt;
    } else {
        policy_.tmp_root = tmp_root;
    }
    policy_.ro_paths = {std::filesystem::path{"/"}};
}

SpawnConfinement::~SpawnConfinement() { cleanup(); }

SpawnConfinement::SpawnConfinement(SpawnConfinement&& other) noexcept
    : policy_(std::move(other.policy_)),
      base_(std::move(other.base_)),
      home_(std::move(other.home_)),
      tmp_(std::move(other.tmp_)) {
    other.base_.clear();
}

SpawnConfinement& SpawnConfinement::operator=(SpawnConfinement&& other) noexcept {
    if (this != &other) {
        cleanup();
        policy_ = std::move(other.policy_);
        base_   = std::move(other.base_);
        home_   = std::move(other.home_);
        tmp_    = std::move(other.tmp_);
        other.base_.clear();
    }
    return *this;
}

void SpawnConfinement::cleanup() noexcept {
    if (base_.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(base_, ec);
    base_.clear();
}

} // namespace ymh
