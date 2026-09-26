#pragma once

// Supervisor-side connection seam (docs/design/05-transport.md §1.1, §3.2).
//
// `HostConnection` is a synchronous JSON-RPC client over the length-prefixed
// Unix socket: connect, mandatory `host.hello`, request/response, and a pull
// queue for server notifications (`event.stream`, `host.event`,
// `permission.request`). The TUI (spec 10) runs it on its own thread; the
// transport core never depends on it.

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ymh/transport/host.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh::protocol {

class HostConnection {
public:
    HostConnection();
    ~HostConnection();

    HostConnection(const HostConnection&) = delete;
    HostConnection& operator=(const HostConnection&) = delete;

    void connect(const std::string& socket_path);
    [[nodiscard]] HelloResult handshake(ServerProfile profile,
                                        ClientInstanceId instance,
                                        std::chrono::milliseconds timeout = std::chrono::seconds{5});
    [[nodiscard]] HelloResult handshake(ServerProfile profile,
                                        ClientInstanceId instance,
                                        ClientRole role,
                                        std::chrono::milliseconds timeout = std::chrono::seconds{5});
    [[nodiscard]] nlohmann::json request(
        std::string_view method, nlohmann::json params = nlohmann::json::object(),
        std::chrono::milliseconds timeout = std::chrono::seconds{30});
    [[nodiscard]] std::optional<Notification> nextNotification(
        std::chrono::milliseconds timeout);
    void detach(std::chrono::milliseconds timeout = std::chrono::seconds{5});
    void close();

    [[nodiscard]] bool isConnected() const noexcept { return fd_ >= 0; }
    [[nodiscard]] ClientId clientId() const noexcept { return client_id_; }
    [[nodiscard]] const HelloResult& hello() const noexcept { return hello_; }

private:
    void send_message(const nlohmann::json& message);
    [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) const;
    [[nodiscard]] bool fill_incoming(std::chrono::milliseconds timeout);

    int                          fd_{-1};
    std::uint64_t                next_id_{0};
    std::string                  read_buffer;
    std::deque<std::string>      incoming;
    std::deque<Notification>     pending;
    TransportLimits              limits;
    ClientId                     client_id_{};
    HelloResult                  hello_;
};

} // namespace ymh::protocol
