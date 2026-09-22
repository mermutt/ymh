#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/skills/skill_catalog.hpp"
#include "ymh/skills/skill_tool.hpp"
#include "ymh/skills/workspace_trust.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

void write_skill(const std::filesystem::path& root, const std::string& name,
                 const std::string& description, const std::string& body = "Body.\n") {
    const std::filesystem::path directory = root / name;
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "SKILL.md")
        << "---\nname: " << name << "\ndescription: " << description << "\n---\n" << body;
}

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

FakeResponseStep tool_step(std::string name, nlohmann::json args) {
    FakeResponseStep step;
    step.tool_calls.push_back(FakeToolCallStep{std::move(name), std::move(args), std::nullopt});
    step.finish = FinishReason::ToolCalls;
    return step;
}

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
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

std::optional<std::size_t> first_index(const EventRange& events, EventType type) {
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (events[index].event.type == type) {
            return index;
        }
    }
    return std::nullopt;
}

std::string system_text_of(const std::vector<Message>& messages) {
    if (messages.empty() || messages.front().role != Role::System) {
        return {};
    }
    std::string text;
    for (const ContentBlock& block : messages.front().content) {
        text += block.text;
    }
    return text;
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* existing = std::getenv(name); existing != nullptr) {
            previous_ = existing;
        }
        ::setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string                name_;
    std::optional<std::string> previous_;
};

WorkspaceRuntimeOptions runtime_options_for(const TempWorkspace& workspace, bool attach_gate,
                                            bool attach_resolver) {
    WorkspaceRuntimeOptions options;
    options.config                   = Config{};
    options.root                     = workspace.path();
    options.boot_id                  = BootId{"skills-wiring-boot"};
    options.attach_permission_gate   = attach_gate;
    options.attach_permission_resolver = attach_resolver;
    options.store_factory            = [] { return std::make_unique<MemorySessionStore>(); };
    options.provider_factory         = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(script_of({text_step("ok")}));
    };
    return options;
}

SessionId create_session(WorkspaceRuntime& runtime) {
    SessionOptions session_options;
    session_options.cwd           = runtime.root();
    session_options.serverProfile = "interactive";
    session_options.model         = "fake-model";
    session_options.title         = "skills test";
    const std::expected<AgentId, AgentError> created = runtime.agents().create(session_options);
    if (!created.has_value()) {
        throw std::runtime_error("create failed: " + created.error().detail);
    }
    return runtime.agents().getShared(*created)->session();
}

class SkillsWiringTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggingOptions options;
        options.level = LogLevel::Error;
        init_logging(options);
    }
};

TEST_F(SkillsWiringTest, IndexReachesAssembledSystemPrompt) {
    TempWorkspace workspace("skills_t1_ws");
    TempWorkspace config_root("skills_t1_cfg");
    const std::filesystem::path skills_root = config_root.path() / "ymh" / "skills";
    write_skill(skills_root, "git-commit", "Write a conventional commit.");
    ScopedEnv xdg("XDG_CONFIG_HOME", config_root.path().string());

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
        make_workspace_runtime(runtime_options_for(workspace, true, false));
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    WorkspaceRuntime& runtime = **created;

    EXPECT_TRUE(runtime.tools().contains(ToolName{"skill"}));
    EXPECT_NE(runtime.agent_config().system_prompt.find("git-commit"), std::string::npos);

    const SessionId session_id = create_session(runtime);
    auto session_owner = runtime.sessions().sessionPtr(session_id);
    Session& session = *session_owner;
    const std::vector<Message> messages = runtime.context().assemble(session, TurnContext{});

    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages.front().role, Role::System);
    const std::string text = system_text_of(messages);
    EXPECT_NE(text.find("git-commit"), std::string::npos);
    EXPECT_NE(text.find("Write a conventional commit."), std::string::npos);
}

TEST_F(SkillsWiringTest, IndexContributesTokens) {
    TempWorkspace workspace("skills_tokens_ws");
    TempWorkspace config_root("skills_tokens_cfg");
    write_skill(config_root.path() / "ymh" / "skills", "git-commit", "Write a commit message.");
    ScopedEnv xdg("XDG_CONFIG_HOME", config_root.path().string());

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
        make_workspace_runtime(runtime_options_for(workspace, true, false));
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    WorkspaceRuntime& runtime = **created;
    const SessionId   session_id = create_session(runtime);
    auto session_owner = runtime.sessions().sessionPtr(session_id);
    Session& session = *session_owner;

    SessionContextAssembler with_index(runtime.tools(), runtime.agent_config().system_prompt);
    SessionContextAssembler without_index(runtime.tools(), default_system_prompt());
    DefaultTokenEstimator    estimator;
    const std::size_t with = estimator.estimate(with_index.assemble(session, TurnContext{}));
    const std::size_t without =
        estimator.estimate(without_index.assemble(session, TurnContext{}));
    EXPECT_GT(with, without);
}

TEST_F(SkillsWiringTest, ResolverPathRegistersAndAdvertises) {
    TempWorkspace workspace("skills_resolver_ws");
    TempWorkspace config_root("skills_resolver_cfg");
    write_skill(config_root.path() / "ymh" / "skills", "git-commit", "Commit helper.");
    ScopedEnv xdg("XDG_CONFIG_HOME", config_root.path().string());

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
        make_workspace_runtime(runtime_options_for(workspace, false, true));
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    WorkspaceRuntime& runtime = **created;

    EXPECT_TRUE(runtime.tools().contains(ToolName{"skill"}));
    EXPECT_NE(runtime.agent_config().system_prompt.find("git-commit"), std::string::npos);
}

TEST_F(SkillsWiringTest, HeadlessIsNotAdvertised) {
    TempWorkspace workspace("skills_headless_ws");
    TempWorkspace config_root("skills_headless_cfg");
    write_skill(config_root.path() / "ymh" / "skills", "git-commit", "Commit helper.");
    ScopedEnv xdg("XDG_CONFIG_HOME", config_root.path().string());

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
        make_workspace_runtime(runtime_options_for(workspace, false, false));
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    WorkspaceRuntime& runtime = **created;

    EXPECT_FALSE(runtime.tools().contains(ToolName{"skill"}));
    EXPECT_EQ(runtime.agent_config().system_prompt.find("git-commit"), std::string::npos);
}

TEST(SkillUsability, DenyWinsEvenWithPromptPath) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    PolicyRule rule;
    rule.tool   = "skill";
    rule.effect = PolicyVerdict::Deny;
    config.rules.push_back(rule);
    RulePermissionPolicy policy(config);
    EXPECT_FALSE(skill_tool_usable(policy, true));
    EXPECT_FALSE(skill_tool_usable(policy, false));
}

TEST(SkillUsability, AllowNeedsNoPromptPath) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    PolicyRule rule;
    rule.tool   = "skill";
    rule.effect = PolicyVerdict::Allow;
    config.rules.push_back(rule);
    RulePermissionPolicy policy(config);
    EXPECT_TRUE(skill_tool_usable(policy, false));
}

TEST(SkillUsability, AskNeedsPromptPath) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    RulePermissionPolicy policy(config);
    EXPECT_TRUE(skill_tool_usable(policy, true));
    EXPECT_FALSE(skill_tool_usable(policy, false));
}

TEST_F(SkillsWiringTest, ToolPathInjectsBodyIntoContext) {
    AgentEnv env("skills_tool_path",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("skill", nlohmann::json{{"name", "git-commit"}}),
                      text_step("done")})),
                 AgentConfig{}, allow_all_permission_config());
    TempWorkspace config_root("skills_tool_path_cfg");
    const std::filesystem::path user_root = config_root.path() / "skills";
    write_skill(user_root, "git-commit", "Commit helper.", "Use conventional commits.\n");

    auto catalog = std::make_shared<SkillCatalog>(
        SkillCatalogConfig{}, env.env,
        std::vector<SkillRoot>{SkillRoot{user_root, SkillSource::User, SkillTrust::Trusted}},
        env.logger);
    catalog->discover();
    ASSERT_EQ(catalog->all().size(), 1u);
    env.keeper.add(make_skill_tool(catalog));
    env.tools.freeze();

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::ToolCall), 1u);
    EXPECT_EQ(count_type(events, EventType::PermissionDecision), 1u);
    EXPECT_EQ(count_type(events, EventType::ToolResult), 1u);

    const std::optional<std::size_t> permission = first_index(events, EventType::PermissionDecision);
    const std::optional<std::size_t> result     = first_index(events, EventType::ToolResult);
    ASSERT_TRUE(permission.has_value());
    ASSERT_TRUE(result.has_value());
    EXPECT_LT(*permission, *result);

    bool body_in_tool_message = false;
    for (const Message& message : session.deriveMessages()) {
        if (message.role != Role::Tool) {
            continue;
        }
        for (const ContentBlock& block : message.content) {
            if (block.text.find("Use conventional commits.") != std::string::npos) {
                body_in_tool_message = true;
            }
        }
    }
    EXPECT_TRUE(body_in_tool_message);
}

TEST_F(SkillsWiringTest, UntrustedSkillNeverReachesContext) {
    AgentEnv env("skills_untrusted",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("skill", nlohmann::json{{"name", "repo"}}),
                      text_step("done")})),
                 AgentConfig{}, allow_all_permission_config());
    TempWorkspace config_root("skills_untrusted_cfg");
    const std::filesystem::path user_root = config_root.path() / "skills";
    std::filesystem::create_directories(user_root);
    write_skill(env.workspace.path() / ".ymh" / "skills", "repo", "Repo skill.", "Hostile.\n");

    SkillCatalogConfig catalog_config;
    catalog_config.workspace_trusted = true;
    auto catalog = std::make_shared<SkillCatalog>(
        catalog_config, env.env,
        std::vector<SkillRoot>{
            SkillRoot{user_root, SkillSource::User, SkillTrust::Trusted},
            SkillRoot{env.workspace.path() / ".ymh" / "skills", SkillSource::Workspace,
                      SkillTrust::Untrusted}},
        env.logger);
    catalog->discover();
    env.keeper.add(make_skill_tool(catalog));
    env.tools.freeze();

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    for (const Message& message : session.deriveMessages()) {
        for (const ContentBlock& block : message.content) {
            EXPECT_EQ(block.text.find("Hostile."), std::string::npos);
        }
    }
}

TEST_F(SkillsWiringTest, CommandPathInjectsSystemMessage) {
    AgentEnv env("skills_cmd_path",
                 std::make_unique<FakeLLM>(script_of({text_step("done")})),
                 AgentConfig{}, allow_all_permission_config());
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ContextMessage injection;
    injection.role       = Role::System;
    injection.text       = "[skill: git-commit]\nUse conventional commits.\n";
    injection.startsTurn = false;
    ASSERT_EQ(agent.inject(injection), InboxResult::Accepted);
    EXPECT_FALSE(agent.hasPendingWork());

    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::ContextInjected), 1u);

    const std::vector<Message> messages = session.deriveMessages();
    std::size_t                injected_index = messages.size();
    std::size_t                user_index     = messages.size();
    for (std::size_t index = 0; index < messages.size(); ++index) {
        for (const ContentBlock& block : messages[index].content) {
            if (block.text.find("Use conventional commits.") != std::string::npos &&
                index < injected_index) {
                injected_index = index;
            }
            if (block.text == "go" && index < user_index) {
                user_index = index;
            }
        }
    }
    EXPECT_LT(injected_index, user_index);
}

TEST_F(SkillsWiringTest, WorkspaceSkillRequiresAnOutsideTrustRecord) {
    TempWorkspace workspace("skills_trust_gate_ws");
    TempWorkspace home("skills_trust_gate_home");
    TempWorkspace state("skills_trust_gate_state");
    TempWorkspace config_root("skills_trust_gate_cfg");
    write_skill(workspace.path() / ".ymh" / "skills", "repo", "Hostile repo skill.");
    ScopedEnv home_env("HOME", home.path().string());
    ScopedEnv state_env("XDG_STATE_HOME", state.path().string());
    ScopedEnv xdg_env("XDG_CONFIG_HOME", config_root.path().string());

    {
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(runtime_options_for(workspace, true, false));
        ASSERT_TRUE(created.has_value()) << created.error().detail;
        WorkspaceRuntime& runtime = **created;
        EXPECT_EQ(runtime.skills().find("repo"), nullptr);
        EXPECT_TRUE(runtime.skills().all().empty());
    }

    ASSERT_TRUE(WorkspaceTrustStore{}.trust(workspace.path()));

    {
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(runtime_options_for(workspace, true, false));
        ASSERT_TRUE(created.has_value()) << created.error().detail;
        WorkspaceRuntime& runtime = **created;
        const Skill*      skill   = runtime.skills().find("repo");
        ASSERT_NE(skill, nullptr);
        EXPECT_EQ(skill->trust, SkillTrust::Untrusted);
    }
}

} // namespace
