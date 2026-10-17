#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <string>

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

PermissionConfig master_config() {
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

PermissionRequest request_for(const fs::path& root, std::string command,
                              bool kernel_confinement) {
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

TEST(Errata89L2Absent, DestructiveInRootIsConfirmNotAllow) {
    test::TempWorkspace workspace("l2_absent");
    RulePermissionPolicy policy(master_config());

    std::string                  reason;
    const PermissionRequest      request = request_for(workspace.path(), "rm -rf build", false);
    EXPECT_EQ(policy.evaluate(request, &reason), PolicyVerdict::Ask);
    EXPECT_EQ(reason, "shell-guard:unbounded-in-root");
}

TEST(Errata89L2Absent, CouplingL2OnAllowsInRootRecognized) {
    test::TempWorkspace workspace("l2_on");
    RulePermissionPolicy policy(master_config());

    const PermissionRequest request = request_for(workspace.path(), "rm -rf build", true);
    EXPECT_EQ(policy.evaluate(request), PolicyVerdict::Allow);
}

TEST(Errata89L2Absent, NoSilentAllowForDestructiveInRootWhenL2Off) {
    test::TempWorkspace workspace("l2_noallow");
    RulePermissionPolicy policy(master_config());

    for (const std::string command :
         {"rm -rf build", "rm -rf ./dist", "rmdir build", "truncate -s 0 build",
          "mv build build.old"}) {
        const PermissionRequest request = request_for(workspace.path(), command, false);
        EXPECT_NE(policy.evaluate(request), PolicyVerdict::Allow) << command;
    }
}

TEST(Errata89L2Absent, HeadlessResolverDeniesWhenNoSubscribers) {
    test::TempWorkspace workspace("l2_headless");
    PermissionConfig    config = master_config();
    RulePermissionPolicy policy(config);
    NullTransport        transport;
    PermissionBroker     broker(policy, transport, config);
    broker.set_subscriber_count([](SessionId) { return std::size_t{0}; });

    std::future<PermissionOutcome> future =
        broker.resolve(request_for(workspace.path(), "rm -rf build", false), {});
    ASSERT_EQ(future.wait_for(std::chrono::milliseconds{0}), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Deny);
}

TEST(Errata89L2Absent, UnrestrictedSandboxIsNotApplicable) {
    test::TempWorkspace workspace("l2_na");

    LocalEnvironment unrestricted(workspace.path(), SandboxMode::Unrestricted);
    EXPECT_EQ(unrestricted.confinementStatus(), ConfinementStatus::NotApplicable);

    LocalEnvironment confined(workspace.path(), SandboxMode::Workspace);
    EXPECT_NE(confined.confinementStatus(), ConfinementStatus::NotApplicable);
}

} // namespace
