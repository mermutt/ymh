#include <gtest/gtest.h>

#include <string_view>

#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace ymh {
namespace {

ShellTargetScope scope_of(std::string_view command, bool relaxed = true) {
    return classify_shell_command_with_scope(command, "/w", "/w", true, {}, relaxed)
        .scope;
}

} // namespace

// 100-I1 / T1: the motivating stdin pipeline.
TEST(Errata100, T1StdinRangePipelineIsInRoot) {
    EXPECT_EQ(scope_of("git show HEAD:x.cpp | sed -n '80,130p'"),
              ShellTargetScope::InRoot);
    EXPECT_EQ(scope_of("cd /w && git show HEAD:x.cpp | sed -n '80,130p'"),
              ShellTargetScope::InRoot);
}

// T2: substitutions over stdin, including the glob character in a quoted script.
TEST(Errata100, T2StdinSubstitutions) {
    EXPECT_EQ(scope_of("sed -n 's/a/b/'"), ShellTargetScope::InRoot);
    EXPECT_EQ(scope_of("sed -n 's/a/b/g'"), ShellTargetScope::InRoot);
    EXPECT_EQ(scope_of("sed -n 's*a*b*'"), ShellTargetScope::Unknown);
}

// T3: addresses.
TEST(Errata100, T3Addresses) {
    EXPECT_EQ(scope_of("sed -n '1,5!p'"), ShellTargetScope::InRoot);
    EXPECT_EQ(scope_of("sed -n '/x/p'"), ShellTargetScope::InRoot);
}

// T4: write commands are never relaxed.
TEST(Errata100, T4WriteCommands) {
    EXPECT_EQ(scope_of("sed 'w /tmp/x'"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed 'W /tmp/x'"), ShellTargetScope::Unknown);
}

// T5: read and execute commands are never relaxed.
TEST(Errata100, T5ReadAndExecCommands) {
    EXPECT_EQ(scope_of("sed 'r /etc/passwd'"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed 'e id'"), ShellTargetScope::Unknown);
}

// T6: `s` flags that write or execute are rejected.
TEST(Errata100, T6SubstitutionFlags) {
    EXPECT_EQ(scope_of("sed 's/a/b/e'"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed 's/a/b/w out'"), ShellTargetScope::Unknown);
}

// T7: script files, file operands, and in-place writes stay Unknown (v1).
TEST(Errata100, T7FileOperandsAndInPlace) {
    EXPECT_EQ(scope_of("sed -f script.sed"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed -n p x.cpp"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed -i 's/a/b/' x.cpp"), ShellTargetScope::Unknown);
}

// T8: `$` is rejected; labels end at separators.
TEST(Errata100, T8DollarAndLabels) {
    EXPECT_EQ(scope_of("sed -n '$p'"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed -n 'b; w /tmp/x'"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed -n 'b end; p; :end'"), ShellTargetScope::InRoot);
}

// T9: shell expansion in the script is never relaxed.
TEST(Errata100, T9Expansions) {
    EXPECT_EQ(scope_of("sed -n \"$p\""), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed -n '${HOME}'"), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed -n `p`"), ShellTargetScope::Unknown);
}

// T10: `--` bounds option recognition; the first non-option is the script.
TEST(Errata100, T10EndOfOptions) {
    EXPECT_EQ(scope_of("sed -- 'w /tmp/x' -e 's/a/b/'"),
              ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("sed -- '80,130p'"), ShellTargetScope::InRoot);
}

// T11: the production policy path (shell) allows the stdin pipeline.
TEST(Errata100, T11PolicyAllowsInRootPipeline) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    PolicyRule rule;
    rule.tool   = "*";
    rule.path   = "**";
    rule.effect = PolicyVerdict::Allow;
    config.rules.push_back(rule);
    RulePermissionPolicy policy(config);

    PermissionRequest allowed;
    allowed.tool               = "shell";
    allowed.root               = "/w";
    allowed.kernel_confinement = true;
    allowed.arguments = nlohmann::json{{"command", "git show HEAD:x | sed -n '80,130p'"}};
    EXPECT_EQ(policy.evaluate(allowed), PolicyVerdict::Allow);

    PermissionRequest unknown = allowed;
    unknown.arguments = nlohmann::json{{"command", "sed 'w /tmp/x'"}};
    EXPECT_EQ(policy.evaluate(unknown), PolicyVerdict::Ask);
}

// T12: terminal requests and other interpreters are never relaxed.
TEST(Errata100, T12TerminalAndAwk) {
    PermissionRequest terminal;
    terminal.tool               = "terminal";
    terminal.root               = "/w";
    terminal.kernel_confinement = true;
    terminal.arguments = nlohmann::json{{"command", "sed -n '80,130p'"}};
    EXPECT_EQ(classify_shell_request(terminal).scope, ShellTargetScope::Unknown);

    EXPECT_EQ(scope_of("sed -n '80,130p'", false), ShellTargetScope::Unknown);
    EXPECT_EQ(scope_of("awk '{print}'"), ShellTargetScope::Unknown);
}

} // namespace ymh
