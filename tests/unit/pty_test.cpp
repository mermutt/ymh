#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_pty.hpp"
#include "support/test_env.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/tools/terminal_tool.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;

payload::ToolCall tool_call(const std::string& name, nlohmann::json arguments) {
    payload::ToolCall call;
    call.id = "call-1";
    call.name = name;
    call.arguments = std::move(arguments);
    return call;
}

ToolResult run_terminal(ToolRegistry& registry,
                        ymh::test::ToolEnv& env,
                        nlohmann::json arguments) {
    return registry.execute(tool_call("terminal", std::move(arguments)),
                            env.context())
        .get();
}

std::uint64_t opened_id(const ToolResult& result) {
    const auto parsed = nlohmann::json::parse(result.output);
    return std::stoull(parsed.at("terminal_id").get<std::string>());
}

// ---- 14 §13.1 — PtyOutputRing (E-P4) ---------------------------------------

TEST(PtyOutputRing, UnderCapKeepsFifoOrder) {
    PtyOutputRing ring(16);
    ring.append("abc");
    ring.append("def");
    bool truncated = false;
    EXPECT_EQ(ring.read(4, truncated), "abcd");
    EXPECT_FALSE(truncated);
    EXPECT_EQ(ring.read(16, truncated), "ef");
    EXPECT_FALSE(truncated);
    EXPECT_TRUE(ring.empty());
}

TEST(PtyOutputRing, OverCapEvictsOldestAndReportsTruncationOnce) {
    PtyOutputRing ring(8);
    ring.append("abcdef");
    bool truncated = false;
    EXPECT_EQ(ring.read(3, truncated), "abc");
    EXPECT_FALSE(truncated);

    ring.append("ghijkl");
    EXPECT_EQ(ring.read(100, truncated), "efghijkl");
    EXPECT_TRUE(truncated);
    EXPECT_TRUE(ring.empty());

    ring.append("z");
    EXPECT_EQ(ring.read(100, truncated), "z");
    EXPECT_FALSE(truncated);
}

TEST(PtyOutputRing, ReadTrimsToUtf8Boundary) {
    PtyOutputRing ring(16);
    ring.append("\xC3\xA9x");
    bool truncated = false;
    EXPECT_EQ(ring.read(1, truncated), "");
    EXPECT_EQ(ring.read(16, truncated), "\xC3\xA9x");
}

TEST(PtyOutputRing, CapacityIsNeverExceeded) {
    PtyOutputRing ring(4);
    ring.append("0123456789");
    EXPECT_EQ(ring.size(), 4u);
    bool truncated = false;
    EXPECT_EQ(ring.read(100, truncated), "6789");
    EXPECT_TRUE(truncated);
}

// ---- 14 §13.1 — Stream<T> ---------------------------------------------------

TEST(PtyStream, SingleConsumerReceivesInOrder) {
    StreamWriter<std::string> writer;
    Stream<std::string>       stream = writer.handle();
    std::vector<std::string>  seen;
    StreamSubscription<std::string> subscription =
        stream.subscribe([&](const std::string& chunk) { seen.push_back(chunk); });
    EXPECT_TRUE(subscription.active());

    writer.push("a");
    writer.push("b");
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "a");
    EXPECT_EQ(seen[1], "b");
}

TEST(PtyStream, ResubscribeReplacesAndStalesTheOldHandle) {
    StreamWriter<std::string> writer;
    Stream<std::string>       stream = writer.handle();
    std::vector<std::string>  seen;
    StreamSubscription<std::string> first =
        stream.subscribe([&](const std::string& chunk) { seen.push_back("1:" + chunk); });
    StreamSubscription<std::string> second =
        stream.subscribe([&](const std::string& chunk) { seen.push_back("2:" + chunk); });
    EXPECT_FALSE(first.active());
    EXPECT_TRUE(second.active());

    writer.push("x");
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "2:x");
}

TEST(PtyStream, DropDetachesAndBuffersBeforeSubscribe) {
    StreamWriter<std::string> writer;
    Stream<std::string>       stream = writer.handle();
    writer.push("buffered");
    EXPECT_EQ(stream.buffered(), 1u);

    std::vector<std::string> seen;
    {
        StreamSubscription<std::string> subscription =
            stream.subscribe([&](const std::string& chunk) { seen.push_back(chunk); });
        ASSERT_EQ(seen.size(), 1u);
        EXPECT_EQ(seen[0], "buffered");
    }
    writer.push("after");
    EXPECT_EQ(seen.size(), 1u);

    writer.finish();
    EXPECT_FALSE(stream.closed());

    std::vector<std::string> drained;
    StreamSubscription<std::string> resubscribed =
        stream.subscribe([&](const std::string& chunk) { drained.push_back(chunk); });
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0], "after");
    EXPECT_TRUE(stream.closed());
}

// ---- 14 §2.2 — error translation -------------------------------------------

TEST(PtyError, TranslationIsTotalAndPinsTheErrataMapping) {
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::SpawnFailed), ToolErrorCode::Io);
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::NotFound), ToolErrorCode::NotFound);
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::Closed), ToolErrorCode::Io);
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::Eio), ToolErrorCode::Io);
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::WouldBlock),
              ToolErrorCode::ResourceExhausted);
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::CapExceeded),
              ToolErrorCode::ResourceExhausted);
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::ResizeFailed), ToolErrorCode::Io);
    EXPECT_EQ(to_tool_error_code(PtyErrorCode::Internal), ToolErrorCode::Internal);

    for (const PtyErrorCode code :
         {PtyErrorCode::SpawnFailed, PtyErrorCode::NotFound, PtyErrorCode::Closed,
          PtyErrorCode::Eio, PtyErrorCode::WouldBlock, PtyErrorCode::CapExceeded,
          PtyErrorCode::ResizeFailed, PtyErrorCode::Internal}) {
        EXPECT_FALSE(to_string(code).empty());
    }
}

TEST(PtyError, UnavailableServiceReportsUnavailable) {
    UnavailablePtyService unavailable;
    EXPECT_FALSE(unavailable.available());
    EXPECT_EQ(unavailable.find(PtySessionId{1}), nullptr);
    EXPECT_TRUE(unavailable.list(SessionId{"s"}).empty());
    EXPECT_NO_THROW(unavailable.closeAll());
}

// ---- 14 §13.2 — the terminal tool over a fake service ----------------------

TEST(TerminalTool, OpenWriteReadResizeCloseList) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_actions", SandboxMode::Workspace, {}, {},
                                  &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult open =
        run_terminal(registry, env, {{"action", "open"}, {"command", "cat"}});
    EXPECT_EQ(open.outcome, payload::ToolOutcome::Ok);
    const std::uint64_t id = opened_id(open);
    ASSERT_NE(pty.at(id), nullptr);

    const ToolResult write =
        run_terminal(registry, env,
                     {{"action", "write"},
                      {"terminal_id", std::to_string(id)},
                      {"input", "hello"}});
    EXPECT_EQ(write.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(pty.at(id)->writes_, "hello\n");

    pty.at(id)->script("world", true);
    const ToolResult read =
        run_terminal(registry, env,
                     {{"action", "read"}, {"terminal_id", std::to_string(id)}});
    EXPECT_EQ(read.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(read.output, "world\n[eof]");

    const ToolResult resize =
        run_terminal(registry, env,
                     {{"action", "resize"},
                      {"terminal_id", std::to_string(id)},
                      {"rows", 40},
                      {"cols", 120}});
    EXPECT_EQ(resize.outcome, payload::ToolOutcome::Ok);
    ASSERT_EQ(pty.at(id)->resizes_.size(), 1u);
    EXPECT_EQ(pty.at(id)->resizes_[0], std::make_pair(40, 120));

    const ToolResult list = run_terminal(registry, env, {{"action", "list"}});
    EXPECT_EQ(list.output, nlohmann::json::array({std::to_string(id)}).dump());

    pty.at(id)->setExit(PtyExit{3, false, 0});
    const ToolResult close =
        run_terminal(registry, env,
                     {{"action", "close"}, {"terminal_id", std::to_string(id)}});
    EXPECT_EQ(close.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(nlohmann::json::parse(close.output).at("exit_code").get<int>(), 3);
    EXPECT_EQ(pty.at(id)->terminate_calls, 1);
}

TEST(TerminalTool, CloseWithKillUsesTheForceSeam) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_kill", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult open =
        run_terminal(registry, env, {{"action", "open"}, {"command", "sleep 100"}});
    const std::uint64_t id = opened_id(open);
    run_terminal(registry, env,
                 {{"action", "close"}, {"terminal_id", std::to_string(id)}, {"signal", "kill"}});
    EXPECT_EQ(pty.at(id)->kill_calls, 1);
    EXPECT_EQ(pty.at(id)->terminate_calls, 0);
}

TEST(TerminalTool, OpenRequiresExactlyOneOfCommandOrArgv) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_open_args", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult neither = run_terminal(registry, env, {{"action", "open"}});
    EXPECT_EQ(neither.outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(neither.error, std::string(to_string(ToolErrorCode::InvalidArguments)));

    const ToolResult both =
        run_terminal(registry, env,
                     {{"action", "open"}, {"command", "cat"}, {"argv", {"cat"}}});
    EXPECT_EQ(both.outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(both.error, std::string(to_string(ToolErrorCode::InvalidArguments)));

    const ToolResult argv_open =
        run_terminal(registry, env, {{"action", "open"}, {"argv", {"/bin/echo", "hi"}}});
    EXPECT_EQ(argv_open.outcome, payload::ToolOutcome::Ok);
}

TEST(TerminalTool, UnknownActionIsInvalidArguments) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_action", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult result = run_terminal(registry, env, {{"action", "nope"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(result.error, std::string(to_string(ToolErrorCode::InvalidArguments)));
}

TEST(TerminalTool, UnknownTerminalIdIsNotFound) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_foreign", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult result =
        run_terminal(registry, env, {{"action", "write"}, {"terminal_id", "999"},
                                     {"input", "x"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(result.error, std::string(to_string(ToolErrorCode::NotFound)));
}

TEST(TerminalTool, CapExceededMapsToResourceExhausted) {
    ymh::test::FakePtyService pty;
    pty.fail_with_cap = true;
    ymh::test::ToolEnv env("pty_tool_cap", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry       registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult result =
        run_terminal(registry, env, {{"action", "open"}, {"command", "cat"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(result.error, std::string(to_string(ToolErrorCode::ResourceExhausted)));
}

TEST(TerminalTool, WriteAfterExitReportsIo) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_dead", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult open =
        run_terminal(registry, env, {{"action", "open"}, {"command", "cat"}});
    const std::uint64_t id = opened_id(open);
    pty.at(id)->setState(PtyState::Exited);

    const ToolResult result =
        run_terminal(registry, env, {{"action", "write"},
                                     {"terminal_id", std::to_string(id)},
                                     {"input", "x"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(result.error, std::string(to_string(ToolErrorCode::Io)));
}

TEST(TerminalTool, ResizeRejectsNonPositiveSize) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_resize", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult open =
        run_terminal(registry, env, {{"action", "open"}, {"command", "cat"}});
    const std::uint64_t id = opened_id(open);
    const ToolResult result =
        run_terminal(registry, env, {{"action", "resize"},
                                     {"terminal_id", std::to_string(id)},
                                     {"rows", 0},
                                     {"cols", 80}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(result.error, std::string(to_string(ToolErrorCode::InvalidArguments)));
}

TEST(TerminalTool, ReadCancellationIsReportedAsCancelled) {
    ymh::test::FakePtyService pty;
    ymh::test::ToolEnv        env("pty_tool_cancel", SandboxMode::Workspace, {}, {}, &pty);
    ToolRegistry              registry;
    auto registration = registry.add(make_terminal_tool());
    registry.freeze();

    const ToolResult open =
        run_terminal(registry, env, {{"action", "open"}, {"command", "cat"}});
    const std::uint64_t id = opened_id(open);

    CancellationSource source;
    source.cancel();
    ToolContext cancelling(env.env, *env.session, env.logger, source.token(),
                           env.governor, env.sink, env.permission, "call-2", 1, 1,
                           std::nullopt);
    const ToolResult result =
        registry
            .execute(tool_call("terminal", {{"action", "read"},
                                            {"terminal_id", std::to_string(id)}}),
                     cancelling)
            .get();
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Cancelled);
}

// ---- 14 §13.2 — permission gating ------------------------------------------

TEST(TerminalPermission, ReadOnlyHardDeniesAndCommandRuleMatches) {
    PermissionRequest request;
    request.tool = "terminal";
    request.arguments = {{"action", "open"}, {"command", "git push origin"}};
    request.destructive = true;
    request.sandbox = SandboxMode::ReadOnly;
    PermissionConfig config;
    RulePermissionPolicy policy(config);
    EXPECT_EQ(policy.evaluate(request), PolicyVerdict::Deny);

    PermissionConfig config2;
    PolicyRule rule;
    rule.tool = "terminal";
    rule.command = "git push*";
    rule.effect = PolicyVerdict::Deny;
    rule.layer = PolicyRule::Layer::Global;
    config2.rules.push_back(rule);
    RulePermissionPolicy policy2(config2);

    PermissionRequest workspace_request = request;
    workspace_request.sandbox = SandboxMode::Workspace;
    EXPECT_EQ(policy2.evaluate(workspace_request), PolicyVerdict::Deny);
}

} // namespace
