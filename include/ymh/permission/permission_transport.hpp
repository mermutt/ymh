#pragma once

// Transport port for `PermissionBroker` (errata 11-m2-errata.md §7.2/§7.3).
//
// The broker must post `permission.request` onto the transport's single io
// thread and must schedule its fail-closed deadline there too, but it must not
// know about `asio`, the socket, or `ProtocolServer`. Errata §7.2 pins the
// broker's ctor against `protocol::TransportServer&` and `TransportServer::post`
// (§2.2). That method belongs to the transport component (track A); the broker
// is deliberately written against this narrow, track-C-owned port instead, so
// it compiles and is unit-testable before `post()` lands. The daemon adapts a
// `TransportServer` to this port in two lines (see
// `transport_server_adapter.hpp`).
//
// Threading contract (E2, D19.5):
//   * both methods are safe to call from the turn thread;
//   * the supplied callbacks run on the transport io thread only;
//   * neither method blocks.

#include <chrono>
#include <functional>

#include "ymh/transport/protocol.hpp"

namespace ymh {

class PermissionTransport {
public:
    virtual ~PermissionTransport() = default;

    // Broadcast `permission.request` to every attached Interactive subscriber
    // of the session (ProtocolServer::onPermissionRequest, 05 §7.6; errata
    // §7.3 D19.4 — broadcast, not targeted). Returns false when the transport
    // is not running (before start()/after stop()); the broker then resolves
    // fail-closed and never leaves the request pending.
    virtual bool broadcast_permission_request(protocol::PermissionRequest request) = 0;

    // Run `fn` on the transport io thread after `delay` (a steady timer on
    // `TransportServer::io()`, errata §2.2). Returns false when the transport
    // is not running. `fn` is invoked at most once; a request resolved before
    // the deadline makes the callback a no-op (first-wins).
    virtual bool schedule_after(std::chrono::milliseconds delay,
                                std::function<void()> fn) = 0;
};

} // namespace ymh
