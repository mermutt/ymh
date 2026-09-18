#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "support/host_harness.hpp"
#include "support/test_env.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/cli/headless.hpp"
#include "ymh/cli/session_cli.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session_persistence.hpp"

namespace {

using namespace ymh;

FakeScript single_text(std::string text) {
    FakeScript script;
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    script.steps.push_back(std::move(step));
    return script;
}

PersistenceConfig cli_persistence_config(const test::TempWorkspace& workspace) {
    PersistenceConfig config;
    config.db_path   = workspace.path() / ".ymh" / "sessions.db";
    config.lock_path = workspace.path() / ".ymh" / "sessions.lock";
    config.boot_id   = BootId{"session-cli-test"};
    return config;
}

SessionId create_unprompted_session(const test::TempWorkspace& workspace) {
    const std::unique_ptr<SessionPersistence> store =
        SessionPersistence::open(cli_persistence_config(workspace));
    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = std::filesystem::canonical(workspace.path());
    header.createdAt     = 1;
    header.updatedAt     = 1;
    header.title         = "unprompted";
    header.model         = "test-model";
    header.serverProfile = "interactive";
    header.kind          = SessionKind::Root;
    return store->create(std::move(header)).id;
}

class SessionCliTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggingOptions options;
        options.level = LogLevel::Error;
        init_logging(options);
    }

    HeadlessResult createSession(const test::TempWorkspace& workspace) {
        std::ostringstream out;
        std::ostringstream err;
        HeadlessOptions    options;
        options.workspace = workspace.path();
        options.task      = "create a session";
        options.out       = &out;
        options.err       = &err;
        options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            return std::make_unique<FakeLLM>(single_text("hello from agent"));
        };
        return run_headless(options);
    }
};

TEST_F(SessionCliTest, ListEmptyWorkspace) {
    test::TempWorkspace workspace("session_cli_empty");
    std::ostringstream  out;
    std::ostringstream  err;
    EXPECT_EQ(session_list(workspace.path(), out, err), 0);
    EXPECT_NE(out.str().find("no sessions"), std::string::npos);
}

TEST_F(SessionCliTest, ListShowReplayFork) {
    test::TempWorkspace workspace("session_cli");
    const HeadlessResult created = createSession(workspace);
    ASSERT_EQ(created.exit_code, 0);

    std::ostringstream list_out;
    std::ostringstream list_err;
    EXPECT_EQ(session_list(workspace.path(), list_out, list_err), 0);
    EXPECT_NE(list_out.str().find(created.session.value), std::string::npos);

    std::ostringstream show_out;
    std::ostringstream show_err;
    EXPECT_EQ(session_show(workspace.path(), created.session.value, show_out, show_err), 0);
    EXPECT_NE(show_out.str().find("hello from agent"), std::string::npos);

    std::ostringstream replay_out;
    std::ostringstream replay_err;
    EXPECT_EQ(session_replay(workspace.path(), created.session.value, replay_out, replay_err), 0);
    EXPECT_NE(replay_out.str().find("turn/end"), std::string::npos);

    std::ostringstream fork_out;
    std::ostringstream fork_err;
    EXPECT_EQ(session_fork(workspace.path(), created.session.value, fork_out, fork_err), 0);
    std::string child = fork_out.str();
    const std::size_t newline = child.find('\n');
    if (newline != std::string::npos) {
        child.resize(newline);
    }
    EXPECT_FALSE(child.empty());

    std::ostringstream list_after;
    EXPECT_EQ(session_list(workspace.path(), list_after, list_err), 0);
    EXPECT_NE(list_after.str().find(child), std::string::npos);
}

TEST_F(SessionCliTest, SL_I2_ListHidesUnpromptedRootWhileShowStillPrintsIt) {
    test::TempWorkspace  workspace("session_cli_hidden");
    const SessionId      unprompted = create_unprompted_session(workspace);

    std::ostringstream list_out;
    std::ostringstream list_err;
    EXPECT_EQ(session_list(workspace.path(), list_out, list_err), 0);
    EXPECT_EQ(list_out.str().find(unprompted.value), std::string::npos);
    EXPECT_NE(list_out.str().find("no sessions"), std::string::npos);

    std::ostringstream show_out;
    std::ostringstream show_err;
    EXPECT_EQ(session_show(workspace.path(), unprompted.value, show_out, show_err), 0);
    EXPECT_NE(show_out.str().find("unprompted"), std::string::npos);
}

TEST_F(SessionCliTest, SL_I2_ListKeepsPromptedAndHidesUnprompted) {
    test::TempWorkspace workspace("session_cli_mixed");
    const HeadlessResult created = createSession(workspace);
    ASSERT_EQ(created.exit_code, 0);
    const SessionId unprompted = create_unprompted_session(workspace);

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(session_list(workspace.path(), out, err), 0);
    EXPECT_NE(out.str().find(created.session.value), std::string::npos);
    EXPECT_EQ(out.str().find(unprompted.value), std::string::npos);
}

TEST_F(SessionCliTest, UnknownSessionRejected) {
    test::TempWorkspace workspace("session_cli_unknown");
    (void)createSession(workspace);

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(session_show(workspace.path(), "does-not-exist", out, err), 2);
    EXPECT_NE(err.str().find("unknown session"), std::string::npos);
}

class SilentHostSocket {
public:
    explicit SilentHostSocket(std::filesystem::path path) : path_(std::move(path)) {
        std::filesystem::create_directories(path_.parent_path());
        std::filesystem::remove(path_);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            throw std::runtime_error("silent socket: socket() failed");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path_.c_str());
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd_, 4) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("silent socket: bind/listen failed");
        }
    }

    ~SilentHostSocket() { close(); }

    SilentHostSocket(const SilentHostSocket&) = delete;
    SilentHostSocket& operator=(const SilentHostSocket&) = delete;

    void accept_and_close() {
        const int client = ::accept(fd_, nullptr, nullptr);
        if (client >= 0) {
            ::close(client);
        }
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

private:
    std::filesystem::path path_;
    int                   fd_ = -1;
};

class SessionPruneTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* previous = std::getenv("XDG_STATE_HOME");
        had_state_ = previous != nullptr;
        previous_state_ = previous != nullptr ? previous : "";
        state_ = test::make_temp_dir("session_prune_state");
        ASSERT_EQ(::setenv("XDG_STATE_HOME", state_.c_str(), 1), 0);

        workspace_ = test::make_temp_dir("session_prune_ws");
        registry_  = WorkspaceRegistry::open(default_registry_config());
        record_    = registry_->registerWorkspace(std::filesystem::canonical(workspace_),
                                                  "prune-test");
    }

    void TearDown() override {
        registry_.reset();
        if (had_state_) {
            ::setenv("XDG_STATE_HOME", previous_state_.c_str(), 1);
        } else {
            ::unsetenv("XDG_STATE_HOME");
        }
        std::error_code error;
        std::filesystem::remove_all(state_, error);
        std::filesystem::remove_all(workspace_, error);
    }

    PersistenceConfig persistence() const {
        PersistenceConfig config;
        config.db_path   = workspace_ / ".ymh" / "sessions.db";
        config.lock_path = workspace_ / ".ymh" / "sessions.lock";
        config.boot_id   = BootId{"prune-test"};
        return config;
    }

    SessionId create_session(SessionKind kind, bool prompted,
                             std::optional<SessionId> parent = std::nullopt,
                             std::size_t seed = 0, std::int64_t updated_at = 1) {
        const std::unique_ptr<SessionPersistence> store =
            SessionPersistence::open(persistence());
        SessionHeader header;
        header.id            = make_session_id();
        header.cwd           = std::filesystem::canonical(workspace_);
        header.createdAt     = updated_at;
        header.updatedAt     = updated_at;
        header.model         = "test-model";
        header.serverProfile = "interactive";
        header.kind          = kind;
        header.parentSession = parent;
        if (kind == SessionKind::Fork) {
            header.seedLength = seed;
        }
        const SessionId id = store->create(std::move(header)).id;

        TypedEvent<payload::SessionStarted> started;
        started.id         = make_event_id();
        started.session_id = id;
        started.timestamp =
            std::chrono::system_clock::time_point{std::chrono::milliseconds{updated_at}};
        started.payload = payload::SessionStarted{"test-model", "interactive", "t"};
        store->append(id, encode(started));

        if (prompted) {
            TypedEvent<payload::UserMessage> message;
            message.id         = make_event_id();
            message.session_id = id;
            message.timestamp =
                std::chrono::system_clock::time_point{std::chrono::milliseconds{updated_at}};
            ContentBlock block;
            block.kind = ContentBlockKind::Text;
            block.text = "hi";
            message.payload = payload::UserMessage{MessageId{"m-" + id.value}, {block}};
            store->append(id, encode(message));
        }
        registry_->addSession(record_.id, id);
        return id;
    }

    struct Counts {
        bool        row      = false;
        bool        lease    = false;
        bool        junction = false;
        std::size_t events   = 0;
    };

    Counts counts(const SessionId& id) const {
        const std::unique_ptr<SessionPersistence> store =
            SessionPersistence::openReadOnly(persistence());
        Counts result;
        result.row      = store->load(id).has_value();
        result.lease    = store->leaseState(id) != LeaseState::Absent;
        result.events   = result.row ? store->read(id).size() : 0;
        result.junction = registry_->findSession(record_.id, id).has_value();
        return result;
    }

    void expect_pruned(const SessionId& id) const {
        const Counts after = counts(id);
        EXPECT_FALSE(after.row);
        EXPECT_FALSE(after.lease);
        EXPECT_FALSE(after.junction);
        EXPECT_EQ(after.events, 0u);
    }

    void expect_intact(const SessionId& id) const {
        const Counts after = counts(id);
        EXPECT_TRUE(after.row);
        EXPECT_TRUE(after.lease);
        EXPECT_TRUE(after.junction);
        EXPECT_GT(after.events, 0u);
    }

    std::filesystem::path      state_;
    std::filesystem::path      workspace_;
    std::unique_ptr<WorkspaceRegistry> registry_;
    WorkspaceRecord            record_;
    bool                       had_state_ = false;
    std::string                previous_state_;
};

TEST_F(SessionPruneTest, SL_I5_DryRunSelectsAndMutatesNothing) {
    const SessionId empty    = create_session(SessionKind::Root, false, std::nullopt, 0, 5);
    const SessionId prompted = create_session(SessionKind::Root, true, std::nullopt, 0, 6);
    std::filesystem::remove(workspace_ / ".ymh" / "sessions.lock");

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    EXPECT_EQ(session_prune(options, out, err), 0);
    EXPECT_NE(out.str().find("would prune 1"), std::string::npos);
    EXPECT_NE(out.str().find(empty.value), std::string::npos);

    expect_intact(empty);
    expect_intact(prompted);
    EXPECT_FALSE(std::filesystem::exists(workspace_ / ".ymh" / "sessions.lock"));
}

TEST_F(SessionPruneTest, SL_I6_ApplyPrunesOnlyUnpromptedRoots) {
    const SessionId empty    = create_session(SessionKind::Root, false, std::nullopt, 0, 5);
    const SessionId prompted = create_session(SessionKind::Root, true, std::nullopt, 0, 6);
    const SessionId fork     = create_session(SessionKind::Fork, false, prompted, 1, 7);
    const SessionId subagent = create_session(SessionKind::Subagent, false, prompted, 0, 8);

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    options.yes       = true;
    EXPECT_EQ(session_prune(options, out, err), 0);
    EXPECT_NE(out.str().find("pruned 1"), std::string::npos);

    expect_pruned(empty);
    expect_intact(prompted);
    expect_intact(fork);
    expect_intact(subagent);
}

TEST_F(SessionPruneTest, SL_I7_ApplyRemovesJunctionSoWorkspaceBecomesRemovable) {
    const SessionId empty = create_session(SessionKind::Root, false);

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    options.yes       = true;
    EXPECT_EQ(session_prune(options, out, err), 0);
    expect_pruned(empty);
    EXPECT_FALSE(registry_->findSession(record_.id, empty).has_value());
    EXPECT_NO_THROW(registry_->removeWorkspace(record_.id));
}

TEST_F(SessionPruneTest, SL_I8_DiskEnumerationSelectsJunctionHiddenUnpromptedRoot) {
    const SessionId hidden = create_session(SessionKind::Root, false);
    ASSERT_TRUE(registry_->findSession(record_.id, hidden).has_value());

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    EXPECT_EQ(session_prune(options, out, err), 0);
    EXPECT_NE(out.str().find(hidden.value), std::string::npos);
}

TEST_F(SessionPruneTest, SL_I10_KeepExcludesMostRecentAndKeepAloneIsUsageError) {
    const SessionId oldest = create_session(SessionKind::Root, false, std::nullopt, 0, 10);
    const SessionId middle = create_session(SessionKind::Root, false, std::nullopt, 0, 20);
    const SessionId newest = create_session(SessionKind::Root, false, std::nullopt, 0, 30);

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    options.yes       = true;
    options.keep      = 1;
    EXPECT_EQ(session_prune(options, out, err), 0);
    expect_pruned(oldest);
    expect_pruned(middle);
    expect_intact(newest);

    std::ostringstream keep_only_err;
    PruneOptions       keep_only;
    keep_only.keep      = 2;
    keep_only.workspace = workspace_;
    keep_only.yes       = true;
    EXPECT_EQ(session_prune(keep_only, out, keep_only_err), 2);
    EXPECT_NE(keep_only_err.str().find("pass --empty"), std::string::npos);
}

TEST_F(SessionPruneTest, SL_I10_KeepZeroExcludesNothing) {
    const SessionId first  = create_session(SessionKind::Root, false, std::nullopt, 0, 10);
    const SessionId second = create_session(SessionKind::Root, false, std::nullopt, 0, 20);

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    options.yes       = true;
    options.keep      = 0;
    EXPECT_EQ(session_prune(options, out, err), 0);
    expect_pruned(first);
    expect_pruned(second);
}

TEST_F(SessionPruneTest, SL_I11_NoInclusionFlagIsUsageError) {
    const SessionId empty = create_session(SessionKind::Root, false);
    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.workspace = workspace_;
    options.yes       = true;
    EXPECT_EQ(session_prune(options, out, err), 2);
    expect_intact(empty);
}

TEST_F(SessionPruneTest, SL_I11_OlderThanIsRejectedByCli) {
    const SessionId empty = create_session(SessionKind::Root, false);
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"session", "prune", "--empty", "--older-than", "5", "--workspace",
                       workspace_.string()},
                      out, err),
              2);
    expect_intact(empty);
}

TEST_F(SessionPruneTest, SL_I12_UnregisteredWorkspaceIsUsageError) {
    const test::TempWorkspace other("session_prune_unregistered");
    std::ostringstream        out;
    std::ostringstream        err;
    PruneOptions              options;
    options.empty     = true;
    options.workspace = other.path();
    options.yes       = true;
    EXPECT_EQ(session_prune(options, out, err), 2);
    EXPECT_NE(err.str().find("unregistered workspace"), std::string::npos);
}

TEST_F(SessionPruneTest, SL_I13_DependentParentSkippedBeforeJunctionRemoval) {
    const SessionId parent = create_session(SessionKind::Root, false, std::nullopt, 0, 1);
    const SessionId child  = create_session(SessionKind::Fork, false, parent, 1, 2);

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    options.yes       = true;
    EXPECT_EQ(session_prune(options, out, err), 0);
    EXPECT_NE(out.str().find("has dependent sessions"), std::string::npos);
    expect_intact(parent);
    expect_intact(child);
}

TEST_F(SessionPruneTest, SL_I16_JsonOutputIsASingleArray) {
    (void)create_session(SessionKind::Root, false);
    (void)create_session(SessionKind::Root, true);

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    options.json      = true;
    EXPECT_EQ(session_prune(options, out, err), 0);
    const nlohmann::json parsed = nlohmann::json::parse(out.str());
    ASSERT_TRUE(parsed.is_array());
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].at("status").get<std::string>(), "would prune");
}

TEST_F(SessionPruneTest, SL_I17_PruneRunsWithoutGlobalConfig) {
    const std::filesystem::path config_home = test::make_temp_dir("session_prune_config");
    const char*                 previous    = std::getenv("XDG_CONFIG_HOME");
    const bool                  had_config  = previous != nullptr;
    const std::string           previous_config = previous != nullptr ? previous : "";
    ASSERT_EQ(::setenv("XDG_CONFIG_HOME", config_home.c_str(), 1), 0);

    (void)create_session(SessionKind::Root, false);
    std::ostringstream out;
    std::ostringstream err;
    const int          status = run_cli(
        {"session", "prune", "--empty", "--workspace", workspace_.string()}, out, err);

    if (had_config) {
        ::setenv("XDG_CONFIG_HOME", previous_config.c_str(), 1);
    } else {
        ::unsetenv("XDG_CONFIG_HOME");
    }
    std::error_code error;
    std::filesystem::remove_all(config_home, error);
    EXPECT_EQ(status, 0);
    EXPECT_NE(out.str().find("would prune 1"), std::string::npos);
}

TEST_F(SessionPruneTest, SL_I19_StoppedApplySweepsStoreAbsentJunction) {
    const SessionId empty = create_session(SessionKind::Root, false);
    const SessionId orphan{generate_uuid_v4()};
    registry_->addSession(record_.id, orphan);
    ASSERT_TRUE(registry_->findSession(record_.id, orphan).has_value());

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    options.yes       = true;
    EXPECT_EQ(session_prune(options, out, err), 0);
    expect_pruned(empty);
    EXPECT_FALSE(registry_->findSession(record_.id, orphan).has_value());
}

TEST_F(SessionPruneTest, SL_I21_DryRunWritesNoLock) {
    (void)create_session(SessionKind::Root, false);
    std::filesystem::remove(workspace_ / ".ymh" / "sessions.lock");

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.workspace = workspace_;
    EXPECT_EQ(session_prune(options, out, err), 0);
    EXPECT_FALSE(std::filesystem::exists(workspace_ / ".ymh" / "sessions.lock"));
}

TEST_F(SessionPruneTest, SL_I9_LiveApplyDeletesViaDaemonAndActiveIsSkippedUnlessForced) {
    const SessionId active   = create_session(SessionKind::Root, false, std::nullopt, 0, 5);
    const SessionId inactive = create_session(SessionKind::Root, false, std::nullopt, 0, 6);
    registry_.reset();

    test::HostHarnessOptions harness_options;
    harness_options.workspace_root        = workspace_;
    harness_options.workspace_id          = record_.id.value;
    harness_options.socket_path           = workspace_ / ".ymh" / "host.sock";
    harness_options.log_sink              = workspace_ / ".ymh" / "host.log";
    harness_options.env["XDG_STATE_HOME"] = state_.string();
    test::HostHarness harness(std::move(harness_options));
    harness.start();
    ASSERT_TRUE(harness.wait_ready(std::chrono::milliseconds{20000})) << harness.read_log();
    ASSERT_TRUE(harness.connect().isConnected());

    [[maybe_unused]] const nlohmann::json activated = harness.connect().request(
        protocol::method::kSessionActivate, {{"session", active.value}});

    registry_ = WorkspaceRegistry::openReadOnly(default_registry_config());

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.yes       = true;
    options.workspace = workspace_;

    EXPECT_EQ(session_prune(options, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("skipped"), std::string::npos) << out.str();
    expect_pruned(inactive);
    expect_intact(active);

    options.force = true;
    std::ostringstream out_forced;
    std::ostringstream err_forced;
    EXPECT_EQ(session_prune(options, out_forced, err_forced), 0) << err_forced.str();
    expect_pruned(active);

    harness.stop();
}

TEST_F(SessionPruneTest, SL_I18_RetryAfterJunctionRemovedCompletesEraseAndWorkspaceRemoval) {
    const SessionId id = create_session(SessionKind::Root, false);
    registry_->removeSession(record_.id, id);
    ASSERT_FALSE(registry_->findSession(record_.id, id).has_value());

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.yes       = true;
    options.workspace = workspace_;
    EXPECT_EQ(session_prune(options, out, err), 0) << err.str();
    expect_pruned(id);
    EXPECT_NO_THROW(registry_->removeWorkspace(record_.id));
}

TEST_F(SessionPruneTest, SL_I20_DaemonStartupWithFreshStoreSweepsNoJunctions) {
    const SessionId junction_only = SessionId{generate_uuid_v4()};
    registry_->addSession(record_.id, junction_only);
    ASSERT_TRUE(registry_->findSession(record_.id, junction_only).has_value());

    std::error_code error;
    std::filesystem::remove_all(workspace_ / ".ymh", error);
    ASSERT_FALSE(std::filesystem::exists(workspace_ / ".ymh" / "sessions.db"));
    registry_.reset();

    test::HostHarnessOptions harness_options;
    harness_options.workspace_root        = workspace_;
    harness_options.workspace_id          = record_.id.value;
    harness_options.socket_path           = workspace_ / ".ymh" / "host.sock";
    harness_options.log_sink              = workspace_ / ".ymh" / "host.log";
    harness_options.env["XDG_STATE_HOME"] = state_.string();
    test::HostHarness harness(std::move(harness_options));
    harness.start();
    ASSERT_TRUE(harness.wait_ready(std::chrono::milliseconds{20000})) << harness.read_log();
    EXPECT_TRUE(std::filesystem::exists(workspace_ / ".ymh" / "sessions.db"))
        << "the daemon must have opened a fresh store for the guard to be exercised";
    harness.stop();

    registry_ = WorkspaceRegistry::openReadOnly(default_registry_config());
    EXPECT_TRUE(registry_->findSession(record_.id, junction_only).has_value());
}

TEST_F(SessionPruneTest, SL_I15_LivenessFlipRetriesTheOtherPathOnce) {
    const SessionId id = create_session(SessionKind::Root, false);

    const std::filesystem::path socket_path = workspace_ / ".ymh" / "host.sock";
    SilentHostSocket            silent(socket_path);

    HostClaim claim;
    claim.workspace  = record_.id;
    claim.pid        = ::getpid();
    claim.bootId     = HostBootId{generate_uuid_v4()};
    claim.socketPath = socket_path;
    registry_->claimHost(claim);

    std::unique_ptr<SessionPersistence> flock_store = SessionPersistence::open(persistence());
    ASSERT_EQ(registry_->probeLiveness(record_.id), HostLiveness::Live);

    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        flock_store.reset();
        silent.accept_and_close();
    });

    std::ostringstream out;
    std::ostringstream err;
    PruneOptions       options;
    options.empty     = true;
    options.yes       = true;
    options.workspace = workspace_;
    EXPECT_EQ(session_prune(options, out, err), 0) << err.str();
    releaser.join();

    expect_pruned(id);
}

} // namespace
