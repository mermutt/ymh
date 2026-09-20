# 38 - Transcript Provenance Errata (amends 17-ui-transcript-errata.md, Wave-3 slice 3)

```
Status: Rev 2 written · verified: - · reviewer: -
Component: 38 (errata) - amends 17-ui-transcript-errata.md by reference only
Depends on: 17-ui-transcript-errata.md (verified), 37-message-provenance-errata.md,
            36-prompt-registry.md (verified, Rev 1), 10-supervisor-tui.md,
            18-context-errata.md, 26-dsh-alignment-part2.md (verified)
Scope: the transcript row model and rendering for Message.source / source.context
       (Instructions, Catalog, Snapshot, Notice), the projected SystemMessage
       row and its multiplicity, the UI event/adapter/model path for
       payload::ContextInjected, the tool-notice data path from
       payload::ToolResult.context to ToolCallView.notice, and the provenance
       label on tool rows
```

## 1. Purpose, authority, precedence

### 1.1 The problem this closes

`36-prompt-registry.md` §3.3 (`:640-645`) states the `17` errata obligation in
three parts:

> (a) add transcript row kinds for `ContextForm::{Instructions,Catalog,Snapshot,Notice}`
> and for the projected `SystemMessage`, (b) render `Message.source` / `source.form`,
> and (c) preserve the verified `17` render contracts.

Spec 36 supplies the types; `17` owns the rendering (`36-prompt-registry.md:644-645`).
No `17` errata existed. This file is it. It amends `17` by reference, which in
turn amends `10-supervisor-tui.md` by reference (`17 §1`).

Today the transcript cannot render any of this:

- `ConversationRole` has only `User, Assistant, Reasoning, Tool, System`
  (`include/ymh/ui/ui_model.hpp:80-86`); there is no context row kind.
- The `UiEvent` variant has no `ContextInjected` or system-message alternative
  (`include/ymh/ui/ui_event.hpp:203-226`).
- The adapter has no `EventType::ContextInjected` case and no
  `EventType::LlmRequestHeader` case; injected context is silently dropped
  (`src/ui/ui_event_adapter.cpp:82-...`).
- No UI code reads `MessageSource` or `ContextForm`; a repo search of
  `src/ui` + `include/ymh/ui` finds only the unrelated `ContextSnapshot`
  `tool.provenance` string (`src/ui/ui_render.cpp:1009`) and the session-export
  `ContextInjected` case (`src/ui/session_export.cpp:259-260`).

### 1.2 Authority and relationship to the other specs

- `36-prompt-registry.md` owns the provenance vocabulary and the obligation.
  This errata does not restate the types; it renders them.
- `37-message-provenance-errata.md` owns the durable payload fields this errata
  consumes. This errata is downstream of 37.
- `17-ui-transcript-errata.md` owns the five render contracts (RB-01/02/08/10/11)
  and the test-impact register (`17 §9`). This errata is additive to those and
  names the exact clauses it extends.
- `10-supervisor-tui.md` §8.1/§8.2 owns the conversation model and render
  hierarchy that `17` already amended.
- `18-context-errata.md` owns the `/context` overlay chrome, its generation
  guard, and `has_error`. Those are **preserved**, not changed (OQ-1).
- `26-dsh-alignment-part2.md` §4.3.9.2 (`:986`) pins `LlmRequestHeader` as
  "no row; never rendered". This errata's §4.2 system row is derived from that
  event, so it is a **recorded contradiction**, not a silent reversal: the
  conflict is OQ-2, and 26 wins until it is resolved. 26 is added to the
  dependency/reference list so the conflict is visible.

Where this file and `26`/`36`/`17` disagree, the prior verified spec wins and
the disagreement is an open question (§13).

### 1.3 Verified-against-the-tree note

Every `file:line` below was re-read against the working tree. The anchors are:
`ui_model.hpp:80-94` (role enum + entry), `:131-140` (`ToolCallView`);
`ui_event.hpp:77-123` (payloads), `:203-226` (variant);
`ui_event_adapter.cpp:82-...` (adapt switch), `:87-91`, `:107-112`, `:120-...`;
`ui_model.cpp:618-633` (apply predicate), `:638-643`, `:812-819`, `:828-849`;
`ui_render.cpp:176-209` (`render_tool_entry`), `:242-267` (`render_entry`),
`:269-286` (`render_conversation`), `:1104-1117` (`build_ui`).
The `EventType::LlmRequestHeader` payload is `include/ymh/session/events.hpp:233-245`.

## 2. Amendment register

| ID | Amended clause (17) | Verified code anchors | New behaviour |
|---|---|---|---|
| **38-A1** | §3 RB-01 transcript styling; §4 RB-02 fold | `ui_model.hpp:80-94` | `ConversationRole` gains `Context`; `ConversationEntry` gains `std::optional<MessageSource> source` and `std::optional<ContextFormed> context` |
| **38-A2** | §3 RB-01; §4 RB-02 | `ui_event.hpp:77-123,203-226`; `ui_event_adapter.cpp:82-...` | new UI events `ContextInjected` and `SystemPromptShown`; `UserMessage`/`AssistantMessageFinished`/`ToolFinished` gain `source` (and `context` for the tool); adapter cases for `ContextInjected`, `LlmRequestHeader`, and the three existing cases |
| **38-A3** | §4 RB-02 (fold/collapse) | `ui_model.cpp:618-633,638-643`; `ui_render.cpp:242-267` | apply a `Context` entry and render a form-labelled row |
| **38-A4** | §4 RB-02 (tool row) | `ui_render.cpp:176-209`; `ui_model.cpp:737-745` | a `Tool` row with `context->form == Notice` renders a dim `notice: <summary>` suffix; `apply` sets `ToolCallView.notice` from `ui::ToolFinished.context` (§4.3) |
| **38-A5** | §9 test-impact register | `ui_render_golden_test.cpp:178,302,462`; `render_golden_test.cpp:156` | extend the goldens with the four context forms and the system row |
| **38-A6** | §8 cross-spec consistency | `context_request.hpp:3-27`; `context_snapshot.hpp:61`; `ui_render.cpp:1002` | preserve the overlay generation guard and `has_error` (no change) |

This errata introduces **no new durable event type, no new wire field, and no
new protocol message**. The UI reads existing durable events (`37-D10`,
`36-D16`).

## 3. The provenance-carrying row model (38-A1, 38-D1, 38-D2)

The two-axis provenance model (`36 §3.1`, `:597-598`) is preserved: `role`
answers **who produced** the row, `context->form` answers **what kind of thing**
it is. The row model gains one role member and two optional provenance fields:

```cpp
// include/ymh/ui/ui_model.hpp (amended)

enum class ConversationRole : std::uint8_t {
    User,
    Assistant,
    Reasoning,
    Tool,
    System,
    Context,     // 38-A1: injected context (source.kind == Plugin)
};

struct ConversationEntry {
    ConversationRole               role = ConversationRole::Assistant;
    std::string                    text;
    std::string                    tool_name;
    std::string                    tool_call_id;
    bool                           streaming = false;
    // 38-A1 additions:
    std::optional<MessageSource>   source;   // who produced it (37 §5)
    std::optional<ContextFormed>   context;  // form + sections/summary (36 §3.1)
};
```

`ContextFormed` and `MessageSource` are the `36 §3.1` types (in
`include/ymh/agent/provenance.hpp`). This errata does not redefine them.
`context` is `ContextFormed`, not `ContextForm`, because §5 renders
`context->form` for the label and `context->sections` for a `Snapshot` body
(§5, 38-I2); `ContextForm` alone carries neither the section names nor the
`Notice` summary.

**Why one `Context` member and a `context` field, not four literal members
(38-D1).** `36 §3.1` insists the axes are independent; four literal role
members (`Instructions`, `Catalog`, ...) would fuse the form axis into the role
axis and leave `ContextForm::{None,Relay,Recall}` unrepresentable. The row kind
is the pair `(ConversationRole::Context, context->form)`. The alternative, four
literal members, is recorded in OQ-3.

`ToolCallView` (`ui_model.hpp:131-140`) gains an optional
`std::optional<ContextFormed> notice;` so a tool row can carry a `Notice` and
its `summary` (`36 §3.3`, `ToolResultMessage.context`) without changing the
durable payload (38-A4). The type is `ContextFormed` because §5 renders
`notice->summary`; a bare `ContextForm` would lose the text.

## 4. The UI event path for `ContextInjected` and the system row (38-A2, 38-D5)

### 4.1 Injected context

The adapter currently has no `EventType::ContextInjected` case, so injected
instructions, catalog entries, snapshots, and notices never reach the
transcript. This errata adds a UI event and an adapter case:

```cpp
// include/ymh/ui/ui_event.hpp (amended)

struct ContextInjected {
    SessionId                session;
    MessageId                id;
    Role                     role = Role::User;   // 37 §5.2: default User
    std::string              text;
    MessageSource            source;              // 37 §5
    ContextFormed            context;             // 37 §5
};

// Added to the UiEvent variant (ui_event.hpp:203-226).
```

Adapter obligation (`src/ui/ui_event_adapter.cpp`, the switch at `:82-...`):

```cpp
case EventType::ContextInjected: {
    const auto payload = event.payload.get<payload::ContextInjected>();
    events.push_back(UiEvent{ui::ContextInjected{session, payload.id, payload.role,
                                                 payload.text, payload.source,
                                                 payload.context}});
    break;
}
```

The payload fields are the `37` amendment; the adapter copies them verbatim
(`38-I1`). The existing UI events also gain the provenance fields the transcript
needs (38-D5). `include/ymh/ui/ui_event.hpp` (amended):

```cpp
struct UserMessage {              // :77-81
    SessionId     session;
    MessageId     id;
    std::string   text;
    MessageSource source;         // 38-D5: from payload::UserMessage.source
};

struct AssistantMessageFinished { // :95-100
    SessionId            session;
    MessageId            message;
    std::string          text;
    std::optional<Usage> usage;
    MessageSource        source;  // 38-D5: from payload::AssistantMessage.source
};

struct ToolFinished {             // :115-123
    SessionId                  session;
    ToolCallId                 id;
    std::string                name;
    payload::ToolOutcome       outcome = payload::ToolOutcome::Ok;
    std::string                output;
    bool                       truncated = false;
    std::optional<std::string> error;
    MessageSource              source;   // 38-D5: from payload::ToolResult.source
    ContextFormed              context;  // 38-D5: from payload::ToolResult.context
};
```

Adapter obligations, all in the `adapt` switch (`ui_event_adapter.cpp:82-...`):

- `EventType::UserMessage` (`:87-91`): append `payload.source` to the
  `ui::UserMessage`.
- `EventType::AssistantMessage` (`:107-112`): append `payload.source` to the
  `ui::AssistantMessageFinished`.
- `EventType::ToolResult` (`:120-127`): append `payload.source` and
  `payload.context` to the `ui::ToolFinished`.
- `EventType::ContextInjected` (new): the case above.

`AssistantMessageStarted` and `AssistantTextDelta` carry no source; the source
arrives once, on the settled `AssistantMessageFinished` (38-I1). `ToolStarted`
carries none; the tool row's source and notice arrive on `ToolFinished`, which
is why the model applies them there (§4.3).

The `conversation_event` predicate (`ui_model.cpp:618-633`) gains
`ContextInjected` and `SystemPromptShown`, so a new context/system row triggers
`scroll.onNewContent()` (`:870-876`) like every other conversation row.

### 4.2 The projected `SystemMessage` row (38-D3, 38-D4)

`SystemMessage` is a projection-level type (`36 §3.3`), not a durable event. Its
durable carrier is `LlmRequestHeader.system_prompt_digest`, with the full text
only under `session.persist_prompt_text` (`36 §3.3`, `:637`;
`events.hpp:239-240`). The UI is event-driven, so the `17` transcript reads the
header, not the assembler:

```cpp
// include/ymh/ui/ui_event.hpp (amended)

struct SystemPromptShown {
    SessionId                  session;
    std::string                prompt_digest;   // always present
    std::optional<std::string> text;            // present iff persist_prompt_text
};
```

Adapter obligation: a `case EventType::LlmRequestHeader` builds
`SystemPromptShown{session, header.system_prompt_digest, header.system_prompt}`.
The
`std::optional` is already the opt-in carrier (`agent_loop.cpp:522-524`), so the
default (`persist_prompt_text == false`, `36-D11`) yields a digest-only row and
no prompt text ever reaches the screen (38-I5, `36-I10`).

The `UiModel::apply` handler maps `SystemPromptShown` to a
`ConversationRole::System` entry (reusing the existing dim system render at
`ui_render.cpp:262-264`) with
`source = MessageSource{Kind::Plugin, plugin = "system-prompt"}` and `text` =
the digest (and the text when present). Reusing `System` rather than a new
member keeps the existing `ErrorOccurred`/compaction system rows
(`ui_model.cpp:812-819`, `:828-849`) unchanged.

**Multiplicity (38-D11).** `LlmRequestHeader` is a changed snapshot, not a
per-dispatch record (`events.hpp:230-232`): a session can log several. The
transcript must therefore keep **at most one** System prompt row per session.
`apply` identifies it by `role == ConversationRole::System &&
source->plugin == "system-prompt"`; when it exists, `SystemPromptShown` updates
that row's `text` in place, and only the first occurrence appends. Without this,
every header change would stack a duplicate System row. The
`ErrorOccurred`/compaction system rows are a different `source` (`plugin`
empty) and are never touched (38-I10, 38-F9).

**Recorded contradiction with 26 (OQ-2).** `26-dsh-alignment-part2.md` §4.3.9.2
(`:986`) pins `LlmRequestHeader` as "no row; never rendered", and `36 §3.3`
routes the system row from the projected `SystemMessage` (producer
`SessionContextAssembler::assemble`). Deriving the row from `LlmRequestHeader`
in the supervisor therefore contradicts a verified spec. This errata does not
silently reverse 26: it records the conflict as OQ-2, keeps the header-derived
row as the proposed mechanism, and treats 26 as authoritative until the user
resolves it. If 26's "never rendered" stands, §4.2, 38-A2 (the
`SystemPromptShown` half), and 38-D3/38-D4 are withdrawn; the Context rows,
the tool notice, and the provenance labels do not depend on the system row.

### 4.3 The tool-notice data path (38-D10)

`payload::ToolResult` carries `source` and `context` (37 §5). Neither reaches
the transcript today: the adapter's `ToolFinished` (`ui_event_adapter.cpp:120-127`)
copies only id/name/outcome/output/truncated/error, and `UiModel::apply`'s
`ToolFinished` branch (`ui_model.cpp:737-745`) sets the tool view's
finished/outcome/truncated/output and never a notice. The pinned path is:

1. `ui::ToolFinished` gains `MessageSource source` and `ContextFormed context`
   (§4.1); the adapter copies `payload.source`/`payload.context` (`:122-124`).
2. `UiModel::apply`, `ToolFinished` branch (`ui_model.cpp:737-745`): after the
   existing `ToolCallView` / `ConversationEntry` updates,
   - `call.notice = (e.context.form == ContextForm::Notice)`
     `? std::optional<ContextFormed>{e.context} : std::nullopt;`
   - for the matching tool `ConversationEntry`:
     `entry.source = e.source;` and
     `entry.context = (e.context.form == ContextForm::None)`
     `? std::nullopt : std::optional<ContextFormed>{e.context};`
   - mark `UiDirtyFlag::Tools | UiDirtyFlag::Conversation` (already done).
3. `render_tool_entry` (`ui_render.cpp:176-209`) reads `call.notice` (§5).

The conversation entry is created earlier, on `ToolStarted`
(`ui_model.cpp:712-724`), which carries no provenance; the fields are filled on
the later `ToolFinished`. A session that ends between the two leaves the row
without source/notice, which is correct (the result never settled). No new
durable event or wire field is introduced; the data was already durable in
`payload::ToolResult` and only the UI-event field was missing (38-D8, 38-I11).

## 5. Rendering rules (38-A3, 38-A4, 38-D6)

`render_entry` (`ui_render.cpp:242-267`) gains a `ConversationRole::Context`
case. The row is rendered in the existing dim/header idiom and is labelled by
the form:

| `context->form` | Row label | Notes |
|---|---|---|
| `Instructions` | `instructions` | workspace instructions (`AGENTS.md` / `CLAUDE.md`) |
| `Catalog` | `catalog` | skill catalog `<system-reminder>` |
| `Snapshot` | `runtime context` | the supersession snapshot |
| `Notice` | `notice` | removal/change notices |
| `None` / `Relay` / `Recall` | `context` | fallback label (OQ-4) |

A `Context` row is emitted only when `entry.context.has_value()`; a missing
value renders the fallback `context` label. The label is prefixed to the row
like the existing `tool:` header (`ui_render.cpp:188`). When `source->plugin` is
non-empty it is appended in parentheses, so a row names its producer. The body
uses the existing bounded markdown/text path (`render_entry` User/Assistant
bodies) and the fold rule of `17 §4`: a collapsed context row is exactly one
header line, and `expand_all_folds` (`ui_render.cpp:186`) reveals the body. A
`Snapshot` body may be multi-section; the row renders the
`context->sections[i].name` names as a dim header and each
`context->sections[i].text` body under the fold.

`render_tool_entry` (`ui_render.cpp:176-209`) gains one suffix: when
`call.notice.has_value()` (set only for `form == Notice`, §4.3), append a dim
`notice: <call.notice->summary>` line after the tool header. This consumes the
`ToolResultMessage.context` notice (`36 §3.3`) without changing the durable
`payload::ToolResult`.

`render_conversation` (`ui_render.cpp:269-286`) is unchanged: it iterates the
entries and delegates to `render_entry`. No new chrome row is introduced
(38-I6).

## 6. Preserved contracts (38-A6, 38-D7)

Spec 36 §3.3 asks that the `17` errata preserve "fixed chrome budget, generation
guard, `has_error`". Those three are not `17` contracts; they live in `18`:

- **Fixed chrome budget.** The `/context` overlay chrome is pinned to exactly
  `kContextOverlayChromeRows = 10` rows (`18-context-errata.md:1088`,
  `:1000-1205`). This errata adds rows only inside the conversation viewport, so
  the overlay chrome is untouched.
- **Generation guard.** The `/context` reply guard is
  `bump_context_generation` / `context_reply_is_current`
  (`include/ymh/ui/context_request.hpp:3-27`), applied in
  `src/ui/supervisor.cpp:2087-2130`. This errata does not touch that path.
- **`has_error`.** `ContextServerEntry::has_error` (`context_snapshot.hpp:61`)
  is rendered at `src/ui/ui_render.cpp:1002`. This errata does not touch the
  overlay renderer.

The obligation this errata pins is therefore: the new provenance rows must fit
inside the existing conversation viewport with **no** change to the overlay
chrome, the generation guard, or `has_error` (38-I6). OQ-1 records the
attribution mismatch in spec 36.

## 7. Invariants

These extend `17` and `18`. `38-I*` are local to this errata.

- **38-I1 (provenance carried).** Every `ConversationEntry` produced from a
  provenance-bearing payload carries its `source` (and `context` when the
  payload has one). The adapter copies them verbatim; the model never invents
  them.
- **38-I2 (two axes).** Role and form are independent. A context row is
  `ConversationRole::Context` with `context->form` selecting the label; the form
  never changes the role. `context` is a `ContextFormed`, so the row also
  carries the `sections`/`summary` the form requires.
- **38-I3 (consumer only).** The transcript is a read-only consumer of the event
  stream. Rendering provenance never appends, mutates, or reorders the durable
  log (`01` I1, I19).
- **38-I4 (system row from the header).** The projected system row is produced
  from `LlmRequestHeader` and always carries `prompt_digest`.
- **38-I5 (digest-only default).** With `session.persist_prompt_text == false`,
  no prompt text is rendered; only the digest (`36-I10`, `36-D11`).
- **38-I6 (preserved contracts).** The overlay chrome budget, the generation
  guard, and `has_error` handling are unchanged.
- **38-I7 (pure render).** `render_entry` and `render_conversation` stay pure
  over the model (`17` `RenderIsPure`, `ui_render_golden_test.cpp:186`); they
  read no clock, bus, or log.
- **38-I8 (bounded rows).** A context body is subject to the existing fold rule;
  a collapsed row is exactly one header line. No unbounded text enters the
  viewport.
- **38-I9 (adapter totality).** `ContextInjected` and `LlmRequestHeader` are
  handled explicitly; every other unhandled event type stays ignored as today.
- **38-I10 (single system prompt row).** A session has at most one
  `ConversationRole::System` row with `source->plugin == "system-prompt"`; a
  later `SystemPromptShown` updates it in place. `ErrorOccurred`/compaction
  system rows (different/empty `plugin`) are unaffected (38-D11, 38-F9).
- **38-I11 (tool notice data path).** `payload::ToolResult.context` reaches
  `ToolCallView.notice` only through the pinned path of §4.3: adapter ->
  `ui::ToolFinished.context` -> `apply`. A `Notice` form always yields a
  `notice` with a non-empty `summary` (36-I8); the tool row's `source` is set
  on the same event.

## 8. Failure modes

### 8.1 Shared findings (`F1`-`F12`, §54)

- **F1 (path/process isolation).** Not applicable: no path is resolved here.
- **F3 (late event after close).** A late `ContextInjected` for a closed session
  must not create a phantom entry; the adapter's session routing applies.
- **F10 (resume-suspended).** A resumed session re-derives its rows from the
  replayed events; provenance is re-read, never fabricated.

The remaining shared findings are out of scope. The UI is a consumer
(`38-I3`).

### 8.2 Component-local failure modes

- **38-F1 (injected context invisible).** The adapter drops
  `EventType::ContextInjected`, so no row appears. Guard: 38-I9; adapter test.
- **38-F2 (wrong form label).** A `Notice` renders as `instructions`. Guard:
  38-I2; the form-label table test.
- **38-F3 (prompt text leaked).** The full system prompt renders with
  `persist_prompt_text == false`. Guard: 38-I5; a digest-only golden.
- **38-F4 (unbounded row).** A large snapshot floods the viewport. Guard:
  38-I8; the fold golden.
- **38-F5 (chrome regression).** A new row kind changes the overlay chrome or
  the viewport height. Guard: 38-I6; the `18` overlay goldens
  (`ui_render_golden_test.cpp:973`).
- **38-F6 (provenance dropped).** A tool or user row loses its `source`. Guard:
  38-I1.
- **38-F7 (stale overlay reply applied).** The generation guard is bypassed.
  Guard: 38-I6.
- **38-F8 (`has_error` regression).** The server error marker stops rendering.
  Guard: 38-I6.
- **38-F9 (duplicate system rows).** Each `LlmRequestHeader` change appends a
  new System prompt row instead of updating the existing one. Guard: 38-I10;
  the multiplicity test (12.1) and a two-header golden (12.2).
- **38-F10 (tool notice dropped).** `payload::ToolResult.context` never reaches
  `ToolCallView.notice` because `ui::ToolFinished` lacks the field or `apply`
  ignores it. Guard: 38-I11; the notice data-path test (12.1).
- **38-F11 (system row contradicts 26).** The header-derived System row ships
  without resolving 26 §4.3.9.2. Guard: OQ-2; do not implement §4.2 until the
  open question is closed.

## 9. dsh mapping

| dsh concept | ymh type / site | Reference |
|---|---|---|
| `MessageSourceMap` / `ContextFormed` | `ConversationEntry.source` / `.context` | `36 §3.1`; `26 part2 §4.3.10:1023-1068` |
| `SystemMessage` projection | `SystemPromptShown` UI event from `LlmRequestHeader` (OQ-2 vs 26) | `36 §3.3:652-657`; `events.hpp:233-245` |
| `ToolResultMessage.context` notice | `ToolCallView.notice` dim suffix | `36 §3.3:661-669` |
| transcript row kind | `ConversationRole` + `context->form` | this errata §3, §5 |

**Deliberate divergence (stated).** dsh's frontend consumes a typed message
stream; ymh's supervisor consumes the durable event stream over the wire
(`10 §4.5`), so the system row is derived from `LlmRequestHeader` rather than
from a projected `SystemMessage` object. The two-axis model is preserved.

## 10. Dependencies

**Upstream (must be verified before slice-3 code):**

| Spec | What this errata needs | Status |
|---|---|---|
| `17-ui-transcript-errata.md` | the render contracts and test-impact register being extended | verified |
| `37-message-provenance-errata.md` | the payload fields (`source`/`context`) consumed here | Rev 1 written |
| `36-prompt-registry.md` | the provenance vocabulary and the `17` obligation | verified Rev 1 |
| `10-supervisor-tui.md` | the conversation model and render hierarchy | verified |
| `18-context-errata.md` | the preserved chrome/generation/`has_error` contracts | verified |

**Downstream:** none; the transcript is a leaf consumer.

**Code sites this errata touches (for the implementer; no code here):**
`include/ymh/ui/ui_model.hpp:80-94,131-140`;
`include/ymh/ui/ui_event.hpp:77-123,203-226`;
`src/ui/ui_event_adapter.cpp:82-...` (the `UserMessage` `:87-91`,
`AssistantMessage` `:107-112`, `ToolResult` `:120-127` cases gain provenance,
plus the new `ContextInjected`/`LlmRequestHeader` cases);
`src/ui/ui_model.cpp:618-633,638-643,712-724,737-745,870-876`;
`src/ui/ui_render.cpp:176-209,242-267,269-286`;
tests `tests/unit/ui_render_golden_test.cpp`,
`tests/unit/render_golden_test.cpp`, `tests/unit/ui_event_adapter_test.cpp`,
`tests/unit/ui_model_test.cpp`.

## 11. Decision register

| ID | Decision | Add./Brk. | Owning spec |
|---|---|---|---|
| **38-D1** | One `ConversationRole::Context` member plus a `ContextFormed context`, not four literal role members; the two provenance axes stay independent. | Add. | 17, 38 |
| **38-D2** | `ConversationEntry` gains `std::optional<MessageSource> source` and `std::optional<ContextFormed> context`; `ToolCallView` gains `std::optional<ContextFormed> notice` (the form record, so `sections`/`summary` survive). | Add. | 38 |
| **38-D3** | The projected system row reuses `ConversationRole::System` and is produced from `LlmRequestHeader`. Contingent on OQ-2 (26 §4.3.9.2 says "never rendered"). | Add. | 38 |
| **38-D4** | The system row always shows `system_prompt_digest`; it shows the full text only when `header.system_prompt` is present. | Add. | 36, 38 |
| **38-D5** | New UI events `ContextInjected` and `SystemPromptShown`; adapter cases for `EventType::ContextInjected` and `EventType::LlmRequestHeader`. Existing `ui::UserMessage` / `ui::AssistantMessageFinished` gain `MessageSource source`, and `ui::ToolFinished` gains `source`/`ContextFormed context`; their adapter cases copy the payload fields. | Add. | 38 |
| **38-D6** | The form-label table (`instructions`, `catalog`, `runtime context`, `notice`, fallback `context`) is pinned; labels use the existing dim/header idiom. | Add. | 38 |
| **38-D7** | The `/context` overlay chrome budget, generation guard, and `has_error` handling are untouched. | None | 18, 38 |
| **38-D8** | No new durable event type, wire field, or protocol message (`36-D16`, `37-D10`). | None | 36, 37, 38 |
| **38-D9** | A `Tool` row with `context->form == Notice` renders a dim `notice: <summary>` suffix, consuming `ToolResultMessage.context` without changing the durable payload. | Add. | 36, 38 |
| **38-D10** | The tool-notice data path is pinned (§4.3): `payload::ToolResult.context` -> `ui::ToolFinished.context` -> `apply` sets `ToolCallView.notice` and the entry's `source`/`context`. | Add. | 38 |
| **38-D11** | At most one System prompt row per session; `SystemPromptShown` updates it in place (`source.plugin == "system-prompt"`). | Add. | 38 |
| **38-D12** | The `LlmRequestHeader`-derived System row contradicts verified 26 §4.3.9.2 (`:986`) and 36's assembler route; recorded as OQ-2, not silently resolved. 26 is added to deps/refs. | None (recorded) | 26, 36, 38 |

## 12. Test plan

Strategy follows `17 §9` and `§44`. Hermetic fixtures come from `FakeLLM` runs.

### 12.1 Unit tests

- **Adapter.** `EventType::ContextInjected` produces a `ui::ContextInjected` with
  role/source/context copied verbatim; `EventType::UserMessage`,
  `EventType::AssistantMessage`, and `EventType::ToolResult` produce
  `ui::UserMessage.source`, `ui::AssistantMessageFinished.source`, and
  `ui::ToolFinished.source`/`.context`; `EventType::LlmRequestHeader` produces a
  `SystemPromptShown` with the digest always and `text` only when the header
  carries it; a header without text yields `text == nullopt`.
- **Model.** Applying `ContextInjected` appends a `ConversationRole::Context`
  entry with `source`/`context`; applying `SystemPromptShown` appends a
  `System` entry; both mark `UiDirtyFlag::Conversation`.
- **System row multiplicity.** Two `SystemPromptShown` events for one session
  leave exactly one `System` row with `source->plugin == "system-prompt"`,
  whose `text` is the second digest (38-I10).
- **Tool notice data path.** Applying a `ToolFinished` with
  `context.form == Notice` sets `ToolCallView.notice->summary` and the matching
  entry's `source`/`context`; with `form == None` it sets neither (38-I11).
- **Form labels.** The form-to-label mapping is total over the `ContextForm`
  enum; `None`/`Relay`/`Recall` use the fallback.
- **Two axes.** Changing `context->form` never changes `role` (38-I2).
- **Notice suffix.** A `Tool` entry with `notice` renders the dim
  `notice: <summary>` suffix; one without renders none.

### 12.2 Golden tests

- **Context rows.** One golden per form (`Instructions`, `Catalog`, `Snapshot`,
  `Notice`) plus a `Relay` fallback, collapsed and expanded.
- **System row.** A digest-only golden (default) and a text golden (opt-in); a
  two-header golden asserts a single updated System row (38-I10). Gated on
  OQ-2: skip until the 26 contradiction is resolved.
- **Tool notice.** A collapsed tool row with a `Notice` suffix whose summary
  text is rendered.
- **Chrome preservation.** Re-run the `18` overlay goldens
  (`ui_render_golden_test.cpp:973,997,1008,1018`) unchanged; assert the fixed
  chrome and `has_error` output are byte-identical.
- **Extend existing goldens.** `ConversationSnapshot`
  (`ui_render_golden_test.cpp:178`), `ExpandedToolCallShowsArguments` (`:302`),
  `CollapsedToolShowsOnlyHeaderNoBody` (`:462`), and
  `TuiRoutesAssistantMarkdownAndToolDiff` (`render_golden_test.cpp:156`) gain a
  provenance-bearing entry each.
- **Purity.** `RenderIsPure` (`ui_render_golden_test.cpp:186`) still passes with
  provenance entries.

### 12.3 Failure-mode coverage matrix

| Failure | Test |
|---|---|
| 38-F1 | 12.1 adapter (`ContextInjected`) |
| 38-F2 | 12.1 form labels |
| 38-F3 | 12.2 system digest-only |
| 38-F4 | 12.2 context rows (fold) |
| 38-F5 | 12.2 chrome preservation |
| 38-F6 | 12.1 model |
| 38-F7 | 12.2 chrome preservation (overlay) |
| 38-F8 | 12.2 chrome preservation (`has_error`) |
| 38-F9 | 12.1 system row multiplicity |
| 38-F10 | 12.1 tool notice data path |
| 38-F11 | OQ-2 (do not implement §4.2 until resolved) |

## 13. Open questions

- **OQ-1 (contract attribution).** `36 §3.3` (`:642-643`) attributes "fixed
  chrome budget, generation guard, `has_error`" to the `17` render contracts.
  They live in `18` (`18-context-errata.md:1088`; `context_request.hpp:3-27`;
  `context_snapshot.hpp:61`). This errata reads the obligation as "do not break
  them" (§6). Confirm the intended attribution.
- **OQ-2 (system row carrier and the 26 contradiction).** Two verified specs
  bear on the system row, and this errata's §4.2 conflicts with one of them:
  - `26-dsh-alignment-part2.md` §4.3.9.2 (`:986`) pins `LlmRequestHeader` as
    "no row; never rendered".
  - `36 §3.3` (`:637`) says the `17` transcript renders the projected
    `SystemMessage`, whose producer is `SessionContextAssembler::assemble`, not
    the header.

  The supervisor UI is event-driven, so this errata proposed deriving the row
  from `LlmRequestHeader`. That is a **contradiction of a verified spec**, and
  26 wins until the user resolves it; this errata does not silently reverse 26.
  Options: (a) 26's "never rendered" stands and §4.2/38-D3/38-D4 are withdrawn;
  (b) 26 is amended to permit a header-derived row; (c) the daemon ships the
  assembled `SystemMessage` as a projected event and 38 renders that instead.
  §4.2 is not implementable until this is closed (38-F11). Recorded, not
  resolved.
- **OQ-3 (row-kind shape).** `36 §3.3` says "add transcript row kinds for
  `ContextForm::{...}`". This errata realizes them as
  `(ConversationRole::Context, context->form)` rather than four literal enum
  members (38-D1), because `36 §3.1` makes the axes independent. If a literal
  expansion is required, the renderer and goldens change; recorded, not resolved.
- **OQ-4 (`Relay` / `Recall`).** `ContextForm` has `Relay` and `Recall`
  (`36 §3.1`), which the `36 §3.3` obligation does not list. This errata renders
  them with a fallback `context` label. Confirm the intended treatment.
- **OQ-5 (tool notice placement).** `36 §3.3` gives `ToolResultMessage.context`
  a Notice (retention/truncation, Wave 4). This errata renders it as a dim suffix
  on the tool row. Confirm, or whether the notice belongs on its own row.

## 14. References

- `docs/design/17-ui-transcript-errata.md` §1, §2, §3 (RB-01), §4 (RB-02), §8,
  §9, §10.
- `docs/design/36-prompt-registry.md` §3.1, §3.3, §3.5, §6 (`36-I8`), §10
  (`36-D7`, `36-D11`, `36-D16`), §12.
- `docs/design/37-message-provenance-errata.md` §4, §5 (esp. §5.3/§5.4), §6.
- `docs/design/10-supervisor-tui.md` §5.1, §5.3, §8.1, §8.2.
- `docs/design/18-context-errata.md` §5 (chrome, generation, `has_error`).
- `docs/design/26-dsh-alignment-part2.md` §4.3.9.2 (`:986`), §4.3.10
  (`:1023-1068`) (the system-row contradiction, OQ-2).
- `include/ymh/ui/ui_model.hpp`; `include/ymh/ui/ui_event.hpp`;
  `src/ui/ui_event_adapter.cpp`; `src/ui/ui_model.cpp`; `src/ui/ui_render.cpp`;
  `include/ymh/session/events.hpp:233-245`.

## 15. Revision log

| Rev | Date | Change |
|---|---|---|
| Rev 1 | 2026-09-19 | Initial write. Pins the `17` errata that `36 §3.3` requires: the `Context` row kind and provenance fields, the `ContextInjected` / `SystemPromptShown` UI events and adapter cases, the form-label rendering, the tool notice suffix, the preserved `18` chrome/generation/`has_error` contracts, invariants `38-I1`-`38-I9`, failure modes `38-F1`-`38-F8`, and decisions `38-D1`-`38-D9`. No implementation code. |
| Rev 2 | 2026-09-19 | Gate fix-pass. `ConversationEntry.context` and `ToolCallView.notice` are `std::optional<ContextFormed>` (§3, 38-D2); the tool-notice data path is pinned through `ui::ToolFinished.context` to `apply`/`ToolCallView.notice` (§4.3, 38-D10, 38-I11, 38-F10); existing UI events gain `source`/`context` (§4.1, 38-D5); the `SystemPromptShown` multiplicity is pinned to one updated row (§4.2, 38-D11, 38-I10, 38-F9); the contradiction with verified 26 §4.3.9.2 is recorded as OQ-2 (38-D12, 38-F11) and 26 is added to deps/refs. Invariants now `38-I1`-`38-I11`, failure modes `38-F1`-`38-F11`, decisions `38-D1`-`38-D12`. |
