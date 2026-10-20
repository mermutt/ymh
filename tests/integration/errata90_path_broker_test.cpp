#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace std::chrono_literals;

class FakeTransport final : public PermissionTransport {
public:
    bool broadcast_permission_request(protocol::PermissionRequest request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        broadcasts_.push_back(std::move(request));
        return true;
    }
    bool schedule_after(std::chrono::milliseconds,
                        std::function<void()>) override {
        return true;
    }
    std::size_t broadcastCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return broadcasts_.size();
    }
    std::vector<protocol::PermissionRequest> broadcasts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return broadcasts_;
    }

private:
    mutable std::mutex                          mutex_;
    std::vector<protocol::PermissionRequest>    broadcasts_;
};

PermissionRequest request(const std::filesystem::path& root, std::string tool,
                          std::optional<std::filesystem::path> path = std::nullopt,
                          nlohmann::json arguments = nlohmann::json::object()) {
    PermissionRequest value;
    value.call      = "call-1";
    value.session   = SessionId{"s1"};
    value.tool      = std::move(tool);
    value.root      = root;
    value.path      = std::move(path);
    value.arguments = std::move(arguments);
    value.kernel_confinement = true;
    return value;
}

struct Fixture {
    Fixture() : broker(policy, transport, PermissionConfig{}) {
        broker.set_subscriber_count([](SessionId) { return 1u; });
    }
    RulePermissionPolicy policy{PermissionConfig{}};
    FakeTransport        transport;
    PermissionBroker     broker;
};

protocol::PermissionDecisionParams decision(std::string id,
                                            protocol::PermissionAnswer answer,
                                            protocol::PermissionScope scope) {
    protocol::PermissionDecisionParams params;
    params.request_id = std::move(id);
    params.decision   = answer;
    params.scope      = scope;
    return params;
}

std::optional<protocol::PermissionRequest> wait_for_broadcast(FakeTransport& transport,
                                                             std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (transport.broadcastCount() >= count) {
            return transport.broadcasts().back();
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

} // namespace

TEST(Errata90PathBroker, SessionGrantCoversShellPrefix) {
    ymh::test::TempWorkspace workspace("p90_broker_prefix");
    Fixture fixture;
    const auto root = workspace.path();
    std::future<PermissionOutcome> first =
        fixture.broker.resolve(request(root, "shell", std::nullopt,
                                       {{"command", "git status"}}),
                               {});
    const auto broadcast = wait_for_broadcast(fixture.transport, 1);
    ASSERT_TRUE(broadcast.has_value());
    ASSERT_TRUE(fixture.broker.onDecision(decision(
        broadcast->request_id, protocol::PermissionAnswer::Allow,
        protocol::PermissionScope::Session)));
    ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(first.get().decision, payload::PermissionDecisionKind::Allow);

    std::future<PermissionOutcome> second =
        fixture.broker.resolve(request(root, "shell", std::nullopt,
                                       {{"command", "git diff"}}),
                               {});
    ASSERT_EQ(second.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(second.get().decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(fixture.transport.broadcastCount(), 1u);
}

TEST(Errata90PathBroker, SessionGrantOutsideStaysExact) {
    ymh::test::TempWorkspace workspace("p90_broker_outside");
    Fixture fixture;
    const auto root = workspace.path();
    std::future<PermissionOutcome> first =
        fixture.broker.resolve(request(root, "shell", std::nullopt,
                                       {{"command", "cat /etc/passwd"}}),
                               {});
    const auto broadcast = wait_for_broadcast(fixture.transport, 1);
    ASSERT_TRUE(broadcast.has_value());
    fixture.broker.onDecision(decision(broadcast->request_id,
                                       protocol::PermissionAnswer::Allow,
                                       protocol::PermissionScope::Session));
    ASSERT_EQ(first.wait_for(2s), std::future_status::ready);

    std::future<PermissionOutcome> second =
        fixture.broker.resolve(request(root, "shell", std::nullopt,
                                       {{"command", "cat /etc/shadow"}}),
                               {});
    EXPECT_EQ(second.wait_for(20ms), std::future_status::timeout);
    EXPECT_EQ(fixture.transport.broadcastCount(), 2u);
    fixture.broker.denyAll("cleanup");
}

TEST(Errata90PathBroker, SummaryIsRootRelative) {
    ymh::test::TempWorkspace workspace("p90_broker_summary");
    Fixture fixture;
    const auto root = workspace.path();
    std::future<PermissionOutcome> future =
        fixture.broker.resolve(request(root, "write_file", root / "src" / "x"), {});
    const auto broadcast = wait_for_broadcast(fixture.transport, 1);
    ASSERT_TRUE(broadcast.has_value());
    EXPECT_NE(broadcast->summary.find("src/x"), std::string::npos);
    EXPECT_EQ(broadcast->summary.find(root.generic_string()), std::string::npos);
    fixture.broker.denyAll("cleanup");
    (void)future;
}

TEST(Errata90PathBroker, GuardStillShortCircuitsSessionGrant) {
    ymh::test::TempWorkspace workspace("p90_broker_guard");
    Fixture fixture;
    const auto root = workspace.path();
    std::future<PermissionOutcome> first =
        fixture.broker.resolve(request(root, "shell", std::nullopt,
                                       {{"command", "rm -rf ~"}}),
                               {});
    ASSERT_EQ(first.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(first.get().decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(fixture.transport.broadcastCount(), 0u);
}
