#pragma once

// Core Event -> UiEvent adaptation (10-supervisor-tui.md §5, §54 D15).
//
// The adapter owns edge synthesis: the loop never emits a UI-shaped event. It
// reads `lastState_` to compute `AgentStateChanged` and never mutates that cache
// from `adapt()`; `onEvent()` updates it after applying (10 §5.2).

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/ui/ui_model.hpp"

namespace ymh::ui {

class UiController {
public:
    virtual ~UiController() = default;
    virtual void submit(const std::string& text) = 0;
    virtual void cancelActive() = 0;
    virtual void resolvePermission(const SessionId& session,
                                   const PermissionRequestId& request,
                                   payload::PermissionDecisionKind decision,
                                   GrantScope scope) = 0;
    virtual void requestExit() = 0;
};

class UiEventAdapter {
public:
    explicit UiEventAdapter(UiModel& model);
    UiEventAdapter(UiModel& model, UiController& controller);
    ~UiEventAdapter();

    UiEventAdapter(const UiEventAdapter&) = delete;
    UiEventAdapter& operator=(const UiEventAdapter&) = delete;

    // Authoritative agent state source; when unset the adapter projects state
    // from the core event type (10 §5.2).
    void set_state_provider(std::function<AgentState(const SessionId&)> provider);

    // Decoded wire frames, posted to the UI thread (10 §3.3).
    void onEvent(const Event& event);
    void onSessionEnvelope(const protocol::SessionEnvelope& envelope);
    void onPermissionRequest(const SessionId& session, const PermissionRequestId& id,
                             const PermissionRequest& request);
    void onPermissionResolved(const SessionId& session, const PermissionRequestId& id,
                              payload::PermissionDecisionKind decision);
    void onWorkspaceEvent(const WorkspaceEvent& event);

    // Model-level clock; the ONLY place the flash advances (10 §5.2, F12, D16).
    void onTick(std::chrono::milliseconds delta);

    // Pure: reads caches, returns the events to apply; never mutates (10 §5.2).
    [[nodiscard]] std::vector<UiEvent> adapt(const Event& event) const;

private:
    void applyAndMark(const UiEvent& event);
    [[nodiscard]] AgentState project_state(const SessionId& session,
                                           const Event& event) const;

    UiModel&      model_;
    std::map<SessionId, AgentState>      lastState_;
    std::map<SessionId, std::string>     startedMessages_;
    std::function<AgentState(const SessionId&)> state_provider_;
};

[[nodiscard]] std::string summarize_tool_arguments(const std::string& name,
                                                   const nlohmann::json& arguments);

} // namespace ymh::ui
