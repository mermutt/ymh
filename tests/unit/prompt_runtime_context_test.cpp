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

TEST(PromptRuntimeContext, ProducerRendersHeaderAndSources) {
    SystemPrompt prompt;
    const ContextHandle handle = register_runtime_context(
        prompt, RuntimeContextConfig{"/workspace", "deepseek-flash"},
        [] { return std::string{"2026-09-19"}; });

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.contexts.size(), 1u);
    EXPECT_EQ(assembly.contexts[0].name, "runtime-context");
    EXPECT_EQ(assembly.contexts[0].text.rfind(std::string{kRuntimeContextHeader}, 0), 0u);
    EXPECT_NE(assembly.contexts[0].text.find("/workspace"), std::string::npos);
    EXPECT_NE(assembly.contexts[0].text.find("deepseek-flash"), std::string::npos);
    EXPECT_NE(assembly.contexts[0].text.find("2026-09-19"), std::string::npos);
    EXPECT_EQ(prompt.render(AssembleContext{}), "");
}

TEST(PromptRuntimeContext, MaterializedOnceAndOnlyOnChangedText) {
    std::string date = "2026-09-19";
    SystemPrompt prompt;
    const ContextHandle handle = register_runtime_context(
        prompt, RuntimeContextConfig{"/workspace", "deepseek-flash"}, [&date] { return date; });

    AgentEnv env("prompt_runtime_timing",
                 std::make_unique<FakeLLM>(
                     script_of({text_step("one"), text_step("two"), text_step("three"),
                                text_step("four")})),
                 AgentConfig{}, allow_all_permission_config(), {}, false, 4, nullptr, false,
                 std::nullopt, std::chrono::system_clock::now, false, &prompt);
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ASSERT_EQ(agent.send(user_message("a")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("b")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("c")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    std::vector<payload::ContextInjected> injected = injected_contexts(session_owner->events());
    ASSERT_EQ(injected.size(), 1u);
    EXPECT_EQ(injected[0].role, Role::User);
    EXPECT_NE(injected[0].text.find("2026-09-19"), std::string::npos);

    date = "2026-09-20";
    ASSERT_EQ(agent.send(user_message("d")), InboxResult::Accepted);
    injected = injected_contexts(session_owner->events());
    ASSERT_EQ(injected.size(), 2u);
    EXPECT_EQ(injected[1].role, Role::User);
    EXPECT_NE(injected[1].text.find("2026-09-20"), std::string::npos);
}

TEST(PromptRuntimeContext, InstructionsMaterializedOnceAtFirstRequest) {
    TempWorkspace instructions_root("prompt_instr_loop");
    ScopedXdg     xdg(instructions_root.path().string());
    instructions_root.write(".git", "");
    instructions_root.write("AGENTS.md", "workspace rules");
    LocalEnvironment  instruction_env(instructions_root.path());
    InstructionFileConfig config;
    config.max_bytes = 4096;
    InstructionLoader loader(config, instruction_env);

    AgentEnv env("prompt_instr_loop_agent",
                 std::make_unique<FakeLLM>(script_of({text_step("one"), text_step("two")})),
                 AgentConfig{}, allow_all_permission_config(), {}, false, 4, nullptr, false,
                 std::nullopt, std::chrono::system_clock::now, false, nullptr, &loader);
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ASSERT_EQ(agent.send(user_message("a")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("b")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    const std::vector<payload::ContextInjected> injected =
        injected_contexts(session_owner->events());
    ASSERT_EQ(injected.size(), 1u);
    EXPECT_EQ(injected[0].role, Role::User);
    EXPECT_EQ(injected[0].text.rfind("<system-reminder>", 0), 0u);
    EXPECT_NE(injected[0].text.find("workspace rules"), std::string::npos);
}

} // namespace
