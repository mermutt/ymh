#include "ymh/transport/host_connection.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ymh/transport/frame_codec.hpp"

namespace ymh::protocol {
namespace {

[[nodiscard]] std::runtime_error connection_error(const std::string& what) {
    return std::runtime_error("host connection: " + what);
}

[[nodiscard]] std::chrono::milliseconds remaining_until(
    std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

} // namespace

HostConnection::HostConnection() = default;

HostConnection::~HostConnection() { close(); }

void HostConnection::connect(const std::string& socket_path) {
    close();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw connection_error(std::string("socket: ") + std::strerror(errno));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        throw connection_error("socket path too long");
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        throw connection_error("connect: " + reason);
    }
    fd_ = fd;
}

void HostConnection::send_message(const nlohmann::json& message) {
    if (fd_ < 0) {
        throw connection_error("not connected");
    }
    const std::string frame = FrameCodec::encode(message.dump(), limits.max_frame_bytes);
    std::size_t written = 0;
    while (written < frame.size()) {
        const ssize_t count =
            ::send(fd_, frame.data() + written, frame.size() - written, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw connection_error(std::string("write: ") + std::strerror(errno));
        }
        written += static_cast<std::size_t>(count);
    }
}

bool HostConnection::wait_readable(std::chrono::milliseconds timeout) const {
    if (fd_ < 0) {
        return false;
    }
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (ready < 0) {
        if (errno == EINTR) {
            return false;
        }
        throw connection_error(std::string("poll: ") + std::strerror(errno));
    }
    return ready > 0;
}

bool HostConnection::fill_incoming(std::chrono::milliseconds timeout) {
    if (!incoming.empty()) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (incoming.empty()) {
        std::string frame;
        DecodeStatus status = DecodeStatus::NeedMore;
        try {
            status = FrameCodec::decode_step(read_buffer, limits.max_frame_bytes, frame);
        } catch (const ProtocolError& error) {
            throw RpcException(error.code, error.what());
        }
        if (status == DecodeStatus::Ok) {
            incoming.push_back(std::move(frame));
            return true;
        }
        if (!wait_readable(remaining_until(deadline))) {
            return false;
        }
        char buffer[65536];
        const ssize_t count = ::read(fd_, buffer, sizeof(buffer));
        if (count == 0) {
            throw connection_error("peer closed the connection");
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw connection_error(std::string("read: ") + std::strerror(errno));
        }
        read_buffer.append(buffer, static_cast<std::size_t>(count));
    }
    return true;
}

HelloResult HostConnection::handshake(ServerProfile profile, ClientInstanceId instance,
                                      std::chrono::milliseconds timeout) {
    HelloParams params;
    params.protocol_version = kProtocolVersion;
    params.profile = profile;
    params.client_instance = std::move(instance);
    nlohmann::json body;
    to_json(body, params);
    const nlohmann::json result = request(method::kHostHello, std::move(body), timeout);
    hello_ = result.get<HelloResult>();
    client_id_ = hello_.client_id;
    return hello_;
}

nlohmann::json HostConnection::request(std::string_view method, nlohmann::json params,
                                       std::chrono::milliseconds timeout) {
    const RequestId id{static_cast<std::int64_t>(++next_id_)};
    const std::string key = request_id_key(id);
    send_message(encode(Request{id, std::string{method}, std::move(params)}));

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (!fill_incoming(remaining_until(deadline))) {
            throw RpcException(static_cast<int>(RpcCode::InternalError),
                               "timed out waiting for a response");
        }
        Message message = parse_message(incoming.front());
        incoming.pop_front();
        if (const auto* response = std::get_if<Response>(&message)) {
            if (request_id_key(response->id) == key) {
                return response->result;
            }
            continue;
        }
        if (const auto* error = std::get_if<ErrorResponse>(&message)) {
            if (request_id_key(error->id) == key) {
                throw RpcException(error->code, error->message, error->data);
            }
            continue;
        }
        if (const auto* notification = std::get_if<Notification>(&message)) {
            pending.push_back(*notification);
        }
    }
}

std::optional<Notification> HostConnection::nextNotification(
    std::chrono::milliseconds timeout) {
    if (!pending.empty()) {
        Notification notification = std::move(pending.front());
        pending.pop_front();
        return notification;
    }
    if (!fill_incoming(timeout)) {
        return std::nullopt;
    }
    Message message = parse_message(incoming.front());
    incoming.pop_front();
    if (const auto* notification = std::get_if<Notification>(&message)) {
        return *notification;
    }
    return std::nullopt;
}

void HostConnection::detach(std::chrono::milliseconds timeout) {
    if (fd_ < 0) {
        return;
    }
    try {
        static_cast<void>(request(method::kHostDetach, nlohmann::json::object(), timeout));
    } catch (const std::exception&) {
    }
    close();
}

void HostConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    read_buffer.clear();
    incoming.clear();
    pending.clear();
    client_id_ = ClientId{};
    hello_ = HelloResult{};
}

} // namespace ymh::protocol
