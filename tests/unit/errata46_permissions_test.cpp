#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "support/test_env.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;

PermissionRequest request_for(const std::filesystem::path& root,
                              std::string tool,
                              std::string command = std::string(),
                              bool force_ask = false,
                              SandboxMode sandbox = SandboxMode::Workspace) {
    PermissionRequest request;
    request.call    = "call-1";
    request.session = SessionId{"session-1"};
    request.tool    = std::move(tool);
    request.root    = root;
    request.sandbox = sandbox;
    request.force_ask = force_ask;
    request.arguments = nlohmann::json::object();
    if (!command.empty()) {
        request.arguments["command"] = std::move(command);
    }
    return request;
}

Config master_allow_config() {
    Config config;
    config.permissions.default_verdict = "allow";
    return config;
}

void apply_document(const std::filesystem::path& path, const std::string& body, Config& config) {
    std::ofstream(path, std::ios::binary) << body;
    apply_jsonc_file(config, path, /*required=*/true);
}

PermissionRuleSettings config_rule(std::optional<std::string> tool,
                                   std::optional<std::string> command,
                                   std::string effect,
                                   std::string id) {
    PermissionRuleSettings rule;
    rule.tool    = std::move(tool);
    rule.command = std::move(command);
    rule.effect  = std::move(effect);
    rule.id      = std::move(id);
    return rule;
}

class FakeGrantStore final : public GrantStore {
public:
    std::vector<PolicyRule> initial;
    std::vector<PolicyRule> appended;
    bool                    append_result = true;
    int                     load_calls    = 0;
    std::function<void()>   before_append;

    std::vector<PolicyRule> load() override {
        ++load_calls;
        return initial;
    }

    bool append(const PolicyRule& grant) override {
        if (before_append) {
            before_append();
        }
        appended.push_back(grant);
        return append_result;
    }
};

PolicyRule local_grant(std::string tool, std::string command = std::string()) {
    PolicyRule rule;
    rule.tool    = std::move(tool);
    rule.command = std::move(command);
    rule.effect  = PolicyVerdict::Allow;
    rule.layer   = PolicyRule::Layer::LocalGrant;
    rule.literal = true;
    rule.id      = "grant:" + rule.tool;
    return rule;
}

std::size_t count_layer(const std::vector<PolicyRule>& rules, PolicyRule::Layer layer) {
    std::size_t count = 0;
    for (const PolicyRule& rule : rules) {
        if (rule.layer == layer) {
            ++count;
        }
    }
    return count;
}

// ── D1 ─────────────────────────────────────────────────────────────────────

TEST(Errata46D1, UI46_D1_DefaultAllowNoPrompts) {
    RulePermissionPolicy policy(to_permission_config(master_allow_config()));
    test::TempWorkspace  workspace("d1_no_prompts");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file")), PolicyVerdict::Allow);
}

TEST(Errata46D1, UI46_D1_DefaultAllowShellGitMcp) {
    RulePermissionPolicy policy(to_permission_config(master_allow_config()));
    test::TempWorkspace  workspace("d1_all_classes");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "rm -rf /")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "git_commit")), PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "mcp.github.create_issue")),
              PolicyVerdict::Allow);
}

TEST(Errata46D1, UI46_D1_DefaultAllowCannotBypassReadOnly) {
    RulePermissionPolicy policy(to_permission_config(master_allow_config()));
    test::TempWorkspace  workspace("d1_readonly");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file", "", false,
                                          SandboxMode::ReadOnly)),
              PolicyVerdict::Deny);
}

TEST(Errata46D1, UI46_D1_DefaultAllowCannotBypassForceAsk) {
    RulePermissionPolicy policy(to_permission_config(master_allow_config()));
    test::TempWorkspace  workspace("d1_force_ask");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "ls", true)),
              PolicyVerdict::Ask);
}

TEST(Errata46D1, UI46_D1_DefaultAllowPathToolsNoPrompts) {
    RulePermissionPolicy policy(to_permission_config(master_allow_config()));
    test::TempWorkspace  workspace("d1_path_tools");
    for (const char* tool : {"read_file", "grep", "glob", "write_file", "edit_file"}) {
        EXPECT_EQ(policy.evaluate(request_for(workspace.path(), tool)), PolicyVerdict::Allow)
            << tool;
    }
}

TEST(Errata46D1, UI46_D1_RulesOutrankBuiltins) {
    Config config;
    config.permissions.write = "allow";
    config.permissions.rules.push_back(
        config_rule("write_file", std::nullopt, "deny", "config.rule.0"));
    RulePermissionPolicy policy(to_permission_config(config));
    test::TempWorkspace  workspace("d1_rules_outrank");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file")), PolicyVerdict::Deny);
}

TEST(Errata46D1, UI46_D1_CommandOnlyRuleRequiresTool) {
    test::TempWorkspace workspace("d1_command_only");
    Config              config;
    EXPECT_THROW(apply_document(workspace.path() / "c.jsonc",
                                R"JSONC({"permissions":{"rules":[{"command":"ls","effect":"allow"}]}})JSONC",
                                config),
                 ConfigError);
}

TEST(Errata46D1, UI46_D1_PathKeyRejected) {
    test::TempWorkspace workspace("d1_path_key");
    Config              config;
    EXPECT_THROW(apply_document(
                     workspace.path() / "c.jsonc",
                     R"JSONC({"permissions":{"rules":[{"tool":"write_file","path":"src/**","effect":"allow"}]}})JSONC",
                     config),
                 ConfigError);
}

TEST(Errata46D1, UI46_D1_CommandDenyBeatsBuiltinAllow) {
    Config config;
    config.permissions.shell = "allow";
    config.permissions.rules.push_back(
        config_rule("shell", "rm *", "deny", "config.rule.0"));
    RulePermissionPolicy policy(to_permission_config(config));
    test::TempWorkspace  workspace("d1_command_deny");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "rm -rf build")),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "ls -la")),
              PolicyVerdict::Allow);
}

TEST(Errata46D1, UI46_D1_MasterSwitchBeatsShellDeny) {
    Config config = master_allow_config();
    config.permissions.shell = "deny";
    RulePermissionPolicy policy(to_permission_config(config));
    test::TempWorkspace  workspace("d1_master_beats_shell");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "ls")),
              PolicyVerdict::Allow);
}

TEST(Errata46D1, UI46_D1_MalformedRuleIsConfigError) {
    test::TempWorkspace workspace("d1_malformed_rule");
    Config              config;
    EXPECT_THROW(apply_document(
                     workspace.path() / "c.jsonc",
                     R"JSONC({"permissions":{"rules":[{"tool":"shell","command":"[unclosed","effect":"allow"}]}})JSONC",
                     config),
                 ConfigError);
}

TEST(Errata46D1, UI46_D1_UnknownPermissionKeyIsConfigError) {
    test::TempWorkspace workspace("d1_unknown_key");
    Config              config;
    EXPECT_THROW(apply_document(workspace.path() / "c.jsonc",
                                R"JSONC({"permissions":{"bogus":1}})JSONC", config),
                 ConfigError);
}

// ── D2 ─────────────────────────────────────────────────────────────────────

TEST(Errata46D2, UI46_D2_AlwaysPersistsAcrossRestart) {
    test::TempWorkspace         workspace("d2_restart");
    const std::filesystem::path grants = workspace.path() / ".ymh" / "permissions.jsonc";
    std::filesystem::create_directories(grants.parent_path());
    {
        std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
        RulePermissionPolicy        policy(PermissionConfig{}, store.get());
        policy.remember(request_for(workspace.path(), "shell", "git push origin main"),
                        payload::PermissionDecisionKind::Allow, GrantScope::Always);
    }
    {
        std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
        RulePermissionPolicy        policy(PermissionConfig{}, store.get());
        EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "git push origin main")),
                  PolicyVerdict::Allow);
        EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "rm -rf /")),
                  PolicyVerdict::Ask);
    }
}

TEST(Errata46D2, UI46_D2_AlwaysPersistsThroughGate) {
    test::TempWorkspace         workspace("d2_gate");
    const std::filesystem::path grants = workspace.path() / ".ymh" / "permissions.jsonc";
    std::filesystem::create_directories(grants.parent_path());
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    RulePermissionPolicy        policy(PermissionConfig{}, store.get());
    PermissionGate              gate(policy, PermissionConfig{});

    std::optional<PermissionRequestId> pending;
    gate.set_attention_hook(
        [&pending](const PermissionRequestId& id, const PermissionRequest&) { pending = id; });

    PermissionOutcome outcome;
    std::thread       resolver([&] {
        outcome = gate.resolve(request_for(workspace.path(), "shell", "echo hi"), {});
    });
    while (!pending.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(gate.decide(*pending, payload::PermissionDecisionKind::AllowAlways,
                            GrantScope::Always, "approved"));
    resolver.join();

    std::unique_ptr<GrantStore> reopened = open_file_grant_store(grants, "ws-1");
    RulePermissionPolicy        reloaded(PermissionConfig{}, reopened.get());
    EXPECT_EQ(reloaded.evaluate(request_for(workspace.path(), "shell", "echo hi")),
              PolicyVerdict::Allow);
}

TEST(Errata46D2, UI46_D2_OnceNotPersisted) {
    FakeGrantStore       store;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_once");
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Once);
    EXPECT_TRUE(store.appended.empty());
}

TEST(Errata46D2, UI46_D2_SessionNotPersisted) {
    FakeGrantStore       store;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_session");
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Session);
    EXPECT_TRUE(store.appended.empty());
}

TEST(Errata46D2, UI46_D2_ForceAskNotPersisted) {
    FakeGrantStore      store;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_force_ask");
    policy.remember(request_for(workspace.path(), "shell", "ls", /*force_ask=*/true),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    EXPECT_TRUE(store.appended.empty());
}

TEST(Errata46D2, UI46_D2_MalformedGrantsFileReasks) {
    test::TempWorkspace workspace("d2_malformed");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::ofstream(grants, std::ios::binary) << "not json";
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    EXPECT_TRUE(store->load().empty());
}

TEST(Errata46D2, UI46_D2_MissingMarkerIgnored) {
    test::TempWorkspace workspace("d2_marker");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::ofstream(grants, std::ios::binary)
        << R"JSON({"workspace_id":"ws-1","grants":[{"tool":"shell","command":"ls"}]})JSON";
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    EXPECT_TRUE(store->load().empty());
}

TEST(Errata46D2, UI46_D2_ForeignWorkspaceIgnored) {
    test::TempWorkspace workspace("d2_foreign");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::ofstream(grants, std::ios::binary)
        << R"JSON({"schema":"ymh.permissions/1","workspace_id":"other","grants":[{"tool":"shell","command":"ls"}]})JSON";
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    EXPECT_TRUE(store->load().empty());
}

TEST(Errata46D2, UI46_D2_AppendFailureDegradesToSession) {
    FakeGrantStore      store;
    store.append_result = false;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_append_fail");
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "ls")), PolicyVerdict::Allow);
    EXPECT_EQ(count_layer(policy.rules(), PolicyRule::Layer::LocalGrant), 0u);
    EXPECT_EQ(count_layer(policy.rules(), PolicyRule::Layer::SessionGrant), 1u);
}

TEST(Errata46D2, UI46_D2_LockContentionDegradesToSession) {
    test::TempWorkspace         workspace("d2_lock");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::ofstream(grants, std::ios::binary) << "";

    const int lock_fd = ::open(grants.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(lock_fd, 0);
    ASSERT_EQ(::flock(lock_fd, LOCK_EX), 0);

    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    RulePermissionPolicy        policy(PermissionConfig{}, store.get());
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);

    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "ls")), PolicyVerdict::Allow);
    EXPECT_EQ(count_layer(policy.rules(), PolicyRule::Layer::LocalGrant), 0u);
    EXPECT_EQ(count_layer(policy.rules(), PolicyRule::Layer::SessionGrant), 1u);

    (void)::flock(lock_fd, LOCK_UN);
    ::close(lock_fd);
}

TEST(Errata46D2, UI46_D2_AtomicReplaceSurvivesKill) {
    test::TempWorkspace         workspace("d2_atomic");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::filesystem::create_directories(grants.parent_path());
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    RulePermissionPolicy        policy(PermissionConfig{}, store.get());
    policy.remember(request_for(workspace.path(), "shell", "first"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    policy.remember(request_for(workspace.path(), "shell", "second"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);

    std::unique_ptr<GrantStore> reopened = open_file_grant_store(grants, "ws-1");
    const std::vector<PolicyRule> rules  = reopened->load();
    ASSERT_EQ(rules.size(), 2u);
    std::size_t temp_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(grants.parent_path())) {
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
            ++temp_files;
        }
    }
    EXPECT_EQ(temp_files, 0u);
}

TEST(Errata46D2, UI46_D2_DeleteFileDropsGrants) {
    test::TempWorkspace         workspace("d2_delete");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::filesystem::create_directories(grants.parent_path());
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    RulePermissionPolicy        policy(PermissionConfig{}, store.get());
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    ASSERT_EQ(store->load().size(), 1u);

    std::filesystem::remove(grants);
    std::unique_ptr<GrantStore> reopened = open_file_grant_store(grants, "ws-1");
    EXPECT_TRUE(reopened->load().empty());
}

TEST(Errata46D2, UI46_D2_GrantIsWorkspaceScoped) {
    test::TempWorkspace         workspace("d2_scoped");
    const std::filesystem::path first  = workspace.path() / "a" / "permissions.jsonc";
    const std::filesystem::path second = workspace.path() / "b" / "permissions.jsonc";
    std::filesystem::create_directories(first.parent_path());
    std::filesystem::create_directories(second.parent_path());
    std::unique_ptr<GrantStore> store = open_file_grant_store(first, "ws-1");
    RulePermissionPolicy        policy(PermissionConfig{}, store.get());
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);

    std::unique_ptr<GrantStore> other = open_file_grant_store(second, "ws-2");
    EXPECT_TRUE(other->load().empty());
}

TEST(Errata46D2, UI46_D2_GrantIdIsContentDerived) {
    FakeGrantStore      store;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_ids");
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    policy.remember(request_for(workspace.path(), "shell", "ls"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    policy.remember(request_for(workspace.path(), "shell", "pwd"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);
    ASSERT_EQ(store.appended.size(), 3u);
    EXPECT_EQ(store.appended[0].id, store.appended[1].id);
    EXPECT_NE(store.appended[0].id, store.appended[2].id);
}

TEST(Errata46D2, UI46_D2_GrantCommandIsLiteral) {
    FakeGrantStore      store;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_literal");
    policy.remember(request_for(workspace.path(), "shell", "rm -rf build/*"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);

    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "rm -rf build/*")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "rm -rf build/../../etc")),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "rm -rf build/foo")),
              PolicyVerdict::Ask);
}

TEST(Errata46D2, UI46_D2_PathToolGrantIsToolWide) {
    FakeGrantStore      store;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_tool_wide");
    policy.remember(request_for(workspace.path(), "write_file"),
                    payload::PermissionDecisionKind::Allow, GrantScope::Always);

    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file")), PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file")), PolicyVerdict::Allow);
}

TEST(Errata46D2, UI46_D2_StoreInjectedBeforeRuntime) {
    std::ifstream input{std::filesystem::path{YMH_SOURCE_DIR} / "src" / "host" /
                            "workspace_host.cpp",
                        std::ios::binary};
    ASSERT_TRUE(input);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const std::size_t registry = source.find("registry_ = WorkspaceRegistry::open");
    const std::size_t store    = source.find("grant_store_ = open_file_grant_store");
    const std::size_t runtime  = source.find("WorkspaceRuntime::create");
    ASSERT_NE(registry, std::string::npos);
    ASSERT_NE(store, std::string::npos);
    ASSERT_NE(runtime, std::string::npos);
    EXPECT_LT(registry, runtime);
    EXPECT_LT(store, runtime);
}

TEST(Errata46D2, UI46_D2_LoadsViaConstructor) {
    FakeGrantStore      store;
    store.initial.push_back(local_grant("shell", "ls"));
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_load");
    EXPECT_EQ(store.load_calls, 1);
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "shell", "ls")), PolicyVerdict::Allow);
}

TEST(Errata46D2, UI46_D2_AppendOutsideMutex) {
    FakeGrantStore      store;
    std::mutex          mutex;
    std::condition_variable cv;
    bool                append_started = false;
    bool                release_append = false;
    store.before_append = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        append_started = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_append; });
    };

    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_outside_mutex");

    std::thread remember_thread([&] {
        policy.remember(request_for(workspace.path(), "shell", "ls"),
                        payload::PermissionDecisionKind::Allow, GrantScope::Always);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds{2}, [&] { return append_started; }));
    }

    const auto start = std::chrono::steady_clock::now();
    (void)policy.evaluate(request_for(workspace.path(), "shell", "ls"));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_append = true;
    }
    cv.notify_all();
    remember_thread.join();
}

TEST(Errata46D2, UI46_D2_ForceAskGuardInsideRemember) {
    FakeGrantStore      store;
    RulePermissionPolicy policy(PermissionConfig{}, &store);
    test::TempWorkspace  workspace("d2_guard_inside");
    const PolicyRule     returned =
        policy.remember(request_for(workspace.path(), "shell", "ls", /*force_ask=*/true),
                        payload::PermissionDecisionKind::Allow, GrantScope::Always);
    EXPECT_EQ(returned.effect, PolicyVerdict::Ask);
    EXPECT_TRUE(store.appended.empty());
}

TEST(Errata46D2, UI46_D2_ForceAskNotPersistedAtGate) {
    test::TempWorkspace         workspace("d2_gate_force_ask");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::filesystem::create_directories(grants.parent_path());
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    RulePermissionPolicy        policy(PermissionConfig{}, store.get());
    PermissionGate              gate(policy, PermissionConfig{});

    std::optional<PermissionRequestId> pending;
    gate.set_attention_hook(
        [&pending](const PermissionRequestId& id, const PermissionRequest&) { pending = id; });

    std::thread resolver([&] {
        (void)gate.resolve(request_for(workspace.path(), "shell", "ls", /*force_ask=*/true), {});
    });
    while (!pending.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(gate.decide(*pending, payload::PermissionDecisionKind::AllowAlways,
                            GrantScope::Always, "approved"));
    resolver.join();

    EXPECT_FALSE(std::filesystem::exists(grants));
}

TEST(Errata46D2, UI46_D2_PathKeyInGrantRejected) {
    test::TempWorkspace workspace("d2_grant_path");
    const std::filesystem::path grants = workspace.path() / "permissions.jsonc";
    std::ofstream(grants, std::ios::binary)
        << R"JSON({"schema":"ymh.permissions/1","workspace_id":"ws-1","grants":[{"tool":"write_file","path":"src/**","effect":"allow"}]})JSON";
    std::unique_ptr<GrantStore> store = open_file_grant_store(grants, "ws-1");
    EXPECT_TRUE(store->load().empty());
}

TEST(Errata46D2, UI46_D2_ConfigDenyOverridesDurableGrant) {
    FakeGrantStore store;
    store.initial.push_back(local_grant("write_file"));

    PermissionConfig config;
    PolicyRule       deny;
    deny.tool   = "write_file";
    deny.effect = PolicyVerdict::Deny;
    deny.layer  = PolicyRule::Layer::Project;
    deny.id     = "config.rule.0";
    config.rules.push_back(deny);

    RulePermissionPolicy policy(config, &store);
    test::TempWorkspace  workspace("d2_config_deny");
    EXPECT_EQ(policy.evaluate(request_for(workspace.path(), "write_file")), PolicyVerdict::Deny);

    // A session grant is intentionally out of scope of the veto.
    FakeGrantStore session_store;
    RulePermissionPolicy session_policy(config, &session_store);
    session_policy.remember(request_for(workspace.path(), "write_file"),
                            payload::PermissionDecisionKind::Allow, GrantScope::Session);
    EXPECT_EQ(session_policy.evaluate(request_for(workspace.path(), "write_file")),
              PolicyVerdict::Allow);
}

} // namespace
