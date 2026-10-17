// Spec 89 (rev 3) B2 / 89-D15: `danger-full-access` disables approval, not
// L1/L2. With `master = true` the incident command is still `Deny`, a
// recognized in-root destructive command with L2 off is still `Ask`, and BOTH
// shipped resolvers short-circuit ONLY on `Allow`. The no-subscriber
// (headless) path therefore denies rather than auto-approving.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

// The shipped preset: `approval = "never"` -> `master = true`; the sandbox is
// still clamped by `effective_sandbox_mode` (89-D15 does not redefine it).
Config danger_full_access_config() {
    Config config;
    config.permissions.default_preset = std::string{kDangerFullAccessPreset};
    return config;
}

PermissionRequest shell_request(const fs::path& root, std::string command, bool kernel_confinement) {
    PermissionRequest request;
    request.call               = "call-1";
    request.session            = SessionId{"session-1"};
    request.tool               = "shell";
    request.root               = root;
    request.sandbox            = SandboxMode::Workspace;
    request.kernel_confinement = kernel_confinement;
    request.arguments          = nlohmann::json::object();
    request.arguments["command"] = std::move(command);
    return request;
}

class CountingTransport final : public PermissionTransport {
public:
    bool broadcast_permission_request(protocol::PermissionRequest) override {
        ++broadcasts;
        return true;
    }

    bool schedule_after(std::chrono::milliseconds, std::function<void()>) override {
        return true;
    }

    int broadcasts = 0;
};

TEST(Errata89B2, DangerFullAccessIncidentIsDeniedNeverAllowed) {
    test::TempWorkspace workspace("b2_incident");
    RulePermissionPolicy policy(to_permission_config(danger_full_access_config()));

    const PermissionRequest incident = shell_request(workspace.path(), "rm -fr build ~", true);
    std::string             reason;
    EXPECT_EQ(policy.evaluate(incident, &reason), PolicyVerdict::Deny);
    EXPECT_EQ(reason, "shell-guard:outside-root");
    EXPECT_NE(policy.evaluate(incident), PolicyVerdict::Allow);

    // `master` is genuinely on: a non-destructive shell command is allowed.
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "echo hi", true)),
              PolicyVerdict::Allow);
}

TEST(Errata89B2, DangerFullAccessGuardAskIsNotAutoApproved) {
    test::TempWorkspace workspace("b2_ask");
    RulePermissionPolicy policy(to_permission_config(danger_full_access_config()));

    // L2 off promotes the recognized in-root destructive command to Confirm.
    const PermissionRequest ask = shell_request(workspace.path(), "rm -rf build", false);
    std::string             reason;
    EXPECT_EQ(policy.evaluate(ask, &reason), PolicyVerdict::Ask);
    EXPECT_EQ(reason, "shell-guard:unbounded-in-root");
    EXPECT_NE(policy.evaluate(ask), PolicyVerdict::Allow);
}

TEST(Errata89B2, GateShortCircuitsOnlyOnAllow) {
    test::TempWorkspace workspace("b2_gate");
    PermissionConfig   config = to_permission_config(danger_full_access_config());
    config.permission_timeout = std::chrono::milliseconds{50};
    RulePermissionPolicy policy(config);
    PermissionGate       gate(policy, config);

    int attention = 0;
    gate.set_attention_hook(
        [&attention](const PermissionRequestId&, const PermissionRequest&) { ++attention; });

    // The incident is a Deny: it short-circuits with no prompt.
    const PermissionOutcome denied =
        gate.resolve(shell_request(workspace.path(), "rm -fr build ~", true), {}, false);
    EXPECT_EQ(denied.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(denied.reason, "shell-guard:outside-root");
    EXPECT_EQ(attention, 0);

    // A guard Ask is a live prompt even under master=true; with no subscriber
    // it fail-closes to Deny and never becomes Allow.
    const PermissionOutcome ask =
        gate.resolve(shell_request(workspace.path(), "rm -rf build", false), {}, false);
    EXPECT_EQ(ask.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(attention, 1);
}

TEST(Errata89B2, HeadlessBrokerPathDeniesWithoutSubscribers) {
    test::TempWorkspace workspace("b2_broker");
    RulePermissionPolicy policy(to_permission_config(danger_full_access_config()));
    CountingTransport    transport;
    PermissionBroker broker(policy, transport,
                            to_permission_config(danger_full_access_config()));
    broker.set_subscriber_count([](SessionId) { return std::size_t{0}; });

    std::future<PermissionOutcome> incident =
        broker.resolve(shell_request(workspace.path(), "rm -fr build ~", true), {});
    ASSERT_EQ(incident.wait_for(std::chrono::milliseconds{0}), std::future_status::ready);
    const PermissionOutcome incident_outcome = incident.get();
    EXPECT_EQ(incident_outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(incident_outcome.reason, "shell-guard:outside-root");

    std::future<PermissionOutcome> ask =
        broker.resolve(shell_request(workspace.path(), "rm -rf build", false), {});
    ASSERT_EQ(ask.wait_for(std::chrono::milliseconds{0}), std::future_status::ready);
    EXPECT_EQ(ask.get().decision, payload::PermissionDecisionKind::Deny);

    // Neither verdict ever reached the wire: Allow/Deny never broadcast, and a
    // no-subscriber Ask short-circuits.
    EXPECT_EQ(transport.broadcasts, 0);
}

TEST(Errata89B2, BrokerAskUnderSubscriberIsALivePromptNotAutoAllow) {
    test::TempWorkspace workspace("b2_broker_live");
    RulePermissionPolicy policy(to_permission_config(danger_full_access_config()));
    CountingTransport    transport;
    PermissionBroker broker(policy, transport,
                            to_permission_config(danger_full_access_config()));
    broker.set_subscriber_count([](SessionId) { return std::size_t{1}; });

    std::future<PermissionOutcome> ask =
        broker.resolve(shell_request(workspace.path(), "rm -rf build", false), {});
    ASSERT_EQ(ask.wait_for(std::chrono::milliseconds{0}), std::future_status::timeout)
        << "a guard Ask must wait for a human, not resolve to Allow";
    EXPECT_EQ(transport.broadcasts, 1);
    EXPECT_EQ(broker.pendingCount(), 1u);

    broker.denyAll("test");
    ASSERT_EQ(ask.wait_for(std::chrono::milliseconds{0}), std::future_status::ready);
    EXPECT_EQ(ask.get().decision, payload::PermissionDecisionKind::Deny);
}

} // namespace
