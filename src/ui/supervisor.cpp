#include "ymh/ui/supervisor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

#include "ymh/llm/redaction.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/command_registry.hpp"
#include "ymh/ui/context_request.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/session_export.hpp"
#include "ymh/ui/status_format.hpp"
#include "ymh/ui/supervisor_connection.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/supervisor_presence.hpp"
#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace ymh::ui {
namespace {

constexpr std::chrono::milliseconds kFrameInterval{50};
constexpr std::chrono::milliseconds kMaxFrameDelta{250};
// RB-18: idle presence heartbeats are serviced by a non-repainting closure at
// this cadence (well under the 5s owner heartbeat interval), so an idle TUI
// never wakes the renderer.
constexpr std::chrono::milliseconds kPresencePostInterval{1000};
// RB-12/RB-18: a modal's resolving keystroke is consumed by the modal, but a
// paste/burst continues after it. Printable input is suppressed for this long
// after a modal closes. The tail of a burst rides in the *same* input batch as
// the resolving key, so it is processed within microseconds of it; 25 ms is
// ~1000x that same-batch margin, so it covers the tail with room to spare while
// staying far below the ~100 ms a person needs before typing the next deliberate
// key, so real input is never lost. The window is wall-clock based, so it
// self-expires even when no repaint is scheduled (RB-18 removed the periodic
// repaint).
constexpr std::chrono::milliseconds kModalTailWindow{25};
constexpr std::chrono::milliseconds kExitQueryMargin{500};
constexpr std::chrono::milliseconds kOwnershipRetryInterval{100};
constexpr int kContextScrollPage = 6;

// RB-18: order-independent identity of one daemon-set scan. The scanner fires
// every 2s; an unchanged set must not reach `on_scan` (which enqueues a UI
// action and therefore forces a full repaint). Compared on the scanner thread
// only.
std::vector<std::string> live_scan_key(const std::vector<SupervisorWorkspace>& live) {
    std::vector<std::string> key;
    key.reserve(live.size());
    for (const SupervisorWorkspace& workspace : live) {
        key.push_back(workspace.id.value + "\n" + workspace.boot_id + "\n" +
                      workspace.socket_path + "\n" + workspace.cwd + "\n" +
                      workspace.title);
    }
    std::sort(key.begin(), key.end());
    return key;
}

// RB-18: identity of the rendered catalog content, excluding the capture
// timestamp/generation. The catalog worker refreshes every 15s; a snapshot whose
// displayed content is unchanged must not reach the UI thread (and repaint) while
// the History overlay is closed. Compared on the worker thread only.
std::string catalog_key(const SessionCatalogSnapshot& snapshot) {
    std::string key = snapshot.complete ? "1" : "0";
    for (const WorkspaceHistory& workspace : snapshot.workspaces) {
        key += "\nW" + workspace.id.value + "|" + workspace.title + "|" +
               workspace.canonicalPath + "|" + (workspace.live ? "1" : "0") + "|" +
               (workspace.note.has_value() ? *workspace.note : std::string{});
        for (const SessionHistoryEntry& session : workspace.sessions) {
            key += "\nS" + session.id.value + "|" + session.title + "|" + session.kind +
                   "|" + session.model + "|" + std::to_string(session.createdAt) + "|" +
                   std::to_string(session.updatedAt) + "|" +
                   (session.parent.has_value() ? session.parent->value : std::string{}) +
                   "|";
            if (session.seedLength.has_value()) {
                key += std::to_string(*session.seedLength);
            }
        }
    }
    return key;
}

// 22 §5.1 (S3): one coalesced lazy-spawn request for the ensure worker.
struct EnsureRequest {
    WorkspaceId workspace;
    SessionId   resume;
};

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

// 18 §4.3: the daemon's `context.show` reply is parsed into the agent snapshot
// value type; a malformed reply is surfaced as a UI-local note, never a crash.
bool parse_context_snapshot(const nlohmann::json& json, ContextSnapshot& out) {
    try {
        out = json.get<ContextSnapshot>();
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
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

std::string trim_command_arg(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string collapse_to_single_line(std::string value) {
    for (char& character : value) {
        if (character == '\n' || character == '\r' || character == '\t') {
            character = ' ';
        }
    }
    return value;
}

std::string pad_field(const std::string& value, std::size_t width) {
    if (value.size() >= width) {
        return value;
    }
    return value + std::string(width - value.size(), ' ');
}

std::string join_json_strings(const nlohmann::json& array) {
    std::string joined;
    for (const auto& item : array) {
        if (!item.is_string()) {
            continue;
        }
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += item.get<std::string>();
    }
    return joined;
}

std::string format_skills_listing(const nlohmann::json& result) {
    const nlohmann::json skills = result.value("skills", nlohmann::json::array());
    const nlohmann::json warnings = result.value("warnings", nlohmann::json::array());
    const std::string    note = result.value("note", std::string{});
    std::string          out;
    if (!note.empty()) {
        out += "note: " + note + "\n";
    }
    if (!skills.is_array() || skills.empty()) {
        out += "no skills found.\n";
        out += "add a skill at <workspace>/.ymh/skills/<name>/SKILL.md\n";
        out += "or at ~/.config/ymh/skills/<name>/SKILL.md (trusted).\n";
        out += "a skill file starts with '---', then 'name:' and 'description:'.\n";
    } else {
        out += "skills (" + std::to_string(skills.size()) + "):\n";
        for (const auto& skill : skills) {
            const std::string name = skill.value("name", std::string{});
            const std::string trust = skill.value("trust", std::string{});
            const std::string description =
                collapse_to_single_line(skill.value("description", std::string{}));
            const std::string tier = trust == "trusted" ? "[user]" : "[workspace]";
            out += "  " + pad_field(name, 18) + pad_field(tier, 13) + description + "\n";
        }
        out += "skills are offered to the model automatically; detail with /skills --show <name>\n";
    }
    if (warnings.is_array() && !warnings.empty()) {
        out += std::to_string(warnings.size()) + " skills skipped:\n";
        for (const auto& warning : warnings) {
            out += "  " + warning.value("file", std::string{}) + ": " +
                   warning.value("reason", std::string{}) + "\n";
        }
    }
    return out;
}

std::string format_skill_detail(const nlohmann::json& result) {
    std::string out;
    out += "skill " + result.value("name", std::string{}) + "\n";
    out += "  trust:   " + result.value("trust", std::string{}) + "\n";
    out += "  source:  " + result.value("source", std::string{}) + "\n";
    out += "  version: " + std::to_string(result.value("version", 0)) + "\n";
    const nlohmann::json tags = result.value("tags", nlohmann::json::array());
    if (tags.is_array() && !tags.empty()) {
        out += "  tags:    " + join_json_strings(tags) + "\n";
    }
    const nlohmann::json tools = result.value("allowed_tools", nlohmann::json::array());
    if (tools.is_array() && !tools.empty()) {
        out += "  allowed-tools: " + join_json_strings(tools) + "\n";
    }
    out += "\n" + result.value("body", std::string{});
    return out;
}

struct AgentListEntry {
    std::string id;
    std::string display_name;
    bool        blank      = false;
    bool        can_select = false;
};

std::vector<AgentListEntry> parse_agent_entries(const nlohmann::json& result) {
    std::vector<AgentListEntry> entries;
    const nlohmann::json        agents = result.value("agents", nlohmann::json::array());
    if (!agents.is_array()) {
        return entries;
    }
    for (const nlohmann::json& agent : agents) {
        if (!agent.is_object()) {
            continue;
        }
        AgentListEntry entry;
        entry.id           = agent.value("id", std::string{});
        entry.display_name = agent.value("display_name", entry.id);
        entry.blank        = agent.value("blank", false);
        entry.can_select   = agent.value("can_select", false);
        if (!entry.id.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

bool bare_slash_prefix(const std::string& draft) {
    return !draft.empty() && draft.front() == '/' &&
           draft.find_first_of(" \t") == std::string::npos;
}

std::string next_agent_id(const std::vector<AgentListEntry>& entries, const std::string& base,
                          int delta) {
    if (entries.empty()) {
        return {};
    }
    const std::size_t count = entries.size();
    std::size_t       index = count;
    for (std::size_t i = 0; i < count; ++i) {
        if (entries[i].id == base) {
            index = i;
            break;
        }
    }
    if (index == count) {
        return delta < 0 ? entries.back().id : entries.front().id;
    }
    const std::size_t next = delta < 0 ? (index + count - 1) % count : (index + 1) % count;
    return entries[next].id;
}

class SupervisorApp final : public UiController {
public:
    explicit SupervisorApp(SupervisorRunOptions options)
        : options_(std::move(options)), adapter_(model_), registry_(CommandRegistry::builtin()) {
        model_.aggregate.flash.enabled = true;
    }

    ~SupervisorApp() override {
        // 22 §4.2 (SW26, MEDIUM-2): stop+join the catalog worker first, before
        // any member teardown. Its sink calls `enqueue`, which locks
        // `action_mutex_` and reads `screen_`; letting it outlive them would be
        // a use-after-free.
        if (catalog_ != nullptr) {
            catalog_->stop();
        }
        // 22 §5.1 (SW24, H2): stop+join the spawn worker before any member
        // teardown. The worker checks `stop_requested()` before `enqueue`, so it
        // never touches `this` after the stop request.
        //
        // `request_stop()` must run under `ensure_mutex_`: the worker evaluates
        // its wait predicate (`stop_requested() || !ensure_requests_.empty()`)
        // while holding that mutex, so mutating `stop_requested()` outside it
        // races the predicate check. A notify that lands after the check but
        // before the block is lost and never re-sent, hanging `join()` forever.
        {
            std::lock_guard lock(ensure_mutex_);
            ensure_worker_.request_stop();
        }
        ensure_cv_.notify_all();
        if (ensure_worker_.joinable()) {
            ensure_worker_.join();
        }
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

        // 22 §6.1 (S4): seed the resume before the FTXUI loop drains any
        // `on_link_state(Attached)` action, so it is consumed by the shared S3
        // path (`on_link_state` -> `resume_after_attach`).
        if (options_.initial_resume.has_value()) {
            pending_resume_[options_.initial_resume->first] = options_.initial_resume->second;
        }

        register_presence();
        start_scanner();
        start_catalog();
        ensure_worker_ = std::jthread([this](std::stop_token stop) { ensure_worker_loop(stop); });
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
        SessionUiState* state = model_.session(workspace->activeSessionId());
        if (state != nullptr) {
            state->input.push_history(text);
            state->input.draft.clear();
            state->input.cursor = 0;
            state->input.saved_draft.clear();
            state->command_hints.clear();
            state->command_hint_selected = 0;
            model_.dirty.mark(workspace->activeSessionId(), UiDirtyFlag::Input);
        }
        const WorkspaceId workspace_id = workspace->id;
        if (workspace->activeSessionId().value.empty()) {
            create_session(workspace_id, text);
            return;
        }
        prompt(workspace_id, workspace->activeSessionId(), text);
    }

    void cancelActive() override {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
            return;
        }
        nlohmann::json params{{"session", workspace->activeSessionId().value}};
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

    void requestExit() override { begin_exit(/*allow_prompt=*/false); }

private:
    // 22 §10.1/§10.2: the additive test seam (`SupervisorHarnessImpl`) needs the
    // private spawn/catalog/eviction surface to make the deferred pinned tests
    // real; production never uses it.
    friend class SupervisorHarnessImpl;

    // RB-12: the composer/input state captured when a modal opens.
    struct ModalComposerSnapshot {
        SessionId                session;
        InputModel               input;
        std::vector<CommandHint> command_hints;
        std::size_t              command_hint_selected = 0;
    };

    // 16 §4.2 (16-D9). The supervisor stays registered until the user confirms:
    // query the orphaning set read-only, then either exit (empty set), auto-
    // confirm (--yes), or open the prompt. Ctrl+D and `/exit` stay thin callers.
    void begin_exit(bool allow_prompt) {
        if (quit_.load() || model_.exitConfirm.open) {
            return;
        }
        const std::vector<WorkspaceId> orphaning = compute_orphaning_set();
        if (orphaning.empty() || !allow_prompt || options_.no_prompt) {
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
        model_.exitConfirm.selected = 0;
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
            event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
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
            model.live = false;
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
                if (envelope.event.type == EventType::AssistantMessage) {
                    refresh_status_context(workspace_id, envelope.session);
                }
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
        auto last_scan = std::make_shared<std::vector<std::string>>();
        scanner_ = std::make_unique<DaemonSetScanner>(
            *options_.registry, options_.scan_interval,
            [this, last_scan](std::vector<SupervisorWorkspace> live) {
                std::vector<std::string> key = live_scan_key(live);
                if (key == *last_scan) {
                    return;
                }
                on_scan(std::move(live));
                *last_scan = std::move(key);
            });
        scanner_->start();
    }

    // 22 §4.2 (S2): one owned worker reads every registered workspace's
    // `sessions.db` and posts immutable snapshots back to the UI thread through
    // `enqueue`. Requires a registry; without one `/sessions` has no source.
    void start_catalog() {
        if (options_.registry == nullptr) {
            return;
        }
        auto last_key = std::make_shared<std::string>();
        catalog_ = std::make_unique<SessionCatalogReader>(
            *options_.registry,
            [this, last_key](SessionCatalogSnapshot snapshot) {
                std::string key = catalog_key(snapshot);
                if (key == *last_key && !catalog_visible_.load()) {
                    return;
                }
                enqueue([this, snapshot = std::move(snapshot)]() mutable {
                    on_catalog_snapshot(std::move(snapshot));
                });
                *last_key = std::move(key);
            },
            options_.catalog_refresh_interval);
        catalog_->start();
    }

    // 22 §4.6 (S2): UI thread. Store the snapshot and rebuild the History
    // overlay in place when it is open, preserving `filter`/`collapsed` and
    // never closing the overlay (LOW-3). A stale snapshot is displayed as-is
    // until a newer one arrives.
    void on_catalog_snapshot(SessionCatalogSnapshot snapshot) {
        if (model_.catalog.loaded && snapshot.generation < model_.catalog.generation) {
            return;
        }
        model_.catalog.workspaces = std::move(snapshot.workspaces);
        model_.catalog.loaded = true;
        model_.catalog.complete = snapshot.complete;
        model_.catalog.capturedAtMs = snapshot.capturedAtMs;
        model_.catalog.nowMs = epoch_ms(options_.wall_clock());
        model_.catalog.generation = snapshot.generation;
        if (model_.mode == UiMode::Switcher &&
            model_.switcher.source == SwitcherSource::History) {
            model_.switcher.openHistory(model_);
        } else if (model_.mode == UiMode::Switcher &&
                   model_.switcher.source == SwitcherSource::Live) {
            resnapshot_switcher();
        }
        model_.dirty.markAggregate();
    }

    // 22 §4.3 (S2): `/sessions` opens the History source and requests an
    // immediate rebuild. The loading placeholder shows until the first snapshot.
    void open_sessions() {
        model_.switcher.source = SwitcherSource::History;
        model_.switcher.openHistory(model_);
        model_.mode = UiMode::Switcher;
        catalog_visible_.store(true);
        model_.dirty.markAggregate();
        if (catalog_ != nullptr) {
            catalog_->refreshNow();
        }
    }

    void push_notice(std::string text) { model_.pushNotice(std::move(text)); }

    // 46-D3: true when the Live switcher has at least one actionable target:
    // another live-renderable workspace, or a visible session leaf in the active
    // workspace after the focused-session exclusion.
    [[nodiscard]] bool switcher_has_targets() const {
        for (const auto& [id, workspace] : model_.workspaces) {
            if (id != model_.activeWorkspaceId && live_switcher_renderable(workspace)) {
                return true;
            }
        }
        const auto active = model_.workspaces.find(model_.activeWorkspaceId);
        if (active == model_.workspaces.end()) {
            return false;
        }
        const SessionId focused = active->second.activeSessionId();
        for (const SessionCell& cell : active->second.sessions) {
            if (!focused.value.empty() && cell.id == focused) {
                continue;
            }
            if (model_.catalog_has_session(active->first, cell.id)) {
                return true;
            }
        }
        return false;
    }

    // 46-D3: opens the notice instead of the switcher when there is no target.
    void openSwitcher() {
        if (!switcher_has_targets()) {
            model_.message.text = "No other workspaces available";
            model_.message.open = true;
            model_.mode = UiMode::Notice;
            model_.dirty.markAggregate();
            return;
        }
        model_.openSwitcher();
    }

    // 46-D3: Enter/Esc/Ctrl+C dismiss; every other key is swallowed.
    bool handle_notice(const ftxui::Event& event) {
        if (event == ftxui::Event::Return || event == ftxui::Event::Escape ||
            event == ftxui::Event::CtrlC) {
            model_.message.open = false;
            model_.mode = UiMode::Conversation;
            model_.dirty.markAggregate();
        }
        return true;
    }

    // 22 §3.3: the switcher overlay is a snapshot. Rebuild it in place (not via
    // `openSwitcher()`, so `mode`/`source` survive) whenever the live set
    // changes while a Live-source switcher is open.
    void resnapshot_switcher() {
        if (model_.mode == UiMode::Switcher &&
            model_.switcher.source == SwitcherSource::Live) {
            model_.switcher.open(model_);
        }
    }

    // 22 §5.1 (S3, H1/SW23): failure notices never inject a workspace. A modeled
    // session reuses the ErrorOccurred path (a Role::System entry); an unmodeled
    // one (the primary S3 case) goes to the workspace-independent notice ring.
    void surface_notice(const WorkspaceId& workspace, const SessionId& session,
                        std::string text) {
        (void)workspace;
        text = redact_secrets(text);
        if (model_.sessions.count(session) != 0) {
            model_.apply(UiEvent{ErrorOccurred{session, std::move(text)}});
            return;
        }
        model_.pushNotice(std::move(text));
    }

    // 22 §5.1 (S3, SW12): called on the UI thread. Records the last selection for
    // the workspace and queues a coalesced spawn request for the owned worker;
    // `ensureRunning` blocks and must never run here.
    void ensure_workspace_running(const WorkspaceId& workspace, const SessionId& resume) {
        if (options_.lifecycle == nullptr) {
            surface_notice(workspace, resume, "cannot start workspace");
            return;
        }
        pending_resume_[workspace] = resume;
        if (ensure_in_flight_.count(workspace) != 0) {
            return;
        }
        ensure_in_flight_.insert(workspace);
        {
            std::lock_guard lock(ensure_mutex_);
            std::erase_if(ensure_requests_, [&](const EnsureRequest& request) {
                return request.workspace == workspace;
            });
            ensure_requests_.push_back(EnsureRequest{workspace, resume});
        }
        ensure_cv_.notify_all();
    }

    void ensure_worker_loop(std::stop_token stop) {
        while (!stop.stop_requested()) {
            EnsureRequest request;
            {
                std::unique_lock lock(ensure_mutex_);
                ensure_cv_.wait(lock, [&] {
                    return stop.stop_requested() || !ensure_requests_.empty();
                });
                if (stop.stop_requested()) {
                    return;
                }
                request = ensure_requests_.front();
                ensure_requests_.pop_front();
            }

            std::optional<SupervisorWorkspace> spec;
            std::string                        notice;
            try {
                if (options_.lifecycle == nullptr) {
                    notice = "cannot start workspace";
                } else if (options_.registry != nullptr &&
                           !options_.registry->findById(request.workspace).has_value()) {
                    notice = "workspace no longer registered";
                } else {
                    AttachResult attach = options_.lifecycle->ensureRunning(
                        WorkspaceId{request.workspace.value}, options_.identity);
                    (void)attach;   // dropped (RAII close); `attach_workspace` makes
                                    // its own connection. The supervisor presence
                                    // keeps the daemon owned.
                    spec = workspace_spec_from_registry(request.workspace);
                    if (!spec.has_value()) {
                        notice = "cannot start workspace: daemon did not register";
                    }
                }
            } catch (const std::exception& error) {
                notice = "cannot start workspace: " + std::string{error.what()};
            }

            if (stop.stop_requested()) {
                return;   // (H2) never touch `this` after a stop request
            }
            enqueue([this, request, spec, notice = std::move(notice)] {
                ensure_in_flight_.erase(request.workspace);
                if (!spec.has_value()) {
                    pending_resume_.erase(request.workspace);
                    surface_notice(request.workspace, request.resume, notice);
                    return;
                }
                attach_workspace(*spec);
            });
        }
    }

    // 22 §5.2 (S3, SW13): the single S3/S4 entry point. An attached workspace
    // resumes immediately; a non-live one is spawned and resumed on `Attached`.
    void resume_from_history(const WorkspaceId& workspace, const SessionId& session) {
        const auto connection = connections_.find(workspace);
        if (connection != connections_.end() &&
            connection->second->state() == SupervisorLinkState::Attached) {
            resume_after_attach(workspace, session);
            return;
        }
        ensure_workspace_running(workspace, session);
    }

    void resume_after_attach(const WorkspaceId& workspace, const SessionId& session) {
        submit_to(workspace, std::string(protocol::method::kSessionResume),
                  nlohmann::json{{"session", session.value}},
                  [this, workspace, session](SupervisorReply reply) {
                      if (!reply.ok) {
                          // 22 §5.3 (SW-F3): a stale/unknown stored row yields a
                          // definitive `UnknownSession`; report it distinctly from
                          // a transport failure.
                          const bool unknown =
                              reply.error_code ==
                              static_cast<int>(protocol::AppCode::UnknownSession);
                          enqueue([this, workspace, session, error = reply.error, unknown] {
                              if (unknown) {
                                  recover_unknown_session(workspace, session);
                                  return;
                              }
                              surface_notice(workspace, session, "resume failed: " + error);
                          });
                          return;
                      }
                      enqueue([this, workspace, session] {
                          apply_resume_success(workspace, session);
                          refresh_status_context(workspace, session);
                      });
                  });
    }

    // The stored header's model (the value `/sessions` shows), falling back to
    // the effective config model when the catalog has not delivered the row yet.
    std::string stored_session_model(const WorkspaceId& workspace,
                                     const SessionId& session) const {
        for (const WorkspaceHistory& history : model_.catalog.workspaces) {
            if (history.id != workspace) {
                continue;
            }
            for (const SessionHistoryEntry& entry : history.sessions) {
                if (entry.id == session && !entry.model.empty()) {
                    return entry.model;
                }
            }
            break;
        }
        return effective_model(options_.config);
    }

    // 22 §5.2 (SW25, MEDIUM-1): the success branch must never inject a workspace.
    // A workspace evicted between the submit and its reply is reported through
    // `surface_notice` and left unmodeled.
    void apply_resume_success(const WorkspaceId& workspace, const SessionId& session) {
        if (model_.workspaces.count(workspace) == 0) {
            surface_notice(workspace, session,
                           "session resumed in a workspace that is no longer open");
            return;
        }
        SessionUiState& state = model_.ensureSessionIn(workspace, session);
        // Hydrate the display model here: a stored session resumed into a fresh
        // process is unmodeled, and `refresh_sessions` is not guaranteed to run
        // for it (it only hydrates sessions the daemon reports as `live`).
        if (state.status.model.empty()) {
            state.status.model = stored_session_model(workspace, session);
        }
        model_.ensureCellIn(workspace, session);
        // Subscribe here, not only via the `SessionCreated` notice: that notice
        // refreshes `session.list`, which tracks only daemon-reported `live`
        // sessions. A daemon predating that flag leaves the resume unsubscribed.
        if (const auto connection = connections_.find(workspace);
            connection != connections_.end()) {
            connection->second->track(session);
        }
        // 22 §5.2 "focus only; no session.activate": `focusSession` sets the
        // workspace's active session and switches `activeWorkspaceId`, so a
        // session selected in another workspace becomes the visible one.
        model_.focusSession(session);
    }

    // 45-D10.8 (45-F23): drop a daemon-rejected session (which clears the focus
    // through the single mutator), report it, then focus the workspace's first
    // remaining session or create one when none remain, so the focus never
    // dangles on a session the daemon will reject.
    void recover_unknown_session(const WorkspaceId& workspace, const SessionId& session) {
        model_.eraseSession(workspace, session);
        push_notice("session no longer exists");
        const auto it = model_.workspaces.find(workspace);
        if (it == model_.workspaces.end()) {
            return;
        }
        if (!it->second.sessions.empty()) {
            model_.focusSessionIn(workspace, it->second.sessions.front().id);
        } else {
            create_session(workspace, std::string{});
        }
    }

    std::optional<SupervisorWorkspace> workspace_spec_from_registry(
        const WorkspaceId& workspace) const {
        if (options_.registry == nullptr) {
            return std::nullopt;
        }
        const std::optional<WorkspaceRecord> row = options_.registry->findById(workspace);
        if (!row.has_value() || !row->host.has_value()) {
            return std::nullopt;
        }
        SupervisorWorkspace spec;
        spec.id          = WorkspaceId{row->id.value};
        spec.cwd         = row->canonicalPath.string();
        spec.title       = row->displayTitle;
        spec.socket_path = row->host->socketPath.string();
        spec.boot_id     = row->host->bootId.value;
        return spec;
    }

    void on_scan(std::vector<SupervisorWorkspace> live) {
        enqueue([this, live = std::move(live)] {
            std::set<WorkspaceId> live_ids;
            for (const SupervisorWorkspace& spec : live) {
                live_ids.insert(spec.id);
            }
            for (const SupervisorWorkspace& spec : live) {
                const auto connection = connections_.find(spec.id);
                const auto pinned = specs_.find(spec.id);
                if (connection == connections_.end() || pinned == specs_.end()) {
                    continue;
                }
                if (pinned->second.boot_id != spec.boot_id) {
                    connection->second->stop();
                    connections_.erase(connection);
                    specs_.erase(pinned);
                    model_.eraseWorkspace(spec.id);
                }
            }
            for (const SupervisorWorkspace& spec : live) {
                attach_workspace(spec);
            }
            for (const WorkspaceId& id : live_ids) {
                const auto it = model_.workspaces.find(id);
                const auto connection = connections_.find(id);
                if (it != model_.workspaces.end() && connection != connections_.end() &&
                    connection->second->state() == SupervisorLinkState::Attached) {
                    it->second.live = true;
                }
            }
            if (model_.mode != UiMode::ExitConfirm) {
                evict_dead_workspaces(live_ids);
            }
            resnapshot_switcher();
        });
    }

    void evict_dead_workspaces(const std::set<WorkspaceId>& live_ids) {
        std::set<WorkspaceId> connecting_ids;
        for (const auto& [id, connection] : connections_) {
            if (connection->state() == SupervisorLinkState::Connecting) {
                connecting_ids.insert(id);
            }
        }
        std::vector<WorkspaceId> doomed =
            switcher_eviction_candidates(model_.workspaces, live_ids, connecting_ids);
        // 22 §3.2 rule 8 (M5, SW21): a scan tick landing during an in-flight
        // spawn must not erase the workspace or its `pending_resume_`; the later
        // `Attached` still consumes it.
        std::erase_if(doomed, [this](const WorkspaceId& id) {
            return ensure_in_flight_.count(id) != 0;
        });
        for (const WorkspaceId& id : doomed) {
            if (auto connection = connections_.find(id); connection != connections_.end()) {
                connection->second->stop();
                connections_.erase(connection);
            }
            specs_.erase(id);
            pending_creates_.erase(id);
            pending_resume_.erase(id);
            model_.eraseWorkspace(id);
        }
        if (!doomed.empty()) {
            model_.dirty.markAggregate();
        }
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
            apply_daemon_status_liveness(it->second, it->second.daemonStatus);
            model_.dirty.markAggregate();
            if (state == SupervisorLinkState::Attached) {
                refresh_sessions(workspace);
                // 25 review M3: a plain re-attach does not re-activate, so
                // refresh the status context for the already-active session.
                if (!it->second.activeSessionId().value.empty()) {
                    refresh_status_context(workspace, it->second.activeSessionId());
                }
                // 22 §5.1 (S3/S4, SW14/SW15): consume the pending resume exactly
                // once per successful attach through the shared resume path.
                const auto pending = pending_resume_.find(workspace);
                if (pending != pending_resume_.end()) {
                    const SessionId session = pending->second;
                    pending_resume_.erase(pending);
                    resume_after_attach(workspace, session);
                }
            }
            resnapshot_switcher();
        });
    }

    void activate_session(const WorkspaceId& workspace, const SessionId& session) {
        model_.focusSessionIn(workspace, session);
    }

    // The Live switcher's session source is the daemon's OPEN/LIVE set, never
    // stored-but-closed history. `session.list` still enumerates every stored
    // session (its pinned contract) and tags each with a `live` flag; only live
    // entries become cells and are subscribed. The focus/create/resume decision
    // is scoped to the user's ACTIVE workspace: a background workspace is only
    // observed (its live sessions render as leaves) and never steals focus. For
    // the active workspace with no open session but stored history, the first
    // stored session is resumed (the History selection path) so the focus is
    // always a usable, live session.
    void refresh_sessions(const WorkspaceId& workspace) {
        const auto connection = connections_.find(workspace);
        if (connection == connections_.end()) {
            return;
        }
        connection->second->submit(
            std::string(protocol::method::kSessionList), nlohmann::json::object(),
            [this, workspace](SupervisorReply reply) {
                std::vector<std::pair<SessionId, std::string>> stored;
                std::vector<std::pair<SessionId, std::string>> live;
                if (reply.ok && reply.result.is_array()) {
                    for (const nlohmann::json& entry : reply.result) {
                        const std::string id = entry.value("id", std::string{});
                        if (id.empty()) {
                            continue;
                        }
                        const std::string title = entry.value("title", std::string{});
                        stored.emplace_back(SessionId{id}, title);
                        if (entry.value("live", false)) {
                            live.emplace_back(SessionId{id}, title);
                        }
                    }
                }
                const auto connection_it = connections_.find(workspace);
                if (connection_it != connections_.end()) {
                    for (const auto& entry : live) {
                        connection_it->second->track(entry.first);
                    }
                }
                enqueue([this, workspace, stored, live] {
                    const auto it = model_.workspaces.find(workspace);
                    if (it == model_.workspaces.end()) {
                        return;
                    }
                    for (const auto& [session, title] : live) {
                        SessionUiState& state = model_.ensureSessionIn(workspace, session);
                        if (state.status.model.empty()) {
                            state.status.model = effective_model(options_.config);
                        }
                        model_.ensureCellIn(workspace, session);
                        model_.setCellTitle(workspace, session, title);
                    }
                    if (it->second.activeSessionId().value.empty() &&
                        model_.activeWorkspaceId == workspace) {
                        if (!live.empty()) {
                            // A live session can be focused directly.
                            activate_session(workspace, live.front().first);
                            // 25 review M3: plain attach must refresh the context.
                            refresh_status_context(workspace, live.front().first);
                        } else if (!stored.empty()) {
                            resume_after_attach(workspace, stored.front().first);
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
        if (!preferred_agent_.empty()) {
            create_params["agent_preset"] = preferred_agent_;
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
                enqueue([this, workspace, session, error = reply.error] {
                    apply_create_reply(workspace, SessionId{session}, error);
                });
            });
    }

    // RB-15 (22 §5.2/SW25 generalised): the `session.create` reply must never
    // inject a workspace. A workspace evicted between the submit and its reply
    // is reported through `surface_notice` and left unmodeled; a create failure
    // is likewise surfaced there rather than being swallowed.
    void apply_create_reply(const WorkspaceId& workspace, const SessionId& session,
                            std::string error) {
        std::string queued;
        const auto pending_it = pending_creates_.find(workspace);
        if (pending_it != pending_creates_.end()) {
            queued = pending_it->second;
            pending_creates_.erase(pending_it);
        }
        if (model_.workspaces.count(workspace) == 0) {
            surface_notice(
                workspace, session,
                session.value.empty()
                    ? "cannot create session: workspace is no longer open"
                    : "session created in a workspace that is no longer open");
            return;
        }
        if (session.value.empty()) {
            surface_notice(workspace, session,
                           error.empty() ? "cannot create session"
                                         : "cannot create session: " + error);
            return;
        }
        SessionUiState& state = model_.ensureSessionIn(workspace, session);
        if (state.status.model.empty()) {
            state.status.model = effective_model(options_.config);
        }
        model_.ensureCellIn(workspace, session);
        activate_session(workspace, session);
        // 25 review M3: a newly created session must refresh the context too.
        refresh_status_context(workspace, session);
        if (!queued.empty()) {
            prompt(workspace, session, queued);
        }
        // 17 §6 (RB-10): re-list so the created session's title ("tui") populates
        // its cell via the pinned setCellTitle path.
        refresh_sessions(workspace);
    }

    void prompt(const WorkspaceId& workspace, const SessionId& session, const std::string& text) {
        submit_to(workspace, std::string(protocol::method::kAgentPrompt),
                  nlohmann::json{{"session", session.value}, {"message", text}},
                  [this, workspace, session](SupervisorReply reply) {
                      if (reply.ok ||
                          reply.error_code !=
                              static_cast<int>(protocol::AppCode::UnknownSession)) {
                          return;
                      }
                      enqueue([this, workspace, session] {
                          recover_unknown_session(workspace, session);
                      });
                  });
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
        animation_active_.store(model_.aggregate.flash.isFlashing() ||
                                model_.has_streaming_reasoning() ||
                                model_.has_active_turn());
    }

    SessionUiState* active() {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr) {
            return nullptr;
        }
        return model_.session(workspace->activeSessionId());
    }

    // RB-12: true while an input-blocking modal owns the keyboard. The
    // permission dialog and the exit prompt resolve on a bare keystroke; the
    // switcher and the context overlay capture every key while they are up.
    // Printable input must never reach the composer in any of these states.
    [[nodiscard]] bool modal_owns_input() const {
        return model_.exitConfirm.open || model_.dialog.open ||
               (model_.mode == UiMode::Context && model_.context.open) ||
               model_.message.open || model_.mode == UiMode::Switcher;
    }

    // RB-12: composer snapshot taken when a modal opens, restored when it
    // closes. Text typed before the modal opened survives; anything that
    // reached the composer while the modal was up is discarded.
    void capture_modal_composer() {
        if (modal_composer_snapshot_.has_value()) {
            return;
        }
        SessionUiState* state = active();
        if (state == nullptr) {
            return;
        }
        modal_composer_snapshot_ =
            ModalComposerSnapshot{state->id, state->input, state->command_hints,
                                  state->command_hint_selected};
    }

    void release_modal_composer() {
        if (!modal_composer_snapshot_.has_value()) {
            return;
        }
        ModalComposerSnapshot snapshot = std::move(*modal_composer_snapshot_);
        modal_composer_snapshot_.reset();
        // RB-12: the keystroke that closed the modal is consumed by the modal,
        // but a paste/burst continues after it. Anchor a short wall-clock window
        // at the close instant so the tail is dropped; because it expires by
        // time (not by a render), it can never leave the composer deaf (RB-18).
        modal_closed_at_ = std::chrono::steady_clock::now();
        SessionUiState* state = model_.session(snapshot.session);
        if (state == nullptr) {
            return;
        }
        state->input = std::move(snapshot.input);
        state->command_hints = std::move(snapshot.command_hints);
        state->command_hint_selected = snapshot.command_hint_selected;
        model_.dirty.mark(state->id, UiDirtyFlag::Input);
    }

    void sync_modal_composer() {
        if (modal_owns_input()) {
            capture_modal_composer();
        } else {
            release_modal_composer();
        }
    }

    // RB-12/RB-18: true only during the short window after a modal closed in
    // which the tail of the resolving burst may still arrive.
    [[nodiscard]] bool modal_tail_suppression_active() const {
        if (!modal_closed_at_.has_value()) {
            return false;
        }
        return std::chrono::steady_clock::now() - *modal_closed_at_ < kModalTailWindow;
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

    // 46-D13: hands the composer to $EDITOR and copies the result back. The
    // terminal is restored for the child; a non-zero status leaves the draft
    // untouched and surfaces a notice.
    void edit_prompt(SessionUiState& state) {
        if (screen_ == nullptr && !with_restored_io_) {
            push_notice("editor unavailable");
            return;
        }
        std::optional<std::string> edited;
        std::string                error;
        const auto run = [&] { edited = edit_text_in_editor(state.input.draft, error); };
        if (with_restored_io_) {
            with_restored_io_(run);
        } else {
            screen_->WithRestoredIO(run)();
        }
        if (!edited.has_value()) {
            push_notice(error.empty() ? std::string("editor failed") : error);
            return;
        }
        state.input.draft = *edited;
        state.input.cursor = state.input.draft.size();
        state.hints_dismissed = false;
        refresh_hints(state);
        model_.dirty.mark(state.id, UiDirtyFlag::Input);
    }

    // /export: reads the active session's durable event log from the workspace
    // store, renders markdown, writes it under the workspace root, and optionally
    // opens the editor with the terminal temporarily restored.
    std::string export_session(const std::string& args) {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
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

        const SessionId                session = workspace->activeSessionId();
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

    SessionUiState* active_session_of(const WorkspaceId& id) {
        const auto it = model_.workspaces.find(id);
        if (it == model_.workspaces.end()) {
            return nullptr;
        }
        return model_.session(it->second.activeSessionId());
    }

    void request_skills(const std::string& args) {
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
            return;
        }
        const WorkspaceId id = workspace->id;
        const std::string trimmed = trim_command_arg(args);
        if (trimmed.rfind("--show", 0) == 0) {
            const std::string name = trim_command_arg(trimmed.substr(6));
            if (name.empty()) {
                if (SessionUiState* state = model_.session(workspace->activeSessionId());
                    state != nullptr) {
                    append_system_entry(model_, *state, "usage: /skills --show <name>");
                }
                return;
            }
            submit_to(id, std::string(protocol::method::kSkillsShow),
                      nlohmann::json{{"name", name}},
                      [this, id, name](SupervisorReply reply) {
                          enqueue([this, id, name, reply = std::move(reply)] {
                              append_skill_detail(id, name, reply);
                          });
                      });
            return;
        }
        submit_to(id, std::string(protocol::method::kSkillsList), nlohmann::json::object(),
                  [this, id](SupervisorReply reply) {
                      enqueue([this, id, reply = std::move(reply)] {
                          append_skills_reply(id, reply);
                      });
                  });
    }

    void append_skills_reply(const WorkspaceId& id, const SupervisorReply& reply) {
        SessionUiState* state = active_session_of(id);
        if (state == nullptr) {
            return;
        }
        if (!reply.ok) {
            append_system_entry(model_, *state,
                                "skills: " + (reply.error.empty() ? std::string{"request failed"}
                                                                  : reply.error));
            return;
        }
        append_system_entry(model_, *state, format_skills_listing(reply.result));
    }

    void append_skill_detail(const WorkspaceId& id, const std::string& name,
                             const SupervisorReply& reply) {
        SessionUiState* state = active_session_of(id);
        if (state == nullptr) {
            return;
        }
        if (!reply.ok) {
            append_system_entry(model_, *state, "skills: unknown skill '" + name + "'");
            return;
        }
        append_system_entry(model_, *state, format_skill_detail(reply.result));
    }

    // 45-D6: `/mcp` issues `mcp.status` unconditionally (session-less) and
    // routes the block to the active session, else the notice ring. A daemon
    // that cannot serve the method disables the surface for the rest of the
    // process (45-F19: no retry loop).
    void request_mcp() {
        if (mcp_disabled_) {
            model_.pushNotice(*mcp_unavailable_notice(
                static_cast<int>(protocol::RpcCode::MethodNotFound)));
            model_.dirty.markAggregate();
            return;
        }
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr) {
            model_.pushNotice("mcp: no active workspace");
            model_.dirty.markAggregate();
            return;
        }
        const WorkspaceId id = workspace->id;
        submit_to(id, std::string(protocol::method::kMcpStatus), nlohmann::json::object(),
                  [this, id](SupervisorReply reply) {
                      enqueue([this, id, reply = std::move(reply)]() mutable {
                          if (model_.workspaces.count(id) == 0) {
                              return;
                          }
                          std::string block;
                          if (reply.ok) {
                              block = format_mcp_block(reply.result);
                          } else if (std::optional<std::string> notice =
                                         mcp_unavailable_notice(reply.error_code)) {
                              mcp_disabled_ = true;
                              block = std::move(*notice);
                          } else {
                              block = "mcp: " + (reply.error.empty()
                                                     ? std::string{"request failed"}
                                                     : reply.error);
                          }
                          if (SessionUiState* state = model_.ensureActiveSession()) {
                              append_system_entry(model_, *state, std::move(block));
                          } else {
                              model_.pushNotice(std::move(block));
                              model_.dirty.markAggregate();
                          }
                      });
                  });
    }

    void set_preferred_agent(std::string id) {
        preferred_agent_ = std::move(id);
        if (SessionUiState* state = active()) {
            state->status.pending_agent = preferred_agent_;
            model_.dirty.mark(state->id, UiDirtyFlag::Status);
        }
        model_.dirty.markAggregate();
    }

    void submit_agent(const WorkspaceId& workspace, std::string method, nlohmann::json params,
                      SupervisorConnection::ReplyFn reply) {
        if (!agent_replies_installed_) {
            submit_to(workspace, std::move(method), std::move(params), std::move(reply));
            return;
        }
        const bool list = method == protocol::method::kAgentList;
        SupervisorReply canned;
        canned.ok         = (list ? agent_list_error_ : agent_select_error_) == 0;
        canned.result     = list ? agent_list_reply_ : agent_select_reply_;
        canned.error_code = list ? agent_list_error_ : agent_select_error_;
        canned.error      = canned.ok ? std::string{} : "stubbed error";
        reply(std::move(canned));
    }

    // 45-D9: cycles `preferred_agent_`; issues `agent.select` ONLY when the
    // daemon's `can_select` is true (45-D9.5), never a local blank predicate.
    void cycle_agent(int delta) {
        if (agents_disabled_) {
            model_.pushNotice(agent_no_agents_notice());
            model_.dirty.markAggregate();
            return;
        }
        WorkspaceModel* workspace = model_.activeWorkspace();
        if (workspace == nullptr || workspace->id.value.empty()) {
            return;
        }
        SessionUiState* state   = active();
        const SessionId session = state != nullptr ? state->id : SessionId{};
        nlohmann::json  params  = nlohmann::json::object();
        if (!session.value.empty()) {
            params["session"] = session.value;
        }
        const WorkspaceId id = workspace->id;
        submit_agent(id, std::string(protocol::method::kAgentList), std::move(params),
                     [this, id, session, delta](SupervisorReply reply) {
                         enqueue([this, id, session, delta, reply = std::move(reply)]() mutable {
                             on_agent_list_reply(id, session, delta, std::move(reply));
                         });
                     });
    }

    void on_agent_list_reply(const WorkspaceId& workspace, const SessionId& session, int delta,
                             SupervisorReply reply) {
        if (model_.workspaces.count(workspace) == 0) {
            return;
        }
        if (!reply.ok) {
            if (agent_unavailable_notice(reply.error_code).has_value()) {
                agents_disabled_ = true;
                model_.pushNotice(agent_no_agents_notice());
                model_.dirty.markAggregate();
                return;
            }
            model_.pushNotice("agents: " + (reply.error.empty() ? std::string{"request failed"}
                                                                : reply.error));
            model_.dirty.markAggregate();
            return;
        }
        const std::vector<AgentListEntry> entries   = parse_agent_entries(reply.result);
        const std::string                 active_id = reply.result.value("active", std::string{});
        if (entries.empty()) {
            if (!agent_empty_noticed_) {
                agent_empty_noticed_ = true;
                model_.pushNotice(agent_no_agents_notice());
                model_.dirty.markAggregate();
            }
            return;
        }
        if (SessionUiState* state = model_.session(session)) {
            state->status.agent = active_id;
            model_.dirty.mark(state->id, UiDirtyFlag::Status);
        }
        const std::string base = preferred_agent_.empty() ? active_id : preferred_agent_;
        const std::string next = next_agent_id(entries, base, delta);
        bool              any_can_select = false;
        bool              has_other      = false;
        for (const AgentListEntry& entry : entries) {
            any_can_select = any_can_select || entry.can_select;
            has_other      = has_other || entry.id != active_id;
        }
        if (any_can_select) {
            if (session.value.empty()) {
                set_preferred_agent(next);
                return;
            }
            submit_agent(workspace, std::string(protocol::method::kAgentSelect),
                         nlohmann::json{{"session", session.value}, {"agent", next}},
                         [this, workspace, session, next](SupervisorReply select_reply) {
                             enqueue([this, workspace, session, next,
                                      select_reply = std::move(select_reply)]() mutable {
                                 on_agent_select_reply(workspace, session, next,
                                                       std::move(select_reply));
                             });
                         });
            return;
        }
        if (has_other) {
            set_preferred_agent(next);
            return;
        }
        if (!agent_no_others_noticed_) {
            agent_no_others_noticed_ = true;
            model_.pushNotice(agent_no_others_notice());
            model_.dirty.markAggregate();
        }
    }

    void on_agent_select_reply(const WorkspaceId& workspace, const SessionId& session,
                               const std::string& requested, SupervisorReply reply) {
        if (model_.workspaces.count(workspace) == 0) {
            return;
        }
        if (reply.ok) {
            const std::string active = reply.result.value("agent", requested);
            if (SessionUiState* state = model_.session(session)) {
                state->status.agent = active;
                state->status.pending_agent.clear();
                model_.dirty.mark(state->id, UiDirtyFlag::Status);
            }
            preferred_agent_.clear();
            return;
        }
        if (reply.error_code == static_cast<int>(protocol::AppCode::CompositionFixed)) {
            set_preferred_agent(requested);
            model_.pushNotice(agent_composition_fixed_notice());
            model_.dirty.markAggregate();
            return;
        }
        if (reply.error_code == static_cast<int>(protocol::AppCode::UnknownSession)) {
            enqueue([this, workspace, session] { recover_unknown_session(workspace, session); });
            return;
        }
        if (agent_unavailable_notice(reply.error_code).has_value()) {
            agents_disabled_ = true;
            set_preferred_agent(requested);
            model_.pushNotice(agent_no_agents_notice());
            model_.dirty.markAggregate();
            return;
        }
        model_.pushNotice("agents: " + (reply.error.empty() ? std::string{"request failed"}
                                                            : reply.error));
        model_.dirty.markAggregate();
    }

    // 45-D7: `/status` renders local lines plus the per-tool and per-server
    // block from `context.show` when a session exists; otherwise local lines
    // with a `(no session)` note go to the notice ring.
    void request_status() {
        WorkspaceModel* workspace = model_.activeWorkspace();
        StatusBlockInputs inputs;
        inputs.version = options_.version;
        inputs.model = effective_model(options_.config);
        if (workspace != nullptr) {
            inputs.daemon = workspace->daemonStatus;
            inputs.workspace_title = workspace->title;
        }
        SessionUiState* state = model_.ensureActiveSession();
        if (state == nullptr || workspace == nullptr) {
            model_.pushNotice(format_status_block(inputs));
            model_.dirty.markAggregate();
            return;
        }
        inputs.has_session = true;
        inputs.api_state = state->status.api_state;
        inputs.last_error = state->status.last_error;
        const WorkspaceId id = workspace->id;
        const SessionId   session = state->id;
        submit_to(id, std::string(protocol::method::kContextShow),
                  nlohmann::json{{"session", session.value}},
                  [this, id, session, inputs](SupervisorReply reply) mutable {
                      ContextSnapshot snapshot;
                      const bool loaded = reply.ok && parse_context_snapshot(reply.result, snapshot);
                      enqueue([this, id, session, inputs = std::move(inputs), loaded,
                               snapshot = std::move(snapshot)]() mutable {
                          if (model_.workspaces.count(id) == 0) {
                              return;
                          }
                          SessionUiState* target = model_.session(session);
                          if (target == nullptr) {
                              return;
                          }
                          if (loaded) {
                              inputs.snapshot = &snapshot;
                          }
                          append_system_entry(model_, *target, format_status_block(inputs));
                      });
                  });
    }

    bool dispatch_command(const std::string& line) {
        CommandContext context{model_};
        context.session = model_.ensureActiveSession();
        context.request_exit = [this] { requestExit(); };
        context.create_session = [this] { new_session(); };
        context.set_model = [this](const std::string& name) { preferred_model_ = name; };
        context.compact = [this] {
            WorkspaceModel* workspace = model_.activeWorkspace();
            if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
                return;
            }
            nlohmann::json params{{"session", workspace->activeSessionId().value}};
            submit_to(workspace->id, std::string(protocol::method::kSessionCompact),
                      std::move(params), nullptr);
        };
        context.rename_session = [this](const std::string& title) {
            WorkspaceModel* workspace = model_.activeWorkspace();
            if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
                return;
            }
            const SessionId session = workspace->activeSessionId();
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
        context.skills = [this](const std::string& args) { request_skills(args); };
        context.plan_mode = [this](bool active, const std::string& message) {
            WorkspaceModel* workspace = model_.activeWorkspace();
            if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
                return;
            }
            const WorkspaceId id = workspace->id;
            const SessionId   session = workspace->activeSessionId();
            submit_to(id, std::string(protocol::method::kSessionSetMode),
                      nlohmann::json{{"session", session.value}, {"active", active}},
                      [this, id, session, message](SupervisorReply reply) {
                          enqueue([this, id, session, message,
                                   reply = std::move(reply)]() mutable {
                              if (!reply.ok) {
                                  if (SessionUiState* state = model_.session(session);
                                      state != nullptr) {
                                      append_system_entry(
                                          model_, *state,
                                          "plan: " + (reply.error.empty()
                                                          ? std::string{"request failed"}
                                                          : reply.error));
                                  }
                                  return;
                              }
                              if (reply.result.value("pending", false)) {
                                  model_.pushNotice("plan change queued");
                                  model_.dirty.markAggregate();
                              }
                              if (!message.empty()) {
                                  submit_to(id, std::string(protocol::method::kAgentSteer),
                                            nlohmann::json{{"session", session.value},
                                                           {"message", message}},
                                            nullptr);
                              }
                          });
                      });
        };
        context.context = [this] { open_context(); };
        context.sessions = [this] { open_sessions(); };
        context.mcp = [this] { request_mcp(); };
        context.status = [this] { request_status(); };
        return registry_.dispatch(line, context);
    }

    // 45-D1/45-D5: the list is navigable only while visible and not Esc-dismissed.
    bool command_list_active(const SessionUiState& state) const {
        return !state.command_hints.empty() && !state.hints_dismissed;
    }

    // 45-D5.2: while `hints_dismissed` is set the list stays hidden; only a
    // command-prefix edit clears the flag (handled by `handle_input`).
    void refresh_hints(SessionUiState& state) {
        state.command_hints.clear();
        state.command_hint_selected = 0;
        if (state.hints_dismissed) {
            return;
        }
        const std::string& draft = state.input.draft;
        if (draft.empty() || draft.front() != '/') {
            return;
        }
        const std::string prefix = draft.substr(1);
        if (prefix.find_first_of(" \t") != std::string::npos) {
            return;
        }
        for (const CompletionCandidate& candidate : registry_.complete_candidates(prefix)) {
            state.command_hints.push_back(CommandHint{candidate.command->name,
                                                      command_display_name(*candidate.command),
                                                      candidate.spelling,
                                                      candidate.command->description});
        }
    }

    // 45-D1.2: ArrowUp/ArrowDown move the highlight when the list is active.
    // Wraps modulo the list size and never mutates the draft or history.
    bool move_hint_selection(SessionUiState& state, int delta) {
        const std::size_t count = state.command_hints.size();
        if (count == 0) {
            return false;
        }
        const std::size_t current = state.command_hint_selected % count;
        const std::size_t next =
            delta < 0 ? (current + count - 1) % count : (current + 1) % count;
        state.command_hint_selected = next;
        return true;
    }

    // 45-D2.3: Tab completes the selected command with a trailing space and
    // clears the list; it never cycles (45-I3/I4). After Esc the list is hidden,
    // so the matches are recomputed from the bare `/prefix` and index 0 is
    // completed (45-D5.5).
    bool complete_selected_command(SessionUiState& state) {
        if (state.command_hints.empty()) {
            const std::string& draft = state.input.draft;
            if (draft.empty() || draft.front() != '/' ||
                draft.find_first_of(" \t") != std::string::npos) {
                return false;
            }
            const std::vector<CompletionCandidate> matches =
                registry_.complete_candidates(draft.substr(1));
            if (matches.empty()) {
                return false;
            }
            state.input.draft  = "/" + matches.front().spelling + " ";
            state.input.cursor = state.input.draft.size();
            state.command_hint_selected = 0;
            return true;
        }
        const std::size_t count = state.command_hints.size();
        const std::size_t index = std::min(state.command_hint_selected, count - 1);
        state.input.draft  = "/" + state.command_hints[index].insert + " ";
        state.input.cursor = state.input.draft.size();
        state.command_hints.clear();
        state.command_hint_selected = 0;
        return true;
    }

    // 45-D2.5/46-D5.3: the second pinned `CommandHint` construction site.
    void set_command_hints(SessionUiState& state, const std::vector<std::string>& names) {
        state.command_hints.clear();
        for (const std::string& name : names) {
            for (const CompletionCandidate& candidate : registry_.complete_candidates(name)) {
                if (candidate.spelling != name) {
                    continue;
                }
                state.command_hints.push_back(CommandHint{candidate.command->name,
                                                          command_display_name(*candidate.command),
                                                          candidate.spelling,
                                                          candidate.command->description});
                break;
            }
        }
    }

    // 46-D6: exact -> Dispatch; partial-with-candidates -> Complete; else Submit.
    enum class EnterAction : std::uint8_t { Dispatch, Complete, Submit };

    // 46-D6.1: an exact name/alias dispatches on the first Enter; a partial
    // command-shaped draft with candidates is completed in place and never
    // dispatched. `state` is part of the pinned signature; the classification is
    // independent of it.
    [[nodiscard]] EnterAction classify_enter(const SessionUiState& state,
                                             const std::string& draft) const {
        (void)state;
        if (draft.empty() || draft.front() != '/') {
            return EnterAction::Submit;
        }
        const std::string body  = draft.substr(1);
        const std::size_t split = body.find_first_of(" \t");
        const std::string token = split == std::string::npos ? body : body.substr(0, split);
        if (!token.empty() && registry_.find(token) != nullptr) {
            return EnterAction::Dispatch;
        }
        if (split == std::string::npos && !token.empty() &&
            !registry_.complete_candidates(token).empty()) {
            return EnterAction::Complete;
        }
        // A command-shaped draft that is not a partial completion (a bare `/` or
        // an unknown command) keeps today's `dispatch_command` notice path.
        return EnterAction::Dispatch;
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
        const int option_count = model_.dialog.force_ask ? 2 : 4;
        if (event == ftxui::Event::ArrowUp) {
            model_.dialog.selected =
                (model_.dialog.selected + option_count - 1) % option_count;
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            model_.dialog.selected = (model_.dialog.selected + 1) % option_count;
            return true;
        }
        if (event == ftxui::Event::Return) {
            if (model_.dialog.force_ask) {
                if (model_.dialog.selected == 0) {
                    resolve_dialog(payload::PermissionDecisionKind::Allow, GrantScope::Once);
                } else {
                    resolve_dialog(payload::PermissionDecisionKind::Deny, GrantScope::Once);
                }
                return true;
            }
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
        // Decision 2026-09-17 (RB-12): the dialog resolves only on `Return`
        // against the highlighted option. Printable keys are deliberately
        // swallowed (this method still returns true) so a slash command typed
        // while the dialog is up can never silently allow or deny. See
        // REQUIREMENTS_BACKLOG.md RB-12.
        return true;
    }

    void resolve_dialog(payload::PermissionDecisionKind decision, GrantScope scope) {
        const PermissionDialogModel dialog = model_.dialog;
        last_dialog_resolution_ = std::make_pair(decision, scope);
        if (dialog.tool == "exit_plan_mode" &&
            decision != payload::PermissionDecisionKind::Deny) {
            model_.pushNotice("plan exit queued");
            model_.dirty.markAggregate();
        }
        resolvePermission(dialog.session, dialog.request, decision, scope);
    }

    std::string workspace_label(const WorkspaceId& id) const {
        const auto modeled = model_.workspaces.find(id);
        if (modeled != model_.workspaces.end() && !modeled->second.title.empty()) {
            return modeled->second.title;
        }
        for (const WorkspaceHistory& history : model_.catalog.workspaces) {
            if (history.id == id) {
                return history.title.empty() ? history.canonicalPath : history.title;
            }
        }
        return id.value;
    }

    // 22 §4.3/§5.2 (S3): the History Enter path. Selecting a stored session
    // routes through `resume_from_history` (spawn-and-resume when the workspace
    // is not running). Selecting a workspace node only focuses an attached one;
    // an unmodeled workspace is reported through the notice ring (H1).
    void select_history(const SwitcherCursor& cursor) {
        if (cursor.session.has_value()) {
            resume_from_history(cursor.workspace, *cursor.session);
            return;
        }
        const auto connection = connections_.find(cursor.workspace);
        const bool attached = connection != connections_.end() &&
                              connection->second->state() == SupervisorLinkState::Attached;
        if (attached) {
            model_.focusWorkspace(cursor.workspace);
            return;
        }
        push_notice("workspace not running: " + workspace_label(cursor.workspace));
    }

    bool handle_switcher(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
            model_.switcher.close();
            model_.mode = UiMode::Conversation;
            catalog_visible_.store(false);
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
        if (model_.switcher.source == SwitcherSource::History && event.is_character() &&
            event.character() == "r") {
            if (catalog_ != nullptr) {
                catalog_->refreshNow();
            }
            return true;
        }
        if (event == ftxui::Event::Return) {
            const SwitcherCursor cursor = model_.switcher.cursor;
            if (model_.switcher.source == SwitcherSource::History) {
                select_history(cursor);
            } else if (cursor.session.has_value()) {
                model_.focusSessionIn(cursor.workspace, *cursor.session);
            } else {
                model_.focusWorkspace(cursor.workspace);
            }
            model_.switcher.close();
            model_.mode = UiMode::Conversation;
            catalog_visible_.store(false);
            return true;
        }
        return true;
    }

    bool handle_input(const ftxui::Event& event) {
        SessionUiState* state = active();
        if (state == nullptr) {
            state = model_.ensureActiveSession();
        }
        if (state == nullptr) {
            WorkspaceModel* workspace = model_.activeWorkspace();
            if (workspace != nullptr && workspace->activeSessionId().value.empty()) {
                create_session(workspace->id, std::string{});
            }
            return false;
        }
        InputModel& input = state->input;
        if (event.is_character() && modal_tail_suppression_active()) {
            return true;
        }
        // 45-D5.1: Esc hides a visible command list without touching the draft.
        if (event == ftxui::Event::Escape) {
            if (!state->command_hints.empty()) {
                state->hints_dismissed = true;
                state->command_hints.clear();
                state->command_hint_selected = 0;
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
                return true;
            }
            return false;
        }
        if (event == ftxui::Event::Return) {
            const std::string text = input.draft;
            switch (classify_enter(*state, text)) {
                case EnterAction::Complete:
                    // 46-D6.3: completion is non-destructive; a second Enter
                    // dispatches the now-exact draft.
                    complete_selected_command(*state);
                    refresh_hints(*state);
                    model_.dirty.mark(state->id, UiDirtyFlag::Input);
                    return true;
                case EnterAction::Dispatch:
                    if (dispatch_command(text)) {
                        input.push_history(text);
                        input.draft.clear();
                        input.cursor = 0;
                        input.saved_draft.clear();
                        state->command_hints.clear();
                        state->command_hint_selected = 0;
                        model_.dirty.mark(state->id,
                                          UiDirtyFlag::Input | UiDirtyFlag::Conversation);
                        return true;
                    }
                    submit(text);
                    return true;
                case EnterAction::Submit:
                    submit(text);
                    return true;
            }
            return true;
        }
        if (event == ftxui::Event::Tab) {
            if (command_list_active(*state) || bare_slash_prefix(input.draft)) {
                if (complete_selected_command(*state)) {
                    model_.dirty.mark(state->id, UiDirtyFlag::Input);
                    return true;
                }
                return false;
            }
            if (input.draft.empty()) {
                cycle_agent(1);
                return true;
            }
            return false;
        }
        if (event == ftxui::Event::TabReverse) {
            if (command_list_active(*state) || bare_slash_prefix(input.draft)) {
                return false;
            }
            if (input.draft.empty()) {
                cycle_agent(-1);
                return true;
            }
            return false;
        }
        if (event == ftxui::Event::Backspace) {
            if (input.cursor > 0) {
                input.draft.erase(input.cursor - 1, 1);
                --input.cursor;
                state->hints_dismissed = false;
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::Delete) {
            if (input.delete_forward()) {
                state->hints_dismissed = false;
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
            // 45-D1.2: the command list takes precedence over history recall.
            if (command_list_active(*state)) {
                move_hint_selection(*state, -1);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
                return true;
            }
            if (input.history_up()) {
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            if (command_list_active(*state)) {
                move_hint_selection(*state, 1);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
                return true;
            }
            if (input.history_down()) {
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::CtrlU) {
            input.clear_line();
            state->hints_dismissed = false;
            refresh_hints(*state);
            model_.dirty.mark(state->id, UiDirtyFlag::Input);
            return true;
        }
        if (event == ftxui::Event::CtrlW) {
            if (input.delete_word()) {
                state->hints_dismissed = false;
                refresh_hints(*state);
                model_.dirty.mark(state->id, UiDirtyFlag::Input);
            }
            return true;
        }
        if (event == ftxui::Event::CtrlE) {
            edit_prompt(*state);
            return true;
        }
        if (event.is_character()) {
            input.draft.insert(input.cursor, event.character());
            input.cursor += event.character().size();
            state->hints_dismissed = false;
            refresh_hints(*state);
            model_.dirty.mark(state->id, UiDirtyFlag::Input);
            return true;
        }
        return false;
    }

    void refresh_status_context(const WorkspaceId& workspace, const SessionId& session) {
        submit_to(workspace, std::string(protocol::method::kContextShow),
                  nlohmann::json{{"session", session.value}},
                  [this, session](SupervisorReply reply) {
                      ContextSnapshot snapshot;
                      const bool loaded = reply.ok && parse_context_snapshot(reply.result, snapshot);
                      enqueue([this, session, loaded, snapshot]() mutable {
                          SessionUiState* state = model_.session(session);
                          if (state == nullptr) {
                              return;
                          }
                          if (!loaded) {
                              state->status.context_used_tokens =
                                  static_cast<std::uint64_t>(std::max<std::int64_t>(
                                      0, state->status.input_tokens));
                              model_.dirty.mark(session, UiDirtyFlag::Status);
                              return;
                          }
                          state->status.context_used_tokens = snapshot.used_tokens;
                          state->status.context_window_tokens =
                              snapshot.budget.window_tokens != 0
                                  ? snapshot.budget.window_tokens
                                  : static_cast<std::uint64_t>(
                                        options_.config.agent.compaction.context_window_tokens);
                          model_.dirty.mark(session, UiDirtyFlag::Status);
                      });
                  });
    }

    // 18 §4.3 (M9): opens the read-only context overlay. Every request bumps the
    // generation, superseding any in-flight one; the reply is applied only while
    // its generation still matches (checked on the pump thread and again on the
    // UI thread), so a late reply can never re-open a dismissed overlay.
    void open_context() {
        WorkspaceModel* workspace = model_.activeWorkspace();
        SessionUiState* state = active();
        if (workspace == nullptr || state == nullptr) {
            return;
        }
        const SessionId     session = state->id;
        const std::uint64_t generation = bump_context_generation(context_generation_);
        submit_to(workspace->id, std::string(protocol::method::kContextShow),
                  nlohmann::json{{"session", session.value}},
                  [this, session, generation](SupervisorReply reply) {
                      if (!context_reply_is_current(context_generation_, generation)) {
                          return;
                      }
                      ContextOverlayModel overlay;
                      overlay.session = session;
                      if (reply.ok) {
                          overlay.loaded =
                              parse_context_snapshot(reply.result, overlay.snapshot);
                          if (!overlay.loaded) {
                              overlay.note = "malformed snapshot";
                          } else {
                              overlay.note = overlay.snapshot.note;
                          }
                      } else {
                          overlay.note = reply.error;
                      }
                      enqueue([this, overlay = std::move(overlay), generation]() mutable {
                          if (!context_reply_is_current(context_generation_, generation)) {
                              return;
                          }
                          model_.context = std::move(overlay);
                          model_.context.open = true;
                          model_.mode = UiMode::Context;
                          model_.dirty.markAggregate();
                      });
                  });
    }

    void close_context() {
        (void)bump_context_generation(context_generation_);
        model_.context.open = false;
        model_.mode = UiMode::Conversation;
        model_.dirty.markAggregate();
    }

    bool handle_context(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC ||
            (event.is_character() && event.character() == "q")) {
            close_context();
            return true;
        }
        if (event.is_character() && event.character() == "r") {
            open_context();
            return true;
        }
        if (event.is_character() && event.character() == "g") {
            model_.context.view = 0;
            model_.context.scroll = 0;
            model_.dirty.markAggregate();
            return true;
        }
        if (event.is_character() && event.character() == "t") {
            model_.context.view = 1;
            model_.context.scroll = 0;
            model_.dirty.markAggregate();
            return true;
        }
        if (event == ftxui::Event::Tab) {
            model_.context.view = model_.context.view == 0 ? 1 : 0;
            model_.context.scroll = 0;
            model_.dirty.markAggregate();
            return true;
        }
        if (event == ftxui::Event::ArrowDown || event == ftxui::Event::PageDown) {
            const int step = event == ftxui::Event::PageDown ? kContextScrollPage : 1;
            model_.context.scroll += step;
            model_.dirty.markAggregate();
            return true;
        }
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::PageUp) {
            const int step = event == ftxui::Event::PageUp ? kContextScrollPage : 1;
            model_.context.scroll = std::max(0, model_.context.scroll - step);
            model_.dirty.markAggregate();
            return true;
        }
        return true;
    }

    bool handle_event(ftxui::Event event) {
        const bool handled = handle_event_inner(std::move(event));
        // RB-12/RB-18: reconcile the composer snapshot *after* dispatch, so a
        // modal that closed during this event releases its snapshot and records
        // the true close instant immediately. Anchoring the burst-tail window
        // here (rather than at the first event of a later input batch) keeps a
        // delayed deliberate keystroke from being swallowed.
        sync_modal_composer();
        return handled;
    }

    bool handle_event_inner(ftxui::Event event) {
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
        if (model_.mode == UiMode::Context && model_.context.open) {
            return handle_context(event);
        }
        if (model_.mode == UiMode::Notice && model_.message.open) {
            return handle_notice(event);
        }
        if (model_.mode == UiMode::Switcher) {
            return handle_switcher(event);
        }
        if (event == ftxui::Event::CtrlS || event == ftxui::Event::CtrlP) {
            openSwitcher();
            catalog_visible_.store(false);
            if (catalog_ != nullptr) {
                catalog_->refreshNow();
            }
            return true;
        }
        if (event == ftxui::Event::CtrlD) {
            begin_exit(/*allow_prompt=*/true);
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
        animation_active_.store(false);
        last_tick_ = std::chrono::steady_clock::now();
        std::thread timer([this] {
            auto last_presence_post = std::chrono::steady_clock::now();
            while (!quit_.load()) {
                std::this_thread::sleep_for(kFrameInterval);
                if (screen_ == nullptr) {
                    continue;
                }
                // RB-18: only an active animation may force a repaint. Posting
                // Event::Custom invalidates FTXUI's frame and re-emits the whole
                // screen (re-asserting the terminal cursor), so an idle TUI stays
                // silent here and the cursor cannot flicker in an unfocused pane.
                if (animation_active_.load()) {
                    screen_->PostEvent(ftxui::Event::Custom);
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - last_presence_post < kPresencePostInterval) {
                    continue;
                }
                last_presence_post = now;
                screen_->Post(ftxui::Task{ftxui::Closure{[this] { tick_presence(); }}});
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
    // 45-D6.11: set once when the daemon reports `mcp.status` unavailable, so
    // `/mcp` never retries (45-F19). UI thread only.
    bool mcp_disabled_ = false;
    // 45-D9.6: the pending preference for the next `session.create`; never the
    // active composition (45-I28). UI thread only.
    std::string preferred_agent_;
    // 45-D9.10: set once when the daemon cannot serve the agent RPCs, so the
    // surface is disabled without a retry loop. UI thread only.
    bool agents_disabled_ = false;
    // 45-D9.9: one-shot notices so repeated Tabs do not spam the ring.
    bool agent_empty_noticed_     = false;
    bool agent_no_others_noticed_ = false;
    // 45-D9 test seam: canned `agent.list`/`agent.select` replies installed by
    // the harness, so the async cycle path is drivable without a live daemon.
    bool           agent_replies_installed_ = false;
    nlohmann::json agent_list_reply_        = nlohmann::json::object();
    int            agent_list_error_        = 0;
    nlohmann::json agent_select_reply_      = nlohmann::json::object();
    int            agent_select_error_      = 0;
    std::map<WorkspaceId, SupervisorWorkspace> specs_;
    std::map<WorkspaceId, std::unique_ptr<SupervisorConnection>> connections_;
    std::map<WorkspaceId, std::string> pending_creates_;
    std::optional<SupervisorPresence> presence_;
    std::unique_ptr<DaemonSetScanner> scanner_;
    std::chrono::steady_clock::time_point last_presence_tick_{};
    std::mutex action_mutex_;
    std::deque<std::function<void()>> actions_;
    ftxui::ScreenInteractive* screen_ = nullptr;
    // 46-D13 test seam: overrides the terminal hand-off around the prompt
    // editor. Production leaves it null and uses `screen_->WithRestoredIO`.
    std::function<void(const std::function<void()>&)> with_restored_io_;
    // 22 §4.2 (SW26): declared after `action_mutex_`/`actions_`/`screen_` so
    // reverse destruction stops+joins the catalog worker before them even if
    // the explicit destructor join is bypassed.
    std::unique_ptr<SessionCatalogReader> catalog_;
    // 22 §5.1 (SW24, SW26): the owned spawn worker and its queue. Declared after
    // `action_mutex_`/`actions_`/`screen_` for the same reverse-destruction
    // reason (the worker's completion action locks `action_mutex_`).
    std::jthread              ensure_worker_;
    std::mutex                ensure_mutex_;
    std::condition_variable   ensure_cv_;
    std::deque<EnsureRequest> ensure_requests_;
    std::set<WorkspaceId>     ensure_in_flight_;
    std::map<WorkspaceId, SessionId> pending_resume_;
    std::atomic<bool> quit_{false};
    // RB-17/RB-18: set on the UI thread after every `adapter_.onTick` and read by
    // the frame timer. True only while the flash or the reasoning spinner is
    // animating, so the timer posts a repaint only when the screen can change.
    std::atomic<bool> animation_active_{false};
    // RB-18: true while the `/sessions` History overlay is open. Read by the
    // catalog worker so it keeps delivering snapshots (fresh relative ages) only
    // while they are on screen; closed, unchanged snapshots are dropped.
    std::atomic<bool> catalog_visible_{false};
    // 18 §4.3 (M9/C1): written on the UI thread (open/close) and read on the
    // pump thread (reply early-out), so it MUST be atomic. It guards no other
    // memory; relaxed ordering is provably sufficient (see context_request.hpp).
    std::atomic<std::uint64_t> context_generation_{0};
    // RB-12/RB-18: composer snapshot while a modal is up, and the steady-clock
    // instant the modal closed. Printable input is dropped only while
    // `now - modal_closed_at_ < kModalTailWindow`, so the guard self-expires
    // without a repaint and never swallows later deliberate typing.
    std::optional<ModalComposerSnapshot>                 modal_composer_snapshot_;
    std::optional<std::chrono::steady_clock::time_point> modal_closed_at_;
    std::chrono::steady_clock::time_point last_tick_ = std::chrono::steady_clock::now();

    // RB-12 addendum (2026-09-17): test-only capture of the last permission
    // dialog resolution, read back through `SupervisorHarnessImpl`. The only
    // writer is `resolve_dialog`, so it stays empty on every other path.
    std::optional<std::pair<payload::PermissionDecisionKind, GrantScope>> last_dialog_resolution_;
};

// 22 §10.1/§10.2: additive test-only seam. Production reaches `SupervisorApp`
// only through `run_supervisor`; the deferred pinned tests (SW-U12/U13/U14/U18/
// U19, SW-I9/I10) drive the same private methods through this peer.
class SupervisorHarnessImpl final : public SupervisorHarness {
public:
    explicit SupervisorHarnessImpl(SupervisorRunOptions options)
        : app_(std::move(options)) {
        app_.ensure_worker_ =
            std::jthread([this](std::stop_token stop) { app_.ensure_worker_loop(stop); });
    }

    void ensure_workspace_running(const WorkspaceId& workspace,
                                  const SessionId& resume) override {
        app_.ensure_workspace_running(workspace, resume);
    }

    void on_scan(std::vector<SupervisorWorkspace> live) override {
        app_.on_scan(std::move(live));
    }

    void evict_dead_workspaces(const std::set<WorkspaceId>& live_ids) override {
        app_.evict_dead_workspaces(live_ids);
    }

    void on_link_state(const WorkspaceId& workspace, SupervisorLinkState state,
                       std::string detail) override {
        app_.on_link_state(workspace, state, std::move(detail));
    }

    void apply_resume_success(const WorkspaceId& workspace, const SessionId& session) override {
        app_.apply_resume_success(workspace, session);
    }

    void apply_create_reply(const WorkspaceId& workspace, const SessionId& session,
                            std::string error) override {
        app_.apply_create_reply(workspace, session, std::move(error));
    }

    void activate_session(const WorkspaceId& workspace, const SessionId& session) override {
        app_.activate_session(workspace, session);
    }

    void recover_unknown_session(const WorkspaceId& workspace,
                                 const SessionId& session) override {
        app_.recover_unknown_session(workspace, session);
    }

    void drain_actions() override { app_.drain(); }

    void open_switcher() override { app_.openSwitcher(); }

    void install_prompt_editor_io() override {
        app_.with_restored_io_ = [](const std::function<void()>& run) { run(); };
    }

    bool dispatch_command_line(const std::string& line) override {
        return app_.dispatch_command(line);
    }

    void start_catalog_with(WorkspaceCatalogSource source,
                            std::chrono::milliseconds refresh_interval) override {
        app_.catalog_ = std::make_unique<SessionCatalogReader>(
            std::move(source),
            [this](SessionCatalogSnapshot snapshot) {
                app_.enqueue([this, snapshot = std::move(snapshot)]() mutable {
                    app_.on_catalog_snapshot(std::move(snapshot));
                });
            },
            refresh_interval);
        app_.catalog_->start();
    }

    void refresh_catalog_now() override {
        if (app_.catalog_ != nullptr) {
            app_.catalog_->refreshNow();
        }
    }

    void on_catalog_snapshot(SessionCatalogSnapshot snapshot) override {
        app_.on_catalog_snapshot(std::move(snapshot));
    }

    void forget_session_state(const SessionId& session) override {
        app_.model_.sessions.erase(session);
    }

    void seed_pending_resume(const WorkspaceId& workspace, const SessionId& session) override {
        app_.pending_resume_[workspace] = session;
    }

    void seed_ensure_in_flight(const WorkspaceId& workspace) override {
        app_.ensure_in_flight_.insert(workspace);
    }

    void seed_workspace(const WorkspaceModel& workspace) override {
        app_.model_.workspaces[workspace.id] = workspace;
    }

    void seed_active_workspace(const WorkspaceModel& workspace) override {
        app_.model_.workspaces[workspace.id] = workspace;
        app_.model_.activeWorkspaceId = workspace.id;
    }

    void install_agent_replies(nlohmann::json list_result, int list_error,
                               nlohmann::json select_result, int select_error) override {
        app_.agent_replies_installed_ = true;
        app_.agent_list_reply_        = std::move(list_result);
        app_.agent_list_error_        = list_error;
        app_.agent_select_reply_      = std::move(select_result);
        app_.agent_select_error_      = select_error;
    }

    void open_permission_dialog(const SessionId& session, const PermissionRequestId& request,
                                std::string tool, std::string summary) override {
        app_.last_dialog_resolution_.reset();
        app_.model_.dialog.open    = true;
        app_.model_.dialog.session = session;
        app_.model_.dialog.request = request;
        app_.model_.dialog.tool    = std::move(tool);
        app_.model_.dialog.summary = std::move(summary);
        app_.model_.dialog.selected = 0;
        app_.model_.mode = UiMode::Dialog;
    }

    bool dispatch_key(const std::string& key) override {
        if (key == "up") {
            return app_.handle_event(ftxui::Event::ArrowUp);
        }
        if (key == "down") {
            return app_.handle_event(ftxui::Event::ArrowDown);
        }
        if (key == "enter") {
            return app_.handle_event(ftxui::Event::Return);
        }
        if (key == "escape") {
            return app_.handle_event(ftxui::Event::Escape);
        }
        if (key == "backspace") {
            return app_.handle_event(ftxui::Event::Backspace);
        }
        if (key == "delete") {
            return app_.handle_event(ftxui::Event::Delete);
        }
        if (key == "tab") {
            return app_.handle_event(ftxui::Event::Tab);
        }
        if (key == "tab-reverse") {
            return app_.handle_event(ftxui::Event::TabReverse);
        }
        if (key == "ctrl-c") {
            return app_.handle_event(ftxui::Event::CtrlC);
        }
        if (key == "ctrl-s") {
            return app_.handle_event(ftxui::Event::CtrlS);
        }
        if (key == "ctrl-p") {
            return app_.handle_event(ftxui::Event::CtrlP);
        }
        if (key == "ctrl-e") {
            return app_.handle_event(ftxui::Event::CtrlE);
        }
        return app_.handle_event(ftxui::Event::Character(key));
    }

    [[nodiscard]] std::optional<std::pair<payload::PermissionDecisionKind, GrantScope>>
    last_dialog_resolution() const override {
        return app_.last_dialog_resolution_;
    }

    void open_exit_prompt(const std::vector<WorkspaceId>& orphaning) override {
        app_.open_exit_prompt(orphaning);
    }

    [[nodiscard]] bool quit_requested() const override { return app_.quit_.load(); }

    [[nodiscard]] const UiModel& model() const override { return app_.model_; }
    [[nodiscard]] const std::set<WorkspaceId>& ensure_in_flight() const override {
        return app_.ensure_in_flight_;
    }
    [[nodiscard]] const std::map<WorkspaceId, SessionId>& pending_resume() const override {
        return app_.pending_resume_;
    }
    [[nodiscard]] bool has_connection(const WorkspaceId& workspace) const override {
        return app_.connections_.count(workspace) != 0;
    }
    [[nodiscard]] std::optional<std::string> spec_boot_id(
        const WorkspaceId& workspace) const override {
        const auto spec = app_.specs_.find(workspace);
        if (spec == app_.specs_.end()) {
            return std::nullopt;
        }
        return spec->second.boot_id;
    }
    [[nodiscard]] bool worker_joinable() const override { return app_.ensure_worker_.joinable(); }

private:
    SupervisorApp app_;
};

} // namespace

std::unique_ptr<SupervisorHarness> make_supervisor_harness(SupervisorRunOptions options) {
    return std::make_unique<SupervisorHarnessImpl>(std::move(options));
}

int run_supervisor(const SupervisorRunOptions& options) {
    SupervisorApp app(options);
    return app.run();
}

} // namespace ymh::ui
