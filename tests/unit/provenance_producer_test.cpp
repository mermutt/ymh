#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/prompt/instructions.hpp"
#include "ymh/prompt/runtime_context.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/session/session.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

class ScopedXdg {
public:
    explicit ScopedXdg(std::string value) {
        if (const char* previous = ::getenv("XDG_CONFIG_HOME"); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv("XDG_CONFIG_HOME", value.c_str(), 1);
    }
    ~ScopedXdg() {
        if (previous_.has_value()) {
            ::setenv("XDG_CONFIG_HOME", previous_->c_str(), 1);
        } else {
            ::unsetenv("XDG_CONFIG_HOME");
        }
    }

    ScopedXdg(const ScopedXdg&) = delete;
    ScopedXdg& operator=(const ScopedXdg&) = delete;

private:
    std::optional<std::string> previous_;
};

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

FakeResponseStep tool_step(std::string name, nlohmann::json args) {
    FakeResponseStep step;
    step.tool_calls.push_back(FakeToolCallStep{std::move(name), std::move(args), std::nullopt});
    step.finish = FinishReason::ToolCalls;
    return step;
}

AgentConfig fake_config() {
    AgentConfig config;
    config.provider = "fake";
    config.model    = "fake-model";
    return config;
}

std::vector<payload::ContextInjected> injected_contexts(const EventRange& events) {
    std::vector<payload::ContextInjected> injected;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::ContextInjected) {
            continue;
        }
        injected.push_back(record.event.payload.get<payload::ContextInjected>());
    }
    return injected;
}

TEST(ProvenanceProducer, UserAndAssistantSource) {
    AgentEnv env("prov_assistant",
                 std::make_unique<FakeLLM>(script_of({text_step("done")})), fake_config());
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const EventRange events = env.sessionOf(agent)->events();
    bool saw_user = false;
    bool saw_assistant = false;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::UserMessage) {
            const auto& user = record.event.payload.get<payload::UserMessage>();
            EXPECT_EQ(user.source.kind, MessageSource::Kind::User);
            saw_user = true;
        } else if (record.event.type == EventType::AssistantMessage) {
            const auto& assistant = record.event.payload.get<payload::AssistantMessage>();
            EXPECT_EQ(assistant.source.kind, MessageSource::Kind::Model);
            EXPECT_EQ(assistant.source.provider, "fake");
            EXPECT_EQ(assistant.source.model, "fake-model");
            saw_assistant = true;
        }
    }
    EXPECT_TRUE(saw_user);
    EXPECT_TRUE(saw_assistant);
}

TEST(ProvenanceProducer, ToolResultSourceMirrorsCallId) {
    AgentEnv env("prov_tool",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")})),
                 fake_config(), allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "file-body");

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    const EventRange events = env.sessionOf(agent)->events();
    std::size_t results = 0;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::ToolResult) {
            continue;
        }
        const auto& result = record.event.payload.get<payload::ToolResult>();
        EXPECT_EQ(result.source.kind, MessageSource::Kind::Tool);
        ASSERT_TRUE(result.source.call.has_value());
        EXPECT_EQ(*result.source.call, result.id);
        ++results;
    }
    EXPECT_EQ(results, 1u);
}

TEST(ProvenanceProducer, InstructionsCarryPluginAndForm) {
    TempWorkspace root("prov_instr");
    ScopedXdg     xdg(root.path().string());
    root.write(".git", "");
    root.write("AGENTS.md", "workspace rules");
    LocalEnvironment  instruction_env(root.path());
    InstructionFileConfig config;
    config.max_bytes = 4096;
    InstructionLoader loader(config, instruction_env);

    AgentEnv env("prov_instr_agent",
                 std::make_unique<FakeLLM>(script_of({text_step("done")})), fake_config(),
                 allow_all_permission_config(), {}, false, 4, nullptr, false, std::nullopt,
                 std::chrono::system_clock::now, false, nullptr, &loader);
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const std::vector<payload::ContextInjected> injected =
        injected_contexts(env.sessionOf(agent)->events());
    ASSERT_EQ(injected.size(), 1u);
    EXPECT_EQ(injected[0].role, Role::User);
    EXPECT_EQ(injected[0].source.kind, MessageSource::Kind::Plugin);
    EXPECT_EQ(injected[0].source.plugin, "agent-instructions");
    EXPECT_EQ(injected[0].context.form, ContextForm::Instructions);
    EXPECT_TRUE(injected[0].source.context == injected[0].context);
}

TEST(ProvenanceProducer, RuntimeContextCarriesSnapshotSections) {
    SystemPrompt prompt;
    [[maybe_unused]] const ContextHandle handle =
        register_runtime_context(prompt, RuntimeContextConfig{"/workspace", "fake-model"},
                                 [] { return std::string{"2026-09-19"}; });

    AgentEnv env("prov_ctx",
                 std::make_unique<FakeLLM>(script_of({text_step("done")})), fake_config(),
                 allow_all_permission_config(), {}, false, 4, nullptr, false, std::nullopt,
                 std::chrono::system_clock::now, false, &prompt);
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const std::vector<payload::ContextInjected> injected =
        injected_contexts(env.sessionOf(agent)->events());
    ASSERT_EQ(injected.size(), 1u);
    EXPECT_EQ(injected[0].role, Role::User);
    EXPECT_EQ(injected[0].source.plugin, "runtime-context");
    ASSERT_EQ(injected[0].context.form, ContextForm::Snapshot);
    EXPECT_FALSE(injected[0].context.sections.empty());
    EXPECT_TRUE(injected[0].source.context == injected[0].context);
}

TEST(ProvenanceProducer, InjectDefaultsToAgentPlugin) {
    AgentEnv env("prov_inject",
                 std::make_unique<FakeLLM>(script_of({text_step("done")})), fake_config());
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ContextMessage context;
    context.role       = Role::User;
    context.text       = "caller context";
    context.startsTurn = false;
    ASSERT_EQ(agent.inject(context), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const std::vector<payload::ContextInjected> injected =
        injected_contexts(env.sessionOf(agent)->events());
    ASSERT_EQ(injected.size(), 1u);
    EXPECT_EQ(injected[0].source.kind, MessageSource::Kind::Plugin);
    EXPECT_EQ(injected[0].source.plugin, "agent");
    EXPECT_EQ(injected[0].context.form, ContextForm::None);
}

TEST(ProvenanceProducer, InjectCarriesCallerCatalogProvenance) {
    AgentEnv env("prov_inject_catalog",
                 std::make_unique<FakeLLM>(script_of({text_step("done")})), fake_config());
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ContextMessage context;
    context.role       = Role::User;
    context.text       = "skill catalog";
    context.startsTurn = false;
    context.context    = ContextFormed{ContextForm::Catalog};
    context.source     = plugin_message_source("skill-catalog", context.context);
    ASSERT_EQ(agent.inject(context), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const std::vector<payload::ContextInjected> injected =
        injected_contexts(env.sessionOf(agent)->events());
    ASSERT_EQ(injected.size(), 1u);
    EXPECT_EQ(injected[0].source.plugin, "skill-catalog");
    EXPECT_EQ(injected[0].context.form, ContextForm::Catalog);
}

} // namespace
