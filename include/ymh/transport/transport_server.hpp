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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/types.h>

#include <asio.hpp>

#include "ymh/transport/protocol.hpp"
#include "ymh/transport/protocol_server.hpp"

namespace ymh::protocol {

class SocketSession;

// Startup failure of `TransportServer::start()` (04 §5.2, 11 §2.3). The code
// mirrors the `HostErrorCode` the daemon reports: `AlreadyRunning` when the
// connect-probe reaches a live server, `SocketUnavailable` for any other probe
// errno, and `SocketPathTooLong` before bind.
class TransportError : public std::runtime_error {
public:
    TransportError(HostErrorCode code, std::string message);
    [[nodiscard]] HostErrorCode code() const noexcept { return code_; }

private:
    HostErrorCode code_;
};

class TransportServer {
public:
    TransportServer(ProtocolServer& server, std::string socket_path,
                    TransportLimits limits = {});
    ~TransportServer();

    TransportServer(const TransportServer&) = delete;
    TransportServer& operator=(const TransportServer&) = delete;

    void start();
    void stop();

    // D1: marshal `fn` onto the transport's io thread. Safe from any thread.
    // Returns false (and drops `fn`) before start() or after stop() has begun.
    bool post(std::function<void()> fn);

    // D1: the transport's io_context, for constructing dependent steady_timers
    // and scheduling on the same thread. Never run() it from the caller; the
    // transport owns the single runner thread.
    [[nodiscard]] asio::io_context& io() noexcept { return io_; }

    [[nodiscard]] bool running() const noexcept {
        return started_.load() && !stopping_.load();
    }
    [[nodiscard]] const std::string& socketPath() const noexcept { return socket_path_; }

private:
    void do_accept();
    [[nodiscard]] bool socket_path_too_long() const;

    ProtocolServer&                          server_;
    std::string                              socket_path_;
    TransportLimits                          limits_;
    asio::io_context                         io_;
    asio::local::stream_protocol::acceptor   acceptor_;
    std::thread                              thread_;
    std::atomic<bool>                        stopping_{false};
    std::atomic<bool>                        started_{false};
    std::mutex                               post_mutex_;
    std::optional<ino_t>                     own_inode_;
    std::set<std::shared_ptr<SocketSession>> sessions_;
};

} // namespace ymh::protocol
