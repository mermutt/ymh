# 30 — Architecture Cascade Errata: the dsh-Alignment Top-Level Gate (Wave-0 A0)

```
Status: written · verified: — · reviewer: —
Revision: Rev 2 — closes the gate30 HIGH-1 staging defect. Stage A now records
          the `06` agent-loop errata AND the `13` context-compaction errata as
          **Wave-1 blocking prerequisites** (both are pulled forward from
          `26 §5`'s Stage-B Wave 4, because Wave 1 changes
          `AgentLoop::buildRequest` and re-seams `ContextCompactor`), and records
          the **retry executor** as a named ownership gap (no item in `26 §5`
          Waves 1–6). AC-I4 and the §10 test plan are reconciled; Rev 1 is
          retained below. Rev 1 — initial write. Pins the architecture-level seam
          changes that spec 26 (verified Rev 7) forces on `00-architecture.md`,
          the §4.4 cascade classification, the Wave-0 Stage A/B restaging, and
          the migration-program prerequisites. Amends `00` **by reference only**:
          no line of `00` is edited here. Claims spec number 30 and supersedes
          spec 26 §5's stale reservation of numbers 27–30 for the Wave-3+
          specs.
Component: 30 (errata) — amends 00-architecture.md by reference
Depends on: 00-architecture.md (verified), 26-dsh-alignment.md (verified,
            Rev 7), 26-dsh-alignment-part2.md (verified, Rev 7)
Scope: (1) The four architecture-level seam changes and their `26 §4.4`
       classification (Additive vs Brk., affected specs). (2) The
       cascade-coverage statement: which component specs the top-level-gate
       change invalidates and which it asserts unaffected, with the panel's
       `18`/`25` judgment-call caveat. (3) The Wave-0 Stage A/B restaging and
       the "Wave 1 is not blocked on the prompt-registry spec" rule. (4) The
       migration-program prerequisites (user scope decision, prompt-logging
       policy, no-downgrade acceptance) as a top-level statement. (5) The
       numbering supersession.
```

## 1. Purpose, authority, and the two-gate rule

`AGENTS.md` states the non-negotiable rule in two gates: (1) the **top-level
gate** — `00-architecture.md` complete and independently verified, no open
HIGH/MEDIUM; (2) the **component gate** — each component's spec verified before
any code for that component.

Spec 26 (`26-dsh-alignment.md` + `26-dsh-alignment-part2.md`, verified Rev 7) is
a **mechanics-alignment program**, not a component tweak. It changes seams that
live *above* the components: the provider call seam, the session event
vocabulary, the session header, and the collaboration surfaces. Its own §4.4
therefore classifies `00` as **Brk. (errata required)** and §5 Stage A makes `00`
the first up-front Wave-0 item:

> **A0. `00-architecture.md`** — confirm the amended provider/prompt/event seams
> and the new session events do not violate a top-level invariant, and update the
> seam inventory (the register and §4.4 both gate `00`). — `26-dsh-alignment-part2.md:1366-1368`

This errata **is** A0. It discharges that gate by pinning, in one place:

1. the architecture-level seam changes and the §4.4 classification for each;
2. the cascade — which component specs are invalidated and must be re-checked,
   and which are asserted unaffected;
3. the Wave-0 Stage A/B restaging (what blocks Wave 1 and what does not);
4. the migration-program prerequisites as a top-level statement.

**Authority.** The source of truth is the verified spec 26 (Rev 7). This errata
introduces **no new decision** and **no new classification**: every class in §3
and §4 is quoted from `26-dsh-alignment-part2.md` §4.2 (register) and §4.4. If
this errata and spec 26 disagree, spec 26 wins.

**Amendment style (amends by reference).** Following `21`/`23`/`24`, this errata
does **not** edit `00-architecture.md` in place. The affected `00` sections keep
their text and are read as amended by §3/§7 below. The `00` seam inventory is
updated by this errata being added to the design set, not by rewriting `00`.

## 2. Numbering claim and supersession

Spec 26 §5 (`:1388-1391`) reserved the numbers `27`–`30` for the **new** Wave-3+
specs it proposes:

```text
Wave 3 → 27-system-prompt.md (new)
Wave 4 → 28-output-retention.md (new)
Wave 5 → 29-agent-presets.md (new)
Wave 6 → 30-goals-jobs-commands.md (new)
```

That reservation is **stale**. Spec `27` was already taken by
`27-session-locking-errata.md` (verified), and the Wave-0 freezes claim the next
free numbers. The actual allocation is:

| Number | File | Role |
|---|---|---|
| 27 | `27-session-locking-errata.md` | session-locking errata (verified) |
| 28 | `28-llm-service-boundary-errata.md` | Wave-0 A2 — the LLM service boundary |
| 29 | `29-event-family-errata.md` | Wave-0 A1 — the event-family contract |
| 30 | `30-architecture-cascade-errata.md` | Wave-0 A0 — this file |

**Consequence (pinned).** The four Wave-3+ specs that spec 26 §5 named as
`27`/`28`/`29`/`30` must be **renumbered to 36+** when their errata are written (31–35 are now taken):
the prompt-registry spec (reserved `27-system-prompt.md`), the output-retention
spec (reserved `28-output-retention.md`), the presets spec (reserved
`29-agent-presets.md`), and the goals/jobs/commands spec (reserved
`30-goals-jobs-commands.md`). Anywhere spec 26 §5 or §4.4 refers to those
reserved numbers, read it as the renumbered spec. This errata claims `30` and
supersedes the reservation in `26-dsh-alignment-part2.md:1388-1391`.

## 3. The architecture-level seam changes (S1–S4)

Each seam names its decision, its `26 §4.4` classification, the affected specs,
the `00` anchors whose text is read as amended, and the shipped-code anchors
that make the change concrete. `Add.` = additive to the named verified spec;
`Brk.` = breaking (errata/amendment required before code).

### S1 — The provider seam: `LLMProvider` + `ProviderRegistry` → `LlmRuntime`

**What changes.** A provider-neutral **`LlmRuntime`** service is inserted *above*
`LLMProvider`. The runtime owns route→adapter registration, model resolution,
and the stream boundary (`LlmRuntime::register_adapter` / `list_providers` /
`prepare_call` / `stream`); `ProviderRegistry`'s registration role is absorbed
into the runtime. `LLMProvider` and its `stream()` contract are **retained** —
the runtime wraps adapters, it does not replace them. The agent loop no longer
holds a raw provider: `AgentServices::provider` becomes an `LlmRuntime&`.

**Classification.** **Brk. (06, 08)** — register `26-D1` (`:139`); §4.4 rows:
`00` Brk. (`:1091`), `06` Brk. (D1) (`:1092`), `08` Brk. (D1/D2/D3) (`:1093`).
The register states it is a *replacement* of the pinned `AgentServices`/provider
seam, "not an addition beside it".

**`00` anchors read as amended.** §5 High-Level Components (`ProviderRegistry`
node, `:402`); §12 LLM Interface (`class LLMProvider`, `:1554`); §13 Provider
Adapters (`:1582`); §54 D5 "LLM is a replaceable provider" (`:4756`); §55 seam
map `LLM seam → LLMProvider` (`:4878`). Read each as *`LLMProvider` is the
adapter contract; `LlmRuntime` is the seam the agent loop depends on*.

**Shipped-code anchors.** `include/ymh/agent/agent_loop.hpp:64`
(`LLMProvider* provider = nullptr;`), `include/ymh/llm/llm_provider.hpp:61`
(`class LLMProvider`), `include/ymh/llm/provider_registry.hpp:43`
(`class ProviderRegistry`). The loop's raw pointer is exactly what `26-D1`
replaces.

### S2 — The new event family: `EventType` extension

**What changes.** The durable/live event vocabulary grows: the logged
`llm/request_header` (D2), the embedded compact assistant stream plus
`assistant/attempt` with legacy `AssistantChunk` made read-only on disk but still
published live-only (D9), compaction trigger/prune events (D12/D13), message
provenance (D14), `agent_preset/selected` (D16), and the goal/job/command events
(D18/D19/D20). The `EventType` enum and `all_event_types()` extend; the on-disk
decode keeps its loud forward-fence, while the **wire** receiver skips an unknown
event `type` and still advances the cursor (D24 / `26-I12`). `kProtocolVersion`
**stays `1`**: a new event type is not an envelope-shape change.

**Classification.** The **event-vocabulary axis is Additive** (new durable types
are additive; D24 is `Add.` on `01`); the **payload/codec axis is breaking**
where a known event's payload changes. Register rows: D2 `Add.` (`:140`, owning
`01`/`08`/`21`), D9 `Brk.` (`:147`, `01`/`08`), D13 `Brk.` (`:151`,
`13`/`01`/`08`), D14 `Brk.` (`:152`, `01`/`17`/the prompt spec), D16 `Brk.`
(`:154`, header — see S3), D18 `New` (`:156`), D19 `New` (`:157`), D20 `Add.`
(`:158`), D24 `Add.` (`:162`, `01`). §4.4 rows: `08` D9 (`:1093`), `13` Brk. D13
(`:1097`), `17` Additive (`:1080`), `05` Additive (`:1094`), `11` "Additive for
D2; breaking for D9/D13/D16" (`:1079`), `23` Brk. (`:1084`).

**`00` anchors read as amended.** §8.1 Event categories — the durable set is
already declared non-exhaustive (`:565`, errata note `:592`); §8.1's live set
gains the live-only `AssistantChunk` publication (D9). §8.2 `EventType` remains
the pinned extension point.

**Shipped-code anchors.** `include/ymh/core/event.hpp:51` (`enum class EventType`).

### S3 — The session-header change: preset id and goal projection

**What changes.** The frozen `SessionHeader` field list grows: a **preset id**
(D16, per-session composition) and a **goal projection** (D18, objective/phases
across resume/fork). The lifecycle rules (lease, resume, fork) are preserved; the
*header field list* changes, so every reader of the frozen header is affected.

**Classification.** **Brk. (errata required)** — §4.4 row `23` (`:1084`): "The
session header gains a preset id and a goal projection, so the frozen header
field list changes." Affected specs: the session-lifecycle spec `23` (gated in
Wave 0 Stage B), `01`, and the preset/goal specs.

**`00` anchors read as amended.** §9.2 SQLite schema, the `sessions` table
(`:760`): the header **fields** grow, but **no structural DDL change** occurs —
per `26 §4.6`, the D16 `preset`/`goal` fields ride the existing
`sessions.metadata JSON` column rather than new columns, so `kSchemaVersion`
stays `1` and `02` persistence is unaffected (see §4.2/§7).

**Shipped-code anchors.** `include/ymh/session/session.hpp:44`
(`struct SessionHeader`; current fields end at `metadata`).

### S4 — Presets, goals, jobs, and the log-only command surface

**What changes.** New collaboration surfaces sit on top of the agent model:
**presets** (a standing mount + per-session joined scopes, blank-session-only
switching, `agent_preset/selected`) (D16), **goals** (durable objective, phases,
round driver, `/goal`) (D18), **background jobs** (owner-scoped registry,
`job_output`/`job_list`/`job_kill`, wakeup policy) (D19), and the **log-only
command surface** (`command/run`/`command/done`, agent-scoped shadowing) (D20).

**Classification.** D16 presets: **Brk. (session header)** — owning the new
presets spec, `01`, `23` (register `:154`; §4.4 `:1084`). D18 goals: **New** —
owning the new goals spec, `01` (`:156`). D19 jobs: **New** (`:157`). D20
commands: **Add.** (`:158`). For the supervisor TUI spec `10`, D16/D18/D19/D20
are **Additive** —
"New UI affordances; no existing surface contract changes" (`:1078`), and the
live-only switcher rule (`22`) and transcript rendering must not change.

**`00` anchors read as amended.** `00` has no presets/goals/jobs/commands
section: these are **new top-level surfaces** on the §10 Agent Model / §11 Agent
Loop boundary, composed per session (D16) and inheriting the existing
subagent/agent invariants. They add no new process or transport.

## 4. Cascade coverage

Under the two-gate rule, a top-level-gate change invalidates the *assumption* that
dependent component specs still hold. This section records which component specs
the architecture-level seams above touch. It is the `26 §4.4` architecture-seam
table (`:1086-1098`), reproduced, followed by the unaffected list (`:1100-1107`).
**No spec is claimed "all of 01–24"** — only the specs named below.

### 4.1 Specs invalidated / needing re-check (architecture seams)

| Spec | Decision(s) affecting it | Class | Why |
|---|---|---|---|
| **00** architecture | D1 (provider seam), D9/D2 (event family), D16/D18 (presets/goals), D24 (event-vocabulary decoupling) | **Brk. (errata required)** | The top-level gate changes: `LLMProvider`+registry → `LlmRuntime`, the `EventType` family grows, the session header changes. The wire envelope version is **unchanged** (D24). |
| **06** agent-loop | D1 (loop holds `LlmRuntime&`, not `LLMProvider*`), D10 (scheduler), D17 (children), D21 (reminders) | **Brk. (D1); additive (D10/D17/D21)** | D1 replaces the pinned `AgentServices::provider` seam (`agent_loop.hpp:64`) — the register classifies it `Brk. (06,08)`. D10/D17/D21 are additive behind it. |
| **08** llm-provider | D1 (service boundary), D2/D3 (freeze/digest/header), D8 (assembler), D9 (replay state), D13 (summarizer routing), D23 (prompt-text policy) | **Brk. (D1); additive (D2/D3 per the register); Brk. (D9)** | D1 inserts a service above the provider and moves retry/serialization out of the adapter; the `LLMProvider` contract itself is retained. **Reconciliation note (a source inconsistency recorded, not silently copied):** the class above follows the REGISTER, not §4.4's row. §4.4 (`26p2:1093`) says `Brk. (D1/D2/D3); additive (D8/D9)`, but the register classifies `26-D2`/`26-D3` as `Add.` (`26p2:140`/`:141`) and `26-D9` as `Brk.` (`26p2:147`). §4.4's own preamble states that where the register and the spec text disagree **the register wins** (`26p2:1072-1074`), so `08`'s D2/D3 are additive and D9 is breaking; the §4.4 row is a known spec-26 defect. Impact is bounded — `08`'s errata (28) is Stage-A regardless because of D1. |
| **05** transport | D24 (receiver tolerance for unknown event types), D2/D9 (new event types over the socket) | **Additive** | The envelope shape (`SessionEnvelope` = `{session,event}`), the handshake, and `kProtocolVersion = 1` are unchanged; the only new rule is that the client skips an unknown event `type` (cursor still advances) instead of failing the connection. |
| **04** host-daemon / **16** daemon-ownership | D24 (no version change) | **Unaffected** | `kProtocolVersion` stays `1` and the handshake is unchanged, so the version constant and the handshake outcome do not change. Ownership/lifetime rules are untouched. |
| **07** tools-execution | D11 (retention), D15 (presentation) | **Brk. (D11 `ToolResult`/clamp); additive (D15)** | Retention changes the pinned `ToolResult`/`clamp_tool_result` contract. |
| **13** compaction | D12 (pruner), D13 (payload fields) | **Brk. (D13)** | D13 adds `ContextCompaction` fields against spec 13's "does not add fields to the payload". |
| **15** mcp-adapter | D15, `26-I6` (a tool-set change starts a new request series) | **Additive** | MCP keeps its namespaced tool contract; the change is that a server connect/disconnect is a logged series boundary. |

The `10`/`11`/`17`/`20`/`21`/`22`/`23`/`24` specs are classified in `26 §4.4`'s
other table (`:1076-1085`): `10`/`17`/`20`/`21`/`22`/`24` Additive, `11` Additive
for D2 but breaking for D9/D13/D16, `23` Brk. (header).

### 4.2 Specs asserted unaffected in their pinned interfaces

`26 §4.4:1100-1107` asserts the following are unaffected, and this errata adopts
that assertion:

- **`02` persistence** — no schema/DDL change (§4.6).
- **`03` registry** — untouched.
- **`09` permissions** — no semantics change; `ReadOnly` status unchanged.
- **`14` PTY** — behind the execution seam, untouched.
- **`18` context-errata** and **`25` ui-ux-errata** — additive config keys only
  (`§4.9`).
- **`19` session-rename** — untouched.

**Panel caveat (recorded).** `18` and `25` are the **judgment calls** in this
list: their only contact with the program is the additive config-key wiring of
`§4.9`, so "unaffected" holds for their pinned interfaces but is the weakest
assertion here. When a Stage-B wave actually touches config, the owning spec's
errata re-confirms `18`/`25` rather than relying on this list.

**Completeness claim (pinned).** Any spec not named in §4.1 or §4.2 is out of
scope for this cascade. Per `26 §4.4:1106-1107`, **this errata is the gate that
confirms the list is complete**: if the cascade is later found to omit a spec,
that is an open finding against this errata and the top-level gate does not pass.

## 5. Wave-0 Stage A/B restaging

`26 §5:1358-1395` stages Wave 0 into two stages. This errata restates the
restaging with the actual spec numbers (§2) and pins the blocking rule.

### 5.1 Stage A — up-front freeze (blocks Wave 1)

Three items, in order, each requiring Oracle PASS and zero open HIGH/MEDIUM:

| Stage-A item | Deliverable (actual number) | Content |
|---|---|---|
| **A0** | **`30-architecture-cascade-errata.md`** (this file) | confirm the amended provider/prompt/event seams and the new session events do not violate a top-level invariant; update the seam inventory |
| **A1** | `29-event-family-errata.md` | the `EventType`/`wire_name` extension rule, the two-axis compatibility rule (§4.6), the live-only vs durable split, the consumer matrix (§4.3.9.2), and the `llm/request_header` event + codec |
| **A2** | `28-llm-service-boundary-errata.md` | `LlmRuntime`/`PreparedCall`/`FrozenRequest`, freeze/serialization/digest, and `LLMErrorCode::InvalidPreparedCall` |

**Only Stage A blocks Wave 1** — with two additions that the verified spec 26 §5
understates. Both are **Wave-1 blocking prerequisites**: they must be verified
before any Wave-1 code, even though `26 §5` schedules them in Stage-B Wave 4
(`26p2:1389`):

1. **The `06` agent-loop errata.** Wave 1 changes `AgentLoop::buildRequest`
   (`26-dsh-alignment-part2.md` §5 Wave 1 :1404) and re-seams `ContextCompactor`
   (`:1412-1413`), and both are owned by spec `06`; §4.1 of this errata
   classifies the `06` seam as `Brk. (D1)` (`26p2:1092`). Scheduling the `06`
   errata in Wave 4 is a **sequencing defect**: Wave 1 would start without its
   owning spec. Independently confirmed by
   `28-llm-service-boundary-errata.md` §13.1 and by an independent gate of this
   document (F-1).
2. **The `13` context-compaction errata.** The same Wave-1 compactor re-seam
   changes the constructor pinned by `13-context-compaction.md §5.2
   :604-610,633` from `LLMProvider&` to `LlmRuntime&`; the 13 spec must be
   amended before that code lands. `26 §5` also schedules the `13` errata in
   Stage-B Wave 4 — the same sequencing conflict. Independently confirmed by
   `28-llm-service-boundary-errata.md` §13.3.

The `06` and `13` errata therefore join the Wave-1 gate (as Stage-A items A3/A4,
or as Stage-A-adjacent prerequisites gated immediately before Wave 1). Stage A's
three up-front *freezes* (A0/A1/A2) are unchanged; AC-I4 is read accordingly.

**Named ownership gap (recorded, not resolved): the retry executor has no wave.**
`26-I3` and the `llm/retry`/`llm/retry-started` events (`26p2 §4.3.9.1
:947-948`) require a separate durable retry executor, but `26 §5` Waves 1–6
(`:1397-1498`) contain **no retry-executor item**. This is a genuine ownership
gap: no wave owns the executor. The one-attempt contract and the no-double-retry
migration constraint are pinned by `28-llm-service-boundary-errata.md`
§6.2/§13.2, but the implementing wave is unassigned. This errata records the gap
rather than inventing a wave.

Nothing is coded until its owning spec is verified.

### 5.2 Wave 1 is not blocked on the prompt-registry spec

`26 §5:1377-1380` pins that Wave 1's request header carries the **current**
`AgentConfig::system_prompt`, so the prompt-registry spec (reserved
`27-system-prompt.md`, renumbered `36+` per §2) is **not** a Wave-1 dependency.
Freezing the prompt registry before Wave 1 would buy zero Wave-1 risk reduction.
This errata adopts that rule: **the prompt-registry spec must not be moved into
Stage A.**

### 5.3 Stage B — JIT-gated per wave (verified before that wave's code)

| Wave | Specs verified before its code |
|---|---|
| 1 | `21` errata — the new `session.persist_prompt_text` key (D23); the Stage-A A1 `01` errata already pins the D24 wire-vocabulary rule; no `05` change |
| 2 | `01` assistant-stream payloads; the `08` assembler/replay errata — now `34-assembler-replay-errata.md` (Rev 3, verified), with the `StreamEvent` codec in `33-stream-event-codec-errata.md` (verified) |
| 3 | the prompt-registry spec (reserved `27-system-prompt.md`, renumbered `36+`); `01` provenance payloads; `17`/`21` errata |
| 4 | the output-retention spec (reserved `28-output-retention.md`, renumbered `36+`); `06`/`07`/`13` errata |
| 5 | the presets spec (reserved `29-agent-presets.md`, renumbered `36+`); `23` errata; `01` preset event |
| 6 | the goals/jobs/commands spec (reserved `30-goals-jobs-commands.md`, renumbered `36+`); `01` goal/job/command events |
| any | `10`/`11`/`20`/`22` errata only where that wave changes them |

**Two-gate rule preserved.** Stage B defers the *verification date*, not the
requirement: every owning spec is still verified before its wave's code.

## 6. Migration-program prerequisites (top-level statement)

Three prerequisites are decisions the program depends on, stated here at the
top level so they are visible to the gate rather than buried in a component spec.

### P1 — User scope decision

The program is a **mechanics-alignment** effort, not a port. Per
`26-dsh-alignment.md §1.2`:

- **In scope (copy the design):** the shape of the seams (provider-neutral call
  service, frozen request, one attempt per stream, retry as a separate executor
  at a durable step boundary); the data model; the output-processing pipeline;
  the prompt system; and the way of working/collaboration (presets, subagents,
  goals, jobs, non-model slash commands).
- **Out of scope / forbidden:** copying dsh TypeScript source; the Cordis
  plugin/composition runtime, `cordis.yml`, or `!!js`; building the dsh package
  graph; and writing implementation code in the design docs.

This scope is the reason the cascade in §4 is bounded: the excluded surfaces
(Cordis, PTC, remote transports) contribute no seam change to `00`.

**User decision (2026-09-19, recorded).** Run **Waves 3–4 together, then a
checkpoint** (Wave 3 = the prompt system, the first model-visible wave; Wave 4 =
output processing, the other half of the stated goal; Wave 4 must land after
Wave 3 per §5.3). **Wave 6 (goals/jobs/commands) remains in scope** for a later
slice — it is not cut. Wave 5 follows Wave 3.

### P2 — Prompt-logging policy

`26-D23` (`:161`) and `26 §4.9` pin the durable prompt policy:

- The `llm/request_header` event **always** carries `system_prompt_digest`.
- The full rendered `system_prompt` **text** is stored only under the dedicated
  `session.persist_prompt_text` key (bool, default **`false`**, new `[session]`
  section, **global layer only** — a workspace-layer occurrence is a
  `ConfigError`).
- This key governs the **session-DB copy only** and is deliberately distinct from
  `logging.log_prompts`, which governs the spdlog surface and never the session
  DB.
- Absent the opt-in, replay re-derives the prompt from the live registries and
  verifies it against the digest; it **never fabricates** it.

The default durable record is therefore the digest, and the mechanism is safe by
default. Whether to ever enable durable full-prompt capture remains a user
product decision.

**User decision (2026-09-19, recorded).** **Keep the default `false`**; enable
`session.persist_prompt_text` only per test run when a baseline needs to be
diffed (e.g. the Wave-3 checkpoint records its baseline with the flag on in a
scratch workspace). The shipped default does not change.

### P3 — No-downgrade acceptance (the one-way door)

`26 §4.6` pins the compatibility asymmetry:

- **On disk:** once a new binary appends a new event type, an **older binary
  cannot open that DB** — the durable decode keeps its loud forward-fence
  (`CorruptionError`). This is a **one-way door and an explicit product
  decision**: the Wave-0 freeze must record the user's acceptance; a
  migration/dual-read window is not proposed; **downgrades are unsupported**.
- **On the wire:** the same new type is **skipped** by an older receiver (cursor
  still advances), so it does not by itself break the connection.
- `kProtocolVersion` stays `1`; a bump is reserved for an envelope-shape or
  handshake change.

Recording P3 is a Wave-0 gate obligation: this errata is where the acceptance is
recorded for the gate. Until the user's sign-off is on record, the top-level gate
does not pass.

**User sign-off (2026-09-19, recorded).** **ACCEPTED.** The no-downgrade one-way
door is accepted as specified: after a new binary appends a new event type, an
older binary cannot open that DB (the loud forward-fence holds); downgrades are
unsupported; no migration/dual-read window is proposed. **With P1/P2/P3 on
record, the top-level gate passes.**

## 7. Supersession map — what this errata does NOT change

To keep the top-level gate's invariants intact, the following are explicitly
**not** changed by the architecture-level seams:

| Surface | Status |
|---|---|
| Session event log is the durable source of truth (`00 §9.1`) | unchanged (D2/D9 extend the vocabulary; the log remains the source of truth) |
| Session-DB `kSchemaVersion` and structural shape | unchanged at `1`; no DDL delta (D16 header fields ride `sessions.metadata JSON`, `26 §4.6`) |
| Wire envelope `SessionEnvelope = {session,event}` and handshake | unchanged; `kProtocolVersion` stays `1` (D24 / `26-I12`) |
| Registry DB schema 2 and its single-writer `flock` discipline | untouched |
| Path safety / `ExecutionEnvironment::resolve()` root-relative resolution | unchanged (no seam in this program touches it) |
| Supervisor-owned daemon lifetime (spec 16) | unchanged; `04`/`16` are asserted unaffected because the handshake/version do not change |
| One provider attempt per stream; retry only before the first dispatched event | preserved by `LlmRuntime` (the retry barrier moves, it is not removed) |
| The live-only switcher rule and transcript render contracts | preserved (`10`/`17` additive only) |

## 8. Invariants

- **AC-I1 — No new decision.** Every classification in this errata is quoted
  from the verified spec 26; this errata adds no semantic. A reader can trace
  each class to `26 §4.2`/`§4.4`.
- **AC-I2 — Bounded cascade.** The affected set is exactly §4.1 plus the
  `10/11/17/20/21/22/23/24` set from `26 §4.4`'s other table. No claim of "all of
  01–24".
- **AC-I3 — Completeness is gated here.** If a later wave discovers a touched
  spec missing from §4.1/§4.2, that is an open finding against this errata, and
  the top-level gate does not pass until it is resolved.
- **AC-I4 — Stage A's up-front freezes are exactly three items, plus the Wave-1
  blocking component errata.** The three freezes are `00` (this errata), the
  `01` event-family errata (`29`), and the `08` service-boundary errata (`28`);
  additionally the `06` and `13` errata are Wave-1 blocking prerequisites
  (§5.1). The prompt-registry spec is not in Stage A.
- **AC-I5 — No in-place rewrite.** This errata amends `00` by reference; the
  affected `00` sections keep their text and are read through §3/§7.
- **AC-I6 — Numbering is authoritative.** `27`/`28`/`29`/`30` are the
  session-locking / LLM-service-boundary / event-family / architecture-cascade
  errata; the Wave-3+ specs renumber to `36+`.

## 9. Failure modes

- **AC-F1 — Over-broad cascade.** A reader treats the top-level-gate change as
  invalidating every component spec. Guard: §4.1/§4.2 are the complete list;
  `02`/`03`/`09`/`14`/`19` are asserted unaffected.
- **AC-F2 — Under-broad cascade.** A touched spec (e.g. `13`'s payload, `07`'s
  `ToolResult`) is not re-checked. Guard: §4.1 names each breaking seam and the
  owning spec's Stage-B errata.
- **AC-F3 — Version bump by accident.** A new event type is treated as a wire
  break and `kProtocolVersion` is bumped. Guard: D24 / `26-I12` pin `1`;
  §7 restates it.
- **AC-F4 — Wave 1 blocked on the prompt registry.** Stage A grows to include the
  prompt spec. Guard: §5.2 pins that Wave 1 carries the current
  `AgentConfig::system_prompt`.
- **AC-F5 — Silent prompt capture.** The full prompt text is persisted without
  the opt-in. Guard: P2's default is digest-only, the key is global-layer only,
  and it is distinct from `logging.log_prompts`.
- **AC-F6 — Unrecorded downgrade acceptance.** The one-way door is passed without
  the user's sign-off. Guard: P3 makes the acceptance a Wave-0 gate obligation.
- **AC-F7 — Stale numbering.** The Wave-3+ specs are still called `27`–`30`.
  Guard: §2 renumbers them to `36+` and supersedes the reservation.

## 10. Test plan

This errata is a design artifact; its verification is a **gate review**, not
code tests. The gate checks:

1. **Traceability.** Every class in §3/§4 is present verbatim-or-paraphrased in
   `26 §4.2`/`§4.4`, with the cited line range. (AC-I1)
2. **Completeness.** §4.1/§4.2 cover every spec named in `26 §4.4`'s two tables
   and its unaffected list, and no spec is claimed "all of 01–24". (AC-I2/AC-I3)
3. **Stage A discipline.** §5.1 pins the three up-front freezes A0/A1/A2 and the
   `06`/`13` Wave-1 blocking prerequisites; §5.2 pins Wave 1 off the prompt
   registry. (AC-I4)
4. **Numbering.** §2's table matches the files on disk; the Wave-3+ renumbering
   is stated. (AC-I6)
5. **Non-amendment.** §7 lists every surface this program leaves untouched; the
   `00` invariants (event log source of truth, envelope/version, path safety,
   daemon ownership) are all present. (AC-I5)
6. **Tree anchors.** Every `file:line` in §3 resolves to the named symbol at the
   freeze commit.

## 11. Revision log

- **Rev 2 (2026-09-19).** Closes the gate30 HIGH-1 staging defect. §5.1 now
  records the `06` agent-loop errata and the `13` context-compaction errata as
  **Wave-1 blocking prerequisites** (pulled forward from `26 §5`'s Stage-B Wave 4,
  because Wave 1 changes `AgentLoop::buildRequest` and re-seams
  `ContextCompactor`), and records the **retry executor** as a named ownership gap
  (`26 §5` Waves 1–6 contain no retry-executor item). AC-I4 and the §10 test plan
  are reconciled. Rev 1 is retained below.
- **Rev 1 (2026-09-19).** Initial write. Pins the four architecture-level seams
  (S1 provider `LlmRuntime`; S2 event family; S3 session header; S4
  presets/goals/jobs/commands) with the `26 §4.4` classification; the cascade
  coverage and the `18`/`25` judgment-call caveat; the Wave-0 Stage A/B restaging
  (Stage A = `00` + `01` event-family + `08`; Wave 1 not blocked on the
  prompt-registry spec); the three migration prerequisites (user scope,
  prompt-logging policy, no-downgrade acceptance); and the numbering
  supersession (`27`/`28`/`29`/`30` are the four errata; Wave-3+ specs renumber
  to `36+`). Amends `00-architecture.md` by reference only.
