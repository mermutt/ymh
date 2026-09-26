#pragma once

// Git and LSP service seams (07 §6.5-§6.7). Both are Phase 2 capabilities: the
// canonical `ExecutionEnvironment` (§18) exposes them so no tool changes when
// they land, but v1 registers no git/LSP-backed tool and `lsp()` returns
// nullptr. `Unavailable*` are the v1 local implementations; they fail loud
// rather than pretend. The PTY seam moved to `ymh/execution/pty.hpp` (14 §3/§4)
// and is re-exported here so existing includers keep compiling.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/pty.hpp"

namespace ymh {

struct GitQuery {
    std::string           ref;
    std::filesystem::path path;
    bool                  staged = false;
};

struct GitStatus { std::string text; };
struct GitDiff   { std::string text; };
struct GitLog    { std::string text; };
struct GitShow   { std::string text; };
struct GitBranch { std::string text; };

class GitService {
public:
    virtual ~GitService() = default;

    virtual Task<GitStatus> status(const GitQuery&) = 0;
    virtual Task<GitDiff>   diff(const GitQuery&) = 0;
    virtual Task<GitLog>    log(const GitQuery&) = 0;
    virtual Task<GitShow>   show(const GitQuery&) = 0;
    virtual Task<GitBranch> branch(const GitQuery&) = 0;
    virtual Task<void>      checkout(const GitQuery&) = 0;
};

class UnavailableGitService final : public GitService {
public:
    Task<GitStatus> status(const GitQuery&) override { throw unavailable(); }
    Task<GitDiff>   diff(const GitQuery&) override { throw unavailable(); }
    Task<GitLog>    log(const GitQuery&) override { throw unavailable(); }
    Task<GitShow>   show(const GitQuery&) override { throw unavailable(); }
    Task<GitBranch> branch(const GitQuery&) override { throw unavailable(); }
    Task<void>      checkout(const GitQuery&) override { throw unavailable(); }

private:
    static ToolError unavailable() {
        return ToolError{ToolErrorCode::Internal, "git is not available in v1"};
    }
};

// LSP is Phase 2; v1 exposes the type only so `lsp()` can return nullptr.
class LspService {
public:
    virtual ~LspService() = default;
};

} // namespace ymh
