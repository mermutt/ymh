#pragma once

// The canonical ExecutionEnvironment (00 §18, 07 §6). It is the capability
// boundary: every filesystem, process cwd, PTY, git, and LSP path is resolved
// through `resolve()`. Resolution is root-relative and realpath-canonicalized;
// `getcwd()` is never a resolution base in tool code (X1-X4).
//
// `LocalEnvironment` is the v1 implementation. The environment root is a
// constructor parameter, baked in before any tool is written (07 decision (n)).

#include <filesystem>
#include <string_view>

#include "ymh/execution/config.hpp"
#include "ymh/execution/filesystem.hpp"
#include "ymh/execution/git_service.hpp"
#include "ymh/execution/process.hpp"
#include "ymh/execution/services.hpp"

namespace ymh {

enum class SandboxMode : std::uint8_t {
    Workspace,     // v1 default: root-confined FS; daemon uid; caps apply
    ReadOnly,      // inspection only: no writes, no subprocesses, no PTYs
    Unrestricted,  // explicit opt-out: resolve() skips root containment
};

class ExecutionEnvironment {
public:
    virtual ~ExecutionEnvironment() = default;

    virtual const std::filesystem::path& root() const = 0;
    virtual std::filesystem::path resolve(std::string_view) const = 0;

    // The confinement policy `resolve()` obeys (§6.2, §6.8). Immutable per
    // environment; selected by config, never by the model (decision (r)).
    virtual SandboxMode mode() const noexcept = 0;

    virtual Filesystem&      fs() = 0;
    virtual ProcessService&  process() = 0;
    virtual PtyService&      pty() = 0;
    virtual GitService&      git() = 0;
    virtual LspService*      lsp() = 0;      // nullptr until Phase 2

    // 46-D8: the tunable bounds the tools/execution layer consumes, exposed so
    // the agent loop can derive the per-tool-run deadline.
    [[nodiscard]] virtual const ToolConfig& toolConfig() const noexcept = 0;
};

class LocalEnvironment final : public ExecutionEnvironment {
public:
    // Throws std::invalid_argument when `root` is absent or not a directory.
    // The stored root is realpath-canonical. `pty` is borrowed (14 §4.5, E-P7);
    // nullptr keeps the internal `UnavailablePtyService` (the v1 default).
    explicit LocalEnvironment(std::filesystem::path root,
                              SandboxMode mode = SandboxMode::Workspace,
                              ToolConfig config = {},
                              PtyService* pty = nullptr);

    const std::filesystem::path& root() const override { return root_; }
    std::filesystem::path        resolve(std::string_view path) const override;
    SandboxMode                  mode() const noexcept override { return mode_; }

    Filesystem&     fs() override { return fs_; }
    ProcessService& process() override { return process_; }
    PtyService&     pty() override { return *pty_; }
    GitService&     git() override { return git_; }
    LspService*     lsp() override { return nullptr; }

    [[nodiscard]] const ToolConfig& toolConfig() const noexcept override {
        return config_;
    }

private:
    std::filesystem::path root_;
    SandboxMode           mode_;
    LocalFilesystem       fs_;
    LocalProcessService   process_;
    UnavailablePtyService pty_fallback_;
    PtyService*           pty_ = nullptr;
    LibGit2GitService     git_;
    ToolConfig            config_;
};

// Component-wise containment over weakly-canonical operands (07 §6.2). Never a
// string-prefix test: `/a/bc` is not inside `/a/b`.
[[nodiscard]] bool path_is_within(const std::filesystem::path& candidate,
                                  const std::filesystem::path& base);

} // namespace ymh
