#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include "support/test_env.hpp"
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
                ("ymh103_" + name + "_" + std::to_string(::getpid()) + "_" +
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

PermissionConfig ask_all() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    return config;
}

PermissionRequest shell_request(const std::filesystem::path& root, std::string command) {
    PermissionRequest request;
    request.call      = "call-1";
    request.session   = SessionId{"s1"};
    request.tool      = "shell";
    request.root      = root;
    request.arguments = {{"command", std::move(command)}};
    return request;
}

PermissionRequest terminal_request(const std::filesystem::path& root, std::string action,
                                   nlohmann::json extra) {
    PermissionRequest request;
    request.call      = "call-1";
    request.session   = SessionId{"s1"};
    request.tool      = "terminal";
    request.root      = root;
    request.arguments = std::move(extra);
    request.arguments["action"] = std::move(action);
    return request;
}

ShellClassification classify(const PermissionRequest& request) {
    return classify_shell_request(request, canonical_tmp_root());
}

PolicyVerdict evaluate(const PermissionConfig& config, const PermissionRequest& request) {
    RulePermissionPolicy policy(config);
    return policy.evaluate(request);
}

void write_file(const std::filesystem::path& path, std::string_view data) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path);
    out << data;
}

} // namespace

TEST(Errata103, AllowsNeutralReadWrite) {
    ymh::test::TempWorkspace workspace("p103_allow", ymh::test::test_workspace_base());
    TempRootDir             tmp("p103_allow");
    const std::filesystem::path a = workspace.path() / "a";
    const std::filesystem::path b = workspace.path() / "b";
    write_file(a, "alpha");
    write_file(b, "beta");
    const PermissionConfig config = ask_all();

    for (const std::string& command :
         {"md5sum " + a.string() + " > " + (tmp / "x").string(),
          "sha256sum " + a.string() + " " + b.string() + " > " + (tmp / "y").string(),
          "stat " + a.string() + " > " + (tmp / "z").string(),
          "cmp " + a.string() + " " + b.string() + " > " + (tmp / "w").string(),
          "md5sum -- " + a.string() + " > " + (tmp / "v").string(),
          "stat -c '%n' " + a.string() + " > " + (tmp / "u").string(),
          "cmp -i 1 " + a.string() + " " + b.string() + " > " + (tmp / "t").string(),
          "cmp -n 2 " + a.string() + " " + b.string() + " > " + (tmp / "s").string(),
          "cd " + workspace.path().string() + " && md5sum " + a.string() + " > " +
              (tmp / "r").string()}) {
        const PermissionRequest request = shell_request(workspace.path(), command);
        EXPECT_TRUE(classify(request).tmp_exempt) << command;
        EXPECT_EQ(evaluate(config, request), PolicyVerdict::Allow) << command;
    }
}

TEST(Errata103, RejectsOutsideSymlinkProtected) {
    ymh::test::TempWorkspace workspace("p103_outside", ymh::test::test_workspace_base());
    TempRootDir             tmp("p103_outside");
    const std::filesystem::path a = workspace.path() / "a";
    write_file(a, "alpha");
    write_file(workspace.path() / ".git" / "config", "[core]\n");
    std::error_code error;
    std::filesystem::create_symlink("/etc/hostname", workspace.path() / "link", error);

    for (const std::string& command :
         {"md5sum /etc/hostname > " + (tmp / "x").string(),
          "md5sum ../outside > " + (tmp / "y").string(),
          "md5sum " + (workspace.path() / "link").string() + " > " + (tmp / "z").string(),
          "stat " + (workspace.path() / ".git" / "config").string() + " > " +
              (tmp / "w").string(),
          "cmp -l " + (workspace.path() / ".git" / "config").string() + " " +
              a.string() + " | tee " + (tmp / "v").string()}) {
        const PermissionRequest request = shell_request(workspace.path(), command);
        EXPECT_FALSE(request_is_tmp_exempt(request, classify(request),
                                          canonical_tmp_root()))
            << command;
    }
}

TEST(Errata103, RejectsOptionsPrefixesAndInjection) {
    ymh::test::TempWorkspace workspace("p103_opts", ymh::test::test_workspace_base());
    TempRootDir             tmp("p103_opts");
    const std::filesystem::path a = workspace.path() / "a";
    const std::filesystem::path b = workspace.path() / "b";
    write_file(a, "alpha");
    write_file(b, "beta");

    for (const std::string& command :
         {"md5sum -c list > " + (tmp / "x").string(),
          "md5sum -wc list > " + (tmp / "y").string(),
          "md5sum --chec list > " + (tmp / "z").string(),
          "md5sum - > " + (tmp / "w").string(),
          "stat -L " + a.string() + " > " + (tmp / "v").string(),
          "stat -Lc '%n' " + a.string() + " > " + (tmp / "u").string(),
          "stat > " + (tmp / "t").string(),
          "cmp " + a.string() + " > " + (tmp / "s").string(),
          "./md5sum " + a.string() + " > " + (tmp / "r").string(),
          "env --chdir=/etc md5sum hostname > " + (tmp / "q").string(),
          "sudo md5sum " + a.string() + " > " + (tmp / "p").string(),
          "PATH=/workspace:$PATH md5sum " + a.string() + " > " + (tmp / "o").string(),
          "PATH=/workspace; md5sum " + a.string() + " > " + (tmp / "n").string(),
          "CDPATH=/etc; cd ssl; md5sum x > " + (tmp / "m").string(),
          "stat -c \"$(cat /etc/passwd)\" " + a.string() + " > " + (tmp / "l").string(),
          "cmp -i \"$(id)\" " + a.string() + " " + b.string() + " > " +
              (tmp / "k").string()}) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
    }
}

TEST(Errata103, Regressions) {
    ymh::test::TempWorkspace workspace("p103_reg", ymh::test::test_workspace_base());
    TempRootDir             tmp("p103_reg");
    const std::filesystem::path a = workspace.path() / "a";
    const std::filesystem::path b = workspace.path() / "b";
    write_file(a, "alpha");
    write_file(b, "beta");
    const PermissionConfig config = ask_all();

    EXPECT_TRUE(classify(shell_request(workspace.path(),
                                       "echo hi > " + (tmp / "x").string()))
                    .tmp_exempt);
    EXPECT_EQ(evaluate(config,
                       shell_request(workspace.path(),
                                     "echo hi > " + (tmp / "y").string())),
              PolicyVerdict::Allow);
    for (const std::string& command :
         {"rm -rf " + a.string() + " > " + (tmp / "z").string(),
          "md5sum " + a.string() + " > " + (tmp / "w").string() + "; rm " + b.string(),
          "docker exec c sh -c 'true' > " + (tmp / "v").string()}) {
        EXPECT_FALSE(classify(shell_request(workspace.path(), command)).tmp_exempt)
            << command;
    }
}

TEST(Errata103, TerminalNeverExemptsReads) {
    ymh::test::TempWorkspace workspace("p103_term", ymh::test::test_workspace_base());
    TempRootDir             tmp("p103_term");
    std::error_code         error;
    const std::filesystem::path cwd = tmp / "sub";
    std::filesystem::create_directories(cwd, error);
    const std::filesystem::path a = workspace.path() / "a";
    write_file(a, "alpha");
    const PermissionConfig config = ask_all();

    const PermissionRequest echo_open =
        terminal_request(workspace.path(), "open",
                         {{"cwd", cwd.string()}, {"command", "echo hi > out"}});
    EXPECT_TRUE(classify(echo_open).tmp_exempt);
    EXPECT_EQ(evaluate(config, echo_open), PolicyVerdict::Allow);

    const PermissionRequest read_open =
        terminal_request(workspace.path(), "open",
                         {{"cwd", cwd.string()},
                          {"command", "md5sum " + a.string() + " > out"}});
    EXPECT_FALSE(classify(read_open).tmp_exempt);

    const PermissionRequest echo_write =
        terminal_request(workspace.path(), "write",
                         {{"input", "echo hi > " + (tmp / "x").string()}});
    EXPECT_TRUE(classify(echo_write).tmp_exempt);
    EXPECT_EQ(evaluate(config, echo_write), PolicyVerdict::Allow);

    const PermissionRequest read_write =
        terminal_request(workspace.path(), "write",
                         {{"input", "md5sum " + a.string() + " > " +
                                        (tmp / "y").string()}});
    EXPECT_FALSE(classify(read_write).tmp_exempt);
}
