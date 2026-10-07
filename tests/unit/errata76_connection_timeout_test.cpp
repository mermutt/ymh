#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/short_temp.hpp"
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ymh;

int make_listener(const std::string& path, int backlog) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, backlog) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int connect_nonblocking(const std::string& path, int* error_out) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        *error_out = errno;
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    errno = 0;
    const int result = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    *error_out = errno;
    if (result != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::vector<int> fill_backlog(const std::string& path, bool* full) {
    std::vector<int> held;
    *full = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        int error = 0;
        const int fd = connect_nonblocking(path, &error);
        if (fd >= 0) {
            held.push_back(fd);
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS) {
            *full = true;
        }
        break;
    }
    return held;
}

void close_all(std::vector<int>& fds) {
    for (const int fd : fds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    fds.clear();
}

void ensure_ymh_dir(const ymh::test::ShortTempRoot& root) {
    std::filesystem::create_directories(root.path() / ".ymh");
}

std::string socket_path_of(const ymh::test::ShortTempRoot& root) {
    return (root.path() / ".ymh" / "host.sock").string();
}

protocol::ClientInstanceId probe_instance() {
    return protocol::ClientInstanceId{"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"};
}

// 76-I1 / 76-F1: a full accept backlog must return a bounded connect failure
// (never a dead "successful" socket), and an exhausted deadline must surface as
// a timeout rather than a block.
TEST(Err76, ConnectTimesOutOnFullBacklog) {
    ymh::test::ShortTempRoot root("ymh-e76-backlog");
    ensure_ymh_dir(root);
    const std::string path = socket_path_of(root);
    const int listener = make_listener(path, 1);
    ASSERT_GE(listener, 0);

    bool full = false;
    std::vector<int> held = fill_backlog(path, &full);
    ASSERT_TRUE(full) << "could not fill the listen backlog";

    protocol::HostConnection connection;
    const auto started = std::chrono::steady_clock::now();
    std::string message;
    try {
        connection.connect(path, 200ms);
        FAIL() << "connect unexpectedly succeeded on a full backlog";
    } catch (const std::exception& error) {
        message = error.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_NE(message.find("connect:"), std::string::npos) << message;
    EXPECT_LT(elapsed, 2s);

    protocol::HostConnection exhausted;
    std::string exhausted_message;
    try {
        exhausted.connect(path, 0ms);
        FAIL() << "connect unexpectedly succeeded with an exhausted deadline";
    } catch (const std::exception& error) {
        exhausted_message = error.what();
    }
    EXPECT_NE(exhausted_message.find("timed out"), std::string::npos) << exhausted_message;

    close_all(held);
    ::close(listener);
}

// 76-I3 / 76-F3: a failed connect must surface the OS reason and never be
// reported as success or masked by a timeout.
TEST(Err76, ConnectReadsSoError) {
    {
        ymh::test::ShortTempRoot root("ymh-e76-refused");
        ensure_ymh_dir(root);
        const std::string path = socket_path_of(root);
        const int bound = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_GE(bound, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        ASSERT_EQ(::bind(bound, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
        ::close(bound);

        protocol::HostConnection connection;
        std::string message;
        try {
            connection.connect(path, 200ms);
            FAIL() << "connect unexpectedly succeeded without a listener";
        } catch (const std::exception& error) {
            message = error.what();
        }
        EXPECT_NE(message.find("connect:"), std::string::npos) << message;
        EXPECT_EQ(message.find("timed out"), std::string::npos) << message;
    }

    {
        ymh::test::ShortTempRoot root("ymh-e76-soerr");
        ensure_ymh_dir(root);
        const std::string path = socket_path_of(root);
        const int listener = make_listener(path, 1);
        ASSERT_GE(listener, 0);
        bool full = false;
        std::vector<int> held = fill_backlog(path, &full);
        ASSERT_TRUE(full) << "could not fill the listen backlog";

        protocol::HostConnection connection;
        std::string message;
        try {
            connection.connect(path, 200ms);
            FAIL() << "connect unexpectedly succeeded on a failed backlog";
        } catch (const std::exception& error) {
            message = error.what();
        }
        EXPECT_NE(message.find("connect:"), std::string::npos) << message;
        EXPECT_EQ(message.find("timed out"), std::string::npos) << message;

        close_all(held);
        ::close(listener);
    }
}

// 76-I2 / 76-F2: the handshake request has a deadline set before its first byte
// and returns within it when the peer never answers.
TEST(Err76, SendTimesOutBeforeHello) {
    ymh::test::ShortTempRoot root("ymh-e76-hello");
    ensure_ymh_dir(root);
    const std::string path = socket_path_of(root);
    const int listener = make_listener(path, 8);
    ASSERT_GE(listener, 0);

    std::thread peer([listener] {
        const int accepted = ::accept(listener, nullptr, nullptr);
        if (accepted >= 0) {
            std::this_thread::sleep_for(500ms);
            ::close(accepted);
        }
    });

    protocol::HostConnection connection;
    ASSERT_NO_THROW(connection.connect(path, 1s));
    const auto started = std::chrono::steady_clock::now();
    std::string message;
    try {
        static_cast<void>(connection.handshake(protocol::ServerProfile::Interactive, probe_instance(), 200ms));
        FAIL() << "handshake unexpectedly succeeded with a silent peer";
    } catch (const std::exception& error) {
        message = error.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_NE(message.find("timed out"), std::string::npos) << message;
    EXPECT_LT(elapsed, 2s);

    peer.join();
    ::close(listener);
}

// 76-I2 / 76-F2: a large write to a peer that never drains must hit the write
// deadline instead of blocking forever.
TEST(Err76, WriteDeadlineBounds) {
    ymh::test::ShortTempRoot root("ymh-e76-write");
    ensure_ymh_dir(root);
    const std::string path = socket_path_of(root);
    const int listener = make_listener(path, 8);
    ASSERT_GE(listener, 0);

    std::thread peer([listener] {
        const int accepted = ::accept(listener, nullptr, nullptr);
        if (accepted >= 0) {
            std::this_thread::sleep_for(1s);
            ::close(accepted);
        }
    });

    protocol::HostConnection connection;
    ASSERT_NO_THROW(connection.connect(path, 1s));
    nlohmann::json params;
    params["blob"] = std::string(1024u * 1024u, 'x');
    const auto started = std::chrono::steady_clock::now();
    std::string message;
    try {
        static_cast<void>(connection.request(protocol::method::kHostPing, std::move(params), 200ms));
        FAIL() << "request unexpectedly completed with a non-reading peer";
    } catch (const std::exception& error) {
        message = error.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_NE(message.find("timed out"), std::string::npos) << message;
    EXPECT_LT(elapsed, 3s);

    peer.join();
    ::close(listener);
}

// 76-I14 / 76-D12: a response delivered in two chunks across separate reads must
// be reassembled; the non-blocking receive path must never surface a spurious
// "read: Resource temporarily unavailable".
TEST(Err76, ReadEagainOnSpuriousWakeup) {
    ymh::test::ShortTempRoot root("ymh-e76-eagain");
    ensure_ymh_dir(root);
    const std::string path = socket_path_of(root);
    const int listener = make_listener(path, 8);
    ASSERT_GE(listener, 0);

    const protocol::TransportLimits limits;

    std::thread peer([listener, limits] {
        const int accepted = ::accept(listener, nullptr, nullptr);
        if (accepted < 0) {
            return;
        }
        std::string buffer;
        char         scratch[4096];
        protocol::RequestId id;
        bool parsed = false;
        while (!parsed) {
            const ssize_t count = ::read(accepted, scratch, sizeof(scratch));
            if (count <= 0) {
                ::close(accepted);
                return;
            }
            buffer.append(scratch, static_cast<std::size_t>(count));
            std::string frame;
            if (protocol::FrameCodec::decode_step(buffer, limits.max_frame_bytes, frame) ==
                protocol::DecodeStatus::Ok) {
                const protocol::Message message = protocol::parse_message(frame);
                if (const auto* request = std::get_if<protocol::Request>(&message)) {
                    id = request->id;
                    parsed = true;
                }
            }
        }
        const nlohmann::json result{{"server_time_ms", 7}};
        const std::string encoded =
            protocol::FrameCodec::encode(protocol::encode(protocol::Response{id, result}).dump(),
                                         limits.max_frame_bytes);
        // Split the frame so the client observes a readable wakeup, consumes a
        // partial frame, and must retry within the deadline.
        const std::size_t split = encoded.size() / 2;
        if (::send(accepted, encoded.data(), split, MSG_NOSIGNAL) < 0) {
            ::close(accepted);
            return;
        }
        std::this_thread::sleep_for(50ms);
        if (::send(accepted, encoded.data() + split, encoded.size() - split, MSG_NOSIGNAL) < 0) {
            ::close(accepted);
            return;
        }
        std::this_thread::sleep_for(100ms);
        ::close(accepted);
    });

    protocol::HostConnection connection;
    ASSERT_NO_THROW(connection.connect(path, 1s));
    std::string message;
    nlohmann::json result;
    try {
        result = connection.request(protocol::method::kHostPing, nlohmann::json::object(), 2s);
    } catch (const std::exception& error) {
        message = error.what();
    }
    EXPECT_TRUE(message.empty()) << message;
    EXPECT_EQ(result.value("server_time_ms", 0), 7);

    peer.join();
    ::close(listener);
}

// 76-I14 / 76-D12 / 76-F16: a peer that writes one complete frame then closes
// may report POLLIN | POLLHUP in a single poll; the final frame must be
// delivered, not lost.
TEST(Err76, ResponseThenClose) {
    ymh::test::ShortTempRoot root("ymh-e76-close");
    ensure_ymh_dir(root);
    const std::string path = socket_path_of(root);
    const int listener = make_listener(path, 8);
    ASSERT_GE(listener, 0);

    const protocol::TransportLimits limits;

    std::thread peer([listener, limits] {
        const int accepted = ::accept(listener, nullptr, nullptr);
        if (accepted < 0) {
            return;
        }
        std::string buffer;
        char         scratch[4096];
        protocol::RequestId id;
        bool parsed = false;
        while (!parsed) {
            const ssize_t count = ::read(accepted, scratch, sizeof(scratch));
            if (count <= 0) {
                ::close(accepted);
                return;
            }
            buffer.append(scratch, static_cast<std::size_t>(count));
            std::string frame;
            if (protocol::FrameCodec::decode_step(buffer, limits.max_frame_bytes, frame) ==
                protocol::DecodeStatus::Ok) {
                const protocol::Message message = protocol::parse_message(frame);
                if (const auto* request = std::get_if<protocol::Request>(&message)) {
                    id = request->id;
                    parsed = true;
                }
            }
        }
        const nlohmann::json result{{"server_time_ms", 9}};
        const std::string encoded =
            protocol::FrameCodec::encode(protocol::encode(protocol::Response{id, result}).dump(),
                                         limits.max_frame_bytes);
        static_cast<void>(::send(accepted, encoded.data(), encoded.size(), MSG_NOSIGNAL));
        ::close(accepted);
    });

    protocol::HostConnection connection;
    ASSERT_NO_THROW(connection.connect(path, 1s));
    std::string message;
    nlohmann::json result;
    try {
        result = connection.request(protocol::method::kHostPing, nlohmann::json::object(), 2s);
    } catch (const std::exception& error) {
        message = error.what();
    }
    EXPECT_TRUE(message.empty()) << message;
    EXPECT_EQ(result.value("server_time_ms", 0), 9);

    peer.join();
    ::close(listener);
}

} // namespace
