# 73 — Non-Action Turn Guard & Replay-Idempotent Transcript Errata

Status: **verified (Rev 3)** · reviewer: independent review team, round 3
(2026-10-05): PASS · gate: 0 HIGH / 0 MEDIUM (6 LOW). Rev 3 adds decision 73-D11
(same-prompt answer repetition is also a non-action) and its
`kSamePromptRepeatOccurrences = 3` threshold. Rev 2 was gate-passed (Oracle
re-gate PASS, 0 HIGH / 0 MEDIUM; reviewer deepseek-flash, 2026-10-01; round 1
FAILed 1 HIGH / 3 MEDIUM / 3 LOW, all fixed before round 2 PASS) and added
decisions 73-D9/73-D10 (cross-turn answer repetition is also a non-action; see
73-D11 for the same-prompt arm). Rev 1 was Oracle re-gate PASS (0 HIGH / 0 MEDIUM;
1 residual LOW). See `DESIGN_STATUS.md` row 73.

```
Component: 73 (errata) — amends 06-agent-loop.md, 19-session-rename-errata.md
           (which amends 01-session.md), and 17-ui-transcript-errata.md
           (which amends 10-supervisor-tui.md), by reference only
Depends on: 06-agent-loop.md (verified), 01-session.md (verified),
            19-session-rename-errata.md (verified), 10-supervisor-tui.md
            (verified), 17-ui-transcript-errata.md (verified),
            62-composer-caret-and-step-limit-errata.md (verified; recoverable
            `TurnFailed{StepLimitExceeded}` precedent, 62-D4)
Scope: four ymh-side defects in an otherwise model-caused failure (non-action
       acceptance; transcript idempotence; cross-turn repetition; same-prompt
       repetition)
```

## 1. Purpose, scope, and precedence

Four independent ymh-side defects turn a model-side persona leak into a silent
no-op, a repeating no-op, a same-prompt repeating no-op, and an over-counted
transcript:

1. **Non-action acceptance (agent loop).** `runTurn`
   (`src/agent/agent_loop.cpp:1443-1460`; the unchanged terminal is `:1481`)
   ended a turn the moment a `Completed` provider response carries
   `tool_calls.empty()`. There was **no content
   inspection and no retry**: a completion with no tool call *and no text at all*
   is recorded as a finished turn. A coding agent must not silently accept an
   empty completion as an answer.
2. **Non-idempotent transcript append (UI model).** `UiModel::apply`
   (`src/ui/ui_model.cpp:1042-1053`, `:1184-1193`) `push_back`s every
   `UserMessage` / `ContextInjected` entry with no id check, so a replayed event
   stream double-renders the same prompt / identity context.
3. **Repetition blindness (agent loop, Rev 2).** 73-D1's predicate is
   *blank-only by construction*, so it cannot see the more common persona leak:
   the model answers different user prompts with **the same non-blank text** and
   no tool call (`I'm <persona>, an AI assistant …`), once per turn, forever. In
   the captured failure the same identity sentence answered `TEDAI-567` (typed
   ~10×), `TEDAI-56`, `TEDAI-5`, gibberish, `what?` (×3) and `what happened?` —
   many *different* prompts, one *identical* no-tool answer — until the model
   self-recovered. Each such turn ends `TurnEnded` normally, so the user must
   retype and the loop repeats for as many turns as the model stutters.
4. **Same-prompt repetition blindness (agent loop, Rev 3).** 73-D9's arm needs
   `kRepeatOccurrences = 3` **distinct** normalized prompts, so it structurally
   cannot see the *other* shape of the same stall: the user retyping the **same**
   one-word prompt (e.g. `yes`, typed ~16× in the captured failure) while the
   model returns the **same** non-blank identity sentence, `tool_calls=0`,
   `finish=stop`, every time. The prompt hash stays `hash("yes")` — one distinct
   prompt — so 73-D9's distinct count never reaches 3; 73-D1's blank arm is false
   because the text is non-blank; and each turn ends `TurnEnded` normally. The
   loop runs unbounded and the user must retype forever. `repetition_eligible`
   (`src/agent/agent_loop.cpp:1152-1159`) admits `yes` (non-empty), but a
   verbatim-prompt retry never accumulates distinct prompts. Rev 3 closes this
   with the same-prompt arm 73-D11.

**Out of scope (must not be changed).** The persona text itself ("I'm DSA LLM…")
is **model-side**. This errata does **not** inspect, match, or special-case any
identity, vendor, model name, or prompt wording (73-D1, 73-D9, 73-D11, 73-I5). It
does not touch the model endpoint, config, or the `harness:identity` prompt
section (`src/cli/wiring.cpp`). It only fixes the
acceptance/rendering/repetition defects. 73-D9 and 73-D11 compare **only the
model's own successive outputs and the user prompt** to each other (normalized,
structural): 73-D9 builds a set of distinct prompt hashes per answer hash, 73-D11
counts occurrences of one `(prompt_hash, answer_hash)` **pair**; neither compares
output to any expected string and neither contains a vendor/identity/model-name
constant (73-I5/73-I11/73-I14).

**Precedence.** For the non-action guard this errata amends `06` §5.6/§5.8 and
decision (g); for rendering it amends `17`/`10` §5.1/§8.2; for event-id
replay-safety it amends `19`/`01` §4.5. It is **additive**: no new `EventType`,
no new `AgentErrorCode`, no wire change, no schema change. Rev 3 adds a third
arm to the **same** non-action predicate (73-D11) and a named constant, and
otherwise reuses 73-D2/73-D3/73-D4 unchanged.

**Gate (per `AGENTS.md`).** Independent Oracle review must clear this errata with
no open HIGH/MEDIUM findings before it is coded or promoted out of draft. The
`design_status_drift` ctest checks this spec's header against its tracker row.

## 2. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 73-D1 | **Non-action predicate (structural, content-based).** After a `Completed` stream, a turn is a *non-action* iff `response.tool_calls.empty()` **and** the settled assistant text is **blank** — empty or whitespace-only. The settled text is the concatenation of `ContentBlockKind::Text` blocks of the settled `payload::AssistantMessage` (reasoning blocks excluded). No other property is inspected: not sender, not identity, not vendor/model name, not prompt wording, not language. *(Rev 2: the predicate is broadened by 73-D9 to `blank OR repeat`; Rev 3: broadened again by 73-D11 to `blank OR distinct-prompt repeat OR same-prompt repeat`. The blank arm is unchanged and 73-I5 still forbids any semantic matching.)* | Amends 06 §5.6; new |
| 73-D2 | **One bounded corrective nudge per turn.** On the first non-action of a turn the loop (a) appends the step boundary `StepEnded{turn, step}`, (b) appends **exactly one** corrective user-role message — a `payload::UserMessage` whose `source.kind == MessageSource::Kind::Plugin` and `source.plugin == "agent-nudge"` — and (c) `continue`s the step loop, consuming one `stepNumber` bounded by the absolute per-turn ceiling `max_turn_steps` (62-D9); `segment_step` (`max_steps`, the per-segment budget, 62-D3) is incremented only after tool execution, so a text-only nudge step does not consume it. The nudge is therefore the last user-role message in the reassembled request and cannot be re-triggered as "the prompt". | Amends 06 §5.1/§5.8 |
| 73-D3 | **At most one nudge per user turn.** A per-`runTurn` flag (`nudge_emitted_for_turn`), reset at the start of every `runTurn`, gates 73-D2. A second non-action in the same turn cannot re-enter the nudge branch. There is no loop. | Amends 06 §5.8; new |
| 73-D4 | **Bounded stop.** If the post-nudge step is itself a non-action, the turn stops with the durable, **recoverable** `TurnFailed{StepLimitExceeded}` (`appendTurnFailed(..., recoverable=true)`), message actionable ("no substantive response after corrective nudge — send a message to continue"), and the loop returns to `Idle`. This reuses the 62-D4 recoverable-stop precedent; no new `EventType`/`AgentErrorCode`. | Amends 06 §5.6/§5.8, A-F4; cites 62-D4 |
| 73-D5 | **Ordinary short / conversational replies end normally.** A `Completed` response with no tool call but **non-blank** text (e.g. `Done.`) takes the unchanged terminal path: `TurnEnded`, no nudge, no failure. *(Rev 2: a non-blank, no-tool reply ends normally **only when it is not a 73-D9 repetition**; Rev 3: nor a 73-D11 same-prompt repetition.)* No semantics are inferred from the text. | Amends 06 §5.6; new |
| 73-D9 | **Cross-turn repetition is also a non-action (Rev 2).** A non-blank `Completed` response with `tool_calls.empty()` is a *repeat* iff its **normalized** answer equals the normalized answer of at least `kRepeatOccurrences = 3` turns whose normalized user prompts are **pairwise distinct** — counted as a set of distinct prompt hashes, so a verbatim prompt retry never inflates the count — the current turn included. Only **eligible** turns participate: those carrying non-empty user-prompt text. An `Inject` turn (or an empty/whitespace-only prompt) neither records nor matches, so a non-user prompt never enters the distinct count. Comparison is **exact after normalization**, not a similarity score: `normalize_reply` (a) trims leading/trailing ASCII whitespace, (b) collapses every run of ASCII whitespace (`space tab \\n \\r \\f \\v`) to one `space`, and (c) ASCII-case-folds `A`–`Z` to `a`–`z`. No Unicode normalization, no punctuation stripping, no vendor/identity/model-name constant. The signal is therefore *different prompts → same answer*; a genuinely repeated identical prompt can never satisfy it. Repetition is evaluated only on no-tool turns (tool-call turns are covered by the 62-D6 no-progress guard); it is a **separate, additive arm of the same 73-D1 predicate** and enters the **same** 73-D2 nudge / 73-D4 bounded stop unchanged. *(Rev 3: complemented by 73-D11, which covers the verbatim-prompt case this set-based arm deliberately excludes; the distinct-arm semantics here are unchanged.)* | Amends 06 §5.6/§5.8; new |
| 73-D10 | **Bounded per-session observation ring (Rev 2).** The repetition signal is computed from one bounded ring of the last `kAnswerLogCapacity = 8` completed-turn observations, `AnswerObservation{std::string prompt_hash; std::string answer_hash;}` (SHA-256 of the normalized prompt / normalized answer, so entry size is fixed regardless of prompt length), owned by the `AgentLoop` (one per `Session`), worker-thread-only, never serialized, never written to the durable log. A turn is observed **once**, at its first no-tool terminal decision: that decision consumes the turn's single observation even when its answer is blank, and an entry is recorded only for an **eligible, non-blank** answer (`answer_observed_for_turn` + `repetition_eligible` + a non-empty answer hash). A turn's post-nudge answer is therefore never recorded, which is exactly what lets a post-nudge repeat observe the prior-turn entry and escalate via 73-D3/73-D4. The ring is evicted oldest-first at the fixed cap; it dies with the loop and is reset by a resume (new `AgentLoop`) — cross-restart repetition is deliberately out of scope. *(Rev 3: the same pair ring backs the 73-D11 same-prompt occurrence count; `kAnswerLogCapacity = 8` covers `kSamePromptRepeatOccurrences = 3` with margin (73-I15), so no new bound is required.)* | Amends 06 §5.8; new |
| 73-D11 | **Same-prompt answer repetition is also a non-action (Rev 3).** A non-blank `Completed` response with `tool_calls.empty()` is a *same-prompt repeat* iff the **exact** normalized `(prompt_hash, answer_hash)` **pair** has been observed in at least `kSamePromptRepeatOccurrences = 3` completed turns — the current turn included, including the current turn's just-recorded observation (73-D10). This is exactly the case 73-D9 excludes: one verbatim prompt, one verbatim answer, retyped forever (the captured `yes` × ~16 failure). Threshold **3** is the smallest count that cannot be a one-off: 1 is the current turn alone; 2 is a single prior match, which a coincidentally re-sent legitimate prompt can produce; 3 requires **two** independent prior identical observations. Like 73-D9 it applies only to **eligible** turns (non-empty normalized user prompt) and only to no-tool turns, and it compares hashes of structurally normalized text (`normalize_reply` + SHA-256) — no identity, vendor, model-name, or prompt-wording constant (73-I5/73-I14). It is a **third, additive arm of the same 73-D1 predicate**: `non_action = no_tool && (blank OR repeated_distinct_prompt OR repeated_same_prompt)`, and it enters the **same** 73-D2 one-nudge / 73-D4 bounded stop unchanged. The first same-prompt turn whose pair count reaches 3 earns the turn's single nudge; that turn's **post-nudge** no-tool response is the "second" non-action (the post-nudge answer is never recorded, so the count is still ≥3) and stops recoverably. | Amends 06 §5.6/§5.8; new |
| 73-D6 | **The nudge is user-visible and not user history.** The corrective message is rendered by the UI as a user-role transcript entry (every `UserMessage` renders), so it is never silently swallowed; because its source is `Plugin` (not `User`) it is **not** added to the composer input history (`src/ui/ui_model.cpp:1049-1051`) and does not trigger the `JobWakeupPolicy` user-message path (`src/jobs/job_wakeup.cpp:115-118`). | New; cites 37-message-provenance |
| 73-D7 | **Replay-idempotent transcript.** `ConversationModel` deduplicates `UserMessage` and `ContextInjected` entries on the event `MessageId`: an entry whose `MessageId` was already applied is not appended again. The seen-id set lives in `ConversationModel` (one per `SessionUiState`, i.e. per session), is populated on first application, and dies with the state; it is never serialized and never written to the durable log. | Amends 17/10 §5.1/§8.2; amends 19/01 §4.5 |
| 73-D8 | **Dedup is presentation-only and the 58-A10 reset is honored.** It never suppresses a new id, never mutates the event log, and never affects live append (each live event carries a fresh id). `UiEventAdapter::forget_session` — the 58-A10/E46 "re-entry will replay" signal — also clears the session's model seen-ids (`ConversationModel::reset_dedup`, entries untouched), so the documented pop/re-enter re-application still works while an accidental replay without a forget is idempotent. Scope is exactly the two id-carrying, previously un-deduped events (`UserMessage`, `ContextInjected`); assistant/tool/status entries keep their existing `by_message` handling. | Amends 17/10; cites 58-A10/E46 |

## 3. Interface sketch

```cpp
// src/agent/agent_loop.cpp (anonymous namespace) — file-local helpers
std::string text_of_blocks(const std::vector<ContentBlock>& blocks);  // Text only
bool        is_blank(std::string_view text);                          // empty/ws-only
std::string normalize_reply(std::string_view text);   // 73-D9: trim + collapse ASCII ws + ASCII lower
constexpr std::size_t kRepeatOccurrences  = 3;         // 73-D9
constexpr std::size_t kAnswerLogCapacity  = 8;         // 73-D10
constexpr std::size_t kSamePromptRepeatOccurrences = 3; // 73-D11

// include/ymh/agent/agent_loop.hpp — private, worker-only (73-D9/D10/D11)
struct AnswerObservation { std::string prompt_hash; std::string answer_hash; };

// include/ymh/agent/agent_loop.hpp — private member + methods
std::deque<AnswerObservation> recent_answers_;        // 73-D10 ring, cap kAnswerLogCapacity
void note_answer_observation(const std::string& prompt_hash,
                             const std::string& answer_hash);            // 73-D10, one caller in runTurn
bool repeats_prior_answer(const std::string& prompt_hash,
                          const std::string& answer_hash) const;          // 73-D9, one call in runTurn
// repeats_prior_answer counts the set of DISTINCT stored prompt hashes whose
// answer hash matches (plus the current prompt hash), never raw occurrences.
bool repeats_same_prompt_answer(const std::string& prompt_hash,
                                const std::string& answer_hash) const;    // 73-D11 (Rev 3), one call in runTurn
// repeats_same_prompt_answer counts RAW occurrences of the exact
// (prompt_hash, answer_hash) pair (current observation included) and returns
// true iff the count >= kSamePromptRepeatOccurrences; false when answer_hash
// is empty.

// src/agent/agent_loop.cpp -- runTurn(), outer step scope
std::string settled_text;             // declared beside `settledUsage` (outside the
                                      // per-attempt loop); assigned on Completed
bool        nudge_emitted_for_turn = false;   // per-turn; reset at runTurn start
bool        answer_observed_for_turn = false; // 73-D10: one observation per turn
const std::string normalized_prompt =
    normalize_reply(text_of_blocks(trigger.message.content));
const bool repetition_eligible = !normalized_prompt.empty(); // 73-D9: excludes Inject/empty
const std::string current_prompt_hash = sha256_hex(normalized_prompt);
// at the terminal decision, `no_tool = response.tool_calls.empty()`:
//   blank = no_tool && is_blank(settled_text);
//   answer_hash = no_tool && !blank ? sha256_hex(normalize_reply(settled_text)) : "";
//   if (no_tool && !answer_observed_for_turn) {          // first no-tool decision, even if blank
//       answer_observed_for_turn = true;
//       if (repetition_eligible && !answer_hash.empty())
//           note_answer_observation(current_prompt_hash, answer_hash);
//   }
//   repeated_distinct = no_tool && repetition_eligible
//                       && repeats_prior_answer(current_prompt_hash, answer_hash);      // 73-D9
//   repeated_same     = no_tool && repetition_eligible
//                       && repeats_same_prompt_answer(current_prompt_hash, answer_hash); // 73-D11
//   non_action = no_tool && (blank || repeated_distinct || repeated_same); // 73-D1 + 73-D9 + 73-D11
//   first non-action  -> StepEnded; appendUserMessage(nudge Message{Plugin,"agent-nudge"}); continue;
//   after nudge       -> TokenUsage?; StepEnded; appendTurnFailed(StepLimitExceeded, ms, true); return;
//   else (non-blank, no tools, not repeated) -> unchanged TurnEnded terminal.

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
`reset_dedup` is called by `UiEventAdapter::forget_session` (73-D8).
`normalize_reply` is called at the 73-D9 capture and terminal-decision sites;
`note_answer_observation`, `repeats_prior_answer` and (Rev 3)
`repeats_same_prompt_answer` each have their single caller at the 73-D10 record
site, the 73-D9 distinct-arm predicate site, and the 73-D11 same-prompt-arm
predicate site respectively, all inside `runTurn`; the `recent_answers_` ring is
mutated only by `note_answer_observation`. `kSamePromptRepeatOccurrences` is read
only by `repeats_same_prompt_answer`. Each new symbol has exactly one caller and
none is dead.

### 3.1 Rev-2 → Rev-3 storage and predicate delta (exact signatures)

**Anchor check (verified).** The shipped Rev-2 ring is `std::deque<AnswerObservation>`
of `(prompt_hash, answer_hash)` **pairs** (`src/agent/agent_loop.cpp:1097-1118`,
`include/ymh/agent/agent_loop.hpp:199-212`, `:265`). The `answer_hash → set<prompt_hash>`
shape named for 73-D9 is the *logical projection* `repeats_prior_answer` computes
(`distinct_prompts.insert(...)`, `:1110-1117`), **not** the stored representation.
The pair ring is already the prerequisite 73-D11 needs, so Rev 3 changes **no**
storage: it adds one predicate over the same pairs.

| Element | Rev 2 (before) | Rev 3 (after) |
|---|---|---|
| Ring member | `std::deque<AnswerObservation> recent_answers_;` | **unchanged** — cap `kAnswerLogCapacity = 8`, oldest-first eviction |
| `AnswerObservation` | `{ std::string prompt_hash; std::string answer_hash; }` | **unchanged** |
| `note_answer_observation` | `void note_answer_observation(const std::string& prompt_hash, const std::string& answer_hash);` | **unchanged** signature; still records one eligible non-blank observation per turn at the first no-tool decision |
| `repeats_prior_answer` | `bool repeats_prior_answer(const std::string& prompt_hash, const std::string& answer_hash) const;` — distinct-prompt arm | **unchanged** signature and semantics (73-D9 preserved exactly); one call in `runTurn` |
| `repeats_same_prompt_answer` | *(does not exist)* | **new** `[[nodiscard]] bool repeats_same_prompt_answer(const std::string& prompt_hash, const std::string& answer_hash) const;` — returns `false` when `answer_hash` is empty, else counts records whose `(prompt_hash, answer_hash)` equals the arguments and returns `count >= kSamePromptRepeatOccurrences` |
| `kSamePromptRepeatOccurrences` | *(does not exist)* | **new** `constexpr std::size_t kSamePromptRepeatOccurrences = 3;` (73-D11) |
| `non_action` | `no_tool && (blank \|\| repeated)` | `no_tool && (blank \|\| repeated_distinct \|\| repeated_same)` |

Both predicates read the **same** ring, so a single `note_answer_observation`
call feeds both arms; the current turn's observation is recorded **before** the
predicates run (`:1452-1460`, the record guard then the `non_action` predicate), so
73-D11's count includes it, exactly as 73-D9's
distinct set does. Neither predicate mutates the ring, and neither exists in
`ConversationModel`/UI.

## 4. Invariants

| ID | Invariant |
|---|---|
| 73-I1 | The normal terminal path at `agent_loop.cpp:1481` is reached only when the turn is **not** a non-action (73-D1 as broadened by 73-D9/73-D11), or a nudge was already emitted this turn (73-D4). |
| 73-I2 | At most one corrective `agent-nudge` `UserMessage` is appended per user turn. |
| 73-I3 | The post-nudge non-action stop is `TurnFailed{StepLimitExceeded}`, recoverable, with the loop returning to `Idle` (not `Error`). |
| 73-I4 | The guard adds **at most one** provider call per user turn (it cannot loop: 73-I2 + the `max_turn_steps` ceiling). This is orthogonal to the provider's own two-attempt context-length retry (`agent_loop.cpp:1283`, `:1383-1405`), which may add one further call only on `ContextLengthExceeded`. |
| 73-I5 | No code path added by this errata reads, matches, or branches on identity, vendor, model name, or prompt wording. |
| 73-I6 | A `Completed` response with non-blank text, no tool call, and **no 73-D9 and no 73-D11 repetition** ends with `TurnEnded` and no `TurnFailed`/nudge. |
| 73-I10 | 73-D9 fires only when the same normalized answer has been produced for `kRepeatOccurrences = 3` **distinct prompt hashes** (the current turn included). Distinctness is set-based, so retrying one prompt verbatim never inflates the count (73-F2/73-F8); and only **eligible** turns (non-empty normalized user prompt) record or match, so `Inject`/empty-prompt turns never enter the count. *(Rev 3: the verbatim-prompt case this invariant excludes is handled by the **separate** 73-D11 same-prompt arm — see 73-I14 — not by this count, whose semantics are unchanged.)* |
| 73-I11 | The repetition arms (73-D9 distinct-prompt and 73-D11 same-prompt) inspect only the model's own Text output and the user prompt text, both normalized structurally; they never contain or compare a vendor/identity/model-name constant (extends 73-I5). |
| 73-I12 | The observation ring is bounded by `kAnswerLogCapacity = 8`, is observed at most once per turn (at the first no-tool decision, whether blank or not), records only an eligible non-blank answer, and is never serialized, so repetition state cannot grow with session length (73-D10; AGENTS.md state-lifetime rule; see §9 lifetime table). |
| 73-I13 | Repetition (both arms) reuses the **same** 73-D2 nudge and the **same** 73-D3/73-D4 bound: at most one nudge per turn, a second non-action stops recoverably. It adds no new `EventType`, `AgentErrorCode`, wire, schema, or UI. |
| 73-I14 | 73-D11 fires iff the exact normalized `(prompt_hash, answer_hash)` pair has been recorded for at least `kSamePromptRepeatOccurrences = 3` turns, the current turn included, counting **raw occurrences** (not a set). It is gated by `repetition_eligible` and an empty `answer_hash` never matches, so an `Inject`/empty-prompt turn or a blank answer cannot fire it. Because the current turn's observation is recorded before the predicate (`:1452-1460` — the `note_answer_observation` guard at `:1452-1457` precedes the `repeated`/`non_action` predicate at `:1458-1460`), **two** prior identical `(same prompt, same answer)` turns plus the current one reach the threshold, and **one** prior does not. |
| 73-I15 | The same-prompt arm is satisfiable within the ring bound: `kSamePromptRepeatOccurrences = 3 <= kAnswerLogCapacity = 8`, so three identical pairs can coexist in the last eight observations. No new bound is introduced (no 73-D12); the pair ring of 73-D10/73-I12 is reused unchanged. |
| 73-I16 | On the turn whose pair count first reaches `kSamePromptRepeatOccurrences`, the first no-tool response earns the single 73-D2 nudge and that **same turn's post-nudge** no-tool response is the second non-action that triggers the 73-D4 recoverable stop. The post-nudge answer is never recorded (73-D10), so the count stays >= threshold and the escalation is deterministic. |
| 73-I7 | Each `MessageId` in a `UserMessage`/`ContextInjected` UI event appears at most once in `ConversationModel.entries`, live or replayed. |
| 73-I8 | An id not yet seen still appends exactly once (live append unchanged). |
| 73-I9 | Dedup never removes or rewrites a durable session event. |

## 5. Failure modes

| ID | Failure | Disposition |
|---|---|---|
| 73-F1 | Nudge storm / loop | 73-D3 flag + 73-I2; a second non-action stops (73-D4). |
| 73-F2 | False nudge on a legitimate short reply | The blank arm excludes any non-whitespace text and the repeat arms need prior history, so a **single** short reply (`Done.`) never fires (73-I6). Rev 2: 73-D9 requires ≥3 **distinct** prompts, so a short reply repeated because the user re-sent the **same** prompt never fires *that* arm. **Rev 3: 73-D11 deliberately fires when the same prompt + same non-blank answer repeats ≥3× (73-I14) — this is the captured `yes` loop, not a one-off (threshold 3, 73-I15); the cost is one recoverable nudge/stop, never a silent end (73-D4/73-I3).** |
| 73-F3 | Semantic / identity / vendor detection reintroduced | Forbidden by 73-D1 / 73-D9 / 73-D11 / 73-I5 / 73-I11 / 73-I14; the persona case is model-side and out of scope. |
| 73-F8 | False positive on a legitimate repeated answer to genuinely different prompts | Accepted residual risk, bounded: the signal is exact-after-normalization equality (no fuzzy match), requires `kRepeatOccurrences = 3` distinct prompts, and only ever costs one recoverable nudge/escalation — never a silent end (73-D4) and never a hard `Error` (73-I3). The alternative (a similarity threshold) was rejected as strictly higher-risk. |
| 73-F9 | Repetition ring grows without bound | Bounded by `kAnswerLogCapacity = 8`, at most one observation per turn, oldest-first eviction (73-D10/73-I12); fixed-size hashes, not raw text. |
| 73-F11 | A retried identical prompt inflates the distinct-prompt count | The count is a **set of distinct prompt hashes** (73-I10), so repeated identical prompts collapse; only an injection/empty-prompt turn is excluded outright. *(Rev 3: the same-prompt retry is instead counted, deliberately, by the **separate** 73-D11 arm — it never touches the 73-D9 set.)* |
| 73-F12 | Same-prompt false positive: a user legitimately re-sends an identical prompt and the model legitimately gives the identical short answer ≥3× | Accepted residual risk, bounded and symmetric with 73-F8: the pair is matched **exactly after normalization** (no similarity), the threshold is 3 (73-D11/73-I15), and the outcome is at most one recoverable nudge then a recoverable stop (73-D4/73-I3) — never a silent end and never a hard `Error`. The alternative (no same-prompt arm) leaves the captured unbounded `yes` loop unfixed. |
| 73-F13 | Same-prompt arm misses a model that varies its answer each time under the same prompt | Accepted residual, by design: exact-pair equality (no fuzzy/similarity matching) is the false-positive guard 73-F3/73-I5 requires. A model that varies its non-blank no-tool answer under one verbatim prompt is not the captured defect; tool-call turns remain covered by the 62-D6 no-progress guard and every turn by `max_turn_steps` (62-D9). |
| 73-F10 | Post-nudge repeat fails to escalate (recorded too early) | The turn's observation is recorded once, at its first no-tool decision, and never for the post-nudge answer; the prior-turn entry remains (and, for 73-D11, the pair count stays ≥3), so the post-nudge repeat still matches and escalates via the existing `nudge_emitted_for_turn` (73-D10, 73-I13, 73-I16). |
| 73-F4 | Nudge invisible / history pollution | 73-D6 (Plugin source: rendered, not in composer history, not a job-wakeup user message). |
| 73-F5 | Replay double-render | 73-D7 / 73-I7. |
| 73-F6 | Dedup set growth | Bounded by session lifetime; the model dies with its `SessionUiState`. It is also cleared by `reset_dedup` on `forget_session` (73-D8), so it never outlives the session's re-entry cycle. |
| 73-F7 | Guard masks a genuine provider failure | Provider `Failed`/`Cancelled` are handled **before** the guard (`agent_loop.cpp:1423-1435`) and are untouched; the guard only observes `Completed`. |

## 6. dsh mapping

| dsh concept | Mirror | Justification |
|---|---|---|
| Empty-completion handling | **Non-mirror (deliberate divergence)** | dsh does not define an empty-completion retry; ymh adds one structural nudge bound to one per turn. Reason: ymh is a coding agent and must not silently accept an empty turn; it is a deliberate scope decision (73-D1/D2) with no dsh anchor. |
| Transcript idempotence | **Mirror (conceptual)** | dsh replays a durable event stream into a fresh view; ymh's `ConversationModel` must be idempotent on a re-applied stream. Anchor: 01 §4.5 event ids, 29-D4. |
| Cross-turn answer repetition | **Non-mirror (deliberate divergence)** | dsh has no cross-turn repetition guard. Reason: ymh is a coding agent whose model may stall on a base-persona reply to *different* prompts; the guard is a ymh-side acceptance fix, deliberate scope (73-D9/D10), with no dsh anchor and no vendor/identity matching (73-I11). |
| Same-prompt answer repetition | **Non-mirror (deliberate divergence)** | dsh has no repetition guard at all, so it has none for the verbatim-prompt case either. Reason: the captured failure is the user retyping one prompt (`yes` ×~16) while the model returns one no-tool identity sentence, so the distinct-prompt arm (73-D9) cannot fire; this is a ymh-side acceptance fix, deliberate scope decision **73-D11** (threshold `kSamePromptRepeatOccurrences = 3`, 73-I14), with no dsh anchor. It compares only normalized `(prompt_hash, answer_hash)` pairs and never a vendor/identity/model-name constant (73-I5/73-I11). |

## 7. Test plan

Hermetic (FakeLLM, `tests/support/agent_test_env.hpp`), no live model; the
agent-loop cases live in `tests/unit/agent_non_action_test.cpp` and the UI cases
in `tests/unit/ui_model_dedup_test.cpp`, all compiled into the `ymh_tests`
executable (`tests/CMakeLists.txt:1`), discovered via `gtest_discover_tests`
(`tests/CMakeLists.txt:194`). New Rev-3 cases extend the existing
`AgentLoopNonActionGuard.*` fixture in `tests/unit/agent_non_action_test.cpp`:

| ID | Test | Asserts |
|---|---|---|
| 73-U1 | `AgentLoopNonActionGuard.BlankThenDoneNudgesOnceAndEnds` (`tests/unit/agent_non_action_test.cpp`) | script `[blank, "Done."]`: exactly one `agent-nudge` `UserMessage`, two `AssistantMessage`, one `TurnEnded`, zero `TurnFailed`, state `Idle`, final assistant text `Done.`. |
| 73-U2 | `AgentLoopNonActionGuard.BlankThenBlankBoundedStop` | script `[blank, blank]`: one nudge, exactly two `AssistantMessage` (no loop), one recoverable `TurnFailed{StepLimitExceeded}`, state `Idle`, no `TurnEnded`. |
| 73-U3 | `AgentLoopNonActionGuard.ShortAnswerNoNudge` | script `["Done."]`: zero nudge, zero `TurnFailed`, one `AssistantMessage`, one `TurnEnded`, state `Idle`. |
| 73-U4 | `UiModelDedup.ReplayedUserAndContextIdsAppendOnce` (`tests/unit/ui_model_dedup_test.cpp`) | apply `UserMessage{id=X}` twice then `UserMessage{id=Y}` → 2 user entries; apply `ContextInjected{id=Z}` twice → 1 context entry; a distinct id still appends. |
| 73-U5 | `UiModelDedup.PluginUserMessageRendersWithoutHistory` | a `Plugin`-source `UserMessage` renders one user entry but leaves `InputModel::history` empty, while a `User`-source message is recorded in history — the 73-D6 side-effect contract. |
| 73-U6 (Rev 2) | `AgentLoopNonActionGuard.RepeatedAnswerToDifferentPromptsNudgesThenEscalates` (`tests/unit/agent_non_action_test.cpp`) | three `send`s with three *different* prompts, each answered with the same non-blank no-tool text (`A`): turns 1–2 `TurnEnded` (occurrence 1, 2 < 3); turn 3 fires the repetition arm — exactly one `agent-nudge`, then the post-nudge answer (`A` again) escalates: one recoverable `TurnFailed{StepLimitExceeded}`. Script `[A, A, A, A]`; asserts nudge count 1, `AssistantMessage` 4, `TurnEnded` 2, `TurnFailed` 1, terminal 3, final state `Idle`. This is the reported defect. |
| 73-U7 (Rev 2, revised Rev 3) | `AgentLoopNonActionGuard.SamePromptSameAnswerFiresSamePromptArm` (`tests/unit/agent_non_action_test.cpp`) | the **same** prompt three times, each answered the same short `Done.`: turns 1–2 `TurnEnded` (pair count 1, 2 < 3); turn 3's first response reaches pair count 3 → exactly one `agent-nudge`, then the post-nudge `Done.` fits 73-I16 and escalates: one recoverable `TurnFailed{StepLimitExceeded}`. Script `[Done., Done., Done., Done.]`; nudge 1, `AssistantMessage` 4, `TurnEnded` 2, `TurnFailed` 1, terminal 3, final `Idle`. **Rev 3 supersedes the Rev-2 `SamePromptSameAnswerNeverFires` expectation** — this is exactly the case Rev 2 deliberately did not catch and is the reported defect. The Rev-2 false-positive guard for the *distinct* arm is retained by 73-U10. |
| 73-U8 (Rev 2) | `AgentLoopNonActionGuard.TwoRepeatsThenDistinctAnswerDoesNotFire` | two different prompts answered `A` (no fire; 73-I10 bound), then a third different prompt answered a *different* text: zero nudges, three `TurnEnded`, zero `TurnFailed`. |
| 73-U9 (Rev 2) | `AgentLoopNonActionGuard.VariedRepliesNoToolNoNudge` | three different prompts answered with three *different* non-blank texts: zero nudges, zero `TurnFailed`, three `TurnEnded`. |
| 73-U10 (Rev 2) | `AgentLoopNonActionGuard.SamePromptRetryPlusDifferentPromptNeverFires` | the *same* prompt twice plus one *different* prompt, all answered `A`: zero nudges, three `TurnEnded` — the set-based distinct-hash count (73-I10/73-F2); fails against occurrence-counting code (gate HIGH-1). |
| 73-U11 (Rev 2) | `AgentLoopNonActionGuard.RepetitionMatchesAfterNormalization` | three distinct prompts answered with `I am stuck.` / `i AM   stuck.` / `  I am stuck.  ` (equal only after trim + whitespace-collapse + ASCII-case-fold), then a fourth: one nudge, one recoverable `TurnFailed` — exercises all three `normalize_reply` transforms (gate MEDIUM-4). |
| 73-U12 (Rev 2) | `AgentLoopNonActionGuard.InjectionTurnDoesNotParticipateInRepetition` | two distinct prompts answered `A`, then an `Inject` turn (`startsTurn = true`, empty user text) also answered `A`: zero nudges, three `TurnEnded` — ineligible turns neither match nor record (73-I10; gate MEDIUM-3). |
| 73-U13 (Rev 3) | `AgentLoopNonActionGuard.SamePromptSameAnswerBelowThresholdNoNudge` (`tests/unit/agent_non_action_test.cpp`) | the **same** prompt **twice**, each answered the same `Done.`: zero nudges, two `TurnEnded`, zero `TurnFailed` — pair counts 1 then 2 < `kSamePromptRepeatOccurrences = 3`; the threshold boundary (73-D11/73-I15). Fails against a threshold of 2 (sees 1 nudge / 1 `TurnFailed`). |
| 73-U14 (Rev 3) | `AgentLoopNonActionGuard.SamePromptDifferentAnswersNeverFiresSamePromptArm` | the **same** prompt three times answered with three *different* non-blank texts: zero nudges, three `TurnEnded`, zero `TurnFailed` — exact-pair equality, no fuzzy match (73-F13); the false-positive guard for the new arm. |
| 73-U15 (Rev 3) | `AgentLoopNonActionGuard.SamePromptRepetitionMatchesAfterNormalization` | the **same** verbatim prompt three times answered `I am stuck.` / `i AM   stuck.` / `  I am stuck.  ` (equal only after trim + whitespace-collapse + ASCII case-fold), then a fourth normalized-equal post-nudge answer: one nudge, one recoverable `TurnFailed` — exercises `normalize_reply` on the same-prompt arm (73-D11), analog of 73-U11. |
| 73-U16 (Rev 3) | `AgentLoopNonActionGuard.InjectionTurnDoesNotParticipateInSamePromptRepetition` | the same prompt answered `A` twice (pair count 2), then an `Inject` turn (empty user text) answered `A`: zero nudges, three `TurnEnded` — the ineligible turn neither records nor matches, so the pair count stays 2 (73-I14), analog of 73-U12. |
| 73-U17 (Rev 3) | `AgentLoopNonActionGuard.SamePromptSecondRepeatIsPostNudgeResponse` | the same prompt three times answering `A`, but the **post-nudge** response of turn 3 is a *different* text `B`: exactly one nudge on turn 3's first response, then `TurnEnded` (no `TurnFailed`) — pins 73-I16: only the post-nudge same-pair response is the "second" non-action. Script `[A, A, A, B]`; nudge 1, `TurnEnded` 3, `TurnFailed` 0. |

**Pre-fix evidence (honest).** Before the code change: 73-U1 sees one
`AssistantMessage` and one `TurnEnded` but **zero** nudges (the guard does not
exist) — the first blank completion ends the turn; 73-U4 sees **two** user
entries for the same `id=X`. **Three** tests fail pre-fix (73-U1, 73-U2, 73-U4);
**73-U3 passes pre-fix by construction** — it asserts the *absence* of the new
behavior on non-blank text and is the false-positive guard. All four pass
post-fix.

**Rev-2 pre-fix evidence (honest).** Against the Rev-1 binary, 73-U6 fails: all
three repeated answers are non-blank, so `non_action` is false every turn — the
test observes **zero** nudges and **three** `TurnEnded` where it expects one nudge
and one `TurnFailed`. 73-U8/73-U9 **pass pre-fix by construction** (they assert
the *absence* of the repetition arm) and are the false-positive guards for the
distinct signal; they must stay green post-fix. 73-U7 was a Rev-2 absence guard
but **Rev 3 revises it into the positive same-prompt test** (`SamePromptSameAnswerFiresSamePromptArm`);
73-U13 becomes the new below-threshold absence guard. **Gate-round-1 evidence:** each new
negative/positive guard was run against the code with only its own bug
reintroduced — 73-U10 fails against occurrence-counting `repeats_prior_answer`
(sees 1 nudge / 1 `TurnFailed` where it expects 0 / 0), 73-U11 fails against a
no-op `normalize_reply` (sees 0 nudges where it expects 1), and 73-U12 fails
against an always-eligible gate (sees 1 nudge / 1 `TurnFailed` where it expects
0 / 0). All ten `AgentLoopNonActionGuard.*` tests pass with the fixes in place,
and 73-U1–73-U5 are unchanged and green both before and after Rev 2.

**Rev-3 pre-fix evidence (honest).** Against the Rev-2 binary, the revised
73-U7 fails: the same prompt + same answer three times still yields **zero**
nudges and **three** `TurnEnded` (the same-prompt arm does not exist; the 4th
scripted response is never consumed), where it expects one nudge, two `TurnEnded`,
one recoverable `TurnFailed`. 73-U13, 73-U14 and 73-U16 **pass pre-fix by
construction** — they assert the *absence* of the new arm on below-threshold,
varied-answer, and ineligible-turn inputs — and are the false-positive guards.
**73-U15 and 73-U17 fail pre-fix**: both expect the same-prompt arm to nudge
(73-U15 on a normalization-equivalent repeat; 73-U17 on a repeat whose post-nudge
answer is *different*), and Rev 2 emits no nudge for either. In particular, the
Rev-2 binary runs 73-U17's script `[A, A, A, B]` as three `TurnEnded`, consumes
only three responses, and never consumes `B`.

**Gate evidence (each test run with only its own bug reintroduced).** 73-U13 fails
against a threshold of 2: the pair fires on turn 2, so the test sees one nudge and
a post-nudge stop where it expects 0 / 0. The revised 73-U7 fails against
set-based counting (counting distinct prompts instead of raw pair occurrences:
sees 0 nudges where it expects 1). 73-U15 fails against a no-op `normalize_reply`
(the three answers stay distinct, so the pair count never reaches 3: 0 nudges).
73-U16 fails against an implementation that lets the ineligible `Inject` turn
reuse the user prompt hash / record its answer (the pair reaches 3 on the injected
turn: one nudge / one `TurnFailed`, vs expected 0 / 0). 73-U17 fails against a
mutation that escalates on *any* second no-tool response instead of a repeated
pair (one `TurnFailed` where it expects 0). All fifteen
`AgentLoopNonActionGuard.*` tests (the ten of Rev 2 plus 73-U13..U17) pass with
Rev 3 in place; 73-U1–73-U6 and 73-U8–73-U12 are unchanged and green before and
after Rev 3.

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
- **06-agent-loop.md** §5.6/§5.8 (Rev 2): broaden the non-action predicate to
  `blank OR cross-turn repetition` and add the bounded per-session observation
  ring (73-D9/D10, 73-I10..I13). **This is the only amendment Rev 2 makes.**
- **06-agent-loop.md** §5.6/§5.8 (Rev 3): broaden the non-action predicate again
  to `blank OR cross-turn distinct-prompt repetition OR same-prompt pair
  repetition` and add `kSamePromptRepeatOccurrences = 3` plus
  `repeats_same_prompt_answer` over the unchanged 73-D10 pair ring (73-D11,
  73-I14..I16). **This is the only amendment Rev 3 makes**; it reuses 73-D2/D3/D4
  and adds no bound (73-I15), so no 73-D12 is introduced.
- No event, wire, schema, config, or model change.

## 9. State lifetime (Rev 3: pair ring and per-turn repetition state)

| Property | Value |
|---|---|
| Created | With the `AgentLoop` (empty deque), mutated only on the worker thread. |
| Owner | The `AgentLoop`; one instance per `Session` (`session_owner_`). Never shared across agents. |
| Bound / eviction | `kAnswerLogCapacity = 8` entries; oldest-first `pop_front` on overflow; each entry is two fixed-size SHA-256 hex strings. |
| Append cadence | At most one observation per user turn, at the first no-tool terminal decision, only for an eligible non-blank answer (73-D10). |
| Serialization | None — never written to the durable log, registry, snapshot, or wire. |
| Process restart / resume | Dies with the loop; a resumed session builds a new `AgentLoop`, so the ring starts empty (cross-restart repetition is deliberately out of scope). |
| Reconnect | Worker-process memory only; a supervisor reconnect to a still-live daemon leaves the ring untouched. |
| Session switch | The ring is per-session; switching sessions does not read or write another session's ring. |
| Crash paths | Provider failure / cancellation return before the observation site; a process crash discards the in-memory ring with the loop. No crash path leaves persistent repetition state. |

### 9.1 Pair-ring entries — the 73-D11 keys

The 73-D11 arm adds **no new stored state**: it reads the same
`recent_answers_` entries as 73-D9, keyed by the exact pair instead of by answer.
The table below is the Rev-3 lifetime of those entries as a pair count.

| Aspect | Value (Rev 3) |
|---|---|
| Key | `(prompt_hash, answer_hash)`. 73-D9 projects it to `answer_hash → set<prompt_hash>`; 73-D11 counts exact-pair occurrences. |
| Created | One entry per **eligible** user turn via `note_answer_observation(current_prompt_hash, answer_hash)` at the turn's first no-tool decision (`agent_loop.cpp:1452-1457`); a blank first answer records nothing, a post-nudge answer never records. |
| Read | `repeats_prior_answer` (73-D9) and `repeats_same_prompt_answer` (73-D11), both `const`, both at the terminal decision (`:1458-1460`); neither mutates. |
| Destroyed / eviction | Oldest-first `pop_front` when `size() > kAnswerLogCapacity`; the whole deque dies with the `AgentLoop`. |
| Restart / resume / reconnect / session switch | Same as the ring row above — in-memory, per-`AgentLoop`, never serialized; a resumed session starts empty. |
| Threshold safety | At most one pair per turn and `kSamePromptRepeatOccurrences = 3 <= kAnswerLogCapacity = 8`, so three identical pairs always fit (73-I15); no pair-count state is added outside the bounded ring. |

### 9.2 Per-turn flags and turn-local values the arms interact with

| Symbol | Scope | Created / set | Cleared / destroyed | Persisted? | Consumed by |
|---|---|---|---|---|---|
| `recent_answers_` (pairs) | per `AgentLoop` / session | `note_answer_observation`, first no-tool decision | oldest-first at cap 8; with the loop | no | 73-D9 + 73-D11 predicates |
| `nudge_emitted_for_turn` | per `runTurn` | set `true` when the first non-action earns the nudge (`:1461-1466`) | reset at `runTurn` start | no | 73-D3/73-D4 branch (`:1461`, `:1470`) |
| `answer_observed_for_turn` | per `runTurn` | set `true` at the first no-tool decision, even blank (`:1452-1453`) | reset at `runTurn` start | no | gates the single `note_answer_observation` call |
| `current_prompt_hash` | per `runTurn` | computed once after prompt capture (`:1152-1159`) | end of turn | no | both repeat predicates; also the 73-D11 pair key |
| `answer_hash` | per terminal decision | computed on a non-blank, no-tool `Completed` (`:1445-1448`); empty for blank | end of turn | no | both repeat predicates; also the 73-D11 pair key |
| `kSamePromptRepeatOccurrences` | compile-time constant | defined in the anonymous namespace (73-D11) | n/a | no | `repeats_same_prompt_answer` only |

No flag or hash added by Rev 3 outlives its turn, and no repetition state is
written to the durable log, registry, snapshot, or wire — the Rev-3 arm is as
crash-safe as Rev 2's ring.

## 10. Change log

| Rev | Status | Changes |
|---|---|---|
| 1 | verified (Oracle PASS, 1 residual LOW) | 73-D1..D8: blank-only non-action guard (73-D2/D3/D4), replay-idempotent transcript (73-D7/D8). |
| 2 | verified (Oracle re-gate PASS, 0 HIGH / 0 MEDIUM) | 73-D9 (cross-turn distinct-prompt answer repetition) + 73-D10 (bounded pair ring, `kAnswerLogCapacity = 8`); invariants 73-I10..I13; tests 73-U6..U12. |
| 3 (this) | **draft**, pending Oracle re-gate | 73-D11 (same-prompt answer repetition, `kSamePromptRepeatOccurrences = 3`) reading the **unchanged** 73-D10 pair ring; new `repeats_same_prompt_answer`; invariants 73-I14..I16; failure modes 73-F12/73-F13; amended 73-F2/73-F3/73-F10/73-F11; dsh non-mirror row; tests 73-U13..U17 plus revised 73-U7; §3.1 before/after; §9.1/§9.2 state lifetime. No 73-D12 (capacity 8 suffices, 73-I15). |
