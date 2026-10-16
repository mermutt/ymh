// Spec 87 (daemon-ownership-integrity errata): the supervisor-pump link-loss
// classification (87-D4) and the UI create-notice guard (87-F12). The pump tests
// drive a real `SupervisorConnection` against a scripted JSON-RPC peer.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "support/short_temp.hpp"
#include "ymh/config/config.hpp"
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_connection.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace std::chrono_literals;

constexpr std::string_view kWorkspace = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view kBoot = "22222222-2222-4222-8222-222222222222";

// A minimal JSON-RPC peer that answers `host.hello`, answers every other request
// unless `hang_on` names it, and can be dropped on demand.
class ScriptedServer {
public:
    explicit ScriptedServer(const std::filesystem::path& socket_path) : path_(socket_path) {
        ::unlink(path_.c_str());
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string text = path_.string();
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listen_fd_, 4) != 0) {
            throw std::runtime_error("scripted server bind/listen failed");
        }
        thread_ = std::thread([this] { accept_loop(); });
    }

    ~ScriptedServer() { stop(); }

    ScriptedServer(const ScriptedServer&) = delete;
    ScriptedServer& operator=(const ScriptedServer&) = delete;

    void hang_on(std::string method) {
        const std::lock_guard lock(request_mutex_);
        hang_method_ = std::move(method);
    }

    [[nodiscard]] int hello_count() const { return hello_count_.load(); }

    bool wait_for_request(const std::string& method, std::chrono::milliseconds timeout) {
        std::unique_lock lock(request_mutex_);
        return request_cv_.wait_for(lock, timeout, [&] {
            return request_counts_.find(method) != request_counts_.end();
        });
    }

    void drop_client() {
        std::vector<int> clients;
        {
            const std::lock_guard lock(clients_mutex_);
            clients = clients_;
        }
        for (const int fd : clients) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        drop_client();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        for (std::thread& handler : handlers_) {
            if (handler.joinable()) {
                handler.join();
            }
        }
        handlers_.clear();
        ::unlink(path_.c_str());
    }

private:
    void accept_loop() {
        while (running_.load()) {
            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            {
                const std::lock_guard lock(clients_mutex_);
                clients_.push_back(client);
            }
            handlers_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(int client) {
        std::string buffer;
        while (running_.load()) {
            char chunk[65536];
            const ssize_t count = ::read(client, chunk, sizeof(chunk));
            if (count <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(count));
            std::string frame;
            while (true) {
                protocol::DecodeStatus status = protocol::DecodeStatus::NeedMore;
                try {
                    status = protocol::FrameCodec::decode_step(
                        buffer, protocol::TransportLimits{}.max_frame_bytes, frame);
                } catch (const std::exception&) {
                    status = protocol::DecodeStatus::NeedMore;
                }
                if (status != protocol::DecodeStatus::Ok) {
                    break;
                }
                handle(frame, client);
            }
        }
        ::close(client);
        const std::lock_guard lock(clients_mutex_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    }

    void handle(const std::string& frame, int client) {
        protocol::Message message = protocol::parse_message(frame);
        const auto* request = std::get_if<protocol::Request>(&message);
        if (request == nullptr) {
            return;
        }
        {
            const std::lock_guard lock(request_mutex_);
            ++request_counts_[request->method];
            request_cv_.notify_all();
            if (hang_method_.has_value() && *hang_method_ == request->method) {
                return;
            }
        }
        if (request->method == protocol::method::kHostHello) {
            hello_count_.fetch_add(1);
            protocol::HelloResult hello;
            hello.workspace.value = std::string{kWorkspace};
            hello.boot_id.value = std::string{kBoot};
            hello.client_id = protocol::ClientId{1};
            nlohmann::json body;
            protocol::to_json(body, hello);
            respond(request->id, body, client);
            return;
        }
        respond(request->id, nlohmann::json::object(), client);
    }

    void respond(const protocol::RequestId& id, nlohmann::json result, int client) {
        const nlohmann::json message = protocol::encode(
            protocol::Response{id, std::move(result)});
        send_to(client, message);
    }

    static void send_to(int fd, const nlohmann::json& message) {
        const std::string frame = protocol::FrameCodec::encode(
            message.dump(), protocol::TransportLimits{}.max_frame_bytes);
        std::size_t written = 0;
        while (written < frame.size()) {
            const ssize_t count =
                ::send(fd, frame.data() + written, frame.size() - written, MSG_NOSIGNAL);
            if (count <= 0) {
                return;
            }
            written += static_cast<std::size_t>(count);
        }
    }

    std::filesystem::path     path_;
    int                       listen_fd_{-1};
    std::atomic<bool>         running_{true};
    std::atomic<int>          hello_count_{0};
    std::vector<std::thread>  handlers_;
    std::mutex                clients_mutex_;
    std::vector<int>          clients_;
    std::mutex                request_mutex_;
    std::condition_variable   request_cv_;
    std::map<std::string, int> request_counts_;
    std::optional<std::string> hang_method_;
    std::thread               thread_;
};

SupervisorConnectionConfig config_for(const std::filesystem::path& socket_path) {
    SupervisorConnectionConfig config;
    config.socket_path = socket_path.string();
    config.workspace = WorkspaceId{std::string{kWorkspace}};
    config.expected_boot_id = std::string{kBoot};
    config.client_instance = protocol::ClientInstanceId{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
    config.role = protocol::ClientRole::Supervisor;
    config.profile = protocol::ServerProfile::Interactive;
    config.poll_interval = 10ms;
    config.reconnect_backoff = 50ms;
    config.request_timeout = 300ms;
    config.ping_interval = 60s;
    return config;
}

TEST(SupervisorConn87, TimeoutIsNotLinkLoss) {
    test::ShortTempRoot root("ymh-e87-timeout");
    std::filesystem::create_directories(root.host_socket().parent_path());
    ScriptedServer server(root.host_socket());

    SupervisorConnection connection(config_for(root.host_socket()), SupervisorSink{});
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));

    server.hang_on(std::string(protocol::method::kSessionList));
    std::atomic<bool> replied{false};
    std::string error_text;
    connection.submit(std::string(protocol::method::kSessionList), nlohmann::json::object(),
                      [&](SupervisorReply reply) {
                          error_text = reply.error;
                          replied.store(true);
                      });
    ASSERT_TRUE(connection.waitUntil([&replied] { return replied.load(); }, 3s));

    EXPECT_EQ(error_text, "request timed out");
    EXPECT_EQ(connection.state(), SupervisorLinkState::Attached);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(server.hello_count(), 1) << "a timeout must not reconnect";
    EXPECT_EQ(connection.state(), SupervisorLinkState::Attached);
    connection.stop();
}

TEST(SupervisorConn87, EpipeBecomesDeadAndReconnects) {
    test::ShortTempRoot root("ymh-e87-epipe");
    std::filesystem::create_directories(root.host_socket().parent_path());
    ScriptedServer server(root.host_socket());

    std::mutex states_mutex;
    std::vector<SupervisorLinkState> states;
    SupervisorSink sink;
    sink.on_state = [&](SupervisorLinkState state, const std::string&) {
        const std::lock_guard lock(states_mutex);
        states.push_back(state);
    };

    SupervisorConnection connection(config_for(root.host_socket()), std::move(sink));
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));

    server.hang_on(std::string(protocol::method::kSessionList));
    std::atomic<bool> replied{false};
    std::string error_text;
    connection.submit(std::string(protocol::method::kSessionList), nlohmann::json::object(),
                      [&](SupervisorReply reply) {
                          error_text = reply.error;
                          replied.store(true);
                      });
    ASSERT_TRUE(server.wait_for_request(std::string(protocol::method::kSessionList), 3s));
    server.drop_client();

    ASSERT_TRUE(connection.waitUntil([&replied] { return replied.load(); }, 5s));
    EXPECT_EQ(error_text, "connection lost");

    // The pump re-attaches to the still-listening peer.
    ASSERT_TRUE(connection.waitUntil([&] { return server.hello_count() >= 2; }, 5s));
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 5s));
    {
        const std::lock_guard lock(states_mutex);
        const auto dead = std::find(states.begin(), states.end(), SupervisorLinkState::Dead);
        EXPECT_NE(dead, states.end()) << "a link loss must transition through Dead";
    }
    connection.stop();
}

// ---------------------------------------------------------------------------
// 87-F12: the surfacing path never renders the raw transport string.
// ---------------------------------------------------------------------------

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata87-ui"},
                          protocol::ClientRole::Supervisor};
}

ModelSettings model_settings(const std::string& endpoint, const std::string& model) {
    ModelSettings settings;
    settings.endpoint = endpoint;
    settings.model = model;
    return settings;
}

Config config_with_models() {
    Config config;
    config.llm.endpoints["ds"] = EndpointSettings{};
    config.llm.endpoints["ds"].base_url = "https://test.example/v1";
    config.llm.models["balanced"] = model_settings("ds", "m-a");
    config.llm.active_model = "balanced";
    return config;
}

WorkspaceModel live_workspace(const WorkspaceId& id, const std::string& title) {
    WorkspaceModel workspace;
    workspace.id = id;
    workspace.title = title;
    workspace.cwd = "/" + id.value;
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live = true;
    return workspace;
}

SupervisorWorkspace spec_for(const WorkspaceId& id) {
    SupervisorWorkspace spec;
    spec.id = id;
    spec.cwd = "/" + id.value;
    spec.title = id.value;
    spec.socket_path = "/tmp/" + id.value + "/host.sock";
    spec.boot_id = "boot-a";
    return spec;
}

TEST(Ui87, NoRawTransportStringInCreateNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    options.config = config_with_models();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->on_scan({spec_for(workspace)});
    harness->drain_actions();

    harness->install_session_list_reply(nlohmann::json::array(), 0);
    // The classified token a link-loss `session.create` now carries (87-D4).
    harness->install_method_error_reply(std::string(protocol::method::kSessionCreate), -32603,
                                        "connection lost");
    harness->on_link_state(workspace, SupervisorLinkState::Attached, "test");
    harness->drain_actions();
    harness->drain_actions();
    harness->drain_actions();

    ASSERT_FALSE(harness->model().notices.empty());
    const std::string text = harness->model().notices.back().text;
    EXPECT_NE(text.find("cannot create session"), std::string::npos);
    EXPECT_EQ(text.find("host connection:"), std::string::npos);
    EXPECT_EQ(text.find("Broken pipe"), std::string::npos);
}

} // namespace
