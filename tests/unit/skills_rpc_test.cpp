#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/turn_executor.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/host/host_runtime.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/permission/permission_transport.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/skills/workspace_trust.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

void write_skill(const std::filesystem::path& root, const std::string& name,
                 const std::string& description, const std::string& body = "Body.\n") {
    const std::filesystem::path directory = root / name;
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "SKILL.md")
        << "---\nname: " << name << "\ndescription: " << description + "\n---\n" << body;
}

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

class SkillsRpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggingOptions options;
        options.level = LogLevel::Error;
        init_logging(options);
        if (const char* existing = std::getenv("XDG_CONFIG_HOME"); existing != nullptr) {
            previous_xdg_ = existing;
        }
    }

    void TearDown() override {
        if (previous_xdg_.has_value()) {
            ::setenv("XDG_CONFIG_HOME", previous_xdg_->c_str(), 1);
        } else {
            ::unsetenv("XDG_CONFIG_HOME");
        }
    }

    std::optional<std::string> previous_xdg_;
};

TEST_F(SkillsRpcTest, ListAndShowReflectDaemonCatalog) {
    TempWorkspace workspace("skills_rpc_ws");
    TempWorkspace config_root("skills_rpc_cfg");
    write_skill(config_root.path() / "ymh" / "skills", "git-commit", "Commit helper.",
                "Body text.\n");
    write_skill(workspace.path() / ".ymh" / "skills", "repo", "Repo skill.");
    const std::filesystem::path broken = workspace.path() / ".ymh" / "skills" / "broken";
    std::filesystem::create_directories(broken);
    std::ofstream(broken / "SKILL.md") << "name: broken\ndescription: no fence\n";
    ::setenv("XDG_CONFIG_HOME", config_root.path().c_str(), 1);
    (void)WorkspaceTrustStore{}.trust(workspace.path());

    TempWorkspace registry_dir("skills_rpc_reg");
    RegistryConfig registry_config;
    registry_config.db_path = registry_dir.path() / "registry.db";
    registry_config.lock_path = registry_dir.path() / "registry.lock";
    registry_config.workspace_roots = {};
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
    const WorkspaceRecord record =
        registry->registerWorkspace(std::filesystem::canonical(workspace.path()), "skills-rpc");

    WorkspaceRuntimeOptions options;
    options.config = Config{};
    options.root = std::filesystem::canonical(workspace.path());
    options.boot_id = BootId{"skills-rpc-boot"};
    options.store_factory = [] { return std::make_unique<MemorySessionStore>(); };
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        FakeScript script;
        script.steps.push_back(text_step("ok"));
        return std::make_unique<FakeLLM>(script);
    };
    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
        make_workspace_runtime(std::move(options));
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    std::unique_ptr<WorkspaceRuntime> runtime = std::move(*created);

    NullPermissionTransport transport;
    PermissionBroker        broker(runtime->policy(), transport, PermissionConfig{});
    TurnExecutor            turns(1, 1);

    HostIdentity identity;
    identity.workspace   = record.id;
    identity.boot_id     = HostBootId{"skills-rpc-boot"};
    identity.pid         = 4242;
    identity.socket_path = workspace.path() / ".ymh" / "host.sock";

    HostRuntime host(*runtime, *registry, identity, turns, broker);

    const nlohmann::json list = host.listSkills();
    ASSERT_TRUE(list.contains("skills"));
    const nlohmann::json& skills = list.at("skills");
    ASSERT_TRUE(skills.is_array());
    ASSERT_EQ(skills.size(), 2u);
    EXPECT_EQ(skills.at(0).at("name"), "git-commit");
    EXPECT_EQ(skills.at(0).at("trust"), "trusted");
    EXPECT_EQ(skills.at(0).at("source"), "user");
    EXPECT_EQ(skills.at(1).at("name"), "repo");
    EXPECT_EQ(skills.at(1).at("trust"), "untrusted");
    EXPECT_EQ(skills.at(1).at("source"), "workspace");

    const nlohmann::json& warnings = list.at("warnings");
    ASSERT_TRUE(warnings.is_array());
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings.at(0).at("reason").get<std::string>().find("missing opening"),
              std::string::npos);

    const nlohmann::json show = host.showSkill("git-commit");
    EXPECT_EQ(show.at("name"), "git-commit");
    EXPECT_EQ(show.at("body"), "Body text.\n");

    EXPECT_THROW(host.showSkill("does-not-exist"), protocol::RpcException);

    // 24-D3/AL10: the executor must be quiesced before its destructor (which
    // hard-exits on a non-quiesced state), even when no turn was ever submitted.
    (void)turns.drain(std::chrono::milliseconds{1000});
}

} // namespace
