#include "ymh/transport/transport_server.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

namespace ymh::protocol {

class SocketSession final : public std::enable_shared_from_this<SocketSession> {
public:
    SocketSession(asio::io_context& io, ProtocolServer& server,
                  asio::local::stream_protocol::socket socket, TransportLimits limits,
                  std::function<void(SocketSession*)> on_close)
        : socket_(std::move(socket)),
          server_(server),
          handshake_timer_(io),
          idle_timer_(io),
          limits_(limits),
          on_close_(std::move(on_close)) {}

    void start() {
        ucred credential{};
        socklen_t length = sizeof(credential);
        if (::getsockopt(socket_.native_handle(), SOL_SOCKET, SO_PEERCRED, &credential,
                         &length) == 0) {
            peer_uid_ = credential.uid;
            peer_pid_ = credential.pid;
        }

        const auto self = shared_from_this();
        id_ = server_.openConnection(
            peer_uid_, peer_pid_,
            [self](ClientId id, std::string frame) { self->on_send(id, std::move(frame)); },
            [self](ClientId id, std::string reason) { self->on_drop(id, std::move(reason)); });

        handshake_timer_.expires_after(limits_.handshake_timeout);
        handshake_timer_.async_wait([self](const std::error_code& error) {
            if (!error && !self->server_.isHandshaken(self->id_)) {
                self->shutdown();
            }
        });
        read();
    }

    void close_from_owner() { shutdown(); }

private:
    void read() {
        if (closed_) {
            return;
        }
        const auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(read_buffer_),
            [self](const std::error_code& error, std::size_t count) {
                if (error) {
                    self->server_.closeConnection(self->id_);
                    self->shutdown();
                    return;
                }
                self->server_.receiveBytes(self->id_,
                                           std::string_view{self->read_buffer_.data(), count});
                self->arm_idle_timer();
                self->read();
            });
    }

    void arm_idle_timer() {
        if (server_.isHandshaken(id_)) {
            handshake_timer_.cancel();
        }
        idle_timer_.expires_after(limits_.idle_timeout);
        const auto self = shared_from_this();
        idle_timer_.async_wait([self](const std::error_code& error) {
            if (!error) {
                self->server_.closeConnection(self->id_);
                self->shutdown();
            }
        });
    }

    void on_send(ClientId, std::string frame) {
        if (closed_) {
            return;
        }
        write_queue_.push_back(std::move(frame));
        write_next();
    }

    void write_next() {
        if (closed_ || current_frame_.has_value()) {
            return;
        }
        if (write_queue_.empty()) {
            return;
        }
        current_frame_ = std::move(write_queue_.front());
        write_queue_.pop_front();
        const auto self = shared_from_this();
        asio::async_write(
            socket_, asio::buffer(*current_frame_),
            [self](const std::error_code& error, std::size_t count) {
                self->current_frame_.reset();
                if (error) {
                    self->shutdown();
                    return;
                }
                self->server_.onFrameWritten(self->id_, count);
                self->write_next();
            });
    }

    void on_drop(ClientId, std::string) { shutdown(); }

    void shutdown() {
        if (closed_) {
            return;
        }
        const auto self = shared_from_this();
        closed_ = true;
        std::error_code error;
        handshake_timer_.cancel();
        idle_timer_.cancel();
        socket_.shutdown(asio::socket_base::shutdown_both, error);
        socket_.close(error);
        write_queue_.clear();
        // `current_frame_` is retained until its async_write completion runs:
        // that handler owns the only remaining reference to the buffer.
        if (on_close_) {
            on_close_(this);
        }
    }

    asio::local::stream_protocol::socket socket_;
    ProtocolServer&                      server_;
    asio::steady_timer                   handshake_timer_;
    asio::steady_timer                   idle_timer_;
    TransportLimits                      limits_;
    std::array<char, 65536>              read_buffer_{};
    std::deque<std::string>              write_queue_;
    std::optional<std::string>           current_frame_;
    std::function<void(SocketSession*)>  on_close_;
    ClientId                             id_{};
    std::uint32_t                        peer_uid_{0};
    std::int32_t                         peer_pid_{0};
    bool                                 closed_{false};
};

TransportError::TransportError(HostErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

TransportServer::TransportServer(ProtocolServer& server, std::string socket_path,
                                 TransportLimits limits)
    : server_(server),
      socket_path_(std::move(socket_path)),
      limits_(limits),
      owned_io_(std::make_unique<asio::io_context>()),
      io_(owned_io_.get()),
      acceptor_(*io_) {}

TransportServer::TransportServer(ProtocolServer& server, std::string socket_path,
                                 asio::io_context& io, TransportLimits limits)
    : server_(server),
      socket_path_(std::move(socket_path)),
      limits_(limits),
      io_(&io),
      acceptor_(*io_) {}

TransportServer::~TransportServer() { stop(); }

bool TransportServer::socket_path_too_long() const {
    sockaddr_un address{};
    return socket_path_.size() >= sizeof(address.sun_path);
}

bool TransportServer::post(std::function<void()> fn) {
    std::lock_guard lock(post_mutex_);
    if (!started_.load() || stopping_.load()) {
        return false;
    }
    asio::post(*io_, std::move(fn));
    return true;
}

void TransportServer::start() {
    if (socket_path_too_long()) {
        throw TransportError(HostErrorCode::SocketPathTooLong,
                             "transport: socket path too long: " + socket_path_);
    }

    struct stat status {};
    if (::lstat(socket_path_.c_str(), &status) == 0) {
        const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe < 0) {
            throw TransportError(HostErrorCode::SocketUnavailable,
                                 std::string("transport: probe socket: ") + std::strerror(errno));
        }
        ::fcntl(probe, F_SETFL, ::fcntl(probe, F_GETFL, 0) | O_NONBLOCK);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
        const int connected =
            ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        const int probe_errno = errno;
        ::close(probe);
        if (connected == 0 || probe_errno == EINPROGRESS || probe_errno == EAGAIN) {
            throw TransportError(HostErrorCode::AlreadyRunning,
                                 "transport: another daemon is live at " + socket_path_);
        }
        if (probe_errno == ECONNREFUSED) {
            ::unlink(socket_path_.c_str());
        } else {
            throw TransportError(HostErrorCode::SocketUnavailable,
                                 std::string("transport: connect-probe: ") +
                                     std::strerror(probe_errno));
        }
    }

    std::error_code error;
    acceptor_.open(asio::local::stream_protocol(), error);
    if (error) {
        throw std::runtime_error("transport: open acceptor: " + error.message());
    }
    // 05 §5.1 / 11 §8.1 require the listening socket to be `SOCK_CLOEXEC`. Asio
    // does not set it, so a forked tool/PTY child could inherit the listener;
    // the orphaned child then keeps `host.sock` connectable after the daemon
    // dies and the replacement daemon's stale-socket probe reports a live peer
    // (AlreadyRunning -> WorkspaceBusy) instead of replacing the socket.
    if (const int listener_fd = acceptor_.native_handle(); listener_fd >= 0) {
        const int descriptor_flags = ::fcntl(listener_fd, F_GETFD);
        if (descriptor_flags >= 0) {
            (void)::fcntl(listener_fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
        }
    }
    acceptor_.bind(asio::local::stream_protocol::endpoint(socket_path_), error);
    if (error) {
        throw std::runtime_error("transport: bind " + socket_path_ + ": " + error.message());
    }
    ::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR);
    acceptor_.listen(asio::socket_base::max_listen_connections, error);
    if (error) {
        throw std::runtime_error("transport: listen: " + error.message());
    }

    own_inode_.reset();
    if (::lstat(socket_path_.c_str(), &status) == 0) {
        own_inode_ = status.st_ino;
    }

    do_accept();
    {
        std::lock_guard lock(post_mutex_);
        thread_ = std::thread([this] {
            if (on_runner_start_) {
                on_runner_start_();
            }
            io_->run();
        });
        started_.store(true);
    }
}

void TransportServer::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    {
        std::lock_guard lock(post_mutex_);
        if (started_.load()) {
            asio::post(*io_, [this] {
                std::error_code error;
                acceptor_.close(error);
                const std::vector<std::shared_ptr<SocketSession>> snapshot(sessions_.begin(),
                                                                           sessions_.end());
                for (const auto& session : snapshot) {
                    session->close_from_owner();
                }
                // Outstanding broker/keepalive timers are not owned here, so the
                // io_context must be stopped explicitly or `run()` would block
                // shutdown until the longest deadline (errata §11.3).
                io_->stop();
            });
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    sessions_.clear();
    if (started_.load()) {
        if (own_inode_.has_value()) {
            struct stat status {};
            if (::lstat(socket_path_.c_str(), &status) == 0 && status.st_ino == *own_inode_) {
                ::unlink(socket_path_.c_str());
            }
        }
        started_.store(false);
    }
}

void TransportServer::do_accept() {
    acceptor_.async_accept([this](const std::error_code& error,
                                  asio::local::stream_protocol::socket socket) {
        if (!error) {
            auto session = std::make_shared<SocketSession>(
                *io_, server_, std::move(socket), limits_, [this](SocketSession* closed) {
                    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
                        if (it->get() == closed) {
                            sessions_.erase(it);
                            break;
                        }
                    }
                });
            sessions_.insert(session);
            session->start();
        }
        if (!stopping_.load()) {
            do_accept();
        }
    });
}

} // namespace ymh::protocol
