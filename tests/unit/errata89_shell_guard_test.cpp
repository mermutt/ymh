#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

PermissionRequest shell_request(const fs::path& root,
                                std::string     command,
                                bool            kernel_confinement = true) {
    PermissionRequest request;
    request.call              = "call-1";
    request.session           = SessionId{"session-1"};
    request.tool              = "shell";
    request.root              = root;
    request.sandbox           = SandboxMode::Workspace;
    request.kernel_confinement = kernel_confinement;
    request.arguments          = nlohmann::json::object();
    request.arguments["command"] = std::move(command);
    return request;
}

ShellCommandRisk classify_at(const fs::path&    root,
                             const std::string& command,
                             bool               kernel_confinement = true) {
    return classify_shell_command(command, root, root, kernel_confinement);
}

PermissionConfig shell_allow_config() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    PolicyRule rule;
    rule.tool   = "shell";
    rule.effect = PolicyVerdict::Allow;
    rule.layer  = PolicyRule::Layer::Global;
    rule.id     = "test.shell.allow";
    config.rules.push_back(rule);
    return config;
}

// SG1 -- total, deterministic, I/O-free, never throws (89-D1).
TEST(Errata89ShellGuard, SG1_TotalDeterministicNeverThrows) {
    test::TempWorkspace workspace("sg1_total");
    EXPECT_EQ(classify_at(workspace.path(), ""), ShellCommandRisk::None);
    EXPECT_EQ(classify_at(workspace.path(), "   "), ShellCommandRisk::None);
    EXPECT_EQ(classify_at(workspace.path(), "echo hi"), classify_at(workspace.path(), "echo hi"));
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf /"), ShellCommandRisk::Forbid);
}

// The incident: `rm -fr build ~` through the `shell` tool.
TEST(Errata89ShellGuard, Incident_RmFrBuildTildeIsForbid) {
    test::TempWorkspace workspace("incident");
    EXPECT_EQ(classify_at(workspace.path(), "rm -fr build ~"), ShellCommandRisk::Forbid);
}

// SG4 -- quoting does not hide a dangerous target (89-D2/89-D6).
TEST(Errata89ShellGuard, SG4_QuotingDoesNotHideDangerousTarget) {
    test::TempWorkspace workspace("sg4_quoting");
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf 'build' '~'"), ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf build ~"), ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf \"~\""), ShellCommandRisk::Forbid);
}

// SG3 -- a provably-outside target is Forbid (89-D6/89-D8).
TEST(Errata89ShellGuard, SG3_OutsideRootIsForbid) {
    test::TempWorkspace workspace("sg3_outside");
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf ../escape"), ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf /etc/passwd"), ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf $HOME"), ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf ${HOME}/x"), ShellCommandRisk::Forbid);
}

// SG5 -- an ambiguous/unresolved destructive operand is never None.
TEST(Errata89ShellGuard, SG5_AmbiguousOperandIsConfirmNeverNone) {
    test::TempWorkspace workspace("sg5_ambig");
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf $FOO"), ShellCommandRisk::Confirm);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf *"), ShellCommandRisk::Confirm);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf ."), ShellCommandRisk::Confirm);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf {build,dist}"), ShellCommandRisk::Confirm);
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf 'build"), ShellCommandRisk::Confirm);
}

// SG6 -- terminal `write` input is classified (89-D9).
TEST(Errata89ShellGuard, SG6_TerminalWriteInputIsClassified) {
    test::TempWorkspace workspace("sg6_terminal");
    PermissionRequest  write_request;
    write_request.tool      = "terminal";
    write_request.root      = workspace.path();
    write_request.arguments = nlohmann::json::object();
    write_request.arguments["action"] = "write";
    write_request.arguments["input"]  = "rm -rf /";
    EXPECT_EQ(shell_guard_text(write_request), "rm -rf /");
    EXPECT_EQ(classify_shell_command(shell_guard_text(write_request), workspace.path(),
                                     workspace.path(), true),
              ShellCommandRisk::Forbid);

    PermissionRequest open_request;
    open_request.tool      = "terminal";
    open_request.root      = workspace.path();
    open_request.arguments = nlohmann::json::object();
    open_request.arguments["action"] = "open";
    open_request.arguments["argv"]   = nlohmann::json::array({"rm", "-rf", "~"});
    EXPECT_EQ(shell_guard_text(open_request), "rm -rf ~");

    PermissionConfig  config = shell_allow_config();
    config.default_verdict   = PolicyVerdict::Allow;
    RulePermissionPolicy policy(config);
    EXPECT_EQ(policy.evaluate(write_request), PolicyVerdict::Deny);
}

// SG7 -- an in-root bounded destructive command falls through when L2 is on.
TEST(Errata89ShellGuard, SG7_InRootBoundedFallsThroughWhenL2On) {
    test::TempWorkspace   workspace("sg7_fallthrough");
    RulePermissionPolicy  policy(shell_allow_config());
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf build", true), ShellCommandRisk::None);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -rf build", true)),
              PolicyVerdict::Allow);
}

// SG7/SG13 -- with L2 off, a recognized in-root destructive command is Confirm.
TEST(Errata89ShellGuard, SG7_InRootBoundedConfirmWhenL2Off) {
    test::TempWorkspace  workspace("sg7_l2off");
    EXPECT_EQ(classify_at(workspace.path(), "rm -rf build", false), ShellCommandRisk::Confirm);
    EXPECT_NE(classify_at(workspace.path(), "rm -rf build", false), ShellCommandRisk::None);
}

// SG2 -- no grant, wildcard, default-allow or preset can change the verdict.
TEST(Errata89ShellGuard, SG2_DefaultAllowCannotOverrideForbid) {
    test::TempWorkspace  workspace("sg2_default");
    PermissionConfig     config;
    config.default_verdict = PolicyVerdict::Allow;
    RulePermissionPolicy policy(config);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -rf /")), PolicyVerdict::Deny);
}

TEST(Errata89ShellGuard, SG2_WildcardAndGrantCannotOverrideForbid) {
    test::TempWorkspace workspace("sg2_wildcard");
    PermissionConfig    config;
    config.default_verdict = PolicyVerdict::Allow;
    PolicyRule wildcard;
    wildcard.tool   = "*";
    wildcard.effect = PolicyVerdict::Allow;
    wildcard.layer  = PolicyRule::Layer::Global;
    wildcard.id     = "test.wildcard";
    config.rules.push_back(wildcard);
    RulePermissionPolicy policy(config);
    policy.remember(shell_request(workspace.path(), "rm -fr build ~"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -fr build ~")),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -rf /")), PolicyVerdict::Deny);
}

// SG8 -- non-shell/terminal tools have no guard opinion (89-D9).
TEST(Errata89ShellGuard, SG8_NonShellToolHasNoOpinion) {
    test::TempWorkspace workspace("sg8_other");
    PermissionRequest  request;
    request.tool      = "write_file";
    request.root      = workspace.path();
    request.arguments = nlohmann::json::object();
    request.arguments["command"] = "rm -rf /";
    EXPECT_TRUE(shell_guard_text(request).empty());
    EXPECT_EQ(classify_shell_command(shell_guard_text(request), workspace.path(),
                                     workspace.path(), true),
              ShellCommandRisk::None);

    PermissionConfig     config;
    config.default_verdict = PolicyVerdict::Allow;
    RulePermissionPolicy policy(config);
    EXPECT_EQ(policy.evaluate(request), PolicyVerdict::Allow);
}

// SG9 -- the guard reason is a fixed, echo-free token (09 Q11).
TEST(Errata89ShellGuard, SG9_ReasonIsBoundedAndEchoFree) {
    test::TempWorkspace   workspace("sg9_reason");
    PermissionConfig      config;
    config.default_verdict = PolicyVerdict::Allow;
    RulePermissionPolicy  policy(config);

    std::string reason;
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -rf /"), &reason),
              PolicyVerdict::Deny);
    EXPECT_EQ(reason, "shell-guard:outside-root");
    EXPECT_EQ(reason.find("rm"), std::string::npos);
    EXPECT_EQ(reason.find(workspace.path().string()), std::string::npos);

    std::string confirm_reason;
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -rf $FOO"), &confirm_reason),
              PolicyVerdict::Ask);
    EXPECT_EQ(confirm_reason, "shell-guard:unbounded-in-root");
    EXPECT_EQ(confirm_reason.find("$FOO"), std::string::npos);
}

TEST(Errata89ShellGuard, SG9_GateCarriesGuardReason) {
    test::TempWorkspace  workspace("sg9_gate");
    PermissionConfig     config;
    config.default_verdict = PolicyVerdict::Allow;
    RulePermissionPolicy policy(config);
    PermissionGate       gate(policy, PermissionConfig{});

    const PermissionOutcome outcome =
        gate.resolve(shell_request(workspace.path(), "rm -rf /"), {});
    EXPECT_EQ(outcome.decision, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(outcome.reason, "shell-guard:outside-root");
}

TEST(Errata89ShellGuard, SG9_EvaluateReasonFallbackIsEmptyForNonGuard) {
    test::TempWorkspace   workspace("sg9_fallback");
    PermissionConfig      config;
    config.default_verdict = PolicyVerdict::Allow;
    RulePermissionPolicy  policy(config);
    std::string           reason;
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "echo hi"), &reason),
              PolicyVerdict::Allow);
    EXPECT_TRUE(reason.empty());
}

// SG12 -- adversarial input terminates in bounded time and never throws.
TEST(Errata89ShellGuard, SG12_AdversarialInputTerminates) {
    test::TempWorkspace workspace("sg12_fuzz");
    const fs::path      root = workspace.path();

    EXPECT_NO_THROW(EXPECT_EQ(classify_at(root, std::string(1000000, 'a')), ShellCommandRisk::None));
    EXPECT_EQ(classify_at(root, "rm -rf /" + std::string(1000000, 'a')), ShellCommandRisk::Forbid);

    std::string many = "echo";
    for (int i = 0; i < 10000; ++i) {
        many += " a";
    }
    EXPECT_NO_THROW(classify_at(root, many));

    std::string deep = "rm -rf /";
    for (int i = 0; i < 64; ++i) {
        deep = "echo $(" + deep + ")";
    }
    EXPECT_NE(classify_at(root, deep), ShellCommandRisk::None);
}

// Positive controls: non-destructive commands stay quiet.
TEST(Errata89ShellGuard, PositiveControlsAreNone) {
    test::TempWorkspace workspace("positive");
    for (const char* cmd : {"mkdir -p build", "echo hello", "ls -la", "git status",
                            "rm -rf build/*", "cat build/x"}) {
        EXPECT_EQ(classify_at(workspace.path(), cmd), ShellCommandRisk::None) << cmd;
    }
}

// Wrappers and env prefixes are stripped to reach the real command word (89-D4).
TEST(Errata89ShellGuard, WrappersAndEnvPrefixes) {
    test::TempWorkspace workspace("wrappers");
    const std::vector<std::pair<std::string, ShellCommandRisk>> cases = {
        {"sudo rm -rf /", ShellCommandRisk::Forbid},
        {"sudo -u bob rm -rf /", ShellCommandRisk::Forbid},
        {"env FOO=1 rm -rf ~", ShellCommandRisk::Forbid},
        {"command rm -rf /etc", ShellCommandRisk::Forbid},
        {"exec rm -rf /etc", ShellCommandRisk::Forbid},
        {"nice -n 5 rm -rf ~", ShellCommandRisk::Forbid},
        {"timeout 5 rm -rf /", ShellCommandRisk::Forbid},
        {"stdbuf -oL rm -rf /", ShellCommandRisk::Forbid},
        {"nice -n 5 echo hi", ShellCommandRisk::None},
    };
    for (const auto& [cmd, expected] : cases) {
        EXPECT_EQ(classify_at(workspace.path(), cmd), expected) << cmd;
    }
}

// Nested shells and command substitution recurse (89-D5).
TEST(Errata89ShellGuard, NestedShellsAndSubstitutionsRecurse) {
    test::TempWorkspace workspace("nested");
    const std::vector<std::pair<std::string, ShellCommandRisk>> cases = {
        {"bash -c 'rm -rf /'", ShellCommandRisk::Forbid},
        {"bash -c \"bash -c 'rm -rf /'\"", ShellCommandRisk::Forbid},
        {"sh -lc \"rm -rf ~\"", ShellCommandRisk::Forbid},
        {"echo $(rm -rf /)", ShellCommandRisk::Forbid},
        {"echo `rm -rf ~`", ShellCommandRisk::Forbid},
        {"x=$(rm -rf /)", ShellCommandRisk::Forbid},
    };
    for (const auto& [cmd, expected] : cases) {
        EXPECT_EQ(classify_at(workspace.path(), cmd), expected) << cmd;
    }
}

TEST(Errata89ShellGuard, DepthBoundIsConservative) {
    test::TempWorkspace workspace("depth");
    std::string         too_deep = "rm -rf /";
    for (int i = 0; i < 4; ++i) {
        too_deep = "echo $(" + too_deep + ")";
    }
    EXPECT_EQ(classify_at(workspace.path(), too_deep), ShellCommandRisk::Confirm);
    EXPECT_NE(classify_at(workspace.path(), too_deep), ShellCommandRisk::None);
}

// cwd tracking across a chain (89-D7).
TEST(Errata89ShellGuard, CwdTrackingAcrossChain) {
    test::TempWorkspace workspace("cwd");
    EXPECT_EQ(classify_at(workspace.path(), "cd /tmp && rm -rf x"), ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_at(workspace.path(), "cd build && rm -rf x"), ShellCommandRisk::None);
    EXPECT_EQ(classify_at(workspace.path(), "cd $DIR && rm -rf x"), ShellCommandRisk::Confirm);
    EXPECT_EQ(classify_at(workspace.path(), "cd - && rm -rf x"), ShellCommandRisk::Confirm);
}

// The destructive-verb catalogue and the Deny/Ask split (89-D8).
TEST(Errata89ShellGuard, VerbCatalogueDenyAndAsk) {
    test::TempWorkspace workspace("catalogue");
    const fs::path      root = workspace.path();
    const std::vector<std::pair<std::string, ShellCommandRisk>> cases = {
        {"rmdir /x", ShellCommandRisk::Forbid},
        {"shred ~/f", ShellCommandRisk::Forbid},
        {"truncate /etc/passwd", ShellCommandRisk::Forbid},
        {"truncate build/x", ShellCommandRisk::None},
        {"dd if=/dev/zero of=/etc/passwd", ShellCommandRisk::Forbid},
        {"dd if=/dev/zero of=build/x", ShellCommandRisk::Confirm},
        {"dd if=/dev/zero", ShellCommandRisk::None},
        {"mkfs.ext4 /dev/sda", ShellCommandRisk::Forbid},
        {"wipefs /dev/nvme0n1", ShellCommandRisk::Forbid},
        {"chmod -R 777 /etc", ShellCommandRisk::Forbid},
        {"chmod -R 777 build", ShellCommandRisk::None},
        {"chmod 644 build/x", ShellCommandRisk::None},
        {"chown -R root /etc", ShellCommandRisk::Forbid},
        {"mv build /etc", ShellCommandRisk::Forbid},
        {"mv build dist", ShellCommandRisk::None},
        {"cp -r build /etc", ShellCommandRisk::Forbid},
        {"rm -rf build", ShellCommandRisk::None},
        {"rm -rf ..", ShellCommandRisk::Forbid},
        {"find . -delete", ShellCommandRisk::Confirm},
        {"find / -delete", ShellCommandRisk::Forbid},
        {"find build -delete", ShellCommandRisk::Confirm},
        {"find build -name x", ShellCommandRisk::None},
    };
    for (const auto& [cmd, expected] : cases) {
        EXPECT_EQ(classify_shell_command(cmd, root, root, true), expected) << cmd;
    }
}

TEST(Errata89ShellGuard, RedirectionTargets) {
    test::TempWorkspace workspace("redirect");
    const fs::path      root = workspace.path();
    EXPECT_EQ(classify_shell_command("echo x > /etc/passwd", root, root, true),
              ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_shell_command("echo x >> /dev/sda", root, root, true),
              ShellCommandRisk::Forbid);
    EXPECT_EQ(classify_shell_command("echo x > build/out", root, root, true),
              ShellCommandRisk::None);
    EXPECT_EQ(classify_shell_command("echo x > /dev/null", root, root, true),
              ShellCommandRisk::None);
    EXPECT_EQ(classify_shell_command("echo x > build/out", root, root, false),
              ShellCommandRisk::Confirm);
}

TEST(Errata89ShellGuard, ForkBombIsForbid) {
    test::TempWorkspace workspace("forkbomb");
    EXPECT_EQ(classify_at(workspace.path(), ":(){ :|:& };:"), ShellCommandRisk::Forbid);
}

// path_within_root is lexical and unconditional on `~` (89-D6/SG4).
TEST(Errata89ShellGuard, PathWithinRootLexical) {
    const fs::path root = "/tmp/ws";
    EXPECT_TRUE(path_within_root(root, fs::path("build"), false));
    EXPECT_TRUE(path_within_root(root, root / "src" / "x", false));
    EXPECT_FALSE(path_within_root(root, fs::path("../etc"), false));
    EXPECT_FALSE(path_within_root(root, fs::path("/etc"), false));
    EXPECT_FALSE(path_within_root(root, fs::path("~"), false));
    EXPECT_FALSE(path_within_root(root, fs::path("~"), true));
    EXPECT_FALSE(path_within_root(root, fs::path("$HOME"), false));
    EXPECT_TRUE(path_within_root(root, fs::path("$HOME"), true));
    EXPECT_FALSE(path_within_root(root, fs::path("$HOME/x"), false));
    EXPECT_FALSE(path_within_root(root, fs::path(""), false));
}

// SG13 -- L1.None => L2.on; with L2 off no destructive verb is a silent Allow.
TEST(Errata89ShellGuard, SG13_CouplingNoSilentAllowWhenL2Off) {
    test::TempWorkspace   workspace("sg13_coupling");
    PermissionConfig      config;
    config.default_verdict = PolicyVerdict::Allow;
    RulePermissionPolicy  policy(config);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -rf build", false)),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.path(), "rm -rf /", false)),
              PolicyVerdict::Deny);
}

} // namespace
