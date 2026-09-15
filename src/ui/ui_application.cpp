#include "ymh/ui/ui_application.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <unistd.h>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/tool_registry.hpp"
#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace ymh::ui {
namespace {

constexpr std::chrono::milliseconds kFrameInterval{50};
constexpr std::chrono::milliseconds kMaxFrameDelta{250};

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

struct PendingPermission {
    SessionId           session;
    PermissionRequestId id;
    PermissionRequest   request;
};

class TuiApp final : public UiController {
public:
    TuiApp(Agent& agent, PermissionGate& gate, EventBus& bus, SessionId session,
           std::string cwd, std::string model)
        : adapter_(model_), agent_(agent), gate_(gate), bus_(bus), session_(session) {
        WorkspaceModel workspace;
        workspace.id = WorkspaceId{"workspace"};
        workspace.cwd = std::move(cwd);
        workspace.daemonStatus = DaemonStatus::Attached;
        workspace.activeSessionId = session;
        model_.activeWorkspaceId = workspace.id;
        model_.workspaces.emplace(workspace.id, std::move(workspace));
        SessionUiState& state = model_.ensureSession(session);
        state.status.model = std::move(model);
        gate_.set_attention_hook([this](const PermissionRequestId& id,
                                        const PermissionRequest& request) {
            enqueue_permission(id, request);
        });
        subscription_ = bus_.subscribe([this](const Event& event) { on_bus_event(event); });
        last_tick_ = std::chrono::steady_clock::now();
        worker_ = std::thread([this] { worker_loop(); });
    }

    ~TuiApp() override {
        stop_worker();
        subscription_.unsubscribe();
    }

    TuiApp(const TuiApp&) = delete;
    TuiApp& operator=(const TuiApp&) = delete;

    int run() {
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

    void submit(const std::string& text) override {
        if (text.empty()) {
            return;
        }
        SessionUiState* state = active();
        if (state != nullptr) {
            state->input.push_history(text);
            state->input.draft.clear();
            state->input.cursor = 0;
            model_.dirty.mark(session_, UiDirtyFlag::Input);
        }
        {
            const std::lock_guard lock(inbox_mutex_);
            inbox_.push_back(user_message(text));
        }
        inbox_cv_.notify_one();
    }

    void cancelActive() override {
        agent_.cancel();
    }

    void resolvePermission(const SessionId& session, const PermissionRequestId& request,
                           payload::PermissionDecisionKind decision, GrantScope scope) override {
        gate_.decide(request, decision, scope, "user");
        adapter_.onPermissionResolved(session, request, decision);
    }

    void requestExit() override {
        quit_.store(true);
        if (screen_ != nullptr) {
            screen_->Exit();
        }
    }

private:
    SessionUiState* active() {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr) {
            return nullptr;
        }
        return model_.session(workspace->activeSessionId);
    }

    void on_bus_event(const Event& event) {
        {
            const std::lock_guard lock(queue_mutex_);
            event_queue_.push_back(event);
        }
        if (screen_ != nullptr) {
            screen_->PostEvent(ftxui::Event::Custom);
        }
    }

    void enqueue_permission(const PermissionRequestId& id, const PermissionRequest& request) {
        {
            const std::lock_guard lock(queue_mutex_);
            permission_queue_.push_back(PendingPermission{request.session, id, request});
        }
        if (screen_ != nullptr) {
            screen_->PostEvent(ftxui::Event::Custom);
        }
    }

    void drain() {
        std::vector<Event> events;
        std::vector<PendingPermission> permissions;
        {
            const std::lock_guard lock(queue_mutex_);
            events.swap(event_queue_);
            permissions.swap(permission_queue_);
        }
        for (const Event& event : events) {
            adapter_.onEvent(event);
        }
        for (const PendingPermission& pending : permissions) {
            adapter_.onPermissionRequest(pending.session, pending.id, pending.request);
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

    bool handle_dialog(const ftxui::Event& event) {
        if (!model_.dialog.open) {
            return false;
        }
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
            resolve_selected();
            return true;
        }
        if (event.is_character()) {
            const std::string character = event.character();
            if (character == "1" || character == "y" || character == "Y") {
                resolve_dialog(payload::PermissionDecisionKind::Allow, GrantScope::Once);
                return true;
            }
            if (character == "2") {
                resolve_dialog(payload::PermissionDecisionKind::Allow, GrantScope::Session);
                return true;
            }
            if (character == "3") {
                resolve_dialog(payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
                return true;
            }
            if (character == "0" || character == "n" || character == "N") {
                resolve_dialog(payload::PermissionDecisionKind::Deny, GrantScope::Once);
                return true;
            }
        }
        return true;
    }

    void resolve_selected() {
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
    }

    void resolve_dialog(payload::PermissionDecisionKind decision, GrantScope scope) {
        const PermissionDialogModel dialog = model_.dialog;
        resolvePermission(dialog.session, dialog.request, decision, scope);
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
                model_.dirty.mark(session_, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::Delete) {
            if (input.cursor < input.draft.size()) {
                input.draft.erase(input.cursor, 1);
                model_.dirty.mark(session_, UiDirtyFlag::Input);
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
            if (!input.history.empty() && input.history_pos > 0) {
                --input.history_pos;
                input.draft = input.history[input.history_pos];
                input.cursor = input.draft.size();
                model_.dirty.mark(session_, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            if (input.history_pos + 1 < input.history.size()) {
                ++input.history_pos;
                input.draft = input.history[input.history_pos];
            } else {
                input.history_pos = input.history.size();
                input.draft.clear();
            }
            input.cursor = input.draft.size();
            model_.dirty.mark(session_, UiDirtyFlag::Input);
            return true;
        }
        if (event == ftxui::Event::CtrlU) {
            input.draft.clear();
            input.cursor = 0;
            model_.dirty.mark(session_, UiDirtyFlag::Input);
            return true;
        }
        if (event == ftxui::Event::CtrlW) {
            while (input.cursor > 0 && input.draft[input.cursor - 1] == ' ') {
                input.draft.erase(input.cursor - 1, 1);
                --input.cursor;
            }
            while (input.cursor > 0 && input.draft[input.cursor - 1] != ' ') {
                input.draft.erase(input.cursor - 1, 1);
                --input.cursor;
            }
            model_.dirty.mark(session_, UiDirtyFlag::Input);
            return true;
        }
        if (event.is_character()) {
            input.draft.insert(input.cursor, event.character());
            input.cursor += event.character().size();
            model_.dirty.mark(session_, UiDirtyFlag::Input);
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
        if (event == ftxui::Event::CtrlC) {
            cancelActive();
            return true;
        }
        if (event == ftxui::Event::CtrlD) {
            requestExit();
            return true;
        }
        if (event == ftxui::Event::CtrlL) {
            return true;
        }
        if (event == ftxui::Event::Escape) {
            return true;
        }
        return handle_input(event);
    }

    void worker_loop() {
        for (;;) {
            Message message;
            bool has_message = false;
            {
                std::unique_lock lock(inbox_mutex_);
                inbox_cv_.wait(lock, [this] { return quit_.load() || !inbox_.empty(); });
                if (quit_.load() && inbox_.empty()) {
                    return;
                }
                if (!inbox_.empty()) {
                    message = std::move(inbox_.front());
                    inbox_.pop_front();
                    has_message = true;
                }
            }
            if (has_message) {
                agent_.send(std::move(message));
            }
        }
    }

    void stop_worker() {
        quit_.store(true);
        inbox_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    UiModel          model_;
    UiEventAdapter   adapter_;
    Agent&           agent_;
    PermissionGate&  gate_;
    EventBus&        bus_;
    Subscription     subscription_;
    SessionId        session_;

    std::mutex               queue_mutex_;
    std::vector<Event>       event_queue_;
    std::vector<PendingPermission> permission_queue_;

    std::mutex              inbox_mutex_;
    std::condition_variable inbox_cv_;
    std::deque<Message>     inbox_;
    std::atomic<bool>       quit_{false};
    std::thread             worker_;

    ftxui::ScreenInteractive* screen_ = nullptr;
    std::chrono::steady_clock::time_point last_tick_{};
};

} // namespace

int run_tui(const UiRunOptions& options) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical(options.workspace, error);
    if (error || !std::filesystem::is_directory(root)) {
        std::cerr << "ymh: workspace not found: " << options.workspace << '\n';
        return 2;
    }

    PersistenceConfig persistence;
    persistence.db_path = root / ".ymh" / "sessions.db";
    persistence.lock_path = root / ".ymh" / "sessions.lock";
    persistence.boot_id = BootId{make_boot_id()};

    std::unique_ptr<SessionPersistence> store;
    try {
        store = SessionPersistence::open(persistence);
    } catch (const std::exception& open_error) {
        std::cerr << "ymh: cannot open session store: " << open_error.what() << '\n';
        return 2;
    }

    EventBus bus;
    SessionManager sessions(*store, bus);
    const ToolConfig tool_config;
    LocalEnvironment environment(root, SandboxMode::Workspace, tool_config);
    ResourceGovernor governor;
    ToolRegistry tools;
    std::vector<ToolRegistry::Registration> registrations;
    for (std::unique_ptr<Tool>& tool : make_builtin_tools(tool_config)) {
        registrations.push_back(tools.add(std::move(tool)));
    }
    tools.freeze();

    const PermissionConfig permission_config = to_permission_config(options.config);
    RulePermissionPolicy policy(permission_config);
    const AgentConfig agent_config = to_agent_config(options.config);
    SessionContextAssembler assembler(tools, agent_config.system_prompt);
    DefaultTokenEstimator estimator;
    ProviderRegistry providers = make_default_provider_registry();
    LLMProviderConfig provider_config = to_provider_config(options.config);
    OutputRing ring(governor.caps().session_output_ring_bytes);
    RingOutputSink sink(ring);
    PermissionGate gate(policy, permission_config);

    AgentServices services;
    services.sessions = &sessions;
    services.governor = &governor;
    services.tools = &tools;
    services.policy = &policy;
    services.gate = &gate;
    services.context = &assembler;
    services.execution = &environment;
    services.logger = &category_logger(LogCategory::Tool);
    services.output = &sink;
    services.estimator = &estimator;
    services.providers = &providers;
    services.provider_config = provider_config;

    AgentRegistry registry(services, agent_config);
    SessionOptions session_options;
    session_options.cwd = root;
    session_options.serverProfile = "interactive";
    session_options.model = effective_model(options.config);
    session_options.title = "main";
    const std::expected<AgentId, AgentError> created = registry.create(session_options);
    if (!created.has_value()) {
        std::cerr << "ymh: cannot create session: " << created.error().detail << '\n';
        return 2;
    }
    Agent& agent = registry.get(*created);
    const SessionId session = agent.session();
    if (!store->acquireLease(session)) {
        std::cerr << "ymh: session lease unavailable\n";
        registry.dispose(*created);
        return 2;
    }

    int exit_code = 0;
    {
        TuiApp app(agent, gate, bus, session, root.string(), effective_model(options.config));
        exit_code = app.run();
    }

    registry.dispose(*created);
    try {
        store->releaseLease(session);
    } catch (const std::exception&) {
    }
    return exit_code;
}

} // namespace ymh::ui
