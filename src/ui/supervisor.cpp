#include "ymh/ui/supervisor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "ymh/registry/registry.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/command_registry.hpp"
#include "ymh/ui/session_export.hpp"
#include "ymh/ui/supervisor_connection.hpp"
#include "ymh/ui/supervisor_presence.hpp"
#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace ymh::ui {
namespace {

constexpr std::chrono::milliseconds kFrameInterval{50};
constexpr std::chrono::milliseconds kMaxFrameDelta{250};
constexpr std::chrono::milliseconds kExitQueryMargin{500};
constexpr std::chrono::milliseconds kOwnershipRetryInterval{100};

DaemonStatus daemon_status_for(SupervisorLinkState state) {
    switch (state) {
        case SupervisorLinkState::Connecting:
            return DaemonStatus::Connecting;
        case SupervisorLinkState::Attached:
            return DaemonStatus::Attached;
        case SupervisorLinkState::Detached:
            return DaemonStatus::Detached;
        case SupervisorLinkState::Dead:
            return DaemonStatus::Dead;
    }
    return DaemonStatus::Connecting;
}

protocol::PermissionAnswer wire_answer(payload::PermissionDecisionKind decision) {
    return decision == payload::PermissionDecisionKind::Deny ? protocol::PermissionAnswer::Deny
                                                             : protocol::PermissionAnswer::Allow;
}

protocol::PermissionScope wire_scope(GrantScope scope) {
    switch (scope) {
        case GrantScope::Once:
            return protocol::PermissionScope::Once;
        case GrantScope::Session:
            return protocol::PermissionScope::Session;
        case GrantScope::Always:
            return protocol::PermissionScope::Always;
    }
    return protocol::PermissionScope::Once;
}

bool is_ctrl_home(const ftxui::Event& event) {
    return event.input() == "\x1b[1;5H" || event.input() == "\x1b[1;5~";
}

bool is_ctrl_end(const ftxui::Event& event) {
    return event.input() == "\x1b[1;5F" || event.input() == "\x1b[4;5~";
}

bool is_shift_up(const ftxui::Event& event) {
    return event.input() == "\x1b[1;2A";
}

bool is_shift_down(const ftxui::Event& event) {
    return event.input() == "\x1b[1;2B";
}

struct ExportRequest {
    std::string path;
    bool        edit = false;
    std::string error;
};

ExportRequest parse_export_request(const std::string& args) {
    ExportRequest    request;
    std::istringstream stream(args);
    std::string        token;
    while (stream >> token) {
        if (token == "--edit" || token == "-e") {
            request.edit = true;
        } else if (!token.empty() && token.front() == '-') {
            if (request.error.empty()) {
                request.error = "unknown option: " + token;
            }
        } else if (request.path.empty()) {
            request.path = token;
        } else if (request.error.empty()) {
            request.error = "unexpected argument: " + token;
        }
    }
    return request;
}

std::optional<std::string> tty_name() {
    const char* name = ::ttyname(STDIN_FILENO);
    if (name == nullptr) {
        return std::nullopt;
    }
    return std::string{name};
}

class SupervisorApp final : public UiController {
public:
    explicit SupervisorApp(SupervisorRunOptions options)
        : options_(std::move(options)), adapter_(model_), registry_(CommandRegistry::builtin()) {
        model_.aggregate.flash.enabled = true;
    }

    ~SupervisorApp() override {
        if (scanner_ != nullptr) {
            scanner_->stop();
        }
        if (presence_.has_value()) {
            presence_->deregister();
        }
        for (auto& [id, connection] : connections_) {
            (void)id;
            connection->stop();
        }
    }

    int run() {
        for (const SupervisorWorkspace& workspace : options_.workspaces) {
            attach_workspace(workspace);
        }
        if (model_.workspaces.empty()) {
            return 1;
        }
        model_.activeWorkspaceId = options_.workspaces.front().id;
        if (!options_.initial_workspace.empty()) {
            for (const auto& [id, spec] : specs_) {
                if (spec.cwd == options_.initial_workspace.string()) {
                    model_.activeWorkspaceId = id;
                    break;
                }
            }
        }

        register_presence();
        start_scanner();
        return run_loop();
    }

    void submit(const std::string& text) override {
        if (text.empty()) {
            return;
        }
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr) {
            return;
        }
        SessionUiState* state = model_.session(workspace->activeSessionId);
        if (state != nullptr) {
            state->input.push_history(text);
            state->input.draft.clear();
            state->input.cursor = 0;
            state->input.saved_draft.clear();
            state->input.completion.reset();
            state->command_hints.clear();
            model_.dirty.mark(workspace->activeSessionId, UiDirtyFlag::Input);
        }
        const WorkspaceId workspace_id = workspace->id;
        if (workspace->activeSessionId.value.empty()) {
            create_session(workspace_id, text);
            return;
        }
        prompt(workspace_id, workspace->activeSessionId, text);
    }

    void cancelActive() override {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr || workspace->activeSessionId.value.empty()) {
            return;
        }
        nlohmann::json params{{"session", workspace->activeSessionId.value}};
        submit_to(workspace->id, std::string(protocol::method::kAgentCancel), std::move(params),
                  nullptr);
    }

    void resolvePermission(const SessionId& session, const PermissionRequestId& request,
                           payload::PermissionDecisionKind decision, GrantScope scope) override {
        const WorkspaceId workspace = workspace_of(session);
        protocol::PermissionDecisionParams params;
        params.request_id = request.value;
        params.decision = wire_answer(decision);
        params.scope = wire_scope(scope);
        nlohmann::json body;
        protocol::to_json(body, params);
        submit_to(workspace, std::string(protocol::method::kPermissionDecide), std::move(body),
                  nullptr);
        adapter_.onPermissionResolved(session, request, decision);
    }

    void requestExit() override { begin_exit(); }

private:
    // 16 §4.2 (16-D9). The supervisor stays registered until the user confirms:
    // query the orphaning set read-only, then either exit (empty set), auto-
    // confirm (--yes), or open the prompt. Ctrl+D and `/exit` stay thin callers.
    void begin_exit() {
        if (quit_.load() || model_.exitConfirm.open) {
            return;
        }
        const std::vector<WorkspaceId> orphaning = compute_orphaning_set();
        if (orphaning.empty()) {
            confirm_exit({});
            return;
        }
        if (options_.no_prompt) {
            confirm_exit(orphaning);
            return;
        }
        open_exit_prompt(orphaning);
    }

    // 16 §4.1: a candidate is orphaned iff the daemon reports the caller as its
    // sole live supervisor, no `ymh run` holds it, and no other fresh owner row
    // exists. A daemon that does not answer within `ownership_query_timeout` is
    // skipped; its own watchdog is the backstop.
    std::vector<WorkspaceId> compute_orphaning_set() {
        std::vector<WorkspaceId> orphaning;
        for (auto& [id, connection] : connections_) {
            if (connection->state() != SupervisorLinkState::Attached) {
                continue;
            }
            const std::optional<protocol::OwnershipView> view =
                query_orphaning_view(*connection);
            if (view.has_value() && is_orphaning_view(*view)) {
                orphaning.push_back(id);
            }
        }
        return orphaning;
    }

    // 16 §4.1/O10. `other_fresh_owners` is the daemon's watchdog-cached snapshot,
    // refreshed every `kOwnerWatchdogInterval`, so a peer that just exited cleanly
    // can still be listed for up to one interval. When the live registry shows no
    // other fresh supervisor that cached row is a ghost: the daemon IS orphaned.
    // Re-query until the daemon's cache catches up, so the prompt opens AND the
    // daemon's own admission (§4.4) will accept the shutdown. A genuinely live (or
    // crashed, still-fresh) peer keeps `has_other_fresh_supervisor()` true and is
    // returned unchanged. Bounded by `ownership_query_timeout` + one watchdog tick.
    std::optional<protocol::OwnershipView> query_orphaning_view(SupervisorConnection& connection) {
        const auto deadline = std::chrono::steady_clock::now() +
                              options_.ownership_query_timeout + kOwnerWatchdogInterval +
                              kExitQueryMargin;
        while (true) {
            std::optional<protocol::OwnershipView> view = query_ownership(connection);
            if (!view.has_value() || !is_stale_owner_view(*view) ||
                has_other_fresh_supervisor()) {
                return view;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return view;
            }
            std::this_thread::sleep_for(kOwnershipRetryInterval);
        }
    }

    // The daemon counts only this supervisor's live connection but its cached
    // fresh-owner snapshot still lists someone else: either a ghost or a peer
    // whose row has not been reaped yet. The registry disambiguates.
    static bool is_stale_owner_view(const protocol::OwnershipView& view) noexcept {
        return view.live_supervisors == 1 && view.live_automation == 0 &&
               view.other_fresh_owners > 0;
    }

    bool has_other_fresh_supervisor() const {
        if (options_.registry == nullptr) {
            return true;
        }
        try {
            const std::int64_t now = epoch_ms(options_.wall_clock());
            const std::optional<SupervisorId> me{
                SupervisorId{options_.identity.client_instance.value}};
            return options_.registry->freshSupervisorCount(now, kOwnerLeaseTtl, me) > 0;
        } catch (const std::exception&) {
            return true;
        }
    }

    std::optional<protocol::OwnershipView> query_ownership(SupervisorConnection& connection) {
        struct State {
            std::mutex                              mutex;
            std::condition_variable                 cv;
            bool                                    done = false;
            std::optional<protocol::OwnershipView>  view;
        };
        auto state = std::make_shared<State>();
        connection.submit(
            std::string(protocol::method::kHostOwnership), nlohmann::json::object(),
            [state](SupervisorReply reply) {
                std::lock_guard lock(state->mutex);
                if (reply.ok) {
                    try {
                        protocol::OwnershipView parsed;
                        protocol::from_json(reply.result, parsed);
                        state->view = std::move(parsed);
                    } catch (const std::exception&) {
                    }
                }
                state->done = true;
                state->cv.notify_all();
            },
            options_.ownership_query_timeout);
        std::unique_lock lock(state->mutex);
        state->cv.wait_for(lock, options_.ownership_query_timeout + kExitQueryMargin,
                           [&state] { return state->done; });
        return state->view;
    }

    void open_exit_prompt(const std::vector<WorkspaceId>& orphaning) {
        model_.exitConfirm.open = true;
        model_.exitConfirm.orphaning = orphaning;
        model_.exitConfirm.sessions = count_sessions(orphaning);
        model_.exitConfirm.running = count_running(orphaning);
        model_.exitConfirm.selected = 1;
        model_.mode = UiMode::ExitConfirm;
        model_.dirty.markAggregate();
    }

    int count_sessions(const std::vector<WorkspaceId>& orphaning) const {
        int total = 0;
        for (const WorkspaceId& workspace : orphaning) {
            const auto it = model_.workspaces.find(workspace);
            if (it != model_.workspaces.end()) {
                total += static_cast<int>(it->second.sessions.size());
            }
        }
        return total;
    }

    int count_running(const std::vector<WorkspaceId>& orphaning) const {
        int total = 0;
        for (const auto& [id, session] : model_.sessions) {
            (void)id;
            if (std::find(orphaning.begin(), orphaning.end(), session.workspace) ==
                orphaning.end()) {
                continue;
            }
            if (is_active_state(session.agent_state)) {
                ++total;
            }
        }
        return total;
    }

    // Cancel (n/N/Esc/Ctrl+C): no registry write, still registered, stay in the
    // TUI (16 §4.2). No path may leave a live supervisor deregistered.
    void cancel_exit() {
        model_.exitConfirm = ExitConfirmState{};
        model_.mode = UiMode::Conversation;
        model_.dirty.markAggregate();
    }

    // Confirm (y/Y/Enter on Terminate, or --yes). Pinned order: deregister, then
    // `host.shutdown{last_supervisor}` per orphaned daemon, then close (§4.3).
    void confirm_exit(std::vector<WorkspaceId> orphaning) {
        if (quit_.load()) {
            return;
        }
        model_.exitConfirm = ExitConfirmState{};
        if (presence_.has_value()) {
            presence_->deregister();
            presence_.reset();
        }
        teardown_daemons(orphaning);
        quit_.store(true);
        if (screen_ != nullptr) {
            screen_->Exit();
        }
    }

    // 16 §4.3 steps 2-3: send the typed `last_supervisor` shutdown to each
    // orphaned daemon, then wait for each to close, all bounded by one
    // `teardown_grace`. On expiry the socket is closed anyway (never SIGKILL).
    void teardown_daemons(const std::vector<WorkspaceId>& orphaning) {
        const auto deadline = std::chrono::steady_clock::now() + options_.teardown_grace;
        struct State {
            std::mutex              mutex;
            std::condition_variable cv;
            std::size_t             pending = 0;
        };
        auto state = std::make_shared<State>();
        for (const WorkspaceId& workspace : orphaning) {
            const auto it = connections_.find(workspace);
            if (it == connections_.end()) {
                continue;
            }
            {
                std::lock_guard lock(state->mutex);
                ++state->pending;
            }
            it->second->submit(
                std::string(protocol::method::kHostShutdown),
                nlohmann::json{{"reason", "last_supervisor"}},
                [state](SupervisorReply) {
                    std::lock_guard lock(state->mutex);
                    if (state->pending > 0) {
                        --state->pending;
                    }
                    state->cv.notify_all();
                },
                options_.teardown_grace);
        }
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait_for(lock, options_.teardown_grace + kExitQueryMargin,
                               [&state] { return state->pending == 0; });
        }
        for (const WorkspaceId& workspace : orphaning) {
            const auto it = connections_.find(workspace);
            if (it == connections_.end()) {
                continue;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                break;
            }
            static_cast<void>(it->second->waitUntil(
                [&it] {
                    const SupervisorLinkState link = it->second->state();
                    return link == SupervisorLinkState::Dead ||
                           link == SupervisorLinkState::Detached;
                },
                remaining));
        }
    }

    bool handle_exit_confirm(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
            cancel_exit();
            return true;
        }
        if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Tab) {
            model_.exitConfirm.selected = model_.exitConfirm.selected == 0 ? 1 : 0;
            return true;
        }
        if (event == ftxui::Event::Return) {
            if (model_.exitConfirm.selected == 0) {
                confirm_exit(model_.exitConfirm.orphaning);
            } else {
                cancel_exit();
            }
            return true;
        }
        if (event.is_character()) {
            const std::string character = event.character();
            if (character == "y" || character == "Y") {
                confirm_exit(model_.exitConfirm.orphaning);
            } else if (character == "n" || character == "N") {
                cancel_exit();
            }
        }
        return true;
    }

    void enqueue(std::function<void()> action) {
        {
            const std::lock_guard lock(action_mutex_);
            actions_.push_back(std::move(action));
        }
        if (screen_ != nullptr) {
            screen_->PostEvent(ftxui::Event::Custom);
        }
    }

    void attach_workspace(const SupervisorWorkspace& spec) {
        if (connections_.count(spec.id) != 0) {
            return;
        }
        specs_.emplace(spec.id, spec);
        if (model_.workspaces.count(spec.id) == 0) {
            WorkspaceModel model;
            model.id = spec.id;
            model.title = spec.title;
            model.cwd = spec.cwd;
            model.boot_id = spec.boot_id;
            model.daemonStatus = DaemonStatus::Connecting;
            model_.workspaces.emplace(spec.id, std::move(model));
            model_.dirty.markAggregate();
        }

        SupervisorConnectionConfig config;
        config.socket_path = spec.socket_path;
        config.workspace = spec.id;
        config.expected_boot_id = spec.boot_id;
        config.client_instance = options_.identity.client_instance;
        config.role = options_.identity.role;
        config.profile = protocol::profile_for_role(options_.identity.role);

        SupervisorSink sink;
        const WorkspaceId workspace_id = spec.id;
        sink.on_envelope = [this, workspace_id](const protocol::SessionEnvelope& envelope) {
            enqueue([this, workspace_id, envelope] {
                adapter_.onSessionEnvelope(workspace_id, envelope);
            });
        };
        sink.on_permission = [this, workspace_id](const protocol::PermissionRequest& request) {
            enqueue([this, workspace_id, request] {
                adapter_.onPermissionRequest(workspace_id, request);
            });
        };
        sink.on_notice = [this, workspace_id](const protocol::HostNotice& notice) {
            enqueue([this, workspace_id, notice] {
                if (notice.kind == protocol::HostNoticeKind::SessionClosed &&
                    notice.session.has_value()) {
                    const auto connection = connections_.find(workspace_id);
                    if (connection != connections_.end()) {
                        connection->second->untrack(*notice.session);
                    }
                }
                adapter_.onHostNotice(workspace_id, notice);
                if (notice.kind == protocol::HostNoticeKind::SessionCreated) {
                    // 16 §7.6: one refresh fills the new cell's title; the
                    // broadcast notice itself carries only the SessionId.
                    refresh_sessions(workspace_id);
                }
            });
        };
        sink.on_state = [this, workspace_id](SupervisorLinkState state, std::string detail) {
            on_link_state(workspace_id, state, std::move(detail));
        };

        auto connection =
            std::make_unique<SupervisorConnection>(std::move(config), std::move(sink));
        connection->start();
        connections_.emplace(spec.id, std::move(connection));
    }

    void register_presence() {
        if (options_.registry == nullptr) {
            return;
        }
        SupervisorPresence::Options presence_options;
        presence_options.wall_clock = options_.wall_clock;
        presence_options.tty = tty_name();
        try {
            presence_ = SupervisorPresence::registerSelf(
                *options_.registry, SupervisorId{options_.identity.client_instance.value},
                std::move(presence_options));
        } catch (const std::exception&) {
        }
    }

    void start_scanner() {
        if (options_.registry == nullptr ||
            options_.scan_interval <= std::chrono::milliseconds::zero()) {
            return;
        }
        scanner_ = std::make_unique<DaemonSetScanner>(
            *options_.registry, options_.scan_interval,
            [this](std::vector<SupervisorWorkspace> live) { on_scan(std::move(live)); });
        scanner_->start();
    }

    void on_scan(std::vector<SupervisorWorkspace> live) {
        enqueue([this, live = std::move(live)] {
            for (const SupervisorWorkspace& spec : live) {
                attach_workspace(spec);
            }
        });
    }

    void tick_presence() {
        if (!presence_.has_value()) {
            return;
        }
        const auto now = options_.monotonic_clock();
        if (last_presence_tick_ == std::chrono::steady_clock::time_point{}) {
            last_presence_tick_ = now;
            return;
        }
        if (now - last_presence_tick_ < presence_->heartbeat_interval()) {
            return;
        }
        last_presence_tick_ = now;

        bool updated = false;
        try {
            updated = presence_->heartbeat(epoch_ms(options_.wall_clock()));
        } catch (const std::exception&) {
            updated = false;
        }
        if (updated) {
            return;
        }
        try {
            presence_->reRegister();
        } catch (const std::exception&) {
        }
        for (auto& [id, connection] : connections_) {
            (void)id;
            connection->forceReconnect();
        }
    }

    void on_link_state(const WorkspaceId& workspace, SupervisorLinkState state, std::string) {
        enqueue([this, workspace, state] {
            const auto it = model_.workspaces.find(workspace);
            if (it == model_.workspaces.end()) {
                return;
            }
            it->second.daemonStatus = daemon_status_for(state);
            model_.dirty.markAggregate();
            if (state == SupervisorLinkState::Attached) {
                refresh_sessions(workspace);
            }
        });
    }

    void activate_session(const WorkspaceId& workspace, const SessionId& session) {
        const auto it = model_.workspaces.find(workspace);
        if (it == model_.workspaces.end()) {
            return;
        }
        it->second.activeSessionId = session;
        model_.dirty.mark(session,
                          UiDirtyFlag::Conversation | UiDirtyFlag::Layout | UiDirtyFlag::Input);
        model_.dirty.markAggregate();
    }

    void refresh_sessions(const WorkspaceId& workspace) {
        const auto connection = connections_.find(workspace);
        if (connection == connections_.end()) {
            return;
        }
        connection->second->submit(
            std::string(protocol::method::kSessionList), nlohmann::json::object(),
            [this, workspace](SupervisorReply reply) {
                std::vector<std::pair<SessionId, std::string>> sessions;
                if (reply.ok && reply.result.is_array()) {
                    for (const nlohmann::json& entry : reply.result) {
                        const std::string id = entry.value("id", std::string{});
                        if (id.empty()) {
                            continue;
                        }
                        sessions.emplace_back(SessionId{id}, entry.value("title", std::string{}));
                    }
                }
                const auto connection_it = connections_.find(workspace);
                if (connection_it != connections_.end()) {
                    for (const auto& entry : sessions) {
                        connection_it->second->track(entry.first);
                    }
                }
                enqueue([this, workspace, sessions] {
                    const auto it = model_.workspaces.find(workspace);
                    if (it == model_.workspaces.end()) {
                        return;
                    }
                    for (const auto& [session, title] : sessions) {
                        SessionUiState& state = model_.ensureSessionIn(workspace, session);
                        if (state.status.model.empty()) {
                            state.status.model = options_.config.agent.model;
                        }
                        model_.ensureCellIn(workspace, session);
                        model_.setCellTitle(workspace, session, title);
                    }
                    if (it->second.activeSessionId.value.empty()) {
                        if (!sessions.empty()) {
                            activate_session(workspace, sessions.front().first);
                        } else {
                            // Attach (existing daemon) and spawn paths both
                            // converge here, so auto-create the first session.
                            create_session(workspace, std::string{});
                        }
                    }
                });
            });
    }

    // Creates a session through the daemon and activates it. De-duplicated per
    // workspace: concurrent callers share one `session.create`, and a prompt
    // supplied while a create is in flight is sent once the daemon confirms it.
    void create_session(const WorkspaceId& workspace, std::string prompt_text) {
        const auto connection = connections_.find(workspace);
        if (connection == connections_.end()) {
            return;
        }
        const auto pending = pending_creates_.find(workspace);
        if (pending != pending_creates_.end()) {
            if (pending->second.empty() && !prompt_text.empty()) {
                pending->second = std::move(prompt_text);
            }
            return;
        }
        pending_creates_.emplace(workspace, std::move(prompt_text));
        nlohmann::json create_params{{"title", "tui"}};
        if (!preferred_model_.empty()) {
            create_params["model"] = preferred_model_;
        }
        connection->second->submit(
            std::string(protocol::method::kSessionCreate), std::move(create_params),
            [this, workspace](SupervisorReply reply) {
                std::string session;
                if (reply.ok) {
                    session = reply.result.value("session", std::string{});
                }
                if (!session.empty()) {
                    const auto connection_it = connections_.find(workspace);
                    if (connection_it != connections_.end()) {
                        connection_it->second->track(SessionId{session});
                    }
                }
                enqueue([this, workspace, session] {
                    std::string queued;
                    const auto pending_it = pending_creates_.find(workspace);
                    if (pending_it != pending_creates_.end()) {
                        queued = pending_it->second;
                        pending_creates_.erase(pending_it);
                    }
                    if (session.empty()) {
                        return;
                    }
                    SessionUiState& state =
                        model_.ensureSessionIn(workspace, SessionId{session});
                    if (state.status.model.empty()) {
                        state.status.model = options_.config.agent.model;
                    }
                    model_.ensureCellIn(workspace, SessionId{session});
                    activate_session(workspace, SessionId{session});
                    if (!queued.empty()) {
                        prompt(workspace, SessionId{session}, queued);
                    }
                    // 17 §6 (RB-10): re-list so the created session's title
                    // ("tui") populates its cell via the pinned setCellTitle path.
                    refresh_sessions(workspace);
                });
            });
    }

    void prompt(const WorkspaceId& workspace, const SessionId& session, const std::string& text) {
        submit_to(workspace, std::string(protocol::method::kAgentPrompt),
                  nlohmann::json{{"session", session.value}, {"message", text}}, nullptr);
    }

    void submit_to(const WorkspaceId& workspace, std::string method, nlohmann::json params,
                   SupervisorConnection::ReplyFn reply) {
        const auto connection = connections_.find(workspace);
        if (connection == connections_.end()) {
            if (reply) {
                reply(SupervisorReply{false, {}, 0, "no supervisor connection"});
            }
            return;
        }
        connection->second->submit(std::move(method), std::move(params), std::move(reply));
    }

    WorkspaceId workspace_of(const SessionId& session) const {
        const auto state = model_.sessions.find(session);
        if (state != model_.sessions.end()) {
            return state->second.workspace;
        }
        return model_.activeWorkspaceId;
    }

    void drain() {
        std::deque<std::function<void()>> actions;
        {
            const std::lock_guard lock(action_mutex_);
            actions.swap(actions_);
        }
        for (auto& action : actions) {
            action();
        }
        const auto now = std::chrono::steady_clock::now();
        std::chrono::milliseconds delta =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_);
        if (delta <= std::chrono::milliseconds::zero()) {
            delta = kFrameInterval;
        } else if (delta > kMaxFrameDelta) {
            delta = kMaxFrameDelta;
        }
        last_tick_ = now;
        adapter_.onTick(delta);
        tick_presence();
    }

    SessionUiState* active() {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr) {
            return nullptr;
        }
        return model_.session(workspace->activeSessionId);
    }

    void new_session() {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace != nullptr) {
            create_session(workspace->id, std::string{});
        }
    }

    int edit_export_file(const std::filesystem::path& file) {
        if (screen_ == nullptr) {
            return -1;
        }
        int status = -1;
        screen_->WithRestoredIO(
            [&status, &file] { status = run_editor(file, editor_from_environment()); })();
        return status;
    }

    // /export: reads the active session's durable event log from the workspace
    // store, renders markdown, writes it under the workspace root, and optionally
    // opens the editor with the terminal temporarily restored.
    std::string export_session(const std::string& args) {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr || workspace->activeSessionId.value.empty()) {
            return "export: no active session";
        }
        if (workspace->cwd.empty()) {
            return "export: workspace path is unavailable";
        }
        const ExportRequest request = parse_export_request(args);
        if (!request.error.empty()) {
            return "export: " + request.error;
        }

        const std::filesystem::path root{workspace->cwd};
        PersistenceConfig           config;
        config.db_path   = root / ".ymh" / "sessions.db";
        config.lock_path = root / ".ymh" / "sessions.lock";

        const SessionId                session = workspace->activeSessionId;
        std::optional<SessionHeader>   header;
        EventRange                     events;
        try {
            std::unique_ptr<SessionPersistence> store = SessionPersistence::openReadOnly(config);
            header = store->load(session);
            if (header.has_value()) {
                events = store->read(session);
            }
        } catch (const std::exception& error) {
            return std::string{"export: cannot read session log: "} + error.what();
        }
        if (!header.has_value()) {
            return "export: unknown session " + session.value;
        }

        std::filesystem::path output;
        try {
            LocalEnvironment environment{root};
            output = resolve_export_path(environment, request.path, header->title,
                                         std::time(nullptr));
        } catch (const std::exception& error) {
            return std::string{"export: "} + error.what();
        }

        const std::string markdown = render_session_markdown(*header, events);
        if (!write_export_file(output, markdown)) {
            return "export: cannot write " + output.string();
        }

        std::error_code       relative_error;
        const std::filesystem::path shown = std::filesystem::relative(output, root, relative_error);
        std::string           result = "exported session to " +
                                       (relative_error ? output.string() : shown.string());
        if (request.edit) {
            const int status = edit_export_file(output);
            if (status != 0) {
                result += " (editor exited with status " + std::to_string(status) + ")";
            } else {
                result += " (edited)";
            }
        }
        return result;
    }

    bool dispatch_command(const std::string& line) {
        CommandContext context{model_};
        context.session = active();
        context.request_exit = [this] { requestExit(); };
        context.create_session = [this] { new_session(); };
        context.set_model = [this](const std::string& name) { preferred_model_ = name; };
        context.compact = [this] {
            WorkspaceModel* workspace = model_.activeWorkspace();
            if (workspace == nullptr || workspace->activeSessionId.value.empty()) {
                return;
            }
            nlohmann::json params{{"session", workspace->activeSessionId.value}};
            submit_to(workspace->id, std::string(protocol::method::kSessionCompact),
                      std::move(params), nullptr);
        };
        context.rename_session = [this](const std::string& title) {
            WorkspaceModel* workspace = model_.activeWorkspace();
            if (workspace == nullptr || workspace->activeSessionId.value.empty()) {
                return;
            }
            const SessionId session = workspace->activeSessionId;
            // 19 §6.1 (M1): the reply surfaces a rejected rename. It runs on the
            // pump thread, so it marshals to the UI thread via enqueue and
            // reuses the ErrorOccurred path, which appends a Role::System entry.
            // Success is silent: session/renamed updates the title via the
            // stream (RN9/RN11).
            submit_to(workspace->id, std::string(protocol::method::kSessionRename),
                      nlohmann::json{{"session", session.value}, {"title", title}},
                      [this, session](SupervisorReply reply) {
                          if (reply.ok) {
                              return;
                          }
                          enqueue([this, session, error = reply.error] {
                              model_.apply(UiEvent{ErrorOccurred{
                                  session, "rename failed: " + error}});
                          });
                      });
        };
        context.export_session = [this](const std::string& args) { return export_session(args); };
        return registry_.dispatch(line, context);
    }

    void refresh_hints(SessionUiState& state) {
        state.command_hints.clear();
        const std::string& draft = state.input.draft;
        if (draft.empty() || draft.front() != '/') {
            return;
        }
        const std::string prefix = draft.substr(1);
        if (prefix.find_first_of(" \t") != std::string::npos) {
            return;
        }
        for (const Command* command : registry_.complete(prefix)) {
            state.command_hints.push_back(CommandHint{command->name, command->description});
        }
    }

    // 17 §5 (RB-08): Tab completes a bare `/prefix`. A unique match terminates
    // with a trailing space; a multi-match step stores the cycle and emits
    // "/" + name with no trailing space so the next Tab keeps cycling. While a
    // cycle is active for the current draft it takes precedence over the
    // unique-match terminal case (otherwise the first stepped-to name, itself a
    // complete command, would end the cycle — contradicting the pinned test
    // plan "third Tab = names[1]").
    bool complete_command(SessionUiState& state) {
        InputModel& input = state.input;
        const std::string& draft = input.draft;
        if (draft.empty() || draft.front() != '/' ||
            draft.find_first_of(" \t") != std::string::npos) {
            return false;
        }
        if (input.completion.has_value() && input.completion->draft == input.draft) {
            CompletionCycle& cycle = *input.completion;
            input.draft = "/" + cycle.names[cycle.index];
            input.cursor = input.draft.size();
            cycle.index = (cycle.index + 1) % cycle.names.size();
            cycle.draft = input.draft;
            set_command_hints(state, cycle.names);
            return true;
        }
        const std::vector<const Command*> matches = registry_.complete(draft.substr(1));
        if (matches.empty()) {
            return false;
        }
        if (matches.size() == 1) {
            input.draft = "/" + matches.front()->name + " ";
            input.cursor = input.draft.size();
            input.completion.reset();
            state.command_hints.clear();
            return true;
        }
        input.draft = "/" + CommandRegistry::longest_common_prefix(matches);
        input.cursor = input.draft.size();
        CompletionCycle cycle;
        cycle.draft = input.draft;
        cycle.names.reserve(matches.size());
        state.command_hints.clear();
        for (const Command* command : matches) {
            cycle.names.push_back(command->name);
            state.command_hints.push_back(CommandHint{command->name, command->description});
        }
        input.completion = std::move(cycle);
        return true;
    }

    void set_command_hints(SessionUiState& state, const std::vector<std::string>& names) {
        state.command_hints.clear();
        for (const std::string& name : names) {
            const Command* command = registry_.find(name);
            if (command != nullptr) {
                state.command_hints.push_back(
                    CommandHint{command->name, command->description});
            }
        }
    }

    void scroll_by(bool up, bool page) {
        SessionUiState* state = active();
        if (state == nullptr) {
            return;
        }
        if (up && page) {
            state->scroll.pageUp();
        } else if (up) {
            state->scroll.lineUp();
        } else if (page) {
            state->scroll.pageDown();
        } else {
            state->scroll.lineDown();
        }
        model_.dirty.mark(state->id, UiDirtyFlag::Conversation);
    }

    void scroll_to_top() {
        SessionUiState* state = active();
        if (state == nullptr) {
            return;
        }
        state->scroll.toTop();
        model_.dirty.mark(state->id, UiDirtyFlag::Conversation);
    }

    void scroll_to_bottom() {
        SessionUiState* state = active();
        if (state == nullptr) {
            return;
        }
        state->scroll.toBottom();
        model_.dirty.mark(state->id, UiDirtyFlag::Conversation);
    }

    void toggle_folds() {
        SessionUiState* state = active();
        if (state == nullptr) {
            return;
        }
        state->expand_all_folds = !state->expand_all_folds;
        model_.dirty.mark(state->id, UiDirtyFlag::Tools | UiDirtyFlag::Conversation);
    }

    bool handle_dialog(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
            resolve_dialog(payload::PermissionDecisionKind::Deny, GrantScope::Once);
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            model_.dialog.selected = (model_.dialog.selected + 3) % 4;
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            model_.dialog.selected = (model_.dialog.selected + 1) % 4;
            return true;
        }
        if (event == ftxui::Event::Return) {
            switch (model_.dialog.selected) {
                case 0:
                    resolve_dialog(payload::PermissionDecisionKind::Allow, GrantScope::Once);
                    break;
                case 1:
                    resolve_dialog(payload::PermissionDecisionKind::Allow, GrantScope::Session);
                    break;
                case 2:
                    resolve_dialog(payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
                    break;
                default:
                    resolve_dialog(payload::PermissionDecisionKind::Deny, GrantScope::Once);
                    break;
            }
            return true;
        }
        if (event.is_character()) {
            const std::string character = event.character();
            if (character == "1" || character == "y" || character == "Y") {
                resolve_dialog(payload::PermissionDecisionKind::Allow, GrantScope::Once);
            } else if (character == "2") {
                resolve_dialog(payload::PermissionDecisionKind::Allow, GrantScope::Session);
            } else if (character == "3") {
                resolve_dialog(payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
            } else if (character == "0" || character == "n" || character == "N") {
                resolve_dialog(payload::PermissionDecisionKind::Deny, GrantScope::Once);
            }
        }
        return true;
    }

    void resolve_dialog(payload::PermissionDecisionKind decision, GrantScope scope) {
        const PermissionDialogModel dialog = model_.dialog;
        resolvePermission(dialog.session, dialog.request, decision, scope);
    }

    bool handle_switcher(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
            model_.switcher.close();
            model_.mode = UiMode::Conversation;
            return true;
        }
        if (event == ftxui::Event::ArrowDown || (event.is_character() && event.character() == "j")) {
            model_.switcher.moveDown();
            return true;
        }
        if (event == ftxui::Event::ArrowUp || (event.is_character() && event.character() == "k")) {
            model_.switcher.moveUp();
            return true;
        }
        if (event == ftxui::Event::Tab) {
            model_.switcher.toggleExpand();
            return true;
        }
        if (event == ftxui::Event::Return) {
            const SwitcherCursor cursor = model_.switcher.cursor;
            if (cursor.session.has_value()) {
                model_.focusSession(*cursor.session);
            } else {
                model_.focusWorkspace(cursor.workspace);
            }
            model_.switcher.close();
            model_.mode = UiMode::Conversation;
            return true;
        }
        return true;
    }

    bool handle_input(const ftxui::Event& event) {
        SessionUiState* state = active();
        if (state == nullptr) {
            WorkspaceModel* workspace = model_.activeWorkspace();
            if (workspace != nullptr && workspace->activeSessionId.value.empty()) {
                create_session(workspace->id, std::string{});
            }
            return false;
        }
        InputModel& input = state->input;
        if (event == ftxui::Event::Return) {
            const std::string text = input.draft;
            if (dispatch_command(text)) {
                input.push_history(text);
                input.draft.clear();
                input.cursor = 0;
                input.saved_draft.clear();
                input.completion.reset();
                state->command_hints.clear();
                model_.dirty.mark(state->id, UiDirtyFlag::Input | UiDirtyFlag::Conversation);
                return true;
            }
            submit(text);
            return true;
        }
        if (event == ftxui::Event::Tab) {
            if (complete_command(*state)) {
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
                return true;
            }
            return false;
        }
        if (event == ftxui::Event::Backspace) {
            if (input.cursor > 0) {
                input.draft.erase(input.cursor - 1, 1);
                --input.cursor;
                input.completion.reset();
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::Delete) {
            if (input.delete_forward()) {
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::ArrowLeft) {
            if (input.cursor > 0) {
                --input.cursor;
            }
            return true;
        }
        if (event == ftxui::Event::ArrowRight) {
            if (input.cursor < input.draft.size()) {
                ++input.cursor;
            }
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            if (input.history_up()) {
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            if (input.history_down()) {
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::CtrlU) {
            input.clear_line();
            refresh_hints(*state);
            model_.dirty.mark(state->id, UiDirtyFlag::Input);
            return true;
        }
        if (event == ftxui::Event::CtrlW) {
            if (input.delete_word()) {
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event.is_character()) {
            input.draft.insert(input.cursor, event.character());
            input.cursor += event.character().size();
            input.completion.reset();
            refresh_hints(*state);
            model_.dirty.mark(state->id, UiDirtyFlag::Input);
            return true;
        }
        return false;
    }

    bool handle_event(ftxui::Event event) {
        if (event == ftxui::Event::Custom) {
            drain();
            return true;
        }
        if (model_.exitConfirm.open) {
            return handle_exit_confirm(event);
        }
        if (model_.dialog.open) {
            return handle_dialog(event);
        }
        if (model_.mode == UiMode::Switcher) {
            return handle_switcher(event);
        }
        if (event == ftxui::Event::CtrlS || event == ftxui::Event::CtrlP) {
            model_.openSwitcher();
            return true;
        }
        if (event == ftxui::Event::CtrlD) {
            requestExit();
            return true;
        }
        if (event == ftxui::Event::CtrlC) {
            cancelActive();
            return true;
        }
        if (event == ftxui::Event::CtrlN) {
            new_session();
            return true;
        }
        if (event == ftxui::Event::CtrlO) {
            toggle_folds();
            return true;
        }
        if (event == ftxui::Event::PageUp) {
            scroll_by(true, true);
            return true;
        }
        if (event == ftxui::Event::PageDown) {
            scroll_by(false, true);
            return true;
        }
        if (is_ctrl_home(event)) {
            scroll_to_top();
            return true;
        }
        if (is_ctrl_end(event)) {
            scroll_to_bottom();
            return true;
        }
        if (is_shift_up(event)) {
            scroll_by(true, false);
            return true;
        }
        if (is_shift_down(event)) {
            scroll_by(false, false);
            return true;
        }
        return handle_input(event);
    }

    int run_loop() {
        TerminalLayer terminal(STDIN_FILENO);
        terminal.enterRawMode();
        ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
        screen_ = &screen;
        const Theme theme{terminal.capabilities().trueColor};

        auto renderer = ftxui::Renderer([this, &screen, theme] {
            const TerminalSize size{screen.dimx(), screen.dimy()};
            return build_ui(model_, size, theme);
        });
        auto component = ftxui::CatchEvent(
            renderer, [this](ftxui::Event event) { return handle_event(std::move(event)); });

        quit_.store(false);
        last_tick_ = std::chrono::steady_clock::now();
        std::thread timer([this] {
            while (!quit_.load()) {
                std::this_thread::sleep_for(kFrameInterval);
                if (screen_ != nullptr) {
                    screen_->PostEvent(ftxui::Event::Custom);
                }
            }
        });

        screen.Loop(component);
        quit_.store(true);
        timer.join();
        screen_ = nullptr;
        terminal.leaveRawMode();
        return 0;
    }

    SupervisorRunOptions options_;
    UiModel model_;
    UiEventAdapter adapter_;
    CommandRegistry registry_;
    std::string preferred_model_;
    std::map<WorkspaceId, SupervisorWorkspace> specs_;
    std::map<WorkspaceId, std::unique_ptr<SupervisorConnection>> connections_;
    std::map<WorkspaceId, std::string> pending_creates_;
    std::optional<SupervisorPresence> presence_;
    std::unique_ptr<DaemonSetScanner> scanner_;
    std::chrono::steady_clock::time_point last_presence_tick_{};
    std::mutex action_mutex_;
    std::deque<std::function<void()>> actions_;
    ftxui::ScreenInteractive* screen_ = nullptr;
    std::atomic<bool> quit_{false};
    std::chrono::steady_clock::time_point last_tick_ = std::chrono::steady_clock::now();
};

} // namespace

int run_supervisor(const SupervisorRunOptions& options) {
    SupervisorApp app(options);
    return app.run();
}

} // namespace ymh::ui
