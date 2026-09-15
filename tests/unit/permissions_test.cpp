#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/glob.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/tools/tool.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;

PermissionRequest request_for(const std::filesystem::path& root,
                              std::string tool,
                              std::optional<std::filesystem::path> path = std::nullopt,
                              SandboxMode sandbox = SandboxMode::Workspace,
                              nlohmann::json arguments = nlohmann::json::object()) {
    PermissionRequest request;
    request.call = "call-1";
    request.session = SessionId{"session-1"};
    request.tool = std::move(tool);
    request.arguments = std::move(arguments);
    request.root = root;
    request.path = std::move(path);
    request.sandbox = sandbox;
    return request;
}

PolicyRule rule(std::string tool, std::string path, PolicyVerdict effect,
                PolicyRule::Layer layer) {
    PolicyRule value;
    value.tool = std::move(tool);
    value.path = std::move(path);
    value.effect = effect;
    value.layer = layer;
    value.id = "rule";
    return value;
}

TEST(Glob, FlatAndPathMatching) {
    EXPECT_TRUE(glob_match("git_*", "git_status"));
    EXPECT_TRUE(glob_match("*", "anything"));
    EXPECT_FALSE(glob_match("git_*", "status"));

    EXPECT_TRUE(path_glob_match("src/**", "src/a/b"));
    EXPECT_TRUE(path_glob_match("src/**", "src/a"));
    EXPECT_FALSE(path_glob_match("src/**", "srcx/a"));
    EXPECT_TRUE(path_glob_match("**/*.lock", "a.lock"));
    EXPECT_TRUE(path_glob_match("**/*.lock", "x/y.lock"));
    EXPECT_FALSE(path_glob_match("**/*.lock", "x/y.txt"));
}

TEST(Policy, MostSpecificRuleWins) {
    PermissionConfig config;
    config.rules.push_back(rule("write_file", "", PolicyVerdict::Ask,
                                PolicyRule::Layer::Global));
    config.rules.push_back(rule("write_file", "src/**", PolicyVerdict::Allow,
                                PolicyRule::Layer::Project));
    RulePermissionPolicy policy(config);
    ymh::test::TempWorkspace workspace("policy_specific");

    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file",
                                          workspace.path() / "src" / "x")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file",
                                          workspace.path() / "docs" / "x")),
              PolicyVerdict::Ask);
}

TEST(Policy, LayerTieBreakAndEffectTieBreak) {
    PermissionConfig config;
    config.rules.push_back(rule("shell", "", PolicyVerdict::Allow,
                                PolicyRule::Layer::Global));
    config.rules.push_back(rule("shell", "", PolicyVerdict::Ask,
                                PolicyRule::Layer::Project));
    RulePermissionPolicy policy(config);
    ymh::test::TempWorkspace workspace("policy_layer");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell")),
              PolicyVerdict::Ask);

    PermissionConfig tie;
    tie.rules.push_back(rule("shell", "", PolicyVerdict::Allow,
                             PolicyRule::Layer::Project));
    tie.rules.push_back(rule("shell", "", PolicyVerdict::Deny,
                             PolicyRule::Layer::Project));
    RulePermissionPolicy tie_policy(tie);
    EXPECT_EQ(tie_policy.evaluate(request_for(workspace.path(), "shell")),
              PolicyVerdict::Deny);
}

TEST(Policy, DefaultIsFailClosedAsk) {
    RulePermissionPolicy policy(PermissionConfig{});
    ymh::test::TempWorkspace workspace("policy_default");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "read_file")),
              PolicyVerdict::Ask);
}

TEST(Policy, ReadOnlyHardDenyIsNotOverridable) {
    PermissionConfig config;
    config.rules.push_back(rule("edit_file", "", PolicyVerdict::Allow,
                                PolicyRule::Layer::SessionGrant));
    config.rules.push_back(rule("edit_file", "**", PolicyVerdict::Allow,
                                PolicyRule::Layer::CommandLine));
    RulePermissionPolicy policy(config);
    ymh::test::TempWorkspace workspace("policy_readonly");

    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "edit_file", std::nullopt,
                                          SandboxMode::ReadOnly)),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "read_file", std::nullopt,
                                          SandboxMode::ReadOnly)),
              PolicyVerdict::Ask);
}

TEST(Policy, UnrestrictedIsStillPolicyGated) {
    RulePermissionPolicy policy(PermissionConfig{});
    ymh::test::TempWorkspace workspace("policy_unrestricted");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", std::nullopt,
                                          SandboxMode::Unrestricted)),
              PolicyVerdict::Ask);
}

TEST(Policy, InvalidRulesFailLoudAtLoad) {
    PermissionConfig empty;
    PolicyRule dimension_less;
    dimension_less.effect = PolicyVerdict::Allow;
    dimension_less.id = "bad";
    empty.rules.push_back(dimension_less);
    EXPECT_THROW(RulePermissionPolicy{empty}, PolicyConfigError);

    PermissionConfig malformed;
    malformed.rules.push_back(rule("shell", "[unclosed", PolicyVerdict::Ask,
                                   PolicyRule::Layer::Global));
    EXPECT_THROW(RulePermissionPolicy{malformed}, PolicyConfigError);
}

TEST(Policy, SessionGrantNarrowsToItsPath) {
    RulePermissionPolicy policy(PermissionConfig{});
    ymh::test::TempWorkspace workspace("policy_grant");
    const PermissionRequest grant_request =
        request_for(workspace.path(), "write_file", workspace.path() / "src" / "x");

    EXPECT_EQ(policy.evaluate(grant_request), PolicyVerdict::Ask);
    policy.remember(grant_request, payload::PermissionDecisionKind::Allow,
                    GrantScope::Session);

    EXPECT_EQ(policy.evaluate(grant_request), PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file",
                                          workspace.path() / "docs" / "x")),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file",
                                          workspace.path() / "src" / "x",
                                          SandboxMode::Workspace,
                                          nlohmann::json::object())),
              PolicyVerdict::Allow);
}

TEST(PermissionHandle, ReturnsRecordedNonAskDecision) {
    StaticPermissionHandle handle(payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(handle.decision(), payload::PermissionDecisionKind::Allow);
    ymh::test::TempWorkspace workspace("policy_handle");
    EXPECT_EQ(handle.evaluate(request_for(workspace.path(), "read_file")).get(),
              payload::PermissionDecisionKind::Allow);
}

TEST(PermissionGate, AllowVerdictSkipsWaiting) {
    PermissionConfig config;
    config.rules.push_back(rule("read_file", "", PolicyVerdict::Allow,
                                PolicyRule::Layer::Global));
    RulePermissionPolicy policy(config);
    PermissionGate gate(policy, config);
    ymh::test::TempWorkspace workspace("gate_allow");

    std::vector<payload::PermissionDecision> decisions;
    gate.set_decision_hook(
        [&decisions](const payload::PermissionDecision& decision) {
            decisions.push_back(decision);
        });

    const PermissionOutcome outcome =
        gate.resolve(request_for(workspace.path(), "read_file"), {});
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Allow);
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_EQ(decisions[0].call, "call-1");
}

TEST(PermissionGate, AskIsResolvedByAHumanDecision) {
    RulePermissionPolicy policy(PermissionConfig{});
    PermissionConfig config;
    config.permission_timeout = std::chrono::milliseconds{5000};
    PermissionGate gate(policy, config);
    ymh::test::TempWorkspace workspace("gate_ask");

    std::mutex mutex;
    std::string request_id;
    gate.set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest&) {
            std::lock_guard<std::mutex> lock(mutex);
            request_id = id.value;
        });

    std::thread decider([&]() {
        for (;;) {
            std::string id;
            {
                std::lock_guard<std::mutex> lock(mutex);
                id = request_id;
            }
            if (!id.empty()) {
                gate.decide(PermissionRequestId{id},
                            payload::PermissionDecisionKind::Allow, GrantScope::Once,
                            "user");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    });

    const PermissionOutcome outcome =
        gate.resolve(request_for(workspace.path(), "shell"), {});
    decider.join();

    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(gate.pendingCount(), 0u);
}

TEST(PermissionGate, AskTimesOutFailClosed) {
    RulePermissionPolicy policy(PermissionConfig{});
    PermissionConfig config;
    config.permission_timeout = std::chrono::milliseconds{100};
    PermissionGate gate(policy, config);
    ymh::test::TempWorkspace workspace("gate_timeout");

    const PermissionOutcome outcome =
        gate.resolve(request_for(workspace.path(), "shell"), {});
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "timeout");
    EXPECT_EQ(gate.pendingCount(), 0u);
}

TEST(PermissionGate, CancelledWaitFailsClosed) {
    RulePermissionPolicy policy(PermissionConfig{});
    PermissionConfig config;
    config.permission_timeout = std::chrono::milliseconds{5000};
    PermissionGate gate(policy, config);
    ymh::test::TempWorkspace workspace("gate_cancel");

    CancellationSource source;
    source.cancel();
    const PermissionOutcome outcome =
        gate.resolve(request_for(workspace.path(), "shell"), source.token());
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "cancelled");
}

class CountingTool final : public Tool {
public:
    explicit CountingTool(int* counter) : counter_(counter) {}

    ToolSchema schema() const override {
        return ToolSchema{ToolName{"edit_file"}, ToolVersion{1, 0}, "edit",
                          nlohmann::json{{"type", "object"},
                                         {"properties", nlohmann::json::object()},
                                         {"required", nlohmann::json::array()},
                                         {"additionalProperties", false}},
                          true};
    }

    Task<ToolResult> execute(const ToolContext& context, const ToolArguments&) override {
        ++*counter_;
        ToolResult result;
        result.id = context.callId();
        result.name = "edit_file";
        return Task<ToolResult>(std::move(result));
    }

private:
    int* counter_;
};

TEST(PermissionGate, DeniedCallIsNeverExecuted) {
    PermissionConfig config;
    config.rules.push_back(rule("edit_file", "", PolicyVerdict::Deny,
                                PolicyRule::Layer::Global));
    RulePermissionPolicy policy(config);
    PermissionGate gate(policy, config);
    ymh::test::ToolEnv env("gate_deny");
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);

    int executions = 0;
    keeper.add(std::make_unique<CountingTool>(&executions));

    payload::ToolCall call;
    call.id = "call-1";
    call.name = "edit_file";
    call.arguments = nlohmann::json::object();

    const PermissionRequest request = request_for(env.workspace.path(), "edit_file");
    const PermissionOutcome outcome = gate.resolve(request, {});

    ToolResult result;
    if (outcome.decision == payload::PermissionDecisionKind::Deny) {
        result.id = call.id;
        result.name = call.name;
        result.outcome = payload::ToolOutcome::Denied;
    } else {
        result = registry.execute(call, env.context()).get();
    }

    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Denied);
    EXPECT_EQ(executions, 0);
}

} // namespace
