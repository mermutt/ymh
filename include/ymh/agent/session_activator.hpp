#pragma once

// 55-D11/D13: the background-activation seam. `SubagentService` submits a child
// activation through this seam; the live implementation forwards to
// `TurnExecutor::submit` WITHOUT setting `HostRuntime::active_session_` (55-H9).
// It is injected rather than reaching through `HostRuntime`, which is
// constructed WITH `WorkspaceRuntime` while `SubagentService` is constructed BY
// it (circular).

#include "ymh/session/ids.hpp"

namespace ymh {

class SessionActivator {
public:
    virtual ~SessionActivator() = default;

    // Submit one activation for `child`. Returns false (surfaced as InboxFull)
    // when the executor queue is full; the caller then rolls the child back
    // (55-F13). The activation runs asynchronously; settlement is the drain
    // epoch (55-D4), observed separately from this submit.
    [[nodiscard]] virtual bool submit(const SessionId& child) = 0;
};

} // namespace ymh
