// Spec 16 §8.4.2: the hermetic two-supervisor scenarios. Driven by the real
// `ymh` binary (supervisor TUI on a PTY + `ymh --host` daemons) with the Fake
// LLM; every wait is event/condition-driven, never a sleep-based synchronisation.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/child_process.hpp"
#include "support/host_harness.hpp"
#include "support/pty_child.hpp"
#include "support/short_temp.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr const char* kFakeText = R"([{"text": "hello", "finish": "stop"}])";
constexpr const char* kFakeSlowTool = R"([
  {"text": "working", "finish": "tool_calls",
   "tool_calls": [{"name": "shell", "arguments": {"command": "sleep 8"}, "id": "call-1"}]},
  {"text": "done", "finish": "stop"}
])";
constexpr const char* kAllowAll =
    "{\n  \"permissions\": {\n    \"read\": \"allow\",\n    \"write\": \"allow\",\n"
    "    \"shell\": \"allow\"\n  }\n}\n";

RegistryConfig registry_config_for(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path         = root / ".state" / "ymh" / "registry.db";
    config.lock_path       = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots = {};
    return config;
}

std::string register_workspace(const RegistryConfig& config, const std::filesystem::path& root,
                               const std::string& title) {
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(config);
    return registry->registerWorkspace(root, title).id.value;
}

std::unique_ptr<protocol::HostConnection> open_connection(const std::filesystem::path& socket) {
    auto connection = std::make_unique<protocol::HostConnection>();
    connection->connect(socket.string());
    static_cast<void>(connection->handshake(protocol::ServerProfile::Interactive,
                                            protocol::ClientInstanceId{generate_uuid_v4()}));
    return connection;
}

std::vector<protocol::SessionEnvelope> replay_session(const std::filesystem::path& socket,
                                                      const std::string&           session) {
    std::unique_ptr<protocol::HostConnection> connection = open_connection(socket);
    static_cast<void>(connection->request(
        std::string(protocol::method::kSessionReplay),
        nlohmann::json{{"session", session}, {"from", {{"kind", "beginning"}}}}));

    std::vector<protocol::SessionEnvelope> envelopes;
    const auto                             deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::optional<protocol::Notification> notification =
            connection->nextNotification(200ms);
        if (!notification.has_value()) {
            if (!envelopes.empty()) {
                break;
            }
            continue;
        }
        if (notification->method == protocol::notify::kEventStream) {
            envelopes.push_back(
                notification->params.get<protocol::StreamNotification>().envelope);
            continue;
        }
        if (notification->method == protocol::notify::kEventUnsubscribed) {
            break;
        }
    }
    return envelopes;
}

bool has_event(const std::vector<protocol::SessionEnvelope>& envelopes, EventType type) {
    return std::any_of(envelopes.begin(), envelopes.end(),
                       [type](const protocol::SessionEnvelope& envelope) {
                           return envelope.event.type == type;
                       });
}

std::string active_session_of(const nlohmann::json& status) {
    if (!status.contains("active_session") || status.at("active_session").is_null()) {
        return {};
    }
    return status.at("active_session").get<std::string>();
}

std::map<std::string, std::string> child_env(const ShortTempRoot& root,
                                             const std::filesystem::path& script) {
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"]      = (root.path() / ".state").string();
    env["HOME"]                = root.path().string();
    env["XDG_CONFIG_HOME"]     = (root.path() / ".config").string();
    env["XDG_CACHE_HOME"]      = (root.path() / ".cache").string();
    env["TERM"]                = "xterm-256color";
    env["YMH_FAKE_LLM_SCRIPT"] = script.string();
    return env;
}

// 16 §8.4.2 (2), 16-D4/O8/C-H1: a session another supervisor creates is
// announced to every attached Interactive client as `host.event{SessionCreated}`
// carrying the `SessionId` (the broadcast, not a poll). The `UiModel::apply`
// half of the scenario (the cell is inserted/removed) is unit-tested in
// `tests/unit/ui_event_adapter_test.cpp:HostNoticeSessionLifecycleCarriesSessionId`.
TEST(OwnershipTwoSupervisor, SessionCreatedBroadcastReachesAttachedClients) {
    ShortTempRoot root("ymh-own-vis");
    root.write(".ymh/config.jsonc", kAllowAll);
    root.write("fake.json", kFakeText);

    const std::string workspace_id =
        register_workspace(registry_config_for(root.path()), root.path(), "vis");
    HostHarnessOptions options;
    options.binary                     = resolve_ymh_binary();
    options.workspace_root             = root.path();
    options.workspace_id               = workspace_id;
    options.socket_path                = root.host_socket();
    options.env["XDG_STATE_HOME"]      = (root.path() / ".state").string();
    options.env["HOME"]                = root.path().string();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    HostDaemonGuard guard(workspace_id);

    std::unique_ptr<protocol::HostConnection> observer = open_connection(root.host_socket());
    std::unique_ptr<protocol::HostConnection> creator  = open_connection(root.host_socket());

    const nlohmann::json reply = creator->request(std::string(protocol::method::kSessionCreate),
                                                  nlohmann::json{{"title", "peer"}});
    const std::string    session = reply.value("session", std::string{});
    ASSERT_FALSE(session.empty());

    bool       saw_created = false;
    const auto created_deadline = std::chrono::steady_clock::now() + 10s;
    while (!saw_created && std::chrono::steady_clock::now() < created_deadline) {
        const std::optional<protocol::Notification> notice = observer->nextNotification(500ms);
        if (!notice.has_value()) {
            continue;
        }
        if (notice->method == protocol::notify::kHostEvent &&
            notice->params.value("kind", std::string{}) == "session_created" &&
            notice->params.value("session", std::string{}) == session) {
            saw_created = true;
        }
    }
    EXPECT_TRUE(saw_created)
        << "host.event{SessionCreated} must reach every attached Interactive client";

    static_cast<void>(
        observer->request(std::string(protocol::method::kEventSubscribe),
                          nlohmann::json{{"session", session}, {"from", {{"kind", "now"}}}}));
    static_cast<void>(creator->request(std::string(protocol::method::kSessionDelete),
                                       nlohmann::json{{"session", session}, {"confirm", true}}));
    bool       saw_closed = false;
    const auto closed_deadline = std::chrono::steady_clock::now() + 10s;
    while (!saw_closed && std::chrono::steady_clock::now() < closed_deadline) {
        const std::optional<protocol::Notification> notice = observer->nextNotification(500ms);
        if (!notice.has_value()) {
            continue;
        }
        if (notice->method == protocol::notify::kHostEvent &&
            notice->params.value("kind", std::string{}) == "session_closed" &&
            notice->params.value("session", std::string{}) == session) {
            saw_closed = true;
        }
    }
    EXPECT_TRUE(saw_closed) << "closing the session must be announced to subscribers";

    creator->close();
    observer->close();
}

// 16 §8.4.2 (6), O13: `ymh run` holds D_A mid-turn while the supervisor exits.
// The daemon is not in the orphaning set (a live Automation client keeps it), so
// the supervisor leaves without a prompt, the turn completes, and only the
// automation's own disconnect lets the watchdog stop the daemon.
TEST(OwnershipTwoSupervisor, AutomationGuardKeepsDaemonThroughSupervisorExit) {
    ShortTempRoot root("ymh-own-automation");
    const std::filesystem::path workspace = root.path() / "alpha";
    std::filesystem::create_directories(workspace);
    root.write("alpha/.ymh/config.jsonc", kAllowAll);
    root.write("alpha/fake.json", kFakeSlowTool);

    const std::string workspace_id =
        register_workspace(registry_config_for(root.path()), workspace, "alpha");
    HostDaemonGuard guard(workspace_id);

    const std::filesystem::path binary = resolve_ymh_binary();
    PtyChild supervisor;
    ASSERT_TRUE(supervisor.spawn(binary, workspace, child_env(root, workspace / "fake.json"), 30,
                                 100, {"--new"}));
    ASSERT_TRUE(supervisor.wait_for("Type a message and press Enter", 30s))
        << supervisor.plain();
    ASSERT_EQ(host_processes(&workspace_id).size(), 1u);

    ChildOptions run_options;
    run_options.argv        = {binary.string(), "--workspace", workspace.string(), "run",
                               "do the slow thing"};
    run_options.executable  = binary;
    run_options.cwd         = workspace;
    run_options.stdout_path = root.path() / "run.stdout.log";
    run_options.stderr_path = root.path() / "run.stderr.log";
    run_options.env         = child_env(root, workspace / "fake.json");
    ChildProcess runner;
    runner.start(std::move(run_options));

    bool       automation_attached = false;
    const auto automation_deadline = std::chrono::steady_clock::now() + 20s;
    while (!automation_attached && std::chrono::steady_clock::now() < automation_deadline) {
        std::unique_ptr<protocol::HostConnection> probe =
            open_connection(workspace / ".ymh" / "host.sock");
        automation_attached =
            probe->request(std::string(protocol::method::kHostOwnership))
                .get<protocol::OwnershipView>()
                .live_automation >= 1;
        probe->close();
        if (!automation_attached) {
            std::this_thread::sleep_for(50ms);
        }
    }
    ASSERT_TRUE(automation_attached)
        << "ymh run never attached as Automation; stdout="
        << read_text_file(root.path() / "run.stdout.log")
        << " stderr=" << read_text_file(root.path() / "run.stderr.log");

    supervisor.write("/exit\r");
    const std::optional<int> status = supervisor.wait_for_exit(20s);
    ASSERT_TRUE(status.has_value())
        << supervisor.plain() << " run_out=" << read_text_file(root.path() / "run.stdout.log")
        << " run_err=" << read_text_file(root.path() / "run.stderr.log");
    EXPECT_EQ(*status, 0) << "a live Automation client must suppress the exit prompt";
    EXPECT_EQ(host_processes(&workspace_id).size(), 1u)
        << "D_A must not be torn down while ymh run holds it";

    const std::optional<ExitStatus> run_status = runner.wait_for(30s);
    ASSERT_TRUE(run_status.has_value()) << "the automation turn never completed";
    EXPECT_TRUE(run_status->exited);
    EXPECT_EQ(run_status->code, 0) << "the turn must complete after the supervisor left";

    bool       daemon_stopped = false;
    const auto stop_deadline  = std::chrono::steady_clock::now() + 20s;
    while (!daemon_stopped && std::chrono::steady_clock::now() < stop_deadline) {
        daemon_stopped = host_processes(&workspace_id).empty();
        if (!daemon_stopped) {
            std::this_thread::sleep_for(50ms);
        }
    }
    EXPECT_TRUE(daemon_stopped)
        << "the watchdog must stop the daemon once the automation disconnects";
}

// 16 §8.4.2 (3), O9: a peer replays the owner's session from `beginning` and
// submits a follow-up without disturbing the owner. The daemon's active session
// and the registry rows are unchanged (focus isolation).
TEST(OwnershipTwoSupervisor, PeerReplayAndFollowUpLeaveOwnerFocusUnchanged) {
    ShortTempRoot root("ymh-own-switch");
    root.write(".ymh/config.jsonc", kAllowAll);
    root.write("fake.json", kFakeText);

    const std::string workspace_id =
        register_workspace(registry_config_for(root.path()), root.path(), "switch");
    HostHarnessOptions options;
    options.binary                     = resolve_ymh_binary();
    options.workspace_root             = root.path();
    options.workspace_id               = workspace_id;
    options.socket_path                = root.host_socket();
    options.env["XDG_STATE_HOME"]      = (root.path() / ".state").string();
    options.env["HOME"]                = root.path().string();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    HostDaemonGuard guard(workspace_id);

    std::unique_ptr<protocol::HostConnection> owner = open_connection(root.host_socket());
    const nlohmann::json created = owner->request(std::string(protocol::method::kSessionCreate),
                                                  nlohmann::json{{"title", "owner"}});
    const std::string    session = created.value("session", std::string{});
    ASSERT_FALSE(session.empty());
    static_cast<void>(owner->request(std::string(protocol::method::kAgentPrompt),
                                     nlohmann::json{{"session", session}, {"message", "first"}}));

    const auto assistant_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < assistant_deadline &&
           !has_event(replay_session(root.host_socket(), session), EventType::AssistantMessage)) {
        std::this_thread::sleep_for(50ms);
    }

    const nlohmann::json status_before =
        owner->request(std::string(protocol::method::kHostStatus));
    const std::string active_before = active_session_of(status_before);
    std::vector<SupervisorRow> rows_before;
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config_for(root.path()));
        rows_before = registry->listSupervisors();
    }

    const std::vector<protocol::SessionEnvelope> peer_replay =
        replay_session(root.host_socket(), session);
    EXPECT_TRUE(has_event(peer_replay, EventType::UserMessage))
        << "a peer replaying from `beginning` must see the owner's user message";
    EXPECT_TRUE(has_event(peer_replay, EventType::AssistantMessage))
        << "a peer replaying from `beginning` must see the owner's assistant reply";

    std::unique_ptr<protocol::HostConnection> peer = open_connection(root.host_socket());
    static_cast<void>(peer->request(std::string(protocol::method::kAgentFollowup),
                                    nlohmann::json{{"session", session}, {"message", "second"}}));
    const auto followup_deadline = std::chrono::steady_clock::now() + 15s;
    std::size_t assistant_messages = 0;
    while (std::chrono::steady_clock::now() < followup_deadline) {
        const std::vector<protocol::SessionEnvelope> replay =
            replay_session(root.host_socket(), session);
        assistant_messages = static_cast<std::size_t>(std::count_if(
            replay.begin(), replay.end(), [](const protocol::SessionEnvelope& envelope) {
                return envelope.event.type == EventType::AssistantMessage;
            }));
        if (assistant_messages >= 2) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_GE(assistant_messages, 2u) << "the peer follow-up must run in the owner's session";

    const nlohmann::json status_after = owner->request(std::string(protocol::method::kHostStatus));
    EXPECT_EQ(active_session_of(status_after), active_before)
        << "switching must not change the daemon's active session";
    std::vector<SupervisorRow> rows_after;
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config_for(root.path()));
        rows_after = registry->listSupervisors();
    }
    EXPECT_EQ(rows_after.size(), rows_before.size())
        << "switching must not change the ownership set";

    peer->close();
    owner->close();
}

} // namespace
