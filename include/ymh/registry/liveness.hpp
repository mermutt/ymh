#pragma once

// Host liveness primitives for the workspace registry (03 §6.3, R5, R6).
//
// Liveness is sidecar-flock-primary: the daemon holds
// `<workspace>/.ymh/sessions.lock` for its whole lifetime and the kernel
// releases it on process death, including SIGKILL (§9.7). `kill(pid, 0)` and
// heartbeat staleness are hints only and are never sufficient to clear a claim.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ymh {

// Best-effort diagnostic read from a held sidecar lock. The contents are
// self-reported and non-authoritative (02 §5.1); only `held` comes from the
// kernel flock. `pid`/`bootId` are populated when the diagnostic parses.
struct WorkspaceLockProbe {
    bool                        held = false;
    std::optional<std::int32_t> pid;
    std::optional<std::string>  bootId;
};

// `open(<workspace>/.ymh/sessions.lock, O_RDWR|O_CREAT|O_CLOEXEC, 0600)`, then
// `flock(LOCK_EX|LOCK_NB)`: success ⇒ the lock is free (`held = false`),
// `EWOULDBLOCK` ⇒ held (read the diagnostic). A missing/unopenable lock file is
// reported as not held; callers treat that as "no live daemon".
[[nodiscard]] WorkspaceLockProbe probeWorkspaceLock(
    const std::filesystem::path& workspaceRoot);

// `kill(pid, 0)` classification (§6.3 rule 3). Only `ESRCH` proves death;
// `EPERM` means "exists but not ours" ⇒ Alive.
enum class KillHint : std::uint8_t { Alive, Dead, Unknown };

[[nodiscard]] KillHint killHint(std::int32_t pid);

} // namespace ymh
