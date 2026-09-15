#pragma once

// Subprocess execution service (07 §6.4). argv-first: the caller supplies an
// executable and an argv vector, never a concatenated shell string. The child
// is placed in its own process group so the tool can signal the whole tree
// without touching the daemon or a sibling tool (X13). Output is streamed to
// `OutputSink` when present and bounded by the sink's ring.

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/output.hpp"

namespace ymh {

struct ProcessRequest {
    std::string               executable;      // absolute or PATH-resolved
    std::vector<std::string>  argv;            // never a concatenated shell string
    std::filesystem::path     cwd;             // MUST be resolve()d under root()
    std::vector<std::pair<std::string, std::string>> environment;
    std::chrono::milliseconds timeout{0};      // 0 => no deadline
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

class ProcessService {
public:
    virtual ~ProcessService() = default;

    virtual Task<ProcessResult> run(const ProcessRequest&,
                                    CancellationToken) = 0;
};

class LocalProcessService final : public ProcessService {
public:
    explicit LocalProcessService(std::chrono::milliseconds terminate_grace =
                                     std::chrono::milliseconds{2000});

    Task<ProcessResult> run(const ProcessRequest& request,
                            CancellationToken cancel) override;

private:
    std::chrono::milliseconds terminate_grace_;
};

} // namespace ymh
