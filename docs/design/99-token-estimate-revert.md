# 99 - Token estimate: restore the 4 bytes/token divisor

Status: verified (Rev 4)
Revision: 4
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 4, 3 LOW)

Supersedes **75-D3 only** (the estimator divisor 4 -> 3). 75-D1 (default
compaction window), 75-D2 (reserve sizing), and 75-D4 stay in force.

## 1. Why

Spec 75 raised the token-estimate divisor from 4 to 3 bytes/token because an
incident was read as context overflow that failed to trigger compaction
(~860,882 chars, `docs/design/75-context-budget-errata.md` sec. 2). The divisor change
inflates every estimate by ~33%: it moves the context-usage meter and the
compaction trigger, and it bounds summaries and tool-schema estimates
pessimistically.

The incident's real signature was the model replying with a foreign identity
("I'm DSA LLM, an AI assistant powered by Samsung DSA") mid-session. That
reproduces on a **freshly started session at < 8% context usage** (2026-10-21),
so it is not context overflow; other harnesses report the same endpoint
behavior. The 4 -> 3 change does not address it and only distorts the budget.

## 2. Decisions

### 99-D1 - Restore divisor 4 at the four token -> byte sites

* `DefaultTokenEstimator::estimate` (`src/agent/context_assembler.cpp:32`):
  `kBytesPerToken = 4`; the formula returns to
  `text_bytes(messages) / 4 + messages.size() * 4`.
* Tool-schema estimate (`src/agent/context_snapshot.cpp:17`):
  `kSchemaBytesPerToken = 4`; returns to `bytes / 4 + 8`.
* `ContextCompactor::bound_summary` (`src/agent/compactor.cpp:274`):
  `kBytesPerToken = 4`; the token budget maps back to
  `(max_summary_tokens - kPerMessageOverhead) * 4` bytes.
* `bound_summary` in `src/agent/handoff.cpp:373` (spec 82, added after 75-D3):
  the same token -> byte cap, `kBytesPerToken = 4` as well.

These four are the complete set of token -> byte conversions whose divisor was
tightened with the pessimistic-estimate rationale; all return to 4.

### 99-D2 - What is deliberately kept

75-D1's `kDefaultContextWindowTokens = 32'768` fallback, 75-D2's reserve sizing
(`reserve_output_tokens` covers the advertised generation cap), and 75-D4's
clamp remain. **Risk statement, corrected:** divisor 4 is safe only for content
at or above `ratio * 4 = 3.2` bytes/token; the 75 incident measured ~2.7, so it
falls back inside the under-count band and OQ-75-5's residual risk is
**re-opened**, not unchanged. 75-D1/D2 keep the window and reserve honest but
cannot compensate the estimate: for the incident configuration (window 320,000,
reserve 32,768, divisor 4) the threshold is 919,140 bytes - above the ~860 KB
truncation point (`75:136-138`), so dense content can again fail to compact
before the ceiling. This is accepted deliberately: the identity-reply incident
was **not** overflow (reproduced at < 8% usage), and inflating every estimate by
33% to cover a misdiagnosed failure distorts the meter and the trigger. A
future dense-tool-output overflow should be fixed by the threshold ratio or a
real tokenizer, not by the divisor (OQ-75-5).

## 3. Interfaces

No signature change. `DefaultTokenEstimator::estimate`,
`estimate_tool_schema_tokens`, `ContextCompactor::bound_summary`, and
`handoff::bound_summary` keep their declarations; only the four constants and
their comments change.

## 4. Implementation

* `src/agent/context_assembler.cpp` - constant 3 -> 4, comment removed.
* `src/agent/compactor.cpp:273` - remove the stale "75-D3 lockstep" comment.
* `src/agent/context_snapshot.cpp` - constant 3 -> 4, comment removed.
* `src/agent/compactor.cpp` - constant 3 -> 4.
* `src/agent/handoff.cpp` - constant 3 -> 4.
* `tests/unit/context_snapshot_test.cpp:98,99,112,129` - `40u` -> `32u`,
  `bytes / 3 + 8` -> `bytes / 4 + 8`, `399u / 3u + 4u` -> `399u / 4u + 4u`,
  `schema_tokens` `40u` -> `32u`; rename
  `DefaultTokenEstimatorUsesDivisorThree` to `...DivisorFour`.
* `tests/unit/compaction_test.cpp:355-356` - the summary bound 3 -> 4 changes
  the truncation expectation: `summary.size()` `3u` -> `4u`, `"abc"` ->
  `"abcd"` (verify the fixture summary is at least 4 bytes).

## 5. Invariants

* **99-I1.** `estimate(messages) == sum(text_bytes(msg)) / 4 + 4 * count`,
  integer division, for every message list.
* **99-I2.** `estimate_tool_schema_tokens(text) == text.size() / 4 + 8`.
* **99-I3.** No token -> byte conversion in `src/agent/` uses divisor 3 after
  this change: `context_assembler`, `context_snapshot`, `compactor`, `handoff`.

## 6. Failure modes

No new failure mode. The pre-75 under-count on content denser than
`ratio * divisor = 3.2 B/token` (75's text: 2.4 with divisor 3) is accepted and
re-opens OQ-75-5; the window/reserve fixes cannot compensate (99-D2: the
919,140-byte threshold for the incident configuration is above the ~860 KB
truncation point, `75:136-138`).

## 7. State lifetime

None: constants only.

## 8. Tests

1. **T1** `DefaultTokenEstimator` on a known corpus returns
   `bytes / 4 + 4 * n` (`context_snapshot_test.cpp:112`).
2. **T2** `estimate_tool_schema_tokens` returns `bytes / 4 + 8` and the
   snapshot's `schema_tokens` is `32` for the 97-byte fixture
   (`context_snapshot_test.cpp:98,99,129`).
3. **T3** `ContextCompactor::bound_summary` truncates at
   `(max_summary_tokens - 4) * 4` bytes (`compaction_test.cpp:355-356`, updated).
4. **T4** `handoff::bound_summary` uses the same cap (`(max-4)*4`); no
   existing handoff test asserts the byte cap, so add one at the boundary.
5. **T5** no-regression: the compaction trigger still fires at
   `threshold_ratio * (window - reserve)`; only the estimate scale changed.

## 9. dsh mapping

Not a dsh-mirrored capability; internal accounting only.

## 10. Amendments

* **75-D3** - superseded by 99-D1 (divisor back to 4). 75-D1/D2/D4 unchanged.
* **18 sec. 3.3** and **13 sec. 5.6** - the original `floor(bytes/4) + 4*count`
  formula is authoritative again; 75-D3's restatement to `/3` is withdrawn.
