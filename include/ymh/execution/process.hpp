#pragma once

// Subprocess execution service (07 §6.4). argv-first: the caller supplies an
// executable and an argv vector, never a concatenated shell string. The child
// is placed in its own process group so the tool can signal the whole tree
// without touching the daemon or a sibling tool (X13). Output is streamed to
// `OutputSink` when present and bounded by the sink's ring.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/output.hpp"

namespace ymh {

enum class ProcessEnvMode : std::uint8_t {
    Inherit,
    Minimal,
};

struct ProcessRequest {
    std::string               executable;      // absolute or PATH-resolved
    std::vector<std::string>  argv;            // never a concatenated shell string
    std::filesystem::path     cwd;             // MUST be resolve()d under root()
    std::vector<std::pair<std::string, std::string>> environment;
    ProcessEnvMode            env_mode = ProcessEnvMode::Inherit;
    std::optional<std::filesystem::path> stderr_path;  // spawn()-only
    std::chrono::milliseconds timeout{0};      // 0 => no deadline
    // 46-D8: the clamped per-tool-run budget. `nullopt` => the deadline layer is
    // disabled (`timeout` applies); `0ms` => already expired, the child is NOT
    // started and `timed_out` is returned; `>0` => the wait budget.
    std::optional<std::chrono::milliseconds> deadline;
    bool                      capture_stdout{true};
    bool                      capture_stderr{true};
    OutputSink*               sink{nullptr};   // streaming; null => buffered
};

struct ProcessResult {
    int  exit_code{-1};
    bool signalled{false};
    int  signal{0};
    bool timed_out{false};
};

class ChildProcessHandle;

class ProcessService {
public:
    virtual ~ProcessService() = default;

    virtual Task<ProcessResult> run(const ProcessRequest&,
                                    CancellationToken) = 0;

    // Additive (15 §5.1, AM-0b). Spawn a long-lived child with piped
    // stdin/stdout. The child is placed in its own process group and is reaped
    // by the same specific-pid path as run() (07 §9.2, 11 E8). `sink` is
    // ignored; the caller reads via the handle. On failure throws
    // ToolError{SpawnFailed}.
    virtual Task<std::unique_ptr<ChildProcessHandle>>
    spawn(const ProcessRequest&) = 0;
};

// Additive (15 §5.1, AM-0b). A long-lived bidirectional child. One owner; the
// handle closes its pipe descriptors on destruction and reaps the child so no
// orphan/zombie outlives the caller (M15).
class ChildProcessHandle {
public:
    virtual ~ChildProcessHandle() = default;

    virtual std::uint64_t pid() const noexcept = 0;

    // Bounded write to the child's stdin. Backpressure is the caller's
    // (McpTransport) concern; a closed pipe is ToolError{Io}.
    virtual Task<void> writeStdin(std::string_view bytes, CancellationToken) = 0;
    virtual void closeStdin() = 0;

    // One read chunk; empty result => EOF.
    virtual Task<std::size_t> readStdout(std::span<char>, CancellationToken) = 0;

    virtual void signal(int sig) noexcept = 0;   // signals the process group
    virtual Task<ProcessResult> wait(CancellationToken) = 0;
};

class LocalProcessService final : public ProcessService {
public:
    explicit LocalProcessService(std::chrono::milliseconds terminate_grace =
                                     std::chrono::milliseconds{2000});

    Task<ProcessResult> run(const ProcessRequest& request,
                            CancellationToken cancel) override;

    Task<std::unique_ptr<ChildProcessHandle>> spawn(
        const ProcessRequest& request) override;

private:
    std::chrono::milliseconds terminate_grace_;
};

// The process layer's single specific-pid reaper, exposed so the PTY pump can
// collect one child without a second `waitpid` (14 §7.2, E-P5):
//   * tryReap — `waitpid(pid, &st, WNOHANG)`; nullopt while running. Loop-safe.
//   * reap    — blocking `waitpid(pid, &st, 0)`; OFF-LOOP callers only.
// A direct `waitpid` in `execution/pty` is a defect (P6).
[[nodiscard]] std::optional<ProcessResult> tryReap(int pid);
[[nodiscard]] ProcessResult                reap(int pid);

// Additive (15 §2.3/§6.3): the stdout descriptor of a `spawn()`ed child, so a
// poll-based MCP transport can bound a read with a deadline. -1 when the handle
// exposes none.
[[nodiscard]] int childStdoutFd(ChildProcessHandle& handle) noexcept;

} // namespace ymh
