#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/manual_clock.hpp"
#include "support/test_env.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/git_tools.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;

std::string git_env() {
    return "GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null "
           "GIT_AUTHOR_NAME=ymh GIT_AUTHOR_EMAIL=ymh@example.com "
           "GIT_COMMITTER_NAME=ymh GIT_COMMITTER_EMAIL=ymh@example.com ";
}

int git(const std::filesystem::path& dir, const std::string& args) {
    const std::string command =
        git_env() + "git -C '" + dir.string() + "' " + args + " >/dev/null 2>&1";
    return std::system(command.c_str());
}

void init_repo(const std::filesystem::path& dir) {
    ASSERT_EQ(git(dir, "init -q -b main"), 0);
}

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
    tool_call.id = "call-git";
    tool_call.name = name;
    tool_call.arguments = std::move(arguments);
    return tool_call;
}

ToolResult run(ToolRegistry& registry, ymh::test::ToolEnv& env, const std::string& name,
               nlohmann::json arguments) {
    return registry.execute(call(name, std::move(arguments)), env.context()).get();
}

TEST(GitTools, StatusReportsModifiedAndUntracked) {
    ymh::test::ToolEnv env("git_status");
    init_repo(env.workspace.path());
    env.workspace.write("tracked.txt", "hello\n");
    ASSERT_EQ(git(env.workspace.path(), "add tracked.txt"), 0);
    ASSERT_EQ(git(env.workspace.path(), "commit -q -m init"), 0);

    env.workspace.write("tracked.txt", "hello world\n");
    env.workspace.write("untracked.txt", "new\n");

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result = run(registry, env, "git_status", nlohmann::json::object());

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("## "), std::string::npos);
    EXPECT_NE(result.output.find(" M tracked.txt"), std::string::npos);
    EXPECT_NE(result.output.find("?? untracked.txt"), std::string::npos);
}

TEST(GitTools, StatusReportsStagedChange) {
    ymh::test::ToolEnv env("git_status_staged");
    init_repo(env.workspace.path());
    env.workspace.write("staged.txt", "one\n");
    ASSERT_EQ(git(env.workspace.path(), "add staged.txt"), 0);
    ASSERT_EQ(git(env.workspace.path(), "commit -q -m init"), 0);

    env.workspace.write("staged.txt", "two\n");
    ASSERT_EQ(git(env.workspace.path(), "add staged.txt"), 0);

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result = run(registry, env, "git_status", nlohmann::json::object());

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("M  staged.txt"), std::string::npos);
}

TEST(GitTools, DiffShowsUnstagedChange) {
    ymh::test::ToolEnv env("git_diff_unstaged");
    init_repo(env.workspace.path());
    env.workspace.write("file.txt", "one\n");
    ASSERT_EQ(git(env.workspace.path(), "add file.txt"), 0);
    ASSERT_EQ(git(env.workspace.path(), "commit -q -m init"), 0);
    env.workspace.write("file.txt", "one\ntwo\n");

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result = run(registry, env, "git_diff", nlohmann::json::object());

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("diff --git"), std::string::npos);
    EXPECT_NE(result.output.find("+two"), std::string::npos);
}

TEST(GitTools, DiffShowsStagedChange) {
    ymh::test::ToolEnv env("git_diff_staged");
    init_repo(env.workspace.path());
    env.workspace.write("file.txt", "one\n");
    ASSERT_EQ(git(env.workspace.path(), "add file.txt"), 0);
    ASSERT_EQ(git(env.workspace.path(), "commit -q -m init"), 0);
    env.workspace.write("file.txt", "one\ntwo\n");
    ASSERT_EQ(git(env.workspace.path(), "add file.txt"), 0);

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result =
        run(registry, env, "git_diff", {{"staged", true}});

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("+two"), std::string::npos);
}

TEST(GitTools, DiffAgainstRef) {
    ymh::test::ToolEnv env("git_diff_ref");
    init_repo(env.workspace.path());
    env.workspace.write("file.txt", "one\n");
    ASSERT_EQ(git(env.workspace.path(), "add file.txt"), 0);
    ASSERT_EQ(git(env.workspace.path(), "commit -q -m init"), 0);
    env.workspace.write("file.txt", "one\ntwo\n");

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result = run(registry, env, "git_diff", {{"ref", "HEAD"}});

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("+two"), std::string::npos);
}

TEST(GitTools, DiffPathFilterSelectsOneFile) {
    ymh::test::ToolEnv env("git_diff_path");
    init_repo(env.workspace.path());
    env.workspace.write("a.txt", "a1\n");
    env.workspace.write("b.txt", "b1\n");
    ASSERT_EQ(git(env.workspace.path(), "add a.txt b.txt"), 0);
    ASSERT_EQ(git(env.workspace.path(), "commit -q -m init"), 0);
    env.workspace.write("a.txt", "a1\na2\n");
    env.workspace.write("b.txt", "b1\nb2\n");

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result = run(registry, env, "git_diff", {{"path", "a.txt"}});

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("a/a.txt"), std::string::npos);
    EXPECT_EQ(result.output.find("b/b.txt"), std::string::npos);
}

TEST(GitTools, DiffRejectsPathEscape) {
    ymh::test::ToolEnv env("git_diff_escape");
    init_repo(env.workspace.path());

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result = run(registry, env, "git_diff", {{"path", "../escape.txt"}});

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "PathEscape");
}

TEST(GitTools, DiffTruncatesAtConfiguredCap) {
    ToolConfig config;
    config.tool_result_max_bytes = 200;
    ymh::test::ToolEnv env("git_diff_cap", SandboxMode::Workspace, config);
    init_repo(env.workspace.path());

    std::string large;
    for (int i = 0; i < 200; ++i) {
        large += "line " + std::to_string(i) + "\n";
    }
    env.workspace.write("big.txt", "start\n");
    ASSERT_EQ(git(env.workspace.path(), "add big.txt"), 0);
    ASSERT_EQ(git(env.workspace.path(), "commit -q -m init"), 0);
    env.workspace.write("big.txt", large);

    ToolRegistry registry;
    auto registrations = register_builtins(registry, config);
    const ToolResult result = run(registry, env, "git_diff", nlohmann::json::object());

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_TRUE(result.truncated);
    EXPECT_NE(result.output.find("truncated"), std::string::npos);
}

TEST(GitTools, StatusOnNonRepositoryFails) {
    ymh::test::ToolEnv env("git_not_repo");

    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const ToolResult result = run(registry, env, "git_status", nlohmann::json::object());

    ASSERT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "NotFound");
}

TEST(GitTools, UI46_D8_GitPrecheckTimesOut) {
    ymh::test::ToolEnv env("git_d8_precheck");
    init_repo(env.workspace.path());
    auto tool = make_git_status_tool(ToolConfig{});

    const ToolArguments arguments{nlohmann::json::object()};
    ToolContext context =
        env.context("call-1", 1, 1,
                    std::chrono::steady_clock::now() - std::chrono::seconds{1});
    try {
        (void)tool->execute(context, arguments);
        FAIL() << "expected a Timeout ToolError";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::Timeout);
    }
}

TEST(GitTools, UI46_D8_GitPostcheckTimesOut) {
    ymh::test::ToolEnv env("git_d8_postcheck");
    init_repo(env.workspace.path());
    env.workspace.write("tracked.txt", "hello\n");
    ASSERT_EQ(git(env.workspace.path(), "add tracked.txt"), 0);
    auto tool = make_git_status_tool(ToolConfig{});

    auto clock = std::make_shared<ymh::test::ManualClock>();
    const auto start = clock->now();
    int calls = 0;
    ToolContext::ClockReader reader = [clock, &calls] {
        return calls++ == 0 ? clock->now() : clock->now() + std::chrono::seconds{20};
    };

    const ToolArguments arguments{nlohmann::json::object()};
    ToolContext context = env.context("call-1", 1, 1, start + std::chrono::seconds{10},
                                      std::move(reader));
    try {
        (void)tool->execute(context, arguments);
        FAIL() << "expected a post-call Timeout ToolError";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::Timeout);
    }
}

} // namespace
