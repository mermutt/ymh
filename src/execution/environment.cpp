#include "ymh/execution/environment.hpp"

#include <atomic>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

#include "ymh/execution/errors.hpp"

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

SpawnConfinement::SpawnConfinement(const std::filesystem::path& workspace_root) {
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

    policy_.root    = workspace_root;
    policy_.home    = home_;
    policy_.tmpdir  = tmp_;
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
