#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
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

class RecordingProvider final : public LLMProvider {
public:
    explicit RecordingProvider(FakeScript script) : fake_(std::move(script)) {}

    ProviderId id() const override { return "recording"; }
    ProviderCapabilities capabilities() const override { return fake_.capabilities(); }
    std::vector<ModelInfo> models() const override { return fake_.models(); }

    Task<LLMResponse> stream(const LLMRequest& request, StreamSink sink,
                             CancellationToken cancel) override {
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        return fake_.stream(request, std::move(sink), cancel);
    }

    std::size_t count() const {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }

    LLMRequest at(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return requests_.at(index);
    }

private:
    mutable std::mutex      mutex_;
    std::vector<LLMRequest> requests_;
    FakeLLM                 fake_;
};

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

TEST(PromptRuntimeContext, RendersSandboxApprovalAndDelegationFacts) {
    const std::string text = render_runtime_context(
        RuntimeContextConfig{.cwd        = "/ws",
                             .model      = "m",
                             .sandbox    = "workspace",
                             .approval   = "workspace-write",
                             .delegation = "available (max depth 3)"},
        "2026-09-19");
    EXPECT_NE(text.find("- Sandbox: workspace"), std::string::npos);
    EXPECT_NE(text.find("- Approval: workspace-write"), std::string::npos);
    EXPECT_NE(text.find("- Delegation: available (max depth 3)"), std::string::npos);

    const std::string bare = render_runtime_context(
        RuntimeContextConfig{.cwd = "/ws", .model = "m", .sandbox = {}, .approval = {},
                             .delegation = {}},
        "2026-09-19");
    EXPECT_EQ(bare.find("- Sandbox:"), std::string::npos);
    EXPECT_EQ(bare.find("- Approval:"), std::string::npos);
    EXPECT_EQ(bare.find("- Delegation:"), std::string::npos);
}

TEST(PromptRuntimeContext, ProducerRendersHeaderAndSources) {
    SystemPrompt prompt;
    const ContextHandle handle = register_runtime_context(
        prompt, RuntimeContextConfig{.cwd = "/workspace", .model = "deepseek-flash", .sandbox = {}, .approval = {}, .delegation = {}},
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
        prompt, RuntimeContextConfig{.cwd = "/workspace", .model = "deepseek-flash", .sandbox = {}, .approval = {}, .delegation = {}},
        [&date] { return date; });

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

TEST(PromptRuntimeContext, UserPromptIsLastUserMessageOnFirstTurn) {
    SystemPrompt prompt;
    [[maybe_unused]] const ContextHandle handle = register_runtime_context(
        prompt, RuntimeContextConfig{.cwd = "/workspace", .model = "fake-model", .sandbox = {}, .approval = {}, .delegation = {}},
        [] { return std::string{"2026-09-19"}; });

    auto               provider     = std::make_unique<RecordingProvider>(script_of({text_step("ok")}));
    RecordingProvider* provider_raw = provider.get();
    AgentEnv env("prompt_runtime_order", std::move(provider), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, nullptr, false, std::nullopt,
                 std::chrono::system_clock::now, false, &prompt);
    auto  agent_owner = env.createAgent();
    Agent& agent      = *agent_owner;

    ASSERT_EQ(agent.send(user_message("what is the capital of France?")), InboxResult::Accepted);
    ASSERT_EQ(provider_raw->count(), 1u);

    const std::vector<Message> messages = provider_raw->at(0).messages;
    std::optional<std::size_t> prompt_index;
    std::optional<std::size_t> context_index;
    std::size_t                context_count = 0;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role != Role::User) {
            continue;
        }
        const std::string text =
            messages[i].content.empty() ? std::string{} : messages[i].content.at(0).text;
        if (text == "what is the capital of France?") {
            prompt_index = i;
        }
        if (text.rfind(std::string{kRuntimeContextHeader}, 0) == 0) {
            ++context_count;
            context_index = i;
        }
    }

    ASSERT_TRUE(prompt_index.has_value());
    EXPECT_EQ(*prompt_index, messages.size() - 1);
    ASSERT_TRUE(context_index.has_value());
    EXPECT_LT(*context_index, *prompt_index);
    EXPECT_EQ(context_count, 1u);
}

} // namespace
