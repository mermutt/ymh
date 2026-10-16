# 85 - Turn Failure Attribution Errata

```
Status: verified (Rev 3)
Verification status: adversarial reviewer (independent, Rev 3): GATE PASS (0 HIGH / 0 MEDIUM, 3 LOW); Oracle confirm PASS
Revision: 3
Component: 85 (corrective errata) -- supervisor TUI, UI event adaptation, UI model/render
Depends on: 10-supervisor-tui.md Sec. 5.1 (UiEvent sketch), Sec. 6 (status line);
            17-ui-transcript-errata.md Sec. 3 (RB-01 transcript styling);
            ConversationRole/ConversationEntry (include/ymh/ui/ui_model.hpp:96-116);
            25-ui-ux-errata.md (status model fields);
            45-ui-interaction-errata.md Sec. 9.2 (45-D7 derived connectivity);
            62-composer-caret-and-step-limit-errata.md Sec. 2 (62-D4/62-D5);
            06-agent-loop.md Sec. 5.7/5.8 (turn terminal events);
            src/agent/agent_loop.cpp:1433-1445 (Cancelled vs Failed routing);
            07-tools-execution.md Sec. 14.1 (tool success/failure taxonomy);
            01-session.md (S10, event log); include/ymh/session/events.hpp:80-84;
            00-architecture.md Sec. 9.11, Sec. 54 (failure-mode tags), Sec. 55;
            56-dsh-fidelity-gap-closure-errata.md (56-D6 non-mirror convention)
Scope: one confirmed defect -- a provider turn failure (`EventType::TurnFailed`
       with `code != "StepLimitExceeded"`) is adapted to `UiEvent::ErrorOccurred`
       and appended to the same conversation list as tool rows with no turn/tool
       association, so it renders directly under the last tool result and is read
       as that tool's result. This spec pins a turn-scoped attribution and
       rendering for turn-level failures. Design only; no implementation code.
Supersedes: the `ErrorOccurred` adaptation for non-`StepLimitExceeded`
            `TurnFailed` at `src/ui/ui_event_adapter.cpp:137-145` (the
            `ErrorOccurred` push is `:142`); the generic
            `Role::System` / dim `error: <message>` rendering of a turn failure
            at `src/ui/ui_model.cpp:1646-1654`; any statement that `TurnFailed`
            and the supervisor/rename notices share one rendering.
Amends: 45-D7 (additive: a turn failure is still an `ApiConnectivity::Error`
        outcome, now via its own event); 10 Sec. 5.1 `UiEvent` variant (additive
        `TurnFailedNotice`); the role enum (additive `ConversationRole::TurnError`,
        `include/ymh/ui/ui_model.hpp:96-106`);
        62-D5 (unchanged, cross-referenced -- `StepLimitReached` remains a
        non-error `Notice`).
Tracker: DESIGN_STATUS.md row 85 is orchestrator-owned and added only at
         promotion (AGENTS.md rule); it is not created or edited by this spec.
Revision note: Rev 3 closes the gate-85 re-review (1 HIGH N1 + 1 MEDIUM N2 + 1
         LOW N3) and completes G1. N1: the banner label was doubled (model stored
         `"turn failed: ..."` AND the renderer prepended `turn <N> `). Now the
         model stores only the bare message (`entry.text = message or code`) and
         the renderer composes the single canonical label `turn <N> failed: <message>`
         (`turn failed: <message>` when the id is 0), so the label is synthesised
         once (fixed in the model branch, the render case, Sec. 2.4, TF2, 85-D3/D5,
         85-U1, 85-U6a). N2: TF11's false claim ("only `StreamOutcome::Failed`
         reaches `appendTurnFailed`") is corrected -- `appendTurnFailed` has many
         call sites and all but the stream-failure site pass explicit codes; only
         `:1443` derives a provider code via `mapAgentError`. N3: citation ranges
         corrected (`ui_event.hpp:252-279`; `agent_loop.cpp:1433-1445`;
         `agent.cpp:26-34`; `26-dsh-alignment-part2.md:62-66`; the role anchor is
         the header enum, not doc 17).
Revision note (Rev 2, retained): Rev 2 closed the gate-85 review (2 MEDIUM G1/G2
         + 3 LOW G3/G4/G5). G1: 85-U6 split into a normalized structural golden
         check (`Theme{false}`) and a raw SGR check (`\x1b[31m`/`\x1b[1m`) under
         `Theme{true}`. G2: `ConversationEntry::turn` gains a production consumer
         (`render_entry`). G3: 85-U6 asserts absence of the real tool-header glyphs
         U+25B8 / U+2717. G4: TF11 pins cancellation routing. G5: ranges corrected
         (`:137-145`, `:1512-1556`).
```

---

## 1. Purpose and scope

### 1.1 The defect (confirmed chain)

A provider/loop turn failure is presented indistinguishably from a tool result.
The exact shipped chain, with anchors:

1. The loop records a durable `payload::TurnFailed{turn, code, message}`
   (`include/ymh/session/events.hpp:80-84`; codes from
   `AgentErrorCode`, `include/ymh/agent/agent.hpp:56-72`). The terminal-event
   invariant is 26-I4 (`docs/design/26-dsh-alignment-part2.md:62-66`).
2. `UiEventAdapter::adapt` maps `EventType::TurnFailed` to a frontend event
   (`src/ui/ui_event_adapter.cpp:137-145`): `code == "StepLimitExceeded"` becomes
   `UiEvent::StepLimitReached` (`:139-140`); **every other code** becomes
   `UiEvent::ErrorOccurred{session, payload.message}` (`:142`). The payload's
   `turn` and `code` are dropped.
3. `UiModel::apply` handles `ErrorOccurred` (`src/ui/ui_model.cpp:1646-1654`): it
   sets `status.last_error`, forces `status.api_state = ApiConnectivity::Error`,
   and **appends** a `ConversationEntry{role = ConversationRole::System,
   text = "error: " + message}` to `state.conversation.entries` -- the same list
   that holds tool rows.
4. `render_conversation` stacks each entry with no separator except after
   `Reasoning` (`src/ui/ui_render.cpp:511-533`), so the `System` row lands
   immediately under the last `ConversationRole::Tool` entry (`render_tool_entry`,
   `src/ui/ui_render.cpp:330-386`). The `System` rendering is a dim paragraph
   (`src/ui/ui_render.cpp:479-481`), visually merging with the dim tool output
   just above it.

Observed symptom: `tool/call edit_file` -> `tool/result ok` ->
`error: transport timeout` reads as the `edit_file` result. The user then asked
"what was the error exactly?" and the next turn's assistant -- whose context has
no memory of the failure -- answered "no error", contradicting the UI. The
surface was the only place the failure was visible, and it was misattributed.

The root cause is **attribution**, not colour: a turn-level outcome is emitted as
a session-level `ErrorOccurred` with no association to the turn it terminates.

### 1.2 Correcting the record (what is already right)

- `project_state` (`src/ui/ui_event_adapter.cpp:64-69`) already returns
  `AgentState::Error` for a non-step `TurnFailed` and `Idle` for
  `StepLimitExceeded`; this spec does not change it (TF9).
- `62-D5` already established the precedent: a turn outcome gets its **own**
  frontend-only event (`StepLimitReached`) and its **own** role
  (`ConversationRole::Notice`), rendered bold yellow rather than dim
  (`src/ui/ui_render.cpp:482-484`; `docs/design/62-composer-caret-and-step-limit-errata.md:64`).
  The turn-failure defect is the missing half of that same pattern.
- `45-D7` already pins "any `ErrorOccurred` -> `Error`" for derived connectivity
  (`docs/design/45-ui-interaction-errata.md:904-918`); this spec preserves that
  rule and adds a parallel path for the turn-failure event (TF5).

### 1.3 What this spec changes, in one sentence

A non-`StepLimitExceeded` `TurnFailed` is adapted to a **dedicated
frontend-only `TurnFailedNotice`** carrying its `turn` id, and rendered as a
**turn-scoped banner** -- `turn <N> failed: <message>` in bold red, preceded by a
blank separator row, in a new `ConversationRole::TurnError` -- so it can never be
read as a tool result, while `ErrorOccurred` keeps its current role and rendering
for the non-turn sources.

### 1.4 Supersession and amendment map

| # | Clause superseded / amended (file:line) | New text (this spec) |
|---|---|---|
| A1 | `src/ui/ui_event_adapter.cpp:142`: non-step `TurnFailed` -> `ErrorOccurred{session, payload.message}` | Becomes `TurnFailedNotice{session, payload.turn, payload.code, payload.message}`. See Sec. 2.2. |
| A2 | `src/ui/ui_model.cpp:1646-1654`: turn failures rendered as `Role::System` `error: <message>` | Turn failures get their own branch producing `ConversationRole::TurnError` with bare `text = (message or code)` and `entry.turn` set; the renderer composes the label (85-D5). `ErrorOccurred`'s branch is unchanged for its remaining callers. See Sec. 2.2, TF4. |
| A3 | 10 Sec. 5.1 `UiEvent` variant (`include/ymh/ui/ui_event.hpp:252-279`) | **Additive** `TurnFailedNotice` variant. See Sec. 2.2. |
| A4 | the role enum (`ConversationRole`, `include/ymh/ui/ui_model.hpp:96-106`) | **Additive** `ConversationRole::TurnError`. `System`/`Notice` semantics unchanged. See Sec. 2.4. |
| A5 | `ConversationEntry` (`include/ymh/ui/ui_model.hpp:108-116`) | **Additive** `std::optional<TurnId> turn` (default `std::nullopt`). See Sec. 2.2, TF10. |
| A6 | `45-D7` (`docs/design/45-ui-interaction-errata.md:904-918`) | **Amended additively**: the "any `ErrorOccurred` -> `Error`" rule stands; a `TurnFailedNotice` is a **second** event that also sets `ApiConnectivity::Error` (a turn failure is a real error, unlike 62-D5's recoverable stop). See TF5. |
| A7 | `62-D5` (`docs/design/62-composer-caret-and-step-limit-errata.md:64`) | **Unchanged, cross-referenced.** `TurnFailed{StepLimitExceeded}` still adapts only to `StepLimitReached` (a `Notice`), never `TurnFailedNotice` (TF8). |

Nothing else is superseded. `ErrorOccurred` is retained as a general notice sink
(see Sec. 2.5); its `Role::System` rendering, `last_error`/`api_state` updates,
and `45-D7` semantics are intact.

### 1.5 Scope boundaries (out of scope)

- **Giving the model memory of the failure.** The durable `TurnFailed` is already
  in the event log (`events.hpp:80-84`); the assistant's next-turn context not
  including it is an agent-context concern, not a rendering one, and is not
  changed here.
- **A distinct "stopped" agent-state edge.** `62-F8` already accepts reusing the
  running->`Idle` edge; unchanged.
- **Transport-level `TurnCancelled`.** A cancellation is already a distinct
  durable event (`events.hpp:75-78`) and is not an error; unchanged. The loop
  dispatches `StreamOutcome::Cancelled` to `payload::TurnCancelled`
  (`src/agent/agent_loop.cpp:1433-1438`) and returns before any failure path;
  `appendTurnFailed` is called from many sites with explicit codes (`:358`
  `InboxFull`, `:1491`/`:1546` `StepLimitExceeded`), and only the
  `StreamOutcome::Failed` site (`:1443`) derives a provider code via
  `mapAgentError`. No site passes `Cancelled`/`None`, so a
  `TurnFailed{Cancelled}` (or `{None}`) is not produced in normal operation;
  TF11 pins this and the conservative fallback if it ever occurs.
- **A generic error taxonomy in the banner.** The banner shows the message (or
  the code when the message is empty, TF-F1); it does not re-map error codes.

---

## 2. The fix: turn-scoped failure attribution

### 2.1 Chosen mechanism, and the alternatives rejected

**Chosen: a dedicated frontend-only `UiEvent::TurnFailedNotice` (carrying
`turn`, `code`, `message`), rendered through a dedicated `ConversationRole::TurnError`
as a turn-scoped banner with the label `turn <N> failed: <message>`
(or `turn failed: <message>` when the turn id is 0) and a leading blank
separator row.**

This is the `62-D5` pattern applied to the complementary half of the problem: a
turn outcome gets its own event and its own role, so it is categorically not a
tool row and not the dim `System` chrome an `ErrorOccurred` produces today.

Rejected alternatives, with the anchor that rules them out:

- **Relabel only: keep `ErrorOccurred` and change its `text` to
  `"turn failed: " + message`.** Insufficient: the entry keeps
  `ConversationRole::System` and is appended with no attribution, so it still
  lands flush under the tool row and stays dim (`src/ui/ui_render.cpp:479-481`);
  the misread survives. It also cannot carry the turn id, so the visible banner
  cannot name the turn it terminates (TF2).
- **Reuse `ConversationRole::Notice` for turn failures.** Conflates a
  **recoverable** stop (62-D5's step ceiling, deliberately non-error,
  `docs/design/62-composer-caret-and-step-limit-errata.md:64`, 62-I8) with a
  **real failure**. A yellow, non-error notice would under-signal a transport
  timeout and blur the 62-D5 decision. Rejected (TF5).
- **Add `turn` to `ErrorOccurred` and keep everything else.** `ErrorOccurred` is
  also produced by non-turn sources -- `surface_notice`
  (`src/ui/supervisor.cpp:1514-1525`, S3 connection/agent-unavailable notices)
  and the rename-failure path (`src/ui/supervisor.cpp:3310-3323`) -- which have
  no turn. Widening the shared struct forces a meaningless `turn` at every call
  site and still leaves the rendering un-attributed. Rejected; the two concerns
  are split instead (TF1, TF4).
- **Status-bar-only surface (never append an entry).** The problem is that the
  transcript lied; removing the transcript row removes the visible trace of a
  failure and regresses 62-I10's "turn outcomes are conversation content"
  principle (the failure should scroll with the turn). Rejected.

### 2.2 Interface sketch

New frontend event in `include/ymh/ui/ui_event.hpp` (10 Sec. 5.1 amendment, A3).
ASCII:

```cpp
// 85-D1: a turn-level (non-recoverable) loop/provider failure. Distinct from
// ErrorOccurred so the model can attribute it to its turn -- not to the
// preceding tool row -- and render a turn-scoped banner. Frontend-only; never
// serialized on the wire (U4, ui_event.hpp:3-7).
struct TurnFailedNotice {
    SessionId   session;
    TurnId      turn = 0;   // payload::TurnFailed::turn (events.hpp:81)
    std::string code;       // AgentErrorCode name, e.g. "ProviderFailed"
                            // (agent.hpp:56-72, agent_error_code_name)
    std::string message;    // payload::TurnFailed::message (events.hpp:83)
};
```

`TurnId` is `std::uint64_t` (`include/ymh/session/ids.hpp:14`), reachable in
`ymh::ui` through its enclosing namespace. The `UiEvent` variant
(`include/ymh/ui/ui_event.hpp:252-279`) gains `TurnFailedNotice,` beside
`ErrorOccurred` and `StepLimitReached`.

Adapter change in `src/ui/ui_event_adapter.cpp:137-145`:

```cpp
case EventType::TurnFailed: {
    const auto payload = event.payload.get<payload::TurnFailed>();
    if (payload.code == "StepLimitExceeded") {
        events.push_back(UiEvent{StepLimitReached{session, payload.message}});
    } else {
        events.push_back(UiEvent{TurnFailedNotice{
            session, payload.turn, payload.code, payload.message}});
    }
    break;
}
```

Model state in `include/ymh/ui/ui_model.hpp`:

```cpp
enum class ConversationRole : std::uint8_t {
    // ... User, Assistant, Reasoning, Tool, System, Context ...
    Notice,      // 62-D5: recoverable, non-error (yellow)
    // 85-D2: a turn-level failure. Distinct from System (dim "error:") and
    // Notice (yellow, recoverable): rendered as a bold red banner, attributed
    // to its turn. Never carries tool_name/tool_call_id.
    TurnError,
};

struct ConversationEntry {
    ConversationRole role = ConversationRole::Assistant;
    std::string      text;
    std::string      tool_name;
    std::string      tool_call_id;
    bool             streaming = false;
    std::optional<MessageSource>  source  = std::nullopt;
    std::optional<ContextFormed>  context = std::nullopt;
    // 85-D3: the durable turn this entry is attributed to. std::nullopt for
    // every entry not tied to a turn (all pre-85 roles). Set only by TurnError
    // and READ by render_entry (ui_render.cpp), which composes the
    // "turn <N> failed:" label when set and non-zero (the production consumer; TF2).
    std::optional<TurnId> turn = std::nullopt;
};
```

New `UiModel::apply` branch in `src/ui/ui_model.cpp`, beside the `ErrorOccurred`
branch (`:1646-1654`):

```cpp
} else if constexpr (std::is_same_v<T, TurnFailedNotice>) {
    state.status.last_error = e.message;
    state.status.api_state  = ApiConnectivity::Error;   // 85-D4; 45-D7 amended
    ConversationEntry entry;
    entry.role = ConversationRole::TurnError;
    entry.turn = e.turn;                                // 85-D3 attribution
    entry.text = e.message.empty() ? e.code : e.message;  // 85-D5: bare message
    state.conversation.entries.push_back(std::move(entry));
    dirty.mark(e.session, UiDirtyFlag::Conversation | UiDirtyFlag::Status |
                               UiDirtyFlag::Attention);
}
```

The `conversation_event` classifier (`src/ui/ui_model.cpp:1408-1425`) gains
`std::is_same_v<T, TurnFailedNotice> ||` so the banner raises `scroll.unseen`
while scrolled up, exactly like `ErrorOccurred`/`StepLimitReached` (TF6).
`entry_presentation` (`src/ui/ui_model.cpp:946-978`) gains
`case ConversationRole::TurnError: return Presentation::FinalAnswer;` beside
`Notice` (`:960-961`) so the banner is not dimmed as intermediate chrome (TF7).

Renderer change in `render_entry` (`src/ui/ui_render.cpp:445-509`), as a new
`case` beside `Notice` (`:482-484`):

```cpp
case ConversationRole::TurnError: {
    rows.push_back(ftxui::text(""));                       // turn-boundary separator
    // 85-D3/85-D5: read the attribution and compose the canonical label HERE.
    // entry.text is the bare message; the renderer owns the single label.
    const std::string turn_part =
        entry.turn.has_value() && *entry.turn != 0
            ? "turn " + std::to_string(*entry.turn) + " "
            : "turn ";
    rows.push_back(paint(ftxui::paragraph(turn_part + "failed: " + entry.text) |
                             ftxui::bold,
                         ftxui::Color::Red, theme));       // 85-D6 banner
    break;
}
```

`paint` (`src/ui/ui_render.cpp:34`) and `ftxui::paragraph` are already in scope
in this translation unit. `paragraph` (not `text`) is chosen so a long
transport/provider message wraps fully instead of clipping (TF-F5).
`render_entry`'s read of `entry.turn` is the field's production consumer (G2);
the model stores the bare `entry.text` and the **renderer owns the single
canonical label** (`turn <N> failed: <message>`, 85-D5), so the label is never
synthesised twice (N1).

### 2.3 Construction, ownership, and lifetime

- The `TurnFailedNotice` value is constructed on the adapter's `events` vector in
  `UiEventAdapter::adapt` and moved into `UiModel::apply`; it is transient and
  never stored (Sec. 3).
- The `ConversationEntry` is owned by `SessionUiState::conversation.entries`
  (`src/ui/ui_model.cpp:1649-1652` pattern), the same owner as tool rows. It is
  supervisor-local presentation state, not durable; the durable source of truth
  remains the `payload::TurnFailed` event (`events.hpp:80-84`). On a fresh attach
  or a replay the adapter re-derives the `TurnFailedNotice` from the durable
  event, so the banner is **reconstructible**, not invented (TF-F2).
- No new persistent state and no new registry/session-DB column are introduced.

### 2.4 Exact rendered text and shape

Rendered text (normative): the ASCII label

```
turn <N> failed: <message>          (N = entry.turn when set and non-zero)
turn failed: <message>              (otherwise)
```

where `<message>` is `payload::TurnFailed::message` (or `code` when the message
is empty, TF-F1), and `N` is the `turn` id read from the entry by `render_entry`
(85-D3). The model stores **only the bare message** in
`entry.text = (message.empty() ? code : message)`; the **renderer** composes the
single canonical label above (85-D5), so no prefix is ever concatenated twice
(N1). The shape is:

- a blank separator row, then
- one bold red (`ftxui::Color::Red`) wrapped paragraph, `turn <N> failed: ...`,
  rendered at the entry's presentation (`FinalAnswer`, undimmed).

ASCII of a full sequence (tool row, then the new banner):

```
 > edit_file  {"file":"a.txt"}              <- Tool header (render_tool_entry, :330)
   ok                                       <- Tool result body (still dim, unchanged)
                                            <- blank separator row (NEW, 85-D6)
 turn 7 failed: transport timeout           <- NEW banner: bold + Color::Red (paint)
```

Distinctness is threefold and each is independently checkable:

| Prior surface | This banner |
|---|---|
| `System`: dim paragraph, text `error: <m>` (`:480`) | bold, colour `Red`, text `turn <N> failed: <m>` |
| `Notice`: bold yellow, text `<m>` (`:483-484`) | bold red, prefixed `turn <N> failed:` label |
| `Tool`: header `> name` + dim body (`:351-386`) | no U+25B8/U+2717 glyph, no name, its own separated row, red |

### 2.5 `ErrorOccurred` retention (what stays)

`ErrorOccurred` is **retained unchanged** for the non-turn sources and is not
split into sub-types:

- `surface_notice` (`src/ui/supervisor.cpp:1514-1525`) -- S3 connection /
  agent-unavailable notices for a modeled session.
- The rename-failure callback (`src/ui/supervisor.cpp:3310-3323`).
- Any future supervisor-originated notice with no turn association.

Its branch (`src/ui/ui_model.cpp:1646-1654`) still sets `last_error` and
`api_state = Error`, still appends `ConversationRole::System` text
`"error: " + message`, and still raises `scroll.unseen`. `45-D7`'s rule (A6) is
unchanged; the turn-failure path is additive and parallel (TF4, TF5).

---

## 3. State lifetime table (new and changed state)

Every row is normative. "Restart" means supervisor process restart; "switch"
means session switch within a live supervisor (the model outlives the switch).

| State | Type | Created | Owned by | Destroyed / evicted | Survives restart? | Survives switch? | Crash path |
|---|---|---|---|---|---|---|---|
| Turn-failure presentation event | `UiEvent::TurnFailedNotice` in the adapter's `events` vector | `UiEventAdapter::adapt`, `EventType::TurnFailed` branch (`src/ui/ui_event_adapter.cpp:137-145`) | the local `std::vector<UiEvent>` in `adapt`, for one call | when `adapt` returns (moved into `apply`) | no (re-derived on replay) | n/a | unwinds with `adapt`; nothing retained |
| Turn-failure conversation entry | `ConversationEntry{role = TurnError}` in `SessionUiState::conversation.entries` | `UiModel::apply`, `TurnFailedNotice` branch (`src/ui/ui_model.cpp`, new, beside `:1646-1654`) | `SessionUiState` of the session | session close/eviction or model destruction; re-derived on replay/attach | no (supervisor-local; rebuilt from the durable `TurnFailed`) | yes (per-session state) | process death discards it; a reconnect re-derives it from the event log |
| Entry turn attribution | `std::optional<TurnId> ConversationEntry::turn` | with the entry (`85-D3`) | the entry | with the entry | no | yes | same as the entry. Read by `render_entry` (`src/ui/ui_render.cpp:445-509`), which composes the `turn <N> failed:` label (the production consumer). |
| Status last-error | `StatusModel::last_error` (string) | `UiModel::apply`, `TurnFailedNotice` branch | `SessionUiState::status` | overwritten by the next error/notice; never cleared otherwise (unchanged from 45-D7) | no | yes | same as before |
| Status connectivity | `StatusModel::api_state = ApiConnectivity::Error` | same branch | `SessionUiState::status` | overwritten by the next `Ok` (`AssistantMessageFinished`/`TokenUsageUpdated`) or `Error` (45-D7) | no | yes | same as before |

No other state is introduced or changed. `ConversationModel::seen_ids_`
(`include/ymh/ui/ui_model.hpp:138`) is not touched: `TurnFailedNotice` carries no
`MessageId`, so it is not deduped and a replayed failure appends as it did before.

---

## 4. Invariants

| ID | Invariant | Where |
|---|---|---|
| **TF1** | A `TurnFailed` with `code != "StepLimitExceeded"` is adapted to exactly one `TurnFailedNotice` (carrying `session`, `turn`, `code`, `message`) and never to `ErrorOccurred`; `TurnFailed{StepLimitExceeded}` adapts only to `StepLimitReached`. | Sec. 2.2; `ui_event_adapter.cpp:137-145` |
| **TF2** | Applying a `TurnFailedNotice` appends exactly one `ConversationEntry` with `role == ConversationRole::TurnError`, `turn == e.turn`, and bare `text == (message.empty() ? code : message)`. `render_entry` reads `entry.turn` and composes the single label `turn <N> failed: <message>` (id set and non-zero) or `turn failed: <message>` (otherwise); the label is never concatenated twice (the field's production consumer). | Sec. 2.2, 2.4; `85-D3/D5`; `ui_render.cpp:445-509` |
| **TF3** | The `TurnError` entry never carries `tool_name` or `tool_call_id` (both stay empty), is never registered in `ToolModel`, and is never associated with a `ToolFinished`. Its rendered row begins with the `turn ...` label and carries neither the U+25B8 tool glyph nor the U+2717 failure glyph (`ui_render.cpp:351-353`). It is not a tool result. | Sec. 2.4; TF-F3 |
| **TF4** | `ErrorOccurred` is retained for non-turn sources and its model branch (`Role::System`, `"error: " + message`, `last_error`, `api_state = Error`) is unchanged. The two events do not coalesce. | Sec. 2.5; `ui_model.cpp:1646-1654`; `supervisor.cpp:1514-1525`, `:3310-3323` |
| **TF5** | A `TurnFailedNotice` sets `api_state = ApiConnectivity::Error` (a real failure). A `StepLimitReached` does not touch `api_state` (62-D5/62-I8 intact). Both keep `45-D7`'s "any `ErrorOccurred` -> `Error`" rule. | Sec. 2.2; `docs/design/45-ui-interaction-errata.md:904-918`; 62-D5 |
| **TF6** | `TurnFailedNotice` is conversation content: it is in the classifier (`ui_model.cpp:1408-1425`) and raises `scroll.unseen` when the view is scrolled up, exactly like `ErrorOccurred` and `StepLimitReached` (62-I10). | Sec. 2.2 |
| **TF7** | `entry_presentation(TurnError) == Presentation::FinalAnswer`; the banner is never classified `System`/`Context` (`Chrome`) and never `Tool`/`Reasoning` (`Intermediate`), so it is not dimmed. | Sec. 2.2; `ui_model.cpp:946-978` |
| **TF8** | A step-ceiling stop never produces a `TurnError` banner: `StepLimitExceeded` still yields a `Notice`. 62-D5 is intact. | Sec. 2.2; TF1 |
| **TF9** | `UiEventAdapter::project_state` is unchanged: a non-step `TurnFailed` projects `AgentState::Error`, `StepLimitExceeded` projects `Idle`. | `ui_event_adapter.cpp:64-69` |
| **TF10** | `ConversationEntry::turn` defaults to `std::nullopt`; only the `TurnError` branch sets it. No existing entry construction changes role or text. | Sec. 2.2; `ui_model.hpp:108-116` |
| **TF11** | Cancellation is not a turn failure. `StreamOutcome::Cancelled` is dispatched to `payload::TurnCancelled` and returns before any failure path (`src/agent/agent_loop.cpp:1433-1438`). `appendTurnFailed` has many call sites (e.g. `:358` `InboxFull`, `:1491` `StepLimitExceeded`, `:1546` `StepLimitExceeded`); all but one pass an explicit non-cancellation code, and the **only** dynamic code source is `mapAgentError(response.error.code)` at `:1443`, reached only under `StreamOutcome::Failed` (`:1439-1445`). No call site passes `AgentErrorCode::Cancelled`/`None`, so a `TurnFailed{Cancelled}`/`{None}` is not produced in normal operation. If one ever occurred it is treated conservatively as a real failure and renders the red `TurnError` banner (fail loud, never silent); `mapAgentError(LLMErrorCode::Cancelled) == AgentErrorCode::Cancelled` (`src/agent/agent.cpp:26-34`) is the only path that could produce it. | Sec. 1.5; `src/agent/agent_loop.cpp:1433-1445`, `:358`, `:1491`, `:1546` |

---

## 5. Failure modes

Tags follow `00` Sec. 54's F-tag convention.

| ID | Failure mode | Disposition |
|---|---|---|
| **TF-F1** | `payload.message` is empty for a non-step failure. | `entry.text` takes the `code` fallback (`85-D5`), so the canonical banner renders `turn <N> failed: <code>` and is never a bare `turn failed:`. The durable event still records `code` (`events.hpp:82`). |
| **TF-F2** | A replayed/attached session re-derives the failure. | Handled: `TurnFailed` is durable (`events.hpp:80-84`); the adapter re-derives the same `TurnFailedNotice`, so the banner is reconstructed, not lost (Sec. 2.3). |
| **TF-F3** | The tool result immediately preceding is mutated or the banner is indexed as a tool. | Prevented: the branch only appends a `TurnError` entry and never touches `ToolModel` (`ui_model.cpp`, tool branches are `:1512-1556`); TF3. |
| **TF-F4** | A supervisor notice (`surface_notice` / rename failure) is rendered as a turn failure. | Prevented: those call sites pass `ErrorOccurred` directly (`supervisor.cpp:1523`, `:3322`), which keeps the `System` branch; only `EventType::TurnFailed` produces `TurnFailedNotice` (TF1, TF4). |
| **TF-F5** | A long provider/transport message overflows the row. | Handled: the banner uses `ftxui::paragraph` (wrapping), not `ftxui::text`, and is `FinalAnswer` (undimmed) (`ui_render.cpp:482-484`; TF7). |
| **TF-F6** | A `TurnError` with no preceding entry (first content of a turn) renders a leading blank row. | Accepted: the blank separator is harmless and keeps the banner turn-scoped; no state depends on it. |
| **TF-F7** | `ErrorOccurred` and `TurnFailedNotice` for one session in sequence are merged. | Prevented: distinct event types and distinct branches (`ui_model.cpp:1646-1654` and the new branch); nothing coalesces entries (TF4). |
| **TF-F8** | `StepLimitExceeded` is misrouted to the failure branch. | Prevented: the adapter branches on the exact code string before the else (`ui_event_adapter.cpp:139-143`); TF1/TF8. |
| **TF-F9** | A `TurnFailed{Cancelled}` (or `{None}`) reaches the adapter and renders a red `TurnError` banner for a user cancellation. | Accepted and bounded: cancellation does not take this path -- `StreamOutcome::Cancelled` -> `TurnCancelled` (`agent_loop.cpp:1433-1438`) returns first, and the only dynamic `appendTurnFailed` code is the `StreamOutcome::Failed` site `:1443` (`mapAgentError`); no site passes `Cancelled`/`None` (`:358`, `:1491`, `:1546` show explicit codes); TF11. If it ever did, the conservative red banner is the safe rendering (never silent), and the durable record still names the code. |

---

## 6. Test plan

New test file `tests/unit/errata85_turn_failure_test.cpp` (added to
`tests/CMakeLists.txt` beside `unit/errata81_ui_test.cpp`), plus the golden tests
appended to `tests/unit/ui_render_golden_test.cpp` (it owns the local
`normalize`/`render_to_ansi`/`element_ansi` golden helpers). All hermetic; no live
LLM. Golden SGR checks follow the repo pattern: `normalize()` strips ANSI
(`ui_render_golden_test.cpp:38-58`), and `Theme{false}` sets `color = false` so
`paint()` (`src/ui/ui_render.cpp:34-39`) emits no SGR; colour is therefore
asserted on the **raw** `render_to_ansi(..., Theme{true})` string (cf.
`ui_render_golden_test.cpp:2971-2982`).

| ID | Test | Asserts |
|---|---|---|
| 85-U1 | `Errata85.TurnFailedNoticeAppendsTurnErrorEntry` (model) | TF2/TF5: after `model.apply(UiEvent{TurnFailedNotice{session, 7, "ProviderFailed", "transport timeout"}})`, the last `conversation.entries` item has `role == ConversationRole::TurnError`, bare `text == "transport timeout"` (the renderer adds the label), `turn == 7`, empty `tool_name`/`tool_call_id`; `status.last_error == "transport timeout"`; `api_state == ApiConnectivity::Error`. |
| 85-U2 | `Errata85.TurnFailedAdaptationRoutesByCode` (adapter) | TF1/TF8: `Event{TurnFailed{turn=3, code="ProviderFailed", message="transport timeout"}}` adapts to exactly one `TurnFailedNotice{turn=3, code="ProviderFailed"}` and zero `ErrorOccurred`; `Event{TurnFailed{code="StepLimitExceeded"}}` adapts to one `StepLimitReached` (62-U6 preserved) and zero `TurnFailedNotice`. |
| 85-U3 | `Errata85.TurnFailureIsNotAToolResult` (model) | TF3: seed a `ToolFinished` for `edit_file`, then apply `TurnFailedNotice`; the tool entry is byte-identical, the new entry's role is `TurnError` (not `Tool`), `tools.by_id` does not contain it, and no `ToolFinished` references it. |
| 85-U4 | `Errata85.TurnFailureRaisesUnseenWhileScrolled` (model) | TF6/TF7: with `scroll.following = false`, applying `TurnFailedNotice` raises `scroll.unseen` and appends a `TurnError` entry (mirrors 62-U8). |
| 85-U5 | `Errata85.ErrorOccurredStillAppendsSystemEntry` (regression) | TF4: `UiEvent{ErrorOccurred{session, "rename failed: InvalidParams"}}` still appends `role == ConversationRole::System`, `text == "error: rename failed: InvalidParams"`, and sets `api_state == Error` (45-D7 unchanged). |
| 85-U6a | `UiRenderGolden.TurnFailureBannerStructure` (golden, structural) | TF2/TF3/TF7: build a model with a `ToolFinished` (`> edit_file` + `ok`) then a `TurnFailedNotice{turn=7}`; render `normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}))`; assert the line `turn 7 failed: transport timeout` is present; that its transcript row is preceded by a blank row; and that the banner row carries neither the U+25B8 tool glyph nor the U+2717 failure glyph (`ui_render.cpp:351-353`), i.e. it is not a tool row (fixes G3; the meaningful "not a tool row" signal is the model-level `role == TurnError` in 85-U1/U3). |
| 85-U6b | `UiRenderGolden.TurnFailureBannerColor` (golden, colour) | TF2 (85-D6): on the **raw** `render_to_ansi(model, TerminalSize{72, 20}, Theme{true})` string, locate the label position and assert `trailing_sgr(raw, label_pos)` (`ui_render_golden_test.cpp:106`) contains both the red SGR (`\x1b[31m`) and the bold SGR (`\x1b[1m`) (implements G1's colour half; the `trailing_sgr` pattern is the repo's, cf. `ui_render_golden_test.cpp:3339`). |
| 85-U7 | `UiRenderGolden.StepLimitNoticeUnchanged` (regression) | TF8/62-D5: a `StepLimitReached` renders the label without a `turn ... failed:` prefix and, on the raw `Theme{true}` string, with the yellow SGR rather than the red SGR (`ui_render.cpp:482-484` vs the `TurnError` case). |
| 85-U8 | `Errata85.TurnFailureReplayReconstructsBanner` (optional, adapter+model) | TF-F2: converting a replayed `TurnFailed` event through `adapt` then `apply` yields the same `TurnError` entry as the live path. |

---

## 7. dsh (DeepSeek Harness) mapping

dsh is the architectural reference (`00` Sec. 55; `26-dsh-alignment.md`;
`26-dsh-alignment-part2.md`). This errata is a presentation-attribution fix; it
does not move a dsh capability boundary. Per 56-D6, every non-mirror cell states
why the divergence is acceptable and cites a concrete anchor.

| dsh concept | ymh equivalent | Disposition |
|---|---|---|
| A stream/turn ends in exactly one terminal event | durable `payload::TurnFailed` | **Mirrored, unchanged.** 26-I4 (`docs/design/26-dsh-alignment-part2.md:62-66`) already pins one `TurnEnded`/`TurnCancelled`/`TurnFailed` per turn; this spec only changes how that terminal is presented. Anchor: `include/ymh/session/events.hpp:80-84`. |
| Turn outcome rendered at turn scope, not as a tool row | `UiEvent::TurnFailedNotice` + `ConversationRole::TurnError` banner | **Mirrored (precedent 62-D5).** dsh surfaces an error/aborted finish as the turn's terminal fact (`docs/design/26-dsh-alignment.md:270-277`), not inside a tool result; ymh now does the same via the 62-D5 event/role pattern. Anchor: `62-D5` (`docs/design/62-composer-caret-and-step-limit-errata.md:64`). |
| Provider failure carries a structured failure fact (`LlmFailure`: stable `code`, `status?`, `requestId?`) | `TurnFailedNotice::message` (text) plus `code` (string) | **Non-mirror (deliberate scope).** ymh's `payload::TurnFailed` is intentionally flat (`{turn, code, message}`, `events.hpp:80-84`) and the `LlmFailure` struct is not carried (`26-dsh-alignment.md:272-283`); this spec keeps the flat shape and renders the human message, adding no field to the durable event. Acceptable because the failure is already durable and actionable, and widening the wire/schema is out of scope for a rendering fix. Anchor: `26-dsh-alignment.md:272-283`; `events.hpp:80-84`. |
| Frontend-only presentation events never serialized | `UiEvent` (supervisor-local) | **Mirrored, unchanged.** `TurnFailedNotice` is a `UiEvent`, produced only inside the supervisor and never on the wire. Anchor: `include/ymh/ui/ui_event.hpp:3-7`; `05` Sec. 5.2; `54 D15`. |
| Rich per-step error attribution (a failure belongs to a step) | turn-level attribution only (`entry.turn`, rendered as `turn <N>`; no step) | **Non-mirror (unbuilt seam).** ymh now renders the turn-level attribution (85-D3, `render_entry`), but `payload::TurnFailed` carries no `StepId` (`events.hpp:80-84`) and no consumer needs a step reference; adding one is a candidate future amendment, recorded here rather than silently. Anchor: `events.hpp:80-84`; `06` Sec. 5.8 (turn terminal events). |
| Derived connectivity from observed outcomes | `StatusModel::api_state = Error` | **Mirrored, extended additively.** A turn failure is a real error outcome; it sets `Error` on its own branch, parallel to `ErrorOccurred`. Anchor: `45-D7` (`docs/design/45-ui-interaction-errata.md:904-918`). |

---

## 8. Decisions

| ID | Decision |
|---|---|
| **85-D1** | A non-`StepLimitExceeded` `TurnFailed` adapts to a new frontend-only `UiEvent::TurnFailedNotice{session, turn, code, message}` instead of `ErrorOccurred`. Rationale: attribution requires the turn id and a distinct event; the 62-D5 pattern is the precedent. (TF1) |
| **85-D2** | A new `ConversationRole::TurnError` renders the failure as a turn-scoped banner; it is distinct from `System` (dim `error:`) and `Notice` (yellow, recoverable). Rationale: a real failure must not share the recoverable-notice or dim-chrome surface. (TF3, TF7) |
| **85-D3** | `ConversationEntry` gains `std::optional<TurnId> turn`, set only by `TurnError` and **read by `render_entry`**, which composes the `turn <N> failed:` label (85-D5). Rationale: the attribution is visible in production (not dead metadata), gives the field a concrete caller, and adds no durable schema. (TF2, TF10) |
| **85-D4** | `TurnFailedNotice` sets `api_state = ApiConnectivity::Error`; `StepLimitReached` still does not. Rationale: a provider/loop failure is a real error; a step-ceiling stop is recoverable (62-D5). (TF5, TF8) |
| **85-D5** | The rendered label is exactly `turn <N> failed: <message>` when the turn id is set and non-zero, else `turn failed: <message>`; `<message>` falls back to `<code>` when empty. The model stores only the bare message in `entry.text`; the renderer composes the whole label once (never a doubled prefix, N1). Rationale: naming the turn makes the attribution explicit and exercises the field; red bold plus a separator row disambiguate from tool rows. (TF2, TF-F1) |
| **85-D6** | The banner is a bold `Color::Red` `ftxui::paragraph` preceded by a blank separator row. Rationale: wrapping keeps long messages whole, and the separator visually detaches the row from the tool result above. (Sec. 2.4, TF-F5) |
| **85-D7** | `ErrorOccurred` is retained, not split, for supervisor/rename/non-turn notices; its `System` rendering and `45-D7` semantics are unchanged. Rationale: those sources have no turn, and splitting adds surface for no semantic gain. (TF4) |

---

## 9. References

- `src/ui/ui_event_adapter.cpp:64-69` (`project_state`), `:137-145` (`TurnFailed` adaptation)
- `src/ui/ui_model.cpp:946-978` (`entry_presentation`), `:1408-1425` (conversation classifier),
  `:1646-1654` (`ErrorOccurred`), `:1655-1663` (`StepLimitReached`)
- `src/ui/ui_render.cpp:34` (`paint`), `:330-386` (`render_tool_entry`),
  `:445-509` (`render_entry`), `:479-484` (`System`/`Notice`), `:511-533` (`render_conversation`)
- `src/ui/supervisor.cpp:1514-1525` (`surface_notice`), `:3310-3323` (rename-failure notice)
- `include/ymh/ui/ui_event.hpp:196-207` (`ErrorOccurred`/`StepLimitReached`), `:252-279` (`UiEvent` variant; `ErrorOccurred` `:268`, `StepLimitReached` `:269`)
- `include/ymh/ui/ui_model.hpp:96-116` (`ConversationRole`/`ConversationEntry`),
  `:264-292` (`ApiConnectivity`/`StatusModel`)
- `include/ymh/session/events.hpp:75-84` (`TurnCancelled`/`TurnFailed`); `include/ymh/session/ids.hpp:14` (`TurnId`)
- `include/ymh/agent/agent.hpp:56-72` (`AgentErrorCode`); `src/agent/agent.cpp:26-34` (`mapAgentError`, `Cancelled` case `:30-31`)
- `src/agent/agent_loop.cpp:1432-1444` (`StreamOutcome::Cancelled` -> `TurnCancelled`; `StreamOutcome::Failed` -> `appendTurnFailed`)
- `docs/design/00-architecture.md` Sec. 9.11, Sec. 54 (failure-mode tags), Sec. 55
- `docs/design/10-supervisor-tui.md` Sec. 5.1 (UiEvent), Sec. 6 (status line)
- `docs/design/17-ui-transcript-errata.md` (role rendering contract)
- `docs/design/25-ui-ux-errata.md` (status model fields)
- `docs/design/45-ui-interaction-errata.md:904-918` (45-D7 derived connectivity)
- `docs/design/62-composer-caret-and-step-limit-errata.md:64` (62-D5), `:144` (62-I8), `:146` (62-I10), `:164` (62-F8)
- `docs/design/26-dsh-alignment.md:270-283` (failure finish facts); `docs/design/26-dsh-alignment-part2.md:62-66` (26-I4)
- `docs/design/56-dsh-fidelity-gap-closure-errata.md` Sec. 3 (56-D6 non-mirror convention)
- Tests: `tests/unit/errata85_turn_failure_test.cpp` (new), `tests/unit/ui_render_golden_test.cpp` (golden), `tests/unit/ui_model_test.cpp:738` (62-U8), `tests/unit/ui_model_test.cpp:947` (45-D7 regression)
