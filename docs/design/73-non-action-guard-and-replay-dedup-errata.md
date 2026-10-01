# 73 — Non-Action Turn Guard & Replay-Idempotent Transcript Errata

Status: **verified (Rev 1)** — Oracle re-gate PASS (0 HIGH / 0 MEDIUM; reviewer deepseek-flash,
2026-10-01; the first gate FAILed 0 HIGH / 2 MEDIUM / 4 LOW on spec/tracker wording only and
F1–F6 were fixed). See `DESIGN_STATUS.md` row 73.

```
Component: 73 (errata) — amends 06-agent-loop.md, 19-session-rename-errata.md
           (which amends 01-session.md), and 17-ui-transcript-errata.md
           (which amends 10-supervisor-tui.md), by reference only
Depends on: 06-agent-loop.md (verified), 01-session.md (verified),
            19-session-rename-errata.md (verified), 10-supervisor-tui.md
            (verified), 17-ui-transcript-errata.md (verified),
            62-composer-caret-and-step-limit-errata.md (verified; recoverable
            `TurnFailed{StepLimitExceeded}` precedent, 62-D4)
Scope: two ymh-side defects in an otherwise model-caused failure
```

## 1. Purpose, scope, and precedence

Two independent ymh-side defects turn a model-side persona leak into a silent
no-op and into an over-counted transcript:

1. **Non-action acceptance (agent loop).** `runTurn`
   (`src/agent/agent_loop.cpp:1371-1391`; the unchanged terminal is `:1392`)
   ended a turn the moment a `Completed` provider response carries
   `tool_calls.empty()`. There was **no content
   inspection and no retry**: a completion with no tool call *and no text at all*
   is recorded as a finished turn. A coding agent must not silently accept an
   empty completion as an answer.
2. **Non-idempotent transcript append (UI model).** `UiModel::apply`
   (`src/ui/ui_model.cpp:1042-1053`, `:1184-1193`) `push_back`s every
   `UserMessage` / `ContextInjected` entry with no id check, so a replayed event
   stream double-renders the same prompt / identity context.

**Out of scope (must not be changed).** The persona text itself ("I'm DSA LLM…")
is **model-side**. This errata does **not** inspect, match, or special-case any
identity, vendor, model name, or prompt wording (73-D1, 73-I5). It does not touch
the model endpoint, config, or the `harness:identity` prompt section
(`src/cli/wiring.cpp`). It only fixes the two acceptance/rendering defects.

**Precedence.** For the non-action guard this errata amends `06` §5.6/§5.8 and
decision (g); for rendering it amends `17`/`10` §5.1/§8.2; for event-id
replay-safety it amends `19`/`01` §4.5. It is **additive**: no new `EventType`,
no new `AgentErrorCode`, no wire change, no schema change.

**Gate (per `AGENTS.md`).** Independent Oracle review must mark this `verified`
with no open HIGH/MEDIUM findings. The `design_status_drift` ctest checks this
spec's header against its tracker row.

## 2. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 73-D1 | **Non-action predicate (structural, content-based).** After a `Completed` stream, a turn is a *non-action* iff `response.tool_calls.empty()` **and** the settled assistant text is **blank** — empty or whitespace-only. The settled text is the concatenation of `ContentBlockKind::Text` blocks of the settled `payload::AssistantMessage` (reasoning blocks excluded). No other property is inspected: not sender, not identity, not vendor/model name, not prompt wording, not language. | Amends 06 §5.6; new |
| 73-D2 | **One bounded corrective nudge per turn.** On the first non-action of a turn the loop (a) appends the step boundary `StepEnded{turn, step}`, (b) appends **exactly one** corrective user-role message — a `payload::UserMessage` whose `source.kind == MessageSource::Kind::Plugin` and `source.plugin == "agent-nudge"` — and (c) `continue`s the step loop, consuming one `stepNumber` bounded by the absolute per-turn ceiling `max_turn_steps` (62-D9); `segment_step` (`max_steps`, the per-segment budget, 62-D3) is incremented only after tool execution, so a text-only nudge step does not consume it. The nudge is therefore the last user-role message in the reassembled request and cannot be re-triggered as "the prompt". | Amends 06 §5.1/§5.8 |
| 73-D3 | **At most one nudge per user turn.** A per-`runTurn` flag (`nudge_emitted_for_turn`), reset at the start of every `runTurn`, gates 73-D2. A second non-action in the same turn cannot re-enter the nudge branch. There is no loop. | Amends 06 §5.8; new |
| 73-D4 | **Bounded stop.** If the post-nudge step is itself a non-action, the turn stops with the durable, **recoverable** `TurnFailed{StepLimitExceeded}` (`appendTurnFailed(..., recoverable=true)`), message actionable ("no substantive response after corrective nudge — send a message to continue"), and the loop returns to `Idle`. This reuses the 62-D4 recoverable-stop precedent; no new `EventType`/`AgentErrorCode`. | Amends 06 §5.6/§5.8, A-F4; cites 62-D4 |
| 73-D5 | **Ordinary short / conversational replies end normally.** A `Completed` response with no tool call but **non-blank** text (e.g. `Done.`) takes the unchanged terminal path: `TurnEnded`, no nudge, no failure. The predicate is blank-only by construction, so no semantics are inferred from the text. | Amends 06 §5.6; new |
| 73-D6 | **The nudge is user-visible and not user history.** The corrective message is rendered by the UI as a user-role transcript entry (every `UserMessage` renders), so it is never silently swallowed; because its source is `Plugin` (not `User`) it is **not** added to the composer input history (`src/ui/ui_model.cpp:1049-1051`) and does not trigger the `JobWakeupPolicy` user-message path (`src/jobs/job_wakeup.cpp:115-118`). | New; cites 37-message-provenance |
| 73-D7 | **Replay-idempotent transcript.** `ConversationModel` deduplicates `UserMessage` and `ContextInjected` entries on the event `MessageId`: an entry whose `MessageId` was already applied is not appended again. The seen-id set lives in `ConversationModel` (one per `SessionUiState`, i.e. per session), is populated on first application, and dies with the state; it is never serialized and never written to the durable log. | Amends 17/10 §5.1/§8.2; amends 19/01 §4.5 |
| 73-D8 | **Dedup is presentation-only and the 58-A10 reset is honored.** It never suppresses a new id, never mutates the event log, and never affects live append (each live event carries a fresh id). `UiEventAdapter::forget_session` — the 58-A10/E46 "re-entry will replay" signal — also clears the session's model seen-ids (`ConversationModel::reset_dedup`, entries untouched), so the documented pop/re-enter re-application still works while an accidental replay without a forget is idempotent. Scope is exactly the two id-carrying, previously un-deduped events (`UserMessage`, `ContextInjected`); assistant/tool/status entries keep their existing `by_message` handling. | Amends 17/10; cites 58-A10/E46 |

## 3. Interface sketch

```cpp
// src/agent/agent_loop.cpp (anonymous namespace) — file-local helpers
std::string text_of_blocks(const std::vector<ContentBlock>& blocks);  // Text only
bool        is_blank(std::string_view text);                          // empty/ws-only

// src/agent/agent_loop.cpp -- runTurn(), outer step scope
std::string settled_text;             // declared beside `settledUsage` (outside the
                                      // per-attempt loop); assigned on Completed
bool        nudge_emitted_for_turn = false;   // per-turn; reset at runTurn start
// when response.tool_calls.empty() && is_blank(settled_text):
//   first time  -> StepEnded; appendUserMessage(nudge Message{Plugin,"agent-nudge"}); continue;
//   after nudge -> TokenUsage?; StepEnded; appendTurnFailed(StepLimitExceeded, ms, true); return;
// else (non-blank, no tools) -> unchanged TurnEnded terminal.

// include/ymh/ui/ui_model.hpp
struct ConversationModel {
    // ... existing entries / by_message / by_reasoning_message ...
    bool mark_seen(const MessageId& id);   // returns true iff newly inserted
    void reset_dedup();                    // 73-D8: called by forget_session
  private:
    std::unordered_set<MessageId> seen_ids_;
};

// src/ui/ui_event_adapter.cpp — the 73-D8 reset caller
void UiEventAdapter::forget_session(const SessionId& id) {
    // ... 58-A10 adapter dedup reset ...
    session->second.conversation.reset_dedup();   // drop seen-ids, keep entries
}
```

The nudge helper has its concrete caller at the 73-D2 branch in `runTurn`;
`make_nudge_message`/`is_blank`/`text_of_blocks` are file-local and each has a
caller there; `mark_seen` is called at the two `apply` sites (73-D7);
`reset_dedup` is called by `UiEventAdapter::forget_session` (73-D8). Each new
symbol has exactly one caller and none is dead.

## 4. Invariants

| ID | Invariant |
|---|---|
| 73-I1 | The normal terminal path at `agent_loop.cpp:1392` is reached only when the turn is **not** a non-action (73-D1), or a nudge was already emitted this turn (73-D4). |
| 73-I2 | At most one corrective `agent-nudge` `UserMessage` is appended per user turn. |
| 73-I3 | The post-nudge non-action stop is `TurnFailed{StepLimitExceeded}`, recoverable, with the loop returning to `Idle` (not `Error`). |
| 73-I4 | The guard adds **at most one** provider call per user turn (it cannot loop: 73-I2 + the `max_turn_steps` ceiling). This is orthogonal to the provider's own two-attempt context-length retry (`agent_loop.cpp:1212`, `:1313-1334`), which may add one further call only on `ContextLengthExceeded`. |
| 73-I5 | No code path added by this errata reads, matches, or branches on identity, vendor, model name, or prompt wording. |
| 73-I6 | A `Completed` response with non-blank text and no tool call ends with `TurnEnded` and no `TurnFailed`/nudge. |
| 73-I7 | Each `MessageId` in a `UserMessage`/`ContextInjected` UI event appears at most once in `ConversationModel.entries`, live or replayed. |
| 73-I8 | An id not yet seen still appends exactly once (live append unchanged). |
| 73-I9 | Dedup never removes or rewrites a durable session event. |

## 5. Failure modes

| ID | Failure | Disposition |
|---|---|---|
| 73-F1 | Nudge storm / loop | 73-D3 flag + 73-I2; a second non-action stops (73-D4). |
| 73-F2 | False nudge on a legitimate short reply | 73-D1 blank-only predicate excludes any non-whitespace text; 73-I6. |
| 73-F3 | Semantic / identity / vendor detection reintroduced | Forbidden by 73-D1 / 73-I5; the persona case is model-side and out of scope. |
| 73-F4 | Nudge invisible / history pollution | 73-D6 (Plugin source: rendered, not in composer history, not a job-wakeup user message). |
| 73-F5 | Replay double-render | 73-D7 / 73-I7. |
| 73-F6 | Dedup set growth | Bounded by session lifetime; the model dies with its `SessionUiState`. It is also cleared by `reset_dedup` on `forget_session` (73-D8), so it never outlives the session's re-entry cycle. |
| 73-F7 | Guard masks a genuine provider failure | Provider `Failed`/`Cancelled` are handled **before** the guard (`agent_loop.cpp:1352-1364`) and are untouched; the guard only observes `Completed`. |

## 6. dsh mapping

| dsh concept | Mirror | Justification |
|---|---|---|
| Empty-completion handling | **Non-mirror (deliberate divergence)** | dsh does not define an empty-completion retry; ymh adds one structural nudge bound to one per turn. Reason: ymh is a coding agent and must not silently accept an empty turn; it is a deliberate scope decision (73-D1/D2) with no dsh anchor. |
| Transcript idempotence | **Mirror (conceptual)** | dsh replays a durable event stream into a fresh view; ymh's `ConversationModel` must be idempotent on a re-applied stream. Anchor: 01 §4.5 event ids, 29-D4. |

## 7. Test plan

Hermetic (FakeLLM, `tests/support/agent_test_env.hpp`), no live model:

| ID | Test | Asserts |
|---|---|---|
| 73-U1 | `AgentLoopNonActionGuard.BlankThenDoneNudgesOnceAndEnds` (`tests/unit/agent_non_action_test.cpp`) | script `[blank, "Done."]`: exactly one `agent-nudge` `UserMessage`, two `AssistantMessage`, one `TurnEnded`, zero `TurnFailed`, state `Idle`, final assistant text `Done.`. |
| 73-U2 | `AgentLoopNonActionGuard.BlankThenBlankBoundedStop` | script `[blank, blank]`: one nudge, exactly two `AssistantMessage` (no loop), one recoverable `TurnFailed{StepLimitExceeded}`, state `Idle`, no `TurnEnded`. |
| 73-U3 | `AgentLoopNonActionGuard.ShortAnswerNoNudge` | script `["Done."]`: zero nudge, zero `TurnFailed`, one `AssistantMessage`, one `TurnEnded`, state `Idle`. |
| 73-U4 | `UiModelDedup.ReplayedUserAndContextIdsAppendOnce` (`tests/unit/ui_model_dedup_test.cpp`) | apply `UserMessage{id=X}` twice then `UserMessage{id=Y}` → 2 user entries; apply `ContextInjected{id=Z}` twice → 1 context entry; a distinct id still appends. |
| 73-U5 | `UiModelDedup.PluginUserMessageRendersWithoutHistory` | a `Plugin`-source `UserMessage` renders one user entry but leaves `InputModel::history` empty, while a `User`-source message is recorded in history — the 73-D6 side-effect contract. |

**Pre-fix evidence (honest).** Before the code change: 73-U1 sees one
`AssistantMessage` and one `TurnEnded` but **zero** nudges (the guard does not
exist) — the first blank completion ends the turn; 73-U4 sees **two** user
entries for the same `id=X`. **Three** tests fail pre-fix (73-U1, 73-U2, 73-U4);
**73-U3 passes pre-fix by construction** — it asserts the *absence* of the new
behavior on non-blank text and is the false-positive guard. All four pass
post-fix.

**Pre-existing tests adapted to the new behavior (not weakened).**
`HeadlessTest.DurableMessageWithoutLiveChunksIsHandled` and
`HostRuntimeTest.AgentPromptAutoNamesOnce` scripted a blank/exhausted completion
and asserted `turn/end`; the guard now makes a blank completion a non-action, so
each gains the substantive follow-up its scenario needs (the durable empty
message and the auto-name assertions are unchanged). `UiEventAdapter.UI58_U24_ForgetSessionClearsDedup`
stays green unchanged because 73-D8 preserves the 58-A10 reset.

## 8. Supersedes / amendments

- **06-agent-loop.md** §5.1, §5.6, §5.8, decision (g): add the non-action
  terminal condition and the one-nudge bound (73-D1..D5, 73-I1..I6).
- **17-ui-transcript-errata.md** (amending **10-supervisor-tui.md** §5.1/§8.2):
  the `UserMessage`/`ContextInjected` append is idempotent on `MessageId`
  (73-D7/D8, 73-I7..I9).
- **19-session-rename-errata.md** (amending **01-session.md** §4.5): replaying a
  log of id-carrying user/context events into a consumer is idempotent on the
  event `MessageId`; the log itself is unchanged (73-D7/D8).
- No event, wire, schema, config, or model change.
