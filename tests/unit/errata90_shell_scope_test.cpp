#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "support/test_env.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;

ShellClassification classify(const std::filesystem::path& root, std::string_view text,
                             bool kernel = true,
                             const std::filesystem::path& cwd = {}) {
    return classify_shell_command_with_scope(text, root, cwd.empty() ? root : cwd,
                                             kernel, canonical_tmp_root());
}

PermissionRequest terminal_request(const std::filesystem::path& root, std::string action,
                                   nlohmann::json extra = nlohmann::json::object()) {
    PermissionRequest request;
    request.call    = "call-1";
    request.session = SessionId{"s1"};
    request.tool    = "terminal";
    request.root    = root;
    request.arguments = std::move(extra);
    request.arguments["action"] = std::move(action);
    request.kernel_confinement = true;
    return request;
}

} // namespace

TEST(ShellScope, InRootCommand) {
    ymh::test::TempWorkspace workspace("p90_scope_in");
    const auto root = workspace.path();
    EXPECT_EQ(classify(root, "ls").scope, ShellTargetScope::InRoot);
    EXPECT_EQ(classify(root, "git status").scope, ShellTargetScope::InRoot);
    EXPECT_EQ(classify(root, "cargo build").scope, ShellTargetScope::InRoot);
}

TEST(ShellScope, OutsideAbsolute) {
    ymh::test::TempWorkspace workspace("p90_scope_abs");
    EXPECT_EQ(classify(workspace.path(), "cat /etc/passwd").scope,
              ShellTargetScope::Outside);
}

TEST(ShellScope, OutsideDotDot) {
    ymh::test::TempWorkspace workspace("p90_scope_dotdot");
    EXPECT_EQ(classify(workspace.path(), "cat ../x").scope, ShellTargetScope::Outside);
    EXPECT_EQ(classify(workspace.path(), "rm -rf ../x").scope,
              ShellTargetScope::Outside);
}

TEST(ShellScope, OutsideHome) {
    ymh::test::TempWorkspace workspace("p90_scope_home");
    EXPECT_EQ(classify(workspace.path(), "cat ~/.ssh/id_rsa").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(workspace.path(), "cat $HOME/x").scope,
              ShellTargetScope::Outside);
}

TEST(ShellScope, NestedShellInRoot) {
    ymh::test::TempWorkspace workspace("p90_scope_nested");
    EXPECT_EQ(classify(workspace.path(), "bash -c 'ls'").scope,
              ShellTargetScope::InRoot);
}

TEST(ShellScope, NestedShellOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_nested_out");
    EXPECT_EQ(classify(workspace.path(), "bash -c 'cat /etc/passwd'").scope,
              ShellTargetScope::Outside);
}

TEST(ShellScope, InterpreterPayloadUnknown) {
    ymh::test::TempWorkspace workspace("p90_scope_interp");
    const auto root = workspace.path();
    for (const char* text : {"python3 -c 'print(1)'", "node -e 'x'",
                             "awk 'BEGIN{x}'", "sed 's/x/y/'",
                             "find . -exec cat {} +", "xargs rm"}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::Unknown) << text;
    }
}

TEST(ShellScope, DotDotOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_cd");
    EXPECT_EQ(classify(workspace.path(), "cd ..").scope, ShellTargetScope::Outside);
}

TEST(ShellScope, CdOutsideThenInside) {
    ymh::test::TempWorkspace workspace("p90_scope_cd2");
    EXPECT_EQ(classify(workspace.path(), "cd /tmp && cd \"$PWD\" && ls").scope,
              ShellTargetScope::Outside);
}

TEST(ShellScope, HostPath) {
    ymh::test::TempWorkspace workspace("p90_scope_host");
    EXPECT_EQ(classify(workspace.path(), "scp host:/etc/passwd .").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(workspace.path(), "curl https://example.com").scope,
              ShellTargetScope::InRoot);
}

TEST(ShellScope, FileUrlOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_file");
    const auto root = workspace.path();
    EXPECT_EQ(classify(root, "curl file:///etc/passwd").scope, ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl file://localhost/etc/passwd").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl file:///etc/%70asswd").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl file://otherhost/x").scope,
              ShellTargetScope::Unknown);
}

TEST(ShellScope, HttpsUrlNeutral) {
    ymh::test::TempWorkspace workspace("p90_scope_https");
    EXPECT_EQ(classify(workspace.path(), "curl https://example.com").scope,
              ShellTargetScope::InRoot);
    EXPECT_EQ(classify(workspace.path(), "wget https://host/file").scope,
              ShellTargetScope::InRoot);
}

TEST(ShellScope, CurlDataFileOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_curl");
    const auto root = workspace.path();
    EXPECT_EQ(classify(root, "curl -d @/etc/passwd").scope, ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl --data=@/etc/passwd").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl -d @../../etc/passwd").scope,
              ShellTargetScope::Outside);
    EXPECT_TRUE(classify(root, "curl -d @.ymh/config.jsonc").protected_target);
}

TEST(ShellScope, CurlFormFileOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_curlform");
    const auto root = workspace.path();
    EXPECT_EQ(classify(root, "curl -F f=@/etc/passwd").scope, ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl -F f=</etc/passwd").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl --data-urlencode name@/etc/passwd").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "curl --data-urlencode name@-").scope,
              ShellTargetScope::InRoot);
}

TEST(ShellScope, ProtectedGlobDenied) {
    ymh::test::TempWorkspace workspace("p90_scope_protglob");
    const auto root = workspace.path();
    EXPECT_TRUE(classify(root, "rm -rf .ymh*").protected_target);
    EXPECT_TRUE(classify(root, "rm -rf .git*").protected_target);
    EXPECT_TRUE(classify(root, "rm -rf .ym?").protected_target);
    EXPECT_TRUE(classify(root, "rm -rf .*").protected_target);
    EXPECT_TRUE(classify(root, "rm -rf src/.ymh*").protected_target);
    EXPECT_FALSE(classify(root, "ls *").protected_target);
    EXPECT_FALSE(classify(root, "cat **").protected_target);
    EXPECT_FALSE(classify(root, "cat /tmp/repo/.git/config").protected_target);
    EXPECT_FALSE(classify(root, "rm -rf .ymhfoo").protected_target);
}

TEST(ShellScope, GitStashAllProtected) {
    ymh::test::TempWorkspace workspace("p90_scope_stash");
    const auto root = workspace.path();
    for (const char* text : {"git stash -a", "git stash -u",
                             "git stash --include-untracked", "git stash -au",
                             "git stash -ua", "git stash -uab"}) {
        EXPECT_TRUE(classify(root, text).protected_target) << text;
    }
    EXPECT_FALSE(classify(root, "git stash").protected_target);
}

TEST(ShellScope, GitRemoteMutationProtected) {
    ymh::test::TempWorkspace workspace("p90_scope_remote");
    const auto root = workspace.path();
    for (const char* text : {
             "git remote add evil file:///tmp/x", "git remote set-url origin x",
             "git branch --set-upstream-to=origin/main",
             "git branch --track br2 origin/master", "git branch --unset-upstream br2",
             "git branch --edit-description", "git checkout --track origin/master",
             "git checkout -b x --track", "git switch -c sw --track origin/master",
             "git push -u origin main"}) {
        EXPECT_TRUE(classify(root, text).protected_target) << text;
    }
}

TEST(ShellScope, GitAutoTrackProtected) {
    ymh::test::TempWorkspace workspace("p90_scope_autotrack");
    const auto root = workspace.path();
    for (const char* text : {
             "git checkout -b feat origin/main", "git checkout -B feat2 origin/main",
             "git branch feat3 origin/main", "git switch -c feat4 origin/main",
             "git switch -C feat5 origin/main", "git switch -t origin/main",
             "git switch --track origin/main", "git branch -m old new",
             "git branch -M old new", "git branch -c main copyb",
             "git branch -C main copyc", "git -c branch.autoSetupMerge=always checkout -b z1"}) {
        EXPECT_TRUE(classify(root, text).protected_target) << text;
    }
    EXPECT_FALSE(classify(root, "git checkout -b z2 --no-track").protected_target);
    EXPECT_FALSE(classify(root, "git checkout main").protected_target);
}

TEST(ShellScope, GitReadOnlyFormsAllowed) {
    ymh::test::TempWorkspace workspace("p90_scope_gitread");
    const auto root = workspace.path();
    for (const char* text : {"git remote -v", "git remote prune",
                             "git submodule status", "git worktree list"}) {
        EXPECT_FALSE(classify(root, text).protected_target) << text;
    }
}

TEST(ShellScope, GitFetchSetUpstreamProtected) {
    ymh::test::TempWorkspace workspace("p90_scope_fetch");
    const auto root = workspace.path();
    EXPECT_TRUE(classify(root, "git fetch --set-upstream origin main").protected_target);
    EXPECT_TRUE(classify(root, "git pull --set-upstream origin main").protected_target);
    EXPECT_FALSE(classify(root, "git fetch -u origin main").protected_target);
    EXPECT_FALSE(classify(root, "git pull -u").protected_target);
}

TEST(ShellScope, GitAttachedBranchOptionProtected) {
    ymh::test::TempWorkspace workspace("p90_scope_attach");
    const auto root = workspace.path();
    for (const char* text : {
             "git branch --move old new", "git branch --copy main copyb",
             "git switch --create=bar origin/main",
             "git switch --force-create=baz origin/main", "git checkout -bfoo",
             "git checkout -Bfoo", "git switch -cfoo", "git switch -Cfoo",
             "git branch -uorigin/main", "git branch -torigin/x",
             "git branch -d feat", "git branch -D feat", "git branch --delete feat"}) {
        EXPECT_TRUE(classify(root, text).protected_target) << text;
    }
}

TEST(ShellScope, GitConfigReadFormsReadOnly) {
    ymh::test::TempWorkspace workspace("p90_scope_gitcfg");
    const auto root = workspace.path();
    for (const char* text : {"git config --get x", "git config --get-all x",
                             "git config --get-regexp x", "git config --list"}) {
        EXPECT_FALSE(classify(root, text).protected_target) << text;
    }
    EXPECT_TRUE(classify(root, "git config --worktree x y").protected_target);
}

TEST(ShellScope, EmptyNotApplicable) {
    EXPECT_EQ(classify_shell_command_with_scope("", "/tmp", "/tmp", true,
                                                canonical_tmp_root())
                  .scope,
              ShellTargetScope::NotApplicable);
}

TEST(ShellScope, ConfirmScopeDependsOnConfinement) {
    ymh::test::TempWorkspace workspace("p90_scope_confirm");
    const auto on = classify(workspace.path(), "rm -rf build", true);
    EXPECT_EQ(on.risk, ShellCommandRisk::None);
    EXPECT_EQ(on.scope, ShellTargetScope::InRoot);
    const auto off = classify(workspace.path(), "rm -rf build", false);
    EXPECT_EQ(off.risk, ShellCommandRisk::Confirm);
    EXPECT_EQ(off.scope, ShellTargetScope::Unknown);
}

TEST(ShellScope, TerminalWriteAlwaysUnknown) {
    ymh::test::TempWorkspace workspace("p90_scope_termwrite");
    const auto root = workspace.path();
    EXPECT_EQ(classify_shell_request(terminal_request(root, "write", {{"input", "ls\n"}})).scope,
              ShellTargetScope::Unknown);
    EXPECT_EQ(classify_shell_request(
                  terminal_request(root, "write", {{"input", "cd /tmp\n"}}))
                  .scope,
              ShellTargetScope::Unknown);
    EXPECT_EQ(classify_shell_request(
                  terminal_request(root, "open", {{"command", "ls"}}))
                  .scope,
              ShellTargetScope::InRoot);
}

TEST(ShellScope, TerminalOpenCwdOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_termcwd");
    const auto root = workspace.path();
    const auto outside =
        classify_shell_request(terminal_request(root, "open",
                                                {{"cwd", "/tmp"}, {"command", "cat x"}}));
    EXPECT_EQ(outside.scope, ShellTargetScope::Outside);
    const auto relative =
        classify_shell_request(terminal_request(root, "open",
                                                {{"cwd", "../../x"}}));
    EXPECT_EQ(relative.scope, ShellTargetScope::Outside);
    const auto inside = classify_shell_request(
        terminal_request(root, "open", {{"cwd", "src"}, {"command", "cat ../../x"}}));
    EXPECT_EQ(inside.scope, ShellTargetScope::Outside);
}

TEST(ShellScope, TerminalOpenCwdProtected) {
    ymh::test::TempWorkspace workspace("p90_scope_termprot");
    const auto root = workspace.path();
    EXPECT_TRUE(classify_shell_request(
                    terminal_request(root, "open", {{"cwd", ".ymh"}}))
                    .protected_target);
    EXPECT_TRUE(classify_shell_request(
                    terminal_request(root, "open", {{"cwd", ".git"}}))
                    .protected_target);
    EXPECT_EQ(classify_shell_request(
                  terminal_request(root, "open", {{"cwd", 7}}))
                  .scope,
              ShellTargetScope::Unknown);
}

TEST(ShellScope, AttachedOptionOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_attached");
    const auto root = workspace.path();
    EXPECT_EQ(classify(root, "git --git-dir=/tmp/repo log").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "cp -t/tmp x").scope, ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "tar --directory=/tmp -cf x y").scope,
              ShellTargetScope::Outside);
}

TEST(ShellScope, ProtectedTarget) {
    ymh::test::TempWorkspace workspace("p90_scope_prot");
    const auto root = workspace.path();
    EXPECT_TRUE(classify(root, "cat .git/config").protected_target);
    EXPECT_TRUE(classify(root, "chmod +x .git/hooks/pre-commit").protected_target);
    EXPECT_TRUE(classify(root, "rm -rf .ymh").protected_target);
}

TEST(ShellScope, GitControlProtected) {
    ymh::test::TempWorkspace workspace("p90_scope_gitcontrol");
    const auto root = workspace.path();
    for (const char* text : {"git config --local core.hooksPath .evil",
                             "git clean -fdx", "git submodule update",
                             "git worktree add ../x"}) {
        EXPECT_TRUE(classify(root, text).protected_target) << text;
    }
}

TEST(ShellScope, GitGlobalConfigOutside) {
    ymh::test::TempWorkspace workspace("p90_scope_gitglobal");
    const auto root = workspace.path();
    EXPECT_EQ(classify(root, "git config --global x y").scope,
              ShellTargetScope::Outside);
    EXPECT_EQ(classify(root, "git -C /tmp config --local x y").scope,
              ShellTargetScope::Outside);
}

TEST(ShellScope, GitDataPlaneNormal) {
    ymh::test::TempWorkspace workspace("p90_scope_gitdata");
    const auto root = workspace.path();
    for (const char* text : {"git add .", "git commit -m x", "git log", "git status"}) {
        const auto result = classify(root, text);
        EXPECT_EQ(result.scope, ShellTargetScope::InRoot) << text;
        EXPECT_FALSE(result.protected_target) << text;
    }
}

TEST(ShellScope, GitInitAsk) {
    ymh::test::TempWorkspace workspace("p90_scope_gitinit");
    const auto root = workspace.path();
    for (const char* text : {"git init", "git clone x", "git filter-branch"}) {
        EXPECT_EQ(classify(root, text).scope, ShellTargetScope::Unknown) << text;
    }
}

TEST(ShellScope, RiskDelegation) {
    ymh::test::TempWorkspace workspace("p90_scope_risk");
    const auto root = workspace.path();
    for (const char* text : {"ls", "cat /etc/passwd", "rm -rf ~", "rm -rf build",
                             "python3 -c 'x'", "git clean -fdx"}) {
        for (bool kernel : {true, false}) {
            EXPECT_EQ(classify_shell_command(text, root, root, kernel),
                      classify_shell_command_with_scope(text, root, root, kernel,
                                                        canonical_tmp_root())
                          .risk)
                << text;
        }
    }
}
