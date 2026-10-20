#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/test_env.hpp"
#include "ymh/execution/confinement.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;

class TempRootDir {
public:
    explicit TempRootDir(const std::string& name) {
        static std::atomic<unsigned> counter{0};
        path_ = canonical_tmp_root() /
                ("ymh90_" + name + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
    }

    ~TempRootDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempRootDir(const TempRootDir&) = delete;
    TempRootDir& operator=(const TempRootDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path operator/(const std::string& relative) const {
        return path_ / relative;
    }

private:
    std::filesystem::path path_;
};

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name); previous != nullptr) {
            previous_ = previous;
            had_previous_ = true;
        }
        ::setenv(name_, value, 1);
    }

    ~ScopedEnv() {
        if (had_previous_) {
            ::setenv(name_, previous_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::string previous_;
    bool        had_previous_ = false;
};

PolicyRule rule(std::string tool, PolicyVerdict effect, std::string id) {
    PolicyRule value;
    value.tool   = std::move(tool);
    value.effect = effect;
    value.layer  = PolicyRule::Layer::Global;
    value.id     = std::move(id);
    return value;
}

PermissionConfig ask_all() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    return config;
}

PermissionConfig deny_all() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Deny;
    config.rules.push_back(rule("shell", PolicyVerdict::Deny, "shell.deny"));
    config.rules.push_back(rule("terminal", PolicyVerdict::Deny, "terminal.deny"));
    config.rules.push_back(rule("write_file", PolicyVerdict::Deny, "write.deny"));
    return config;
}

PermissionRequest shell_request(const std::filesystem::path& root, std::string command,
                                bool kernel = false,
                                SandboxMode sandbox = SandboxMode::Workspace,
                                bool force_ask = false) {
    PermissionRequest request;
    request.call    = "call-1";
    request.session = SessionId{"s1"};
    request.tool    = "shell";
    request.root    = root;
    request.arguments = {{"command", std::move(command)}};
    request.kernel_confinement = kernel;
    request.sandbox            = sandbox;
    request.force_ask          = force_ask;
    return request;
}

PermissionRequest terminal_request(const std::filesystem::path& root, std::string action,
                                   nlohmann::json extra = nlohmann::json::object(),
                                   bool kernel = false) {
    PermissionRequest request;
    request.call    = "call-1";
    request.session = SessionId{"s1"};
    request.tool    = "terminal";
    request.root    = root;
    request.arguments = std::move(extra);
    request.arguments["action"] = std::move(action);
    request.kernel_confinement = kernel;
    return request;
}

PermissionRequest path_request(const std::filesystem::path& root, std::string tool,
                               std::filesystem::path path) {
    PermissionRequest request;
    request.call    = "call-1";
    request.session = SessionId{"s1"};
    request.tool    = std::move(tool);
    request.root    = root;
    request.path    = std::move(path);
    return request;
}

ShellClassification classify(const PermissionRequest& request) {
    return classify_shell_request(request, canonical_tmp_root());
}

PolicyVerdict evaluate(const PermissionConfig& config,
                       const PermissionRequest& request) {
    RulePermissionPolicy policy(config);
    return policy.evaluate(request);
}

} // namespace

TEST(TempRoot, PredicateStrict) {
    const std::filesystem::path tmp = canonical_tmp_root();
    ASSERT_FALSE(tmp.empty());
    EXPECT_TRUE(path_is_under_tmp_root(tmp, "/tmp/x"));
    EXPECT_TRUE(path_is_under_tmp_root(tmp, "/tmp/a/b"));
    for (const char* candidate : {"/tmp", "/tmp/", "/tmp/.", "/tmpfoo", "/tmp2/x",
                                  "/tmp/..", "/tmp/a/../b", "/tmp/../etc/passwd",
                                  "tmp/x"}) {
        EXPECT_FALSE(path_is_under_tmp_root(tmp, candidate)) << candidate;
    }
}

TEST(TempRoot, EmptyRoot) {
    EXPECT_FALSE(path_is_under_tmp_root({}, "/tmp/x"));
    const std::filesystem::path root = "/workspace";
    ShellClassification        classification;
    classification.tmp_exempt = true;
    classification.tmp_targets.push_back("/tmp/x");
    EXPECT_FALSE(
        request_is_tmp_exempt(shell_request(root, "touch /tmp/x"), classification, {}));
}

TEST(TempRoot, CanonicalRoot) {
    const std::filesystem::path root = canonical_tmp_root();
    ASSERT_FALSE(root.empty());
    std::error_code error;
    const std::filesystem::path expected =
        std::filesystem::weakly_canonical(std::filesystem::path{"/tmp"}, error);
    ASSERT_FALSE(error);
    EXPECT_EQ(root, expected);
}

TEST(TempRoot, TmpdirIgnored) {
    const std::filesystem::path before = canonical_tmp_root();
    ScopedEnv                   tmpdir("TMPDIR", "/var/tmp");
    ScopedEnv                   runtime("XDG_RUNTIME_DIR", "/etc");
    EXPECT_EQ(canonical_tmp_root(), before);
    ymh::test::TempWorkspace workspace("p90_tmp_tmpdir", ymh::test::test_workspace_base());
    const PermissionConfig   config = ask_all();
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "touch $TMPDIR/x")),
              PolicyVerdict::Ask);
}

TEST(TempRoot, WorkspaceUnderTmpDisabled) {
    TempRootDir workspace("p90_tmp_ws");
    ASSERT_TRUE(path_is_under_tmp_root(canonical_tmp_root(), workspace.path()));
    const PermissionConfig config = ask_all();
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "touch /tmp/x")),
              PolicyVerdict::Ask);
    const auto classification =
        classify(shell_request(workspace.path(), "touch /tmp/x"));
    EXPECT_FALSE(request_is_tmp_exempt(shell_request(workspace.path(), "touch /tmp/x"),
                                       classification, canonical_tmp_root()));
}

TEST(TempRoot, ShellWriteAllow) {
    ymh::test::TempWorkspace workspace("p90_tmp_allow", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_allow_target");
    const std::string       target = (tmp / "x").string();
    const PermissionConfig  config = deny_all();
    EXPECT_EQ(evaluate(config,
                       shell_request(workspace.path(), "touch " + target, false,
                                     SandboxMode::ReadOnly, true)),
              PolicyVerdict::Allow);
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "rm " + target)),
              PolicyVerdict::Allow);
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "rm -rf " + target)),
              PolicyVerdict::Deny);
}

TEST(TempRoot, RedirectProducerAllow) {
    ymh::test::TempWorkspace workspace("p90_tmp_redirect", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_redirect_target");
    const PermissionConfig  config = ask_all();
    for (const std::string& command :
         {"printf x > " + (tmp / "a").string(),
          "echo hi > " + (tmp / "b").string(),
          ": > " + (tmp / "c").string(),
          "true > " + (tmp / "d").string()}) {
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Allow)
            << command;
    }
}

TEST(TempRoot, DirectFileWriteAllow) {
    ymh::test::TempWorkspace workspace("p90_tmp_direct", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_direct_target");
    const PermissionConfig  config = ask_all();
    const std::string       a      = (tmp / "a").string();
    const std::string       b      = (tmp / "b").string();
    const std::string       d      = (tmp / "d").string();
    for (const std::string& command : {"rm " + a,
                                       "mv " + a + " " + b,
                                       "mkdir " + d,
                                       "touch " + a,
                                       "truncate " + a,
                                       "tee " + a,
                                       "dd of=" + a,
                                       "printf x > " + a}) {
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Allow)
            << command;
    }
}

TEST(TempRoot, RecursiveWriteNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_recursive", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_recursive_target");
    const PermissionConfig  config = ask_all();
    const std::string       a      = (tmp / "a").string();
    const std::string       b      = (tmp / "b").string();
    const std::string       d      = (tmp / "d").string();
    for (const std::string& command :
         {"rm -rf " + d, "rm -r " + d, "cp -a " + a + " " + b,
          "chmod -R " + d, "chown -R " + d, "find -L " + d + " -delete"}) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Deny)
            << command;
    }
    for (const std::string& command :
         {"rsync " + a + " " + b, "install -D " + a + " " + b}) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Ask)
            << command;
    }
}

TEST(TempRoot, DirectoryTargetNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_directory", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_directory_target");
    const std::filesystem::path d = tmp / "d";
    ASSERT_TRUE(std::filesystem::create_directories(d));
    const PermissionConfig config = ask_all();
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "rm " + d.string())),
              PolicyVerdict::Deny);
    EXPECT_EQ(evaluate(config,
                       shell_request(workspace.path(),
                                     "mv " + d.string() + " " + (tmp / "e").string())),
              PolicyVerdict::Deny);

    const PermissionRequest open = terminal_request(
        workspace.path(), "open", {{"cwd", d.string()}, {"command", "touch x"}});
    EXPECT_EQ(evaluate(config, open), PolicyVerdict::Allow);
    const PermissionRequest missing = terminal_request(
        workspace.path(), "open",
        {{"cwd", (tmp / "missing").string()}, {"command", "touch x"}});
    EXPECT_FALSE(request_is_tmp_exempt(missing, classify(missing),
                                       canonical_tmp_root()));
    EXPECT_EQ(evaluate(config, missing), PolicyVerdict::Ask);
}

TEST(TempRoot, ReadWriteRedirectNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_rw_redirect", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_rw_redirect_target");
    const std::string       x      = (tmp / "x").string();
    const std::string       y      = (tmp / "y").string();
    const PermissionConfig  config = ask_all();
    EXPECT_FALSE(classify(shell_request(workspace.path(), "printf x <> " + x)).tmp_exempt);
    EXPECT_NE(evaluate(config, shell_request(workspace.path(), "printf x <> " + x)),
              PolicyVerdict::Allow);
    EXPECT_FALSE(classify(shell_request(workspace.path(),
                                        "dd of=" + y + " <> " + x))
                     .tmp_exempt);
    EXPECT_NE(evaluate(config, shell_request(workspace.path(),
                                             "dd of=" + y + " <> " + x)),
              PolicyVerdict::Allow);
}

TEST(TempRoot, NonRegularTargetNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_nonregular", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_nonregular_target");
    const std::filesystem::path fifo = tmp / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

    const std::filesystem::path socket_path = tmp / "socket";
    const int                   fd          = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string socket_text = socket_path.string();
    ASSERT_LT(socket_text.size(), sizeof(address.sun_path));
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_text.c_str());
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

    const PermissionConfig config = ask_all();
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "touch " + fifo.string())),
              PolicyVerdict::Ask);
    EXPECT_EQ(evaluate(config,
                       shell_request(workspace.path(), "touch " + socket_path.string())),
              PolicyVerdict::Ask);
    EXPECT_FALSE(
        classify(shell_request(workspace.path(), "touch /tmp/x 2>/dev/null")).tmp_exempt);
    ::close(fd);
}

TEST(TempRoot, ShellReadNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_read", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_read_target");
    const std::string       x      = (tmp / "x").string();
    const PermissionConfig  config = ask_all();
    for (const std::string& command : {"cat " + x, "head " + x}) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Ask)
            << command;
    }
}

TEST(TempRoot, ShellBareRoot) {
    ymh::test::TempWorkspace workspace("p90_tmp_bare_root", ymh::test::test_workspace_base());
    const PermissionConfig  config = ask_all();
    for (const char* command_text : {"rm -rf /tmp", "rm -rf /tmp/", "rm /tmp/.."}) {
        const std::string command = command_text;
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Deny)
            << command;
    }
}

TEST(TempRoot, ShellGlobBrace) {
    ymh::test::TempWorkspace workspace("p90_tmp_glob", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_glob_target");
    const std::string       x      = (tmp / "x").string();
    const PermissionConfig  config = ask_all();
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "rm -rf /tmp/*")),
              PolicyVerdict::Deny);
    const std::vector<std::string> glob_commands = {
        x + "*", "touch " + (tmp / "?.txt").string(), "touch $TMPDIR/x",
        "touch " + tmp.path().string() + "/{x,y}"};
    for (const std::string& command : glob_commands) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_NE(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Allow)
            << command;
    }
    EXPECT_NE(evaluate(config,
                       shell_request(workspace.path(),
                                     "rm -rf /tmp/{x,/etc/passwd}")),
              PolicyVerdict::Allow);
}

TEST(TempRoot, SymlinkWorkspaceAliasNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_symlink_ws", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_symlink_ws_target");
    const std::filesystem::path hooks = workspace.path() / ".git" / "hooks";
    ASSERT_TRUE(std::filesystem::create_directories(hooks));
    const std::filesystem::path link = tmp / "link";
    std::error_code             link_error;
    std::filesystem::create_directory_symlink(hooks, link, link_error);
    ASSERT_FALSE(link_error);
    const PermissionConfig config = ask_all();
    const std::string command = "touch " + (link / "pre-commit").string();
    const PermissionRequest request = shell_request(workspace.path(), command);
    EXPECT_FALSE(request_is_tmp_exempt(request, classify(request),
                                       canonical_tmp_root()));
    EXPECT_EQ(evaluate(config, request), PolicyVerdict::Ask);

    const std::filesystem::path outside = tmp / "outside";
    std::filesystem::create_directory_symlink(std::filesystem::path{"/etc"}, outside,
                                              link_error);
    ASSERT_FALSE(link_error);
    const std::string outside_command = "touch " + (outside / "passwd").string();
    const PermissionRequest outside_request =
        shell_request(workspace.path(), outside_command);
    EXPECT_FALSE(request_is_tmp_exempt(outside_request,
                                       classify(outside_request),
                                       canonical_tmp_root()));
    EXPECT_EQ(evaluate(config, outside_request), PolicyVerdict::Ask);
}

TEST(TempRoot, DanglingSymlinkNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_dangling", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_dangling_target");
    const std::filesystem::path missing =
        workspace.path() / ".git" / "hooks" / "missing";
    const std::filesystem::path link = tmp / "link";
    std::error_code             link_error;
    std::filesystem::create_directory_symlink(missing, link, link_error);
    ASSERT_FALSE(link_error);
    const PermissionConfig config = ask_all();
    const std::string command = "touch " + link.string();
    const PermissionRequest request = shell_request(workspace.path(), command);
    EXPECT_FALSE(request_is_tmp_exempt(request, classify(request),
                                       canonical_tmp_root()));
    EXPECT_EQ(evaluate(config, request), PolicyVerdict::Ask);
}

TEST(TempRoot, SourceOperandReadDenied) {
    ymh::test::TempWorkspace workspace("p90_tmp_source", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_source_target");
    const std::string       a      = (tmp / "a").string();
    const std::string       b      = (tmp / "b").string();
    const PermissionConfig  config = ask_all();
    EXPECT_FALSE(
        classify(shell_request(workspace.path(), "cp " + a + " " + b)).tmp_exempt);
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "cp " + a + " " + b)),
              PolicyVerdict::Deny);
}

TEST(TempRoot, SourceSymlinkNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_source_link", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_source_link_target");
    const std::filesystem::path config_file = workspace.path() / ".git" / "config";
    ASSERT_TRUE(std::filesystem::create_directories(config_file.parent_path()));
    { std::ofstream(config_file) << "x"; }
    const std::filesystem::path link = tmp / "src";
    std::error_code             link_error;
    std::filesystem::create_symlink(config_file, link, link_error);
    ASSERT_FALSE(link_error);
    const PermissionConfig config = ask_all();
    const std::string command = "cp " + link.string() + " " + (tmp / "out").string();
    EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt);
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
              PolicyVerdict::Deny);
}

TEST(TempRoot, MvTempAllow) {
    ymh::test::TempWorkspace workspace("p90_tmp_mv", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_mv_target");
    const PermissionConfig  config = ask_all();
    EXPECT_EQ(evaluate(config,
                       shell_request(workspace.path(),
                                     "mv " + (tmp / "a").string() + " " +
                                         (tmp / "b").string())),
              PolicyVerdict::Allow);
}

TEST(TempRoot, TeeAllow) {
    ymh::test::TempWorkspace workspace("p90_tmp_tee", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_tee_target");
    const PermissionConfig  config = ask_all();
    EXPECT_EQ(evaluate(config,
                       shell_request(workspace.path(), "tee " + (tmp / "x").string())),
              PolicyVerdict::Allow);
}

TEST(TempRoot, DdOfAllow) {
    ymh::test::TempWorkspace workspace("p90_tmp_dd", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_dd_target");
    const PermissionConfig  config = ask_all();
    EXPECT_EQ(evaluate(config,
                       shell_request(workspace.path(),
                                     "dd of=" + (tmp / "x").string())),
              PolicyVerdict::Allow);
}

TEST(TempRoot, UnknownProducerNoRedirect) {
    ymh::test::TempWorkspace workspace("p90_tmp_unknown_producer", ymh::test::test_workspace_base());
    const PermissionConfig  config = ask_all();
    EXPECT_FALSE(classify(shell_request(workspace.path(), "echo hi")).tmp_exempt);
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), "echo hi")),
              PolicyVerdict::Ask);
}

TEST(TempRoot, UnknownProducerWithTempRedirectNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_unknown_redirect", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_unknown_redirect_target");
    const PermissionConfig  config = ask_all();
    const std::string command = "git checkout main > " + (tmp / "x").string();
    EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt);
    EXPECT_NE(evaluate(config, shell_request(workspace.path(), command)),
              PolicyVerdict::Allow);
}

TEST(TempRoot, InputRedirectNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_input_redirect", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_input_redirect_target");
    const PermissionConfig  config = ask_all();
    const std::string command =
        "tee " + (tmp / "x").string() + " < ~/.ssh/id_rsa";
    EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt);
    EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
              PolicyVerdict::Ask);
}

TEST(TempRoot, UnknownWriterNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_unknown_writer", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_unknown_writer_target");
    const PermissionConfig  config = ask_all();
    for (const std::string& command :
         {"rsync " + (tmp / "a").string() + " " + (tmp / "b").string(),
          "curl -o " + (tmp / "x").string() + " https://example.invalid/y"}) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Ask)
            << command;
    }
}

TEST(TempRoot, ShellUntargetedForbid) {
    ymh::test::TempWorkspace workspace("p90_tmp_forbid", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_forbid_target");
    const std::string       x      = (tmp / "x").string();
    const PermissionConfig  config = ask_all();
    for (const std::string& command :
         {"touch " + x + "; :(){ :|:& };:", "touch " + x + "; mkfs /tmp/y"}) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_EQ(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Deny)
            << command;
    }
}

TEST(TempRoot, ShellInterpreter) {
    ymh::test::TempWorkspace workspace("p90_tmp_interpreter", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_interpreter_target");
    const PermissionConfig  config = ask_all();
    const std::vector<std::string> interpreter_commands = {
        "python3 -c 'open(\"" + (tmp / "x").string() + "\",\"w\")'",
        "find /tmp -exec rm {} +", "xargs rm"};
    for (const std::string& command : interpreter_commands) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
        EXPECT_NE(evaluate(config, shell_request(workspace.path(), command)),
                  PolicyVerdict::Allow)
            << command;
    }
}

TEST(TempRoot, PathToolNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_path_tool", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_path_tool_target");
    const PermissionConfig  config = ask_all();
    const std::filesystem::path target = tmp / "x";
    for (const char* tool : {"write_file", "read_file", "grep", "glob"}) {
        EXPECT_EQ(evaluate(config, path_request(workspace.path(), tool, target)),
                  PolicyVerdict::Ask)
            << tool;
    }
}

TEST(TempRoot, TerminalOpenTemp) {
    ymh::test::TempWorkspace workspace("p90_tmp_terminal_open", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_terminal_open_target");
    const std::filesystem::path sub = tmp / "sub";
    ASSERT_TRUE(std::filesystem::create_directories(sub));
    const PermissionConfig config = ask_all();
    EXPECT_EQ(evaluate(config,
                       terminal_request(workspace.path(), "open",
                                        {{"cwd", sub.string()}, {"command", "touch x"}})),
              PolicyVerdict::Allow);
    EXPECT_EQ(evaluate(config,
                       terminal_request(workspace.path(), "open",
                                        {{"cwd", sub.string()},
                                         {"command", "touch " + (tmp / "x").string()}})),
              PolicyVerdict::Allow);
}

TEST(TempRoot, TerminalOpenCwdRequired) {
    ymh::test::TempWorkspace workspace("p90_tmp_terminal_cwd", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_terminal_cwd_target");
    const std::string       target = "touch " + (tmp / "x").string();
    const PermissionConfig  config = ask_all();
    for (const nlohmann::json& extra :
         {nlohmann::json{{"cwd", workspace.path().string()}, {"command", target}},
          nlohmann::json{{"command", target}},
          nlohmann::json{{"cwd", "/etc"}, {"command", target}}}) {
        const PermissionRequest request = terminal_request(workspace.path(), "open", extra);
        EXPECT_FALSE(classify(request).tmp_exempt);
        EXPECT_EQ(evaluate(config, request), PolicyVerdict::Ask);
    }
}

TEST(TempRoot, TerminalOpenBare) {
    ymh::test::TempWorkspace workspace("p90_tmp_terminal_bare", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_terminal_bare_target");
    const std::filesystem::path sub = tmp / "sub";
    ASSERT_TRUE(std::filesystem::create_directories(sub));
    const PermissionConfig config = ask_all();
    const PermissionRequest request =
        terminal_request(workspace.path(), "open", {{"cwd", sub.string()}});
    EXPECT_FALSE(classify(request).tmp_exempt);
    EXPECT_EQ(evaluate(config, request), PolicyVerdict::Ask);
}

TEST(TempRoot, BareInterpreterNotExempt) {
    ymh::test::TempWorkspace workspace("p90_tmp_terminal_interpreter", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_terminal_interpreter_target");
    const std::filesystem::path sub = tmp / "sub";
    ASSERT_TRUE(std::filesystem::create_directories(sub));
    const PermissionConfig config = ask_all();
    const PermissionRequest request =
        terminal_request(workspace.path(), "open",
                         {{"cwd", sub.string()}, {"command", "bash"}});
    EXPECT_FALSE(classify(request).tmp_exempt);
    EXPECT_EQ(evaluate(config, request), PolicyVerdict::Ask);
}

TEST(TempRoot, TerminalWriteTemp) {
    ymh::test::TempWorkspace workspace("p90_tmp_terminal_write", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_terminal_write_target");
    const PermissionConfig  config = ask_all();
    EXPECT_EQ(evaluate(config,
                       terminal_request(workspace.path(), "write",
                                        {{"input",
                                          "touch " + (tmp / "x").string() + "\n"}})),
              PolicyVerdict::Allow);
    EXPECT_EQ(evaluate(config,
                       terminal_request(workspace.path(), "write",
                                        {{"input", "ls\n"}})),
              PolicyVerdict::Ask);
}

TEST(TempRoot, ConfinementTmpRoot) {
    ymh::test::TempWorkspace workspace("p90_tmp_confinement", ymh::test::test_workspace_base());
    SpawnConfinement          writable(workspace.path(), SandboxMode::Workspace);
    EXPECT_TRUE(writable.policy().tmp_root.has_value());
    EXPECT_EQ(*writable.policy().tmp_root, canonical_tmp_root());
    EXPECT_TRUE(writable.policy().workspace_writable);

    SpawnConfinement read_only(workspace.path(), SandboxMode::ReadOnly);
    EXPECT_FALSE(read_only.policy().workspace_writable);

    TempRootDir       under_tmp("p90_tmp_confinement_under");
    SpawnConfinement  disabled(under_tmp.path(), SandboxMode::Workspace);
    EXPECT_FALSE(disabled.policy().tmp_root.has_value());
}

TEST(TempRoot, NoGrant) {
    ymh::test::TempWorkspace workspace("p90_tmp_no_grant", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_no_grant_target");
    RulePermissionPolicy    policy(ask_all());
    const PermissionRequest request =
        shell_request(workspace.path(), "touch " + (tmp / "x").string());
    EXPECT_TRUE(policy
                    .remember(request, payload::PermissionDecisionKind::Allow,
                              GrantScope::Session)
                    .tool.empty());
    EXPECT_TRUE(policy
                    .remember(request, payload::PermissionDecisionKind::Allow,
                              GrantScope::Always)
                    .tool.empty());
}

TEST(TempRoot, Determinism) {
    ymh::test::TempWorkspace workspace("p90_tmp_determinism", ymh::test::test_workspace_base());
    TempRootDir             tmp("p90_tmp_determinism_target");
    RulePermissionPolicy    policy(ask_all());
    const PermissionRequest request =
        shell_request(workspace.path(), "touch " + (tmp / "x").string());
    EXPECT_EQ(policy.evaluate(request), policy.evaluate(request));
    const ShellClassification first  = classify(request);
    const ShellClassification second = classify(request);
    EXPECT_EQ(first.tmp_exempt, second.tmp_exempt);
    EXPECT_EQ(first.tmp_targets, second.tmp_targets);
}
