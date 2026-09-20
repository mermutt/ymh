#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/preset.hpp"
#include "ymh/agent/turn_executor.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/host/host_runtime.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/permission/permission_transport.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

class NullPermissionTransport final : public PermissionTransport {
public:
    bool broadcast_permission_request(protocol::PermissionRequest) override { return true; }
    bool schedule_after(std::chrono::milliseconds, std::function<void()>) override {
        return true;
    }
};

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

void write_preset(const std::filesystem::path& root, const std::string& id,
                  const std::string& body) {
    const std::filesystem::path dir = root / id;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "preset.jsonc", std::ios::binary) << body;
}

// Owns a WorkspaceRuntime + HostRuntime wired with a two-preset roster.
class AgentRpcFixture {
public:
    AgentRpcFixture(const std::string& prefix, std::vector<std::string> preset_ids) {
        root_ = std::filesystem::canonical(workspace_.path());
        for (const std::string& id : preset_ids) {
            write_preset(presets_.path(), id, R"({"display_name":")" + id + R"("})");
        }
        RegistryConfig registry_config;
        registry_config.db_path = registry_dir_.path() / "registry.db";
        registry_config.lock_path = registry_dir_.path() / "registry.lock";
        registry_config.workspace_roots = {};
        registry_ = WorkspaceRegistry::open(registry_config);
        record_ = registry_->registerWorkspace(root_, prefix);

        WorkspaceRuntimeOptions options;
        options.config = Config{};
        options.config.presets.root = presets_.path();
        options.config.presets.include_user_root = false;
        options.config.presets.include_shipped_root = false;
        options.config.presets.default_id = preset_ids.empty() ? std::nullopt
                                                               : std::optional<std::string>{
                                                                     preset_ids.front()};
        options.root = root_;
        options.boot_id = BootId{prefix + "-boot"};
        options.store_factory = [] { return std::make_unique<MemorySessionStore>(); };
        options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            FakeScript script;
            script.steps.push_back(text_step("ok"));
            return std::make_unique<FakeLLM>(script);
        };
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(std::move(options));
        if (!created.has_value()) {
            throw std::runtime_error("runtime create failed: " + created.error().detail);
        }
        runtime_ = std::move(*created);

        transport_ = std::make_unique<NullPermissionTransport>();
        broker_ = std::make_unique<PermissionBroker>(runtime_->policy(), *transport_,
                                                     PermissionConfig{});

        HostIdentity identity;
        identity.workspace = record_.id;
        identity.boot_id = HostBootId{prefix + "-boot"};
        identity.pid = 4242;
        identity.socket_path = root_ / ".ymh" / "host.sock";
        host_ = std::make_unique<HostRuntime>(*runtime_, *registry_, identity, turns_, *broker_);
    }

    ~AgentRpcFixture() { (void)turns_.drain(std::chrono::milliseconds{2000}); }

    [[nodiscard]] HostRuntime& host() { return *host_; }
    [[nodiscard]] WorkspaceRuntime& runtime() { return *runtime_; }

    [[nodiscard]] std::string create_session(const std::string& preset = "standard") {
        nlohmann::json params{{"title", "t"}};
        if (!preset.empty()) {
            params["agent_preset"] = preset;
        }
        const protocol::SessionCreated created = host_->createSession(params);
        return created.session.value;
    }

    void make_non_blank(const std::string& session) {
        runtime_->sessions().sessionPtr(SessionId{session})->append(payload::UserMessage{});
    }

    [[nodiscard]] std::size_t event_count(const std::string& session) {
        return runtime_->sessions().sessionPtr(SessionId{session})->events().size();
    }

    [[nodiscard]] std::size_t selected_event_count(const std::string& session) {
        std::size_t count = 0;
        for (const EventRecord& record :
             runtime_->sessions().sessionPtr(SessionId{session})->events()) {
            if (record.event.type == EventType::AgentPresetSelected) {
                ++count;
            }
        }
        return count;
    }

private:
    TempWorkspace                      workspace_{"agent_rpc_ws"};
    TempWorkspace                      presets_{"agent_rpc_presets"};
    TempWorkspace                      registry_dir_{"agent_rpc_reg"};
    std::filesystem::path              root_;
    std::unique_ptr<WorkspaceRegistry> registry_;
    WorkspaceRecord                    record_;
    std::unique_ptr<WorkspaceRuntime>  runtime_;
    std::unique_ptr<NullPermissionTransport> transport_;
    std::unique_ptr<PermissionBroker>  broker_;
    TurnExecutor                       turns_{1, 1};
    std::unique_ptr<HostRuntime>       host_;
};

// 45-D9.3/45-I22: agent.list returns the roster with the daemon-owned
// blank/can_select; a blank session marks every non-active preset selectable.
TEST(AgentRpcTest, UI45_D9_AgentListBlankCanSelect) {
    AgentRpcFixture fixture("agent-list-blank", {"standard", "second"});
    const std::string session = fixture.create_session();

    const nlohmann::json result =
        fixture.host().listAgents(nlohmann::json{{"session", session}});
    ASSERT_TRUE(result.contains("agents"));
    ASSERT_EQ(result.at("agents").size(), 2u);
    EXPECT_EQ(result.at("active").get<std::string>(), "standard");
    EXPECT_EQ(result.at("default").get<std::string>(), "standard");
    for (const nlohmann::json& agent : result.at("agents")) {
        EXPECT_TRUE(agent.at("blank").get<bool>());
        const bool active = agent.at("id").get<std::string>() == "standard";
        EXPECT_EQ(agent.at("can_select").get<bool>(), !active);
    }

    // With no session there is nothing to test: blank/can_select are false and
    // active is empty.
    const nlohmann::json no_session = fixture.host().listAgents(nlohmann::json::object());
    EXPECT_EQ(no_session.at("active").get<std::string>(), "");
    for (const nlohmann::json& agent : no_session.at("agents")) {
        EXPECT_FALSE(agent.at("blank").get<bool>());
        EXPECT_FALSE(agent.at("can_select").get<bool>());
    }
}

// 45-D9.3: `active` folds the last `agent_preset/selected` event, not the
// creation-time header preset.
TEST(AgentRpcTest, UI45_D9_AgentListActiveFoldsLog) {
    AgentRpcFixture fixture("agent-list-fold", {"standard", "second"});
    const std::string session = fixture.create_session();

    fixture.host().selectAgent(
        nlohmann::json{{"session", session}, {"agent", "second"}});

    const nlohmann::json result =
        fixture.host().listAgents(nlohmann::json{{"session", session}});
    EXPECT_EQ(result.at("active").get<std::string>(), "second");
    for (const nlohmann::json& agent : result.at("agents")) {
        const bool active = agent.at("id").get<std::string>() == "second";
        EXPECT_EQ(agent.at("can_select").get<bool>(), !active);
    }
}

// 45-D9.3/45-I26: agent.list params are strict.
TEST(AgentRpcTest, UI45_D9_AgentListStrictParams) {
    AgentRpcFixture fixture("agent-list-strict", {"standard", "second"});
    const std::string session = fixture.create_session();

    EXPECT_THROW(fixture.host().listAgents(nlohmann::json::array()),
                 protocol::RpcException);
    EXPECT_THROW(fixture.host().listAgents(nlohmann::json{{"bogus", 1}}),
                 protocol::RpcException);
    EXPECT_THROW(fixture.host().listAgents(nlohmann::json{{"session", 7}}),
                 protocol::RpcException);
    EXPECT_THROW(fixture.host().listAgents(nlohmann::json{{"session", ""}}),
                 protocol::RpcException);

    try {
        (void)fixture.host().listAgents(nlohmann::json::array());
        FAIL() << "non-object params must throw";
    } catch (const protocol::RpcException& error) {
        EXPECT_EQ(error.code(), protocol::code_value(protocol::RpcCode::InvalidParams));
    }

    // Unknown session -> UnknownSession.
    try {
        (void)fixture.host().listAgents(nlohmann::json{{"session", "missing"}});
        FAIL() << "unknown session must throw";
    } catch (const protocol::RpcException& error) {
        EXPECT_EQ(error.code(), protocol::code_value(protocol::AppCode::UnknownSession));
    }
    EXPECT_EQ(session.empty(), false);
}

// 45-D9.4/45-I26: agent.select params are strict.
TEST(AgentRpcTest, UI45_D9_AgentSelectStrictParams) {
    AgentRpcFixture fixture("agent-select-strict", {"standard", "second"});
    const std::string session = fixture.create_session();

    EXPECT_THROW(fixture.host().selectAgent(nlohmann::json()), protocol::RpcException);
    EXPECT_THROW(fixture.host().selectAgent(nlohmann::json::array()),
                 protocol::RpcException);
    EXPECT_THROW(fixture.host().selectAgent(nlohmann::json{{"session", session}}),
                 protocol::RpcException);
    EXPECT_THROW(fixture.host().selectAgent(
                     nlohmann::json{{"session", session}, {"agent", ""}}),
                 protocol::RpcException);
    EXPECT_THROW(fixture.host().selectAgent(
                     nlohmann::json{{"session", session}, {"agent", 3}}),
                 protocol::RpcException);
    EXPECT_THROW(fixture.host().selectAgent(nlohmann::json{{"session", session},
                                                           {"agent", "second"},
                                                           {"bogus", true}}),
                 protocol::RpcException);

    try {
        (void)fixture.host().selectAgent(nlohmann::json{{"session", session}});
        FAIL() << "missing agent must throw";
    } catch (const protocol::RpcException& error) {
        EXPECT_EQ(error.code(), protocol::code_value(protocol::RpcCode::InvalidParams));
    }

    // Unknown preset -> InvalidParams (42-F1).
    try {
        (void)fixture.host().selectAgent(
            nlohmann::json{{"session", session}, {"agent", "nope"}});
        FAIL() << "unknown preset must throw";
    } catch (const protocol::RpcException& error) {
        EXPECT_EQ(error.code(), protocol::code_value(protocol::RpcCode::InvalidParams));
    }
}

// 45-D9.4/45-I21: a blank session switches and appends agent_preset/selected.
TEST(AgentRpcTest, UI45_D9_AgentSelectBlankSucceeds) {
    AgentRpcFixture fixture("agent-select-blank", {"standard", "second"});
    const std::string session = fixture.create_session();
    const std::size_t before = fixture.selected_event_count(session);

    const nlohmann::json result =
        fixture.host().selectAgent(nlohmann::json{{"session", session}, {"agent", "second"}});
    EXPECT_EQ(result.at("agent").get<std::string>(), "second");
    EXPECT_EQ(fixture.selected_event_count(session), before + 1);
}

// 45-D9.4/45-I21/45-F11: a non-blank session is rejected with CompositionFixed
// and appends nothing.
TEST(AgentRpcTest, UI45_D9_AgentSelectNonBlankRejected) {
    AgentRpcFixture fixture("agent-select-fixed", {"standard", "second"});
    const std::string session = fixture.create_session();
    fixture.make_non_blank(session);
    const std::size_t before = fixture.selected_event_count(session);

    try {
        (void)fixture.host().selectAgent(
            nlohmann::json{{"session", session}, {"agent", "second"}});
        FAIL() << "non-blank session must be rejected";
    } catch (const protocol::RpcException& error) {
        EXPECT_EQ(error.code(), protocol::code_value(protocol::AppCode::CompositionFixed));
        EXPECT_EQ(error.code(), -32020);
    }
    EXPECT_EQ(fixture.selected_event_count(session), before);
}

// 45-D9.7: session.create copies agent_preset into the header and agent.list
// reports it as active.
TEST(AgentRpcTest, UI45_D9_PreferredAgentOnCreate) {
    AgentRpcFixture fixture("agent-create-preset", {"standard", "second"});
    const protocol::SessionCreated created =
        fixture.host().createSession(nlohmann::json{{"title", "t"}, {"agent_preset", "second"}});

    const nlohmann::json result =
        fixture.host().listAgents(nlohmann::json{{"session", created.session.value}});
    EXPECT_EQ(result.at("active").get<std::string>(), "second");
}

// 45-D9.10/45-I27: the protocol version stays 1 and CompositionFixed is the
// next free server-range code.
TEST(AgentRpcTest, UI45_D9_ProtocolVersionRetained) {
    EXPECT_EQ(protocol::kProtocolVersion, 1u);
    EXPECT_EQ(protocol::code_value(protocol::AppCode::CompositionFixed), -32020);
    EXPECT_TRUE(protocol::is_known_method(protocol::method::kAgentList));
    EXPECT_TRUE(protocol::is_known_method(protocol::method::kAgentSelect));
}

} // namespace
