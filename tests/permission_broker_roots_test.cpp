// Multi-root permission broker: the `Directory` answer registers the directory
// offered by the dialog and MUST fail closed when the add hook is missing or
// refuses; it never becomes a tool/command grant.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "test_support.hpp"

#include "ymh/core/cancellation.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/execution/workspace_roots.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using ymh_test::fresh_uuid;

std::filesystem::path canonical_or_lexical(const std::filesystem::path& path) {
    std::error_code       ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

// A directory outside /tmp (validation refuses scratch-area roots). When $HOME
// is unavailable the candidate tests skip.
struct ScratchRepo {
    std::filesystem::path base;
    std::filesystem::path repo;

    ScratchRepo() {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            base = canonical_or_lexical(std::filesystem::path{home}) /
                   ("ymh-broker-roots-" + fresh_uuid());
        }
        if (!base.empty()) {
            std::error_code ec;
            repo = base / "other-repo";
            std::filesystem::create_directories(repo / ".git", ec);
        }
    }

    ~ScratchRepo() {
        if (!base.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }
    }

    ScratchRepo(const ScratchRepo&)            = delete;
    ScratchRepo& operator=(const ScratchRepo&) = delete;

    [[nodiscard]] bool usable() const {
        if (base.empty()) {
            return false;
        }
        const std::filesystem::path& temp = canonical_tmp_root();
        const std::filesystem::path canonical = canonical_or_lexical(base);
        return temp.empty() || (canonical != temp && !path_is_under_tmp_root(temp, canonical));
    }
};

class FakeTransport final : public PermissionTransport {
public:
    bool broadcast_permission_request(protocol::PermissionRequest request) override {
        requests.push_back(std::move(request));
        return true;
    }
    bool broadcast_permission_resolved(protocol::PermissionResolvedNotice notice) override {
        notices.push_back(std::move(notice));
        return true;
    }
    bool schedule_after(std::chrono::milliseconds,
                        std::function<void()>) override {
        return true;
    }

    std::vector<protocol::PermissionRequest>        requests;
    std::vector<protocol::PermissionResolvedNotice> notices;
};

PermissionRequest make_request(const std::filesystem::path& primary,
                               const std::filesystem::path& target) {
    PermissionRequest request;
    request.session.value = "ses-broker-roots";
    request.call          = "call-1";
    request.tool          = "write_file";
    request.root          = primary;
    request.roots         = {primary};
    request.path          = target;
    request.arguments     = nlohmann::json{{"path", target.string()}};
    request.sandbox       = SandboxMode::WorkspaceWrite;
    request.approval      = ApprovalPolicy::Ask;
    return request;
}

PermissionConfig broker_config() {
    PermissionConfig config;
    config.permission_timeout = std::chrono::milliseconds{0};   // wait for the answer
    return config;
}

} // namespace

TEST(PermissionBrokerRoots, OffCandidateIsAdvertisedOnTheWire) {
    ScratchRepo scratch;
    if (!scratch.usable()) {
        GTEST_SKIP() << "no durable scratch directory outside /tmp";
    }
    const std::filesystem::path primary = scratch.base / "primary";
    std::error_code             ec;
    std::filesystem::create_directories(primary, ec);
    const std::filesystem::path target = scratch.repo / "src" / "file.txt";

    RulePermissionPolicy policy{broker_config()};
    FakeTransport         transport;
    PermissionBroker      broker{policy, transport, broker_config()};
    broker.set_subscriber_count([](SessionId) { return std::size_t{1}; });

    std::future<PermissionOutcome> outcome =
        broker.resolve(make_request(primary, target), CancellationToken{});
    ASSERT_EQ(transport.requests.size(), 1u);
    ASSERT_TRUE(transport.requests.front().add_root.has_value());
    EXPECT_EQ(std::filesystem::path{*transport.requests.front().add_root},
              std::filesystem::canonical(scratch.repo));

    ASSERT_TRUE(broker.onDecision(protocol::PermissionDecisionParams{
        transport.requests.front().request_id, protocol::PermissionAnswer::Deny,
        protocol::PermissionScope::Once}));
    EXPECT_EQ(outcome.get().decision, payload::PermissionDecisionKind::Deny);
}

TEST(PermissionBrokerRoots, MissingHookFailsClosed) {
    ScratchRepo scratch;
    if (!scratch.usable()) {
        GTEST_SKIP() << "no durable scratch directory outside /tmp";
    }
    const std::filesystem::path primary = scratch.base / "primary";
    std::error_code             ec;
    std::filesystem::create_directories(primary, ec);

    RulePermissionPolicy policy{broker_config()};
    FakeTransport         transport;
    PermissionBroker      broker{policy, transport, broker_config()};
    broker.set_subscriber_count([](SessionId) { return std::size_t{1}; });

    std::future<PermissionOutcome> outcome =
        broker.resolve(make_request(primary, scratch.repo / "src" / "file.txt"),
                       CancellationToken{});
    ASSERT_EQ(transport.requests.size(), 1u);
    ASSERT_TRUE(transport.requests.front().add_root.has_value());

    ASSERT_TRUE(broker.onDecision(protocol::PermissionDecisionParams{
        transport.requests.front().request_id, protocol::PermissionAnswer::Allow,
        protocol::PermissionScope::Directory}));
    const PermissionOutcome resolved = outcome.get();
    EXPECT_EQ(resolved.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_NE(resolved.reason.find("add-root-unavailable"), std::string::npos)
        << resolved.reason;
    ASSERT_FALSE(transport.notices.empty());
    EXPECT_EQ(transport.notices.back().decision, protocol::PermissionAnswer::Deny);
    EXPECT_EQ(transport.notices.back().scope, protocol::PermissionScope::Once);
}

TEST(PermissionBrokerRoots, RefusedHookFailsClosed) {
    ScratchRepo scratch;
    if (!scratch.usable()) {
        GTEST_SKIP() << "no durable scratch directory outside /tmp";
    }
    const std::filesystem::path primary = scratch.base / "primary";
    std::error_code             ec;
    std::filesystem::create_directories(primary, ec);

    RulePermissionPolicy policy{broker_config()};
    FakeTransport         transport;
    PermissionBroker      broker{
        policy, transport, broker_config(), Clock::now,
        [](const std::filesystem::path&) {
            return std::expected<std::filesystem::path, std::string>(
                std::unexpected(std::string{"refused"}));
        }};
    broker.set_subscriber_count([](SessionId) { return std::size_t{1}; });

    std::future<PermissionOutcome> outcome =
        broker.resolve(make_request(primary, scratch.repo / "src" / "file.txt"),
                       CancellationToken{});
    ASSERT_TRUE(broker.onDecision(protocol::PermissionDecisionParams{
        transport.requests.front().request_id, protocol::PermissionAnswer::Allow,
        protocol::PermissionScope::Directory}));
    const PermissionOutcome resolved = outcome.get();
    EXPECT_EQ(resolved.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_NE(resolved.reason.find("add-root-failed"), std::string::npos)
        << resolved.reason;
}

TEST(PermissionBrokerRoots, DirectoryAnswerAddsTheRootAndSkipsRemember) {
    ScratchRepo scratch;
    if (!scratch.usable()) {
        GTEST_SKIP() << "no durable scratch directory outside /tmp";
    }
    const std::filesystem::path primary = scratch.base / "primary";
    std::error_code             ec;
    std::filesystem::create_directories(primary, ec);
    const std::filesystem::path expected = std::filesystem::canonical(scratch.repo);

    RulePermissionPolicy policy{broker_config()};
    FakeTransport         transport;
    std::optional<std::filesystem::path> added;
    PermissionBroker broker{
        policy, transport, broker_config(), Clock::now,
        [&added](const std::filesystem::path& directory)
            -> std::expected<std::filesystem::path, std::string> {
            added = directory;
            return directory;
        }};
    broker.set_subscriber_count([](SessionId) { return std::size_t{1}; });

    const PermissionRequest request =
        make_request(primary, scratch.repo / "src" / "file.txt");
    ASSERT_EQ(policy.evaluate(request), PolicyVerdict::Ask);
    std::future<PermissionOutcome> outcome = broker.resolve(request, CancellationToken{});
    ASSERT_EQ(transport.requests.size(), 1u);

    ASSERT_TRUE(broker.onDecision(protocol::PermissionDecisionParams{
        transport.requests.front().request_id, protocol::PermissionAnswer::Allow,
        protocol::PermissionScope::Directory}));
    const PermissionOutcome resolved = outcome.get();
    EXPECT_EQ(resolved.decision, payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(resolved.reason, "user:added-root");
    ASSERT_TRUE(added.has_value());
    EXPECT_EQ(*added, expected);

    ASSERT_FALSE(transport.notices.empty());
    EXPECT_EQ(transport.notices.back().decision, protocol::PermissionAnswer::Allow);
    EXPECT_EQ(transport.notices.back().scope, protocol::PermissionScope::Directory);
    // No tool/session/durable grant is created for this scope.
    EXPECT_TRUE(policy.rules().empty());
    EXPECT_EQ(policy.evaluate(request), PolicyVerdict::Ask);
}
