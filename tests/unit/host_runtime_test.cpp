#include <gtest/gtest.h>

#include <atomic>
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

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/turn_executor.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/asio_executor.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/host/host_runtime.hpp"
#include "ymh/host/workspace_host.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/permission/permission_transport.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/plan_mode.hpp"
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

// A provider whose stream() blocks until released, so a test can hold a turn
// in flight while it exercises the detach / close / shutdown paths. It polls
// the cancellation token (a CancellationToken does not notify an unrelated
// condition variable) so the turn's own cancel path is observable.
class BlockingProvider final : public LLMProvider {
public:
    ProviderId id() const override { return "blocking"; }
    ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken cancel) override {
        entered_.store(true);
        for (int attempt = 0; attempt < 400 && !released_.load() && !cancel.cancelled(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (cancel.cancelled()) {
            LLMResponse cancelled;
            cancelled.outcome = StreamOutcome::Cancelled;
            cancelled.finish  = FinishReason::Other;
            return Task<LLMResponse>{cancelled};
        }
        sink(TextDelta{"hello"});
        sink(Finished{FinishReason::Stop, std::nullopt, std::nullopt});
        LLMResponse response;
        response.outcome = StreamOutcome::Completed;
        response.finish  = FinishReason::Stop;
        return Task<LLMResponse>{response};
    }

    bool waitEntered(std::chrono::milliseconds timeout = 2s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (entered_.load()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return entered_.load();
    }

    void release() { released_.store(true); }

private:
    std::atomic<bool> entered_{false};
    std::atomic<bool> released_{false};
};

// A provider that counts concurrent entries and blocks every call until
// released, so a test can occupy every executor worker and leave a later
// prompt queued behind them.
class GatedProvider final : public LLMProvider {
public:
    ProviderId id() const override { return "gated"; }
    ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken cancel) override {
        entered_.fetch_add(1);
        for (int attempt = 0; attempt < 400 && !released_.load() && !cancel.cancelled(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (cancel.cancelled()) {
            LLMResponse cancelled;
            cancelled.outcome = StreamOutcome::Cancelled;
            cancelled.finish  = FinishReason::Other;
            return Task<LLMResponse>{cancelled};
        }
        sink(TextDelta{"hello"});
        sink(Finished{FinishReason::Stop, std::nullopt, std::nullopt});
        LLMResponse response;
        response.outcome = StreamOutcome::Completed;
        response.finish  = FinishReason::Stop;
        return Task<LLMResponse>{response};
    }

    bool waitEntered(std::size_t count, std::chrono::milliseconds timeout = 2s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (entered_.load() >= count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return entered_.load() >= count;
    }

    void release() { released_.store(true); }

private:
    std::atomic<std::size_t> entered_{0};
    std::atomic<bool>        released_{false};
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

class IoLoop {
public:
    void start() {
        executor = std::make_unique<AsioExecutor>(io);
        work = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(io));
        thread = std::thread([this] { io.run(); });
        executor->bindRunnerThread(thread.get_id());
    }

    ~IoLoop() {
        if (work != nullptr) {
            work->reset();
        }
        io.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    IoLoop() = default;
    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;

    asio::io_context                                                          io;
    std::unique_ptr<AsioExecutor>                                             executor;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread                                                               thread;
};

class Bridge {
public:
    struct Peer {
        protocol::ClientId       id{};
        std::vector<std::string> frames;
        std::string              drop_reason;
    };

    explicit Bridge(const std::string& prefix, FakeScript script = FakeScript{},
                    bool with_pty = false,
                    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)>
                        provider_factory = {})
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
        if (provider_factory) {
            options.provider_factory = std::move(provider_factory);
        } else {
            options.provider_factory =
                [script](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
                return std::make_unique<FakeLLM>(script);
            };
        }
        if (with_pty) {
            io_.start();
            options.executor = io_.executor.get();
        }
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
        (void)turns_.drain(std::chrono::milliseconds{2000});
    }

    [[nodiscard]] WorkspaceRuntime&       runtime() { return *runtime_; }
    [[nodiscard]] WorkspaceRegistry&      registry() { return *registry_; }
    [[nodiscard]] HostRuntime&            host() { return *host_; }
    [[nodiscard]] PermissionBroker&       broker() { return *broker_; }
    [[nodiscard]] protocol::ProtocolServer& server() { return *server_; }
    [[nodiscard]] FakePermissionTransport&  permission_transport() { return *permission_transport_; }
    [[nodiscard]] const HostIdentity&     identity() const { return identity_; }
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    // AL-I13: tear the HostRuntime down (server first, so its `TransportHost&`
    // never dangles) while the bus stays alive, so a later committed publish
    // can prove the committed handler was unsubscribed.
    void destroy_host_for_test() {
        server_.reset();
        host_.reset();
    }

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
        auto     session_owner = runtime_->sessions().sessionPtr(id);
        Session& session       = *session_owner;
        Event event;
        event.id = make_event_id();
        event.session_id = id;
        event.timestamp = std::chrono::system_clock::now();
        event.type = type;
        event.payload = nlohmann::json::object();
        return session.appendEvent(event);
    }

    void emit_live(const SessionId& id, EventType type) {
        auto     session_owner = runtime_->sessions().sessionPtr(id);
        Session& session       = *session_owner;
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
    IoLoop                                   io_;
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

// 25 review H1: a restarted daemon's controller memo is cold, but the durable
// log already carries `plan/mode{active:true}`. `/plan off` must append
// `plan/mode{false}` rather than return Unchanged off a false default.
TEST_F(HostRuntimeTest, SetModeSeesLoggedStateWithColdMemo) {
    Bridge bridge("hr_set_mode_cold");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    ASSERT_FALSE(created.session.value.empty());
    const protocol::SessionResumed resumed = bridge.host().resumeSession(created.session);
    ASSERT_EQ(resumed.status, "Idle");

    std::shared_ptr<Session> session = bridge.runtime().sessions().sessionPtr(created.session);
    ASSERT_NE(session, nullptr);
    session->append(payload::PlanMode{true});

    const protocol::SetModeResult result = bridge.host().setSessionMode(
        nlohmann::json{{"session", created.session.value}, {"active", false}});
    EXPECT_FALSE(result.active);
    EXPECT_FALSE(result.pending);
    EXPECT_FALSE(plan_mode_active(bridge.runtime().sessions().sessionPtr(created.session)->events()));
}

// 23-D55 / SL-I22: all guards precede all side effects. A delete refused by the
// dependent-children precheck must leave the PTY open, the agent resident, and
// the row + junction intact.
TEST_F(HostRuntimeTest, SL_I22_DependentDeleteRefusalLeavesNoSideEffects) {
    Bridge bridge("hr_dep_refuse", FakeScript{}, /*with_pty=*/true);
    const protocol::SessionCreated parent =
        bridge.host().createSession(nlohmann::json::object());

    SessionOptions child_options;
    child_options.cwd           = bridge.root();
    child_options.serverProfile = "interactive";
    child_options.model         = "fake-model";
    child_options.kind          = SessionKind::Subagent;
    child_options.parentSession = parent.session;
    const SessionId child = bridge.runtime().sessions().createSession(child_options);
    ASSERT_TRUE(bridge.runtime().store().hasDependents(parent.session));

    nlohmann::json message;
    message["role"]    = "user";
    message["content"] = nlohmann::json::array(
        {nlohmann::json{{"kind", "text"}, {"text", "hi"}}});
    bridge.clear_forwarded();
    bridge.host().agentPrompt(parent.session, message);
    ASSERT_TRUE(bridge.wait_for_turn_end(10s));
    ASSERT_NE(bridge.runtime().agents().findShared(parent.session), nullptr);

    PtyRequest request;
    request.executable = "/bin/sh";
    request.argv       = {"/bin/sh", "-c", "sleep 30"};
    request.cwd        = bridge.root();
    request.session    = parent.session;
    (void)bridge.runtime().environment().pty().open(request, CancellationToken{}).get();
    ASSERT_FALSE(bridge.runtime().environment().pty().list(parent.session).empty());

    const std::size_t junctions_before =
        bridge.registry().listSessions(bridge.identity().workspace).size();

    const RpcFailure refused =
        expect_rpc([&] { bridge.host().deleteSession(parent.session); });
    EXPECT_EQ(refused.code, protocol::code_value(protocol::AppCode::DependentSession));
    EXPECT_EQ(refused.kind, "DependentSession");

    EXPECT_FALSE(bridge.runtime().environment().pty().list(parent.session).empty());
    EXPECT_NE(bridge.runtime().agents().findShared(parent.session), nullptr);
    EXPECT_TRUE(bridge.runtime().store().load(parent.session).has_value());
    EXPECT_TRUE(bridge.runtime().store().load(child).has_value());
    EXPECT_EQ(bridge.registry().listSessions(bridge.identity().workspace).size(),
              junctions_before);
}

// 23-D20 / SL-I23: deleting the session named by active_session_ resets it.
TEST_F(HostRuntimeTest, SL_I23_DeleteResetsActiveSession) {
    Bridge bridge("hr_delete_active");
    const protocol::SessionCreated created =
        bridge.host().createSession(nlohmann::json::object());
    bridge.host().activateSession(created.session);
    ASSERT_TRUE(bridge.host().hostStatus().active_session.has_value());

    // 24-D5: the activate body is itself queued executor work, so the
    // queue-aware mid-turn guard refuses until it has settled.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (bridge.host().hasPendingWork(created.session) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_FALSE(bridge.host().hasPendingWork(created.session));

    bridge.host().deleteSession(created.session, false, /*force=*/true);
    EXPECT_FALSE(bridge.host().hostStatus().active_session.has_value());
    EXPECT_FALSE(bridge.host().sessionExists(created.session));
}

// 23-D19 / SL-I24: the manager delete is lease-exempt and works on a
// non-resident durable session.
TEST_F(HostRuntimeTest, SL_I24_ManagerDeleteIsLeaseExemptForNonResidentSession) {
    Bridge bridge("hr_delete_nonresident");
    const protocol::SessionCreated created =
        bridge.host().createSession(nlohmann::json::object());
    bridge.runtime().sessions().closeSession(created.session);
    ASSERT_THROW(static_cast<void>(bridge.runtime().sessions().sessionPtr(created.session)),
                 UnknownSession);

    EXPECT_NO_THROW(bridge.runtime().sessions().deleteSession(created.session, false));
    EXPECT_FALSE(bridge.runtime().store().load(created.session).has_value());
}

// 23-D19 / SL-I25: the delete publishes exactly one fully-stamped
// SessionEnded{Deleted} after the commit.
TEST_F(HostRuntimeTest, SL_I25_ManagerDeletePublishesSessionEnded) {
    Bridge bridge("hr_delete_publish");
    const protocol::SessionCreated created =
        bridge.host().createSession(nlohmann::json::object());

    std::vector<Event> seen;
    Subscription       subscription = bridge.runtime().bus().subscribe(
        [&seen](const Event& event) { seen.push_back(event); });

    bridge.runtime().sessions().deleteSession(created.session, false);

    std::size_t ended = 0;
    for (const Event& event : seen) {
        if (event.type != EventType::SessionEnded) {
            continue;
        }
        ++ended;
        EXPECT_EQ(event.session_id.value, created.session.value);
        EXPECT_EQ(event.payload.get<payload::SessionEnded>().reason,
                  payload::SessionEndReason::Deleted);
        EXPECT_FALSE(event.id.value.empty());
    }
    EXPECT_EQ(ended, 1u);
}

// 23-D55 / SL-I26: the success path closes the session PTY, after the guards.
TEST_F(HostRuntimeTest, SL_I26_DeleteClosesPtyOnSuccess) {
    Bridge bridge("hr_delete_pty", FakeScript{}, /*with_pty=*/true);
    const protocol::SessionCreated created =
        bridge.host().createSession(nlohmann::json::object());

    PtyRequest request;
    request.executable = "/bin/sh";
    request.argv       = {"/bin/sh", "-c", "sleep 30"};
    request.cwd        = bridge.root();
    request.session    = created.session;
    (void)bridge.runtime().environment().pty().open(request, CancellationToken{}).get();
    ASSERT_FALSE(bridge.runtime().environment().pty().list(created.session).empty());

    bridge.host().deleteSession(created.session);
    EXPECT_TRUE(bridge.runtime().environment().pty().list(created.session).empty());
}

// AL-I9/AL23/AL-F15: `session.close` on a running turn is a detach. It must not
// cancel the turn, erase the agent, or close the resident session. The old
// cancel-on-close failed every assertion below.
TEST_F(HostRuntimeTest, AL_I9_CloseDetachesWithoutCancellingTheTurn) {
    BlockingProvider* provider = nullptr;
    Bridge            bridge(
        "hr_detach_close", FakeScript{}, /*with_pty=*/false,
        [&provider](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            auto owned = std::make_unique<BlockingProvider>();
            provider   = owned.get();
            return owned;
        });
    const protocol::SessionCreated created =
        bridge.host().createSession(nlohmann::json::object());

    nlohmann::json message;
    message["role"]    = "user";
    message["content"] = nlohmann::json::array(
        {nlohmann::json{{"kind", "text"}, {"text", "hi"}}});
    bridge.clear_forwarded();
    bridge.host().agentPrompt(created.session, message);
    ASSERT_NE(provider, nullptr);
    ASSERT_TRUE(provider->waitEntered());

    bridge.host().closeSession(created.session);

    EXPECT_NE(bridge.runtime().agents().findShared(created.session), nullptr);
    EXPECT_NO_THROW((void)bridge.runtime().sessions().sessionPtr(created.session));
    EXPECT_FALSE(bridge.wait_for_turn_end(0ms));

    provider->release();
    EXPECT_TRUE(bridge.wait_for_turn_end(10s));

    std::size_t ended = 0;
    for (const EventRecord& record : bridge.forwarded()) {
        if (record.event.type == EventType::SessionEnded) {
            ++ended;
        }
    }
    EXPECT_EQ(ended, 0u);
    EXPECT_NE(bridge.runtime().agents().findShared(created.session), nullptr);
}

// AL-I2/AL7/AL-F14: a prompt submitted to the executor but not yet picked up by
// the agent is pending work. Deleting its session must be refused with
// InvalidParams "turn in progress", and the queued prompt must still run. The
// old guard consulted only the agent inbox/state — blind to the executor queue
// — so it deleted the session and dropped the just-acked prompt.
TEST_F(HostRuntimeTest, AL_I2_DeleteRefusesWhilePromptIsQueued) {
    GatedProvider* provider = nullptr;
    Bridge         bridge(
        "hr_al_i2", FakeScript{}, /*with_pty=*/false,
        [&provider](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            auto owned = std::make_unique<GatedProvider>();
            provider   = owned.get();
            return owned;
        });
    ASSERT_NE(provider, nullptr);

    const protocol::SessionCreated first = bridge.host().createSession(nlohmann::json::object());
    const protocol::SessionCreated second = bridge.host().createSession(nlohmann::json::object());
    const protocol::SessionCreated queued = bridge.host().createSession(nlohmann::json::object());

    nlohmann::json message;
    message["role"]    = "user";
    message["content"] = nlohmann::json::array({nlohmann::json{{"kind", "text"}, {"text", "hi"}}});

    // Occupy both workers, so the third prompt stays in the executor queue and
    // is invisible to the agent until a worker picks it up.
    bridge.host().agentPrompt(first.session, message);
    bridge.host().agentPrompt(second.session, message);
    ASSERT_TRUE(provider->waitEntered(2));
    bridge.host().agentPrompt(queued.session, message);
    ASSERT_TRUE(bridge.host().hasPendingWork(queued.session));

    try {
        bridge.host().deleteSession(queued.session);
        FAIL() << "delete must refuse while a prompt is queued";
    } catch (const protocol::RpcException& error) {
        EXPECT_EQ(error.code(), protocol::code_value(protocol::RpcCode::InvalidParams));
        EXPECT_EQ(std::string{error.what()}, "turn in progress");
    }
    EXPECT_TRUE(bridge.host().sessionExists(queued.session));

    provider->release();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (bridge.host().hasPendingWork(queued.session) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(bridge.host().hasPendingWork(queued.session));
    EXPECT_TRUE(bridge.host().sessionExists(queued.session));
    const EventRange queued_events = bridge.runtime().store().readAfter(queued.session, 0, 1000);
    EXPECT_GE(count_type(queued_events, EventType::TurnStarted), 1u);
}

// AL-I15/AL25/AL33/AL-F23: last-exit finalizeAll parks each agent, explicitly
// closes every resident session (the manager's map empties), and emits no
// SessionEnded; the durable log still ends with exactly one terminal event.
TEST_F(HostRuntimeTest, AL_I15_FinalizeAllClosesResidentSessionsWithoutSessionEnded) {
    Bridge bridge("hr_finalize_idle");
    const protocol::SessionCreated created =
        bridge.host().createSession(nlohmann::json::object());

    nlohmann::json message;
    message["role"]    = "user";
    message["content"] = nlohmann::json::array(
        {nlohmann::json{{"kind", "text"}, {"text", "hi"}}});
    bridge.clear_forwarded();
    bridge.host().agentPrompt(created.session, message);
    ASSERT_TRUE(bridge.wait_for_turn_end(10s));
    ASSERT_NE(bridge.runtime().agents().findShared(created.session), nullptr);

    const EventRange before = bridge.runtime().store().readAfter(created.session, 0, 1000);
    const std::size_t before_terminal =
        count_type(before, EventType::TurnEnded) + count_type(before, EventType::TurnCancelled);
    ASSERT_EQ(before_terminal, 1u);

    bridge.clear_forwarded();
    bridge.runtime().agents().finalizeAll();

    EXPECT_EQ(bridge.runtime().agents().findShared(created.session), nullptr);
    EXPECT_THROW((void)bridge.runtime().sessions().sessionPtr(created.session), UnknownSession);

    std::size_t ended = 0;
    for (const EventRecord& record : bridge.forwarded()) {
        if (record.event.type == EventType::SessionEnded) {
            ++ended;
        }
    }
    EXPECT_EQ(ended, 0u);

    const EventRange events = bridge.runtime().store().readAfter(created.session, 0, 1000);
    const std::size_t after_terminal =
        count_type(events, EventType::TurnEnded) + count_type(events, EventType::TurnCancelled);
    EXPECT_EQ(after_terminal, before_terminal);
    EXPECT_EQ(after_terminal, 1u);
}

// AL-I10/AL25/AL-F16: last-exit finalizeAll finalizes an IN-FLIGHT turn — the
// cancel path appends exactly one terminal event, the session is closed, and no
// SessionEnded is emitted.
TEST_F(HostRuntimeTest, AL_I10_FinalizeAllFinalizesInFlightTurn) {
    BlockingProvider* provider = nullptr;
    Bridge            bridge(
        "hr_finalize_inflight", FakeScript{}, /*with_pty=*/false,
        [&provider](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            auto owned = std::make_unique<BlockingProvider>();
            provider   = owned.get();
            return owned;
        });
    const protocol::SessionCreated created =
        bridge.host().createSession(nlohmann::json::object());

    nlohmann::json message;
    message["role"]    = "user";
    message["content"] = nlohmann::json::array(
        {nlohmann::json{{"kind", "text"}, {"text", "hi"}}});
    bridge.clear_forwarded();
    bridge.host().agentPrompt(created.session, message);
    ASSERT_NE(provider, nullptr);
    ASSERT_TRUE(provider->waitEntered());

    bridge.runtime().agents().finalizeAll();

    EXPECT_EQ(bridge.runtime().agents().findShared(created.session), nullptr);
    EXPECT_THROW((void)bridge.runtime().sessions().sessionPtr(created.session), UnknownSession);

    // The cancelled turn's terminal event is appended by the worker's cancel
    // path. Assert the specific terminal kind: a natural `TurnEnded` here would
    // mean the turn was not cancelled and the test must fail.
    auto cancelled_count = [&] {
        const EventRange events = bridge.runtime().store().readAfter(created.session, 0, 1000);
        return count_type(events, EventType::TurnCancelled);
    };
    for (int attempt = 0; attempt < 400 && cancelled_count() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    EXPECT_EQ(cancelled_count(), 1u);
    const EventRange settled = bridge.runtime().store().readAfter(created.session, 0, 1000);
    EXPECT_EQ(count_type(settled, EventType::TurnEnded), 0u);
    EXPECT_EQ(count_type(settled, EventType::TurnCancelled) +
                  count_type(settled, EventType::TurnEnded),
              1u);

    std::size_t ended = 0;
    for (const EventRecord& record : bridge.forwarded()) {
        if (record.event.type == EventType::SessionEnded) {
            ++ended;
        }
    }
    EXPECT_EQ(ended, 0u);
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

// AL-U11/AL17/AL-F4: the delete's `SessionEnded` is forwarded with its real
// store `Sequence` even though the row was erased in the same transaction. The
// old re-read path (`readAfter`) found the row gone and dropped the event.
TEST_F(HostRuntimeTest, AL_U11_ForwardsErasedRowSessionEndedWithSequence) {
    Bridge bridge("hr_forward_deleted");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.append_event(created.session, EventType::TokenUsage);
    bridge.clear_forwarded();

    bridge.host().deleteSession(created.session);

    const std::vector<EventRecord> forwarded = bridge.forwarded();
    ASSERT_EQ(forwarded.size(), 1u);
    EXPECT_EQ(forwarded[0].event.type, EventType::SessionEnded);
    EXPECT_EQ(forwarded[0].event.session_id.value, created.session.value);
    EXPECT_EQ(forwarded[0].event.payload.get<payload::SessionEnded>().reason,
              payload::SessionEndReason::Deleted);
    EXPECT_GT(forwarded[0].seq, 0);
    EXPECT_FALSE(bridge.runtime().store().load(created.session).has_value());
}

// AL-U12/AL18/AL-F11: the per-session monotonic guard forwards only strictly
// increasing sequences.
TEST_F(HostRuntimeTest, AL_U12_MonotonicGuardDropsLowerAndEqualSequences) {
    Bridge bridge("hr_monotonic");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.clear_forwarded();

    Event event;
    event.id         = make_event_id();
    event.session_id = created.session;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = EventType::TokenUsage;
    event.payload    = nlohmann::json::object();

    bridge.runtime().bus().publishCommitted(EventRecord{10, event});
    bridge.runtime().bus().publishCommitted(EventRecord{10, event});
    bridge.runtime().bus().publishCommitted(EventRecord{9, event});
    bridge.runtime().bus().publishCommitted(EventRecord{11, event});

    const std::vector<EventRecord> forwarded = bridge.forwarded();
    ASSERT_EQ(forwarded.size(), 2u);
    EXPECT_EQ(forwarded[0].seq, 10);
    EXPECT_EQ(forwarded[1].seq, 11);
}

// AL-I3/AL17: a subscribed client receives the delete's `SessionEnded` with a
// minted cursor. `resolveCursor` on the erased id is `CursorInvalid` by design
// (05 §8.5/T16), so validity here is the non-empty cursor minted from the
// record's real sequence (AL-U11 covers the sequence itself).
TEST_F(HostRuntimeTest, AL_I3_SubscribedClientReceivesDeletedSessionEndedWithCursor) {
    Bridge bridge("hr_delete_cursor");
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

    bridge.host().deleteSession(created.session);

    bool saw_ended = false;
    for (const nlohmann::json& message : bridge.drain(*peer)) {
        if (message.value("method", std::string{}) != "event.stream") {
            continue;
        }
        const protocol::StreamNotification notification =
            message.at("params").get<protocol::StreamNotification>();
        if (notification.envelope.event.type != EventType::SessionEnded) {
            continue;
        }
        saw_ended = true;
        EXPECT_FALSE(notification.replay);
        EXPECT_FALSE(notification.cursor.value.empty());
        EXPECT_EQ(notification.envelope.event.session_id.value, created.session.value);
        EXPECT_EQ(notification.envelope.event.payload.get<payload::SessionEnded>().reason,
                  payload::SessionEndReason::Deleted);
    }
    EXPECT_TRUE(saw_ended);

    const std::vector<EventRecord> forwarded = bridge.forwarded();
    ASSERT_EQ(forwarded.size(), 1u);
    EXPECT_GT(forwarded[0].seq, 0);
}

// AL-I13/AL29/AL-F20: `~HostRuntime` unsubscribes the committed handler, so a
// later committed publish does not call into the destroyed runtime.
TEST_F(HostRuntimeTest, AL_I13_DestructorUnsubscribesCommittedHandler) {
    Bridge bridge("hr_dtor_committed");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    bridge.clear_forwarded();

    bridge.append_event(created.session, EventType::TokenUsage);
    ASSERT_EQ(bridge.forwarded().size(), 1u);

    bridge.destroy_host_for_test();

    Event event;
    event.id         = make_event_id();
    event.session_id = created.session;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = EventType::TokenUsage;
    event.payload    = nlohmann::json::object();
    bridge.runtime().bus().publishCommitted(EventRecord{9999, event});

    EXPECT_EQ(bridge.forwarded().size(), 1u);
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

    bool           hook_called = false;
    ShutdownReason hook_reason = ShutdownReason::ClientRequest;
    bridge.host().setShutdownHook([&](ShutdownReason reason) {
        hook_called = true;
        hook_reason = reason;
    });
    bridge.host().requestShutdown(protocol::ShutdownReason::LastSupervisor);
    EXPECT_EQ(bridge.host().hostState(), protocol::HostState::Draining);
    EXPECT_TRUE(hook_called);
    EXPECT_EQ(hook_reason, ShutdownReason::LastSupervisor);

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

TEST_F(HostRuntimeTest, ShutdownReasonMappingDoesNotDegrade) {
    Bridge bridge("hr_reason_map");
    const std::pair<protocol::ShutdownReason, ShutdownReason> cases[] = {
        {protocol::ShutdownReason::ClientRequest, ShutdownReason::ClientRequest},
        {protocol::ShutdownReason::Signal, ShutdownReason::Signal},
        {protocol::ShutdownReason::StartupFailure, ShutdownReason::StartupFailure},
        {protocol::ShutdownReason::LastSupervisor, ShutdownReason::LastSupervisor},
        {protocol::ShutdownReason::NoOwners, ShutdownReason::NoOwners},
        {protocol::ShutdownReason::WorkspaceStop, ShutdownReason::WorkspaceStop},
    };
    for (const auto& [wire, expected] : cases) {
        std::optional<ShutdownReason> seen;
        bridge.host().setShutdownHook([&](ShutdownReason reason) { seen = reason; });
        bridge.host().requestShutdown(wire);
        ASSERT_TRUE(seen.has_value()) << protocol::to_string(wire);
        EXPECT_EQ(*seen, expected) << protocol::to_string(wire);
    }
}

TEST_F(HostRuntimeTest, FreshOwnerSnapshotConvertsSupervisorIds) {
    Bridge bridge("hr_snapshot");
    bridge.host().setOwnerSnapshotSource([] {
        auto ids = std::make_shared<std::vector<SupervisorId>>();
        ids->push_back(SupervisorId{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"});
        ids->push_back(SupervisorId{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"});
        return std::shared_ptr<const std::vector<SupervisorId>>(std::move(ids));
    });
    const std::shared_ptr<const std::vector<protocol::ClientInstanceId>> snapshot =
        bridge.host().freshOwnerSnapshot();
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->size(), 2u);
    EXPECT_EQ((*snapshot)[0].value, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    EXPECT_EQ((*snapshot)[1].value, "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");

    bridge.host().setOwnerSnapshotSource({});
    EXPECT_TRUE(bridge.host().freshOwnerSnapshot()->empty());
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
             bridge.runtime().sessions().sessionPtr(created.session)->events()) {
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

TEST_F(HostRuntimeTest, RenameSessionValidatesNormalizesAndForwards) {
    Bridge bridge("hr_rename");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    const SessionId              session = created.session;
    bridge.clear_forwarded();

    const protocol::SessionRenamedResult result = bridge.host().renameSession(
        nlohmann::json{{"session", session.value}, {"title", "  trimmed title  "}});
    EXPECT_EQ(result.session.value, session.value);
    EXPECT_EQ(result.title, "trimmed title");
    EXPECT_EQ(bridge.runtime().sessions().sessionPtr(session)->header().title, "trimmed title");

    bool forwarded = false;
    for (const EventRecord& record : bridge.forwarded()) {
        if (record.event.type == EventType::SessionRenamed) {
            forwarded = true;
        }
    }
    EXPECT_TRUE(forwarded);

    const RpcFailure missing_title = expect_rpc([&] {
        static_cast<void>(bridge.host().renameSession(nlohmann::json{{"session", session.value}}));
    });
    EXPECT_EQ(missing_title.code, protocol::code_value(protocol::RpcCode::InvalidParams));

    const RpcFailure non_string = expect_rpc([&] {
        static_cast<void>(bridge.host().renameSession(
            nlohmann::json{{"session", session.value}, {"title", 5}}));
    });
    EXPECT_EQ(non_string.code, protocol::code_value(protocol::RpcCode::InvalidParams));

    const RpcFailure empty = expect_rpc([&] {
        static_cast<void>(bridge.host().renameSession(
            nlohmann::json{{"session", session.value}, {"title", "   "}}));
    });
    EXPECT_EQ(empty.code, protocol::code_value(protocol::RpcCode::InvalidParams));

    const RpcFailure control = expect_rpc([&] {
        static_cast<void>(bridge.host().renameSession(
            nlohmann::json{{"session", session.value}, {"title", "a\nb"}}));
    });
    EXPECT_EQ(control.code, protocol::code_value(protocol::RpcCode::InvalidParams));

    const RpcFailure oversize = expect_rpc([&] {
        static_cast<void>(bridge.host().renameSession(
            nlohmann::json{{"session", session.value}, {"title", std::string(121, 'a')}}));
    });
    EXPECT_EQ(oversize.code, protocol::code_value(protocol::RpcCode::InvalidParams));

    const RpcFailure bad_utf8 = expect_rpc([&] {
        static_cast<void>(bridge.host().renameSession(
            nlohmann::json{{"session", session.value}, {"title", "\xc0\xaf"}}));
    });
    EXPECT_EQ(bad_utf8.code, protocol::code_value(protocol::RpcCode::InvalidParams));

    const RpcFailure unknown = expect_rpc([&] {
        static_cast<void>(bridge.host().renameSession(
            nlohmann::json{{"session", "missing"}, {"title", "x"}}));
    });
    EXPECT_EQ(unknown.code, protocol::code_value(protocol::AppCode::UnknownSession));

    EXPECT_EQ(bridge.runtime().sessions().sessionPtr(session)->header().title, "trimmed title");
}

TEST_F(HostRuntimeTest, AgentPromptAutoNamesOnce) {
    Bridge bridge("hr_autoname");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    const SessionId              session = created.session;
    EXPECT_EQ(bridge.runtime().sessions().sessionPtr(session)->header().title, "");

    bridge.host().agentPrompt(session, nlohmann::json("fix the flaky PTY test"));
    ASSERT_TRUE(bridge.wait_for_turn_end(10s));
    EXPECT_EQ(bridge.runtime().sessions().sessionPtr(session)->header().title, "fix the flaky PTY test");

    bridge.clear_forwarded();
    bridge.host().agentPrompt(session, nlohmann::json("a second prompt"));
    ASSERT_TRUE(bridge.wait_for_turn_end(10s));
    EXPECT_EQ(bridge.runtime().sessions().sessionPtr(session)->header().title, "fix the flaky PTY test");

    std::size_t renames = 0;
    for (const EventRecord& record : bridge.runtime().sessions().sessionPtr(session)->ownEvents()) {
        if (record.event.type == EventType::SessionRenamed) {
            ++renames;
        }
    }
    EXPECT_EQ(renames, 1u);
}

TEST_F(HostRuntimeTest, ManualRenameIsNotOverwrittenByAutoName) {
    Bridge bridge("hr_manualwin");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    const SessionId              session = created.session;

    const protocol::SessionRenamedResult renamed = bridge.host().renameSession(
        nlohmann::json{{"session", session.value}, {"title", "manual name"}});
    EXPECT_EQ(renamed.title, "manual name");

    bridge.clear_forwarded();
    bridge.host().agentPrompt(session, nlohmann::json("first prompt"));
    ASSERT_TRUE(bridge.wait_for_turn_end(10s));
    EXPECT_EQ(bridge.runtime().sessions().sessionPtr(session)->header().title, "manual name");

    std::size_t user_renames = 0;
    std::size_t auto_renames = 0;
    for (const EventRecord& record : bridge.runtime().sessions().sessionPtr(session)->ownEvents()) {
        if (record.event.type != EventType::SessionRenamed) {
            continue;
        }
        const auto payload = record.event.payload.get<payload::SessionRenamed>();
        if (payload.origin == payload::RenameOrigin::Auto) {
            ++auto_renames;
        } else {
            ++user_renames;
        }
    }
    EXPECT_EQ(user_renames, 1u);
    EXPECT_EQ(auto_renames, 0u);
}

TEST_F(HostRuntimeTest, AutoNameAppendFailureIsSwallowed) {
    Bridge bridge("hr_autofail");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());
    const SessionId              session = created.session;

    auto* memory = dynamic_cast<MemorySessionStore*>(&bridge.runtime().store());
    ASSERT_NE(memory, nullptr);
    memory->throw_on_append_type = EventType::SessionRenamed;

    EXPECT_NO_THROW(bridge.host().agentPrompt(session, nlohmann::json("first prompt")));
    ASSERT_TRUE(bridge.wait_for_turn_end(10s));

    EXPECT_EQ(bridge.runtime().sessions().sessionPtr(session)->header().title, "");
    for (const EventRecord& record : bridge.runtime().sessions().sessionPtr(session)->ownEvents()) {
        EXPECT_NE(record.event.type, EventType::SessionRenamed);
    }
}

TEST_F(HostRuntimeTest, ShowContextProjectsAssembledStateReadOnly) {
    Bridge                        bridge("hr_context");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());

    nlohmann::json message;
    message["role"] = "user";
    message["content"] =
        nlohmann::json::array({nlohmann::json{{"kind", "text"}, {"text", "hi there"}}});
    bridge.host().agentPrompt(created.session, message);
    ASSERT_TRUE(bridge.wait_for_turn_end(10s));

    const std::size_t before = bridge.runtime().store().read(created.session, 0).size();
    const nlohmann::json snapshot = bridge.host().showContext(created.session);
    const std::size_t after = bridge.runtime().store().read(created.session, 0).size();
    EXPECT_EQ(before, after);

    EXPECT_GT(snapshot.at("used_tokens").get<std::uint64_t>(), 0u);
    EXPECT_EQ(snapshot.at("captured_sequence").get<std::uint64_t>(),
              bridge.runtime().store().headSequence(created.session));
    bool saw_conversation = false;
    for (const nlohmann::json& segment : snapshot.at("segments")) {
        if (segment.at("kind") == "conversation") {
            saw_conversation = true;
            EXPECT_GT(segment.at("items").get<std::size_t>(), 0u);
        }
    }
    EXPECT_TRUE(saw_conversation);
    EXPECT_EQ(snapshot.at("budget").at("window_tokens").get<std::uint64_t>(), 0u);
    EXPECT_NE(snapshot.at("note").get<std::string>().find("budget unknown"), std::string::npos);
}

TEST_F(HostRuntimeTest, ShowContextStaysUnknownWhenNoWindowSourceIsKnown) {
    Bridge bridge("hr_context_no_window");
    const protocol::SessionCreated created = bridge.host().createSession(nlohmann::json::object());

    const nlohmann::json snapshot = bridge.host().showContext(created.session);
    EXPECT_EQ(snapshot.at("budget").at("window_tokens").get<std::uint64_t>(), 0u);
    EXPECT_NE(snapshot.at("note").get<std::string>().find("budget unknown"), std::string::npos);
}

TEST_F(HostRuntimeTest, ShowContextUnknownSessionIsTypedError) {
    Bridge bridge("hr_context_unknown");
    try {
        (void)bridge.host().showContext(SessionId{"nope"});
        FAIL() << "expected RpcException";
    } catch (const protocol::RpcException& error) {
        EXPECT_EQ(error.code(), protocol::code_value(protocol::AppCode::UnknownSession));
    }
}

} // namespace
