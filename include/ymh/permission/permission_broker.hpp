#pragma once

// Async permission broker (docs/design/09-permissions.md §4, amended by
// docs/design/11-m2-errata.md §7; defects D18/D19).
//
// M1's `PermissionGate` is the transport-free, blocking core. `PermissionBroker`
// is the M2 daemon-side wrapper: it registers an Ask, broadcasts
// `permission.request` onto the transport io thread, and returns a future
// resolved later by `onDecision` (transport thread), the fail-closed deadline,
// or cancellation. It never blocks the transport thread and never blocks the
// daemon io loop; the turn thread waits on the returned future.
//
// Frozen rules (errata §7.3):
//   * D19.1 fail-closed: timeout => Deny "timeout", cancel => Deny "cancelled".
//   * D19.2 first-wins: `onDecision` applies the first decision atomically and
//     ignores later/unknown/expired `request_id`s.
//   * D19.3 live subscriber count: zero Interactive subscribers => auto-deny,
//     `resolve` never waits.
//   * D19.4 broadcast + `request_id` dedupe; `pending()` feeds re-broadcast.
//   * D19.5 turn thread != transport thread; the pending-map mutex is never
//     held across a transport call.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/permission/permission_transport.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/ids.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh {

// Pinned public clock (errata §7.2): the M2 code had only file-local aliases;
// exposing it makes the deadline injectable and testable.
using Clock = std::chrono::steady_clock;

// AMENDED (errata §7.2, E21): an inert `Clock&` cannot inject time
// (`clock.now()` always resolves to the static `Clock::now()`), so the broker
// takes a callable clock READER instead. It is the broker's only time source;
// `Clock::now` is the default, so production construction is unchanged, while
// tests inject `ManualClock::reader()` for deterministic timeouts.
using ClockReader = std::function<Clock::time_point()>;

class PermissionBroker {
public:
    // Thread-safe reader for the live Interactive-subscriber count of a session
    // (D19.3). The daemon publishes `ProtocolServer::sessionSubscriberCount`
    // as an atomic snapshot and injects this reader; the broker calls it on the
    // turn thread and must never touch the io-confined accessor directly.
    using SubscriberCount = std::function<std::size_t(SessionId)>;

    // `now` defaults to `Clock::now`; tests pass `ManualClock::reader()`.
    // Every deadline/expiry is computed from `now()`, NEVER `Clock::now()`
    // directly (errata §7.2, E21).
    PermissionBroker(PermissionPolicy& policy,
                     PermissionTransport& transport,
                     PermissionConfig config,
                     ClockReader now = Clock::now);
    ~PermissionBroker();

    PermissionBroker(const PermissionBroker&) = delete;
    PermissionBroker& operator=(const PermissionBroker&) = delete;
    PermissionBroker(PermissionBroker&&) = delete;
    PermissionBroker& operator=(PermissionBroker&&) = delete;

    // Called for an Ask verdict from the TURN thread. Registers the request,
    // broadcasts `permission.request`, and returns a future resolved by
    // `onDecision`/timeout/cancellation. Never throws: any failure (no
    // subscribers, transport down, already cancelled) resolves fail-closed with
    // a ready future. `Allow`/`Deny` verdicts never touch the wire.
    [[nodiscard]] std::future<PermissionOutcome> resolve(const PermissionRequest& request,
                                                        CancellationToken cancel);

    // Transport callback; runs on the TRANSPORT thread. First decision wins.
    // Returns true iff this call resolved the request; false for an unknown,
    // expired, or already-resolved `request_id` (errata §4.3 mapping:
    // `TransportHost::decidePermission` -> here).
    bool onDecision(const protocol::PermissionDecisionParams& params);

    // A newly attached Interactive client re-issues these through the same
    // broadcast path so a late supervisor can answer (D19.4). Ordered by
    // registration.
    [[nodiscard]] std::vector<protocol::PermissionRequest> pending(SessionId session) const;

    [[nodiscard]] std::size_t pendingCount() const noexcept;

    void set_subscriber_count(SubscriberCount count);

private:
    struct State;

    std::shared_ptr<State> state_;
};

} // namespace ymh
