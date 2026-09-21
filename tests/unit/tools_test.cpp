#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;

std::vector<ToolRegistry::Registration> register_builtins(ToolRegistry& registry,
                                                          ToolConfig config = {}) {
    std::vector<ToolRegistry::Registration> registrations;
    for (auto& tool : make_builtin_tools(config)) {
        registrations.push_back(registry.add(std::move(tool)));
    }
    return registrations;
}

payload::ToolCall call(const std::string& name, nlohmann::json arguments) {
    payload::ToolCall tool_call;
    tool_call.id = "call-1";
    tool_call.name = name;
    tool_call.arguments = std::move(arguments);
    return tool_call;
}

ToolResult run(ToolRegistry& registry, ymh::test::ToolEnv& env, const std::string& name,
               nlohmann::json arguments) {
    return registry.execute(call(name, std::move(arguments)), env.context()).get();
}

TEST(BuiltinTools, ReadFileHappyPath) {
    ymh::test::ToolEnv env("tools_read");
    env.workspace.write("a.txt", "hello\nworld\n");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result = run(registry, env, "read_file", {{"path", "a.txt"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(result.output, "hello\nworld\n");
}

TEST(BuiltinTools, ReadFileLineWindow) {
    ymh::test::ToolEnv env("tools_read_window");
    env.workspace.write("a.txt", "l1\nl2\nl3\nl4\n");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result =
        run(registry, env, "read_file", {{"path", "a.txt"}, {"offset", 1}, {"limit", 2}});
    EXPECT_EQ(result.output, "l2\nl3");
}

TEST(BuiltinTools, ReadFileIsBoundedByConfig) {
    ToolConfig config;
    config.read_file_max_bytes = 5;
    ymh::test::ToolEnv env("tools_read_bound", SandboxMode::Workspace, config);
    env.workspace.write("a.txt", "0123456789");
    ToolRegistry registry;
    auto registrations = register_builtins(registry, config);

    const ToolResult result = run(registry, env, "read_file", {{"path", "a.txt"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(result.output, "01234");
    EXPECT_TRUE(result.truncated);
}

TEST(BuiltinTools, ReadFileRejectsPathEscape) {
    ymh::test::ToolEnv env("tools_read_escape");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result = run(registry, env, "read_file", {{"path", "../secret"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "PathEscape");
}

TEST(BuiltinTools, ReadFileMissingIsNotFound) {
    ymh::test::ToolEnv env("tools_read_missing");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result = run(registry, env, "read_file", {{"path", "missing.txt"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "NotFound");
}

TEST(BuiltinTools, WriteThenReadRoundTrips) {
    ymh::test::ToolEnv env("tools_write");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult written =
        run(registry, env, "write_file", {{"path", "out/nested.txt"}, {"content", "hello world"}});
    EXPECT_EQ(written.outcome, payload::ToolOutcome::Ok);

    const ToolResult read = run(registry, env, "read_file", {{"path", "out/nested.txt"}});
    EXPECT_EQ(read.output, "hello world");
}

TEST(BuiltinTools, EditFileReplacesAndReportsMissing) {
    ymh::test::ToolEnv env("tools_edit");
    env.workspace.write("a.txt", "hello world");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult edited =
        run(registry, env, "edit_file",
            {{"path", "a.txt"}, {"old_string", "world"}, {"new_string", "there"}});
    EXPECT_EQ(edited.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(run(registry, env, "read_file", {{"path", "a.txt"}}).output, "hello there");

    const ToolResult missing =
        run(registry, env, "edit_file",
            {{"path", "a.txt"}, {"old_string", "absent"}, {"new_string", "x"}});
    EXPECT_EQ(missing.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(missing.error.has_value());
    EXPECT_EQ(*missing.error, "NotFound");
}

TEST(BuiltinTools, GrepContentMode) {
    ymh::test::ToolEnv env("tools_grep_content");
    env.workspace.write("a.txt", "hello\nworld\nhello\n");
    env.workspace.write("b.cpp", "hello\n");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result = run(registry, env, "grep", {{"pattern", "hello"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(result.output, "a.txt:1:hello\na.txt:3:hello\nb.cpp:1:hello");
}

TEST(BuiltinTools, GrepFilesWithMatchesAndCount) {
    ymh::test::ToolEnv env("tools_grep_modes");
    env.workspace.write("a.txt", "hello\nworld\nhello\n");
    env.workspace.write("b.cpp", "hello\n");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult files =
        run(registry, env, "grep",
            {{"pattern", "hello"}, {"output_mode", "files_with_matches"}});
    EXPECT_EQ(files.output, "a.txt\nb.cpp");

    const ToolResult counts =
        run(registry, env, "grep", {{"pattern", "hello"}, {"output_mode", "count"}});
    EXPECT_EQ(counts.output, "a.txt:2\nb.cpp:1");
}

TEST(BuiltinTools, GrepInvalidRegexIsInvalidArguments) {
    ymh::test::ToolEnv env("tools_grep_bad");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result = run(registry, env, "grep", {{"pattern", "([unclosed"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "InvalidArguments");
}

TEST(BuiltinTools, GrepSkipsLargeBinaryAndStaysBounded) {
    ymh::test::ToolEnv env("tools_grep_binary");
    env.workspace.write("hit.txt", "alpha\nYMH_LIVE_LLM\nomega\n");
    std::string blob(4u * 1024u * 1024u, '\0');
    blob += "YMH_LIVE_LLM\n";
    env.workspace.write("blob.bin", blob);
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result = run(registry, env, "grep", {{"pattern", "YMH_LIVE_LLM"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("hit.txt"), std::string::npos);
    EXPECT_EQ(result.output.find("blob.bin"), std::string::npos);
}

TEST(BuiltinTools, GlobMatchesPaths) {
    ymh::test::ToolEnv env("tools_glob");
    env.workspace.write("src/a.cpp", "a");
    env.workspace.write("src/b.hpp", "b");
    env.workspace.write("other/c.cpp", "c");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult under_src = run(registry, env, "glob", {{"pattern", "src/**"}});
    EXPECT_EQ(under_src.output, "src/a.cpp\nsrc/b.hpp");

    const ToolResult cpp = run(registry, env, "glob", {{"pattern", "**/*.cpp"}});
    EXPECT_EQ(cpp.output, "other/c.cpp\nsrc/a.cpp");
}

TEST(BuiltinTools, ShellCapturesOutputAndExitCode) {
    ymh::test::ToolEnv env("tools_shell");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult ok = run(registry, env, "shell", {{"command", "printf hi"}});
    EXPECT_EQ(ok.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(ok.output, "hi\nexit_code: 0");

    env.ring.clear();
    const ToolResult failed = run(registry, env, "shell", {{"command", "exit 7"}});
    EXPECT_EQ(failed.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(failed.output, "exit_code: 7");
}

TEST(BuiltinTools, ShellTimeoutIsAnError) {
    ymh::test::ToolEnv env("tools_shell_timeout");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result =
        run(registry, env, "shell", {{"command", "sleep 5"}, {"timeout_ms", 200}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "Timeout");
}

TEST(BuiltinTools, UI46_D8_ShellDeadlineKillsProcess) {
    ToolConfig config;
    config.tool_timeout = std::chrono::milliseconds{200};
    ymh::test::ToolEnv env("tools_d8_shell", SandboxMode::Workspace, config);
    ToolRegistry registry;
    auto registrations = register_builtins(registry, config);

    const auto start = std::chrono::steady_clock::now();
    const ToolResult result =
        registry
            .execute(call("shell", {{"command", "sleep 5"}}),
                     env.context("call-1", 1, 1,
                                 std::chrono::steady_clock::now() + std::chrono::milliseconds{200}))
            .get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "Timeout");
    EXPECT_LT(elapsed, std::chrono::milliseconds{3000});
}

TEST(BuiltinTools, UI46_D8_TimeoutResultIsErrorAndTurnContinues) {
    ToolConfig config;
    config.tool_timeout = std::chrono::milliseconds{200};
    ymh::test::ToolEnv env("tools_d8_continue", SandboxMode::Workspace, config);
    ToolRegistry registry;
    auto registrations = register_builtins(registry, config);

    const ToolResult timed_out =
        registry
            .execute(call("shell", {{"command", "sleep 5"}}),
                     env.context("call-1", 1, 1,
                                 std::chrono::steady_clock::now() + std::chrono::milliseconds{200}))
            .get();
    EXPECT_EQ(timed_out.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(timed_out.error.has_value());
    EXPECT_EQ(*timed_out.error, "Timeout");

    env.ring.clear();
    const ToolResult next =
        registry.execute(call("shell", {{"command", "printf ok"}}), env.context("call-2")).get();
    EXPECT_EQ(next.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(next.output, "ok\nexit_code: 0");
}

TEST(BuiltinTools, UI46_D8_DeadlineExpiredPreDispatch) {
    ymh::test::ToolEnv env("tools_d8_predispatch");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result =
        registry
            .execute(call("write_file", {{"path", "created.txt"}, {"content", "hi"}}),
                     env.context("call-1", 1, 1,
                                 std::chrono::steady_clock::now() - std::chrono::seconds{1}))
            .get();

    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "Timeout");
    EXPECT_FALSE(std::filesystem::exists(env.workspace.path() / "created.txt"));
}

TEST(BuiltinTools, ShellCapExhaustionIsResourceExhausted) {
    ResourceCaps caps;
    caps.max_global_subprocesses = 0;
    ymh::test::ToolEnv env("tools_shell_cap", SandboxMode::Workspace, {}, caps);
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const ToolResult result = run(registry, env, "shell", {{"command", "printf hi"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "ResourceExhausted");
}

TEST(BuiltinTools, SchemasAreDeterministicAndSorted) {
    ToolRegistry registry;
    auto registrations = register_builtins(registry);

    const std::vector<ToolName> names = registry.names();
    std::vector<std::string> values;
    for (const ToolName& name : names) {
        values.push_back(name.value);
    }
    EXPECT_EQ(values, (std::vector<std::string>{"edit_file", "git_diff", "git_status",
                                                "glob", "grep", "read_file", "shell",
                                                "write_file"}));
}

} // namespace
