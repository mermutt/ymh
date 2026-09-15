#include "ymh/ui/supervisor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_connection.hpp"
#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace ymh::ui {
namespace {

constexpr std::chrono::milliseconds kFrameInterval{50};
constexpr std::chrono::milliseconds kMaxFrameDelta{250};

protocol::ClientInstanceId load_client_instance() {
    const std::filesystem::path path = default_state_dir() / "supervisor.json";
    try {
        if (std::filesystem::exists(path)) {
            std::ifstream input(path);
            const nlohmann::json document = nlohmann::json::parse(input);
            const std::string value = document.value("client_instance", std::string{});
            if (!value.empty()) {
                return protocol::ClientInstanceId{value};
            }
        }
    } catch (const std::exception&) {
    }
    const std::string minted = generate_uuid_v4();
    try {
        ensure_state_dir(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        output << nlohmann::json{{"client_instance", minted}}.dump();
    } catch (const std::exception&) {
    }
    return protocol::ClientInstanceId{minted};
}

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

class SupervisorApp final : public UiController {
public:
    explicit SupervisorApp(SupervisorRunOptions options)
        : options_(std::move(options)), adapter_(model_) {
        model_.aggregate.flash.enabled = true;
    }

    ~SupervisorApp() override {
        for (auto& [id, connection] : connections_) {
            (void)id;
            connection->stop();
        }
    }

    int run() {
        for (const SupervisorWorkspace& workspace : options_.workspaces) {
            specs_.emplace(workspace.id, workspace);
            WorkspaceModel model;
            model.id = workspace.id;
            model.title = workspace.title;
            model.cwd = workspace.cwd;
            model.boot_id = workspace.boot_id;
            model.daemonStatus = DaemonStatus::Connecting;
            model_.workspaces.emplace(workspace.id, std::move(model));
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

        const protocol::ClientInstanceId instance = load_client_instance();
        std::vector<SupervisorConnection*> started;
        for (const SupervisorWorkspace& spec : options_.workspaces) {
            SupervisorConnectionConfig config;
            config.socket_path = spec.socket_path;
            config.workspace = spec.id;
            config.expected_boot_id = spec.boot_id;
            config.client_instance = instance;
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
                    adapter_.onHostNotice(workspace_id, notice);
                });
            };
            sink.on_state = [this, workspace_id](SupervisorLinkState state, std::string detail) {
                on_link_state(workspace_id, state, std::move(detail));
            };
            auto connection = std::make_unique<SupervisorConnection>(std::move(config),
                                                                     std::move(sink));
            started.push_back(connection.get());
            connections_.emplace(spec.id, std::move(connection));
        }
        for (SupervisorConnection* connection : started) {
            connection->start();
        }

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
            model_.dirty.mark(workspace->activeSessionId, UiDirtyFlag::Input);
        }
        const WorkspaceId workspace_id = workspace->id;
        if (workspace->activeSessionId.value.empty()) {
            create_session_and_prompt(workspace_id, text);
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

    void requestExit() override {
        quit_.store(true);
        if (screen_ != nullptr) {
            screen_->Exit();
        }
    }

private:
    void enqueue(std::function<void()> action) {
        {
            const std::lock_guard lock(action_mutex_);
            actions_.push_back(std::move(action));
        }
        if (screen_ != nullptr) {
            screen_->PostEvent(ftxui::Event::Custom);
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

    void refresh_sessions(const WorkspaceId& workspace) {
        const auto connection = connections_.find(workspace);
        if (connection == connections_.end()) {
            return;
        }
        connection->second->submit(
            std::string(protocol::method::kSessionList), nlohmann::json::object(),
            [this, workspace](SupervisorReply reply) {
                if (!reply.ok || !reply.result.is_array()) {
                    return;
                }
                std::vector<std::pair<SessionId, std::string>> sessions;
                for (const nlohmann::json& entry : reply.result) {
                    const std::string id = entry.value("id", std::string{});
                    if (id.empty()) {
                        continue;
                    }
                    sessions.emplace_back(SessionId{id}, entry.value("title", std::string{}));
                }
                const auto connection_it = connections_.find(workspace);
                if (connection_it != connections_.end()) {
                    for (const auto& [session, title] : sessions) {
                        (void)title;
                        connection_it->second->track(session);
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
                    }
                    if (it->second.activeSessionId.value.empty() && !sessions.empty()) {
                        it->second.activeSessionId = sessions.front().first;
                        model_.focusSession(sessions.front().first);
                    }
                });
            });
    }

    void create_session_and_prompt(const WorkspaceId& workspace, const std::string& text) {
        const auto connection = connections_.find(workspace);
        if (connection == connections_.end()) {
            return;
        }
        connection->second->submit(
            std::string(protocol::method::kSessionCreate),
            nlohmann::json{{"title", "tui"}},
            [this, workspace, text](SupervisorReply reply) {
                if (!reply.ok) {
                    return;
                }
                const std::string session = reply.result.value("session", std::string{});
                if (session.empty()) {
                    return;
                }
                const auto connection_it = connections_.find(workspace);
                if (connection_it != connections_.end()) {
                    connection_it->second->track(SessionId{session});
                }
                enqueue([this, workspace, session] {
                    SessionUiState& state =
                        model_.ensureSessionIn(workspace, SessionId{session});
                    state.status.model = options_.config.agent.model;
                    model_.ensureCellIn(workspace, SessionId{session});
                    const auto it = model_.workspaces.find(workspace);
                    if (it != model_.workspaces.end()) {
                        it->second.activeSessionId = SessionId{session};
                    }
                });
                prompt(workspace, SessionId{session}, text);
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
    }

    SessionUiState* active() {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr) {
            return nullptr;
        }
        return model_.session(workspace->activeSessionId);
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
            return false;
        }
        InputModel& input = state->input;
        if (event == ftxui::Event::Return) {
            const std::string text = input.draft;
            if (text == "/exit") {
                requestExit();
                return true;
            }
            submit(text);
            return true;
        }
        if (event == ftxui::Event::Backspace) {
            if (input.cursor > 0) {
                input.draft.erase(input.cursor - 1, 1);
                --input.cursor;
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
        if (event.is_character()) {
            input.draft.insert(input.cursor, event.character());
            input.cursor += event.character().size();
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
    std::map<WorkspaceId, SupervisorWorkspace> specs_;
    std::map<WorkspaceId, std::unique_ptr<SupervisorConnection>> connections_;
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
