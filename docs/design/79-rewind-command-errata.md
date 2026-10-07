# 79 - Rewind Command Errata: a conversation-only TUI `/rewind`

```
Status: draft - reviewer: (none yet) - gate: not yet run
Revision: 1
Verification status: draft (Rev 1) -- NOT verified. On promotion, add/update the
           `DESIGN_STATUS.md` row 79 so its Status/Revision match this header,
           per AGENTS.md and the `design_status_drift` ctest. The tracker row is
           orchestrator-owned; this spec does not edit it.
Component: 79 (errata) - adds a supervisor-local `/rewind` command surface to
           the `ui::run_supervisor` path. `/rewind` opens a turn picker; choosing
           a turn branches the focused session into a new `kind='fork'` child at
           the turn's resolved-view boundary, focuses that child, and restores
           the turn's user prompt into the composer. It reuses spec 78's fork
           plumbing end to end and adds only a read-only turn-projection RPC and
           one overlay. Design only: no implementation code.
Depends on: 00-architecture.md (verified; sec 9.2/sec 9.9/sec 20.24/sec 44/sec
            54 D14/D16), 01-session.md (verified; sec 3/sec 6/sec 9.3),
            02-persistence.md (verified; sec 4.5/sec 4.7/sec 6/sec 9 P10),
            03-workspace-registry.md (verified),
            04-workspace-host-daemon.md (verified),
            05-transport.md (verified; sec 7.4 method table), 06-agent-loop.md
            (verified; turn lifecycle), 10-supervisor-tui.md (verified;
            sec 4.3/sec 4.4/sec 7/sec 9.2),
            13-context-compaction.md (verified; sec 4.4),
            16-daemon-ownership.md (verified; daemon lifetime unchanged),
            19-session-rename-errata.md, 22-switcher-sessions-errata.md
            (verified; sec 3/sec 4 live-vs-stored split),
            23-session-lifecycle-errata.md (verified), 24-agent-lifetime-errata.md
            (verified), 42-agent-presets.md (verified; sec 3.4),
            45-ui-interaction-errata.md (verified; key precedence),
            48-ui-and-config-errata.md (verified; EscArm/Esc-Esc semantics),
            55-multi-agent-delegation-errata.md (verified; sec 3.4),
            57-switcher-sessions-popup-errata.md (verified; key ownership),
            60-scroll-keybindings-errata.md (verified),
            78-fork-command-errata.md (the fork surface: `CommandContext::fork`,
            `SupervisorApp::fork_session`/`apply_fork_success`, the
            `std::optional<std::int64_t> seed_length` form, FK1-FK11),
            81-session-dashboard-errata.md (verified; sec 2.1/sec 2.2 chain A/R,
            81-D1 base selector, 81-D8 keys, 81-D13 overlay save/restore).
Scope: one user-requested feature: a TUI slash command `/rewind` that rewinds
       the CONVERSATION of the focused session. The mechanism is a branch: fork
       the focused session at the chosen turn's resolved-view boundary
       (`seed_length = boundary_index`, exactly spec 78's seam), focus the child
       via spec 78's `apply_fork_success` shape, and restore the chosen turn's
       user prompt into the child's composer. The parent is unchanged and keeps
       running. A `/rewind` picker lists the session's user turns (time +
       prompt summary) with Up/Down + Enter. This spec adds a read-only
       `session.rewind_targets` projection so the TUI can name a turn and its
       resolved-view index without owning the resolved view.
       Out of scope: file/code checkpoints and any file restore (spec 80,
       deliberately deferred); CC's "Summarize from here / up to here" actions;
       an in-place suffix-shadow truncation; the `Esc Esc` open gesture;
       `--resume`-style conversation-only rewind without branching.
Supersedes: (nothing normative). This spec does not withdraw 78; it reuses it.
            For the avoidance of doubt it states that 78 sec.13's forward-looking
            sentence ("a turn-aligned picker belongs to `/rewind` (spec 79, not
            yet written)") is fulfilled by this spec, not superseded.
Amends:     05 sec.7.4 (the method table) - adds a read-only method
            `session.rewind_targets` (params `{session}`, result `{view_length,
            targets[]}`); no existing method binding changes.
            The `TransportHost` base virtual set - adds
            `rewindTargets(const SessionId&)`; this changes the frozen interface,
            so the full implementer set is enumerated in sec.4.3 (amends 11
            sec.4.2/sec.4.3 beside the 78 fork delta).
            10 sec.9.2 (the command surface) - the built-in list gains `/rewind`
            registered before the `/help` snapshot (`src/ui/command_registry.cpp:285`).
            45 (key precedence) and 81 sec.2.2 - additive: a new chain-A guard
            `mode == UiMode::Rewind` and a new chain-R (`build_ui`) branch,
            placed relative to the existing overlays and to spec 81's
            `UiMode::Dashboard` as pinned in sec.7. It does NOT supersede
            81-D1/81-D13; it reuses the `dashboard.open` base selector and the
            prev-mode restore pattern.
```

ASCII only. Every anchor is `file:line` against the merged `tmp` tree (main+dev),
except where a cited spec is the anchor (then `NN sec...`).

---

## 1. Purpose and scope

Claude Code's `/rewind` opens a checkpoint menu and can restore "the conversation
and/or code" ([recon-claude-code.md sec.1.2], official
`code.claude.com/docs/en/checkpointing`). ymh already has the conversation half of
the substrate:

| Layer | Symbol | Anchor |
|---|---|---|
| Turn boundary | `payload::TurnStarted{turn, origin}` / `TurnEnded` | `include/ymh/session/events.hpp:52-84` |
| Prompt event | `payload::UserMessage{id, content, source}` | `include/ymh/session/events.hpp:98-102` |
| Pure projection | `Session::deriveMessages(header, events)` | `src/session/session.cpp:397-560` |
| Fork (branch) | `Session::fork(parent, seedLength, store, bus)` | `src/session/session.cpp:766-801` |
| Manager | `SessionManager::forkSession(const SessionId&, std::size_t)` | `src/session/session_manager.cpp:98-116` |
| Daemon | `HostRuntime::forkSession(const SessionId&, std::optional<std::int64_t>)` | `src/host/host_runtime.cpp:827-852` (post-78) |
| Wire | `method::kSessionFork = "session.fork"` | `include/ymh/transport/protocol.hpp:532`; `src/transport/protocol_server.cpp:424-434` |
| Resolved view | `resolve_after_locked` (prefix + own) | `src/session/session_persistence.cpp:477-517`; `readAfter` `:1044-1050` |
| UI command surface | `CommandRegistry::builtin()` | `src/ui/command_registry.cpp:147-298` |
| TUI `/fork` (dependency) | `CommandContext::fork`, `apply_fork_success` | `78 sec.4.1/sec.4.4` |

What is missing is only:

1. a **turn -> resolved-view index** projection the TUI can consume (the TUI does
   not own the resolved view; its stream is sequence-ordered, not index-ordered
   -- sec.2);
2. the `/rewind` command + `CommandContext::rewind` hook;
3. the turn-picker overlay (`UiMode::Rewind`) and its key handling;
4. the prompt-restore step after spec 78's fork/focus succeeds.

This spec is deliberately small and additive:

1. `/rewind` as a built-in command with a `CommandContext::rewind` hook (79-D1).
2. A read-only `session.rewind_targets` RPC that names each user turn's
   resolved-view boundary (79-D5).
3. A pinned turn -> index mapping (79-D2) and boundary rule (79-D3).
4. An overlay picker and its precedence relative to spec 81 (79-D6).
5. Conversation-only restoration; code restore is spec 80 (79-D7).

The fork itself is spec 78's; this spec sends `seed_length = boundary_index` and
reuses `session.fork` unchanged.

---

## 2. Current behaviour (verified against the tree, not trusted from the brief)

**2.1 Turns are explicit and monotone.** `payload::TurnStarted{turn, origin}` and
the terminal events `TurnEnded`/`TurnCancelled`/`TurnFailed` are the turn
boundaries; `TurnId` is session-local monotone (`include/ymh/session/ids.hpp:14`;
`include/ymh/session/events.hpp:52-84`). `TurnOrigin` is
`{User, Steer, FollowUp, Injection, Maintenance}` (`events.hpp:52-62`).

**2.2 The prompt is emitted BEFORE `TurnStarted`.** In the agent loop's turn
prologue the order is: `materializeInstructions()` (`agent_loop.cpp:1164`) ->
`materializeContexts(...)` (`:1165-1168`) -> `appendUserMessage(trigger.message)`
(`:1169`) -> `session_.append(payload::TurnStarted{turn, origin})` (`:1179`). The
local computations at `:1170-1178` append no events. Therefore, for every
prompt-bearing (non-`Inject`) turn, the turn's `UserMessage` is the event
**immediately preceding** its `TurnStarted`. This is the load-bearing fact for
the boundary rule in sec.5: the `TurnStarted` index is one *past* the prompt.

**2.3 The resolved view is an ordered event array, not the DB `Sequence`.** The
subscription/replay stream is `host_.readEvents(session, after, batch)`
(`protocol_server.cpp:623-635`), which resolves the fork prefix
(`HostRuntime::readEvents` -> `store().readAfter` -> `resolve_after_locked`,
`host_runtime.cpp:1242-1253`; `session_persistence.cpp:477-517,1044-1050`). The
`Sequence` column is DB-global `AUTOINCREMENT` and is **preserved** by a fork
(`recon-history.md sec.1`), so a `Sequence` is neither zero-based nor contiguous
within a session; the resolved-view **index** is the 0-based array position in
`read(session)`. Spec 78's `seed_length` indexes exactly that array
(`78 sec.5`). The UI stream is sequence-ordered, so the UI cannot derive the
array index from arrival order (spec 77 changes the subscribe start cursor), and
the UI adapter discards raw event records (`src/ui/ui_event_adapter.cpp:225-232`).

**2.4 The UI already has a focus-only selection primitive.** `apply_fork_success`
models the child, subscribes it (`track`), and calls `focusSession(child)` --
never `session.activate` (`78 sec.4.4`; `src/ui/supervisor.cpp:1661-1672`). This
spec reuses that shape.

**2.5 Overlays are `clear_under` windows in a fixed precedence.** `UiMode` is
`{Conversation, Switcher, Dialog, ExitConfirm, Context, Notice, ModelPicker}`
(`include/ymh/ui/ui_event.hpp:45-55`). Chain A dispatches by mode
(`src/ui/supervisor.cpp:3810-3904`); chain R (`build_ui`,
`src/ui/ui_render.cpp:2196-2215`) selects the base and composes overlays. Spec 81
adds `UiMode::Dashboard`, makes the base
`dashboard.open ? render_dashboard(...) : main` (81-D1), and pins the overlay
save/restore (`prev_mode`) pattern (81-D13). The Switcher and ModelPicker are
`clear_under` overlays (`ui_render.cpp:1530-1531`).

**2.6 No `rewind` symbol exists.** A tree-wide search for `rewind` finds only
prose in specs (`docs/design/76,77,81,82`); there is no symbol, command, or RPC.

---

## 3. Decisions

### 79-D1 - `/rewind` is a built-in command that opens a picker, not an immediate RPC

A new `Command` named `rewind` is registered in `CommandRegistry::builtin()`
**before** the `/help` snapshot line (`src/ui/command_registry.cpp:285`), so it
appears in `/help` and completion. Its handler calls `context.rewind(args)`; the
supervisor wires `context.rewind` in `dispatch_command` beside `context.fork`
(78-D1) to a new `SupervisorApp::open_rewind()`. Opening the picker performs a
**read-only** RPC (`session.rewind_targets`, 79-D5) and no mutation. No new
command file; no immediate fork on `/rewind` with an argument (the turn is chosen
in the picker).

### 79-D2 - Turn -> index mapping: boundary is the resolved-view index of the turn's prompt UserMessage

Pinned exactly:

- The universe is the resolved view `read(session, 0)` in sequence order -- the
  same array spec 78's `seed_length` indexes (`78 sec.5`).
- For a `TurnStarted` at array position `turn_start_index` whose
  `origin == TurnOrigin::User`, the **boundary index** is `turn_start_index - 1`,
  the position of that turn's prompt `UserMessage` (79 sec.2.2 proves the
  immediacy). `seed_length = boundary_index`.
- Indices are **array positions**, not DB `Sequence` values and not positions in
  `deriveMessages(...)` output. `ContextCompaction`/`ContextPrune` events are
  ordinary array elements and therefore shift indices; that is correct and
  consistent with 78, which seeds over the same array.
- The picker row is keyed by the `TurnStarted`'s `turn` (its identity) and its
  event timestamp; the row's *value* is `boundary_index`.

Rationale (and honest departure from the task brief's shorthand): the brief says
"the resolved-view index of the chosen `TurnStarted`". Using `turn_start_index`
verbatim would keep the prompt inside the child's prefix, and the missing
prompt would also be copied into the composer -- a duplicated user message. CC's
checkpoint is created "before each prompt you send that starts a turn"
([recon-claude-code.md sec.1.3]); the boundary must therefore be the prompt's own
index. The `TurnStarted` remains the turn's anchor; the seed is the immediately
preceding prompt event.

### 79-D3 - Mechanism: fork at the boundary via spec 78's `session.fork`, then focus

On Enter, `rewind_to` submits the existing `session.fork` with
`seed_length = boundary_index` and reuses spec 78's success path
(`ensureSessionIn` -> `ensureCellIn` -> `track` -> `focusSession`, no
`session.activate`). The parent is unchanged (78 FK1); the child is a
`kind='fork'` row whose resolved view is `read(parent)[0, boundary_index) ++
read(child)` (78 FK2/FK4). No new fork semantics are invented; spec 78's
`InvalidForkBoundary` is the out-of-range authority.

### 79-D4 - Restore the chosen turn's prompt into the child's composer

After focus, `apply_rewind_success` writes the prompt text into the **child's**
`SessionUiState::input.draft` (`include/ymh/ui/ui_model.hpp:207-213,312-319`) and
sets `input.cursor = draft.size()`. The text is the projection of the
`UserMessage` content blocks (the same text projection used by
`text_of_blocks`, `src/agent/agent_loop.cpp:1176`). It overwrites any existing
draft in the child (the user chose to rewind). A prompt with no text blocks
leaves the draft empty and surfaces a notice (79-F9). The prompt is **not**
auto-sent; the user edits and sends, exactly as CC restores into the input field
([recon-claude-code.md sec.1.2]).

### 79-D5 - New read-only RPC `session.rewind_targets`

The resolved view is daemon-owned (sec.2.3), so the turn -> boundary projection
is computed daemon-side and shipped as a read-only result. New method
`kSessionRewindTargets = "session.rewind_targets"`, params `{session}`, result
`{view_length, targets:[{turn, boundary_index, started_at_ms, prompt}]}`. It
mutates nothing (RW9). This is the only new wire surface; the fork reuses
`session.fork`.

### 79-D6 - The picker is a `clear_under` overlay (`UiMode::Rewind`), not a new framework

Add `Rewind` to `UiMode`. It is a peer of `Switcher`/`ModelPicker` in chain A and
chain R: a `clear_under` window over the current `base` (spec 81's
`dashboard.open ? render_dashboard : main`, 81-D1). It does **not** introduce a
full-screen renderer; if spec 81's `UiMode::Dashboard` owns a full-screen
surface, Rewind composes over it. Keys: ArrowUp/ArrowDown (and `k`/`j`) move,
Enter rewinds, Escape cancels; every other key is a consumed no-op (57-D4 key
ownership). Precedence is pinned in sec.7.

### 79-D7 - Conversation-only; CC parity stated honestly

CC `/rewind` restores conversation and/or code with a six-action menu
(restore code+conversation, restore conversation, restore code, summarize from
here, summarize up to here, never mind) ([recon-claude-code.md sec.1.2]). ymh 79
implements **conversation-only, one action**: branch the conversation and restore
the prompt. Code/file restore is spec 80 (not yet written; deliberately
deferred). Summarize-from/up-to-here are not mirrored (they are context
compaction surfaces, spec 13/32). "Never mind" is Escape. No CC capability is
overclaimed.

### 79-D8 - Only user-origin turns are targets

The picker lists `TurnStarted` events with `origin == TurnOrigin::User`. `Steer`,
`FollowUp`, `Injection`, and `Maintenance` turns are not listed: `Injection`/
`Maintenance` carry no user prompt, and `Steer`/`FollowUp` are mid-conversation
injections rather than the "prompts you sent that start a turn"
([recon-claude-code.md sec.1.3]). A future revision may widen this; the daemon
projection filters explicitly.

### 79-D9 - Refuse to rewind a subagent focus; subagent sessions are untouched

If the focused `SessionUiState::subagent == true`
(`include/ymh/ui/ui_model.hpp:349`; `58-D7`), `/rewind` refuses with a notice and
sends no RPC, mirroring `/fork` (78-D9) and `select_history`'s subagent rejection
(`src/ui/supervisor.cpp:2995-2998`). Rewinding a parent leaves any subagent
sessions alone; the fork creates a new session with no subagent children.

### 79-D10 - No-op guard: a boundary not strictly inside the view forks nothing

If the chosen target's `boundary_index >= view_length` (the boundary is at or
past the end, so the fork prefix equals the whole view and no conversation is
rewound), `/rewind` surfaces `"already at the current state"`, closes, and sends
no `session.fork` (79-F8). The `view_length` comes from the
`session.rewind_targets` result (79-D5) and is a defensive guard against a stale
target list. The empty-list case (no user turns) is 79-F3.

### 79-D11 - Failure surfaces a status-bar notice; never a phantom focus

Any transport/daemon error surfaces through the notice ring
(`push_notice`/`surface_notice`). The child is focused only when the fork reply
names a session id; a failed rewind leaves the focus on the parent (mirror 78-D10,
SW-F1).

### 79-D12 - No schema change, no ownership change, no `session.activate`; indices are stable

No new table, column, registry field, or event type. The daemon ownership/lifetime
is spec 16's: the child lives in the same supervisor-owned daemon and dies with
it, while its durable row survives (78 sec.13). `/rewind` issues no
`session.activate` (78 FK8). Because the event log is append-only, appending new
events never changes the resolved-view index of an existing event, so a boundary
captured at picker-open time remains valid at Enter time even if the parent
keeps working (RW7).

### 79-D13 - The `Esc Esc` open gesture is not mirrored

CC opens the rewind menu with double Escape when the composer is empty
([recon-claude-code.md sec.1.1]). ymh already assigns meaning to the Escape
gestures (`48-D2.1` EscArm; `81-D3` ArrowLeft on an empty composer is the
dashboard open gesture); adding a double-Escape arm would collide. ymh exposes
`/rewind` only and records the divergence in sec.11.

---

## 4. Interface sketches

### 4.1 `CommandContext::rewind` (new field)

```cpp
// include/ymh/ui/command_registry.hpp, in CommandContext (after the 78 fork hook)
// 79-D1: open the rewind picker for the focused session. `args` is the trimmed
// command-line tail; it is unused today (reserved), matching /sessions' shape.
std::function<void(const std::string&)> rewind;
```

Concrete caller (the built-in handler, registered before
`command_registry.cpp:285`):

```cpp
registry.add(Command{
    "rewind", "rewind the conversation to a previous turn (branch)",
    [](CommandContext& context, const std::string& args) {
        if (context.rewind) {
            context.rewind(args);
        }
    }});
```

### 4.2 `TransportHost::rewindTargets` (new base virtual + daemon/fake)

Base pure virtual - `include/ymh/transport/host.hpp` (after the post-78
`forkSession` at `:109`):

```cpp
// 79-D5: read-only projection of the session's user turns. Never mutates.
virtual protocol::RewindTargets rewindTargets(const SessionId& id) = 0;
```

Daemon override - `include/ymh/host/host_runtime.hpp` / `src/host/host_runtime.cpp`:

```cpp
protocol::RewindTargets HostRuntime::rewindTargets(const SessionId& id) override;
```

Body sketch (comment-level; the algorithm is pinned in sec.5):

```cpp
protocol::RewindTargets HostRuntime::rewindTargets(const SessionId& id) {
    return translate([&]() -> protocol::RewindTargets {
        if (!runtime_.store().load(id).has_value()) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        const EventRange view = runtime_.store().read(id, 0);   // resolved view
        protocol::RewindTargets result;
        result.view_length = static_cast<std::int64_t>(view.size());
        for (std::size_t i = 1; i < view.size(); ++i) {
            if (view[i].event.type != EventType::TurnStarted) {
                continue;
            }
            const auto started = view[i].event.payload.get<payload::TurnStarted>();
            if (started.origin != payload::TurnOrigin::User) {
                continue;                                        // 79-D8
            }
            if (view[i - 1].event.type != EventType::UserMessage) {
                continue;                                        // defensive (RW3)
            }
            const auto prompt = view[i - 1].event.payload.get<payload::UserMessage>();
            protocol::RewindTarget target;
            target.session          = id;
            target.turn             = started.turn;
            target.boundary_index   = static_cast<std::int64_t>(i - 1);
            target.started_at_ms    = view[i].timestamp;
            target.prompt           = text_of_blocks(prompt.content);   // 79-D4 projection
            result.targets.push_back(std::move(target));
        }
        return result;
    });
}
```

Test-double override - `tests/support/fake_transport_host.hpp` (beside the 78
`forkSession` at `:150`): `rewindTargets` computes the same projection from the
fake's in-memory `logs_`, using the identical `i - 1` rule, so FK-U9-style
agreement holds (RW-U7).

**Complete implementer set** (`TransportHost` has exactly two implementers - 78
sec.4.3): `HostRuntime` (`include/ymh/host/host_runtime.hpp`; production) and
`FakeTransportHost` (`tests/support/fake_transport_host.hpp:19`). Because the base
is a frozen interface, both must be updated in the same change set or the tree
does not build.

### 4.3 Wire method and DTOs

Method constant - `include/ymh/transport/protocol.hpp` (near `kSessionFork` `:532`):

```cpp
inline constexpr std::string_view kSessionRewindTargets = "session.rewind_targets";
```

DTOs - `include/ymh/transport/protocol.hpp` (after `SessionDetail` `:403-407`),
with `to_json`/`from_json` declared in the header and defined in
`src/transport/protocol.cpp` (beside `SessionDetail` `:623-633`):

```cpp
struct RewindTarget {
    SessionId    session;
    std::uint64_t turn{0};           // payload::TurnStarted::turn (TurnId, ids.hpp:14)
    std::int64_t boundary_index{0};  // resolved-view index of the prompt UserMessage
    std::int64_t started_at_ms{0};   // TurnStarted event timestamp (epoch ms)
    std::string  prompt;             // text projection of the prompt UserMessage
};
struct RewindTargets {
    std::int64_t              view_length{0};  // resolved view length at query time
    std::vector<RewindTarget> targets;
};
```

Wire binding:

```text
session.rewind_targets  params: { session }   result: { view_length, targets[] }
```

The method catalog `kMethodCatalog` (`src/transport/protocol.cpp:637-653`) grows
from 38 to **39** entries; `is_method_allowed` needs no change (read-only, allowed
on every profile, like `session.show`).

Handler - `src/transport/protocol_server.cpp` (beside the fork handler `:424-434`):

```cpp
} else if (method_name == method::kSessionRewindTargets) {
    // parse {session}; host_.rewindTargets(session); respond with
    // to_json_value(rewind_targets).
}
```

### 4.4 `RewindOverlayModel` and its view (new UI state)

```cpp
// include/ymh/ui/ui_model.hpp, near SwitcherOverlayModel (:490).
struct RewindTargetView {
    TurnId       turn;
    std::int64_t boundary_index{0};
    std::int64_t started_at_ms{0};
    std::string  prompt;    // full restore text (79-D4)
    std::string  summary;   // one-line display excerpt (renderer)
};
struct RewindOverlayModel {
    bool                          open = false;
    WorkspaceId                   workspace;
    SessionId                     session;
    std::int64_t                  view_length{0};   // 79-D10 no-op guard
    std::vector<RewindTargetView> targets;
    std::size_t                   cursor = 0;
    // 81-D13 pattern: restore the interrupted mode when a Rewind raised over the
    // dashboard closes. Today /rewind is reachable only from the composer, so
    // prev_mode is always Conversation; pinned for consistency.
    UiMode                        prev_mode = UiMode::Conversation;

    void open_with(WorkspaceId ws, SessionId session, std::int64_t view_length,
                   std::vector<RewindTargetView> targets);
    void close();
    void moveUp();
    void moveDown();
    [[nodiscard]] const RewindTargetView* selected() const;
};
```

`UiModel` gains `RewindOverlayModel rewind;` (beside `switcher`). Callers:
`open_rewind_overlay`, `handle_rewind`, `rewind_to`, `render_rewind`.

### 4.5 `SupervisorApp` methods (new)

```cpp
// src/ui/supervisor.cpp. Wired at dispatch_command beside context.fork.
void open_rewind();
// The session.rewind_targets reply: open the overlay, or surface a notice.
void open_rewind_overlay(const WorkspaceId& workspace, const SessionId& session,
                         protocol::RewindTargets targets);
// Chain-A handler while mode == UiMode::Rewind (79-D6).
bool handle_rewind(const ftxui::Event& event);
// Enter: fork at target.boundary_index via session.fork (79-D3).
void rewind_to(const RewindTargetView& target);
// The fork reply: model/track/focus the child, then restore the prompt (79-D4).
void apply_rewind_success(const WorkspaceId& workspace, const SessionId& child,
                          const std::string& prompt);
```

Body sketch - `open_rewind`:

```cpp
void open_rewind() {
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
        push_notice("no active session to rewind");            // 79-F1
        return;
    }
    const SessionUiState* focused = model_.activeSession();
    if (focused != nullptr && focused->subagent) {              // 79-D9
        push_notice("subagent sessions are not rewindable (/subagents)");
        return;
    }
    const WorkspaceId id = workspace->id;
    const SessionId   session = workspace->activeSessionId();
    rewind_query_ = session;                                    // in-flight guard
    submit_to(id, std::string(protocol::method::kSessionRewindTargets),
              nlohmann::json{{"session", session.value}},
              [this, id, session](SupervisorReply reply) {
                  enqueue([this, id, session, reply = std::move(reply)]() mutable {
                      if (!reply.ok) {
                          rewind_query_.reset();
                          surface_notice(id, session, "rewind failed: " + reply.error);
                          return;
                      }
                      open_rewind_overlay(id, session,
                                          reply.result.get<protocol::RewindTargets>());
                  });
              });
}
```

Body sketch - `open_rewind_overlay`:

```cpp
void open_rewind_overlay(const WorkspaceId& workspace, const SessionId& session,
                         protocol::RewindTargets targets) {
    rewind_query_.reset();
    // A focus change while the read was in flight: drop the stale reply.
    if (model_.activeWorkspace() == nullptr ||
        model_.activeWorkspace()->activeSessionId() != session) {
        return;
    }
    std::vector<RewindTargetView> views;
    views.reserve(targets.targets.size());
    for (const protocol::RewindTarget& t : targets.targets) {
        views.push_back(RewindTargetView{
            TurnId{t.turn}, t.boundary_index, t.started_at_ms,
            t.prompt, make_rewind_summary(t.started_at_ms, t.prompt)});
    }
    model_.rewind.prev_mode = model_.mode;      // 81-D13 pattern
    model_.rewind.open_with(workspace, session, targets.view_length,
                            std::move(views));
    model_.mode = UiMode::Rewind;
    model_.dirty.markAggregate();
}
```

Body sketch - `handle_rewind`:

```cpp
bool handle_rewind(const ftxui::Event& event) {
    if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
        model_.rewind.close();                  // open=false
        model_.mode = model_.rewind.prev_mode;  // 81-D13
        model_.dirty.markAggregate();
        return true;
    }
    if (event == ftxui::Event::ArrowDown ||
        (event.is_character() && event.character() == "j")) {
        model_.rewind.moveDown();
        return true;
    }
    if (event == ftxui::Event::ArrowUp ||
        (event.is_character() && event.character() == "k")) {
        model_.rewind.moveUp();
        return true;
    }
    if (event == ftxui::Event::Return) {
        if (const RewindTargetView* target = model_.rewind.selected();
            target != nullptr) {
            rewind_to(*target);
        } else {
            model_.rewind.close();              // empty list: Enter closes (79-F3)
            model_.mode = model_.rewind.prev_mode;
            model_.dirty.markAggregate();
        }
        return true;
    }
    return true;                                // consumed no-op (57-D4)
}
```

Body sketch - `rewind_to`:

```cpp
void rewind_to(const RewindTargetView& target) {
    const SessionId   parent = model_.rewind.session;
    const WorkspaceId id     = model_.rewind.workspace;
    // 79-D10: no-op guard.
    if (target.boundary_index >= model_.rewind.view_length) {
        push_notice("already at the current state");
        model_.rewind.close();
        model_.mode = model_.rewind.prev_mode;
        return;
    }
    const std::string prompt = target.prompt;
    const std::int64_t seed  = target.boundary_index;
    model_.rewind.close();
    model_.mode = UiMode::Conversation;         // restore prev_mode (81-D13)
    nlohmann::json params{{"session", parent.value}, {"seed_length", seed}};
    submit_to(id, std::string(protocol::method::kSessionFork), std::move(params),
              [this, id, parent, prompt](SupervisorReply reply) {
                  enqueue([this, id, parent, prompt, reply = std::move(reply)]() mutable {
                      if (!reply.ok) {          // 79-F5/79-F6
                          surface_notice(id, parent, "rewind failed: " + reply.error);
                          return;
                      }
                      const std::string child =
                          reply.result.value("session", std::string{});
                      if (child.empty()) {
                          surface_notice(id, parent, "rewind failed: empty reply");
                          return;
                      }
                      apply_rewind_success(id, SessionId{child}, prompt);
                  });
              });
}
```

Body sketch - `apply_rewind_success` (mirrors 78 sec.4.4, then restores):

```cpp
void apply_rewind_success(const WorkspaceId& workspace, const SessionId& child,
                          const std::string& prompt) {
    if (model_.workspaces.count(workspace) == 0) {
        surface_notice(workspace, child,
                       "rewound session in a workspace that is no longer open");
        return;
    }
    SessionUiState& state = model_.ensureSessionIn(workspace, child);
    if (state.status.model.empty()) {
        state.status.model =
            display_model(options_.config, stored_session_model(workspace, child));
    }
    model_.ensureCellIn(workspace, child);
    if (const auto connection = connections_.find(workspace);
        connection != connections_.end()) {
        connection->second->track(child);
    }
    model_.focusSession(child);                 // focus only; no session.activate
    sync_subagent_subscriptions();
    if (!prompt.empty()) {                       // 79-D4 / 79-F9
        state.input.draft  = prompt;
        state.input.cursor = prompt.size();
    } else {
        push_notice("rewound -> " + child.value + " (prompt unavailable)");
        model_.dirty.markAggregate();
        return;
    }
    push_notice("rewound -> " + child.value + " (prompt restored)");
    model_.dirty.markAggregate();
}
```

### 4.6 Renderer

```cpp
// src/ui/ui_render.cpp, beside render_switcher (:1496) / render_model_picker (:1534).
Element render_rewind(const UiModel& model, const Theme& theme, int available_width);
```

`render_rewind` returns a `ftxui::window`/`ftxui::border` with the
`clear_under` flag (same as `render_switcher`, `ui_render.cpp:1530-1531`): a
title line, one row per target (`relative age | prompt summary`, cursor
highlighted), an empty-state line when `targets.empty()`
("no user turns to rewind"), and a footer
`"Up/Down move | Enter rewind | Esc cancel"`.

---

## 5. The turn -> resolved-view index mapping (normative)

This section is the exact algorithm required by 79-D2/79-D5. It is the only place
the mapping is defined; an implementer must not re-derive it elsewhere.

**Inputs.** `view = read(session, 0)`, the resolved view in sequence order
(`session_persistence.cpp:477-517`). `view_length = view.size()`.

**Definition.** A **rewind target** exists for each index `i` in `[1, view_length)`
such that:

1. `view[i].event.type == EventType::TurnStarted`;
2. `view[i].event.payload.get<payload::TurnStarted>().origin == TurnOrigin::User`
   (79-D8);
3. `view[i-1].event.type == EventType::UserMessage` (the prompt, sec.2.2).

For such an `i`, the target is:

| Field | Value | Source |
|---|---|---|
| `turn` | `payload::TurnStarted::turn` | identity, monotone (`ids.hpp:14`) |
| `boundary_index` | `i - 1` | resolved-view index of the prompt `UserMessage` |
| `started_at_ms` | `view[i].timestamp` | event timestamp |
| `prompt` | text projection of `payload::UserMessage::content` | `text_of_blocks` (`agent_loop.cpp:1176`) |

**Ordering.** Targets are emitted in ascending `i` (oldest first). The picker
opens with the cursor on the last row (most recent), matching the
newest-relevant default ([recon-claude-code.md sec.1.4]).

**Fork effect.** With `seed_length = boundary_index`, the child's resolved view
is `view[0, boundary_index) ++ read(child)` (78 sec.5). The prefix therefore ends
just before the prompt: the chosen turn -- its prompt, its steps, its assistant
output -- is excluded, and the prompt is restored to the composer (79-D4).

**Index stability.** The log is append-only; new events append at the end, so
`boundary_index` computed at picker-open time still names the same event at Enter
time (RW7). No truncation op exists in ymh, so a captured index cannot be
invalidated by in-process activity.

**Compaction/shadow interaction (pinned).** `ContextCompaction`/`ContextPrune`
are ordinary events in `view`; they count toward `i` and `boundary_index`. A
boundary that lands inside a shadowed range is still a valid event index; the
child inherits the prefix's compaction events and folds them exactly as the
parent does (78 sec.5; `13 sec.4.4`). The `deriveMessages` shadow folding happens
only when projecting messages, and never renumbers `view` (sec.2.3).

**First turn.** `i` may be as small as 1; `boundary_index` may be `0`. A fork at
`0` yields a child with only its own `SessionStarted` and the prompt restored;
this is valid (78 FK2/FK3).

**No-target case.** If no index satisfies the three conditions, `targets` is
empty; the picker shows its empty state (79-F3).

---

## 6. The rewind child's row

The child is exactly spec 78's fork child: `sessions(kind='fork',
parent_session=parent.id, seed_length=boundary_index)`, `depth = parent.depth`,
inheriting the parent's route and composition (78-D3/78-D4, 78 sec.6). The SQL
CHECK and `validateHeader` enforce the matrix (`session_persistence.cpp:49-50`;
`session.cpp:362-368`). No new column or table (79-D12). The child receives its
own lease, agent, and `workspace_sessions` junction via `HostRuntime::forkSession`
(`host_runtime.cpp:835-847`).

---

## 7. Picker UI, mode, and precedence

**Mode.** New `UiMode::Rewind` in `include/ymh/ui/ui_event.hpp:45-55`.

**Chain A (key precedence).** Insert a guard **after** the ModelPicker guard
(`src/ui/supervisor.cpp:3834-3836`) and **before** spec 81's
`if (model_.mode == UiMode::Dashboard) return handle_dashboard(event);` guard
(81-D1, at `:3839`):

```cpp
if (model_.mode == UiMode::Rewind) {
    return handle_rewind(event);
}
```

Resulting order (spec 81 sec.2.2 A3-A9 extended):
exitConfirm > dialog > Context > Switcher > ModelPicker > **Rewind** > Dashboard.
So the exit confirm, the permission dialog, and `/context` stay above the picker;
the picker stays above the dashboard. All keys reach `handle_rewind` while it owns
the screen: ArrowUp/ArrowDown/`k`/`j`, Enter, Escape/Ctrl+C consumed; everything
else consumed no-op (57-D4).

**Chain R (`build_ui`).** Add a branch beside the Switcher/ModelPicker branches
(`src/ui/ui_render.cpp:2209-2214`):

```cpp
if (model.mode == UiMode::Rewind) {
    return dbox({base, render_rewind(model, theme, available_width)});
}
```

`base` is spec 81's `dashboard.open ? render_dashboard(...) : main` (81-D1), so
the picker composes over whatever base is showing. Today the picker is
unreachable while `dashboard.open` is true (`/rewind` is a composer command, so
it is dispatched from chain B, which is reached only when no overlay, and no
`Dashboard` in particular, owns input; `dispatch_command` at
`src/ui/supervisor.cpp:2665-2748`). The composition rule is pinned so a future
dashboard-raised rewind is correct by construction; the `prev_mode` field
(sec.4.4) restores the dashboard on close, per 81-D13.

**Opening gesture.** `/rewind` typed in the composer (chain B) ->
`dispatch_command` -> `context.rewind` -> `open_rewind`. There is no dashboard
key for rewind and no `Esc Esc` gesture (79-D13).

**Selection semantics.** Enter (re)uses spec 78's focus-only path: the child is
focused (`model_.focusSession(child)`), never `session.activate`; the daemon's
`active_session_` is unchanged (78 FK8/FK9). The picker closes on Enter (before
the RPC reply) and on Escape.

**No `session.activate`, no `agent.cancel`.** An in-flight parent turn is not
cancelled; the fork does not alter turn scheduling (78-D6/FK10). The parent and
the child are both subscribed (78 sec.7).

---

## 8. Failure modes

Continue the repo `F1-F12` convention with a spec-local `RW-F` prefix (disjoint
from `00 sec.54 F1-F12` and from `FK-F`).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| RW-F1 | No focused session / no active workspace | `/rewind` prints `"no active session to rewind"` | no RPC; focus unchanged |
| RW-F2 | Focused `SessionUiState::subagent == true` (`ui_model.hpp:349`) | notice `"subagent sessions are not rewindable (/subagents)"` | no RPC (79-D9; `58-E20`) |
| RW-F3 | Session has no user turns (`targets` empty) | picker opens in its empty state; Enter/Escape closes | no fork |
| RW-F4 | `session.rewind_targets` transport failure / daemon unreachable | notice `"rewind failed: <error>"`; overlay never opens | user retries when attached (mirror SW-F1) |
| RW-F5 | `session.fork` rejects `seed_length` (`AppCode::InvalidForkBoundary` `-32008`) | notice `"rewind failed: <error>"` | `Session::fork` throws before `store.create` (`session.cpp:769-771`); no row created; focus unchanged |
| RW-F6 | Workspace evicted between picker-open and Enter | notice `"rewound session in a workspace that is no longer open"` (mirror 78 `apply_fork_success`) | child durable on disk; no parent mutation |
| RW-F7 | Store/lease/route/agent errors on fork | the 78 FK-F6/FK-F7/FK-F8 recovories apply unchanged | parent untouched; recorded partial-fork possible (78 FK-F7) |
| RW-F8 | `boundary_index >= view_length` (stale target / no-op) | notice `"already at the current state"` | no `session.fork`; picker closes (79-D10) |
| RW-F9 | Chosen prompt has no text blocks | fork succeeds; child focused; composer left empty | notice `"rewound -> <child> (prompt unavailable)"`; user types the prompt |
| RW-F10 | `session.rewind_targets`/`session.fork` names an unknown/evicted parent (`AppCode::UnknownSession` `-32006`) | notice `"rewind failed: <error>"` | `refresh_sessions` drops the stale row; focus unchanged |
| RW-F11 | Focus changes while the target read is in flight | reply is dropped; no overlay; no RPC | `rewind_query_` reset; user re-invokes (sec.4.5) |

`RW-F5`/`RW-F7` are spec 78's failure modes reached through this caller; they are
not redefined, only routed.

---

## 9. Invariants

Spec-local `RW` numbering. Every row is testable and cites the code/spec that
would be violated.

| ID | Invariant |
|---|---|
| RW1 | `/rewind` is read-only until Enter: opening the picker issues only `session.rewind_targets` and mutates nothing; the parent's events, header, lease, agent, and turn are unchanged (`host_runtime.cpp:1242-1253` is a pure read). |
| RW2 | The rewind child satisfies 78 FK2: `kind='fork'`, `parent_session=parent.id`, `seed_length = boundary_index` in `[0, view_length)` (`session_persistence.cpp:49-50`; `session.cpp:362-368`). |
| RW3 | `boundary_index` is the resolved-view array index of the chosen turn's prompt `UserMessage`; it is an index into `read(session)`, NOT a DB `Sequence` and NOT a `deriveMessages` position, and it equals `turn_start_index - 1` (`session_persistence.cpp:477-517`; `agent_loop.cpp:1169,1179`). |
| RW4 | The child's resolved view is exactly `read(parent)[0, boundary_index) ++ read(child)`; the chosen turn's prompt, steps, and output are excluded, and the prompt is restored only into the composer (78 FK4; `session.cpp:617-627`). |
| RW5 | `/rewind` never mutates the parent and performs no `session.activate`; it focuses the child only through `focusSession` after `ensureSessionIn`/`track` (78 FK1/FK8/FK9). |
| RW6 | Only `TurnStarted` events with `origin == User` are targets; `Steer`/`FollowUp`/`Injection`/`Maintenance` turns are never listed (79-D8). |
| RW7 | Existing resolved-view indices are stable under append: a `boundary_index` captured at picker-open time names the same event at Enter time (`01` append-only; `02 sec.4`). |
| RW8 | `session.rewind_targets` is a pure projection: it never appends, mutates a header, or acquires a lease (79-D5/RW9). |
| RW9 | A boundary not strictly less than `view_length` forks nothing: `boundary_index >= view_length` produces a notice and no `session.fork` (79-D10). |
| RW10 | On success the chosen prompt text is present in the focused child's composer (`input.draft`) and absent from the child's conversation prefix; on a textless prompt the composer is empty and a notice is surfaced (79-D4/79-F9). |
| RW11 | Subagent focus is refused before any RPC; subagent sessions are never listed and never touched (79-D9). |
| RW12 | No schema, registry, or daemon-ownership change; the child lives in the existing supervisor-owned daemon and dies with it while its durable row survives (79-D12; `16`; 78 sec.13). |
| RW13 | The picker is a `clear_under` overlay whose base respects spec 81's `dashboard.open` selector; it is never a full-screen renderer and it introduces no new framework (79-D6; 81-D1). |

---

## 10. State lifetime

Columns follow 78 sec.10. Every new state introduced by this spec is tabulated.

| State | Created | Destroyed / evicted | Owner | Survives restart | Survives reconnect | Crash behaviour |
|---|---|---|---|---|---|---|
| `UiModel::rewind` (`RewindOverlayModel`) | `open_rewind_overlay` sets `open=true` | `close()` on Escape/Enter/no-op/focus change; workspace eviction; process exit | supervisor process (in-memory) | no | no | lost; rebuilt by re-invoking `/rewind`; no durable effect |
| `SupervisorApp::rewind_query_` (`std::optional<SessionId>`) | `open_rewind` before the read | reply (success or error), focus change, process exit | supervisor process | no | no | lost; the stale reply is dropped by the focus guard (RW-F11) |
| Captured `(workspace, parent, prompt, seed)` in the fork reply closure | `rewind_to` | when the reply runs and the closure is destroyed | supervisor stack/closure | n/a | n/a | dropped with the process; the fork either committed in the daemon or did not |
| Child `SessionUiState::input.draft` (restored prompt) | `apply_rewind_success` (only when prompt text is non-empty) | user send (`push_history` then clear), session close, process exit | supervisor process (in-memory; `ui_model.hpp:207-213`) | no | no | lost; the durable conversation is unaffected; the user can re-open `/rewind` |
| `protocol::RewindTargets` (server result) | `HostRuntime::rewindTargets` per request | when the JSON reply is serialized/sent | daemon request scope (transient) | n/a | n/a | none; recomputed on the next call |
| `session.rewind_targets` in-flight request (client) | `submit_to` | reply / connection drop / process exit | `SupervisorConnection` | no | no | pending request dropped on reconnect; overlay never opens (RW-F4) |
| Child `sessions` row (`kind='fork'`) + own `SessionStarted` + lease + agent + junction | `session.fork` -> `Session::fork`/`HostRuntime::forkSession` | `session.delete`/`erase`; daemon exit for the live agent/lease | daemon store + registry (durable row) | yes (durable) | yes, re-hydrated on resume | inherits 78 sec.10 row-for-row; WAL keeps the committed row, the prefix resolves from the parent |

No existing state's lifetime changes: the parent's lease/agent/stream/turn and the
supervisor focus are untouched (78 sec.10; RW5).

---

## 11. dsh mapping

The DeepSeek Harness separates durable session state from ephemeral client
presentation. `/rewind` adds a read-only projection and one control-plane caller;
it stays inside that separation.

| dsh concept | ymh mapping here | justification |
|---|---|---|
| Durable session store | `<ws>/.ymh/sessions.db`; rewind adds one `sessions` row plus the child's own events | `02 sec.9` P10 / `02 sec.4.5`: a fork stores no copied event rows; the prefix is shared and resolved recursively (`session_persistence.cpp:477-517`) |
| Session prefix / COW | `seed_length = boundary_index` + recursive `resolve_after_locked` | the same mechanism `ymh fork` uses (`session_cli.cpp:356`); no new substrate (78 sec.5) |
| Turn / prompt model | `TurnStarted` identity + preceding `UserMessage` | `events.hpp:63,98-102`; `agent_loop.cpp:1169,1179`; the index rule is sec.5 |
| Open set / membership | shared `registry.db` `workspace_sessions` junction | `registry_.addSession` (`host_runtime.cpp:844`); additive only |
| Client-local focus | `UiModel::activeSessionId` | never written to the registry (`ui_model.hpp:386-391`); rewind focuses locally (79-D3) |
| Ownership / liveness | supervisor-owned daemon (spec 16); child is a new session in the same daemon | `HostRuntime::forkSession` creates an independent child; no ownership change (`host_runtime.cpp:827-852`) |
| Control plane | existing `session.fork` + one new read-only `session.rewind_targets` | `session.fork` exists (`protocol.hpp:532`); the read-only method is 79-D5 |
| Event stream is truth | child own-log + parent prefix; the TUI is one consumer | `onSessionCreated` broadcasts; the TUI subscribes (`protocol_server.cpp:842-851`) |
| Agent lifetime | child gets its own agent; the parent's is untouched | `agents().resume(child)` (`host_runtime.cpp:839`); `24` |
| **Non-mirror**: CC "Restore code" / "Restore code and conversation" | **NOT mirrored** | deliberate scope decision: file/code checkpoints are spec 80 (not yet written), deferred here per this spec's Scope; ymh has no file-snapshot subsystem and `session_snapshots` is a derived cache, not a restore point (`02 sec.6` `:298,:327`; `design-brief.md` sec.D) |
| **Non-mirror**: CC "Summarize from here" / "Summarize up to here" | **NOT mirrored** | architectural absence: these are context-compaction actions, already owned by `/compact` and the compaction projection (`13`; `32`); rewind is a branch, not a summary. Reason anchored in 79-D7 |
| **Non-mirror**: CC six-action menu | **NOT mirrored**; ymh offers one action (rewind conversation) + Escape as "never mind" | scope decision anchored in 79-D7: conversation-only, one action; the CC parity gap is recorded rather than claimed |
| **Non-mirror**: CC `Esc Esc` open gesture on an empty composer | **NOT mirrored** | deliberate scope decision with a collision anchor: ymh already assigns Escape gestures (`48-D2.1` EscArm; `81-D3` ArrowLeft on an empty composer opens the dashboard), so a double-Escape arm would collide. Anchored in 79-D13 |
| **Non-mirror**: CC aliases `/checkpoint`, `/undo` | **NOT mirrored** | deliberate scope decision: ymh has no file checkpoints (`/checkpoint` would mislead), and `/undo` is an editor-ambiguous spelling; anchored in 79-D7/79-D13 |
| **Non-mirror**: CC's special top entry `/resume <session-id> (previous session)` after `/clear` | **NOT mirrored** | architectural absence: ymh `/clear` clears the local conversation model (`command_registry.cpp:157-168`) rather than switching sessions, and stored sessions are already reachable via `/sessions` (spec 22); anchored in `22 sec.4` (S2) |

---

## 12. Test plan

Strategy is `00 sec.44`: unit, integration (Fake LLM / fake daemon), persistence,
golden render, and an opt-in live PTY layer (`YMH_LIVE_LLM=1`). ID scheme:
`RW-U*` unit, `RW-I*` integration, `RW-G*` golden render, `RW-P*` PTY/live.
This spec does not edit tests; the implementation phase does.

### 12.1 Unit

| ID | Test (`file`) | Assertion |
|---|---|---|
| RW-U1 | `CommandRegistry.RW_U1_RewindRegisteredBeforeHelp` (`tests/unit/command_registry_test.cpp`) | `/help` lists `rewind`; dispatch reaches the `context.rewind` hook (79-D1) |
| RW-U2 | `HostRuntime.RW_U2_RewindTargetsBoundaryIsPromptIndex` (`tests/unit/host_runtime_test.cpp`) | for a log `[..., UserMessage, TurnStarted(User), ...]`, the target's `boundary_index` equals the `UserMessage` position (`turn_start_index - 1`), not the `TurnStarted` index (RW3) |
| RW-U3 | `HostRuntime.RW_U3_RewindTargetsOnlyUserOrigin` (`tests/unit/host_runtime_test.cpp`) | `Steer`/`FollowUp`/`Injection`/`Maintenance` `TurnStarted` events produce no target (RW6) |
| RW-U4 | `HostRuntime.RW_U4_RewindTargetsEmptyAndViewLength` (`tests/unit/host_runtime_test.cpp`) | a session with no user turns yields `targets.empty()` and the correct `view_length` (RW-F3/79-D5) |
| RW-U5 | `HostRuntime.RW_U5_RewindTargetsCompactionCounted` (`tests/unit/host_runtime_test.cpp`) | a `ContextCompaction` event before a turn shifts both the turn's `boundary_index` and `view_length`; the boundary still names the prompt event (RW3/RW7) |
| RW-U6 | `HostRuntime.RW_U6_RewindTargetsIsReadOnly` (`tests/unit/host_runtime_test.cpp`) | calling `rewindTargets` appends no event and changes no header/lease (RW8) |
| RW-U7 | `TransportHost.RW_U7_FakeRewindTargetsMatchesDaemon` (`tests/unit/transport_server_test.cpp`) | `FakeTransportHost::rewindTargets` yields the same projection as the daemon for the same log; the TUs including the fake compile (sec.4.2) |
| RW-U8 | `UiModel.RW_U8_RewindOverlayCursorAndClose` (`tests/unit/errata79_ui_test.cpp`) | `open_with` sets `open`, the cursor defaults to the last row, `moveUp`/`moveDown` clamp, and `close` sets `open=false` (sec.4.4) |
| RW-U9 | `Supervisor.RW_U9_RewindNoOpGuard` (`tests/unit/errata79_ui_test.cpp`) | a target with `boundary_index >= view_length` produces the no-op notice and issues no `session.fork` (RW9/79-D10) |
| RW-U10 | `Supervisor.RW_U10_RewindSubagentRefused` (`tests/unit/errata79_ui_test.cpp`) | a focused `subagent` state refuses with a notice and sends no RPC (RW11/79-D9) |

### 12.2 Integration (fake daemon / Fake LLM)

Extend `tests/integration_host_harness_test.cpp` (in-process host via
`tests/support/host_harness.hpp`) and a new `tests/unit/errata79_ui_test.cpp`
(FakeLLM driver / `FakeTransportHost`).

| ID | Test | Assertion |
|---|---|---|
| RW-I1 | `/rewind` on a live fake daemon creates a `kind='fork'` child whose `seed_length == boundary_index`; the parent row and event log are byte-identical (RW1/RW2/RW4/RW5) |
| RW-I2 | The child is focused and subscribed; the daemon's `active_session` is unchanged; the parent keeps its lease and agent (RW5; 78 FK8/FK9) |
| RW-I3 | After rewinding, the child's composer (`input.draft`) equals the chosen prompt text and the child's conversation prefix does not contain it (RW10/79-D4) |
| RW-I4 | A textless prompt rewinds and surfaces the `prompt unavailable` notice with an empty draft (RW-F9) |
| RW-I5 | `/rewind` with an in-flight parent turn (FakeLLM latch) does not cancel it; the parent turn completes and both sessions remain subscribed (78-D6/FK10) |
| RW-I6 | Daemon unreachable for `session.rewind_targets` -> notice, no overlay, focus unchanged (RW-F4) |
| RW-I7 | A stale target whose `boundary_index` exceeds the current view -> `InvalidForkBoundary` notice, no row created (RW-F5) |
| RW-I8 | A focus change between picker-open and Enter drops the reply and opens nothing (RW-F11) |
| RW-I9 | `/rewind` on a `subagent` focus is refused before any RPC (RW11/79-D9) |
| RW-I10 | Enter on an empty target list closes the picker and forks nothing (RW-F3) |

### 12.3 Golden render

| ID | Test (`tests/unit/ui_render_golden_test.cpp`) | Assertion |
|---|---|---|
| RW-G1 | `RW_G1_RewindListsPrompts` | the renderer draws one row per target (age + summary), the cursor highlight, and the footer literal `Up/Down move \| Enter rewind \| Esc cancel` (79-D6) |
| RW-G2 | `RW_G2_RewindEmptyState` | an empty target list renders the `no user turns to rewind` line and no rows (RW-F3) |
| RW-G3 | `RW_G3_RewindComposesOverBase` | with `dashboard.open` forced true, the picker composes over `render_dashboard` (dbox), not over a full-screen replacement (RW13; 81-D1) |

### 12.4 PTY / live (opt-in)

| ID | Test (`tests/integration/ui_supervisor_pty_test.cpp`, `YMH_LIVE_LLM=1`) | Assertion |
|---|---|---|
| RW-P1 | `/rewind` opens the picker, ArrowUp/Down move, Enter rewinds against a real DeepSeek daemon; a new `kind='fork'` row appears and the prompt is prefilled (RW1-RW5/RW10) |
| RW-P2 | Escape in the picker closes it without a fork; the parent session continues (79-D6) |

### 12.5 Invariant and failure-mode coverage

| Invariant | Tests |
|---|---|
| RW1, RW8 | RW-U6, RW-I1 |
| RW2, RW4 | RW-I1, RW-P1 |
| RW3 | RW-U2, RW-U5 |
| RW5, RW7 | RW-I2, RW-I5 |
| RW6 | RW-U3 |
| RW9 | RW-U9, RW-I10 |
| RW10 | RW-I3, RW-I4, RW-P1 |
| RW11 | RW-U10, RW-I9 |
| RW12 | RW-I2, RW-P1 |
| RW13 | RW-G3 |

| Failure mode | Tests |
|---|---|
| RW-F1, RW-F2 | RW-U10, RW-I9 |
| RW-F3 | RW-U4, RW-I10, RW-G2 |
| RW-F4 | RW-I6 |
| RW-F5 | RW-I7 |
| RW-F6 | RW-I7 |
| RW-F7 | RW-I1 (inherited 78 coverage), RW-I7 |
| RW-F8 | RW-U9 |
| RW-F9 | RW-I4 |
| RW-F10 | RW-I6 |
| RW-F11 | RW-I8 |

---

## 13. Out of scope, parity notes, and recorded risks

- **File/code checkpoints are spec 80, deferred.** This spec designs the
  conversation half only. CC's "Restore code" / "Restore code and conversation"
  are not mirrored; ymh has no file-snapshot subsystem and `session_snapshots` is
  an unwired derived cache, not a restore point (`session_persistence.cpp:75-83`;
  `design-detail.md` sec.1.4). Do not read `/rewind` as a file-restore promise.
- **Conversation-only rewind without branching is not offered.** ymh has no
  in-place truncate/suffix-shadow op (only whole-session `erase`,
  `session_persistence.cpp:923,991`); the chosen mechanism is a branch, so the
  original conversation is always retained next to the child. A future in-place
  variant could append a rewind-boundary shadow event ignored by
  `ownEvents`/`deriveMessages` (design-brief.md sec.D), but it is not designed
  here.
- **`Steer`/`FollowUp` prompts are not targets.** 79-D8 lists only
  user-origin turns. Widening the set is a compatible future revision (add the
  origins to the daemon filter; the boundary rule is unchanged because those
  turns also emit a `UserMessage` immediately before `TurnStarted`).
- **`Esc Esc` and CC aliases are not mirrored** (79-D13; sec.11). Escape and
  Enter/Escape in the picker cover the CC "never mind" escape hatch.
- **Recorded risk (index drift).** A `boundary_index` captured by the picker is
  stable only under append-only semantics (RW7). If a future revision adds an
  event-removing/truncating operation, the captured index could become invalid;
  the `InvalidForkBoundary`/view-length guards (RW9; RW-F5) remain the backstop.
- **Recorded risk (focus race).** The picker captures `(workspace, session)` at
  open; the reply and Enter are guarded against a focus change (RW-F11). The
  guard is on the focused session id, mirroring 78's workspace guard.
- **No daemon-ownership change.** The child lives in the existing
  supervisor-owned daemon (spec 16); the last supervisor's exit still tears the
  daemon down and takes the child's live agent with it. The durable row survives
  (78 sec.13).
- **Live-vs-stored split is preserved.** `/rewind` lists turns of the focused
  session only; `/sessions` remains the stored catalog and Ctrl+S (spec 81's
  dashboard) remains the live surface. `/rewind` neither reads nor changes either
  (`22 sec.3/sec.4`; 81-D10).

---

## 14. Revision log

| Rev | Change |
|---|---|
| 1 | Initial draft: `/rewind` command + `CommandContext::rewind` hook; a read-only `session.rewind_targets` RPC and its DTOs; the pinned turn -> resolved-view index mapping (boundary = the prompt `UserMessage` index, `turn_start_index - 1`, over the raw resolved-view array; `Sequence` and `deriveMessages` positions are explicitly excluded; compaction events count); the fork reuses spec 78's `session.fork` with `seed_length = boundary_index`, focus-only; prompt restoration into the child composer; `UiMode::Rewind` picker with its precedence relative to spec 81's dashboard and the existing overlays; failure modes RW-F1..RW-F11, invariants RW1..RW13, state-lifetime table, dsh mapping with non-mirror justifications, and a hermetic test plan. Conversation-only; code restore deferred to spec 80. |
