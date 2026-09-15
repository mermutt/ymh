#pragma once

// Unix-socket server layer (docs/design/05-transport.md §3.1–§3.3, §9).
//
// Wraps a `ProtocolServer` with an Asio acceptor on `<workspace>/.ymh/host.sock`.
// It owns the accept loop and the async reads/writes; it never interprets a
// frame. Every accepted connection reports its SO_PEERCRED to the protocol
// engine, which enforces the same-UID trust boundary (§4.2, T15).
//
// The `ProtocolServer` must outlive the `TransportServer`.

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include <asio.hpp>

#include "ymh/transport/protocol.hpp"
#include "ymh/transport/protocol_server.hpp"

namespace ymh::protocol {

class SocketSession;

class TransportServer {
public:
    TransportServer(ProtocolServer& server, std::string socket_path,
                    TransportLimits limits = {});
    ~TransportServer();

    TransportServer(const TransportServer&) = delete;
    TransportServer& operator=(const TransportServer&) = delete;

    void start();
    void stop();

    [[nodiscard]] const std::string& socketPath() const noexcept { return socket_path_; }

private:
    void do_accept();

    ProtocolServer&                          server_;
    std::string                              socket_path_;
    TransportLimits                          limits_;
    asio::io_context                         io_;
    asio::local::stream_protocol::acceptor   acceptor_;
    std::thread                              thread_;
    std::atomic<bool>                        stopping_{false};
    bool                                     started_{false};
    std::set<std::shared_ptr<SocketSession>> sessions_;
};

} // namespace ymh::protocol
