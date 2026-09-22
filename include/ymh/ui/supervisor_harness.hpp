#pragma once

// Test-only seam for `SupervisorApp` internals (22-switcher-sessions-errata.md
// §10.1/§10.2). The app lives in an anonymous namespace inside
// `src/ui/supervisor.cpp` and enters the FTXUI loop in `run()`, so its
// spawn-worker, catalog-reader and eviction paths are not reachable from a test.
//
// This header is additive test-support surface. It is compiled into production:
// `src/ui/supervisor.cpp` includes it because `make_supervisor_harness` must be
// defined in that TU to reach the anonymous-namespace `SupervisorApp`. Production
// never *calls* it — `run_supervisor` remains the only production entry point —
// so the factory and harness are inert in the shipped binary. The harness drives
// the same private methods on the calling (UI) thread and exposes the pinned
// lifetime state the deferred tests assert on. `drain_actions` is the FTXUI
// loop's action pump, which the tests must call after an operation that enqueues.

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/supervisor.hpp"
#include "ymh/ui/supervisor_connection.hpp"
#include "ymh/ui/ui_model.hpp"

namespace ymh::ui {

class SupervisorHarness {
public:
    virtual ~SupervisorHarness() = default;

    // UI-thread entry points mirroring the private `SupervisorApp` methods.
    virtual void ensure_workspace_running(const WorkspaceId& workspace,
                                          const SessionId& resume) = 0;
    virtual void on_scan(std::vector<SupervisorWorkspace> live) = 0;
    virtual void evict_dead_workspaces(const std::set<WorkspaceId>& live_ids) = 0;
    virtual void on_link_state(const WorkspaceId& workspace, SupervisorLinkState state,
                               std::string detail) = 0;
    virtual void apply_resume_success(const WorkspaceId& workspace,
                                      const SessionId& session) = 0;
    virtual void apply_create_reply(const WorkspaceId& workspace, const SessionId& session,
                                    std::string error) = 0;
    // 45-D10.7: drives the real `activate_session` (a delegate to the single
    // modeling mutator) so the pinned invariant is exercised, not re-seeded.
    virtual void activate_session(const WorkspaceId& workspace, const SessionId& session) = 0;
    // 45-D10.8: drives the real UnknownSession recovery.
    virtual void recover_unknown_session(const WorkspaceId& workspace,
                                         const SessionId& session) = 0;

    // The FTXUI loop's action pump; runs every action queued so far.
    virtual void drain_actions() = 0;

    // 46-D3: drives the real Ctrl+S open path (the target predicate and the
    // notice-vs-switcher decision).
    virtual void open_switcher() = 0;
    // 46-D13: installs a pass-through terminal hand-off so the editor runs
    // without an FTXUI loop.
    virtual void install_prompt_editor_io() = 0;

    // 45-D6/D7: drives the real slash-command dispatch directly (bypassing the
    // composer's auto-create path) so the session-less `/mcp`/`/status`
    // fallbacks are testable.
    virtual bool dispatch_command_line(const std::string& line) = 0;

    // Replaces the app's catalog reader with one built from `source` and starts
    // it (SW-U19/SW-I10 need a blocking read). The app owns it and stops+joins
    // it in its destructor.
    virtual void start_catalog_with(WorkspaceCatalogSource source,
                                    std::chrono::milliseconds refresh_interval) = 0;
    virtual void refresh_catalog_now() = 0;
    // 45-D3/D4: drives the real catalog-delivery path so the Live resnapshot and
    // the History rebuild are exercised without a reader thread.
    virtual void on_catalog_snapshot(SessionCatalogSnapshot snapshot) = 0;
    // 45-D10.3: drops a session's UI state while leaving the workspace cell and
    // the focus in place, reproducing the non-empty-but-unmodeled focus that
    // `handle_input` must self-heal.
    virtual void forget_session_state(const SessionId& session) = 0;

    // Test-only seeding (SW-U13/SW-U14/SW-U18).
    virtual void seed_pending_resume(const WorkspaceId& workspace, const SessionId& session) = 0;
    virtual void seed_ensure_in_flight(const WorkspaceId& workspace) = 0;
    virtual void seed_workspace(const WorkspaceModel& workspace) = 0;
    // 45-D6/D7: seeds a workspace and makes it active with no modeled session,
    // so the session-less `/mcp`/`/status` paths can be driven through the real
    // command dispatch.
    virtual void seed_active_workspace(const WorkspaceModel& workspace) = 0;

    // 45-D9: installs canned `agent.list`/`agent.select` replies so the async
    // cycle path can be driven without a live daemon.
    virtual void install_agent_replies(nlohmann::json list_result, int list_error,
                                       nlohmann::json select_result,
                                       int select_error) = 0;

    // 46-D7 (N6): installs a canned `session.list` reply so the
    // focus/create/resume decision is drivable without a live daemon.
    virtual void install_session_list_reply(nlohmann::json result, int error) = 0;
    // 46-D7 (N6): installs a canned reply for one method (`session.resume`,
    // `agent.prompt`), so its reply terminal is drivable without a live daemon.
    virtual void install_method_reply(std::string method, nlohmann::json result, int error) = 0;
    // 46-D7 (N6): seeds the per-workspace resume refcount directly, so the
    // `session.list` gate can be exercised while a resume is in flight.
    virtual void seed_resume_in_flight(const WorkspaceId& workspace) = 0;
    [[nodiscard]] virtual const std::map<WorkspaceId, std::size_t>& resume_in_flight() const = 0;
    // 46-D7 / O-H3: stops the workspace connection, forcing the link dead and
    // draining its pending requests with a reply.
    virtual void drop_connection(const WorkspaceId& workspace) = 0;

    // RB-12 addendum (2026-09-17): permission-dialog key semantics. Opens the
    // dialog through the same model fields the adapter writes, then drives FTXUI
    // events through the real handler so the swallow contract and the
    // Enter-only resolution are exercised end to end. `key` is a single
    // printable character or one of "up", "down", "enter", "escape", "ctrl-c",
    // "tab", "tab-reverse", "backspace", "delete".
    virtual void open_permission_dialog(const SessionId& session,
                                        const PermissionRequestId& request,
                                        std::string tool, std::string summary) = 0;
    virtual bool dispatch_key(const std::string& key) = 0;
    [[nodiscard]] virtual std::optional<
        std::pair<payload::PermissionDecisionKind, GrantScope>>
    last_dialog_resolution() const = 0;

    // Exit-confirm key semantics. `open_exit_prompt` drives the real production
    // opener (so the default highlight is exercised, not re-seeded); `dispatch_key`
    // then sends ↑/↓/Enter through the real handler.
    virtual void open_exit_prompt(const std::vector<WorkspaceId>& orphaning) = 0;
    [[nodiscard]] virtual bool quit_requested() const = 0;

    // Observers (valid only while the harness lives).
    [[nodiscard]] virtual const UiModel& model() const = 0;
    // 48-D2 test seam: a mutable model so a test can seed a session's agent
    // state (arming requires an active turn) and inspect the Esc arm.
    [[nodiscard]] virtual UiModel& mutable_model() = 0;
    // 48-D2 test seam: the number of `cancelActive()` submissions so far.
    [[nodiscard]] virtual std::size_t cancel_count() const = 0;
    [[nodiscard]] virtual const std::set<WorkspaceId>& ensure_in_flight() const = 0;
    [[nodiscard]] virtual const std::map<WorkspaceId, SessionId>& pending_resume() const = 0;
    [[nodiscard]] virtual bool has_connection(const WorkspaceId& workspace) const = 0;
    [[nodiscard]] virtual std::optional<std::string> spec_boot_id(
        const WorkspaceId& workspace) const = 0;
    [[nodiscard]] virtual bool worker_joinable() const = 0;
};

[[nodiscard]] std::unique_ptr<SupervisorHarness> make_supervisor_harness(
    SupervisorRunOptions options);

} // namespace ymh::ui
