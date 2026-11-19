// Multi-root permission policy and shell classifier (Lane B).
//
// A workspace starts with one root; the permission dialog may add another
// directory that is "treated exactly like the current dir". These tests cover
// the policy side (target classification, path-rule matching, grant anchoring)
// and the multi-root shell guard (scope plus outside-target reporting). No I/O
// is needed: every path is a lexical, absolute string.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;

namespace fs = std::filesystem;

PermissionRequest path_request(const fs::path&              root,
                               const std::vector<fs::path>& roots,
                               const std::string&           tool,
                               const fs::path&              target) {
    PermissionRequest request;
    request.tool    = tool;
    request.root    = root;
    request.roots   = roots;
    request.path    = target;
    request.sandbox = SandboxMode::WorkspaceWrite;
    return request;
}

PermissionRequest shell_request(const fs::path&              root,
                                const std::vector<fs::path>& roots,
                                const std::string&           command,
                                bool                         kernel_confinement = false) {
    PermissionRequest request;
    request.tool               = "shell";
    request.root               = root;
    request.roots              = roots;
    request.arguments          = nlohmann::json{{"command", command}};
    request.sandbox            = SandboxMode::WorkspaceWrite;
    request.kernel_confinement = kernel_confinement;
    return request;
}

PolicyRule allow_rule(std::string tool, std::string path = {}) {
    PolicyRule rule;
    rule.tool   = std::move(tool);
    rule.path   = std::move(path);
    rule.effect = PolicyVerdict::Allow;
    rule.layer  = PolicyRule::Layer::Project;
    rule.id     = "test.allow";
    return rule;
}

bool contains_path(const std::vector<fs::path>& paths, const fs::path& needle) {
    for (const fs::path& path : paths) {
        if (path.lexically_normal() == needle.lexically_normal()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Policy: evaluate()
// ---------------------------------------------------------------------------

TEST(PermissionRoots, EvaluateTreatsEveryRootAsTheWorkspace) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(allow_rule("write_file"));
    config.rules.push_back(allow_rule("edit_file"));

    RulePermissionPolicy policy(config);
    const fs::path             root1 = "/ws1";
    const fs::path             root2 = "/ws2";
    const std::vector<fs::path> roots{root1, root2};

    std::string reason;
    EXPECT_EQ(policy.evaluate(
                  path_request(root1, roots, "write_file", root2 / "docs" / "a.md"),
                  &reason),
              PolicyVerdict::Allow);
    EXPECT_TRUE(reason.empty());

    EXPECT_EQ(policy.evaluate(
                  path_request(root1, roots, "edit_file", root2 / "src" / "b.cpp")),
              PolicyVerdict::Allow);

    // `.git` and `.ymh` stay hard-denied in every root.
    const PermissionRequest git =
        path_request(root1, roots, "write_file", root2 / ".git" / "config");
    EXPECT_EQ(policy.evaluate(git, &reason), PolicyVerdict::Deny);
    EXPECT_EQ(reason, "permission:protected-path");

    EXPECT_EQ(policy.evaluate(
                  path_request(root1, roots, "edit_file", root2 / ".ymh" / "permissions.jsonc")),
              PolicyVerdict::Deny);
}

TEST(PermissionRoots, AllowedReadOutsideEveryRootAsksSoTheDialogCanAddIt) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(allow_rule("read_file"));
    config.rules.push_back(allow_rule("write_file"));
    RulePermissionPolicy policy{config};
    const std::vector<fs::path> roots{"/ws1", "/ws2"};

    // Readable through an approved root: the tool-wide allow rule applies.
    EXPECT_EQ(policy.evaluate(path_request("/ws1", roots, "read_file",
                                           "/ws2/src/a.cpp")),
              PolicyVerdict::Allow);
    // Outside every root an Allow would be unreachable (resolve() rejects it at
    // execution time), so the policy asks and the broker can offer the
    // directory instead of failing with PathEscape.
    EXPECT_EQ(policy.evaluate(path_request("/ws1", roots, "read_file",
                                           "/other/src/a.cpp")),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(path_request("/ws1", roots, "write_file",
                                           "/other/src/a.cpp")),
              PolicyVerdict::Ask);

    // The canonical /tmp scratch area keeps the tool-wide read default.
    const fs::path& temp = canonical_tmp_root();
    if (!temp.empty()) {
        EXPECT_EQ(policy.evaluate(path_request("/ws1", roots, "read_file",
                                               temp / "scratch.txt")),
                  PolicyVerdict::Allow);
    }
}

TEST(PermissionRoots, PathRulesAreRelativeToTheContainingRoot) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(allow_rule("write_file", "src/**"));

    RulePermissionPolicy policy(config);
    const fs::path             root1 = "/ws1";
    const fs::path             root2 = "/ws2";
    const std::vector<fs::path> roots{root1, root2};

    // `src/**` matches relative to root2, not relative to the primary root.
    EXPECT_EQ(policy.evaluate(
                  path_request(root1, roots, "write_file", root2 / "src" / "nested" / "a.cpp")),
              PolicyVerdict::Allow);
    // Same relative path under root1 also matches.
    EXPECT_EQ(policy.evaluate(
                  path_request(root1, roots, "write_file", root1 / "src" / "a.cpp")),
              PolicyVerdict::Allow);
    // Outside the pattern under root2 still asks.
    EXPECT_EQ(policy.evaluate(
                  path_request(root1, roots, "write_file", root2 / "tests" / "a.cpp")),
              PolicyVerdict::Ask);
}

// ---------------------------------------------------------------------------
// Policy: remember() anchoring
// ---------------------------------------------------------------------------

TEST(PermissionRoots, AlwaysGrantIsAnchoredToItsRoot) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    RulePermissionPolicy policy(config);

    const fs::path              root1 = "/ws1";
    const fs::path              root2 = "/ws2";
    const std::vector<fs::path> roots{root1, root2};

    // A root2 grant does not extend to root1.
    PermissionRequest root2_request =
        path_request(root1, roots, "edit_file", root2 / "a.cpp");
    EXPECT_EQ(policy.evaluate(root2_request), PolicyVerdict::Ask);

    const PolicyRule root2_grant =
        policy.remember(root2_request, payload::PermissionDecisionKind::AllowAlways,
                        GrantScope::Always);
    ASSERT_FALSE(root2_grant.id.empty());
    EXPECT_EQ(root2_grant.root, root2.string());
    EXPECT_EQ(root2_grant.layer, PolicyRule::Layer::LocalGrant);
    EXPECT_EQ(root2_grant.target_class, TargetClass::InRoot);

    EXPECT_EQ(policy.evaluate(root2_request), PolicyVerdict::Allow);
    PermissionRequest root1_request =
        path_request(root1, roots, "edit_file", root1 / "b.cpp");
    EXPECT_EQ(policy.evaluate(root1_request), PolicyVerdict::Ask);

    const PolicyRule root1_grant =
        policy.remember(root1_request, payload::PermissionDecisionKind::AllowAlways,
                        GrantScope::Always);
    ASSERT_FALSE(root1_grant.id.empty());
    EXPECT_EQ(root1_grant.root, root1.string());
    // Anchoring is part of the id: the same tool grant cannot collide across
    // roots.
    EXPECT_NE(root1_grant.id, root2_grant.id);

    EXPECT_EQ(policy.evaluate(root1_request), PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(root2_request), PolicyVerdict::Allow);
}

TEST(PermissionRoots, NonPathAndDirectoryGrantsKeepHistoricalShape) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    RulePermissionPolicy policy(config);

    const fs::path              root1 = "/ws1";
    const std::vector<fs::path> roots{root1};

    PermissionRequest request;
    request.tool    = "todo_write";   // no path dimension
    request.root    = root1;
    request.roots   = roots;
    request.sandbox = SandboxMode::WorkspaceWrite;

    const PolicyRule grant =
        policy.remember(request, payload::PermissionDecisionKind::AllowAlways,
                        GrantScope::Always);
    EXPECT_TRUE(grant.root.empty());
    EXPECT_EQ(grant.target_class, TargetClass::Any);
    // Empty root keeps the historical id shape exactly: no extra anchor suffix.
    const std::string id_prefix = "grant:todo_write:";
    EXPECT_EQ(grant.id.rfind(id_prefix, 0), 0u);
    EXPECT_EQ(grant.id.size(), id_prefix.size() + 16u);

    // `Directory` is consumed by the broker; `remember` must never turn it into
    // a tool/command grant.
    PermissionRequest path =
        path_request(root1, roots, "write_file", root1 / "src" / "a.cpp");
    const PolicyRule directory =
        policy.remember(path, payload::PermissionDecisionKind::AllowAlways,
                        GrantScope::Directory);
    EXPECT_TRUE(directory.id.empty());
    EXPECT_TRUE(directory.tool.empty());
}

TEST(PermissionRoots, OutsideTargetCreatesNoGrant) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    RulePermissionPolicy policy(config);

    const fs::path              root1 = "/ws1";
    const std::vector<fs::path> roots{root1};

    const PolicyRule grant = policy.remember(
        path_request(root1, roots, "write_file", "/elsewhere/a.cpp"),
        payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
    EXPECT_TRUE(grant.id.empty());
}

// ---------------------------------------------------------------------------
// Shell guard: multi-root scope and outside targets
// ---------------------------------------------------------------------------

TEST(PermissionRoots, ShellScopeAcceptsCommandsUnderAnyRoot) {
    const fs::path              root1 = "/ws1";
    const fs::path              root2 = "/ws2";
    const std::vector<fs::path> roots{root1, root2};

    const ShellClassification multi = classify_shell_request(
        shell_request(root1, roots, "cd /ws2 && rm -rf build", true));
    EXPECT_EQ(multi.scope, ShellTargetScope::InRoot);
    EXPECT_FALSE(multi.path_protected);
    EXPECT_TRUE(multi.outside_targets.empty());

    const ShellClassification single = classify_shell_request(
        shell_request(root1, {root1}, "cd /ws2 && rm -rf build", true));
    EXPECT_EQ(single.scope, ShellTargetScope::Outside);
    EXPECT_TRUE(contains_path(single.outside_targets, fs::path{"/ws2"}));
    EXPECT_TRUE(contains_path(single.outside_targets, fs::path{"/ws2/build"}));
    // Deduped and sorted.
    std::vector<fs::path> sorted = single.outside_targets;
    std::sort(sorted.begin(), sorted.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.string() < right.string();
              });
    EXPECT_EQ(sorted, single.outside_targets);
}

TEST(PermissionRoots, ShellGuardProtectsGitAndYmhInEveryRoot) {
    const fs::path              root1 = "/ws1";
    const fs::path              root2 = "/ws2";
    const std::vector<fs::path> roots{root1, root2};

    const ShellClassification git = classify_shell_request(
        shell_request(root1, roots, "cat /ws2/.git/config"));
    EXPECT_TRUE(git.path_protected);
    EXPECT_TRUE(git.protected_target);

    const ShellClassification ymh = classify_shell_request(
        shell_request(root1, roots, "rm -rf /ws2/.ymh"));
    EXPECT_TRUE(ymh.path_protected);

    // A read of root2's ordinary files is neither outside nor protected.
    const ShellClassification ordinary = classify_shell_request(
        shell_request(root1, roots, "cat /ws2/src/a.cpp"));
    EXPECT_EQ(ordinary.scope, ShellTargetScope::InRoot);
    EXPECT_FALSE(ordinary.path_protected);
    EXPECT_TRUE(ordinary.outside_targets.empty());
}

TEST(PermissionRoots, ShellRequestKeepsTmpScratchAndProtection) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    const fs::path              root = "/ws1";
    const std::vector<fs::path> roots{root};

    const ShellClassification scratch = classify_shell_request(
        shell_request(root, roots, "echo scratch > /tmp/ymh-roots-scratch.txt"),
        canonical_tmp_root());
    EXPECT_TRUE(scratch.tmp_exempt);
    EXPECT_FALSE(scratch.path_protected);

    // `/tmp` stays a separate dimension: it is not an additional root.
    const ShellClassification tmp_cd = classify_shell_request(
        shell_request(root, roots, "cd /tmp && echo hi", true), canonical_tmp_root());
    EXPECT_EQ(tmp_cd.scope, ShellTargetScope::InRoot);

    const ShellClassification git =
        classify_shell_request(shell_request(root, roots, "cat .git/config"));
    EXPECT_TRUE(git.path_protected);
}

} // namespace
