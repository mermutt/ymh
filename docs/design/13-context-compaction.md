# 13 — Context Compaction

**Component 13.** Compaction is the mechanism that keeps a long session inside a
model's context window without destroying the durable trace. It is a
**projection / context-generation** operation (`§32`), never a deletion: the
append-only session log (`01`) remains the source of truth (D2), and a committed
compaction changes only how the next LLM request is assembled from that log.

This document pins the compaction trigger policy, the boundary-selection rule,
the summarization call through the frozen LLM seam (`08`) under the frozen
`LLMPool` discipline (`06 §5.9`, A12), the `ContextCompaction` event semantics
(the payload itself is pinned by `01 §4.5`), the projection rule that
`deriveMessages()` already folds (`01 §6.3`), invariants (`C1`–`C19`), failure
modes (`F1`–`F12` plus local `C-F1`–`C-F17b`), the DeepSeek Harness (dsh)
mapping, and the test plan.

It follows `00-architecture.md` (cited inline as `§n`), `01-session.md`
(`01 §n`), `02-persistence.md` (`02 §n`), `04-workspace-host-daemon.md`
(`04 §n`), `05-transport.md` (`05 §n`), `06-agent-loop.md` (`06 §n`),
`08-llm-provider.md` (`08 §n`), `09-permissions.md` (`09 §n`),
`10-supervisor-tui.md` (`10 §n`), and `11-m2-errata.md` (`11 §n`). Where it
cannot follow them it records the conflict under §11 *Decisions and open
questions* rather than choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

> **Naming note.** Invariants in this spec are numbered `C1`, `C2`, … (for
> "compaction") so they cannot collide with the `D1`–`D23` decisions in
> `00-architecture.md` §54 or with the `I#`/`P#`/`R#`/`H#`/`T#`/`A#`/`L#`/`Q#`/
> `U#` namespaces of specs 01–10 and 11. Architecture decisions are always
> written with the `§54` prefix (`§54 D2`); compaction invariants are bare
> (`C3`). Component-local failure modes are `C-F#` and cannot collide with the
> shared `F1`–`F12`. The **interface** name `Compactor` is frozen by `06 §5.3`
> and is not renamed here; the concrete implementation this spec pins is
> `ContextCompactor` (decision C-D1).

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
   Agent loop / TurnExecutor (06 §5, this spec's primary consumer)
        │
        │  1. assemble (06 §5.2 ContextAssembler, §31)
        │  2. estimate  (TokenEstimator, 06 §8)
        │  3. if estimate > effective threshold  ──► ContextCompactor (this spec)
        │  4. provider call under one LLMPool slot (06 §5.9, 08)
        ▼
   ┌──────────────────────────────────────────────────────────────────────┐
   │  ContextCompactor  (this spec)                                        │
   │    CompactionPolicy · boundary selection · summarization request      │
   │    → payload::ContextCompaction{boundary, summary, tokenEstimate,      │
   │                                 model, createdAt}   (01 §4.5)          │
   └───────┬───────────────────────────┬──────────────────┬────────────────┘
           │ append (lease-checked)    │ LLM call          │ usage
           ▼                           ▼                   ▼
   Session (01)                LLMProvider + LLMPool   CompactionResult::usage
   deriveMessages() folds      (06 §5.9, 08 §3.4)      → loop appends
   the compaction (01 §6.3)                             payload::TokenUsage
                                                        (06 §8, 01 §4.5, §33)
           │
           ▼
   SessionStore / SessionPersistence (02): append txn + snapshot refresh (02 §6.4)
```

Compaction is **not** a session-lifecycle operation and **not** a UI concern. It
is a core context-management service that the agent loop calls, whose only
durable output is a `ContextCompaction` event. The TUI observes that event like
any other (`§20.8`, `10 §8.2`) and may *request* compaction through a command
(§6.7), but it never performs compaction.

### 1.2 Owned responsibilities (this spec pins)

This spec pins:

- **When to compact** — the trigger and the effective threshold
  (`CompactionPolicy`, §3.1–§3.2), reconciling `AgentConfig::
  compaction_threshold_tokens` (`06 §5.1`, existing code) with a ratio-based
  policy.
- **What is summarized** — the boundary-selection rule: a `Sequence` that is a
  *compaction point* (a completed turn's terminal event), leaving
  `keep_recent_turns` turns intact (§3.3).
- **The resulting compacted context generation** — the projection fold that
  produces `[summary(System)] + messages(after boundary)`, with the assembler's
  system prompt re-added at assembly time (outside `tokenEstimate`) (§3.4,
  §4.2).
- **The `ContextCompaction` event semantics** — payload pinned by `01 §4.5`;
  this spec pins its meaning, the projection rule, and replay faithfulness
  (§4).
- **The summarization request** through the frozen `LLMProvider` seam (`08
  §3.4`) and the frozen `LLMPool` discipline (`06 §5.9`, A12), including
  auxiliary usage accounting (§5.5–§5.7).
- **The concrete compactor** `ContextCompactor` implementing the frozen
  `Compactor` seam (`06 §5.3`), plus `CompactionPolicy`, `CompactionPlan`,
  `CompactionResult` / `CompactionError` (§5).
- **The manual `/compact` surface** and its maintenance-turn semantics (§6.7),
  as an additive extension to the frozen M2 RPC catalog (decision C-D2).

### 1.3 Boundaries — deferred to other specs

- **The event payload and the fold** — `payload::ContextCompaction` and its
  `deriveMessages()` handling are owned by `01 §4.5` / `01 §6.3`. This spec
  **consumes** them and adds the boundary-validity and composition rules; it
  does not redefine the payload or the projection function.
- **The store, lease, and snapshot** — append durability, lease enforcement,
  and snapshot creation are owned by `02` (`§5.7`, `§6.1`, `§6.4`). This spec
  only *calls* `checkpoint(session)` after a committed compaction (§6.4).
- **The provider and its retry/backoff** — owned by `08` (`§2.2`, `§3.5`,
  `§3.7`). This spec consumes `LLMErrorCode` and `Usage`; it never retries
  inside the provider.
- **The pool** — `LLMPool` and `ResourceGovernor` are owned by `06 §5.9` /
  `04 §8`. This spec acquires/releases exactly one slot per summarization call
  (§5.5, A12).
- **The loop's turn/step algorithm and terminal-event taxonomy** — owned by
  `06 §5.1`. This spec defines the compaction *call sites* within it and the
  `CompactionFailed` mapping (`06 §5.7`, A-F10).
- **The estimator's default arithmetic** — the *interface* `TokenEstimator` and
  a `DefaultTokenEstimator` are pinned by `06 §8` (existing code,
  `context_assembler.hpp`); this spec pins how the estimate is *used*
  (threshold, `tokenEstimate`), not its calibration.
- **The TUI rendering** — the command palette, the conversation model, and the
  aggregate status line are owned by `10` / `§20`. This spec defines only the
  `/compact` command's core contract and the additive `CompactionMarker` view
  (§6.7).

---

## 2. Terminology and identities

### 2.1 Compaction point and boundary

A **compaction point** is a `Sequence` in the session's *resolved logical view*
(`01 §6.2`) that is the terminal event of a completed turn — i.e. the event is
one of `TurnEnded`, `TurnCancelled`, or `TurnFailed` (`01 §4.5`, I11). A
compaction point is the only legal value for a compaction's `boundary`.

Rationale: `deriveMessages()` drops every projected message whose origin
`Sequence` is `<= boundary` (`01 §6.3`). Dropping at a turn boundary guarantees
that no `tool_use` / `ToolResult` pair is split, and that no partial turn is
projected as a hole. A boundary at any other event (mid-turn, mid-step) is
rejected (C2, S7, C-F4).

### 2.2 The effective boundary and the context generation

A session may carry several `ContextCompaction` events. The **effective
boundary** of the resolved view is the maximum `boundary` over all committed
`ContextCompaction` events (0 if none). The **effective context generation** is
the message list produced by `deriveMessages(header, events())` after folding
every compaction in sequence order (`01 §6.3`):

```text
effective_context(session):
  messages := deriveMessages(header, events())          # 01 §6.3, pure
  # equivalent closed form:
  #   [system prompt, added by the assembler]
  #   + [summary of the latest compaction (System)]
  #   + projected messages whose origin Sequence > effective_boundary
  return messages
```

The **compacted context generation** produced by a *new* compaction is the
value of `effective_context` immediately after that event is appended.

### 2.3 Identities

- **`boundary`** — `Sequence` (`01 §2.1`); the compaction point. Store-global
  `AUTOINCREMENT`, preserved across fork (I10), so a boundary is meaningful in
  the resolved view of a fork as well.
- **`summary`** — the durable summary text, produced by the summarizer model.
  Projected as a `Role::System` message (`01 §6.3`).
- **`tokenEstimate`** — the estimator's estimate of the **compacted context**
  after the fold (`summary + retained tail`, §3.4), *not* the summarizer call's
  usage. The assembler-added system prompt is outside the session projection and
  is **not** counted (C15). The summarizer call's usage is returned in
  `CompactionResult.usage` and appended by the loop (§5.7).
- **`model`** — the effective summarizer model, resolved per `08 §5.2`
  (explicit per-request override first).
- **`createdAt`** — the wall-clock instant the compaction event was created,
  read from the injected `WallClock` (§5.2) — a distinct
  `std::function<std::chrono::system_clock::time_point()>` seam, **not** the
  broker's `steady_clock`-based `ClockReader` (`11 §7.2`, E21) — and never from
  a direct `system_clock::now()` inside the pure projection (C5).

`ContextCompaction` carries no `TurnId`; it is attributed to its enclosing turn
by position in the log (I11). The summarizer's `Usage`, by contrast, is
attributed explicitly via `payload::TokenUsage::turn` (§5.7).

---

## 3. Model

### 3.1 When to compact — the trigger

Compaction is attempted at exactly two places, both inside an open turn
(`06 §5.1`):

1. **Threshold trigger (proactive).** Before the provider call of a step, if
   `estimator.estimate(messages) > policy.effective_threshold_tokens()`, the
   loop attempts one compaction and re-assembles. This is the existing call
   site (`agent_loop.cpp`, the `compaction_threshold_tokens` check) generalized
   to the policy's effective threshold.
2. **Overflow retry (reactive).** If the provider returns
   `LLMErrorCode::ContextLengthExceeded`, the loop attempts **one** compaction
   and retries the request once (`06 §5.1`, `06 §5.7`). If the re-assembled
   context still overflows, the turn fails with
   `TurnFailed{CompactionFailed}` (`06 §5.7`, A-F10).

A third, **manual** trigger exists for the `/compact` command (§6.7); it is
serviced by the same core path and is subject to the same invariants.

The trigger is **advisory and best-effort** (decision (h) in `06 §14.1`): a
failed compaction never fails a turn that still fits; only an unrecoverable
overflow fails the turn (C13, C-F1).

### 3.2 `CompactionPolicy` (pinned)

The tunables named across this spec have no owning type elsewhere; 13 pins them
in one struct (cf. `ToolConfig` in `07 §5.5`, `PermissionConfig` in `09 §3.3`,
`PersistenceConfig` in `02 §4.1`, `HostConfig` in `04 §4.1`). The daemon
constructs `CompactionPolicy` at startup from the layered config (§37) and the
CLI wiring injects it into the `ContextCompactor`; nothing reads global state.

```cpp
namespace ymh {

struct CompactionPolicy {
    // Master switch. `false` => no compaction, ever (C7). Constructed from
    // config; the existing `AgentConfig::compaction_threshold_tokens == 0`
    // maps to `enabled = false` (back-compat, decision C-D4).
    bool        enabled = false;

    // Absolute trigger, in estimated tokens. `> 0` wins over `threshold_ratio`.
    // Maps from `[agent] compaction_threshold_tokens` (§37).
    std::size_t threshold_tokens = 0;

    // Ratio trigger used when `threshold_tokens == 0` and the model window is
    // known: effective = floor(threshold_ratio * context_window_tokens).
    // Built-in default 0.80 (leave 20% headroom).
    double      threshold_ratio = 0.80;

    // The model's context window, in tokens. 0 => unknown. When unknown and
    // `threshold_tokens == 0`, compaction is disabled rather than guessed
    // (decision C-D8).
    std::size_t context_window_tokens = 0;

    // Headroom reserved for the response when the ratio trigger is used.
    std::size_t reserve_output_tokens = 4'096;

    // Number of complete, most-recent turns that must remain uncompacted. The
    // boundary is the terminal Sequence of the turn `keep_recent_turns` turns
    // before the head (§3.3). Built-in default 2.
    std::size_t keep_recent_turns = 2;

    // Do not compact a prefix smaller than this many projected messages; the
    // summary would cost more than it saves (C-F14).
    std::size_t min_prefix_messages = 4;

    // Upper bound on the summary, in estimated tokens (C8). The compactor
    // truncates a longer summary deterministically (§8.2 C-F3).
    std::size_t max_summary_tokens = 1'024;

    // Upper bound on the summary, in serialized bytes. Must be
    // <= PersistenceConfig::max_payload_bytes minus envelope headroom
    // (02 §4.1, 01 S10, 02 P-F15).
    std::size_t max_summary_bytes = 256u * 1024u;

    // Per-request model override for the summarization call (08 §5.2, first
    // non-empty wins). Empty => the session model.
    std::string summarizer_model;

    // Loop guard: at most this many compaction attempts per turn (C-F8).
    std::size_t max_compactions_per_turn = 1;

    // Whether a provider `ContextLengthExceeded` triggers the one-shot
    // compaction retry (06 §5.1). `false` disables the reactive path.
    bool        retry_on_context_length = true;

    // ---- derived -----------------------------------------------------------

    [[nodiscard]] bool        is_enabled() const noexcept {
        return enabled && effective_threshold_tokens() > 0;
    }

    // The absolute trigger actually compared against the estimate. Pure.
    [[nodiscard]] std::size_t effective_threshold_tokens() const noexcept {
        if (threshold_tokens > 0) {
            return threshold_tokens;
        }
        if (context_window_tokens > reserve_output_tokens) {
            const auto window = context_window_tokens - reserve_output_tokens;
            return static_cast<std::size_t>(threshold_ratio * static_cast<double>(window));
        }
        return 0;   // unknown window and no absolute trigger => disabled
    }
};

} // namespace ymh
```

- **Configuration shape.** The policy maps to a `[agent.compaction]` TOML table
  under the existing `[agent]` section (§37); `threshold_tokens` also accepts
  the legacy flat `[agent] compaction_threshold_tokens` key. Layering is
  `§37`/`08 §5.3` (built-in → global → project → profile → CLI).
- **Validation.** `effective_threshold_tokens() > 0` is required for
  `enabled = true`; `max_summary_bytes` must be `<=
  PersistenceConfig::max_payload_bytes` minus envelope headroom. A violation is
  a `ConfigError` at load (like `PolicyConfigError`, `09 §3.3`), never a
  runtime surprise (C18).
- **`keep_recent_turns = 0`** is legal and means "summarize everything up to the
  current head turn"; it is still bounded by `min_prefix_messages` and C2/C3.

### 3.3 What is summarized — boundary selection (normative)

The compactor selects the boundary deterministically from the resolved view:

```text
select_boundary(session, policy):
  events   := session.events()                      # ascending Sequence (01 §6.2)
  points   := [seq of every TurnEnded/TurnCancelled/TurnFailed in events]  # 01 §4.5
  if points.size() <= policy.keep_recent_turns:
      return NotNeeded                              # nothing safely summarizable
  # The candidate boundary is the terminal event of the turn that is
  # `keep_recent_turns` complete turns before the head.
  candidate := points[points.size() - 1 - policy.keep_recent_turns]
  if candidate <= effective_boundary(session):
      return NotNeeded                              # no advance (C3)
  prefix := deriveMessages(header, events[0 .. candidate])   # 01 §6.3 free fn
  if prefix.size() < policy.min_prefix_messages:
      return NotNeeded                              # C-F14
  full   := deriveMessages(header, events)          # current effective projection
  # The effective projection is exactly the summarize set followed by the
  # retained tail, so the tail count is the difference (§3.4).
  return CompactionPlan{ boundary = candidate,
                         prefix_messages = prefix.size(),
                         kept_messages   = full.size() - prefix.size() }
```

Rules:

1. **Turn-aligned only.** `candidate` is always a compaction point (§2.1, C2).
   A manual request that names a non-turn sequence is clamped to the nearest
   earlier compaction point, or returns `NotNeeded` (C-F15).
2. **Strictly advancing.** `candidate > effective_boundary` is required; a
   compaction whose boundary would not advance the effective boundary is a
   no-op (C3, C-F5).
3. **Deterministic.** Selection reads only `session.header()` and
   `session.events()`; no clock, no randomness, no I/O (C5). The same log and
   policy always yield the same boundary.
4. **Head turn is never summarized.** Because `keep_recent_turns >= 0` and the
   candidate is `keep_recent_turns` turns back, the in-flight turn (which has no
   terminal event yet) can never be a boundary. A compaction never summarizes
   the turn that triggered it.
5. **Open/partial turns.** A trailing open turn is ignored for boundary
   selection (it has no terminal event) but is retained in the tail by
   construction.

The **summarize set** is the projected prefix `deriveMessages(header,
events[0 .. candidate])` (`01 §6.3`). It includes user messages, assistant
messages, tool results, and injected context in the prefix; it excludes the
assembler-added system prompt (which has no origin `Sequence` and is not part of
the session log).

### 3.4 The compacted context generation

After the summarization call returns text `S`, the compactor produces:

```text
payload::ContextCompaction{
    boundary      = candidate,
    summary       = S,
    tokenEstimate = estimator_.estimate( compacted_context ),  # §2.3, C15
    model         = resolved_summarizer_model,                 # 08 §5.2
    createdAt     = clock_()                                   # WallClock, §5.2
}
```

`compacted_context` is the **projection-level** value the new compaction folds
to — i.e. what `deriveMessages()` returns immediately after the event is
appended (the loop re-assembles; the compactor only estimates):

```text
compacted_context =
      [Message{ Role::System, text = S }]                    # summary (C-D6)
    + projected messages whose origin Sequence > candidate   # retained tail
```

**Tail computation (pinned).** The compactor does **not** need per-message origin
`Sequence` values, which the assembled `std::vector<Message>` does not carry.
The assembled list is exactly

```text
messages = [leading assembler messages]   # e.g. the system prompt (06 §5.2)
         + [effective projection]         # deriveMessages(), 01 §6.3
```

so, using the plan's counts (§3.3):

```text
lead := messages.size() - (plan.prefix_messages + plan.kept_messages)
tail := messages[lead + plan.prefix_messages ..]
        # exactly plan.kept_messages entries, in projection order
```

`lead` is the number of leading non-projected messages the assembler prepends;
it is `0` for an empty system prompt and `1` for the single system prompt
`SessionContextAssembler` prepends (`include/ymh/agent/context_assembler.cpp`).
`tail` is the retained suffix; together with `summary` it is the estimate basis.

**System prompt.** The assembler re-adds its system prompt on re-assembly
(`06 §5.1` step 3, §6.2). That prompt is part of the LLM-visible generation but
is **outside the session projection**, so it is **excluded** from
`compacted_context` and from `tokenEstimate` (C15).

The loop appends the payload and then re-assembles via `ContextAssembler`
(`06 §5.1` step 3). `deriveMessages()` performs the actual fold; the compactor's
`tokenEstimate` is advisory and never overrides provider-reported `Usage`
(`08 §3.5`).

### 3.5 The summarization prompt

The summarizer input is the projected prefix, rendered as a single user message
wrapped by a fixed instruction:

```text
[System] You compress an agent's working history. Preserve, in priority order:
         (1) the user's goals and explicit constraints; (2) decisions made and
         their rationale; (3) file paths, commands, and tool outcomes that are
         still relevant; (4) open tasks and unresolved errors. Omit chit-chat.
         Output only the summary. Do not call tools. Do not ask questions.
[User]   <serialized projected prefix>          # roles + content blocks, bounded
```

- **Deterministic rendering.** The prefix is rendered by a pure function of the
  projected messages; the same prefix always yields the same prompt (C5).
- **Bounded.** The serialized prefix is bounded by `max_summary_bytes`; if the
  prefix itself exceeds the summarizer model's window, see C-F2 (split/chunk or
  fail).
- **No tools.** The summarization request sets `LLMRequest.tools = {}` and
  `tool_choice = "none"` (the summarizer must not call tools, `08 §3.1`).
- **Model override.** `LLMRequest.model` is the resolved summarizer model
  (`08 §5.2`); empty policy override ⇒ the session model.
- **Redaction.** The prompt carries session history; it is subject to the same
  redaction/logging rules as any provider call (`08 §2.2`, `§40`). Prompt
  bodies are never logged by default.

### 3.6 Multiple compactions

Compaction is a fold over the log in sequence order (`01 §6.3`). Consequences:

- **Composition.** After compaction A (boundary `bA`) and later compaction B
  (boundary `bB > bA`), the effective context is `[summary_B] + messages(>
  bB)`. Summary A is itself inside B's summarize set: its origin is A's own
  `Sequence` (`§4.2`), which is `< bB`, so the effective projection uses only
  the latest summary. This is the "latest boundary wins for overlapping ranges"
  rule of `01 §6.3`.
- **No double counting.** Each compaction event is folded exactly once, in
  sequence order. The projection never sums summaries.
- **Chains are deterministic.** Given the same log, the same effective context
  is produced regardless of when the compactions were created (C5).

---

## 4. Event sourcing

### 4.1 The `ContextCompaction` event (payload pinned by `01 §4.5`)

The durable event type is `EventType::ContextCompaction` (wire
`context/compaction`, `01 §4.3`); the payload is `payload::ContextCompaction`
(`01 §4.5`, materialized in `include/ymh/session/events.hpp`):

```cpp
namespace ymh::payload {

struct ContextCompaction {                 // §32: never deletes original events
    Sequence                              boundary = 0;
    std::string                           summary;
    std::size_t                           tokenEstimate = 0;
    std::string                           model;
    std::chrono::system_clock::time_point createdAt{};
};

} // namespace ymh::payload
```

This spec does **not** add fields to the payload. `tokenEstimate` is defined by
§2.3; the summarizer call's `Usage` is a separate `TokenUsage` event (§5.7).
`ContextCompaction` is a **durable** event (`01 §4.3`), so replay reproduces
both the compacted context and its provenance without re-invoking a model.

### 4.2 Projection rule (normative)

`deriveMessages()` (`01 §6.3`) folds compaction as follows, and this spec
adopts that fold verbatim:

```text
messages := []                          # ascending Sequence
origins  := []                          # origin Sequence per projected message
for rec in events():                    # ascending Sequence
    ... (01 §6.3 for every non-compaction event; each projection records rec.seq)

    case ContextCompaction:
        # drop every projected message derived from an event with seq <= boundary
        keep messages[i] where origins[i] > payload.boundary
        prepend Message{ Role::System, content = [Text(payload.summary)] }
        # The synthetic summary's origin Sequence is this ContextCompaction
        # event's own `record.seq` (existing `Session::deriveMessages`,
        # src/session/session.cpp), so a later compaction whose boundary is
        # >= that seq drops it and supersedes it (01 §6.3, §3.6).
```

The rule is a **pure function** of `(header, events())` (01 I7). It is the only
definition of LLM-visible compaction semantics; the UI's conversation model
(`§20.8`) is a separate projection over the same events.

### 4.3 Replay faithfulness

Replay (`01 §9.4`) re-reads the resolved view and re-runs `deriveMessages()`. It
does **not** re-summarize: the `summary` text is durable, so replay is
deterministic, offline, and side-effect free (C5, C6). This is the central
property of §32's "compaction is a projection, not deletion":

- The original events are never rewritten or deleted (C1).
- The compacted context is reconstructed exactly from the stored summary and the
  stored boundary (C5).
- A replay that starts from an empty cache and a replay that starts from a
  snapshot at head produce the same message list (I21, `02 §6.5`).

### 4.4 Fork / resume / snapshot interaction

- **Fork.** A forked session's resolved view is `parent.events()[0, seedLength)
  ++ ownEvents()` (I10). Inherited `ContextCompaction` events fold exactly as in
  the parent; inherited boundaries are still valid because `Sequence` values are
  preserved by the fork (`01 §6.2`). A fork therefore inherits the parent's
  compacted context generation for free (C5).
- **Resume.** Resume projects the compacted context from the log; it never
  re-runs the summarizer (C6). The resumed agent starts `Idle` (A15).
- **Snapshot.** `SessionSnapshot.messages` (`01 §6.4`, `02 §6.4`) already
  contains the folded projection. A committed compaction makes any existing
  snapshot stale (`02 §6.5`, I21); the loop requests `checkpoint(session)`
  after the append when the snapshot threshold is met (§6.4). The store never
  serves a stale snapshot as authority (P-F12).

### 4.5 No deletion

There is no compaction API that deletes, truncates, or rewrites events. The
only whole-log removal is session delete (`01 §9.5`, I16), which is orthogonal
to compaction. A compaction's `boundary` names a position; it does not erase the
events at or before it (C1).

---

## 5. C++ interfaces

### 5.1 The frozen `Compactor` seam (`06 §5.3`)

`06 §5.3` pins the seam the loop depends on. It is **not** modified by this
spec:

```cpp
namespace ymh {

class Compactor {
public:
    virtual ~Compactor() = default;

    // When the estimate exceeds the threshold, summarize the projected prefix
    // and return a ContextCompaction payload; original events are untouched.
    virtual std::optional<payload::ContextCompaction>
    run(const Session&, const std::vector<Message>&, CancellationToken) = 0;
};

} // namespace ymh
```

`run()` is intentionally thin: `std::nullopt` means "no compaction was
produced" (not needed, disabled, cancelled, or failed). The loop's
estimate re-check plus the provider's `ContextLengthExceeded` outcome
disambiguate the failure case (`06 §5.3`, A-F10). The existing `NullCompactor`
(`context_assembler.hpp`) remains the disabled implementation.

### 5.2 `ContextCompactor` (concrete, pinned)

```cpp
namespace ymh {

// Injectable WALL clock for `ContextCompaction.createdAt` (§2.3, C5). This is
// deliberately NOT the broker's `ClockReader` (11 §7.2), which is
// `std::chrono::steady_clock`-based: a steady clock has no wall-clock epoch and
// cannot produce the durable `system_clock::time_point` the payload stores
// (`include/ymh/session/events.hpp`). `system_clock::now` is the default, so
// production construction is unchanged; tests inject a fixed/`ManualWallClock`
// reader.
using WallClock = std::function<std::chrono::system_clock::time_point()>;

// Concrete compactor implementing the frozen `Compactor` seam (06 §5.3).
// One instance per workspace daemon, shared by every agent (the daemon's
// `AgentServices` bundle; src/agent/workspace_runtime.cpp). It is stateless
// across turns AND sessions: `session` is passed in per call and usage is
// returned (never appended), so the shared instance cannot misattribute.
class ContextCompactor final : public Compactor {
public:
    ContextCompactor(LLMProvider&            provider,
                     LLMPool&                pool,
                     const TokenEstimator&   estimator,
                     CompactionPolicy        policy,
                     WallClock               clock = std::chrono::system_clock::now);

    // Frozen seam adapter (06 §5.3): delegates to compact(); returns the
    // payload iff the outcome is Compacted. The frozen `optional` return drops
    // `CompactionResult::usage`; callers that need usage call `compact()`.
    std::optional<payload::ContextCompaction>
    run(const Session&, const std::vector<Message>&, CancellationToken) override;

    // Rich API: the manual path (/compact) and tests use this to distinguish
    // Compacted / NotNeeded / Cancelled / Failed (§5.3; `Queued` is produced
    // only by requestCompaction, §6.7). Synchronous: it calls `.get()` on the
    // eager `Task`s (§5.5).
    CompactionResult compact(const Session&, const std::vector<Message>&,
                             CancellationToken);

    // Pure, no-I/O boundary selection (§3.3); exposed for unit tests and for
    // the TUI to answer "can this session be compacted?".
    [[nodiscard]] CompactionPlan plan(const Session&,
                                      const std::vector<Message>&) const;

    [[nodiscard]] const CompactionPolicy& policy() const noexcept;

private:
    LLMProvider&          provider_;
    LLMPool&              pool_;
    const TokenEstimator& estimator_;
    CompactionPolicy      policy_;
    WallClock             clock_;
};

} // namespace ymh
```

- **Ownership.** The daemon's wiring constructs one `ContextCompactor` and
  injects it through the loop's service bundle as an additive
  `AgentServices::context_compactor` (`ContextCompactor*`; see §6.7/§11.3;
  the frozen `AgentServices::compactor` (`Compactor*`) remains for the legacy
  seam). The compactor is a leaf service: it touches the provider seam, the
  pool, the estimator, and the clock — never the store, the bus, the registry,
  or the session log. It does not append events (the loop owns every append,
  §5.7).
- **Single-threaded per agent.** `compact()` runs on the turn thread, like the
  provider call it wraps (`06 §9`, `08 §9`). No locking is added.
- **`run()` vs `compact()`.** `run()` exists only to satisfy the frozen seam;
  new code calls `compact()`. Because `run()` cannot return usage, the automatic
  loop path calls `compact()` too (§6.1, §6.7).
- **No duplicated system prompt.** The compactor no longer takes or stores the
  assembler's system prompt (`SessionContextAssembler::systemPrompt_`,
  `include/ymh/agent/context_assembler.cpp`); that would duplicate assembler
  state. The system prompt is excluded from `tokenEstimate` (§3.4).

### 5.3 `CompactionResult` and `CompactionError`

```cpp
namespace ymh {

enum class CompactionOutcome : std::uint8_t {
    Compacted,   // a new payload::ContextCompaction was produced
    NotNeeded,   // no boundary advances the effective boundary (or disabled)
    Cancelled,   // the token fired; nothing was appended
    Failed,      // summarization failed; nothing was appended
    Queued,      // requestCompaction only: the immediate RPC ack; the loop
                 // always services it as a dedicated maintenance turn (§6.7)
};

struct CompactionError {
    enum class Code : std::uint8_t {
        None,
        Disabled,              // policy.is_enabled() == false
        NoBoundary,            // no turn-aligned boundary advances (C3)
        PrefixTooSmall,        // below min_prefix_messages (C-F14)
        SummarizerFailed,      // provider terminal failure (C-F1)
        SummarizerOverflow,    // summarizer input itself overflowed (C-F2)
        OversizedSummary,      // summary exceeded max_summary_bytes (C-F3)
        Cancelled,             // cooperative cancellation (C-F7)
        LeaseLost,             // write lease lost mid-compaction (C-F10)
        StoreUnavailable,      // append failed (C-F11)
        Internal,              // invariant violation
    };

    Code        code = Code::None;
    std::string detail;   // short, redacted
};

struct CompactionResult {
    CompactionOutcome                         outcome = CompactionOutcome::NotNeeded;
    std::optional<payload::ContextCompaction> compaction;   // set iff Compacted
    std::optional<Usage>                      usage;        // summarizer usage, if reported
    CompactionError                           error;
};

} // namespace ymh
```

`CompactionResult` is **not** a durable type; it is a return value for the
manual path, the automatic loop path, and tests. `usage` carries the
summarizer's provider-reported `Usage` (if any) back to the loop, which owns
the `TokenUsage` append (`06 §8`, §5.7, C10); the compactor never appends it.
The frozen `run()` adapter collapses the result to an `optional` (the frozen
seam), dropping `usage`, and lets the loop's estimate re-check decide whether to
fail the turn (C13).

`CompactionError::Code::LeaseLost` / `StoreUnavailable` are declared for
completeness but are **not** produced by `compact()`, which performs no append.
The only append in this flow is the loop's post-compaction
`Session::append(payload)` / `append(TokenUsage)`, whose failure surfaces as a
turn-level failure (C-F10/C-F11), not through `CompactionResult`.

The manual path needs to report the outcome after the queued maintenance work
has run. There is **no** separate non-durable completion handle, and the RPC
result is only the immediate `Queued` acknowledgement (§6.8): the final outcome
**crosses the supervisor↔daemon boundary on the existing session event stream** —
the durable events the TUI already consumes (`05 §5.2` `SessionEnvelope` →
`10 §4.5`, adapted by `10 §5.2`) — because the manual request always runs as a
dedicated maintenance turn whose committed events encode the outcome (§6.7). No
new `EventType`, no `protocol::HostNotice`, and no `HostNoticeKind` extension are
introduced.

### 5.4 `CompactionPlan` and the per-turn guard

```cpp
namespace ymh {

struct CompactionPlan {
    Sequence    boundary = 0;        // a compaction point (§2.1)
    std::size_t prefix_messages = 0; // projected messages summarized
    std::size_t kept_messages = 0;   // projected messages retained
    bool        valid = false;       // false => NotNeeded
};

} // namespace ymh
```

The loop enforces `max_compactions_per_turn` (C-F8) by counting committed
compactions in the current turn; the compactor itself is stateless across turns.
A second attempt in the same turn after a successful compaction is a no-op
(`NotNeeded`, because the boundary no longer advances, C3).

### 5.5 The summarization request through the LLM pool

`compact()` is **synchronous** and performs exactly one provider call, bracketed
by exactly one `LLMPool` slot (`06 §5.9`, A12). `Task<T>` is an *eager* value
type (`include/ymh/core/task.hpp`): `pool_.acquire(...)` returns
`Task<std::optional<Slot>>` and `provider_.stream(...)` returns
`Task<LLMResponse>`, so both are consumed with `.get()`, not `co_await`.
Provider text is delivered **only** through the `StreamSink`; `LLMResponse` has
no `text` field (`include/ymh/llm/llm_provider.hpp`), so `compact()` accumulates
it in a pinned collector.

```text
compact(session, messages, cancel):
  if not policy_.is_enabled():
      return { NotNeeded, {}, {}, { Disabled } }
  plan := select_boundary(session, policy_)              # §3.3, pure
  if not plan.valid:
      return { NotNeeded, {}, {}, { NoBoundary | PrefixTooSmall } }

  prefix  := deriveMessages(header, events[0 .. plan.boundary])   # 01 §6.3
  prompt  := build_summary_prompt(prefix)                         # §3.5, pure
  request := LLMRequest{ model = resolve_summarizer_model(policy_, session),
                         messages = prompt, tools = {}, parameters = {} }

  slot := pool_.acquire(cancel).get()                    # 06 §5.9; eager Task
  if not slot.has_value():
      return { Cancelled, {}, {}, { Cancelled } }        # C-F7/C-F9

  # Text is delivered only through the sink; accumulate the summary here.
  summary_text := ""
  collect := [&](const StreamEvent& event) {
      if TextDelta* delta := std::get_if<TextDelta>(&event):
          summary_text += delta->text
      return SinkFlow::Continue
  }
  response := provider_.stream(request, collect, cancel).get()
  # slot destroyed here on every path (A12)

  if response.outcome == Cancelled:
      return { Cancelled, {}, {}, { Cancelled } }
  if response.outcome == Failed:
      code := (response.error.code == ContextLengthExceeded)
                  ? SummarizerOverflow : SummarizerFailed
      return { Failed, {}, {}, { code } }

  summary := bound_summary(summary_text)                 # §3.5, C8
  if summary.bytes > policy_.max_summary_bytes:
      return { Failed, {}, {}, { OversizedSummary } }    # C-F3

  # Tail from the plan counts (§3.4); summary + tail is the estimate basis.
  lead      := messages.size() - (plan.prefix_messages + plan.kept_messages)
  tail      := messages[lead + plan.prefix_messages ..]
  compacted := summary + tail                            # §3.4
  payload := ContextCompaction{ plan.boundary, summary,
                                estimator_.estimate(compacted),
                                request.model, clock_() }
  return { Compacted, payload, response.usage, {} }      # usage -> loop (§5.7)
```

- **Usage is returned, not appended.** The compactor has no session identity and
  is shared across agents (`AgentServices::context_compactor`), so it cannot
  append `TokenUsage` itself: `TokenUsage.turn` is session-local. The loop —
  which owns the enclosing turn and already appends `TokenUsage` (`06 §8`) —
  appends `payload::TokenUsage{*result.usage, turn}` when `usage` is set
  (§5.7, C10). This is why the loop calls `compact()`, not the frozen `run()`.
- **`enclosing_turn` is owned by the loop**, not the compactor: it is the last
  `TurnStarted` without a matching terminal (`01 §4.5`, I11), i.e. the open turn
  that called `compact()`; for the manual path it is the maintenance turn
  (§6.7). Compaction is always inside an open turn (C12).
- **Retry.** The provider's own retry policy (`08 §3.7`) applies inside
  `stream()`; `compact()` never retries. A retryable provider failure that
  exhausts `max_attempts` is `SummarizerFailed`.
- **Deadline.** `LLMRequest.deadline` is left at the provider default (`08
  §3.1`); a future policy field may set it.

### 5.6 `TokenEstimator` and the effective threshold

The estimator interface is pinned by `06 §8` (existing code):

```cpp
class TokenEstimator {
public:
    virtual ~TokenEstimator() = default;
    virtual std::size_t estimate(const std::vector<Message>&) const = 0;
};
```

- `DefaultTokenEstimator` is the v1 implementation (bytes/4 + per-message
  overhead, existing code). It is **advisory only** and never overrides
  provider-reported `Usage` (`08 §3.5`).
- The loop compares `estimator.estimate(messages)` against
  `policy.effective_threshold_tokens()` (§3.2). The estimator is injected
  through the loop's service bundle (`AgentServices::estimator`, existing
  code; `06 §4.1`).
- `tokenEstimate` in the payload is the same estimator applied to the
  compacted context (§2.3).

### 5.7 Auxiliary usage accounting (loop-owned)

The summarization call consumes tokens like any other request and must not
drift the session's cost accounting (`§33`, C10). The compactor cannot append
the event itself: `payload::TokenUsage` carries `turn`, which is *session-local*,
while `ContextCompactor` is a single shared instance across agents
(`AgentServices::context_compactor`; `src/agent/workspace_runtime.cpp`), so a
compactor-side append would be misattributed. Instead:

- **The compactor returns usage.** `compact()` copies
  `LLMResponse::usage` into `CompactionResult::usage` (`std::optional<Usage>`,
  §5.3) and appends nothing.
- **The loop appends it.** On `Compacted` with `usage.has_value()`, the loop
  appends `payload::TokenUsage{*result.usage, turn}` for the turn it owns,
  before `StepEnded` — exactly the existing `TokenUsage` rule (`06 §8`,
  `01 §4.5`, `§33`). The frozen `Compactor::run` path drops usage by design, so
  the automatic path calls `compact()` (§6.1, §6.7).

Rules:

- **Attribution.** `TokenUsage.turn` is the enclosing turn's id, known to the
  loop (the open turn, or the maintenance turn §6.7); it is never `nullopt` in
  v1. This keeps `01` I11 (TokenUsage occurs inside an open turn) and removes
  the shared-instance ambiguity.
- **One event per request.** At most one `TokenUsage` is appended per
  summarization request (`08 §3.5`, L8/L17); absent usage ⇒ no event.
- **No drift.** The main request's `Usage` and the summarizer's `Usage` are
  distinct events; the loop never adds them together (C10, C-F6).
- **Fake.** `FakeLLM` (`§45`) may report usage; tests assert the loop appended
  exactly one auxiliary `TokenUsage` with the expected turn and values, and that
  the main request's `TokenUsage` is unchanged.

### 5.8 How the loop consumes the latest compaction

The loop does not need a new "latest compaction" accessor: `deriveMessages()`
already folds every committed compaction (`01 §6.3`), so after `compact()`
returns `Compacted` the loop simply:

1. appends the payload (`session.append(payload)`, `01 §6.1`, I4/I11);
2. if `result.usage` is set, appends `payload::TokenUsage{*usage, turn}` for the
   open turn (`06 §8`, §5.7, C10);
3. re-assembles via `ContextAssembler::assemble(session, turn, step)` (`06
   §5.2`) — the new summary is now part of the projection;
4. optionally requests `checkpoint(session)` (`02 §6.4`) if the snapshot
   threshold is met;
5. proceeds to the provider call.

This is exactly the existing `runCompaction` → re-assemble sequence
(`agent_loop.cpp`), extended to carry usage from `compact()`; this spec pins its
semantics and failure mapping. The loop holds the rich compactor as
`AgentServices::context_compactor` (`ContextCompactor*`), so it can call
`compact()`; the frozen `Compactor*` seam remains for the legacy path (§6.7,
§11.3).

---

## 6. Integration points

### 6.1 Agent loop / `TurnExecutor` (`06 §5.1`)

- **Call sites.** The threshold check before the provider call and the
  `ContextLengthExceeded` retry (`06 §5.1`). When
  `AgentServices::context_compactor` is set, the loop calls
  `context_compactor->compact(...)` (rich API, §5.2), not the frozen
  `Compactor::run`, so it receives `CompactionResult` and appends `TokenUsage`
  itself (§5.7). When it is null the loop falls back to the frozen
  `AgentServices::compactor->run(...)` (no usage), preserving the existing
  `Compactor` seam and its test doubles. The loop owns the
  `max_compactions_per_turn` guard and the `CompactionFailed` mapping.
- **Terminal mapping.** A summarizer failure that leaves the context unable to
  fit maps to `TurnFailed{CompactionFailed}` (`06 §5.7`, A-F10); a successful
  compaction never changes the turn's terminal event.
- **Cancellation.** The turn's `CancellationToken` is passed into `compact()`
  (`06 §5.1`, `§34`); a cancelled compaction appends nothing and the turn
  closes `TurnCancelled` (C11, C-F7).
- **Lease.** The append is lease-checked by `Session::append` (`01 §6.1`, I5);
  a lost lease degrades to read-only and stops the loop (A11, C-F10).
- **State.** Compaction does not introduce a new `AgentState`; it runs inside
  the existing `Thinking`/step state (`§20.15`). A live `Progress` event may be
  emitted for the TUI (non-durable, `01 §4.1`).

### 6.2 `ContextAssembler` (`06 §5.2`, `§31`)

The assembler remains the **only** place history is selected/trimmed
(`06 §5.2`). Compaction changes what the assembler sees via the session
projection, not by an assembler API. After a compaction the loop re-assembles;
no assembler method is added. The assembler-added system prompt is never part of
the summarize set (§3.3), is re-added on every assembly (`06 §5.1` step 3), and
is therefore present in the LLM-visible generation but **excluded** from
`tokenEstimate` (§3.4, C15). The compactor does not take or duplicate the
assembler's `systemPrompt_`.

### 6.3 Session events (`01`)

- **Append.** The payload is appended inside an open turn (I11) via
  `Session::append`; it is durable and published after commit (I4).
- **Boundary validity.** The boundary must match a committed `Sequence` in the
  resolved view; a non-matching boundary is rejected at append/validate time
  (`01 §13.2` S7). This spec tightens S7 to *turn-terminal* sequences (§2.1,
  C2).
- **Projection.** `deriveMessages()` folds it (`01 §6.3`, §4.2); the fold is
  pure and deterministic (I7).
- **Composition.** Multiple compactions compose deterministically (§3.6).

### 6.4 Persistence (`02`)

- **Append transaction.** Each compaction event is one `append` transaction
  committing before it returns (`02 §6.1`, I4).
- **Snapshot refresh.** After a committed compaction, `checkpoint(session)` is
  requested when `resolved_event_count >= snapshot_threshold_events` (`02
  §6.4`); the snapshot is replaced atomically and is never patched (`02 §6.5`,
  I21, C16).
- **Staleness.** A compaction makes any prior snapshot stale; it is discarded
  and recomputed, never patched (`02 §6.5`, S12).
- **Payload cap.** The summary is bounded by `max_summary_bytes <=
  PersistenceConfig::max_payload_bytes` minus envelope headroom; an over-cap
  payload is rejected (`01` S10, `02` P-F15, C-F3).

### 6.5 LLM pool and usage (`08`)

- **Pool discipline.** Every summarization call holds exactly one `LLMPool`
  slot, acquired cancellably and released on completion, cancel, and failure
  (`06 §5.9`, A12, C9, C-F9).
- **Provider seam.** The call goes through the same `LLMProvider::stream`
  (`08 §3.4`) as a normal request; the compactor owns no transport.
- **Model resolution.** `LLMRequest.model` is resolved with the per-request
  override first (`08 §5.2`, step 1), so the summarizer may use a cheaper
  model. The resolved model is recorded in `ContextCompaction.model`.
- **Usage.** Provider-reported usage is returned in `CompactionResult::usage`
  and appended as a `TokenUsage` event by the loop, which owns the enclosing
  turn (§5.7, `06 §8`, `08 §3.5`); it is never fabricated or estimated
  (`08 §3.5`).

### 6.6 Permissions and configuration

- **No permission gate.** Compaction reads only the session projection and
  makes an LLM call; it never touches the filesystem, shell, or git, so it does
  not pass through `PermissionPolicy` (`§19`, `07 §5`). The LLM call itself is
  governed by the provider config and the same redaction/logging rules as any
  call (`§40`, `08 §2.2`).
- **Config.** `CompactionPolicy` is built from the layered config (§37) under
  `[agent.compaction]`; the legacy `[agent] compaction_threshold_tokens` maps to
  `threshold_tokens` and `enabled`. Invalid policy values fail at load with
  `ConfigError` (C18).
- **Disabled by default.** v1 ships compaction off unless a threshold or model
  window is configured (decision C-D4), matching the existing
  `compaction_threshold_tokens = 0` default.

### 6.7 Supervisor TUI (`/compact`, spec `10`)

The TUI *requests* compaction; it never performs it (D1, §4.1). The additive
surface is:

- **Command.** A `/compact` slash command registered in the `CommandRegistry`
  (`§26`), executed through `UiController::executeCommand` (`§20.11`). Its
  `CommandSchema` is `{ name="/compact", description="Summarize history to
  reclaim context" }`. `InputView` detects the leading `/` and delegates; no
  command is hard-coded in `InputView` (`§26`, `10 §9.2`).
- **Core contract.** The command maps to the additive RPC `session.compact`
  (Interactive profile, §6.8), which the daemon's `HostRuntime` handles by
  submitting the enqueue body to its `TurnExecutor`; the worker body calls
  `AgentRegistry::requestCompaction(SessionId)` (additive, §6.8), which looks up
  the registered `AgentLoop` and calls the **public**
  `AgentLoop::requestCompaction()` (additive; the pinned enqueue target,
  §6.8/§11.3 A5). `Agent` (`§10.1`) is not extended (C-D2).
- **The loop holds the rich compactor.** Both the automatic and the manual path
  need `ContextCompactor::compact()` and `CompactionResult` (for usage, §5.7),
  which the frozen `Compactor*` seam cannot provide. The loop/registry therefore
  hold the additive `AgentServices::context_compactor` (`ContextCompactor*`,
  §5.2, §11.3); the frozen `AgentServices::compactor` (`Compactor*`) remains for
  the legacy path.
- **Activation (pinned).** The `session.compact` handler runs in the daemon's
  `HostRuntime`, which owns the bounded `TurnExecutor`
  (`include/ymh/agent/turn_executor.hpp`, M-F4). It **submits a worker body and
  never touches the loop's inbox on the io/dispatch thread**: the inbox is
  executor-thread-confined and has no lock (`06 §3.2`; `AgentLoop` holds a bare
  `std::deque<InboxItem> inbox_`), so it may only be mutated on the executor
  thread. The submitted body — running on an executor worker — **enqueues** the
  `InboxKind::Compact` item. This is exactly the
  `agentPrompt`/`agentFollowup` shape: `HostRuntime::agentPrompt` submits a body
  that calls `agent->send(parsed)` on the worker
  (`src/host/host_runtime.cpp` ~:546), so no handler enqueues from the
  io/dispatch thread (`include/ymh/transport/protocol_server.hpp`: "handlers
  must not block").

  The worker body calls `AgentRegistry::requestCompaction(SessionId)`, which is
  **enqueue-only** and runs on the executor thread. It looks up the registered
  `AgentLoop` (the registry owns `std::unique_ptr<AgentLoop>` in `agents_`, so no
  `Agent` method is added) and calls the **public**
  `AgentLoop::requestCompaction()` (A5), which enqueues the `InboxKind::Compact`
  item. `enqueue()` then calls `activate()` itself when no turn is running, so
  the maintenance turn runs on that same worker without a separate `activate()`
  submission:
  - **Idle.** The body enqueues the item; `enqueue()` sees no running turn and
    calls `activate()`, running the **maintenance turn** (below) on the worker.
  - **In flight.** The body enqueues the item while a turn is running;
    `enqueue()` does not re-enter `activate()`. The manual request is **never
    folded into the running turn**: when that turn closes, `activate()`'s
    `while (hasTurnTrigger())` loop re-checks the inbox and runs the maintenance
    turn on the same worker. This is what makes the outcome uniformly observable
    as the maintenance turn's own committed events (below). The *automatic*
    threshold path is unaffected: it still compacts inside the current step
    (§6.1).

  `AgentLoop::hasTurnTrigger()` returns `true` for a queued
  `InboxKind::Compact` item, so `hasPendingWork()`
  (`hasTurnTrigger() || turnInFlight()`) is `true` once the worker body has
  enqueued it; an accepted compaction is therefore pending work, not a side flag
  that `hasPendingWork()` would ignore. The handler returns
  `CompactionOutcome::Queued` as soon as `TurnExecutor::submit()` accepts the
  body. If `submit()` returns `false` (queue full / draining) the handler maps
  that to `AgentErrorCode::InboxFull` / `RpcCode::InternalError` (E7, C-F17);
  because the item is enqueued **inside** the accepted body, a rejected submit
  enqueues nothing, so there is **no orphaned `Compact` item** and
  `hasPendingWork()` never becomes permanently true. No path runs the loop
  synchronously on the caller. If the worker body's loop-inbox enqueue is instead
  rejected, the `Compact` item is not queued and no compaction turn opens; the
  two rejection reasons are handled distinctly so no path ever opens a second
  turn on top of a live one (`01` I11):
  - **`AgentDisposed`** (`disposed_`). `requestCompaction()` returns
    `AgentErrorCode::AgentDisposed` and appends **nothing**: post-dispose ops
    produce no durable event (`06` A14, A-F14; `06 §3.2` "a rejected op …
    produces none"; this spec §8.1 F3). A disposed agent has no live turn stream
    to carry a failure turn, and appending one would itself be a post-dispose
    append.
  - **`InboxFull`** (`inbox_.size() >= config_.max_inbox`). The inbox is full, so
    the `Compact` item cannot be queued. A full inbox does **not** by itself imply
    an in-flight turn: `enqueue()` only calls `activate()` when `!running_`
    (`src/agent/agent_loop.cpp:132-147`), and `activate()`'s drain loop is gated on
    `hasTurnTrigger()`, which is false for `Inject` items with `startsTurn ==
    false` (`:83-98`, `:149-162`); an **idle** loop can therefore hold a full
    inbox of non-starting injects that no turn will drain. Appending
    `TurnStarted{origin = Maintenance}` → `TurnFailed` inline is nevertheless
    unsafe: if a turn *is* in flight it would interleave two open turns, which the
    single-`openTurn` projection and `validateLog` reject (`01 §6.3`, I11). The
    failure is therefore **deferred to the loop's single-open-turn discipline**
    via a loop-internal **deferred maintenance-failure trigger**
    (executor-thread-confined; counted by `hasTurnTrigger()`/`hasPendingWork()`):
    the body records the trigger and then **guarantees consumption** — if
    `running_` is false (the idle case above, or `state_ == Error`) it resets
    `Error → Idle` (mirroring `enqueue()`, `:139-141`) and calls `activate()`,
    which emits the zero-step maintenance **failure turn** immediately; if
    `running_` is true the already-running `activate()` loop emits it at the top
    of its next iteration — i.e. **after** the in-flight turn has closed with its
    own terminal event (and even when that turn closed with `TurnFailed`). Exactly
    one turn is open at a time and the failure still reaches the same event-stream
    carrier (§6.8, C-F17b, C19).
- **Semantics (manual path).** Whether the agent is idle or a turn is in flight,
  the queued `InboxKind::Compact` item is serviced as a **dedicated maintenance
  turn** (decision C-D2), never folded into another turn. A `TurnExecutor` worker
  runs:
  `TurnStarted{origin = TurnOrigin::Maintenance}` → `StepStarted` → the
  summarization call → `ContextCompaction` (only when the outcome is `Compacted`)
  → `TokenUsage` (if reported, §5.7) → `StepEnded` → terminal event
  (`TurnEnded`/`TurnCancelled`/`TurnFailed`). The maintenance turn appends **no**
  `AssistantMessage`; its single step *is* the summarization. This keeps all
  durable compaction events inside an open turn (I11, C12). `TurnOrigin::Maintenance`
  is an **added** enumerator (it amends `01 §4.5`; recorded in §11.3 A1) — the
  existing `TurnOrigin::Injection` is reserved for a `ContextInjected` that
  materializes as a turn (`01 §4.5`, `include/ymh/session/events.hpp`) and is
  **not** overloaded here, because a compaction maintenance turn appends no
  `ContextInjected`.
- **Feedback — the wire carrier (pinned).** The immediate RPC result of
  `session.compact` is the acknowledgement `{outcome: "Queued"}` (§6.8); it is
  **not** the final `CompactionOutcome`. The final outcome crosses the
  supervisor↔daemon boundary on the **existing session event stream** — the same
  durable `Event`s the TUI already consumes (`05 §5.2` `SessionEnvelope` →
  `10 §4.5`, adapted by `UiEventAdapter` `10 §5.2`) — because the manual request
  always runs as its own maintenance turn. The committed events of that turn are
  the carrier:

  | `CompactionOutcome` | committed events of the maintenance turn |
  |---|---|
  | `Compacted` | `TurnStarted{Maintenance}` … `ContextCompaction` (boundary / `tokenEstimate` / `model`) … `TurnEnded` |
  | `NotNeeded` | `TurnStarted{Maintenance}` … `TurnEnded` with **no** `ContextCompaction` |
  | `Cancelled` | `TurnCancelled` (reason) |
  | `Failed` | `TurnFailed` (`code`, `message`) — including the zero-step maintenance failure turn emitted when the post-ack loop-inbox enqueue is rejected `InboxFull`: after any in-flight turn closes, or immediately when the loop is idle (including an idle inbox full of non-starting injects). A post-ack `AgentDisposed` rejection emits **no** turn — post-dispose ops produce no event (`06` A14/A-F14) (§6.8, C-F17b, C19) |

  The supervisor's `UiEventAdapter` (`10 §5.2`) correlates the
  `TurnOrigin::Maintenance` turn (A1/A2) with its terminal event and the
  presence/absence of `ContextCompaction`, and emits a **frontend-only**,
  transient `UiEvent` notice (`UiEvent::CompactionOutcomeNotice`, additive —
  §11.3 A11); the notice does **not** block. Because `UiEvent` never crosses the
  wire (`§54 D15`, T4), the notice is "supervisor-local" only in that sense; its
  *content* is derived entirely from wire-carried durable events. It is **not** a
  wire `protocol::HostNotice`: this spec does **not** extend the frozen
  `HostNoticeKind` (exactly 4 values:
  `SessionClosed`/`SessionCreated`/`LeaseLost`/`DaemonShuttingDown`,
  `include/ymh/transport/protocol.hpp`), and it introduces **no** new
  `EventType`.
- **Rendering (additive).** The `ConversationModel` (`§20.8`) gains an additive
  `CompactionMarker` view derived from the `ContextCompaction` event: a
  collapsed system entry ("⋯ compacted history up to #N · summary ~T tokens ·
  model M") that can be expanded to show the summary. The marker is a pure
  projection; rendering stays side-effect free (§20.12, D16). Golden tests
  cover it (§10.4).
- **No status change.** Compaction does not change the aggregate status line or
  the attention counters (`§20.23`); it is not an attention event.

### 6.8 Transport / RPC (`05`)

The manual path needs a request channel from the supervisor to the daemon. This
is an **additive** extension to the frozen M2 catalog (`11`), recorded as
decision C-D2:

```text
session.compact   params: { session }   result: { outcome: "Queued" }   (Interactive)
```

The `result` is the immediate acknowledgement only: the maintenance turn runs
asynchronously via `TurnExecutor`, and the final outcome arrives on the **session
event stream** as the maintenance turn's own committed events (§6.7), never as a
deferred RPC result and never as a "supervisor-local notice" with no wire
carrier.

- **Profile.** `session.compact` is Interactive-only, like `session.activate` /
  `session.suspend` (`05 §7.4`, `11 §6.3`); an Automation-profile caller gets
  `MethodNotAllowedForProfile` (`05` T10, T-F18) before any effect.
- **Domain mapping.** The immediate result is always the value `Queued`
  (accepted; the daemon submitted the enqueue body to `TurnExecutor`). The final
  `Compacted`, `NotNeeded`, `Cancelled`, or `Failed` outcome is **not** returned
  by this RPC — it is delivered on the session event stream (§6.7) as the
  maintenance turn's `ContextCompaction` / `TurnEnded` / `TurnCancelled` /
  `TurnFailed` events. A `CompactionFailed` that instead surfaces as a turn
  failure maps to `RpcCode::InternalError` with `data.kind = "CompactionFailed"`
  (`11 §4.4`).
- **Additive method.** Adding a method does not alter any frozen method's
  signature or semantics; it is documented here so the catalog stays the single
  authority (`05 §7`, `11 §1`).
- **Core additive methods (pinned enqueue target).** Two additive methods, both
  **enqueue-only** and executor-thread-confined:
  - `AgentLoop::requestCompaction() -> std::expected<CompactionOutcome, AgentError>`
    — **public** (additive to `include/ymh/agent/agent_loop.hpp`; A5). It
    enqueues the `InboxKind::Compact` work item and returns
    `CompactionOutcome::Queued`, or `AgentErrorCode::InboxFull` when the loop's
    bounded inbox is full, or `AgentErrorCode::AgentDisposed` after `dispose()`.
    Neither rejection path appends inline: `AgentDisposed` appends **nothing**
    (`06` A14/A-F14, `06 §3.2`, §8.1 F3), and `InboxFull` records a **deferred**
    maintenance-failure trigger that is **guaranteed to be consumed** (idle:
    `requestCompaction()` calls `activate()` itself; in-flight: the running
    `activate()` loop consumes it after the current turn closes; `Error`: the
    `Error → Idle` reset precedes `activate()`) and emitted as a dedicated
    zero-step maintenance turn (`01` I11; C-F17b, C19). The pinned algorithm is
    below. This is the exact enqueue target; `AgentLoop::enqueue()` stays private
    and is never called from the registry.
  - `AgentRegistry::requestCompaction(SessionId) -> std::expected<CompactionOutcome, AgentError>`
    — additive to the frozen `AgentRegistry` (`06 §4.1`; A5). It looks up the
    registered `AgentLoop` (the registry owns `std::unique_ptr<AgentLoop>` in
    `agents_`) and delegates to `AgentLoop::requestCompaction()`.

  Neither changes `Agent` (`§10.1`) or any existing method. Both are invoked
  **inside** the `TurnExecutor` worker body (§6.7), so the inbox is only mutated
  on the executor thread; neither carries a `TurnExecutor` dependency (the submit
  lives in `HostRuntime`, A10).

  **Post-ack enqueue rejection (pinned contract).** `Queued` is the *executor*
  admission only (`TurnExecutor::submit()` accepted the body, §6.7); the
  loop-inbox enqueue runs later, on the worker. If it is rejected, **no `Compact`
  item is queued and no compaction maintenance turn opens**. The two reasons are
  handled distinctly so that no path opens a second turn while one is already
  open (`01` I11):

  - **`AgentDisposed`.** `requestCompaction()` returns
    `AgentErrorCode::AgentDisposed` and appends **nothing**. Post-dispose ops
    produce no durable event (`06` A14, A-F14; `06 §3.2` "a rejected op …
    produces none"; this spec §8.1 F3). The acknowledged request is refused at
    the loop boundary; a disposed agent has no live stream to carry a failure
    turn, and appending one would itself be a post-dispose append.
  - **`InboxFull`** (`inbox_.size() >= config_.max_inbox`). The `Compact` item
    cannot be queued. A full inbox does **not** imply an in-flight turn: an idle
    loop only drains **turn triggers**, and `hasTurnTrigger()` is false for
    `Inject` items with `startsTurn == false` (`src/agent/agent_loop.cpp:83-98`),
    so an idle agent can hold a full inbox of non-starting injects indefinitely
    (`06 §3.2`: "with the default `startsTurn == false` it does not by itself
    create pending work"). Appending `TurnStarted`/`TurnFailed` inline would
    interleave two open turns **only if** a turn is in flight, which the
    single-`openTurn` projection and `validateLog` reject (`01 §6.3`, I11). The
    failure is therefore routed through the loop's single-open-turn discipline via
    a loop-internal **deferred maintenance-failure trigger**
    (executor-thread-confined; counted by `hasTurnTrigger()`/`hasPendingWork()`),
    and **consumption is guaranteed regardless of loop state**:

    ```text
    requestCompaction():                          # executor thread, no lock
      if (disposed_)            return AgentDisposed     # append nothing
      if (inbox_.size() >= max_inbox):
          pending_maintenance_failure_ = true            # deferred trigger
          if (!running_):                                # idle or Error: consume now
              if (state_ == AgentState::Error) state_ = AgentState::Idle
              activate()                                 # emits the failure turn
          return AgentErrorCode::InboxFull
      enqueue(InboxKind::Compact)                # idle: enqueue() calls activate()
      return CompactionOutcome::Queued

    activate():                                   # amended loop head
      if (disposed_ || running_) return
      running_ = true
      while (!disposed_ && hasTurnTrigger()):
          if (pending_maintenance_failure_):             # serviced before inbox
              pending_maintenance_failure_ = false
              emit TurnStarted{origin = TurnOrigin::Maintenance}
              emit TurnFailed{code = InboxFull, message}  # state_ = Error
              continue
          if (state_ == AgentState::Error) break         # a normal turn failed
          runTurn()
      running_ = false
      if (state_ != AgentState::Error) state_ = AgentState::Idle
      flushIdleCallbacks()

    hasTurnTrigger() -> true if any Send/FollowUp/Steer item, any startsTurn
        Inject item, any Compact item, or pending_maintenance_failure_
    dispose():    ... existing teardown ... ; pending_maintenance_failure_ = false
    ```

    The trigger is cleared when the failure turn's `TurnFailed` is appended (and
    by `dispose()`), so the `activate()` cycle terminates and no post-dispose
    event is ever emitted. Because `state_ == Error` is reset before `activate()`
    and the loop services the trigger before the `Error` break, the trigger is
    consumed whether the loop was idle, in flight, or already in `Error`; and if
    the in-flight turn itself closes `TurnFailed` (setting `Error`) the running
    loop still services the pending trigger before exiting. A `TurnFailed`
    terminal leaves `state_ = Error`; the next accepted inbox op resets it to
    `Idle` in `enqueue()` (`:139-141`), so a rejected `/compact` never wedges the
    agent. Cancellation is orthogonal: the zero-step failure turn makes no
    provider call and observes no `CancellationToken`, and a `TurnCancelled`
    close leaves `state_ = Idle`, so the running `activate()` loop consumes a
    pending trigger on its next iteration (C11 is unaffected).

  In the `InboxFull` case the failure is a real, durable, wire-carried carrier
  that the supervisor's `UiEventAdapter` correlates to `CompactionOutcome::Failed`
  (§6.7) — no new `EventType`, no `HostNotice`, no separate completion handle —
  so a `Queued`-acknowledged `/compact` is never lost silently. In the
  `AgentDisposed` case no event is emitted (A14); the teardown/tombstone
  lifecycle (`06 §3.4`, F3) is the signal.
- **Submit wiring (pinned).** The submit belongs to `HostRuntime`, the daemon
  component that owns the bounded `TurnExecutor` and already dispatches
  `agent.prompt` / `agent.followup` this way (`src/host/host_runtime.cpp` ~:546).
  The new `session.compact` handler submits a body that calls
  `runtime_.agents().requestCompaction(id)`; `TurnExecutor::submit()` returning
  `false` (queue full / draining) yields `AgentErrorCode::InboxFull` /
  `RpcCode::InternalError` **before any item is enqueued**, so a rejected submit
  leaves no orphaned work (§6.7, C-F17). This is recorded as amendment A10; no
  `TurnExecutor` parameter is added to `AgentRegistry`'s constructors.
- **No `HostNoticeKind` extension.** Feedback travels in this RPC result (the
  `Queued` ack) and on the session event stream (the final outcome, §6.7); the
  frozen 4-value `HostNoticeKind` is untouched, and no new `EventType` is added.

---

## 7. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**C1 — No deletion.** Compaction never updates, deletes, or rewrites session
events. It appends exactly one `ContextCompaction` event and changes only the
projection. (§32, §54 D2, `01` I1)

**C2 — Turn-aligned boundary.** `ContextCompaction.boundary` is the `Sequence`
of a completed turn's terminal event (`TurnEnded`/`TurnCancelled`/`TurnFailed`)
in the resolved view. A non-turn boundary is never emitted; an externally
supplied non-turn boundary is rejected (§2.1, `01 §13.2` S7, C-F4).

**C3 — Strictly advancing boundary.** A new compaction's boundary is strictly
greater than the session's current effective boundary; otherwise no event is
emitted (§3.3, §2.2, C-F5).

**C4 — Tool pairing preserved.** No compaction splits a `tool_use` /
`ToolResult` pair: the boundary is turn-aligned, so all tool events of a
summarized turn are summarized together and all tool events of a retained turn
are retained together. (`01` I12)

**C5 — Projection purity and determinism.** Boundary selection and the
compaction fold read only `session.header()` and `session.events()`; no I/O, no
clock, no randomness. Identical `(header, events(), policy)` ⇒ identical
boundary and identical compacted context. Replay never re-summarizes. (`01` I7,
§44 replay tests)

**C6 — Summary durability.** The summary text is durable in the event; replay
uses the stored text and never invokes a model. (§4.3, C5)

**C7 — Threshold gating.** Compaction is attempted only when
`policy.is_enabled()` **and** either the estimate exceeds
`effective_threshold_tokens()` or the provider returned
`ContextLengthExceeded` with `retry_on_context_length`. (§3.1, §3.2)

**C8 — Bounded summary.** The emitted `summary` is `<= max_summary_tokens`
(estimated) and `<= max_summary_bytes` (serialized); an over-byte summary fails
the compaction rather than emitting an over-cap payload. (§3.5, `01` S10, C-F3)

**C9 — Pool discipline.** Every summarization call is bracketed by exactly one
`LLMPool` slot, released on completion, cancellation, and failure. (`06 §5.9`,
A12, C-F9)

**C10 — Usage attribution.** The summarizer's reported `Usage` is returned in
`CompactionResult::usage` and appended by the loop as a `TokenUsage` event
attributed to the loop-owned enclosing turn; the compactor (shared across
sessions) never appends it. It is never merged into the main request's usage,
and the main request's `Usage` is unaffected. (§5.7, `06 §8`, `08 §3.5`, C-F6)

**C11 — Cancellation.** `compact()` observes the turn's `CancellationToken` at
the pool acquire and during streaming; on cancellation it appends nothing and
returns `Cancelled`. (`§34`, C-F7)

**C12 — In-turn durability.** Every durable compaction event is appended inside
an open turn (`01` I11). The manual path always uses a dedicated maintenance
turn opened with `TurnOrigin::Maintenance` (§6.7, §11.3 A1).

**C13 — Best-effort failure (automatic path).** On the automatic path a failed
compaction does not fail a turn whose context still fits; the turn fails with
`TurnFailed{CompactionFailed}` only when the re-assembled context still
overflows. The manual maintenance turn is the exception: its single step *is*
the summarization, so a failure closes it with `TurnFailed{CompactionFailed}`
(§6.7). (`06 §5.3`, `06 §5.7`, A-F10, C-F1)

**C14 — Model recorded.** `ContextCompaction.model` is the effective summarizer
model actually used, resolved per `08 §5.2` (override first). It is never
empty when the outcome is `Compacted`. (L11)

**C15 — Estimate semantics.** `tokenEstimate` is the estimator's estimate of
the resulting compacted context — `summary + retained tail` (§3.4), computed
from the plan counts, with the assembler-added system prompt excluded — not the
summarizer call's usage. (§2.3, §3.4)

**C16 — Snapshot refresh.** After a committed compaction, the loop requests a
checkpoint when the snapshot threshold is met; the store replaces the snapshot
atomically and never serves a stale one. (`02 §6.4`–`§6.5`, `01` I21)

**C17 — Single in-flight compaction per session.** At most one summarization
call runs per session at a time; a second trigger in the same turn is coalesced
or no-op'd (`max_compactions_per_turn`). (`01` I18, C-F5)

**C18 — Config authority.** The policy comes from layered config (§37) and is
validated at load; no global mutable state and no hidden defaults outside
`CompactionPolicy`. (`09 §3.3` pattern)

**C19 — Deferred failure delivery.** An `InboxFull` rejection of a `Compact`
request never silently drops the acknowledged request: it sets the deferred
maintenance-failure trigger, and `activate()` **always** consumes it — idle:
`requestCompaction()` resets `Error → Idle` and calls `activate()` itself;
in-flight: the running `activate()` loop services it after the current turn
closes (including after a `TurnFailed` or `TurnCancelled` close); the loop
services the trigger before its `Error` break. Consumption emits exactly one zero-step
`TurnStarted{origin = Maintenance}` → `TurnFailed{code = InboxFull}` turn, then
clears the trigger. `dispose()` clears the trigger without emitting anything, so
no post-dispose turn exists. (§6.7/§6.8, C-F17b, `01` I11)

---

## 8. Failure modes

### 8.1 Shared findings (F1–F12, `§54`)

The compaction layer's responsibilities for the existing findings:

| F# | Finding | Compaction-layer handling |
|---|---|---|
| **F1** | path/process isolation | Compaction performs no path resolution and no process spawn; the summarize set is the projected message list only. It cannot read a file or leave the workspace. (`§9.7`, `§18`) |
| **F3** | late event after close | `dispose()` stops new work, clears any pending deferred maintenance-failure trigger, and emits no turn after disposal; no compaction is attempted after disposal; a compaction in flight is cancelled and appends nothing (C11, A14, C19). |
| **F5** | output ring buffers | The summary is bounded by `max_summary_tokens`/`max_summary_bytes` (C8); it cannot grow the durable payload without bound. (`§9.11`) |
| **F8** | resource caps | The summarization call holds one `LLMPool` slot (C9); a full pool suspends the caller cancellably, never spawns a thread (`06 §5.9`). |
| **F9** | cancellation scoping | Only the enclosing turn's token is observed; a cancel never affects another session (C11, `06` A9). |
| **F12** | flash clock in model | `createdAt` is read from the injected `WallClock` (`system_clock`, §5.2), **not** the broker's `steady_clock` `ClockReader` (`11 §7.2`, E21); the pure projection never reads `system_clock::now()`. |

### 8.2 Component-local failure modes (C-F1–C-F17b)

These are **component-local** to the compaction layer and are not part of the
top-level F1–F12 set. They are numbered `C-F#` and must be covered by tests
(§10.6).

| C-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **C-F1** | Summarizer provider terminal failure | `LLMResponse.outcome == Failed` (non-overflow) (`08 §2.2`) | Best-effort: append nothing; if the context still fits, proceed; else `TurnFailed{CompactionFailed}` (C13, A-F10). Never a silent partial summary. |
| **C-F2** | Summarizer input itself overflows | `LLMErrorCode::ContextLengthExceeded` on the summarization call | Reduce the summarize set (earlier boundary) and retry once; if it still overflows, fail (`SummarizerOverflow`) rather than recursing. Recursive summarization is deferred (decision C-D9). |
| **C-F3** | Oversized summary | summary bytes/tokens exceed the policy bound | Truncate to `max_summary_tokens` deterministically at a block boundary; if the serialized form still exceeds `max_summary_bytes`, fail `OversizedSummary` and append nothing (C8, `01` S10, `02` P-F15). |
| **C-F4** | Boundary off-by-one / invalid boundary | selected `Sequence` is not a compaction point, or is not committed in the resolved view | Never emitted: `select_boundary` only returns turn-terminal sequences; a manually supplied invalid boundary is rejected at append/validate (`01` S7) or clamped to the nearest earlier compaction point (C2, C-F15). |
| **C-F5** | Concurrent / duplicate compaction | two triggers in one turn, or a second attempt after a successful one | Single in-flight guard + `max_compactions_per_turn`; the second attempt is `NotNeeded` (boundary no longer advances, C3, C17). |
| **C-F6** | Usage accounting drift | summarizer usage added to, or replacing, the main request's `Usage` | The compactor returns usage in `CompactionResult`; the loop appends exactly one auxiliary `TokenUsage` for the enclosing turn. Tests assert the main request's usage is unchanged (C10, §5.7). |
| **C-F7** | Compaction during cancel | token fires at pool acquire or mid-stream (`08 §3.6`) | Append nothing; return `Cancelled`; the turn closes `TurnCancelled`, not `TurnFailed` (C11, `06` A10). |
| **C-F8** | Compaction loop (no net reduction) | estimate still exceeds the threshold after a successful compaction | `max_compactions_per_turn` bounds attempts; proceed if it fits, else `TurnFailed{CompactionFailed}` (C13). |
| **C-F9** | Pool exhaustion during compaction | `LLMPool::acquire` waits or the token fires | Cancellable acquire; if cancelled ⇒ C-F7; the slot is never leaked to a cancelled waiter (`06 §5.9`, C9). |
| **C-F10** | Lease lost during compaction | `Session::append` throws `LeaseLost` (`01` I5, `02 §5.7`) | Degrade to read-only; append nothing; surface to the supervisor (`01` S4, `06` A11). |
| **C-F11** | Store append failure | `StoreUnavailable` from `append` (`01` S11, `02` P-F#) | No durable compaction; the turn fails `StoreUnavailable` (A-F#), not `CompactionFailed`. |
| **C-F12** | Summary decode failure on replay | `from_json` fails for `ContextCompaction` (`01` S3) | Fail loud on the offending record; never silently skip (no lossy replay, C5, C6). |
| **C-F13** | Clock unavailable / non-monotonic | injected `WallClock` returns an unexpected value | `createdAt` is diagnostic only; it never affects boundary selection or the fold (C5). No failure path. |
| **C-F14** | Nothing to compact | prefix `< min_prefix_messages`, or no complete turn beyond `keep_recent_turns` | `NotNeeded`; append nothing; the loop proceeds uncompacted (§3.3). |
| **C-F15** | Manual request with no valid boundary | `/compact` when no boundary advances | Return `NotNeeded` with a redacted reason; never append a no-op compaction (C3). |
| **C-F16** | Estimator unavailable / misconfigured | `AgentServices::estimator == nullptr`, or policy disabled | Compaction is skipped (`NotNeeded`); no crash, no guess. The loop's `compaction_threshold_tokens == 0` gate already implements this (§3.2, C18). |
| **C-F17** | Executor submit rejected (manual path) | `TurnExecutor::submit()` returns `false` (queue full / draining) in the `session.compact` handler | Return `AgentErrorCode::InboxFull` / `RpcCode::InternalError` (E7). The `Compact` item is enqueued **inside** the accepted body, so a rejected submit enqueues nothing: no orphaned item, `hasPendingWork()` stays false. Never run the loop synchronously on the caller (§6.7, §6.8, A10). |
| **C-F17b** | Loop-inbox enqueue rejected (manual path) | The worker body's `AgentLoop::requestCompaction()` returns `InboxFull` (`inbox_.size() >= config_.max_inbox`) or `AgentDisposed` **after** the RPC already acked `Queued` | The `Compact` item is not queued and no compaction turn opens. **`AgentDisposed`**: return `AgentDisposed` and append **nothing** — post-dispose ops produce no event (`06` A14/A-F14, `06 §3.2`, §8.1 F3); the teardown/tombstone lifecycle (`06 §3.4`) is the signal. **`InboxFull`**: the `Compact` item cannot be queued (the inbox may be full while the loop is **idle**, e.g. holding only non-starting `Inject`s, or while a turn is in flight), so the failure is **deferred** to the loop's single-open-turn discipline — `requestCompaction()` sets a pending maintenance-failure trigger (counted by `hasTurnTrigger()`/`hasPendingWork()`) and **guarantees consumption**: it resets `Error → Idle` and calls `activate()` when `!running_` (emitting the zero-step maintenance failure turn `TurnStarted{Maintenance}` → `TurnFailed{InboxFull, message}` immediately), and the running `activate()` loop services the trigger after the in-flight turn closes (even if that turn closed `TurnFailed`). Exactly one turn is open (`01 §6.3`, I11) and the failure crosses the session event stream as `CompactionOutcome::Failed`. Never interleave two open turns and never drop the acknowledged request silently (§6.7, §6.8, C19). |

---

## 9. dsh (DeepSeek Harness) mapping

DeepSeek Harness treats the session log as the traceable source of truth and
keeps context management as an explicit, replayable concern. Compaction maps
onto that model as a projection over the append-only log:

```text
dsh concept                          ymh (this spec)
─────────────────────────────────────────────────────────────────────────────
append-only session log          →   Session event log (01); compaction is a
                                     ContextCompaction event, never a rewrite
context window management        →   CompactionPolicy + boundary selection (§3)
summarization as a model call    →   one LLMProvider.stream under one LLMPool
                                     slot (06 §5.9, 08 §3.4)
summary persisted as an event    →   payload::ContextCompaction (01 §4.5)
context rebuilt from the log     →   deriveMessages() fold (01 §6.3); replay
                                     never re-summarizes (C5, C6)
per-run traceability             →   boundary + model + createdAt + tokenEstimate
                                     make every compaction auditable (§4.1)
provider-agnostic model choice   →   per-request override (08 §5.2), so the
                                     summarizer may differ from the main model
cancellation propagation         →   the turn token reaches the summarizer call
                                     (C11, §34)
```

The key dsh-aligned insight (`§56`) is that **compaction is a projection, not a
mutation**: the log stays complete and replayable, and the compacted context is
a derived view that any frontend (TUI, RPC, replay tool) reconstructs
identically.

---

## 10. Test plan

Strategy is `§44`: unit tests, integration tests (Fake LLM `§45`, fake
clock/estimator), replay tests, and golden tests; a live layer (real LLM) is
opt-in. Compaction is deterministic under the fake layers and must be covered
by the hermetic suite.

### 10.1 Unit tests

- **`CompactionPolicy`.** `effective_threshold_tokens()` for: absolute trigger,
  ratio trigger, unknown window (disabled), `reserve_output_tokens` boundary,
  ratio clamping; validation rejects `max_summary_bytes > max_payload_bytes`.
- **Boundary selection (`plan`).** Turn-terminal only; `keep_recent_turns`
  honored (including 0); `NotNeeded` when fewer complete turns than
  `keep_recent_turns + 1`; `NotNeeded` when the candidate does not advance the
  effective boundary; `NotNeeded` below `min_prefix_messages`; an open trailing
  turn is ignored.
- **Summary prompt.** Deterministic rendering; no tools; bounded; redaction
  path does not log bodies by default.
- **Summary bounding.** Truncation at `max_summary_tokens`; `OversizedSummary`
  at `max_summary_bytes`; truncation is deterministic.
- **`ContextCompactor` construction.** Disabled policy ⇒ `NotNeeded`; null
  estimator ⇒ `NotNeeded`; a `WallClock` stub fixes `createdAt`; `compact()`
  returns usage without appending anything.
- **`tokenEstimate` semantics.** Equals the estimator applied to
  `summary + tail` (§3.4), not the summarizer usage; the system prompt is
  excluded (C15).
- **Tail computation.** The assembled list is `[lead] + projection`; `tail`
  derived from `plan.prefix_messages`/`plan.kept_messages` equals the projected
  suffix after the boundary for `lead ∈ {0, 1}` (§3.4).

### 10.2 Integration tests (Fake LLM, `§45`)

Using `FakeLLM` and a scripted `Compactor`/provider:

- **Threshold path.** A long history triggers exactly one `ContextCompaction`
  before the provider call; the re-assembled request drops pre-boundary
  messages and prepends the summary.
- **Overflow retry.** `FakeLLM` returns `ContextLengthExceeded` once, then
  succeeds; exactly one compaction is appended and the retried request fits.
- **Overflow failure.** `FakeLLM` keeps returning `ContextLengthExceeded`; the
  turn closes `TurnFailed{CompactionFailed}` (A-F10, C13).
- **No-op paths.** Disabled policy, nothing to compact, boundary not advancing
  ⇒ no `ContextCompaction` event; the turn is unaffected.
- **Usage attribution.** `FakeLLM` reports usage on the summarization call;
  `compact()` returns it in `CompactionResult::usage`; the loop appends exactly
  one auxiliary `TokenUsage` with the enclosing turn; the main request's
  `TokenUsage` is unchanged (C10, C-F6).
- **Cancellation.** Cancel during the summarization call: nothing appended,
  `TurnCancelled`, not `TurnFailed` (C11, C-F7).
- **Pool discipline.** A one-slot pool with a concurrent request: the
  summarizer waits and releases; no slot leak (C9, C-F9).
- **Lease loss.** Drop the lease before the append: `LeaseLost`, read-only,
  no compaction event (C-F10).
- **Manual path.** `session.compact` submits the enqueue to `TurnExecutor` and
  returns the `Queued` acknowledgement without running the loop on the dispatch
  thread; the worker body enqueues the `Compact` item via the public
  `AgentLoop::requestCompaction()` (so `hasPendingWork()` is true once it runs
  and the agent runs a maintenance turn with
  `TurnStarted{origin = TurnOrigin::Maintenance}`, appending exactly one
  `ContextCompaction`). If a turn is in flight when the request arrives, the
  maintenance turn runs **after** that turn (never folded into its step
  boundary). The maintenance turn emits no `AssistantMessage` (C12, §6.7).
- **Manual path, outcome carrier.** Drive each outcome through `session.compact`
  and assert the final `CompactionOutcome` is observable **only** on the session
  event stream (never a deferred RPC result): `Compacted` ⇒ `ContextCompaction`
  committed inside the maintenance turn; `NotNeeded` ⇒ maintenance-turn
  `TurnEnded` with no `ContextCompaction`; `Cancelled` ⇒ `TurnCancelled`;
  `Failed` ⇒ `TurnFailed{CompactionFailed}`. Assert the RPC result stays
  `{outcome: "Queued"}` and that no `HostNotice`/new `HostNoticeKind`/new
  `EventType` is emitted (§6.7/§6.8, A7/A11).
- **Manual path, full queue.** A full/draining `TurnExecutor` queue rejects the
  submit with `AgentErrorCode::InboxFull` **before** any item is enqueued: no
  orphaned `Compact` item, `hasPendingWork()` stays false (C12, §6.7, C-F17).
- **Manual path, full loop inbox (in flight).** With `inbox_.size() ==
  config_.max_inbox` while a turn is in flight, `session.compact` still acks
  `Queued` (the executor accepted the body); the worker body records the deferred
  maintenance-failure trigger instead of dropping the request, and the loop emits
  the zero-step maintenance failure turn (`TurnStarted{Maintenance}` →
  `TurnFailed{code=InboxFull}`) only **after** the in-flight turn's terminal event.
  Assert: no `Compact` item queued; `hasPendingWork()` is `true` while the trigger
  is pending and `false` after the failure turn; `validateLog` accepts the log
  (single `openTurn`, `01` I11); the outcome is observable on the event stream and
  maps to `CompactionOutcome::Failed` (C-F17b, §6.7/§6.8, C19).
- **Manual path, full loop inbox (idle).** Fill `inbox_` to `config_.max_inbox`
  with non-starting `Inject` items while the agent is **idle** (`running_ ==
  false`, `hasTurnTrigger() == false`), then issue `session.compact`. The worker
  body must not strand the trigger: `requestCompaction()` resets `Error → Idle`
  (if needed) and calls `activate()`, so the zero-step failure turn is emitted
  **immediately** with no in-flight turn to wait for. Assert: no `Compact` item
  queued; the failure turn is emitted in the same call; `hasPendingWork()` is
  `false` afterward; `validateLog` accepts the log; maps to
  `CompactionOutcome::Failed` (C-F17b, C19).
- **Manual path, full loop inbox after a failed turn.** With a turn in flight and
  a deferred trigger pending, let the in-flight turn close `TurnFailed` (setting
  `AgentState::Error`): the running `activate()` loop must still service the
  pending trigger (it is checked before the `Error` break) and emit the zero-step
  failure turn. Assert: two terminal `TurnFailed` events in order, each inside its
  own open turn (`01` I11); no stranded trigger (C19).
- **Manual path, disposed agent.** A `session.compact` whose worker body reaches
  `AgentLoop::requestCompaction()` after `dispose()` gets
  `AgentErrorCode::AgentDisposed` and appends **nothing**; assert no new event and
  no `TurnStarted` (C-F17b, §8.1 F3, `06` A14/A-F14).
- **Composition.** Two compactions in one session: the effective context uses
  only the latest summary; the fold is deterministic (§3.6).
- **Snapshot refresh.** After a committed compaction, `checkpoint` runs and the
  new snapshot equals `deriveMessages()` at head (C16, I21).

### 10.3 Replay-faithfulness tests

- Record a session log containing one or more `ContextCompaction` events;
  replay it; assert `deriveMessages()` is byte-identical to the live
  projection (C5, `01 §15.4`).
- Assert replay performs **no** provider call (the summarizer is never
  re-invoked) (C6).
- Fork a session after a compaction; assert the child's projection folds the
  inherited compaction identically (I10, §4.4).
- Resume from a snapshot taken after a compaction; assert equality with the
  full-log projection (I21, `02 §6.5`).
- Corrupt a summary JSON field; assert replay fails loud (`01` S3, C-F12).

### 10.4 Golden tests

- **TUI marker.** Given an event stream ending in a `ContextCompaction`, render
  the conversation view and assert the collapsed `CompactionMarker` and its
  expanded summary against an expected terminal representation (`§44` golden,
  §6.7).
- **Command palette.** `/compact` appears in `CommandRegistry::complete("/")`
  and its schema renders in the palette (`§26`).
- **Notice.** Given a maintenance-turn event stream, `UiEventAdapter` emits the
  expected transient `CompactionOutcomeNotice` for each outcome (`Compacted` /
  `NotNeeded` / `Cancelled` / `Failed`) derived from the `ContextCompaction` /
  `TurnEnded` / `TurnCancelled` / `TurnFailed` events (§6.7, A11).

### 10.5 Live end-to-end tests (real LLM, opt-in, `§44`)

- Drive the real binary under a PTY with `YMH_LIVE_LLM=1`; build a history long
  enough to cross a configured threshold; assert the session log carries a
  `ContextCompaction`, the turn completes, and the TUI shows the marker.
- Tolerant of model nondeterminism: assert structure and invariants (an event
  was emitted, the boundary is turn-aligned, the turn terminated), not summary
  prose. Skipped (not failed) without a key.

### 10.6 Failure-mode coverage matrix

| C-F# | Test layer | Scenario |
|---|---|---|
| C-F1 | integration (FakeLLM) | summarizer terminal failure; best-effort vs `CompactionFailed` |
| C-F2 | integration (FakeLLM) | summarizer input overflow; reduced boundary or fail |
| C-F3 | unit + integration | oversized summary; truncate or fail |
| C-F4 | unit + session | non-turn / uncommitted boundary rejected (S7) |
| C-F5 | integration | duplicate trigger in one turn ⇒ one event |
| C-F6 | integration | usage attribution; no drift |
| C-F7 | integration | cancel during summarization |
| C-F8 | integration | no net reduction after compaction |
| C-F9 | integration | pool exhaustion; cancellable acquire |
| C-F10 | integration | lease lost before append |
| C-F11 | integration | store append failure |
| C-F12 | replay | corrupt summary JSON fails loud |
| C-F13 | unit | clock stub; `createdAt` only |
| C-F14 | unit | nothing to compact ⇒ `NotNeeded` |
| C-F15 | integration + golden | `/compact` with no valid boundary |
| C-F16 | unit | null estimator / disabled policy |
| C-F17 | integration | `session.compact` on a full/draining `TurnExecutor` ⇒ `InboxFull`, no enqueue, `hasPendingWork()` false |
| C-F17b | integration | `session.compact` with a full loop inbox ⇒ `Queued` ack, then a zero-step maintenance failure turn (`TurnStarted{Maintenance}` → `TurnFailed{InboxFull}`) — after the in-flight turn closes, immediately when idle (full of non-starting injects), or after a failed in-flight turn; no `Compact` item, single `openTurn` (`01` I11), maps to `Failed`; post-`dispose()` ⇒ `AgentDisposed`, no event, trigger cleared |

### 10.7 Invariant coverage

| Invariant | Test layer |
|---|---|
| C1 (no deletion) | replay + store (`events` unchanged after compaction) |
| C2, C4 (turn-aligned, tool pairing) | unit boundary selection + projection |
| C3 (advancing) | unit + integration |
| C5, C6 (purity, durability) | replay |
| C7 (threshold gating) | unit + integration |
| C8 (bounded summary) | unit |
| C9 (pool) | integration |
| C10 (usage) | integration |
| C11 (cancel) | integration |
| C12 (in-turn) | integration (event ordering) |
| C13 (best-effort) | integration |
| C14 (model recorded) | unit + integration |
| C15 (estimate semantics) | unit |
| C16 (snapshot) | integration + replay |
| C17 (single in-flight) | integration |
| C18 (config authority) | unit |
| C19 (deferred failure delivery) | integration (idle / in-flight / failed-turn / disposed) |

---

## 11. Decisions and open questions

### 11.1 Decisions

**C-D1 — Interface naming.** The seam is `Compactor` (frozen by `06 §5.3`,
existing code); the concrete implementation is `ContextCompactor`. This spec
does not rename the frozen interface. New code calls `ContextCompactor::
compact()`; `run()` is the thin frozen adapter.

**C-D2 — Manual `/compact` surface.** The manual path is an **additive**
extension: RPC `session.compact` (Interactive, `05 §7`) → `HostRuntime` (which
owns the `TurnExecutor`) submits a worker body → `AgentRegistry::requestCompaction`
(additive to `06 §4.1`) → the **public** `AgentLoop::requestCompaction()`
(additive; the pinned enqueue target, A5), **enqueue-only on the executor
thread**. The enqueue happens **inside** the submitted body, never on the
io/dispatch thread, because the loop's inbox is executor-thread-confined and
unlocked (`06 §3.2`); this is the `agentPrompt`/`agentFollowup` shape
(`src/host/host_runtime.cpp` ~:546). It **enqueues pending work**
(`InboxKind::Compact`, counted by `hasTurnTrigger()`/`hasPendingWork()`), so the
maintenance turn runs on the worker (never on the io/dispatch thread); the
  request returns the `Queued` acknowledgement as soon as `TurnExecutor::submit()`
  accepts the body (or `AgentErrorCode::InboxFull` when the executor queue is full
  — in which case nothing is enqueued and no work is orphaned). A later rejection
  of the loop-inbox enqueue (post-ack) is handled without opening a second turn
  (`01` I11): `AgentDisposed` returns `AgentDisposed` and appends **nothing**
  (`06` A14/A-F14), while `InboxFull` records a deferred maintenance-failure
  trigger that is **guaranteed to be consumed** — `requestCompaction()` resets
  `Error → Idle` and calls `activate()` when the loop is idle, and the running
  `activate()` loop services it after an in-flight turn closes (even after a
  `TurnFailed` close) — emitting a zero-step maintenance failure turn
  (`TurnStarted{Maintenance}` → `TurnFailed`), delivered on the session event
  stream (§6.7/§6.8, C-F17b, C19). The manual request
is **always serviced as a dedicated maintenance turn** (`TurnOrigin::Maintenance`,
an **added** enumerator — §11.3 A1), whether the agent is idle or a turn is in
flight; if a turn is in flight, the maintenance turn runs after it. The
maintenance turn's single step is the summarization, so every durable compaction
event remains inside an open turn (`01` I11). The **final outcome crosses the
supervisor↔daemon boundary on the session event stream** (the maintenance turn's
own committed events), not as a "supervisor-local notice" and not as a deferred
RPC result (§6.7/§6.8, A7). No `Agent`/`§10.1` method is added; the enqueue
target is the additive public `AgentLoop::requestCompaction()`. Recorded here
rather than silently, because the frozen M2 catalog has no such method.

**C-D3 — Auxiliary usage accounting.** The summarizer's reported `Usage` is
returned in `CompactionResult::usage`; the loop — which owns the enclosing turn
and the `TokenUsage` append (`06 §8`) — appends the event. No
`CompactionUsageSink` and no per-session compactor construction is needed (the
shared-instance misattribution is removed). The frozen `Compactor::run` return
type is unchanged (C10, §5.7).

**C-D4 — Disabled by default.** `CompactionPolicy.enabled` defaults to `false`;
the existing `AgentConfig::compaction_threshold_tokens == 0` maps to disabled.
Compaction turns on when a threshold or a known model window is configured.
This preserves current behavior and avoids surprise summarization.

**C-D5 — Boundary policy.** Default `keep_recent_turns = 2`; the boundary is
the terminal event of the turn that many complete turns before the head.
Rationale: keep working context, summarize settled history.

**C-D6 — Summary role.** The summary is projected as `Role::System` (`01
§6.3`); it is not a user or assistant turn.

**C-D7 — Summarizer model.** Defaults to the session model; a
`CompactionPolicy.summarizer_model` override selects a cheaper model via the
`08 §5.2` per-request override. A separate summarizer *provider* is deferred
(OQ-C2).

**C-D8 — Unknown window ⇒ disabled.** With no absolute threshold and no known
`context_window_tokens`, compaction is disabled rather than guessed (C18,
C-F16).

**C-D9 — No recursive compaction.** The summarizer input is not itself
compacted in v1; if it overflows, the boundary is reduced once, then the
compaction fails (C-F2). Chunked/hierarchical summarization is deferred
(OQ-C1).

**C-D10 — `tokenEstimate` semantics.** It is the estimate of the resulting
compacted context (summary + retained tail), not the summarizer call's usage.
This is the value the TUI shows ("context now ~T tokens") and the value the loop
re-checks.

### 11.2 Open questions

- **OQ-C1 — Hierarchical / chunked summarization.** For very long histories,
  a single summarization call may itself overflow. The v1 answer is
  reduce-boundary-once-then-fail (C-F2, C-D9); a chunked map-reduce summarizer
  is a Phase-2 candidate.
- **OQ-C2 — Separate summarizer provider.** v1 resolves the summarizer model on
  the same provider (per-request override). Routing summarization to a different
  provider/endpoint (e.g. a local model) is a later enhancement; it would add a
  `ProviderRegistry` lookup to `ContextCompactor`.
- **OQ-C3 — Summary truncation provenance.** `ContextCompaction` has no field
  recording that the summary was truncated (C-F3). A payload extension (e.g. a
  `truncated` flag) would require an errata against `01 §4.5`; deferred.
- **OQ-C4 — Automatic vs manual policy split.** Whether the manual path should
  honor a different `keep_recent_turns` (e.g. summarize more aggressively on
  demand) is a UX question for spec 10; v1 uses one policy for both.
- **OQ-C5 — Estimator calibration.** `DefaultTokenEstimator` is a coarse
  bytes/4 heuristic (existing code). Calibrating it per model (or preferring
  provider-reported usage once available) is out of scope here; the policy
  already tolerates error via `reserve_output_tokens`.

A genuine cross-document contradiction, if review finds one, reopens this
subsection rather than being resolved silently.

### 11.3 Required cross-spec / code amendments (errata ledger)

This spec is frozen against the M1/M2 code and specs `01`, `06`, `10`, `11`.
The re-review fixes below force a small set of **additive** amendments outside
this file; they are recorded here (this spec does not edit those documents) so
the catalog stays the single authority. None of them changes an existing
method signature or semantics.

> **Re-review fixes (M3 — post-ack rejection: single-open-turn / post-dispose).**
> Two MEDIUM findings forced a correction to the post-ack rejection contract
> (§6.7/§6.8, C-F17b):
>
> 1. **`AgentDisposed` no longer appends.** The previous text opened a
>    `TurnStarted{Maintenance}` → `TurnFailed` turn after `dispose()`,
>    contradicting `06` A14/A-F14 ("post-dispose ops return `AgentDisposed`; no
>    event"), `06 §3.2` ("a rejected op … produces none"), and §8.1 F3. The
>    `AgentDisposed` path now returns `AgentDisposed` and appends **nothing**.
>    **No carve-out is required** — the spec now conforms to the frozen
>    lifecycle instead of overriding it.
> 2. **`InboxFull` no longer appends inline.** The previous text appended a
>    failure turn from `requestCompaction()` with no ordering against an
>    already-open turn; when a turn *is* in flight that interleaves two open
>    turns, which `01`'s single-`openTurn` projection / `validateLog` (I11)
>    rejects. (A full inbox does **not** imply an in-flight turn — `enqueue()`
>    only calls `activate()` when `!running_` and `hasTurnTrigger()` is false for
>    non-starting `Inject`s, so an idle loop can hold a full inbox; the earlier
>    citation `src/agent/agent_loop.cpp:132-146` proved neither claim.) The
>    failure is now **deferred** to the loop's single-open-turn discipline: a
>    loop-internal maintenance-failure trigger (A4) whose consumption is
>    guaranteed in every state — `requestCompaction()` resets `Error → Idle` and
>    calls `activate()` when idle, and the running `activate()` loop services it
>    after an in-flight turn closes (even a `TurnFailed` close) — is emitted as
>    the failure turn exactly once. This is an **additive code amendment**
>    (A4/A5), **not** a carve-out against `01` — it *restores* conformance with
>    I11.

| # | Target | Required amendment | Why (finding) |
|---|---|---|---|
| A1 | `include/ymh/session/events.hpp`, `01 §4.5`, `src/session/events.cpp` | Add `TurnOrigin::Maintenance` (5th enumerator) plus its `turn_origin_name`/`parse_turn_origin` mapping. | §6.7 maintenance turn; `TurnOrigin::Injection` is reserved for a `ContextInjected` turn and must not be overloaded. |
| A2 | `10` (TUI) turn-origin rendering | Map `TurnOrigin::Maintenance` to the maintenance/compaction view (no user/steer/injection semantics). | A1. |
| A3 | `include/ymh/agent/agent_loop.hpp` (`AgentServices`) | Add `ContextCompactor* context_compactor = nullptr;` (additive; frozen `Compactor* compactor` retained). | §5.2/§5.8/§6.7: the loop needs `compact()`/`CompactionResult`. |
| A4 | `include/ymh/agent/agent_loop.hpp`/`.cpp` | Add `InboxKind::Compact` (no completion handle) counted by `hasTurnTrigger()`/`hasPendingWork()`; service it **always** as a dedicated maintenance turn (after any in-flight turn), never folded into another turn; `runCompaction` calls `compact()` and appends `TokenUsage` from `result.usage`; the turn closes `TurnEnded` (`Compacted` or `NotNeeded`), `TurnCancelled`, or `TurnFailed{CompactionFailed}` accordingly. Also add a loop-internal **deferred maintenance-failure trigger** (set by `requestCompaction()` on `InboxFull`) counted by `hasTurnTrigger()`/`hasPendingWork()`, and amend `activate()` to service it **before** the `Error` break; `requestCompaction()` resets `Error → Idle` and calls `activate()` when `!running_`, so the zero-step `TurnStarted{Maintenance}` → `TurnFailed{InboxFull, message}` turn is emitted exactly once whether the loop was idle, in flight, or in `Error` (`01` I11); `dispose()` clears the trigger without emitting. | §6.7 activation + §5.7 usage ownership + the event-stream outcome carrier (§6.7/§6.8) + the single-`openTurn` fix for C-F17b/C19. |
| A5 | `include/ymh/agent/agent_loop.hpp`, `include/ymh/agent/agent_registry.hpp` / `06 §4.1` | Add **public** `AgentLoop::requestCompaction() -> std::expected<CompactionOutcome, AgentError>` — the pinned enqueue target: enqueue the `InboxKind::Compact` work item (on the executor thread) and return `Queued`; on a rejected enqueue it never appends inline — `AgentDisposed` returns `AgentDisposed` and appends **nothing** (`06` A14/A-F14, `06 §3.2`), while `InboxFull` sets the deferred maintenance-failure trigger (A4) that is **guaranteed to be consumed** — immediately via `activate()` when idle (after an `Error → Idle` reset), or by the running `activate()` loop after an in-flight turn closes (even a `TurnFailed` close) — emitting a zero-step maintenance failure turn (`TurnStarted{Maintenance}` → `TurnFailed{InboxFull, message}`), so exactly one turn is open (`01` I11) and the acked request is never silently dropped (§6.8, C-F17b, C19). Add `AgentRegistry::requestCompaction(SessionId) -> std::expected<CompactionOutcome, AgentError>` that looks up the registered `AgentLoop` (`agents_`) and delegates to it. `AgentLoop::enqueue()` stays private; no `Agent` (`§10.1`) method is added. Neither takes a `TurnExecutor` dependency; the submit lives in `HostRuntime` (A10). | §6.7/§6.8 manual path; C-D2 forbids an `Agent` method and `enqueue()` is private. |
| A6 | `06 §5.1`, `§5.3`, `§5.7` prose | State that the compaction call site uses the rich `ContextCompactor::compact()` and that the loop appends the summarizer `TokenUsage`; `run()` remains the frozen adapter. | §5.7/§6.1. |
| A7 | `10 §4.5`/`§5.2` (feedback) | The `/compact` outcome is **not** a "supervisor-local notice" with no wire carrier: it crosses the boundary on the **session event stream** — the maintenance turn's own committed events (`ContextCompaction` / `TurnEnded` / `TurnCancelled` / `TurnFailed`) that the TUI already consumes (`05 §5.2` `SessionEnvelope` → `10 §4.5`, adapted by `10 §5.2`). No `protocol::HostNotice`, no `HostNoticeKind` extension, no new `EventType`. | §6.7/§6.8 HIGH finding: the previous text claimed a supervisor-local notice but defined no wire carrier, so `NotNeeded`/`Cancelled`/`Failed` could never cross the process boundary. |
| A8 | `01 §6.3` (prose clarification) | State explicitly that the synthetic summary's origin `Sequence` is the `ContextCompaction` event's own `record.seq` (matching `src/session/session.cpp`). | §4.2/§3.6 correction. |
| A9 | `11 §7.2` (clarification, no change) | `ClockReader` is `steady_clock`-based; it is **not** a wall clock and is not reused here. Compaction pins a distinct `WallClock = std::function<system_clock::time_point()>`. | §2.3/§5.2 finding 1. |
| A10 | `include/ymh/host/host_runtime.hpp`/`.cpp` (new `session.compact` handler), `05 §7` catalog | The handler submits a `TurnExecutor` worker body that calls `runtime_.agents().requestCompaction(id)`; the enqueue happens **inside** the body, so a rejected submit (`InboxFull`) enqueues nothing (no orphan, `hasPendingWork()` stays false). Mirrors `agentPrompt`/`agentFollowup` (`src/host/host_runtime.cpp` ~:546). No `TurnExecutor` parameter is added to `AgentRegistry`. | §6.7/§6.8 finding: `AgentRegistry` has no `TurnExecutor`; the submit owner is the daemon. |
| A11 | `10 §5.1`/`§5.2` (`UiEvent`, `UiEventAdapter`) | Add a frontend-only `UiEvent::CompactionOutcomeNotice` variant (session + `CompactionOutcome` + boundary/`tokenEstimate`/model/reason) and have `UiEventAdapter` emit it by correlating the `TurnOrigin::Maintenance` turn (A1/A2) with its terminal event and the presence/absence of `ContextCompaction` (A7). `UiEvent` remains wire-free (`§54 D15`, T4). | A7: the `/compact` outcome needs a supervisor-side rendering target derived from the wire-carried durable events. |

---

## 12. References

- `00-architecture.md` §4.2 (event stream is the runtime spine), §8.1 (durable
  vs live events), §9.1 (session as event log), §9.4 (fork), §9.5 (replay),
  §10.1 (`Agent` handle), §11 (agent loop turn/step), §12 (LLM interface), §18
  (execution environment), §19 (permissions), §20.8 (`ConversationModel`),
  §20.11 (`UiController`), §20.12 (pure rendering), §20.23 (aggregate status),
  §26 (slash command architecture), §31 (context management), §32
  (**compaction**), §33 (token accounting), §34 (cancellation), §37
  (configuration), §40 (logging), §44 (testing strategy), §45 (Fake LLM), §46
  (security model), §54 (D1–D23, F1–F12), §55 (dsh comparison), §56 (most
  important architectural insight).
- `01-session.md` §2.1 (identifiers), §4.3 (`EventType::ContextCompaction`,
  `TokenUsage`), §4.5 (`payload::ContextCompaction`, `TokenUsage`,
  `TurnOrigin`, `TurnFailed`), §6.1 (append protocol), §6.2 (resolved view),
  §6.3 (`deriveMessages` compaction fold), §6.4 (snapshot), §9.1 (create),
  §9.2 (resume), §9.3 (fork), §9.4 (replay), §9.5 (delete), §12 (I1–I23), §13
  (F1–F12, S1–S14, esp. S3/S7/S10), §15 (test plan).
- `02-persistence.md` §4.1 (`PersistenceConfig`), §6.1 (commit/flush), §6.4
  (snapshot creation after a compaction), §6.5 (staleness, S12), §6.6 (snapshot
  encoding), §7 (crash recovery).
- `04-workspace-host-daemon.md` §2.1 (`ResourceCaps`), §4.1 (`HostConfig`), §8
  (`ResourceGovernor`, `LLMPool` placement), §9 (concurrency).
- `05-transport.md` §6.3 (profiles), §7.1 (naming/error conventions), §7.4
  (session lifecycle methods), §7.5 (`agent.*`), §8 (backpressure).
- `06-agent-loop.md` §4.1 (`AgentRegistry`, `AgentServices` bundle), §5.1
  (turn/step algorithm, compaction call sites), §5.2 (`ContextAssembler`), §5.3
  (**`Compactor` seam**), §5.7 (retry / `CompactionFailed`), §5.9 (`LLMPool`),
  §8 (token estimation), §9 (threading), §10 (A1–A18, esp. A10/A12), §11
  (A-F9/A-F10), §14 (decisions/open questions).
- `08-llm-provider.md` §2.2 (error taxonomy), §3.1 (`LLMRequest`), §3.2
  (`StreamEvent`), §3.4 (`LLMProvider`), §3.5 (usage/token accounting), §3.6
  (cancellation), §3.7 (retry), §5.2 (effective model resolution), §5.3 (config
  layering), §9 (threading), §12 (L1–L17).
- `09-permissions.md` §3.3 (`PermissionConfig`/`PolicyConfigError` pattern).
- `10-supervisor-tui.md` §3.2 (layered architecture / `UiController`), §3.4
  (pure rendering), §4.7 (`AggregateStatusModel`), §8.2 (renderers), §9.2
  (`InputView`, slash-command delegation), §9.3 (draft/history).
- `11-m2-errata.md` §4.4 (typed error mapping), §7.2 (injectable clock-reader
  pattern; `ClockReader` is `steady_clock`-based and is **not** reused — §11.3
  A9), E21.
- `07-tools-execution.md` §5.5 (`ToolConfig` pattern).
- Existing code verified against this spec: `include/ymh/permission/
  permission_broker.hpp` (`ClockReader` = `steady_clock`), `include/ymh/session/
  events.hpp` (`ContextCompaction.createdAt` = `system_clock`, `TurnOrigin`,
  `TokenUsage`), `include/ymh/llm/llm_provider.hpp` (`LLMResponse` has no
  `text`; `stream` returns `Task<LLMResponse>`), `include/ymh/core/task.hpp`
  (eager `Task<T>::get()`), `include/ymh/agent/llm_pool.hpp`
  (`acquire` → `Task<std::optional<Slot>>`), `include/ymh/agent/
  context_assembler.hpp`/`src/agent/context_assembler.cpp`
  (`SessionContextAssembler::systemPrompt_`, `Compactor`), `include/ymh/agent/
  agent_loop.hpp` (`AgentServices::compactor`), `src/agent/agent_loop.cpp`
  (`hasPendingWork`, `runCompaction`), `src/agent/workspace_runtime.cpp`
  (shared service bundle), `src/session/session.cpp` (summary origin =
  `record.seq`), `include/ymh/transport/protocol.hpp` (`HostNoticeKind`, 4
  values).
