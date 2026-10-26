#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;

PermissionConfig allow_in_root() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    PolicyRule rule;
    rule.tool   = "*";
    rule.path   = "**";
    rule.effect = PolicyVerdict::Allow;
    config.rules.push_back(rule);
    return config;
}

PermissionRequest shell_request(const std::filesystem::path& root, std::string command) {
    PermissionRequest request;
    request.call      = "call-1";
    request.session   = SessionId{"s1"};
    request.tool      = "shell";
    request.root      = root;
    request.kernel_confinement = true;
    request.arguments = {{"command", std::move(command)}};
    return request;
}

ShellClassification classify(const PermissionRequest& request) {
    return classify_shell_request(request, canonical_tmp_root());
}

PolicyVerdict evaluate(const PermissionConfig& config, const PermissionRequest& request) {
    RulePermissionPolicy policy(config);
    return policy.evaluate(request);
}

} // namespace

TEST(Errata107, LiteralReadOnlyLoopAllows) {
    ymh::test::TempWorkspace workspace("p107_loop", ymh::test::test_workspace_base());
    const PermissionConfig config = allow_in_root();
    const std::vector<std::string> commands{
        "for f in a.h b.cpp; do git diff -- \"$f\"; done",
        "for f in a.h b.cpp; do echo \"====$f====\"; git diff --diff-filter=U \"$f\"; done",
        "cd " + workspace.path().string() +
            " && for f in a.h b.cpp; do git diff --diff-filter=U \"$f\"; done",
        "for f in a.h b.cpp; do git diff \"$f\"; done; git diff \"$f\""};
    for (const std::string& command : commands) {
        const PermissionRequest request = shell_request(workspace.path(), command);
        EXPECT_EQ(classify(request).scope, ShellTargetScope::InRoot) << command;
        EXPECT_EQ(evaluate(config, request), PolicyVerdict::Allow) << command;
    }
}

TEST(Errata107, OutsideAndProtectedStayRestricted) {
    ymh::test::TempWorkspace workspace("p107_out", ymh::test::test_workspace_base());
    const PermissionConfig config = allow_in_root();

    const PermissionRequest outside =
        shell_request(workspace.path(), "for f in /etc/hostname; do git diff -- \"$f\"; done");
    EXPECT_EQ(evaluate(config, outside), PolicyVerdict::Ask);

    const PermissionRequest protected_value =
        shell_request(workspace.path(),
                      "for f in notes.txt .git/config; do git diff -- \"$f\"; done");
    EXPECT_TRUE(classify(protected_value).protected_target);
    EXPECT_EQ(evaluate(config, protected_value), PolicyVerdict::Deny);
}

TEST(Errata107, UnsafeLoopShapesStayRestricted) {
    ymh::test::TempWorkspace workspace("p107_bad", ymh::test::test_workspace_base());
    const PermissionConfig config = allow_in_root();
    const std::vector<std::string> commands{
         "for f in a; do rm \"$f\"; done",
          "for f in a; do \"$f\"; done",
          "for f in *.h; do git diff -- \"$f\"; done",
          "for f in $(ls); do git diff -- \"$f\"; done",
          "for f in a; do echo $((1)); done",
          "for f in a; do git diff -pO/etc/passwd \"$f\"; done",
          "for f in a; do git diff --config-env=diff.external=EVIL \"$f\"; done",
          "for f in a; do git diff --no-index /etc/passwd \"$f\"; done",
          "for f in a; do git diff --open-files-in-pager=x \"$f\"; done",
          "for f in a; do ./git diff \"$f\"; done",
          "for PATH in .; do git diff -- \"$PATH\"; done",
          "for f in a; do git diff \"$f\"; done; for g in b; do git diff \"$g\"; done",
          "for f in a; do git diff \"$f\"; done | cat"};
    for (const std::string& command : commands) {
        const PermissionRequest request = shell_request(workspace.path(), command);
        EXPECT_NE(evaluate(config, request), PolicyVerdict::Allow) << command;
    }
}
