#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace std::chrono_literals;

const WorkspaceId kWorkspace{"workspace-a"};
const WorkspaceId kOtherWorkspace{"workspace-b"};
const SessionId kSession{"session-a"};
const SessionId kOtherSession{"session-b"};

// T1 / 95-D1: wire round-trip for `permission.resolved`.
TEST(Errata95, T1ProtocolRoundTripAndStrictEnums) {
    for (const auto answer : {protocol::PermissionAnswer::Allow,
                              protocol::PermissionAnswer::Deny}) {
        for (const auto scope : {protocol::PermissionScope::Once,
                                 protocol::PermissionScope::Session,
                                 protocol::PermissionScope::Always}) {
            protocol::PermissionResolvedNotice notice;
            notice.request_id = "perm-7";
            notice.session = kSession;
            notice.decision = answer;
            notice.scope = scope;
            notice.reason = "user";
            nlohmann::json json;
            protocol::to_json(json, notice);
            const auto parsed = json.get<protocol::PermissionResolvedNotice>();
            EXPECT_EQ(parsed.request_id, notice.request_id);
            EXPECT_EQ(parsed.session.value, notice.session.value);
            EXPECT_EQ(parsed.decision, answer);
            EXPECT_EQ(parsed.scope, scope);
            EXPECT_EQ(parsed.reason, "user");
        }
    }

    nlohmann::json without_reason{{"request_id", "perm-1"},
                                  {"session", "s1"},
                                  {"decision", "allow"},
                                  {"scope", "once"}};
    const auto lenient = without_reason.get<protocol::PermissionResolvedNotice>();
    EXPECT_EQ(lenient.reason, "");

    nlohmann::json bad_decision = without_reason;
    bad_decision["decision"] = "maybe";
    EXPECT_THROW((void)bad_decision.get<protocol::PermissionResolvedNotice>(),
                 std::invalid_argument);
    nlohmann::json bad_scope = without_reason;
    bad_scope["scope"] = "forever";
    EXPECT_THROW((void)bad_scope.get<protocol::PermissionResolvedNotice>(),
                 std::invalid_argument);

    // 95-D12 caps in the broker, which only ever emits the fixed tokens; a long
    // reason is still a valid wire value (the cap is not a parser rule).
    nlohmann::json long_reason = without_reason;
    long_reason["reason"] = std::string(100, 'x');
    EXPECT_EQ(long_reason.get<protocol::PermissionResolvedNotice>().reason.size(), 100u);
}

// Test transport: records request/notice order, can re-enter, fail, or throw.
class RecordingTransport final : public PermissionTransport {
public:
    struct Call {
        enum class Kind { Request, Notice, Schedule } kind;
        std::string request_id;
        protocol::PermissionResolvedNotice notice;
    };

    bool broadcast_permission_request(protocol::PermissionRequest request) override {
        std::function<void(const std::string&)> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++calls_;
            order_.push_back("request");
            requests_.push_back(request);
            hook = on_request_;
        }
        if (hook) {
            hook(request.request_id);
        }
        if (throw_request_) {
            throw std::runtime_error("request post failed");
        }
        return !fail_request_;
    }

    bool broadcast_permission_resolved(protocol::PermissionResolvedNotice notice) override {
        if (ready_probe_) {
            notice_outcome_ready_ = ready_probe_();
        }
        if (throw_notice_) {
            throw std::runtime_error("notice post failed");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        order_.push_back("notice");
        notices_.push_back(std::move(notice));
        return true;
    }

    bool schedule_after(std::chrono::milliseconds delay, std::function<void()> fn) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        order_.push_back("schedule");
        if (throw_schedule_) {
            throw std::runtime_error("schedule failed");
        }
        if (fail_schedule_) {
            return false;
        }
        timers_.emplace_back(delay, std::move(fn));
        return true;
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

    void set_on_request(std::function<void(const std::string&)> hook) {
        on_request_ = std::move(hook);
    }
    void set_ready_probe(std::function<bool()> probe) { ready_probe_ = std::move(probe); }
    void set_fail_request(bool value) { fail_request_ = value; }
    void set_throw_request(bool value) { throw_request_ = value; }
    void set_fail_schedule(bool value) { fail_schedule_ = value; }
    void set_throw_schedule(bool value) { throw_schedule_ = value; }
    void set_throw_notice(bool value) { throw_notice_ = value; }

    [[nodiscard]] std::size_t request_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }
    [[nodiscard]] std::size_t notice_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return notices_.size();
    }
    [[nodiscard]] std::vector<protocol::PermissionResolvedNotice> notices() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return notices_;
    }
    [[nodiscard]] std::vector<std::string> order() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return order_;
    }
    [[nodiscard]] std::size_t call_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }
    [[nodiscard]] bool notice_outcome_ready() const { return notice_outcome_ready_; }
    [[nodiscard]] std::string last_request_id() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.empty() ? std::string{} : requests_.back().request_id;
    }

private:
    mutable std::mutex mutex_;
    std::size_t        calls_ = 0;
    std::vector<std::string> order_;
    std::vector<protocol::PermissionRequest> requests_;
    std::vector<protocol::PermissionResolvedNotice> notices_;
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> timers_;
    std::function<void(const std::string&)> on_request_;
    std::function<bool()> ready_probe_;
    bool                  notice_outcome_ready_ = false;
    bool                  fail_request_ = false;
    bool                  throw_request_ = false;
    bool                  fail_schedule_ = false;
    bool                  throw_schedule_ = false;
    bool                  throw_notice_ = false;
};

PermissionRequest ask_request(std::string session = kSession.value) {
    PermissionRequest request;
    request.call = "call-1";
    request.session = SessionId{std::move(session)};
    request.tool = "write_file";
    request.arguments = nlohmann::json{{"path", "a.txt"}};
    return request;
}

protocol::PermissionDecisionParams decide(std::string request_id,
                                          protocol::PermissionAnswer answer,
                                          protocol::PermissionScope scope) {
    protocol::PermissionDecisionParams params;
    params.request_id = std::move(request_id);
    params.decision = answer;
    params.scope = scope;
    return params;
}

struct BrokerFixture {
    BrokerFixture() : broker(policy, transport, PermissionConfig{}) {
        broker.set_subscriber_count([](SessionId) { return 1u; });
    }

    RulePermissionPolicy policy{PermissionConfig{}};
    RecordingTransport  transport;
    PermissionBroker    broker;
};

// T3 / 95-D3: the decision site emits exactly one notice after set_value.
TEST(Errata95, T3DecisionSiteEmitsNoticeAfterSetValue) {
    BrokerFixture fixture;
    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::timeout);
    fixture.transport.set_ready_probe(
        [&future] { return future.wait_for(0ms) == std::future_status::ready; });

    EXPECT_TRUE(fixture.broker.onDecision(
        decide(fixture.transport.last_request_id(), protocol::PermissionAnswer::Allow,
               protocol::PermissionScope::Session)));
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(fixture.transport.notice_count(), 1u);
    const auto notice = fixture.transport.notices().front();
    EXPECT_EQ(notice.request_id, fixture.transport.last_request_id());
    EXPECT_EQ(notice.session.value, kSession.value);
    EXPECT_EQ(notice.decision, protocol::PermissionAnswer::Allow);
    EXPECT_EQ(notice.scope, protocol::PermissionScope::Session);
    EXPECT_EQ(notice.reason, "user");
    EXPECT_TRUE(fixture.transport.notice_outcome_ready());
}

// T3 / 95-D3: cancellation emits one Deny/Once "cancelled" notice.
TEST(Errata95, T3CancelSiteEmitsNoticeAfterSetValue) {
    BrokerFixture fixture;
    CancellationSource source;
    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), source.token());
    ASSERT_EQ(future.wait_for(0ms), std::future_status::timeout);
    fixture.transport.set_ready_probe(
        [&future] { return future.wait_for(0ms) == std::future_status::ready; });

    source.cancel();
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    ASSERT_EQ(fixture.transport.notice_count(), 1u);
    const auto notice = fixture.transport.notices().front();
    EXPECT_EQ(notice.decision, protocol::PermissionAnswer::Deny);
    EXPECT_EQ(notice.scope, protocol::PermissionScope::Once);
    EXPECT_EQ(notice.reason, "cancelled");
    EXPECT_TRUE(fixture.transport.notice_outcome_ready());
}

// T3 / 95-D3: the timeout site emits one Deny/Once "timeout" notice.
TEST(Errata95, T3TimeoutSiteEmitsNoticeAfterSetValue) {
    BrokerFixture fixture;
    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::timeout);
    fixture.transport.set_ready_probe(
        [&future] { return future.wait_for(0ms) == std::future_status::ready; });

    fixture.transport.fire_timers();
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    const PermissionOutcome outcome = future.get();
    EXPECT_EQ(outcome.reason, "timeout");
    ASSERT_EQ(fixture.transport.notice_count(), 1u);
    EXPECT_EQ(fixture.transport.notices().front().reason, "timeout");
    EXPECT_EQ(fixture.transport.notices().front().decision,
              protocol::PermissionAnswer::Deny);
    EXPECT_EQ(fixture.transport.notices().front().scope, protocol::PermissionScope::Once);
    EXPECT_TRUE(fixture.transport.notice_outcome_ready());
}

// T3b / 95-D3: scope fidelity: Allow/Session is not collapsed to Allow/Once.
TEST(Errata95, T3bAllowSessionScopeIsPreserved) {
    BrokerFixture fixture;
    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_TRUE(fixture.broker.onDecision(
        decide(fixture.transport.last_request_id(), protocol::PermissionAnswer::Allow,
               protocol::PermissionScope::Session)));
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    ASSERT_EQ(fixture.transport.notice_count(), 1u);
    EXPECT_EQ(fixture.transport.notices().front().decision,
              protocol::PermissionAnswer::Allow);
    EXPECT_EQ(fixture.transport.notices().front().scope, protocol::PermissionScope::Session);
}

// T3a / 95-D3a: a resolution that wins during the request post defers its
// notice; the request post strictly precedes the notice post.
TEST(Errata95, T3aCancelDuringPostFlushesNoticeAfterRequest) {
    BrokerFixture fixture;
    CancellationSource source;
    fixture.transport.set_on_request([&source](const std::string&) { source.cancel(); });

    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), source.token());
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().reason, "cancelled");
    const auto order = fixture.transport.order();
    ASSERT_EQ(fixture.transport.request_count(), 1u);
    ASSERT_EQ(fixture.transport.notice_count(), 1u);
    // order: schedule?, request, notice
    const auto request_it = std::find(order.begin(), order.end(), "request");
    const auto notice_it = std::find(order.begin(), order.end(), "notice");
    ASSERT_NE(request_it, order.end());
    ASSERT_NE(notice_it, order.end());
    EXPECT_LT(request_it, notice_it);
}

// T3a / 95-M1: a transport that re-enters `onDecision` from the request post
// must not deadlock (no virtual call under `State::mutex`).
TEST(Errata95, T3aReentrantDecisionDoesNotDeadlock) {
    BrokerFixture fixture;
    fixture.transport.set_on_request([&fixture](const std::string& request_id) {
        (void)fixture.broker.onDecision(
            decide(request_id, protocol::PermissionAnswer::Allow,
                   protocol::PermissionScope::Once));
    });

    std::future<PermissionOutcome> future = fixture.broker.resolve(ask_request(), {});
    ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(fixture.transport.request_count(), 1u);
    EXPECT_EQ(fixture.transport.notice_count(), 1u);
}

// T4 / 95-I3: undelivered requests emit no notices.
TEST(Errata95, T4UndeliveredSitesEmitNoNotice) {
    // Policy Allow / Deny.
    {
        PermissionConfig config;
        PolicyRule allow;
        allow.tool = "read_file";
        allow.effect = PolicyVerdict::Allow;
        allow.layer = PolicyRule::Layer::Global;
        allow.id = "allow";
        config.rules.push_back(allow);
        RulePermissionPolicy policy(config);
        RecordingTransport transport;
        PermissionBroker broker(policy, transport, config);
        broker.set_subscriber_count([](SessionId) { return 1u; });
        PermissionRequest request = ask_request();
        request.tool = "read_file";
        auto future = broker.resolve(request, {});
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(transport.notice_count(), 0u);
    }
    // Pre-cancelled.
    {
        BrokerFixture fixture;
        CancellationSource source;
        source.cancel();
        auto future = fixture.broker.resolve(ask_request(), source.token());
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(fixture.transport.notice_count(), 0u);
    }
    // No subscribers.
    {
        RulePermissionPolicy policy(PermissionConfig{});
        RecordingTransport transport;
        PermissionBroker broker(policy, transport, PermissionConfig{});
        broker.set_subscriber_count([](SessionId) { return 0u; });
        auto future = broker.resolve(ask_request(), {});
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(transport.notice_count(), 0u);
    }
    // Shutting down.
    {
        BrokerFixture fixture;
        fixture.broker.denyAll("shutdown");
        auto future = fixture.broker.resolve(ask_request(), {});
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(fixture.transport.notice_count(), 0u);
    }
    // Failed schedule.
    {
        BrokerFixture fixture;
        fixture.transport.set_fail_schedule(true);
        auto future = fixture.broker.resolve(ask_request(), {});
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(future.get().reason, "transport");
        EXPECT_EQ(fixture.transport.notice_count(), 0u);
    }
    // Failed request post.
    {
        BrokerFixture fixture;
        fixture.transport.set_fail_request(true);
        auto future = fixture.broker.resolve(ask_request(), {});
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(future.get().reason, "transport");
        EXPECT_EQ(fixture.transport.notice_count(), 0u);
        EXPECT_EQ(fixture.broker.pendingCount(), 0u);
    }
}

// T5 / 95-F4: decision and timeout race in either order -> one value, one notice.
TEST(Errata95, T5FirstWinsDecisionAndTimeout) {
    {
        BrokerFixture fixture;
        auto future = fixture.broker.resolve(ask_request(), {});
        const std::string id = fixture.transport.last_request_id();
        EXPECT_TRUE(fixture.broker.onDecision(
            decide(id, protocol::PermissionAnswer::Allow, protocol::PermissionScope::Once)));
        fixture.transport.fire_timers();
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
        EXPECT_EQ(fixture.transport.notice_count(), 1u);
    }
    {
        BrokerFixture fixture;
        auto future = fixture.broker.resolve(ask_request(), {});
        const std::string id = fixture.transport.last_request_id();
        fixture.transport.fire_timers();
        EXPECT_FALSE(fixture.broker.onDecision(
            decide(id, protocol::PermissionAnswer::Allow, protocol::PermissionScope::Once)));
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(future.get().reason, "timeout");
        EXPECT_EQ(fixture.transport.notice_count(), 1u);
    }
}

// T13 / 95-D17: teardown resolves without any transport call (no notice, no
// request, no schedule).
TEST(Errata95, T13TeardownMakesNoTransportCalls) {
    RecordingTransport transport;
    {
        RulePermissionPolicy policy(PermissionConfig{});
        PermissionBroker broker(policy, transport, PermissionConfig{});
        broker.set_subscriber_count([](SessionId) { return 1u; });
        auto future = broker.resolve(ask_request(), {});
        ASSERT_EQ(future.wait_for(0ms), std::future_status::timeout);
        ASSERT_EQ(transport.notice_count(), 0u);
        const std::size_t calls_before = transport.call_count();
        broker.denyAll("shutdown");
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(transport.call_count(), calls_before);
        EXPECT_EQ(transport.notice_count(), 0u);
    }
    EXPECT_EQ(transport.notice_count(), 0u);
}

// T14 / 95-D18: a throwing schedule resolves exactly one "internal" outcome and
// emits no notice; a throwing notice post is swallowed.
TEST(Errata95, T14ResolveFailureIsContained) {
    {
        BrokerFixture fixture;
        fixture.transport.set_throw_schedule(true);
        auto future = fixture.broker.resolve(ask_request(), {});
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        const PermissionOutcome outcome = future.get();
        EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
        EXPECT_EQ(outcome.reason, "internal");
        EXPECT_EQ(fixture.broker.pendingCount(), 0u);
        EXPECT_EQ(fixture.transport.notice_count(), 0u);
    }
    {
        BrokerFixture fixture;
        auto future = fixture.broker.resolve(ask_request(), {});
        fixture.transport.set_throw_notice(true);
        EXPECT_TRUE(fixture.broker.onDecision(
            decide(fixture.transport.last_request_id(), protocol::PermissionAnswer::Allow,
                   protocol::PermissionScope::Once)));
        ASSERT_EQ(future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(future.get().decision, payload::PermissionDecisionKind::Allow);
    }
}

UiModel make_model() {
    UiModel model;
    model.activeWorkspaceId = kWorkspace;
    WorkspaceModel workspace;
    workspace.id = kWorkspace;
    workspace.cwd = "/tmp/workspace-a";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live = true;
    model.workspaces.emplace(kWorkspace, workspace);
    WorkspaceModel other;
    other.id = kOtherWorkspace;
    other.cwd = "/tmp/workspace-b";
    other.daemonStatus = DaemonStatus::Attached;
    other.live = true;
    model.workspaces.emplace(kOtherWorkspace, other);
    model.focusSessionIn(kWorkspace, kSession);
    model.ensureSessionIn(kOtherWorkspace, kOtherSession);
    model.dirty.clear();
    return model;
}

void open_dialog(UiEventAdapter& adapter, const SessionId& session,
                 const PermissionRequestId& id) {
    PermissionRequest request;
    request.session = session;
    request.tool = "shell";
    request.arguments = {{"command", "ls"}};
    adapter.onPermissionRequest(session, id, request);
}

// T6 / 95-D6a: only an open dialog with the same session AND request closes.
TEST(Errata95, T6UiCloseGuardIsSessionAndRequestKeyed) {
    UiModel model = make_model();
    model.mode = UiMode::Dashboard;
    UiEventAdapter adapter(model);
    const PermissionRequestId id{"perm-1"};
    open_dialog(adapter, kSession, id);
    ASSERT_TRUE(model.dialog.open);
    ASSERT_EQ(model.dialog.prev_mode, UiMode::Dashboard);

    // Same id, different session: no-op (95-F7).
    adapter.onPermissionResolved(kOtherSession, id, payload::PermissionDecisionKind::Deny);
    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.session.value, kSession.value);
    EXPECT_TRUE(model.session(kSession)->attention.needsInput);

    // Duplicate after a close: no-op (the earlier close restored Dashboard).
    adapter.onPermissionResolved(kSession, id, payload::PermissionDecisionKind::Allow);
    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.mode, UiMode::Dashboard);
    adapter.onPermissionResolved(kSession, id, payload::PermissionDecisionKind::Allow);
    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.mode, UiMode::Dashboard);
}

// T6b / 95-D6b: final state Idle; a late notice while open does not resurrect
// Thinking; a stale notice does not clear a newer prompt's attention.
TEST(Errata95, T6bStateTransitionsAreConditional) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    const PermissionRequestId id{"perm-1"};
    open_dialog(adapter, kSession, id);
    ASSERT_EQ(model.session(kSession)->agent_state, AgentState::WaitingForPermission);

    // The turn's committed events reached Idle before the notice: no Thinking.
    adapter.force_idle(kSession);
    ASSERT_EQ(model.session(kSession)->agent_state, AgentState::Idle);
    EXPECT_TRUE(model.dialog.open);
    adapter.onPermissionResolved(kSession, id, payload::PermissionDecisionKind::Allow);
    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.session(kSession)->agent_state, AgentState::Idle);

    // Normal path: the notice moves Waiting -> Thinking; the turn then ends Idle.
    const PermissionRequestId second{"perm-2"};
    open_dialog(adapter, kSession, second);
    adapter.onPermissionResolved(kSession, second, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(model.session(kSession)->agent_state, AgentState::Thinking);
    adapter.force_idle(kSession);
    EXPECT_EQ(model.session(kSession)->agent_state, AgentState::Idle);

    // Stale notice for an older id must not clear the newer prompt's attention.
    const PermissionRequestId newer{"perm-3"};
    open_dialog(adapter, kSession, newer);
    ASSERT_TRUE(model.session(kSession)->attention.needsInput);
    adapter.onPermissionResolved(kSession, PermissionRequestId{"perm-1"},
                                 payload::PermissionDecisionKind::Deny);
    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.request.value, newer.value);
    EXPECT_TRUE(model.session(kSession)->attention.needsInput);
}

// T15 / 95-D16: link loss and session close close only the matching dialog and
// restore `prev_mode`.
TEST(Errata95, T15LinkLossAndSessionCloseCleanup) {
    UiModel model = make_model();
    model.mode = UiMode::Dashboard;
    UiEventAdapter adapter(model);
    const PermissionRequestId id{"perm-1"};
    open_dialog(adapter, kSession, id);
    ASSERT_TRUE(model.dialog.open);

    // Wrong workspace: no-op.
    model.closePermissionDialogForWorkspace(kOtherWorkspace);
    EXPECT_TRUE(model.dialog.open);

    // Matching workspace: closes and restores the interrupted mode.
    model.closePermissionDialogForWorkspace(kWorkspace);
    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.mode, UiMode::Dashboard);
    EXPECT_FALSE(model.session(kSession)->attention.needsInput);

    // Wrong session: no-op.
    open_dialog(adapter, kOtherSession, PermissionRequestId{"perm-2"});
    ASSERT_TRUE(model.dialog.open);
    model.closePermissionDialogForSession(kSession);
    EXPECT_TRUE(model.dialog.open);
    model.closePermissionDialogForSession(kOtherSession);
    EXPECT_FALSE(model.dialog.open);
}

} // namespace
