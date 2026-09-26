#pragma once

// Reap TOCTOU close (11-m2-errata.md §12.5, D26; 03 R5/R11, 04 §7.3).
//
// Clearing a host claim is observe-then-mutate: observe the workspace's sidecar
// lock, then clear `host_pid`/`host_boot_id`. A liveness observation taken
// before the D22 (03 §5) writer lock is stale by the time the lock is held, so
// the claim must be re-probed under the lock, immediately before the mutation.
//
// `reapClaimUnderLock` makes that ordering explicit and testable without the
// registry or the liveness types, so the execution layer keeps no dependency on
// the registry layer. The caller is responsible for holding the D22 lock.
//
// A held lock aborts the reap (`false`); a free lock runs `clear` and returns
// its result. A process is never signalled (04 decision (h)).

#include <utility>

namespace ymh {

// `reprobe` returns true when the sidecar lock is held (a live daemon). `clear`
// performs the mutation and returns whether a claim row was actually cleared.
template <class Reprobe, class Clear>
[[nodiscard]] bool reapClaimUnderLock(Reprobe&& reprobe, Clear&& clear) {
    if (std::forward<Reprobe>(reprobe)()) {
        return false;
    }
    return static_cast<bool>(std::forward<Clear>(clear)());
}

} // namespace ymh
