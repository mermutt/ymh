#pragma once

// Signal policy for the workspace daemon (04 §3.6, amended by 11 §3.3; E8).
//
// SIGTERM / SIGINT -> graceful shutdown (04 §3.5).
// SIGHUP           -> ignored, logged once (the daemon is setsid'd).
// SIGPIPE          -> ignored process-wide, plus MSG_NOSIGNAL on socket writes.
// SIGCHLD          -> SIG_DFL. There is NO global reaper and NO self-pipe:
//                     SIG_IGN / SA_NOCLDWAIT is forbidden because it auto-reaps
//                     and destroys `waitpid`. Child status is owned exclusively
//                     by `src/execution/process.cpp`, which reaps by specific
//                     pid. A global `waitpid(-1)` sweep is a defect (M-F5): it
//                     races the tool layer and destroys the `ProcessResult` it
//                     needs.
//
// The helpers below let the daemon assert the disposition at startup and let
// tests prove the policy without mutating global state.

#include <cstdint>
#include <signal.h>

namespace ymh {

enum class ChildReapPolicy : std::uint8_t {
    SpecificPid,  // SIG_DFL or a non-reaping handler: waitpid(pid) is safe
    Ignored,      // SIG_IGN: the kernel auto-reaps; waitpid(pid) is unsafe
    NoCldWait,    // SA_NOCLDWAIT: the kernel auto-reaps; waitpid(pid) is unsafe
    Unknown,      // sigaction(SIGCHLD, ...) failed
};

// Reads the current SIGCHLD disposition without changing it.
[[nodiscard]] inline ChildReapPolicy childReapPolicy() noexcept {
    struct sigaction current {};
    if (::sigaction(SIGCHLD, nullptr, &current) != 0) {
        return ChildReapPolicy::Unknown;
    }
    if ((current.sa_flags & SA_NOCLDWAIT) != 0) {
        return ChildReapPolicy::NoCldWait;
    }
    if (current.sa_handler == SIG_IGN) {
        return ChildReapPolicy::Ignored;
    }
    return ChildReapPolicy::SpecificPid;
}

// True iff the specific-pid `waitpid` in `process.cpp` can still observe a
// child's exit status (E8).
[[nodiscard]] inline bool specificPidReapIsSafe() noexcept {
    return childReapPolicy() == ChildReapPolicy::SpecificPid;
}

} // namespace ymh
