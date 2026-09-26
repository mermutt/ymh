#pragma once

// libgit2-backed `GitService` (00 §29, 07 §6.6). Read-only: `status` and `diff`
// are implemented; the mutating / Phase-2 operations (`log`, `show`, `branch`,
// `checkout`) fail loud rather than pretend, so no git write can ever be issued
// through this seam.
//
// The repository base path arrives already `resolve()`d by the caller (the git
// tools route every path through `ExecutionEnvironment::resolve()`), so this
// class performs no path resolution of its own and never consults `getcwd()`.

#include <filesystem>

#include "ymh/execution/services.hpp"

namespace ymh {

class LibGit2GitService final : public GitService {
public:
    LibGit2GitService() = default;

    Task<GitStatus> status(const GitQuery&) override;
    Task<GitDiff>   diff(const GitQuery&) override;
    Task<GitLog>    log(const GitQuery&) override;
    Task<GitShow>   show(const GitQuery&) override;
    Task<GitBranch> branch(const GitQuery&) override;
    Task<void>      checkout(const GitQuery&) override;
};

} // namespace ymh
