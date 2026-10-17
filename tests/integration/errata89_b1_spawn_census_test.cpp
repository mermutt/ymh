// Spec 89 (rev 3) B1 / SG-F23 (89-D18): the spawn census. Every
// `{shell,terminal}` execution goes through `ToolRegistry::execute`, whose only
// caller is `agent_loop.cpp` after the permission decision block. The dynamic
// half instruments the dispatch and asserts an `evaluate` preceded each spawn;
// the static half proves the single-chokepoint structure and enumerates the
// delegating paths (subagent/job/plan/headless) plus the shipped
// `PermissionBroker` resolver. MCP is confirmed OUT of the confinement claim.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/events.hpp"
#include "ymh/tools/tool.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

namespace fs = std::filesystem;

// --- dynamic instrumentation ------------------------------------------------

// Decorates the real `RulePermissionPolicy` and counts every `evaluate`. The
// target is bound after `AgentEnv` is built (the env owns the real policy); no
// evaluate runs during construction.
class CountingPolicy final : public PermissionPolicy {
public:
    void set_target(PermissionPolicy* target) { target_ = target; }

    PolicyVerdict evaluate(const PermissionRequest& request) const override {
        ++evaluate_calls;
        return target_->evaluate(request);
    }

    PolicyVerdict evaluate(const PermissionRequest& request, std::string* reason) const override {
        ++evaluate_calls;
        return target_->evaluate(request, reason);
    }

    PolicyRule remember(const PermissionRequest&            request,
                        payload::PermissionDecisionKind    kind,
                        GrantScope                        scope) override {
        return target_->remember(request, kind, scope);
    }

    PolicyGeneration generation() const noexcept override { return target_->generation(); }

    const std::vector<PolicyRule>& rules() const noexcept override { return target_->rules(); }

    mutable std::atomic<int> evaluate_calls{0};

private:
    PermissionPolicy* target_ = nullptr;
};

// A `{shell,terminal}` stand-in that records, at the moment ToolRegistry
// dispatches it, how many `evaluate` calls had already happened.
class RecordingSpawnTool final : public Tool {
public:
    RecordingSpawnTool(std::string        name,
                       const CountingPolicy* policy,
                       std::atomic<int>*  spawned,
                       std::atomic<int>*  evaluate_at_spawn)
        : name_(std::move(name)),
          policy_(policy),
          spawned_(spawned),
          evaluate_at_spawn_(evaluate_at_spawn) {}

    ToolSchema schema() const override {
        nlohmann::json properties;
        if (name_ == "terminal") {
            properties = {{"action", {{"type", "string"}}},
                          {"command", {{"type", "string"}}},
                          {"input", {{"type", "string"}}}};
        } else {
            properties = {{"command", {{"type", "string"}}}};
        }
        return ToolSchema{ToolName{name_},
                          ToolVersion{1, 0},
                          "spawn census recording tool",
                          nlohmann::json{{"type", "object"},
                                         {"properties", properties},
                                         {"required", nlohmann::json::array()},
                                         {"additionalProperties", false}},
                          true};
    }

    Task<ToolResult> execute(const ToolContext& context, const ToolArguments&) override {
        ++*spawned_;
        *evaluate_at_spawn_ = policy_->evaluate_calls.load();
        ToolResult result;
        result.id      = context.callId();
        result.name    = name_;
        result.outcome = payload::ToolOutcome::Ok;
        result.output  = "spawned";
        return Task<ToolResult>(std::move(result));
    }

private:
    std::string           name_;
    const CountingPolicy* policy_;
    std::atomic<int>*     spawned_;
    std::atomic<int>*     evaluate_at_spawn_;
};

// The shipped (daemon/headless) resolver path: a real PermissionBroker that
// broadcasts and is answered synchronously, so the turn proceeds to spawn.
class AutoApproveTransport final : public PermissionTransport {
public:
    PermissionBroker* broker = nullptr;
    int               broadcasts = 0;

    bool broadcast_permission_request(protocol::PermissionRequest request) override {
        ++broadcasts;
        if (broker != nullptr) {
            (void)broker->onDecision(protocol::PermissionDecisionParams{
                request.request_id, protocol::PermissionAnswer::Allow,
                protocol::PermissionScope::Once});
        }
        return true;
    }

    bool schedule_after(std::chrono::milliseconds, std::function<void()>) override {
        return true;
    }
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

TEST(Errata89B1, ShellExecutionIsPrecededByEvaluate) {
    CountingPolicy  counter;
    std::atomic<int> spawned{0};
    std::atomic<int> evaluate_at_spawn{0};

    AgentEnv env("b1_shell",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("shell", {{"command", "echo hi"}}), text_step("done")})),
                 AgentConfig{}, allow_all_permission_config(), {},
                 /*register_builtins=*/false, 4, nullptr, false, std::nullopt,
                 std::chrono::system_clock::now, false, nullptr, nullptr, nullptr,
                 ToolConfig{}, &counter);
    counter.set_target(&env.policy);
    env.keeper.add(
        std::make_unique<RecordingSpawnTool>("shell", &counter, &spawned, &evaluate_at_spawn));
    env.tools.freeze();

    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(spawned.load(), 1);
    EXPECT_GE(evaluate_at_spawn.load(), 1)
        << "a shell execution was dispatched before PermissionPolicy::evaluate ran";
    EXPECT_GE(counter.evaluate_calls.load(), 1);
}

TEST(Errata89B1, TerminalExecutionIsPrecededByEvaluate) {
    CountingPolicy  counter;
    std::atomic<int> spawned{0};
    std::atomic<int> evaluate_at_spawn{0};

    AgentEnv env("b1_terminal",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("terminal", {{"action", "open"}, {"command", "echo hi"}}),
                      text_step("done")})),
                 AgentConfig{}, allow_all_permission_config(), {},
                 /*register_builtins=*/false, 4, nullptr, false, std::nullopt,
                 std::chrono::system_clock::now, false, nullptr, nullptr, nullptr,
                 ToolConfig{}, &counter);
    counter.set_target(&env.policy);
    env.keeper.add(std::make_unique<RecordingSpawnTool>("terminal", &counter, &spawned,
                                                        &evaluate_at_spawn));
    env.tools.freeze();

    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(spawned.load(), 1);
    EXPECT_GE(evaluate_at_spawn.load(), 1)
        << "a terminal execution was dispatched before PermissionPolicy::evaluate ran";
}

// The incident command must be refused at the decision block: no spawn at all.
TEST(Errata89B1, IncidentCommandProducesNoSpawn) {
    CountingPolicy  counter;
    std::atomic<int> spawned{0};
    std::atomic<int> evaluate_at_spawn{0};

    AgentEnv env("b1_incident",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("shell", {{"command", "rm -fr build ~"}}), text_step("done")})),
                 AgentConfig{}, allow_all_permission_config(), {},
                 /*register_builtins=*/false, 4, nullptr, false, std::nullopt,
                 std::chrono::system_clock::now, false, nullptr, nullptr, nullptr,
                 ToolConfig{}, &counter);
    counter.set_target(&env.policy);
    env.keeper.add(
        std::make_unique<RecordingSpawnTool>("shell", &counter, &spawned, &evaluate_at_spawn));
    env.tools.freeze();

    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(spawned.load(), 0) << "the incident command must not reach the shell tool";
    EXPECT_GE(counter.evaluate_calls.load(), 1);

    bool denied        = false;
    bool guard_reason  = false;
    for (const EventRecord& record : env.sessionOf(*agent_owner)->events()) {
        if (record.event.type != EventType::PermissionDecision) {
            continue;
        }
        const auto& decision = record.event.payload.get<payload::PermissionDecision>();
        if (decision.decision == payload::PermissionDecisionKind::Deny) {
            denied = true;
            if (decision.reason == "shell-guard:outside-root") {
                guard_reason = true;
            }
        }
    }
    EXPECT_TRUE(denied);
    EXPECT_TRUE(guard_reason);
}

// The SHIPPED resolver path (`PermissionBroker`), not just the M1 gate: the
// loop's ask is resolved by a real broker, which itself evaluates before the
// spawn.
TEST(Errata89B1, BrokerBackedResolverEvaluatePrecedesSpawn) {
    CountingPolicy        counter;
    AutoApproveTransport  transport;
    PermissionConfig      permission;
    permission.default_verdict = PolicyVerdict::Ask;
    PermissionBroker broker(counter, transport, permission);
    transport.broker = &broker;
    broker.set_subscriber_count([](SessionId) { return std::size_t{1}; });

    std::atomic<int> spawned{0};
    std::atomic<int> evaluate_at_spawn{0};

    AgentServices::PermissionResolver resolver =
        [&broker](const PermissionRequest& request, CancellationToken token) -> PermissionOutcome {
        return broker.resolve(request, token).get();
    };

    AgentEnv env("b1_broker",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("shell", {{"command", "echo hi"}}), text_step("done")})),
                 AgentConfig{}, permission, std::move(resolver),
                 /*register_builtins=*/false, 4, nullptr, false, std::nullopt,
                 std::chrono::system_clock::now, false, nullptr, nullptr, nullptr,
                 ToolConfig{}, &counter);
    counter.set_target(&env.policy);
    env.keeper.add(
        std::make_unique<RecordingSpawnTool>("shell", &counter, &spawned, &evaluate_at_spawn));
    env.tools.freeze();

    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(transport.broadcasts, 1);
    EXPECT_EQ(spawned.load(), 1);
    EXPECT_GE(evaluate_at_spawn.load(), 2)
        << "the loop and the shipped PermissionBroker both evaluate before the spawn";
}

// --- static census ----------------------------------------------------------

std::string read_repo_file(const std::string& relative) {
    std::ifstream in(std::string{YMH_SOURCE_DIR} + "/" + relative, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::size_t occurrences(const std::string& haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::vector<fs::path> source_files() {
    std::vector<fs::path> files;
    const fs::path        root{std::string{YMH_SOURCE_DIR} + "/src"};
    std::error_code       ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".cpp") {
            files.push_back(it->path());
        }
    }
    return files;
}

TEST(Errata89B1, ToolExecuteIsTheSoleDispatchSite) {
    std::vector<std::string> offenders;
    for (const fs::path& file : source_files()) {
        if (contains(read_repo_file(file.lexically_relative(fs::path{YMH_SOURCE_DIR}).generic_string()),
                     "tool->execute(")) {
            offenders.push_back(file.filename().string());
        }
    }
    ASSERT_EQ(offenders.size(), 1u) << "unexpected Tool::execute call sites";
    EXPECT_EQ(offenders.front(), "tool_registry.cpp");

    const std::string registry = read_repo_file("src/tools/tool_registry.cpp");
    EXPECT_TRUE(contains(registry, "ToolResult result = tool->execute(context, arguments).get();"));
}

TEST(Errata89B1, AgentLoopIsSoleRegistryCallerAfterDecisionBlock) {
    std::size_t callers = 0;
    for (const fs::path& file : source_files()) {
        if (contains(read_repo_file(file.lexically_relative(fs::path{YMH_SOURCE_DIR}).generic_string()),
                     "services_.tools->execute(")) {
            ++callers;
        }
    }
    EXPECT_EQ(callers, 1u);

    const std::string loop = read_repo_file("src/agent/agent_loop.cpp");
    ASSERT_FALSE(loop.empty());
    ASSERT_EQ(occurrences(loop, "services_.tools->execute("), 1u);

    const std::size_t prepare_definition = loop.find("AgentLoop::prepareToolCall(");
    const std::size_t run_definition     = loop.find("AgentLoop::runToolCall(");
    const std::size_t execute_definition = loop.find("AgentLoop::executeToolCall(");
    ASSERT_NE(prepare_definition, std::string::npos);
    ASSERT_NE(run_definition, std::string::npos);
    ASSERT_NE(execute_definition, std::string::npos);
    EXPECT_LT(prepare_definition, run_definition);
    EXPECT_LT(run_definition, execute_definition);

    // The decision block (gate resolve + no-gate evaluate) lives in
    // prepareToolCall, which textually and dynamically precedes runToolCall.
    const std::size_t gate_resolve = loop.find("services_.gate->resolve(", prepare_definition);
    const std::size_t policy_eval  = loop.find("services_.policy->evaluate(", prepare_definition);
    ASSERT_NE(gate_resolve, std::string::npos);
    ASSERT_NE(policy_eval, std::string::npos);
    EXPECT_LT(gate_resolve, run_definition);
    EXPECT_LT(policy_eval, run_definition);

    // The sole dispatch call is inside runToolCall, before executeToolCall
    // returns.
    const std::size_t dispatch = loop.find("services_.tools->execute(", run_definition);
    ASSERT_NE(dispatch, std::string::npos);
    EXPECT_LT(dispatch, execute_definition);

    // executeToolCall prepares (decides) before it runs.
    const std::size_t prepared_call = loop.find("prepareToolCall(assembled", execute_definition);
    const std::size_t run_call      = loop.find("runToolCall(plan)", execute_definition);
    ASSERT_NE(prepared_call, std::string::npos);
    ASSERT_NE(run_call, std::string::npos);
    EXPECT_LT(prepared_call, run_call);
}

TEST(Errata89B1, DelegatingPathsRouteThroughAgentLoop) {
    for (const char* relative : {"src/tools/subagent_tools.cpp", "src/tools/job_tools.cpp",
                                 "src/tools/plan_tools.cpp", "src/cli/headless.cpp"}) {
        const std::string text = read_repo_file(relative);
        ASSERT_FALSE(text.empty()) << relative;
        EXPECT_EQ(text.find("ToolRegistry::execute"), std::string::npos) << relative;
        EXPECT_EQ(text.find("tools->execute("), std::string::npos) << relative;
        EXPECT_EQ(text.find("tool->execute("), std::string::npos) << relative;
    }

    EXPECT_TRUE(contains(read_repo_file("src/tools/subagent_tools.cpp"), "SubagentService"));
    EXPECT_TRUE(contains(read_repo_file("src/tools/job_tools.cpp"), "JobRegistry"));
    EXPECT_TRUE(contains(read_repo_file("src/cli/headless.cpp"), "PermissionBroker"));
    EXPECT_TRUE(contains(read_repo_file("src/cli/headless.cpp"), "set_permission_resolver"));
    EXPECT_TRUE(contains(read_repo_file("src/host/workspace_host.cpp"), "PermissionBroker"));
    EXPECT_TRUE(contains(read_repo_file("src/host/workspace_host.cpp"), "set_permission_resolver"));

    // Both shipped resolvers call evaluate before deciding.
    EXPECT_TRUE(contains(read_repo_file("src/permission/permission_broker.cpp"),
                         "policy.evaluate(request, &reason)"));
    EXPECT_TRUE(contains(read_repo_file("src/policy/permission_policy.cpp"),
                         "policy_.evaluate(request, &reason)"));
}

TEST(Errata89B1, McpIsOutOfTheConfinementClaim) {
    const std::string mcp = read_repo_file("src/mcp/mcp_transport.cpp");
    ASSERT_FALSE(mcp.empty());
    EXPECT_EQ(mcp.find("apply_or_abort"), std::string::npos);
    EXPECT_EQ(mcp.find("ConfinementPolicy"), std::string::npos);
    EXPECT_EQ(mcp.find("confinement"), std::string::npos);

    // L2 is applied only at the two claimed {shell,terminal} spawn sites.
    EXPECT_TRUE(contains(read_repo_file("src/execution/process.cpp"), "apply_or_abort"));
    EXPECT_TRUE(contains(read_repo_file("src/execution/pty.cpp"), "apply_or_abort"));
    EXPECT_TRUE(contains(read_repo_file("src/tools/builtin_tools.cpp"), "ToolName{\"shell\"}"));
    EXPECT_TRUE(contains(read_repo_file("src/tools/terminal_tool.cpp"), "ToolName{\"terminal\"}"));
}

} // namespace
