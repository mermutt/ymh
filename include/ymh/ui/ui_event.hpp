#pragma once

// Frontend-only presentation events (10-supervisor-tui.md §5.1).
//
// `UiEvent` is produced *only* inside the supervisor by `UiEventAdapter` and is
// never serialized on the wire (U4, §20.6, 05 §5.2, §54 D15). The wire carries
// the core, durable `Event`; the model sees only `UiEvent`/`WorkspaceEvent`.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/ids.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh::ui {

// 03 §2.1 / 01 §2.1 (never a path). 22 §4.6: aliased to the registry's
// `ymh::WorkspaceId` so the S2 catalog's `WorkspaceHistory` and the UI model's
// node/connection keys are one type — the two definitions were structurally
// identical, and keeping both forced an id bridge and ambiguous unqualified use.
using WorkspaceId = ymh::WorkspaceId;

// Supervisor-local view of a daemon (10 §2.1); never written to the registry.
// 16 §7.6 (additive): `Stopping` is a daemon draining after last-exit/watchdog;
// `NotRunning` is a registered workspace with no live daemon (16-D2).
enum class DaemonStatus : std::uint8_t {
    Connecting,
    Attached,
    Detached,
    Dead,
    Stopping,
    NotRunning,
};

enum class UiMode : std::uint8_t {
    Conversation,
    Switcher,
    Dialog,
    ExitConfirm,
    Context,
};

enum class WorkspaceEventKind : std::uint8_t {
    DaemonAttached,
    DaemonDetached,
    DaemonDied,
    DaemonStopping,
    SessionOpened,
    SessionClosed,
};

// Supervisor-local; produced from 05's `HostNotice` (10 §4.5). Never on the wire.
// 16 §7.6 (C-H1/O-M5): `session` is REQUIRED for SessionOpened/SessionClosed;
// without it a peer supervisor cannot learn which session appeared.
struct WorkspaceEvent {
    WorkspaceId                workspace;
    WorkspaceEventKind         kind = WorkspaceEventKind::DaemonAttached;
    std::optional<SessionId>   session;
};

// ---- adapted, frontend-facing events (10 §5.1) -----------------------------
//
// Every payload carries its routing `SessionId`. `AgentStateChanged` is the
// single source of agent-state truth (old + new) so attention can be edge
// triggered without a second encoding of the same state (§5.1, §20.5).

struct UserMessage {
    SessionId   session;
    MessageId   id;
    std::string text;
};

struct AssistantMessageStarted {
    SessionId session;
    MessageId message;
};

struct AssistantTextDelta {
    SessionId   session;
    MessageId   message;
    std::string text;
    bool        reasoning = false;
};

struct AssistantMessageFinished {
    SessionId         session;
    MessageId         message;
    std::string       text;
    std::optional<Usage> usage;
};

struct ToolStarted {
    SessionId   session;
    ToolCallId  id;
    std::string name;
    std::string arguments;   // rendered JSON (bounded by the adapter)
};

struct ToolOutput {
    SessionId  session;
    ToolCallId id;
    std::string chunk;
};

struct ToolFinished {
    SessionId                  session;
    ToolCallId                 id;
    std::string                name;
    payload::ToolOutcome        outcome = payload::ToolOutcome::Ok;
    std::string                output;
    bool                       truncated = false;
    std::optional<std::string> error;
};

struct FileChanged {
    SessionId   session;
    std::string path;
};

struct DiffUpdated {
    SessionId session;
};

struct PermissionRequested {
    SessionId           session;
    PermissionRequestId request;
    std::string         tool;
    std::string         summary;   // bounded, redacted by 09
};

struct PermissionResolved {
    SessionId                      session;
    PermissionRequestId            request;
    payload::PermissionDecisionKind decision = payload::PermissionDecisionKind::Deny;
};

struct AgentStateChanged {
    SessionId  session;
    AgentState oldState = AgentState::Idle;
    AgentState newState = AgentState::Idle;
};

struct SubagentUpdated {
    SessionId   session;
    SessionId   subagent;
    std::string summary;
    AgentState  state = AgentState::Idle;
};

struct ErrorOccurred {
    SessionId   session;
    std::string message;
};

struct TokenUsageUpdated {
    SessionId session;
    Usage     usage;
};

struct StatusChanged {
    SessionId   session;
    std::string text;
};

struct CompactionMarker {
    SessionId   session;
    std::uint64_t boundary = 0;
    std::size_t   tokenEstimate = 0;
    std::string   model;
    std::string   summary;
};

struct CompactionOutcomeNotice {
    SessionId         session;
    CompactionOutcome outcome = CompactionOutcome::NotNeeded;
    std::uint64_t     boundary = 0;
    std::size_t       tokenEstimate = 0;
    std::string       model;
    std::string       reason;
};

struct SessionTitleChanged {
    SessionId   session;
    std::string title;
};

struct UiEvent {
    std::variant<
        UserMessage,
        AssistantMessageStarted,
        AssistantTextDelta,
        AssistantMessageFinished,
        ToolStarted,
        ToolOutput,
        ToolFinished,
        FileChanged,
        DiffUpdated,
        PermissionRequested,
        PermissionResolved,
        AgentStateChanged,
        SubagentUpdated,
        ErrorOccurred,
        TokenUsageUpdated,
        StatusChanged,
        CompactionMarker,
        CompactionOutcomeNotice,
        SessionTitleChanged>
        value;
};

// The wire envelope is the transport's `protocol::SessionEnvelope` (05 §5.2);
// the local duplicate that Milestone 1 carried is retired so the adapter
// consumes the real wire type (10 §4.5, U4).

} // namespace ymh::ui
