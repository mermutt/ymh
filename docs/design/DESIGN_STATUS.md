# Design Status

Tracker for the design-first gate (see `HANDOFF.md` §6–§7). A spec may only be
coded after it is marked **verified** here.

| # | Spec | Written | Verified | Reviewer | Notes |
|---|---|---|---|---|---|
| 00 | `00-architecture.md` | yes | **verified** | Oracle pass 2: GATE PASS (16 findings fixed; no open HIGH/MEDIUM) | §5 items 1–3 closed; discovery + live-testing folded in; §8.1 + §9.2 errata |
| 01 | `01-session.md` | yes | **verified** | Oracle component gate: GATE PASS (7 findings fixed; no open HIGH/MEDIUM) | 1544 lines; F1–F12 + S1–S14; `turn/fail` errata (additive); decisions in §16 |
| 02 | `02-persistence.md` | yes | **verified** | Oracle component gate: GATE PASS (7 findings fixed; no open HIGH/MEDIUM) | 1529 lines; P1–P18 + P-F1–P-F16; read-only open mode; same-DB snapshot errata |
| 03 | `03-workspace-registry.md` | yes | **verified** | Oracle component gate: GATE PASS (5 findings fixed; no open HIGH/MEDIUM) | 1340 lines; R1–R16 + R-F1–R-F18; DDL byte-identical to §9.10 |
| 04 | `04-workspace-host-daemon.md` | yes | **verified** | Oracle component gate: GATE PASS (6 findings fixed; no open HIGH/MEDIUM) | 1473 lines; H1–H21 + D-F1–D-F22; focus-independent activation |
| 05 | `05-transport.md` | yes | **verified** | Oracle component gate: GATE PASS (13 findings fixed; no open HIGH/MEDIUM) | 1704 lines; T1–T24 + T-F1–T-F22; cursor resume, `id:null`, profiles |
| 06 | `06-agent-loop.md` | yes | **verified** | Oracle component gate: GATE PASS (16 findings fixed; no open HIGH/MEDIUM) | 1424 lines; A1–A18 + A-F1–A-F18; `LLMPool` pinned; AgentRegistry owns lifetime |
| 07 | `07-tools-execution.md` | yes | **verified** | Oracle component gate: GATE PASS (16 findings fixed; no open HIGH/MEDIUM) | 1743 lines; X1–X16 + E-F1–E-F18; symlink-safe `resolve()`; `ToolConfig` |
| 08 | `08-llm-provider.md` | yes | **verified** | Oracle component gate: GATE PASS (8 findings fixed; no open HIGH/MEDIUM) | 1390 lines; L1–L17 + L-F1–L-F18; retry barrier, `turn/fail`; §4.5 errata delegates `TurnFailed.code` to 06 |
| 09 | `09-permissions.md` | yes | **verified** | Oracle component gate: GATE PASS (12 findings fixed; no open HIGH/MEDIUM) | 1360 lines; Q1–Q15 + Q-F1–Q-F16; `PermissionConfig`; F2 surface-via-attention |
| 10 | `10-supervisor-tui.md` | yes | **verified** | Oracle component gate: GATE PASS (8 findings fixed; no open HIGH/MEDIUM) | 1626 lines; U1–U18 + U-F1–U-F20; focus-independent flash |
| 11 | `11-m2-errata.md` | yes | **verified** | Oracle G0 interface-freeze gate: PASS (21 findings fixed; no open HIGH/MEDIUM) | 1426 lines; M2 interfaces frozen; amends 03/04/05/09/10; post-gate additive §7.2 `ClockReader` amendment (track E) + E21 |
| 12 | `12-m1-drift-errata.md` | yes | — (register; not a component spec) | track F (self), pending review | 204 lines; M1 spec-text ↔ code reconciliation register DR1–DR6; defers to 11 (no new semantics) |
| 13 | `13-context-compaction.md` | yes | **verified** | Oracle component gate: GATE PASS (8 review rounds; no open HIGH/MEDIUM) | 1567+ lines; context compaction (projection, never deletion); `ContextCompaction` event |
| 14 | `14-pty-capability.md` | yes | **verified** | Oracle component gate: GATE PASS (4 review rounds; no open HIGH/MEDIUM) | 1865+ lines; `PtySession`/`PtyService` behind the execution-environment seam |
| 15 | `15-mcp-adapter.md` | yes | **verified** | Oracle component gate: GATE PASS (3 review rounds; no open HIGH/MEDIUM) | 2051 lines; MCP client adapter into the shared tool registry; namespaced `mcp.<server>.<tool>` |
| 16 | `16-daemon-ownership.md` | yes | **verified + implemented** | Dual gate, 5 rounds: adversarial critic + Oracle, **both PASS** (0 open HIGH/MEDIUM) | 2737 lines; supervisor-owned daemons; supersedes 04 H8/H9 §3.2/§6.5/§14.1(f), 00 §54 D23/§9.8/§9.9, 10 §2.1, 11 §8.2 D20.3; amends 03/04/05 (A15/A16); invariants O1–O22, failure modes O-F1–O-F16. **Implemented in 8 waves** (`036e4e4e1`…`065e221d9`); verified live with two supervisors against real DeepSeek |
| — | `REQUIREMENTS_BACKLOG.md` | yes | — (register; not a component spec) | track: triage, self-verified | 468 lines; RB-01–RB-15 from `requirements_draft.txt`, dedup'd against shipped code; RB-13 (switcher/sessions) done per spec 22, RB-14/RB-15 recorded in spec 22 §11 |
| — | `UI_SURFACE_INVENTORY.md` | yes | — (register; not a component spec) | track: triage, self-verified | 163 lines; spec-16 ↔ RB-10/RB-11 UI seam (C1–C5), Surface 3 updated for spec 22 (live-only switcher + `/sessions` History source) |
| 17 | `17-ui-transcript-errata.md` | yes | **verified** | Oracle gate: 3 rounds → **GATE PASS** (0 open HIGH/MEDIUM/LOW; 18 findings fixed) | 577 lines; additive errata to `10-supervisor-tui.md` for RB-01/02/08/10/11; invariants `U-RB01-1..3`, `U-RB02-1..4`, `U-RB08-1..4`, `U-RB10-*`, `U-RB11-*` |
| 18 | `18-context-errata.md` | yes | **verified + implemented** | Oracle gate: 7 rounds → **PASS** (0 open HIGH/MEDIUM; 7 LOW) | additive errata to `10` (UI) + `13` (compaction) for RB-06 `/context`; color grid + MCP/tools inventory. Gate forced: `context_generation_` made atomic (was a UI/pump-thread data race), a generation guard so a late reply cannot re-open a dismissed overlay, and `last_error` never shipped (only a boolean `has_error`). Rounds 4–6 were all one defect class — the fixed chrome budget — resolved by one global decision (CX-D12). **Implemented in `edd696c6e`** |
| 19 | `19-session-rename-errata.md` | yes | **verified + implemented** | Oracle gate: 4 rounds → **PASS** (0 open HIGH/MEDIUM; 1 LOW) | 1566 lines; additive errata to `01` (session/event log) + `05` (transport) for RB-03 (auto-name + rename); fixed an io-thread/worker-thread race in the auto-name path and made manual-rename-wins structural (two independent guards). **Implemented in `162fa8641`**; verified live against real DeepSeek |
| 20 | `20-skills.md` | yes | **verified + implemented** | Oracle gate: 3 rounds → **PASS** (0 open HIGH/MEDIUM; 10 LOW) | 2273 lines; NEW skills subsystem for RB-05 (discovery, `/skills`, invocation, context injection, trust tiers); gate caught a construction-order bug (the skill index never reached the model) and a trust-boundary hole (a relative user root acquiring user trust). **Implemented in `6761e62ef`**; verified live |
| 21 | `21-config-jsonc-errata.md` | yes | **verified + implemented** | Oracle gate: 5 rounds → **PASS** (0 open HIGH/MEDIUM; 7 LOW); Rev 5–8 amendment re-gate → **PASS** (0 open HIGH/MEDIUM) | additive errata to `08` config spec for RB-09; **JSONC only, TOML retired** (user decision 2026-09-16); parser claim verified against nlohmann/json 3.12.0 (comments yes, trailing commas no). Rev 5–8 adds **21-D11**–**21-D17**: no TOML awareness, global config required, explicit `--config` never scaffolded, the top-level unknown-key message, supervisor→daemon `--config` handoff, a present path must be a regular file, and the config-load gate (`Tui`/`Run`/`List`/`Show`/`Replay`/`Fork` only); **`toml++` retired** (not fetched). **Implemented** (`9db17cd54`; Rev 5–8 applied in the working tree) |
| 22 | `22-switcher-sessions-errata.md` | yes | **verified + implemented** | Oracle gate: 5 rounds → **PASS** (0 open HIGH/MEDIUM) | Rev 5; 1865 lines; additive errata to `10`/`16`/`03` for S1–S4: live-only Ctrl-S switcher + daemon-death eviction (supersedes "ALL workspaces" in `00` §20.24 / `10` §7.1 and the `NotRunning` browsability in `16` §3.2.1/§7.7), a new `/sessions` disk catalog (extend, not duplicate; O14/C4), spawn/attach-then-resume on selection, and `ymh --resume <id>` in TUI mode. Invariants SW1–SW26, failure modes SW-F1–SW-F19. **Implemented** |

## Open top-level items (`HANDOFF.md` §5)

| # | Item | Status | Resolution |
|---|---|---|---|
| 1 | Naming: `txtcoder` vs legacy `ymh` | **resolved** | `ymh` is final (binary, state, workspace dir, source tree, namespace); `txtcoder` retired |
| 2 | Registry bootstrap discovery (§9.10) | **resolved** | registry + `flock` primary; exact-match process scan is best-effort fallback only |
| 3 | `SessionHeader` final field list | **resolved** | §9.2/§9.10: no boot nonce in header; `ordinal`/`archived` stay in registry junction |
| 4 | Remote/SSH TCP transport | **out of scope** | DECISION (user, 2026-09-17): not deferred, not planned, will not be built; JSON-RPC stays transport-agnostic |
| 5 | Deferred dsh items | accepted | no offline cache; host events merged into mux; no hot reload |

## Operating requirements (2026-09-14)

- **Autonomous development.** Design and coding proceed with minimal user
  intervention. Only non-obvious design or user-interface questions are routed
  to the user.
- **Live end-to-end testing.** A real LLM API is used to drive the TUI in a PTY
  as a human would (start session, type prompts, observe rendered output).
- **Coverage target.** Deterministic Fake-LLM layer for unit/integration/golden/
  replay; live layer is opt-in (requires an API key). Top-level
  (supervisor/manager) components are targeted for 100% automated coverage.

## Review history

| Date | Artifact | Reviewer | Result |
|---|---|---|---|
| 2026-09-14 | `00-architecture.md` | Oracle pass 1 | 18 fixes applied (from handoff) |
| 2026-09-14 | `00-architecture.md` | Oracle pass 2 | GATE FAIL — 9 MEDIUM, 7 LOW |
| 2026-09-14 | `00-architecture.md` | Oracle pass 3 | **GATE PASS** — all 16 resolved; 5 new LOW cleaned up |
| 2026-09-14 | `01-session.md` | Oracle component pass 1 | GATE FAIL — 4 MEDIUM, 3 LOW, S11–S14 undefined |
| 2026-09-14 | `01-session.md` | Oracle component pass 2 | **GATE PASS** — 7 resolved; 2 new LOW cleaned up |
| 2026-09-14 | `02-persistence.md` | Oracle component pass 1 | GATE FAIL — 4 MEDIUM, 3 LOW |
| 2026-09-14 | `02-persistence.md` | Oracle component pass 2 | **GATE PASS** — 7 resolved; 2 LOW cleaned up |
| 2026-09-14 | `03-workspace-registry.md` | Oracle component pass 1 | GATE FAIL — 2 MEDIUM, 3 LOW |
| 2026-09-14 | `03-workspace-registry.md` | Oracle component pass 2 | **GATE PASS** — 5 resolved; 1 LOW cleaned up |
| 2026-09-14 | `04-workspace-host-daemon.md` | Oracle component pass 1 | GATE FAIL — 2 MEDIUM, 4 LOW |
| 2026-09-14 | `04-workspace-host-daemon.md` | Oracle component pass 2 | **GATE PASS** — 6 resolved; 1 LOW cleaned up; focus-independence confirmed |
| 2026-09-14 | `05-transport.md` | Oracle component pass 1 | GATE FAIL — 1 HIGH, 5 MEDIUM, 7 LOW |
| 2026-09-14 | `05-transport.md` | Oracle component pass 2 | **GATE PASS** — 13 resolved; 1 LOW cleaned up |
| 2026-09-14 | `08-llm-provider.md` | Oracle component pass 1 | GATE FAIL — 2 MEDIUM, 6 LOW |
| 2026-09-14 | `08-llm-provider.md` | Oracle component pass 2 | **GATE PASS** — 8 resolved |
| 2026-09-14 | `01-session.md` | Oracle errata check | **GATE PASS** — `turn/fail` additive errata; no regressions |
| 2026-09-14 | `06-agent-loop.md` | Oracle component pass 1 | GATE FAIL — 2 HIGH, 7 MEDIUM, 7 LOW |
| 2026-09-14 | `06-agent-loop.md` | Oracle component pass 2 | **GATE PASS** — 16 resolved; cross-doc `TurnFailed.code` fixed via 08 §4.5 errata |
| 2026-09-14 | `07-tools-execution.md` | Oracle component pass 1 | GATE FAIL — 2 HIGH, 10 MEDIUM, 4 LOW |
| 2026-09-14 | `07-tools-execution.md` | Oracle component pass 2 | **GATE PASS** — 16 resolved; dangling OQ labels cleaned up |
| 2026-09-14 | `09-permissions.md` | Oracle component pass 1 | GATE FAIL — 4 MEDIUM, 8 LOW |
| 2026-09-14 | `09-permissions.md` | Oracle component pass 2 | **GATE PASS** — 12 resolved; 3 LOW cleaned up |
| 2026-09-14 | `10-supervisor-tui.md` | Oracle component pass 1 | GATE FAIL — 1 MEDIUM, 7 LOW |
| 2026-09-14 | `10-supervisor-tui.md` | Oracle component pass 2 | **GATE PASS** — 8 resolved; focus-independent flash confirmed |
| 2026-09-15 | `11-m2-errata.md` | Oracle G0 pass 1 | GATE FAIL — 6 HIGH, 8 MEDIUM, 7 LOW |
| 2026-09-15 | `11-m2-errata.md` | Oracle G0 pass 2 | GATE PASS — 21 resolved + R1–R3; interfaces frozen |
| 2026-09-15 | `11-m2-errata.md` §7.2 | track E finding (post-gate, additive) | `Clock&` inert → injectable `ClockReader` (default `Clock::now`); D19.1 + E21 + §16 updated; no other interface semantics changed |
| 2026-09-15 | `12-m1-drift-errata.md` | track F (self) | written — M1 spec/code reconciliation register DR1–DR6; no new semantics; defers to 11; pending review |
| 2026-09-16 | `16-daemon-ownership.md` | round 1 (rev 0) | GATE FAIL — critic 4 HIGH/10 MEDIUM/5 LOW; Oracle 2 HIGH/11 MEDIUM/8 LOW (incl. safety-property breach: `FRESH_OWNER` clock mismatch) |
| 2026-09-16 | `16-daemon-ownership.md` | round 2 (rev 1a) | GATE FAIL — closure 40/41 + 21/21; **NEW** 1 HIGH (cached-count cannot exclude the requester), 5 MEDIUM, 13 LOW |
| 2026-09-16 | `16-daemon-ownership.md` | round 3 (rev 2) | GATE FAIL — closure 7/7 + 14/15; **NEW** 1 HIGH (fail-open `require_owner` default), 2 MEDIUM, 7 LOW |
| 2026-09-16 | `16-daemon-ownership.md` | round 4 (rev 3) | GATE FAIL — closure 8/9; **NEW** 1 HIGH (guard omitted `watchdog_disabled`), 2 MEDIUM, 6 LOW; adjudicated Oracle `G3` over the critic (seam retyping was overstated) |
| 2026-09-16 | `16-daemon-ownership.md` | round 5 (rev 4) | **GATE PASS** — critic 0 HIGH/0 MEDIUM (closure 3/3); Oracle 0 HIGH/0 MEDIUM (closure 9/9); 5 LOW remained |
| 2026-09-16 | `16-daemon-ownership.md` | final polish (rev 5) + targeted Oracle confirmation | **PASS** — 5/5 LOW fixed, 0 regressions; O15 bounds unchanged (23 s/38 s); escalation budget 12 s → 34 s confirmed exit-bound, not request-bound |
| 2026-09-16 | `17-ui-transcript-errata.md` | round 1 | GATE FAIL — 1 HIGH (cross-thread `UiModel` mutation for the session title), 7 MEDIUM, 6 LOW |
| 2026-09-16 | `17-ui-transcript-errata.md` | round 2 | GATE FAIL — closure 13/14; F-02 residual (text-then-reasoning ordering unpinned while claiming "always `[…, Reasoning, Assistant, …]`"), 3 LOW |
| 2026-09-16 | `17-ui-transcript-errata.md` | round 3 | **GATE PASS** — 0 HIGH/0 MEDIUM/0 LOW; closure 4/4; no regressions. Implemented in `71dda4f16` |
| 2026-09-16 | `16-daemon-ownership.md` | implementation (8 waves) | `036e4e4e1` (foundation) → `3a38334a0` (registry+migration) → `9db080868` (watchdog+guard+typed seam) → `15b757f75` (admission+ownership view+broadcast) → `eb3774cb3` (identity+presence+CLI) → `cbaca8437` (exit prompt+session visibility+marks) → `5bea22e3d` (workspace stop) → `0753fb583` (test rewrites+two-supervisor+crash injection) → `71850ae0d` (two-spawn handoff fix) → `065e221d9` (last-exit stale-snapshot fix). 792 tests, 0 orphans; verified live with two supervisors against real DeepSeek |
| 2026-09-16 | `16-daemon-ownership.md` | post-implementation bugs found by live verification | (1) the two-spawn race burned the full readiness budget retrying the loser's boot nonce → abort under load (`71850ae0d`); (2) exiting the LAST supervisor could silently exit without prompting and **leak the daemon**, because `is_orphaning_view` trusted the daemon's 2 s-cached `other_fresh_owners` snapshot and saw a just-exited peer as a ghost owner (`065e221d9`). Both hermetic-suite gaps are now covered by tests |

