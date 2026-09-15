#include "ymh/transport/transport_server.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
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
        if (!writing_) {
            write_next();
        }
    }

    void write_next() {
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        const auto self = shared_from_this();
        asio::async_write(
            socket_, asio::buffer(write_queue_.front()),
            [self](const std::error_code& error, std::size_t count) {
                if (error) {
                    self->shutdown();
                    return;
                }
                self->write_queue_.pop_front();
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
    std::function<void(SocketSession*)>  on_close_;
    ClientId                             id_{};
    std::uint32_t                        peer_uid_{0};
    std::int32_t                         peer_pid_{0};
    bool                                 writing_{false};
    bool                                 closed_{false};
};

TransportServer::TransportServer(ProtocolServer& server, std::string socket_path,
                                 TransportLimits limits)
    : server_(server),
      socket_path_(std::move(socket_path)),
      limits_(limits),
      acceptor_(io_) {}

TransportServer::~TransportServer() { stop(); }

void TransportServer::start() {
    std::error_code error;
    std::filesystem::remove(socket_path_, error);

    acceptor_.open(asio::local::stream_protocol(), error);
    if (error) {
        throw std::runtime_error("transport: open acceptor: " + error.message());
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

    do_accept();
    thread_ = std::thread([this] { io_.run(); });
    started_ = true;
}

void TransportServer::stop() {
    if (stopping_.exchange(true)) {
        return;
    }
    asio::post(io_, [this] {
        std::error_code error;
        acceptor_.close(error);
        const std::vector<std::shared_ptr<SocketSession>> snapshot(sessions_.begin(),
                                                                   sessions_.end());
        for (const auto& session : snapshot) {
            session->close_from_owner();
        }
    });
    if (thread_.joinable()) {
        thread_.join();
    }
    sessions_.clear();
    if (started_) {
        std::error_code error;
        std::filesystem::remove(socket_path_, error);
        started_ = false;
    }
}

void TransportServer::do_accept() {
    acceptor_.async_accept([this](const std::error_code& error,
                                  asio::local::stream_protocol::socket socket) {
        if (!error) {
            auto session = std::make_shared<SocketSession>(
                io_, server_, std::move(socket), limits_, [this](SocketSession* closed) {
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
