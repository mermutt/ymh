#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "support/test_env.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;

ShellClassification classify(const std::filesystem::path& root, std::string_view text,
                             bool kernel = true) {
    return classify_shell_command_with_scope(text, root, root, kernel,
                                             canonical_tmp_root());
}

PermissionRequest shell_request(const std::filesystem::path& root, std::string command) {
    PermissionRequest request;
    request.call    = "call-1";
    request.session = SessionId{"s1"};
    request.tool    = "shell";
    request.root    = root;
    request.arguments = {{"command", std::move(command)}};
    request.sandbox = SandboxMode::Workspace;
    request.kernel_confinement = true;
    return request;
}

PermissionConfig in_root_allow() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    PolicyRule rule;
    rule.tool   = "*";
    rule.path   = "**";
    rule.effect = PolicyVerdict::Allow;
    rule.layer  = PolicyRule::Layer::Global;
    rule.id     = "workspace.in-root";
    config.rules.push_back(std::move(rule));
    return config;
}

} // namespace

TEST(ShellScope94, SpecialParametersNeutral) {
    ymh::test::TempWorkspace workspace("p94_special");
    const auto root = workspace.path();
    for (const char* text : {"echo $?", "echo \"$?\"", "echo $$", "echo $#", "echo $@",
                             "echo \"$@\"", "echo x$@", "echo ${?} ${@}", "echo $9"}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::InRoot) << text;
    }
    EXPECT_EQ(classify(root, "echo $0").scope, ShellTargetScope::Unknown);
    EXPECT_EQ(classify(root, "echo ${0}").scope, ShellTargetScope::Unknown);
}

TEST(ShellScope94, SpecialParameterTrailer) {
    ymh::test::TempWorkspace workspace("p94_trailer");
    const auto root = workspace.path();
    const std::string command = "cd " + root.string() +
                                " && grep -n \"T7: deallocate\" test/x.cpp; "
                                "echo \"exit: $?\"";
    EXPECT_EQ(classify(root, command).scope, ShellTargetScope::InRoot);
    EXPECT_EQ(classify(root, "echo \"exit: $?\"").scope, ShellTargetScope::InRoot);
    EXPECT_EQ(classify(root, "echo \"status: $? (done)\"").scope,
              ShellTargetScope::InRoot);
}

TEST(ShellScope94, NamedVariableRemainsUnknown) {
    ymh::test::TempWorkspace workspace("p94_named");
    const auto root = workspace.path();
    for (const char* text : {"echo $FOO", "echo ${FOO}/x", "cmake -DKEY=$FOO .",
                             "cat foo$BAR", "cat foo${BAR}", "echo foo$bar",
                             "cat foo$(echo eA== | base64 -d)"}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::Unknown) << text;
    }
    EXPECT_EQ(classify(root, "cat foo`echo etc`").scope, ShellTargetScope::Unknown);
}

TEST(ShellScope94, HomeRemainsOutside) {
    ymh::test::TempWorkspace workspace("p94_home");
    const auto root = workspace.path();
    for (const char* text : {"echo $HOME", "cmake -DKEY=$HOME .",
                             "cmake -DCMAKE_PREFIX_PATH=$HOME/x ."}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::Outside) << text;
    }
}

TEST(ShellScope94, CmakeFormsInRoot) {
    ymh::test::TempWorkspace workspace("p94_cmake");
    const auto root = workspace.path();
    for (const char* text : {"cmake -S . -B build", "cmake --build build",
                             "cmake --build build -j",
                             "cmake -DKEY=value -S . -B build",
                             "cmake -DCMAKE_BUILD_TYPE=Debug -B build ."}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::InRoot) << text;
    }
}

TEST(ShellScope94, CmakePathLikeDefine) {
    ymh::test::TempWorkspace workspace("p94_cmake_paths");
    const auto root = workspace.path();
    for (const char* text : {"cmake -DKEY=a/b -S . -B build",
                             "cmake -DCMAKE_INSTALL_PREFIX=./stage ."}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::InRoot) << text;
    }
}

TEST(ShellScope94, CmakeOutsideDefine) {
    ymh::test::TempWorkspace workspace("p94_cmake_outside");
    const auto root = workspace.path();
    EXPECT_EQ(classify(root, "cmake -DKEY=../x .").scope, ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "cmake -DKEY=$? .").scope, ShellTargetScope::InRoot);
    EXPECT_EQ(classify(root, "cmake -DKEY=$@ .").scope, ShellTargetScope::InRoot);
    EXPECT_EQ(classify(root, "cmake -DKEY=$FOO .").scope, ShellTargetScope::Unknown);
    EXPECT_EQ(classify(root, "cmake -DCMAKE_CXX_FLAGS=-I../include .").scope,
              ShellTargetScope::Unknown);
}

TEST(ShellScope94, ExitStatusInCompound) {
    ymh::test::TempWorkspace workspace("p94_compound");
    EXPECT_EQ(classify(workspace.path(), "cmake --build build && echo \"exit: $?\"").scope,
              ShellTargetScope::InRoot);
}

TEST(ShellScope94, OutsideStillAsks) {
    ymh::test::TempWorkspace workspace("p94_outside");
    const auto root = workspace.path();
    for (const char* text : {"cat /etc/passwd", "cat ../x", "cat ~/.ssh/id_rsa"}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::Outside) << text;
    }
}

TEST(ShellScope94, DestructiveTrailerKeepsGuard) {
    ymh::test::TempWorkspace workspace("p94_destructive");
    const auto root = workspace.path();
    const ShellClassification outside = classify(root, "rm -rf ../x; echo \"exit: $?\"");
    EXPECT_EQ(outside.risk, ShellCommandRisk::Forbid);
    EXPECT_EQ(outside.scope, ShellTargetScope::Outside);
    const ShellClassification inside = classify(root, "rm -rf build; echo \"exit: $?\"");
    EXPECT_EQ(inside.risk, ShellCommandRisk::None);
    EXPECT_EQ(inside.scope, ShellTargetScope::InRoot);
}

TEST(ShellScope94, ProtectedStillDenied) {
    ymh::test::TempWorkspace workspace("p94_protected");
    RulePermissionPolicy policy(in_root_allow());
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "cat .ymh/permissions.jsonc")),
              PolicyVerdict::Deny);
}

TEST(ShellScope94, QuotedDollar) {
    ymh::test::TempWorkspace workspace("p94_quoted");
    EXPECT_EQ(classify(workspace.path(), "echo '$?'").scope, ShellTargetScope::InRoot);
}

TEST(ShellScope94, PathRuleMatchesAfterFix) {
    ymh::test::TempWorkspace workspace("p94_rule");
    const auto root = workspace.path();
    const std::string command = "cd " + root.string() +
                                " && grep -n \"T7: deallocate\" test/x.cpp; "
                                "echo \"exit: $?\"";
    RulePermissionPolicy policy(in_root_allow());
    EXPECT_EQ(policy.evaluate(shell_request(root, command)), PolicyVerdict::Allow);
}
