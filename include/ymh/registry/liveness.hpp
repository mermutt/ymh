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

// 88-A3 (spec 88 §7): the create-controlling overload. With
// `create_if_missing == false` the probe opens the existing lock file
// `O_RDONLY|O_CLOEXEC` and never creates `<ws>/.ymh/sessions.lock`, so
// `ymh doctor` stays read-only (DOC1/88-D5); a missing file reports not held.
// The 1-arg overload above delegates here with `create_if_missing == true` and is
// behaviourally unchanged for all existing callers. Caller: `run_doctor`
// (create_if_missing == false); every existing caller keeps the 1-arg overload.
[[nodiscard]] WorkspaceLockProbe probeWorkspaceLock(
    const std::filesystem::path& workspaceRoot, bool create_if_missing);

// `kill(pid, 0)` classification (§6.3 rule 3). Only `ESRCH` proves death;
// `EPERM` means "exists but not ours" ⇒ Alive.
enum class KillHint : std::uint8_t { Alive, Dead, Unknown };

[[nodiscard]] KillHint killHint(std::int32_t pid);

} // namespace ymh
