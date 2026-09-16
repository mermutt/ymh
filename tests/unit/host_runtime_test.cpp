#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/turn_executor.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/host/host_runtime.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/permission/permission_transport.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol_server.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr std::size_t kMaxFrame = 8u * 1024u * 1024u;

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

class FakePermissionTransport final : public PermissionTransport {
public:
    bool broadcast_permission_request(protocol::PermissionRequest request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return false;
        }
        broadcasts_.push_back(std::move(request));
        return true;
    }

    bool schedule_after(std::chrono::milliseconds, std::function<void()>) override {
        return running_;
    }

    void set_running(bool running) {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = running;
    }

    [[nodiscard]] std::size_t broadcastCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return broadcasts_.size();
    }

    [[nodiscard]] std::vector<protocol::PermissionRequest> broadcasts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return broadcasts_;
    }

private:
    mutable std::mutex                                mutex_;
    bool                                              running_ = true;
    std::vector<protocol::PermissionRequest>          broadcasts_;
};

PermissionRequest ask_request(const SessionId& session, std::string tool = "write_file") {
    PermissionRequest request;
    request.call = "call-1";
    request.session = session;
    request.tool = std::move(tool);
    request.arguments = nlohmann::json::object();
    request.root = "/tmp/ws";
    request.path = std::filesystem::path("src/x");
    request.sandbox = SandboxMode::Workspace;
    return request;
}

std::size_t count_type(const EventRange& events, EventType type) {
    std::size_t count = 0;
    for (const EventRecord& record : events) {
        if (record.event.type == type) {
            ++count;
        }
    }
    return count;
}

struct RpcFailure {
    int         code = 0;
    std::string kind;
};

template <class Fn>
RpcFailure expect_rpc(Fn&& fn) {
    try {
        fn();
    } catch (const protocol::RpcException& error) {
        return RpcFailure{error.code(), error.data().value("kind", std::string{})};
    }
    ADD_FAILURE() << "expected an RpcException";
    return RpcFailure{};
}

class Bridge {
public:
    struct Peer {
        protocol::ClientId       id{};
        std::vector<std::string> frames;
        std::string              drop_reason;
    };

    explicit Bridge(const std::string& prefix, FakeScript script = FakeScript{})
        : workspace_(prefix), registry_dir_("host_runtime_registry_" + prefix) {
        root_ = std::filesystem::canonical(workspace_.path());

        RegistryConfig registry_config;
        registry_config.db_path = registry_dir_.path() / "registry.db";
        registry_config.lock_path = registry_dir_.path() / "registry.lock";
        registry_config.workspace_roots = {registry_dir_.path() / "no-such-root"};
        registry_config.lock_retry_budget = std::chrono::milliseconds{80};
        registry_config.lock_retry_interval = std::chrono::milliseconds{5};
        registry_ = WorkspaceRegistry::open(registry_config);
        record_ = registry_->registerWorkspace(root_, "test-ws");

        boot_id_ = BootId{"host-runtime-test-boot"};
        host_boot_id_ = HostBootId{"host-runtime-test-boot"};
        WorkspaceRuntimeOptions options;
        options.config = Config{};
        options.root = root_;
        options.boot_id = boot_id_;
        options.store_factory = [] { return std::make_unique<MemorySessionStore>(); };
        if (script.steps.empty()) {
            script.steps.push_back(text_step("hello"));
        }
        options.provider_factory =
            [script](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            return std::make_unique<FakeLLM>(script);
        };
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(std::move(options));
        if (!created.has_value()) {
            throw std::runtime_error("runtime create failed: " + created.error().detail);
        }
        runtime_ = std::move(*created);

        permission_transport_ = std::make_unique<FakePermissionTransport>();
        broker_ = std::make_unique<PermissionBroker>(runtime_->policy(), *permission_transport_,
                                                     PermissionConfig{});

        identity_.workspace = record_.id;
        identity_.boot_id = host_boot_id_;
        identity_.pid = 4242;
        identity_.socket_path = root_ / ".ymh" / "host.sock";

        host_ = std::make_unique<HostRuntime>(
            *runtime_, *registry_, identity_, turns_, *broker_,
            [this](const EventRecord& record) {
                record_forwarded(record);
                if (server_ != nullptr) {
                    server_->onEventCommitted(record);
                }
            });

        server_config_.uid = static_cast<std::uint32_t>(::getuid());
        server_config_.workspace = protocol::WorkspaceId{record_.id.value};
        server_config_.boot_id = protocol::HostBootId{host_boot_id_.value};
        server_config_.pid = 4242;
        server_ = std::make_unique<protocol::ProtocolServer>(*host_, server_config_);
        host_->attachServer(*server_);
    }

    ~Bridge() {
        turns_.drain(std::chrono::milliseconds{2000});
    }

    [[nodiscard]] WorkspaceRuntime&       runtime() { return *runtime_; }
    [[nodiscard]] WorkspaceRegistry&      registry() { return *registry_; }
    [[nodiscard]] HostRuntime&            host() { return *host_; }
    [[nodiscard]] PermissionBroker&       broker() { return *broker_; }
    [[nodiscard]] protocol::ProtocolServer& server() { return *server_; }
    [[nodiscard]] FakePermissionTransport&  permission_transport() { return *permission_transport_; }
    [[nodiscard]] const HostIdentity&     identity() const { return identity_; }
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    void clear_forwarded() {
        std::lock_guard<std::mutex> lock(forwarded_mutex_);
        forwarded_.clear();
        turn_ended_ = false;
    }

    [[nodiscard]] std::vector<EventRecord> forwarded() {
        std::lock_guard<std::mutex> lock(forwarded_mutex_);
        return forwarded_;
    }

    bool wait_for_turn_end(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(forwarded_mutex_);
        return forwarded_cv_.wait_for(lock, timeout, [this] { return turn_ended_; });
    }

    Sequence append_event(const SessionId& id, EventType type) {
        Session& session = runtime_->sessions().session(id);
        Event event;
        event.id = make_event_id();
        event.session_id = id;
        event.timestamp = std::chrono::system_clock::now();
        event.type = type;
        event.payload = nlohmann::json::object();
        return session.appendEvent(event);
    }

    void emit_live(const SessionId& id, EventType type) {
        Session& session = runtime_->sessions().session(id);
        Event event;
        event.id = make_event_id();
        event.session_id = id;
        event.timestamp = std::chrono::system_clock::now();
        event.type = type;
        event.payload = nlohmann::json::object();
        session.emit(event);
    }

    Peer* open_peer() {
        peers_.push_back(std::make_unique<Peer>());
        Peer* peer = peers_.back().get();
        protocol::ProtocolServer* engine = server_.get();
        peer->id = server_->openConnection(
            static_cast<std::uint32_t>(::getuid()), 4321,
            [peer, engine](protocol::ClientId id, std::string frame) {
                peer->frames.push_back(std::move(frame));
                engine->onFrameWritten(id, peer->frames.back().size());
            },
            [peer](protocol::ClientId, std::string reason) { peer->drop_reason = std::move(reason); });
        return peer;
    }

    void send_frame(Peer& peer, const nlohmann::json& message) {
        const std::string frame = protocol::FrameCodec::encode(message.dump(), kMaxFrame);
        server_->receiveBytes(peer.id, frame);
    }

    std::vector<nlohmann::json> drain(Peer& peer) {
        std::string buffer;
        for (const std::string& frame : peer.frames) {
            buffer += frame;
        }
        peer.frames.clear();
        std::vector<nlohmann::json> messages;
        for (const std::string& body : protocol::FrameCodec::decode(buffer, kMaxFrame)) {
            messages.push_back(nlohmann::json::parse(body));
        }
        return messages;
    }

    nlohmann::json request(Peer& peer, std::int64_t id, std::string_view method,
                           nlohmann::json params = nlohmann::json::object()) {
        send_frame(peer, protocol::encode(protocol::Request{
                             protocol::RequestId{id}, std::string{method}, std::move(params)}));
        for (const nlohmann::json& message : drain(peer)) {
            if (message.value("id", nlohmann::json{}) == id && message.contains("result")) {
                return message.at("result");
            }
        }
        return nlohmann::json{};
    }

    void hello(Peer& peer, protocol::ServerProfile profile = protocol::ServerProfile::Interactive) {
        nlohmann::json params;
        protocol::to_json(params,
                          protocol::HelloParams{protocol::kProtocolVersion, profile,
                                                protocol::ClientInstanceId{
                                                    "dddddddd-dddd-4ddd-8ddd-dddddddddddd"},
                                                std::nullopt});
        request(peer, 1, protocol::method::kHostHello, params);
    }

private:
    void record_forwarded(const EventRecord& record) {
        std::lock_guard<std::mutex> lock(forwarded_mutex_);
        forwarded_.push_back(record);
        if (record.event.type == EventType::TurnEnded) {
            turn_ended_ = true;
        }
        forwarded_cv_.notify_all();
    }

    TempWorkspace                            workspace_;
    TempWorkspace                            registry_dir_;
    std::filesystem::path                    root_;
    std::unique_ptr<WorkspaceRegistry>       registry_;
    WorkspaceRecord                          record_;
    BootId                                   boot_id_;
    HostBootId                               host_boot_id_;
    HostIdentity                             identity_;
    std::unique_ptr<WorkspaceRuntime>        runtime_;
    std::unique_ptr<FakePermissionTransport> permission_transport_;
    std::unique_ptr<PermissionBroker>        broker_;
    TurnExecutor                             turns_{2, 8};
    std::unique_ptr<protocol::ProtocolServer> server_;
    protocol::ProtocolServerConfig            server_config_;
    std::unique_ptr<HostRuntime>              host_;

    std::mutex                    forwarded_mutex_;
    std::condition_variable       forwarded_cv_;
    std::vector<EventRecord>      forwarded_;
    bool                          turn_ended_ = false;
    std::deque<std::unique_ptr<Peer>> peers_;
};

class HostRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggingOptions options;
        options.level = LogLevel::Error;
        init_logging(options);
    }
};

static_assert(std::is_constructible_v<HostRuntime, WorkspaceRuntime&, WorkspaceRegistry&,
                                      HostIdentity, protocol::ProtocolServer&, TurnExecutor&,
                                      PermissionBroker&, HostRuntime::EventForwarder>);

TEST_F(HostRuntimeTest, ErrorTableMapsWorkspaceRows) {
    const HostRuntime::WireError busy =
        HostRuntime::map_workspace_error(WorkspaceRuntimeErrorCode::WorkspaceBusy);
    EXPECT_EQ(busy.code, protocol::code_value(protocol::RpcCode::InvalidRequest));
    EXPECT_EQ(busy.kind, "AlreadyRunning");

    EXPECT_EQ(HostRuntime::map_workspace_error(WorkspaceRuntimeErrorCode::WorkspaceMissing).code,
              protocol::code_value(protocol::AppCode::UnknownWorkspace));
    EXPECT_EQ(HostRuntime::map_workspace_error(WorkspaceRuntimeErrorCode::StoreUnavailable).code,
              protocol::code_value(protocol::AppCode::StoreUnavailable));
    EXPECT_EQ(HostRuntime::map_workspace_error(WorkspaceRuntimeErrorCode::ProviderSetupFailed).code,
              protocol::code_value(protocol::RpcCode::InternalError));
    EXPECT_EQ(HostRuntime::map_workspace_error(WorkspaceRuntimeErrorCode::Internal).code,
              protocol::code_value(protocol::RpcCode::InternalError));
}

TEST_F(HostRuntimeTest, ErrorTableMapsAgentRows) {
    auto row = [](AgentErrorCode code) {
        AgentError error;
        error.code = code;
        return HostRuntime::map_agent_error(error);
    };
    EXPECT_EQ(row(AgentErrorCode::UnknownSession).code,
              protocol::code_value(protocol::AppCode::UnknownSession));
    EXPECT_EQ(row(AgentErrorCode::LeaseHeldByOther).code,
              protocol::code_value(protocol::AppCode::LeaseLost));
    EXPECT_EQ(row(AgentErrorCode::LeaseLost).code,
              protocol::code_value(protocol::AppCode::LeaseLost));
    EXPECT_EQ(row(AgentErrorCode::StoreUnavailable).code,
              protocol::code_value(protocol::AppCode::StoreUnavailable));
    EXPECT_EQ(row(AgentErrorCode::InboxFull).code,
              protocol::code_value(protocol::RpcCode::InternalError));
    EXPECT_EQ(row(AgentErrorCode::InboxFull).kind, "InboxFull");
    EXPECT_EQ(row(AgentErrorCode::AgentDisposed).code,
              protocol::code_value(protocol::AppCode::SessionNotActive));
    EXPECT_EQ(row(AgentErrorCode::AgentDisposed).kind, "AgentDisposed");
    EXPECT_EQ(row(AgentErrorCode::StepLimitExceeded).kind, "StepLimitExceeded");
    EXPECT_EQ(row(AgentErrorCode::ContextAssemblyFailed).kind, "ContextAssemblyFailed");
    EXPECT_EQ(row(AgentErrorCode::CompactionFailed).kind, "CompactionFailed");
    EXPECT_EQ(row(AgentErrorCode::ProviderFailed).kind, "ProviderFailed");
    EXPECT_EQ(row(AgentErrorCode::Internal).code,
              protocol::code_value(protocol::RpcCode::InternalError));
}

TEST_F(HostRuntimeTest, ErrorTableMapsStoreAndRegistryRows) {
    const UnknownSession unknown("unknown");
    EXPECT_EQ(HostRuntime::map_store_error(unknown).code,
              protocol::code_value(protocol::AppCode::UnknownSession));
    const LeaseLost lease("lost");
    EXPECT_EQ(HostRuntime::map_store_error(lease).code,
              protocol::code_value(protocol::AppCode::LeaseLost));
    const InvalidForkBoundary boundary("boundary");
    EXPECT_EQ(HostRuntime::map_store_error(boundary).code,
              protocol::code_value(protocol::AppCode::InvalidForkBoundary));
    const DependentSessionError dependent("dependent");
    EXPECT_EQ(HostRuntime::map_store_error(dependent).code,
              protocol::code_value(protocol::AppCode::DependentSession));
    const PayloadTooLarge payload("large");
    EXPECT_EQ(HostRuntime::map_store_error(payload).code,
              protocol::code_value(protocol::AppCode::PayloadTooLarge));
    const CorruptionError corrupt("corrupt");
    EXPECT_EQ(HostRuntime::map_store_error(corrupt).code,
              protocol::code_value(protocol::AppCode::StoreUnavailable));
    const StoreError generic("generic");
    EXPECT_EQ(HostRuntime::map_store_error(generic).code,
              protocol::code_value(protocol::AppCode::StoreUnavailable));
    const std::runtime_error other("other");
    EXPECT_EQ(HostRuntime::map_store_error(other).code,
              protocol::code_value(protocol::RpcCode::InternalError));

    const RegistryError unknown_workspace(RegistryErrorCode::UnknownWorkspace, "x");
    EXPECT_EQ(HostRuntime::map_registry_error(unknown_workspace).code,
              protocol::code_value(protocol::AppCode::UnknownWorkspace));
    const RegistryError claimed(RegistryErrorCode::HostClaimed, "x");
    EXPECT_EQ(HostRuntime::map_registry_error(claimed).kind, "HostClaimed");
    const RegistryError mutation(RegistryErrorCode::MutationInProgress, "x");
    EXPECT_EQ(HostRuntime::map_registry_error(mutation).kind, "MutationInProgress");
    const RegistryError open(RegistryErrorCode::OpenFailed, "x");
    EXPECT_EQ(HostRuntime::map_registry_error(open).code,
              protocol::code_value(protocol::AppCode::RegistryUnavailable));
}

TEST_F(HostRuntimeTest, CreateResumeForkDeleteWriteJunction) {
    Bridge bridge("hr_lifecycle");

    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    ASSERT_FALSE(created.session.value.empty());
    ASSERT_EQ(bridge.registry().listSessions(bridge.identity().workspace).size(), 1u);
    EXPECT_EQ(bridge.registry().listSessions(bridge.identity().workspace)[0].sessionId.value,
              created.session.value);
    EXPECT_TRUE(bridge.host().sessionExists(created.session));

    const protocol::SessionResumed resumed = bridge.host().resumeSession(created.session);
    EXPECT_EQ(resumed.session.value, created.session.value);
    EXPECT_EQ(resumed.status, "Idle");

    const protocol::SessionCreated forked = bridge.host().forkSession(created.session, 1);
    EXPECT_NE(forked.session.value, created.session.value);
    EXPECT_EQ(bridge.registry().listSessions(bridge.identity().workspace).size(), 2u);

    bridge.host().deleteSession(forked.session);
    EXPECT_EQ(bridge.registry().listSessions(bridge.identity().workspace).size(), 1u);
    EXPECT_FALSE(bridge.host().sessionExists(forked.session));
}

TEST_F(HostRuntimeTest, ListSessionsUsesJunctionOrderAndFlagsStoreOnly) {
    Bridge bridge("hr_list");

    const protocol::SessionCreated first =
        bridge.host().createSession(nlohmann::json{{"title", "first"}});
    const protocol::SessionCreated second = bridge.host().createSession(nlohmann::json::object());
    const protocol::SessionCreated third = bridge.host().createSession(nlohmann::json::object());

    SessionOptions options;
    options.cwd = bridge.root();
    options.serverProfile = "interactive";
    options.model = "fake-model";
    options.title = "orphan";
    const SessionId orphan = bridge.runtime().sessions().createSession(options);

    const std::vector<protocol::SessionSummary> sessions = bridge.host().listSessions();
    ASSERT_EQ(sessions.size(), 4u);
    EXPECT_EQ(sessions[0].id.value, first.session.value);
    EXPECT_EQ(sessions[1].id.value, second.session.value);
    EXPECT_EQ(sessions[2].id.value, third.session.value);
    EXPECT_EQ(sessions[3].id.value, orphan.value);
    EXPECT_EQ(sessions[0].title, "first");

    const protocol::SessionDetail detail = bridge.host().showSession(first.session);
    EXPECT_EQ(detail.summary.id.value, first.session.value);
    EXPECT_GE(detail.event_count, 1u);
    EXPECT_TRUE(detail.header.is_object());
}

TEST_F(HostRuntimeTest, ReadEventsIsBoundedAndHeadSequence) {
    Bridge bridge("hr_bounded");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    for (int index = 0; index < 300; ++index) {
        bridge.append_event(created.session, EventType::TokenUsage);
    }
    EXPECT_EQ(bridge.host().headSequence(created.session), 301);

    EXPECT_EQ(bridge.host().readEvents(created.session, 0, 1000).size(), 256u);
    EXPECT_EQ(bridge.host().readEvents(created.session, 0, 0).size(), 1u);
    EXPECT_EQ(bridge.host().readEvents(created.session, 0, 3).size(), 3u);
    EXPECT_EQ(bridge.host().readEvents(created.session, 300, 256).size(), 1u);

    const RpcFailure missing = expect_rpc(
        [&] { bridge.host().readEvents(SessionId{"missing"}, 0, 1); });
    EXPECT_EQ(missing.code, protocol::code_value(protocol::AppCode::UnknownSession));
    EXPECT_EQ(missing.kind, "UnknownSession");
}

TEST_F(HostRuntimeTest, CursorRoundTripAndInvalid) {
    Bridge bridge("hr_cursor");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.append_event(created.session, EventType::TokenUsage);
    const Sequence head = bridge.host().headSequence(created.session);
    ASSERT_EQ(head, 2);

    const protocol::EventCursor cursor = bridge.host().cursorFor(created.session, head);
    EXPECT_EQ(cursor.value.find(created.session.value), std::string::npos);
    const std::optional<Sequence> resolved = bridge.host().resolveCursor(created.session, cursor);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, head);

    const std::optional<Sequence> clamped =
        bridge.host().resolveCursor(created.session,
                                    bridge.host().cursorFor(created.session, head + 100));
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(*clamped, head);

    EXPECT_FALSE(bridge.host().resolveCursor(created.session, protocol::EventCursor{"not*base64"})
                     .has_value());
    EXPECT_FALSE(bridge.host()
                     .resolveCursor(SessionId{"other-session"},
                                    bridge.host().cursorFor(created.session, head))
                     .has_value());

    bridge.host().deleteSession(created.session);
    EXPECT_FALSE(bridge.host().resolveCursor(created.session, cursor).has_value());
}

TEST_F(HostRuntimeTest, SingleForwarderPreservesOrderAndDropsLiveEvents) {
    Bridge bridge("hr_forward");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.clear_forwarded();

    bridge.append_event(created.session, EventType::TokenUsage);
    bridge.emit_live(created.session, EventType::AssistantChunk);
    bridge.append_event(created.session, EventType::TokenUsage);

    const std::vector<EventRecord> forwarded = bridge.forwarded();
    ASSERT_EQ(forwarded.size(), 2u);
    EXPECT_EQ(forwarded[0].event.type, EventType::TokenUsage);
    EXPECT_EQ(forwarded[1].event.type, EventType::TokenUsage);
    EXPECT_LT(forwarded[0].seq, forwarded[1].seq);
}

TEST_F(HostRuntimeTest, AgentPromptRunsOnExecutor) {
    Bridge bridge("hr_prompt");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.clear_forwarded();

    nlohmann::json message;
    message["role"] = "user";
    message["content"] = nlohmann::json::array(
        {nlohmann::json{{"kind", "text"}, {"text", "hi there"}}});
    bridge.host().agentPrompt(created.session, message);

    ASSERT_TRUE(bridge.wait_for_turn_end(10s));
    const EventRange events = bridge.runtime().store().read(created.session, 0);
    EXPECT_GE(count_type(events, EventType::AssistantMessage), 1u);
    EXPECT_GE(count_type(events, EventType::TurnEnded), 1u);
}

TEST_F(HostRuntimeTest, PermissionDecideFirstWins) {
    Bridge bridge("hr_permission");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.host().publishSubscriberCount(created.session, 1);

    std::future<PermissionOutcome> future =
        bridge.broker().resolve(ask_request(created.session), {});
    ASSERT_EQ(bridge.permission_transport().broadcastCount(), 1u);
    const std::string request_id = bridge.permission_transport().broadcasts().back().request_id;

    EXPECT_TRUE(bridge.host().decidePermission(request_id, protocol::PermissionAnswer::Allow,
                                               protocol::PermissionScope::Once));
    EXPECT_FALSE(bridge.host().decidePermission(request_id, protocol::PermissionAnswer::Deny,
                                                protocol::PermissionScope::Once));
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
}

TEST_F(HostRuntimeTest, UnknownIdsMapToTypedErrors) {
    Bridge bridge("hr_errors");

    const RpcFailure resume =
        expect_rpc([&] { bridge.host().resumeSession(SessionId{"missing"}); });
    EXPECT_EQ(resume.code, protocol::code_value(protocol::AppCode::UnknownSession));
    EXPECT_EQ(resume.kind, "UnknownSession");

    const RpcFailure show =
        expect_rpc([&] { bridge.host().showSession(SessionId{"missing"}); });
    EXPECT_EQ(show.kind, "UnknownSession");

    const RpcFailure workspace = expect_rpc([&] {
        bridge.host().showWorkspace(protocol::WorkspaceId{"00000000-0000-4000-8000-000000000000"});
    });
    EXPECT_EQ(workspace.code, protocol::code_value(protocol::AppCode::UnknownWorkspace));
    EXPECT_EQ(workspace.kind, "UnknownWorkspace");

    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    const RpcFailure fork =
        expect_rpc([&] { bridge.host().forkSession(created.session, 99); });
    EXPECT_EQ(fork.code, protocol::code_value(protocol::AppCode::InvalidForkBoundary));
    EXPECT_EQ(fork.kind, "InvalidForkBoundary");
}

TEST_F(HostRuntimeTest, HostStateShutdownAndWorkspaceViews) {
    Bridge bridge("hr_state");
    EXPECT_EQ(bridge.host().hostState(), protocol::HostState::Serving);

    bool        hook_called = false;
    std::string hook_reason;
    bridge.host().setShutdownHook([&](std::string reason) {
        hook_called = true;
        hook_reason = std::move(reason);
    });
    bridge.host().requestShutdown("test");
    EXPECT_EQ(bridge.host().hostState(), protocol::HostState::Draining);
    EXPECT_TRUE(hook_called);
    EXPECT_EQ(hook_reason, "test");

    const protocol::HostStatusInfo status = bridge.host().hostStatus();
    EXPECT_EQ(status.workspace.value, bridge.identity().workspace.value);
    EXPECT_EQ(status.pid, bridge.identity().pid);
    EXPECT_EQ(status.state, protocol::HostState::Draining);

    const std::vector<protocol::WorkspaceSummary> workspaces = bridge.host().listWorkspaces();
    ASSERT_FALSE(workspaces.empty());
    EXPECT_EQ(workspaces[0].id.value, bridge.identity().workspace.value);
    const protocol::WorkspaceDetail detail =
        bridge.host().showWorkspace(protocol::WorkspaceId{bridge.identity().workspace.value});
    EXPECT_EQ(detail.summary.canonical_path, bridge.root().string());
}

TEST_F(HostRuntimeTest, ActivateSuspendTracksActiveSession) {
    Bridge bridge("hr_active");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());

    bridge.host().activateSession(created.session);
    ASSERT_TRUE(bridge.host().hostStatus().active_session.has_value());
    EXPECT_EQ(bridge.host().hostStatus().active_session->value, created.session.value);
    EXPECT_EQ(bridge.host().agentStatus(created.session), "Idle");

    bridge.host().suspendSession(created.session);
    EXPECT_FALSE(bridge.host().hostStatus().active_session.has_value());
}

TEST_F(HostRuntimeTest, CompactSessionSubmitsMaintenanceTurn) {
    Bridge bridge("hr_compact");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());

    const RpcFailure missing =
        expect_rpc([&] { bridge.host().compactSession(SessionId{"missing"}); });
    EXPECT_EQ(missing.code, protocol::code_value(protocol::AppCode::UnknownSession));
    EXPECT_EQ(missing.kind, "UnknownSession");

    EXPECT_NO_THROW(bridge.host().compactSession(created.session));

    bool saw_terminal = false;
    for (int attempt = 0; attempt < 200 && !saw_terminal; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        for (const EventRecord& record :
             bridge.runtime().sessions().session(created.session).events()) {
            if (record.event.type == EventType::TurnEnded ||
                record.event.type == EventType::TurnCancelled ||
                record.event.type == EventType::TurnFailed) {
                saw_terminal = true;
            }
        }
    }
    EXPECT_TRUE(saw_terminal);
}

TEST_F(HostRuntimeTest, CommittedEventsStreamToSubscribedClientInOrder) {
    Bridge bridge("hr_stream");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.clear_forwarded();

    Bridge::Peer* peer = bridge.open_peer();
    bridge.hello(*peer);

    nlohmann::json subscribe_params;
    protocol::to_json(subscribe_params,
                      protocol::SubscribeParams{created.session, protocol::StreamFrom{}});
    const nlohmann::json subscribed =
        bridge.request(*peer, 2, protocol::method::kEventSubscribe, subscribe_params);
    ASSERT_TRUE(subscribed.contains("cursor"));

    bridge.append_event(created.session, EventType::TokenUsage);
    bridge.append_event(created.session, EventType::TokenUsage);

    Sequence previous = 0;
    int      streams = 0;
    for (const nlohmann::json& message : bridge.drain(*peer)) {
        if (message.value("method", std::string{}) != "event.stream") {
            continue;
        }
        const protocol::StreamNotification notification =
            message.at("params").get<protocol::StreamNotification>();
        EXPECT_FALSE(notification.replay);
        EXPECT_GT(notification.cursor.value.size(), 0u);
        const std::optional<Sequence> resolved =
            bridge.host().resolveCursor(created.session, notification.cursor);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_GT(*resolved, previous);
        previous = *resolved;
        ++streams;
    }
    EXPECT_EQ(streams, 2);
}

TEST_F(HostRuntimeTest, AttachRebroadcastsPendingPermission) {
    Bridge bridge("hr_rebroadcast");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.host().publishSubscriberCount(created.session, 1);

    std::future<PermissionOutcome> future =
        bridge.broker().resolve(ask_request(created.session), {});
    ASSERT_EQ(bridge.permission_transport().broadcastCount(), 1u);
    const std::string request_id = bridge.permission_transport().broadcasts().back().request_id;

    Bridge::Peer* peer = bridge.open_peer();
    bridge.hello(*peer);

    nlohmann::json subscribe_params;
    protocol::to_json(subscribe_params,
                      protocol::SubscribeParams{created.session, protocol::StreamFrom{}});
    bridge.send_frame(*peer, protocol::encode(protocol::Request{
                                 protocol::RequestId{std::int64_t{2}},
                                 std::string{protocol::method::kEventSubscribe},
                                 std::move(subscribe_params)}));

    bool saw_request = false;
    for (const nlohmann::json& message : bridge.drain(*peer)) {
        if (message.value("method", std::string{}) == "permission.request") {
            saw_request = true;
            EXPECT_EQ(message.at("params").at("request_id").get<std::string>(), request_id);
        }
    }
    EXPECT_TRUE(saw_request);

    EXPECT_TRUE(bridge.host().decidePermission(request_id, protocol::PermissionAnswer::Allow,
                                               protocol::PermissionScope::Once));
    EXPECT_EQ(future.wait_for(0ms), std::future_status::ready);
}

} // namespace
