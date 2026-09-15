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
| 11 | `11-m2-errata.md` | yes | **verified** | Oracle G0 interface-freeze gate: PASS (21 findings fixed; no open HIGH/MEDIUM) | 1412 lines; M2 interfaces frozen; amends 03/04/05/09/10 |

## Open top-level items (`HANDOFF.md` §5)

| # | Item | Status | Resolution |
|---|---|---|---|
| 1 | Naming: `txtcoder` vs legacy `ymh` | **resolved** | `ymh` is final (binary, state, workspace dir, source tree, namespace); `txtcoder` retired |
| 2 | Registry bootstrap discovery (§9.10) | **resolved** | registry + `flock` primary; exact-match process scan is best-effort fallback only |
| 3 | `SessionHeader` final field list | **resolved** | §9.2/§9.10: no boot nonce in header; `ordinal`/`archived` stay in registry junction |
| 4 | Remote/SSH TCP transport | deferred | scheduling decision, not a blocker |
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

