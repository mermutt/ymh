// Spec 89 (rev 3) sec 13 / 89-D14 / SG-F20: the kernel-absent-L2 path at the
// integration level. When `confinementStatus() != Enforced` the loop passes
// `kernel_confinement = false`, and the coupling promotes every recognized
// destructive in-root command to `Confirm`; the shipped `PermissionBroker`
// (headless, zero subscribers) then denies. Nothing destructive is silently
// `Allow`. The kernel-enforced oracle lives in `errata89_l2_confinement_test`.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

class NullTransport final : public PermissionTransport {
public:
    bool broadcast_permission_request(protocol::PermissionRequest) override { return true; }
    bool schedule_after(std::chrono::milliseconds, std::function<void()>) override {
        return true;
    }
};

PermissionConfig allow_shell_config() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Allow;
    PolicyRule shell;
    shell.tool   = "shell";
    shell.effect = PolicyVerdict::Allow;
    shell.layer  = PolicyRule::Layer::Global;
    shell.id     = "test.shell.allow";
    config.rules.push_back(shell);
    return config;
}

PermissionRequest request_for(const fs::path& root, std::string command, bool kernel_confinement) {
    PermissionRequest request;
    request.call                 = "call-1";
    request.session              = SessionId{"session-1"};
    request.tool                 = "shell";
    request.root                 = root;
    request.sandbox              = SandboxMode::Workspace;
    request.kernel_confinement   = kernel_confinement;
    request.arguments            = nlohmann::json::object();
    request.arguments["command"] = std::move(command);
    return request;
}

const std::vector<std::string>& destructive_in_root() {
    static const std::vector<std::string> commands = {
        "rm -rf build", "rm -rf ./dist", "rmdir build", "truncate -s 0 build",
        "mv build build.old"};
    return commands;
}

TEST(Errata89L2Na, ForcedUnavailableCouplingNeverAllows) {
    test::TempWorkspace workspace("l2_na_forced");
    RulePermissionPolicy policy(allow_shell_config());

    for (const std::string& command : destructive_in_root()) {
        SCOPED_TRACE(command);
        std::string                 reason;
        const PermissionRequest     request = request_for(workspace.path(), command, false);
        EXPECT_EQ(policy.evaluate(request, &reason), PolicyVerdict::Ask);
        EXPECT_EQ(reason, "shell-guard:unbounded-in-root");
        EXPECT_NE(policy.evaluate(request), PolicyVerdict::Allow);
    }
}

TEST(Errata89L2Na, ShippedBrokerDeniesDestructiveWhenL2Off) {
    test::TempWorkspace workspace("l2_na_broker");
    PermissionConfig    config = allow_shell_config();
    RulePermissionPolicy policy(config);
    NullTransport        transport;
    PermissionBroker     broker(policy, transport, config);
    broker.set_subscriber_count([](SessionId) { return std::size_t{0}; });

    for (const std::string& command : destructive_in_root()) {
        SCOPED_TRACE(command);
        std::future<PermissionOutcome> future =
            broker.resolve(request_for(workspace.path(), command, false), {});
        ASSERT_EQ(future.wait_for(std::chrono::milliseconds{0}), std::future_status::ready);
        EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Deny);
    }
}

// The literal host oracle: only runs where the kernel really lacks the rights.
TEST(Errata89L2Na, RealUnavailableHostHasNoSilentAllow) {
    test::TempWorkspace workspace("l2_na_host");
    LocalEnvironment    environment(workspace.path(), SandboxMode::Workspace);
    if (environment.confinementStatus() != ConfinementStatus::Unavailable) {
        GTEST_SKIP() << "host reports a usable/NotApplicable confinement status; "
                        "the forced-Unavailable coupling is covered above";
    }

    RulePermissionPolicy policy(allow_shell_config());
    const bool           kernel_confinement =
        environment.confinementStatus() == ConfinementStatus::Enforced;

    for (const std::string& command : destructive_in_root()) {
        SCOPED_TRACE(command);
        const PermissionRequest request = request_for(environment.root(), command,
                                                      kernel_confinement);
        EXPECT_NE(policy.evaluate(request), PolicyVerdict::Allow);
    }
}

} // namespace
