#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/manual_clock.hpp"
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
        if (!running_) {
            return false;
        }
        broadcasts_.push_back(std::move(request));
        return true;
    }

    bool schedule_after(std::chrono::milliseconds delay, std::function<void()> fn) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || fail_schedule_) {
            return false;
        }
        timers_.emplace_back(delay, std::move(fn));
        return true;
    }

    void set_running(bool running) {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = running;
    }

    void set_fail_schedule(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_schedule_ = fail;
    }

    [[nodiscard]] std::size_t broadcastCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return broadcasts_.size();
    }

    [[nodiscard]] std::vector<protocol::PermissionRequest> broadcasts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return broadcasts_;
    }

    [[nodiscard]] std::size_t timerCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timers_.size();
    }

    [[nodiscard]] std::vector<std::chrono::milliseconds> timerDelays() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::chrono::milliseconds> delays;
        delays.reserve(timers_.size());
        for (const auto& entry : timers_) {
            delays.push_back(entry.first);
        }
        return delays;
    }

    void fire_timers() {
        std::vector<std::function<void()>> due;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& entry : timers_) {
                due.push_back(std::move(entry.second));
            }
            timers_.clear();
        }
        for (auto& fn : due) {
            fn();
        }
    }

private:
    mutable std::mutex mutex_;
    bool               running_ = true;
    bool               fail_schedule_ = false;
    std::vector<protocol::PermissionRequest> broadcasts_;
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> timers_;
};

PermissionRequest ask_request(std::string session = "s1", std::string tool = "write_file") {
    PermissionRequest request;
    request.call = "call-1";
    request.session = SessionId{std::move(session)};
    request.tool = std::move(tool);
    request.arguments = nlohmann::json::object();
    request.root = "/tmp/ws";
    request.path = std::filesystem::path("src/x");
    request.sandbox = SandboxMode::Workspace;
    return request;
}

PolicyRule rule(std::string tool, PolicyVerdict effect) {
    PolicyRule value;
    value.tool = std::move(tool);
    value.effect = effect;
    value.layer = PolicyRule::Layer::Global;
    value.id = "rule";
    return value;
}

protocol::PermissionDecisionParams decision(std::string request_id,
                                            protocol::PermissionAnswer answer,
                                            protocol::PermissionScope scope =
                                                protocol::PermissionScope::Once) {
    protocol::PermissionDecisionParams params;
    params.request_id = std::move(request_id);
    params.decision = answer;
    params.scope = scope;
    return params;
}

struct Fixture {
    Fixture() : broker(policy, transport, PermissionConfig{}) {
        broker.set_subscriber_count([](SessionId) { return 1u; });
    }

    RulePermissionPolicy policy{PermissionConfig{}};
    FakeTransport        transport;
    PermissionBroker     broker;
};

TEST(PermissionBroker, AllowVerdictShortCircuitsWithoutWire) {
    PermissionConfig config;
    config.rules.push_back(rule("read_file", PolicyVerdict::Allow));
    RulePermissionPolicy policy(config);
    FakeTransport transport;
    PermissionBroker broker(policy, transport, config);

    std::future<PermissionOutcome> future = broker.resolve(ask_request("s1", "read_file"), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(transport.broadcastCount(), 0u);
    EXPECT_EQ(broker.pendingCount(), 0u);
}

TEST(PermissionBroker, DenyVerdictShortCircuitsWithoutWire) {
    PermissionConfig config;
    config.rules.push_back(rule("shell", PolicyVerdict::Deny));
    RulePermissionPolicy policy(config);
    FakeTransport transport;
    PermissionBroker broker(policy, transport, config);

    std::future<PermissionOutcome> future = broker.resolve(ask_request("s1", "shell"), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(transport.broadcastCount(), 0u);
}

TEST(PermissionBroker, ZeroSubscribersAutoDeniesFailClosed) {
    RulePermissionPolicy policy(PermissionConfig{});
    FakeTransport transport;
    PermissionBroker broker(policy, transport, PermissionConfig{});
    broker.set_subscriber_count([](SessionId) { return 0u; });

    std::future<PermissionOutcome> future = broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "no-subscribers");
    EXPECT_EQ(transport.broadcastCount(), 0u);
    EXPECT_EQ(broker.pendingCount(), 0u);
}

TEST(PermissionBroker, MissingSubscriberReaderAutoDenies) {
    RulePermissionPolicy policy(PermissionConfig{});
    FakeTransport transport;
    PermissionBroker broker(policy, transport, PermissionConfig{});

    std::future<PermissionOutcome> future = broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(transport.broadcastCount(), 0u);
}

TEST(PermissionBroker, AskBroadcastsAndResolvesOnDecision) {
    Fixture fixture;

    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(fixture.broker.pendingCount(), 1u);
    ASSERT_EQ(fixture.transport.broadcastCount(), 1u);

    const protocol::PermissionRequest wire = fixture.transport.broadcasts().front();
    EXPECT_FALSE(wire.request_id.empty());
    EXPECT_EQ(wire.session.value, "s1");
    EXPECT_EQ(wire.tool, "write_file");
    EXPECT_GT(wire.expires_at_ms, 0);

    EXPECT_TRUE(fixture.broker.onDecision(
        decision(wire.request_id, protocol::PermissionAnswer::Allow)));
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(outcome.reason, "user");
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
}

TEST(PermissionBroker, FirstDecisionWins) {
    Fixture fixture;
    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(fixture.transport.broadcastCount(), 1u);
    const std::string request_id = fixture.transport.broadcasts().front().request_id;

    EXPECT_TRUE(fixture.broker.onDecision(
        decision(request_id, protocol::PermissionAnswer::Allow)));
    EXPECT_FALSE(fixture.broker.onDecision(
        decision(request_id, protocol::PermissionAnswer::Deny)));
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
}

TEST(PermissionBroker, UnknownRequestIdIsIgnored) {
    Fixture fixture;
    EXPECT_FALSE(fixture.broker.onDecision(
        decision("does-not-exist", protocol::PermissionAnswer::Allow)));
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
}

TEST(PermissionBroker, TimeoutAutoDeniesFailClosed) {
    PermissionConfig config;
    config.permission_timeout = 10ms;
    RulePermissionPolicy policy(config);
    FakeTransport transport;
    test::ManualClock clock;
    PermissionBroker broker(policy, transport, config, clock.reader());
    broker.set_subscriber_count([](SessionId) { return 1u; });

    std::future<PermissionOutcome> future = broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::timeout);
    ASSERT_EQ(transport.timerCount(), 1u);
    ASSERT_EQ(transport.timerDelays().size(), 1u);
    EXPECT_EQ(transport.timerDelays().front(), 10ms);
    EXPECT_EQ(broker.pendingCount(), 1u);

    clock.advance(10ms);
    transport.fire_timers();
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "timeout");
    EXPECT_EQ(broker.pendingCount(), 0u);
}

TEST(PermissionBroker, DecisionBeforeTimeoutWinsOverExpiry) {
    PermissionConfig config;
    config.permission_timeout = 10ms;
    RulePermissionPolicy policy(config);
    FakeTransport transport;
    test::ManualClock clock;
    PermissionBroker broker(policy, transport, config, clock.reader());
    broker.set_subscriber_count([](SessionId) { return 1u; });

    std::future<PermissionOutcome> future = broker.resolve(ask_request(), {});
    const std::string request_id = transport.broadcasts().front().request_id;
    EXPECT_TRUE(broker.onDecision(decision(request_id, protocol::PermissionAnswer::Allow)));

    transport.fire_timers();
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
}

TEST(PermissionBroker, CancellationDeniesFailClosed) {
    Fixture fixture;
    CancellationSource source;
    std::future<PermissionOutcome> future =
        fixture.broker.resolve(ask_request(), source.token());
    ASSERT_EQ(fixture.transport.broadcastCount(), 1u);

    source.cancel();
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "cancelled");
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
}

TEST(PermissionBroker, PreCancelledTokenDeniesWithoutWire) {
    Fixture fixture;
    CancellationSource source;
    source.cancel();

    std::future<PermissionOutcome> future =
        fixture.broker.resolve(ask_request(), source.token());
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().reason, "cancelled");
    EXPECT_EQ(fixture.transport.broadcastCount(), 0u);
}

TEST(PermissionBroker, PendingIsSessionScopedAndReBroadcastable) {
    Fixture fixture;
    std::future<PermissionOutcome> first = fixture.broker.resolve(ask_request("s1"), {});
    std::future<PermissionOutcome> second = fixture.broker.resolve(ask_request("s2"), {});
    ASSERT_EQ(fixture.broker.pendingCount(), 2u);

    const std::vector<protocol::PermissionRequest> for_s1 = fixture.broker.pending(SessionId{"s1"});
    ASSERT_EQ(for_s1.size(), 1u);
    EXPECT_EQ(for_s1.front().session.value, "s1");

    const std::vector<protocol::PermissionRequest> for_s3 = fixture.broker.pending(SessionId{"s3"});
    EXPECT_TRUE(for_s3.empty());

    EXPECT_EQ(for_s1.front().request_id, fixture.transport.broadcasts().front().request_id);
}

TEST(PermissionBroker, ResolveDoesNotBlockTurnThreadAndDecisionArrivesFromAnotherThread) {
    Fixture fixture;
    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    EXPECT_EQ(future.wait_for(0ms), std::future_status::timeout);
    const std::string request_id = fixture.transport.broadcasts().front().request_id;

    std::thread transport_thread([&fixture, request_id] {
        fixture.broker.onDecision(decision(request_id, protocol::PermissionAnswer::Allow));
    });
    transport_thread.join();

    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
}

TEST(PermissionBroker, SessionScopeRecordsGrant) {
    Fixture fixture;
    const PermissionRequest request = ask_request();
    std::future<PermissionOutcome> future = fixture.broker.resolve(request, {});
    const std::string request_id = fixture.transport.broadcasts().front().request_id;

    EXPECT_TRUE(fixture.broker.onDecision(decision(
        request_id, protocol::PermissionAnswer::Allow, protocol::PermissionScope::Session)));
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(fixture.policy.evaluate(request), PolicyVerdict::Allow);
}

TEST(PermissionBroker, AlwaysScopeMapsToAllowAlways) {
    Fixture fixture;
    const PermissionRequest request = ask_request();
    std::future<PermissionOutcome> future = fixture.broker.resolve(request, {});
    const std::string request_id = fixture.transport.broadcasts().front().request_id;

    EXPECT_TRUE(fixture.broker.onDecision(decision(
        request_id, protocol::PermissionAnswer::Allow, protocol::PermissionScope::Always)));
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::AllowAlways);
    EXPECT_EQ(fixture.policy.evaluate(request), PolicyVerdict::Allow);
}

TEST(PermissionBroker, OnceScopeRecordsNothing) {
    Fixture fixture;
    const PermissionRequest request = ask_request();
    std::future<PermissionOutcome> future = fixture.broker.resolve(request, {});
    const std::string request_id = fixture.transport.broadcasts().front().request_id;

    EXPECT_TRUE(fixture.broker.onDecision(decision(
        request_id, protocol::PermissionAnswer::Allow, protocol::PermissionScope::Once)));
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(fixture.policy.evaluate(request), PolicyVerdict::Ask);
}

TEST(PermissionBroker, TransportDownFailsClosed) {
    Fixture fixture;
    fixture.transport.set_running(false);

    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
}

TEST(PermissionBroker, ScheduleFailureFailsClosed) {
    Fixture fixture;
    fixture.transport.set_fail_schedule(true);

    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "transport");
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
}

TEST(PermissionBroker, LargeArgumentsAreBoundedAndSummaryMarkedTruncated) {
    PermissionConfig config;
    config.arguments_max_bytes = 16;
    RulePermissionPolicy policy(config);
    FakeTransport transport;
    PermissionBroker broker(policy, transport, config);
    broker.set_subscriber_count([](SessionId) { return 1u; });

    PermissionRequest request = ask_request();
    request.arguments = nlohmann::json{{"body", std::string(256, 'x')}};
    std::future<PermissionOutcome> future = broker.resolve(request, {});

    ASSERT_EQ(transport.broadcastCount(), 1u);
    const protocol::PermissionRequest wire = transport.broadcasts().front();
    EXPECT_EQ(wire.arguments, nlohmann::json({{"truncated", true}}));
    EXPECT_NE(wire.summary.find("(truncated)"), std::string::npos);
    EXPECT_LE(wire.summary.size(), 256u + std::string(" (truncated)").size());
}

TEST(PermissionBroker, DestructorResolvesPendingFailClosed) {
    RulePermissionPolicy policy(PermissionConfig{});
    FakeTransport transport;
    std::future<PermissionOutcome> future;
    {
        PermissionBroker broker(policy, transport, PermissionConfig{});
        broker.set_subscriber_count([](SessionId) { return 1u; });
        future = broker.resolve(ask_request(), {});
        ASSERT_EQ(broker.pendingCount(), 1u);
    }
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Deny);
}

// AL-I6/AL13/AL-F6: `denyAll` is the coordinator's wake-before-join seam. It
// resolves every pending Ask with the given reason and is idempotent; the old
// broker could only do this in its destructor, after the join had deadlocked.
TEST(PermissionBroker, DenyAllResolvesEveryPendingRequest) {
    Fixture                        fixture;
    std::future<PermissionOutcome> first  = fixture.broker.resolve(ask_request("s1"), {});
    std::future<PermissionOutcome> second = fixture.broker.resolve(ask_request("s2"), {});
    ASSERT_EQ(first.wait_for(0ms), std::future_status::timeout);
    ASSERT_EQ(second.wait_for(0ms), std::future_status::timeout);
    ASSERT_EQ(fixture.broker.pendingCount(), 2u);

    fixture.broker.denyAll("shutdown");

    ASSERT_EQ(first.wait_for(0ms), std::future_status::ready);
    ASSERT_EQ(second.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome one = first.get();
    const PermissionOutcome two = second.get();
    EXPECT_EQ(one.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(one.reason, "shutdown");
    EXPECT_EQ(two.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(two.reason, "shutdown");
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);

    fixture.broker.denyAll("shutdown");
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
}

// AL-F6/24-D16: the deny latch is persistent, so a worker that reaches
// `resolve` after `denyAll` cleared the pending set is denied immediately
// instead of pushing a Pending that could only expire to TimedOut.
TEST(PermissionBroker, ResolveAfterDenyAllIsDeniedByPersistentLatch) {
    Fixture fixture;
    fixture.broker.denyAll("shutdown");

    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "shutdown");
    EXPECT_EQ(fixture.broker.pendingCount(), 0u);
    EXPECT_EQ(fixture.transport.broadcastCount(), 0u);
}

} // namespace
