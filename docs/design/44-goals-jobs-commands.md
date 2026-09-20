# 44 — Goals, Jobs, and Commands (Wave 6)

```
Status: Rev 2 written · verified: — · reviewer: —
Authority: owning spec for `26-dsh-alignment-part2.md` §5 Wave 6 (decisions
           `26-D18`, `26-D19`, `26-D20`). The reserved filename
           `30-goals-jobs-commands.md` named in `26-dsh-alignment-part2.md`
           §5 Stage B (line 1395) is **unavailable**: 30 is the
           architecture-cascade errata and 31–43 are taken. This file is that
           spec, numbered **44**.
Component: 44 (owning spec). Owns the Wave-6 collaboration component: (1) the
           durable, log-derived **goal** domain — objective, phases,
           compare-and-set revisions, the admitted-round counter, the
           process-local activation, the round driver, the model-facing goal
           tools, and `/goal` (`26-D18`); (2) the **owner-scoped job registry**
           — ids, lifecycle, ownership fence, `job_output`/`job_list`/
           `job_kill`, and the completion wakeup policy (`26-D19`); and (3) the
           **log-only command surface** — `command/run`/`command/done` and
           agent-scoped shadowing (`26-D20`). It amends `01-session.md` (four
           new event types + the `deriveMessages`/consumer cases),
           `36-prompt-registry.md`/`37-message-provenance-errata.md` (the
           `MessageSource::Kind::Goal` provenance), `06-agent-loop.md` (the
           round driver's `followup`/`inject` use and owner-disposal cleanup),
           `14-pty-capability.md` (PTY as a job producer), and
           `21-config-jsonc-errata.md` (the `goals.*`/`jobs.*` keys). It does
           not edit any of those files in place.
Depends on: `26-dsh-alignment.md` (design source; §1.2, §2.4),
            `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS) §4.1,
            §4.3.8, §4.3.9, §4.3.9.1, §4.3.9.2, §4.4, §4.6, §4.9, §5 Stage B,
            §5 Wave 6, §6 OQ7;
            `36-prompt-registry.md` (Wave 3; the prompt/context + provenance
            seam): verified per `DESIGN_STATUS.md:55`;
            `42-agent-presets.md` (Wave 5; subagents + scopes): verified per
            `DESIGN_STATUS.md:64` — **required before Wave-6 code**;
            `43-wave5-dependency-errata.md`: verified per
            `DESIGN_STATUS.md:65`;
            `14-pty-capability.md` (verified per `DESIGN_STATUS.md:22`; the PTY
            producer seam);
            `29-event-family-errata.md` (the event-family contract; verified;
            its §4.4 reserved rows carry all four Wave-6 events);
            `30-architecture-cascade-errata.md` §3 (S3/S4; the header and
            collaboration cascade);
            `01-session.md`, `06-agent-loop.md`, `07-tools-execution.md`,
            `10-supervisor-tui.md`, `17-ui-transcript-errata.md`,
            `21-config-jsonc-errata.md` (all verified); the working tree at
            authoring time.
Scope: pin, for Wave 6, (1) the goal domain, its events, its projection, the
       compare-and-set mutation contract, the round-admission rule, the
       process-local activation, the round driver, the goal tools, and the
       `/goal` grammar (`26-D18`); (2) the job registry, its identity and
       ownership fence, the `job/changed` event, the three model-facing job
       tools, and the completion wakeup policy (`26-D19`); and (3) the
       log-only command surface, the `command/run`/`command/done` pairing, and
       agent-scoped shadowing (`26-D20`). Design only, no code.
Supersedes: `26-dsh-alignment-part2.md` §5 Stage B (line 1395) and
            `30-architecture-cascade-errata.md:324`, which reserved the name
            `30-goals-jobs-commands.md`. That number is taken; the reserved
            artifact is this file.
```

This document is a **pin**, not a proposal. Every `file:line` below was
re-derived against the working tree at authoring time. The goal domain, the job
registry, the log-only command surface, the four new event types, and the
`MessageSource::Kind::Goal` provenance **do not exist in the tree yet**; they are
pinned here as the contract Wave-6 code must satisfy. Where the design source
and a verified spec disagreed, Rev 1 records the disagreement as an open
question (§13) rather than silently resolving it. Per `AGENTS.md`, no code for
this component may be written until this spec is `verified` and its `01`/`36`/
`37`/`21` dependency contracts are pinned (Stage B, §10).

---

## 1. Purpose, numbering, and the staging constraint

### 1.1 Why this spec exists

`26-dsh-alignment.md` §1.2 puts "the way of working and collaboration" in scope:
"presets that compose a session, subagents that join the parent composition,
goals, background jobs, and slash commands that are not model messages"
(`26-dsh-alignment.md:61-63`). Wave 5 (`42-agent-presets.md`) shipped the first
half: presets and subagent composition. Wave 6 is the second half, defined by
`26-dsh-alignment-part2.md` §5 Wave 6 (`:1494-1501`). That document's §6 OQ7
(`:1617-1620`) recommended deferring Waves 5 and 6 ("ship Waves 0 through 4
first; decide 5 and 6 after"); the user decided to keep Wave 6 in scope, and
this spec is that decision's artifact (§13 OQ-7).

The shipped tree has **no** goal, job, or log-only command concept:

- The agent loop queues work through `Agent::send`/`followup`/`steer`/`inject`
  (`include/ymh/agent/agent.hpp:143-146`), but nothing derives a continuation
  from a durable objective.
- The only command registry is supervisor-local and explicitly **log-free**:
  "a handler mutates the presentation model through the `CommandContext`
  callbacks and never touches the wire or the event log"
  (`include/ymh/ui/command_registry.hpp:3-5`; `src/ui/command_registry.cpp:104`).
  Its twelve builtins (`src/ui/command_registry.cpp:129-260`) include
  `/compact` and `/sessions`, but `/goal` is impossible here because it must
  append durable state.
- `SubagentRunner` (`include/ymh/agent/subagent.hpp:17-24`) and the PTY
  (`include/ymh/execution/pty.hpp:336-376`) both run background work, but there
  is no registry that names, lists, reads, or kills it, and no completion
  wakeup.

Wave 6 supplies the missing seam.

### 1.2 Authority and relationship to the other specs

- `26-dsh-alignment-part2.md` is the **design source**. `26-D18`/`D19`/`D20`
  (`:156-158`) name this file as the owning spec; §4.3.8 (`:774-799`) pins the
  goal/job/command value types; §4.3.9 (`:845-848`) pins the four event types
  and wire names; §4.3.9.1 (`:955-959`) pins their JSON key schemas;
  §4.3.9.2 (`:992-995`) pins their consumer obligations; §4.9 (`:1324-1329`)
  pins the `goals.*`/`jobs.*` config keys.
- `01-session.md` owns the event model this spec extends. The append protocol
  is §6.1 (`:695-706`), `deriveMessages()` is §6.3 (`:726`), and the
  no-`default:` projection switch is `src/session/session.cpp:385`.
- `36-prompt-registry.md` owns the prompt/context and provenance seams. This
  spec activates the reserved `ToolGoal`/`ToolJobs` section orders
  (`include/ymh/prompt/order.hpp:30,36`) and **extends** `MessageSource::Kind`
  (`36 §3.1`, `:584-592`) with `Goal`; the extension is pinned in §5.4 and is
  an additive errata to `36`/`37`.
- `42-agent-presets.md` owns the subagent and scope model. The job registry
  fences access by the same `AgentId`/`SessionId` the roster uses, and the
  command registry's agent scope is the roster's `AgentContext` leaf
  (`include/ymh/agent/preset.hpp:38-40`). `42-D16` defers continuable
  subagents (`:1168`); §13 OQ-4 records the interaction.
- `14-pty-capability.md` owns the PTY. A `terminal` session is a job producer
  (`14 §7.5`, `:1316-1332`).
- `29-event-family-errata.md` owns the event-family contract. All four new
  events are its §4.4 reserved rows (`:391-394`), and this spec's obligations
  obey `29-I2`/`I5`/`I6` (`:406-419`).
- `30-architecture-cascade-errata.md` §S3/§S4 (`:164-205`) records the
  top-level cascade. §13 OQ-1 records one claim in `S3` this spec cannot
  reconcile with the dsh design.

### 1.3 In scope

1. The durable goal domain: `GoalSnapshot`, `GoalRef`, `GoalPhase`,
   `GoalBlockReason`, `GoalProjection`, compare-and-set revisions, the seven
   operations, the admitted-round counter, and the `goal/change` event.
2. The process-local `GoalActivation` (`armed`/`disarmed`), never persisted.
3. The round driver: admission of an automatic continuation round, its
   fail-closed race fences, and the `round-limit`/`queue-failed`/
   `prompt-rejected` blocks.
4. The model-facing goal tools (`get_goal`, `create_goal`, `update_goal`) and
   the `/goal` command grammar.
5. The owner-scoped job registry: `JobId`, `JobStatus`, `JobSnapshot`,
   `JobHooks`, `JobOutcome`, settlement, and owner cleanup.
6. The `job/changed` event and the `job_output`/`job_list`/`job_kill` tools.
7. The completion wakeup policy and its bound.
8. The log-only command surface: `CommandSpec`, `CommandInput`,
   `CommandOutcome`, the `command/run`/`command/done` pairing, and
   agent-scoped shadowing.
9. The four event types, their codecs, and their consumer-matrix obligations.

### 1.4 Out of scope

- **The UI itself.** Spec `10`/`17` own the TUI surfaces; this spec pins only
  the events and read models those surfaces consume (`26-I10`, `42-I11`).
- **Continuable subagents / control tools** (`42-D16`, `42-agent-presets.md:1168`).
  Wave 6 spawns a subagent as a job producer through the existing
  `SubagentRunner`; it does not add a continuation control tool.
- **A new process, transport, or persistence schema.** No `kSchemaVersion` or
  `kProtocolVersion` bump (`29-I6`, `26-I12`).
- **The PTC `run_code` SDK, workflow, ralph, attachments/images, remote
  transports** (`26 §5.1`, `:1503-1523`).
- **A TUI redesign.** Wave 6 adds row kinds and panels; the verified `10`/`17`
  chrome budget and the live-only switcher rule (`22`) are preserved
  (`26-dsh-alignment-part2.md:1521-1522`).

---

## 2. The goal model

### 2.1 Durable state and the log

Goal state is **event-sourced**. The durable source of truth is the
`goal/change` event; the current goal is a **pure projection** over the log,
exactly like `deriveMessages()` (`01 §6.3`, `:726`). There is no goal column in
`SessionHeader` (`include/ymh/session/session.hpp:44-63`) and no new DB column
(§13 OQ-1).

A `GoalSnapshot` is written whole by every non-`clear` mutation; a `clear`
writes only a tombstone `GoalRef`. The dsh source types are
`GoalSnapshotChangeMeta`/`GoalClearChangeMeta` (`dsh-goal/lib/types/domain.d.ts:14-32`).

### 2.2 Phases and transitions

`GoalPhase` is `Active | Paused | Blocked | Complete` (dsh
`types.d.ts:38`). The mutation verbs and their legal transitions are pinned:

| Operation | Legal from | Result phase | Activation after | Source |
|---|---|---|---|---|
| `create` | none, or `Complete` | `Active` | `armed` | `/goal <objective>` |
| `edit` | any current goal | unchanged | unchanged | `/goal edit <objective>` |
| `pause` | `Active` | `Paused` | `disarmed` | `/goal pause` |
| `resume` | `Paused`, `Blocked`, or `Active` while disarmed | `Active` | `armed` | `/goal resume` |
| `complete` | any non-`Complete` | `Complete` | `disarmed` | `update_goal` tool |
| `block` | `Active` | `Blocked` | `disarmed` | `update_goal` tool |
| `clear` | any current goal | (tombstone) | `disarmed` | `/goal clear` |

`resume` on a `Complete` goal, or on a goal whose `rounds_started >=
max_goal_rounds`, is invalid (dsh `index.js:197,696`). A `block` requires a
lower-kebab-case `code` and a non-empty `message` (dsh `index.js:471-483`).

### 2.3 Compare-and-set revisions

Every durable mutation increments `revision`. A caller mutates by passing the
exact `GoalRef{id, revision}` it read; a stale revision is rejected. This is
dsh's `GoalRef` compare-and-set (`dsh-goal/lib/types/types.d.ts:16-22`) and
`GOAL_STALE_REVISION` (`domain.d.ts:75`). `create` produces revision `1`
(dsh `index.js:247`).

### 2.4 Admitted rounds

The round counter is **derived from the log**, never stored in the snapshot:

- `GoalChange.rounds_started` carries the admitted count at the mutation
  (`26-dsh-alignment-part2.md:921`; dsh `domain.d.ts:19`).
- A goal round is an ordinary `user/message` event whose `MessageSource` is
  `Goal{goal_id, revision, round}` (§5.4). The projection admits it only when
  it is the **next** round of the **current** revision: `round ==
  rounds_started + 1`, `round <= max_goal_rounds`, `phase == Active`, and the
  source's `goal_id`/`revision` match (dsh `index.js:273-278`).
- A non-contiguous, stale-revision, or over-cap goal message is a
  **projection failure**, not a silent skip (dsh `index.js:277`; `GoalError`
  `GOAL_INVALID_TRANSITION`).

The projection is strict: the first invalid owned event is retained as a
`failure` and host goal access rejects that state, while the client view stays
at the last valid goal (dsh `dsh-goal/lib/types/types.d.ts:100-108`).

### 2.5 Process-local activation

`GoalActivation` is `Armed | Disarmed` and is **never persisted** (dsh
`types.d.ts:57-58,81-82`). It is per-session process-local state:

- Every path that brings a session into a process (fresh create, resume, fork,
  or attach) constructs its process-local `GoalService` `Disarmed` (dsh
  `index.js:594`). The seam is the resident-session path in `SessionManager`:
  `createSession`/`resumeSession`/`forkSession`
  (`include/ymh/session/session_manager.hpp:49-52`; `loadInto`,
  `src/session/session_manager.cpp:43-49`), the same path attach resumes
  through. The durable `session/start` event (`EventType::SessionStarted`,
  `include/ymh/core/event.hpp:52`) marks create and fork only, so the disarm is
  the process-local construction, not an observed event. This is the "goal
  disarm across resume/fork" rule in `26-dsh-alignment-part2.md:1498`.
- `create` and `resume` arm it; `pause`, `complete`, `block`, and `clear`
  disarm it (dsh `index.js:648,677,698,711,728,751`).
- The round driver only continues an `Active` goal that is `armed` (§2.6).

### 2.6 The round driver

The driver owns automatic same-session continuation. Its normative loop, per
agent:

1. On `turn/end` while idle, read the current goal.
2. If no goal, or `phase != Active`, or `activation != Armed`, stop.
3. If `rounds_started >= max_goal_rounds`, append a `block` mutation with
   `code = "round-limit"` and stop (dsh `index.js:125-131`).
4. Otherwise render the round prompt (below), build a `user/message` with
   `MessageSource::Kind::Goal`, and enqueue it with `Agent::followup`
   (`include/ymh/agent/agent.hpp:144`). The driver runs under the
   "no initiator" wrapper so the round is not attributed to a human.
5. If `followup` throws, `block` with `code = "queue-failed"` (dsh
   `index.js:150-158`).

The round prompt is copied verbatim from dsh
(`dsh-goal-round-driver/lib/index.js:12-19`); `objective` is JSON-escaped:

```text
<goal_round>
Objective: "<objective>"
Round: <round>/<max_goal_rounds>

Continue working toward the objective in this same session. Treat the current
workspace, tool results, and durable session state as authoritative; inspect
them instead of assuming earlier narration is still current. Make concrete
progress and verify the result. Before claiming completion, gather evidence
that the whole objective is achieved, read the current goal, and mark it
complete. If work remains, leave the goal active for the next round. Follow the
configured goal-tool policy before reporting a blocker.
</goal_round>
```

**Fail-closed race fences.** The driver is a single coalesced run per agent
(dsh `index.js:162-190`). At `pre-step`, the queued round is admitted only if
it still owns the exact live revision: `phase == Active`,
`activation == Armed`, `round == rounds_started + 1`, and the queued
`(goal_id, revision, round)` and content are unchanged (dsh
`index.js:243-247`). Otherwise the round is rejected and the driver
re-drives; a rejection while the goal is still live blocks with
`code = "prompt-rejected"` (dsh `index.js:293-306`). A driver error disarms.
A `turn/end` with reason `max-tokens` disarms (dsh `index.js:266-268`).

### 2.7 Model-facing goal tools

The model completes or blocks a goal through three tools (dsh
`dsh-tool-goal`), registered with the `tool:goal` section at order `2400`
(`include/ymh/prompt/order.hpp:36`):

| Tool | Effect |
|---|---|
| `get_goal` | read the current `GoalProjection` (no mutation) |
| `create_goal` | `create` (objective, optional `max_goal_rounds`) |
| `update_goal` | `edit`, `pause`, `resume`, `complete`, or `block` |

`update_goal`'s `block` path is gated by
`goals.blocked_after_consecutive_rounds` (default 3, `26 §4.9:1325`): the model
may self-report `blocked` only after that many consecutive admitted rounds
without progress (dsh-tool-goal `Config.blockedAfterConsecutiveRounds`). The
policy is a `tool:goal` guidance section, not a hidden rule.

### 2.8 `/goal`

`/goal` is a **durable command** (§4), not a UI-only command. Its grammar is
copied from dsh (`dsh-command-goal/lib/index.js:10-24`):

```text
Usage: /goal [<objective>|clear|edit <objective>|pause|resume]
```

- empty input → show the current goal (phase, blocker, objective, rounds,
  activation, and the state-legal next commands);
- `clear` → `clear`; `pause` → `pause`; `resume` → `resume`;
- `edit <objective>` → `edit`; a bare `edit` → invalid-edit error;
- any other input → `create` with that objective.

`complete` and `block` are **not** `/goal` verbs; they are model-tool verbs
(§2.7). Rendering hides compare-and-set internals and prints
`Rounds: <rounds_started>/<max_goal_rounds>` and `Activation: <armed|disarmed>`
(dsh `index.js:78-95`).

---

## 3. The job model

### 3.1 Identity and lifecycle

A job is a named unit of background work owned by one live agent (or unowned).
`JobId` is `<kind>-<ordinal>` where `kind` is the producer kind and `ordinal`
is a per-registry monotone counter (dsh `dsh-jobs/lib/types/index.d.ts:54`).
Ids are **predictable**: authorization is by ownership, not secrecy
(`26-I8`, `:83-85`).

`JobStatus` is `Running | Stopping | Completed | Killed | Failed`; `running`
may pass through `stopping` and then reaches exactly one terminal status
(dsh `types.d.ts:10-14`). Settlement is **first-wins**: one terminal record,
released waiters, and one round of contained listener notification, even
against a late producer outcome (dsh `index.d.ts:31-36`).

### 3.2 Ownership fence

Access is fenced by the owner's session id:

- `start` takes an optional owner `AgentId`; omitting it creates an unowned
  job open to any caller until service disposal (dsh `types.d.ts:49-55`).
- `list` returns caller-owned and unowned jobs only, in registration order,
  never another session's labels (dsh `index.d.ts:57-63`).
- `get`/`read`/`kill`/`wait` throw for an unknown or foreign job (dsh
  `index.d.ts:64-102`).
- Owner disposal cancels and awaits the owner's live jobs; teardown
  cancellation marks the record `reported` because no reader is left (dsh
  `index.d.ts:24-29`).

### 3.3 Producers

The producer owns execution resources; the registry owns identity and
lifecycle. A `JobStart` carries `kind`, a one-line `label`, an optional
`output_limit_bytes`, an optional `owner`, and a synchronous `run()` that
returns `JobHooks` (dsh `types.d.ts:39-62`). `run()` is called once; a throw
leaves nothing registered.

Wave 6 pins two producers:

- **PTY** (spec `14`): a persistent `terminal` session is registered as a job
  whose `readOutput` drains the PTY output ring and whose `cancel` calls
  `PtySession::terminate` (`14 §3.1`, `:299-334`; `§7.5`, `:1316-1332`).
- **Subagent** (spec `42`): a `SubagentRunner::run` invocation
  (`include/ymh/agent/subagent.hpp:24`) is registered as a job whose outcome
  maps `Completed`/`Cancelled`/`Failed` to the corresponding `JobStatus`.

`kind` is merge-extensible; the registry treats it as an opaque id namespace
(dsh `types.d.ts:19-24`).

### 3.4 Read and kill

- `read` returns the next stream delta since the previous read, or the
  idempotent final output after settlement; a terminal read marks the job
  `reported` (dsh `types.d.ts:120-128`; `index.d.ts:72-80`).
- `kill` requests cancellation, then marks `stopping` and `reported`; a
  producer throw propagates without changing state; a live job returns
  `requested`, a settled one `already-finished` (dsh `index.d.ts:81-90`).
- `wait` blocks up to a timeout without cancelling; after settlement the
  terminal snapshot wins (dsh `index.d.ts:91-102`).

### 3.5 Wakeup policy

An **unreported** completion reaches its owner:

- injected into a **busy** owner's next step (`Agent::inject`,
  `include/ymh/agent/agent.hpp:146`); or
- if the owner is **idle**, `jobs.completion_delivery` decides:
  `wakeup` (default) opens a turn for it (`Agent::followup`), `quiet` leaves it
  pending until something else wakes the owner.

The self-exciting chain is bounded by `jobs.max_consecutive_wakes` (default 3,
`26 §4.9:1329`): after that many wake-opened turns without new user-authored
input, further notices degrade to injection. The counter resets on any
**user-authored** input — a goal round (§2.4) is **not** user-authored and
must not reset it.

---

## 4. The command model

### 4.1 Log-only, never model-visible

A command invocation appends two durable events paired by `command_id`,
mirroring the `tool/call`↔`tool/result` pairing: `command/run` when the handler
is entered, `command/done` when it settles (dsh
`dsh-commands/lib/types/types.d.ts:87-117`). The events are **log-only**: they
are ignored by `deriveMessages()` and are excluded from token accounting
(`26 §4.3.9.2:992-993`; `29-I5`, `:415-416`). A thrown or aborted handler
settles as `kind = "error"` with the rendered failure.

### 4.2 The two registries

There are two command surfaces, and they do not merge:

1. **The existing `ui::CommandRegistry`** (`include/ymh/ui/command_registry.hpp:52`)
   is supervisor-local and **log-free** by contract (`:3-5`). It keeps the
   twelve pure-presentation builtins (`src/ui/command_registry.cpp:129-260`).
2. **The new `ymh::CommandRegistry`** (this spec) is host-side, agent-scoped,
   and log-only. It is the authority for any command whose effect is durable
   (`/goal`) or that must be attributed in the log.

The UI dispatches a durable command by forwarding the raw line to the host
(§13 OQ-2 records the wire-method question); the host resolves it in the new
registry, appends `command/run`, runs the handler, and appends `command/done`.
The UI then renders the outcome from the event stream, not from the handler
return, so a resumed/attached supervisor sees the same rows.

### 4.3 Agent-scoped shadowing

Commands are registered into a scope (the roster's `AgentContext.scope` chain,
`42 §2.2`). Resolution walks the chain nearest-first: an agent-scoped command
shadows a session-scoped one, which shadows a global one, by name (dsh
`dsh-commands` scoped registration; `26-D20`). Registering the same name twice
**within one scope** fails loud (`26-I7`); registering it in a nearer scope is
the shadowing mechanism. The `source` of an invocation is `user` for a
human-typed line and `agent` for an agent-initiated invocation; `agent` is a
ymh addition (dsh's `CommandSourceMap` has only `user`,
`dsh-commands/lib/types/types.d.ts:63-75`).

### 4.4 `recordInput`

A `CommandSpec` may set `record_input = false` when an authoritative domain
event already owns the input payload; then `command/run.args` is omitted and
the richer event carries the input (dsh `types.d.ts:90-97`). `/goal edit`
does **not** set it: the objective is not duplicated elsewhere, so `args`
carries it verbatim (separator whitespace included).

---

## 5. C++ interfaces (pinned)

All types live in `namespace ymh`. Signatures are transcribed from
`26-dsh-alignment-part2.md` §4.3.8 (`:774-799`) and refined only where a
refinement is load-bearing; every refinement is called out.

### 5.1 Shared goal types

```cpp
// include/ymh/goal/goal.hpp

// 42 §3.1 (:462) declares `using GoalId = std::uint64_t` as a shared type only.
// This spec owns the real type; it is the same alias.
using GoalId       = std::uint64_t;
using RoundNumber  = std::uint32_t;

// 26 §4.3.8 (:750). Compare-and-set identity for one exact revision.
struct GoalRef {
    GoalId        id = 0;
    std::uint64_t revision = 0;
};

// dsh GoalBlockReason (dsh-goal/lib/types/types.d.ts:40-45).
struct GoalBlockReason {
    std::string code;      // lower-kebab-case, validated
    std::string message;   // non-empty, trimmed
};

// 26 §4.3.8 (:775); dsh types.d.ts:38.
enum class GoalPhase : std::uint8_t { Active, Paused, Blocked, Complete };

// dsh types.d.ts:58. Process-local; never persisted.
enum class GoalActivation : std::uint8_t { Armed, Disarmed };

// 26 §4.3.8 (:776-778). The full durable state written by a non-clear change.
struct GoalSnapshot : GoalRef {
    std::string                    objective;
    GoalPhase                      phase = GoalPhase::Active;
    std::optional<GoalBlockReason> blocked;          // present iff phase == Blocked
    std::uint32_t                  max_goal_rounds = 0;
};

// The replay fold (dsh types.d.ts:90-99): snapshot + admitted rounds + the two
// envelope-derived timestamps. `created_at`/`updated_at` come from the
// `goal/change` envelope timestamp (the payload carries no timestamp; see
// 26 §4.3.9.1:936-944).
struct GoalProjection {
    GoalSnapshot  goal;
    RoundNumber   rounds_started = 0;
    std::int64_t  created_at = 0;
    std::int64_t  updated_at = 0;
};

// dsh types.d.ts:74-83. The live view adds process-local activation.
struct GoalView : GoalProjection {
    GoalActivation activation = GoalActivation::Disarmed;
};

// The projection state (dsh types.d.ts:100-108). `failure` is the first strict
// replay error; host access rejects that state.
struct GoalProjectionState {
    std::optional<GoalProjection> current;
    std::vector<GoalId>           seen_goal_ids;   // rejects id reuse
    std::optional<std::string>    failure;
};

struct CreateGoalRequest { std::string objective; std::optional<std::uint32_t> max_goal_rounds; };
struct EditGoalRequest   { std::optional<std::string> objective; std::optional<std::uint32_t> max_goal_rounds; };
```

### 5.2 The goal service

```cpp
// include/ymh/goal/goal_service.hpp

// dsh GoalErrorCode (dsh-goal/lib/types/domain.d.ts:75), lower-snake ymh form.
enum class GoalErrorCode : std::uint8_t {
    AgentNotLive, NotFound, AlreadyExists, StaleRevision, InvalidObjective,
    InvalidMaxRounds, InvalidBlockReason, InvalidEdit, InvalidTransition,
};
struct GoalError { GoalErrorCode code; std::string message; };

// 26-D18. Single-threaded: owned by WorkspaceRuntime and called only on the
// agent executor thread (inherits 36's contract, 36-prompt-registry.md:153-154).
class GoalService {
public:
    // All mutators append exactly one `goal/change` and return the fresh view.
    [[nodiscard]] std::optional<GoalView> get(const Agent&) const;
    GoalView create(Agent&, const CreateGoalRequest&);
    GoalView edit  (Agent&, GoalRef, const EditGoalRequest&);
    GoalView pause (Agent&, GoalRef);
    GoalView resume(Agent&, GoalRef);
    GoalView complete(Agent&, GoalRef);
    GoalView block (Agent&, GoalRef, GoalBlockReason);
    void     clear (Agent&, GoalRef);
    // Removes process-local continuation authority without a durable change
    // (dsh index.js:622-626). Used by the lifetime owner before unload.
    GoalView disarm(Agent&);
private:
    std::uint32_t default_max_rounds_;   // goals.max_rounds (default 256)
};

// Pure projection, the goal analogue of deriveMessages (01 §6.3).
[[nodiscard]] GoalProjectionState apply_goal_projection(
    const GoalProjectionState&, const EventRecord&);
```

### 5.3 The round driver

```cpp
// include/ymh/goal/round_driver.hpp

// dsh-goal-round-driver (lib/types/prompt.d.ts). The prompt text is §2.6.
[[nodiscard]] std::vector<ContentBlock> render_goal_round_prompt(
    const GoalView&, RoundNumber round);

// Installed once per WorkspaceRuntime; keeps one coalesced run per agent.
// Reacts to `turn/end`; enqueues via `Agent::followup`.
class GoalRoundDriver {
public:
    GoalRoundDriver(GoalService&, AgentRegistry&);
    void start();   // subscribes; idempotent
    void stop();    // disarms all agents, unsubscribes
};
```

### 5.4 Provenance extension (additive to `36`/`37`)

`MessageSource::Kind` is `{ User, Plugin, Model, Tool }`
(`36 §3.1`, `:584-592`). Wave 6 adds `Goal` and its payload, matching dsh's
`GoalMessageSource` (`dsh-goal/lib/types/domain.d.ts:33-40`):

```cpp
// include/ymh/agent/provenance.hpp (amended)
struct GoalMessageRef {
    GoalId        goal_id = 0;
    std::uint64_t revision = 0;
    RoundNumber   round = 0;      // positive admitted round
};
struct MessageSource {
    enum class Kind : std::uint8_t { User, Plugin, Model, Tool, Goal };
    Kind                        kind = Kind::User;
    std::string                 plugin;                 // Kind::Plugin
    ContextFormed               context;                // Kind::Plugin
    std::optional<ToolCallId>   call;                   // Kind::Tool
    std::string                 provider;               // Kind::Model
    std::string                 model;                  // Kind::Model
    std::optional<GoalMessageRef> goal;                 // Kind::Goal
};
```

`payload::UserMessage` already carries `MessageSource source{}`
(`include/ymh/session/events.hpp:89-93`), so a goal round reuses `UserMessage`
with `kind == Goal`. Because a goal round **is** a `user/message`, `is_blank`
(`42-D4`) keeps keying on the event type unchanged: a goal round counts as
session content, so it fixes the composition exactly as `42-I3` and the
"produced nothing" rationale require (`26-dsh-alignment.md:1049-1053`). Only
the wake-counter reset (§3.5) discriminates on `MessageSource::Kind::User`,
because that rule is about authorship: a goal round is not user-authored and
does not reset the wake bound.

### 5.5 Job types and registry

```cpp
// include/ymh/jobs/job_registry.hpp

using JobOrdinal = std::uint64_t;
struct JobId { std::string kind; JobOrdinal ordinal = 0; };   // "<kind>-N"

// 26 §4.3.8 (:781); dsh types.d.ts:14.
enum class JobStatus : std::uint8_t { Running, Stopping, Completed, Killed, Failed };

// dsh types.d.ts:26-33.
struct JobOutcome {
    JobStatus                  status = JobStatus::Completed;
    std::optional<std::string> detail;    // 'exit code: 3', 'max-tokens'
    std::optional<std::string> output;    // absent for stream jobs
};

// dsh types.d.ts:64-83.
struct JobHooks {
    std::function<void(std::string_view reason)> cancel;   // sync, idempotent
    Task<JobOutcome>                              done;     // never rejects
    std::function<std::string()>                  read_output;  // absent = final-only
};

// dsh types.d.ts:88-119.
struct JobSnapshot {
    JobId                       id;
    std::string                 kind;
    std::string                 label;
    std::optional<std::size_t>  output_limit_bytes;
    std::optional<AgentId>      owner;
    JobStatus                   status = JobStatus::Running;
    std::optional<std::string>  detail;
    std::int64_t                started_at = 0;
    std::optional<std::int64_t> finished_at;
    bool                        reported = false;
};

// dsh types.d.ts:39-62.
struct JobStart {
    std::string                         kind;
    std::string                         label;
    std::optional<std::size_t>          output_limit_bytes;
    std::optional<AgentId>              owner;
    std::function<JobHooks()>           run;
};

struct JobRead { std::string output; JobSnapshot snapshot; };
enum class KillResult : std::uint8_t { Requested, AlreadyFinished };

using JobDoneListener = std::function<void(const JobSnapshot&, const Agent*)>;

// Single-threaded; owned by WorkspaceRuntime (36-prompt-registry.md:153-154).
class JobRegistry {
public:
    JobId start(JobStart);
    [[nodiscard]] std::vector<JobSnapshot> list(std::optional<AgentId> caller) const;
    [[nodiscard]] JobSnapshot get (JobId, std::optional<AgentId> caller) const;
    JobRead read(JobId, std::optional<AgentId> caller);
    KillResult kill(JobId, std::optional<AgentId> caller, std::string_view reason);
    Task<JobSnapshot> wait(JobId, std::chrono::milliseconds,
                           std::optional<AgentId> caller, CancellationToken);
    void on_job_done(JobDoneListener);
    void attach_controller(std::string_view name);
};
```

### 5.6 Command types and registry

```cpp
// include/ymh/commands/command.hpp

// 26 §4.3.8 (:785). dsh CommandId (dsh-commands brand.d.ts).
using CommandId = std::uint64_t;

// 26 §4.3.9 (:896). dsh has only User; ymh adds Agent (D20).
enum class CommandSource : std::uint8_t { User, Agent };

enum class CommandOutcomeKind : std::uint8_t { Success, Error };

// 26 §4.3.8 (:786-791).
struct CommandInput {
    std::string   name;        // without the leading '/'
    std::string   raw_input;   // verbatim, separator whitespace included
    SessionId     session;
    AgentId       agent;
    CommandSource source = CommandSource::User;
};

// 26 §4.3.8 (:793-796); dsh CommandResult (types.d.ts:33-41).
struct CommandOutcome {
    CommandOutcomeKind          kind = CommandOutcomeKind::Success;
    std::string                 text;               // rendered outcome / error text
    std::optional<Sequence>     source_event_seq;   // richer domain event
};

using CommandHandler = std::function<CommandOutcome(const CommandInput&, Agent&)>;

// 26 §4.3.8 (:798-799) + the recordInput flag (dsh types.d.ts:90-97).
struct CommandSpec {
    std::string                name;
    std::string                description;
    std::optional<std::string> input_hint;
    bool                       record_input = true;
    CommandHandler             handler;
};

// Handler-free view for discovery UI (dsh types.d.ts:55-62).
struct CommandDescriptor {
    std::string                name;
    std::string                description;
    std::optional<std::string> input_hint;
};

// Host-side, agent-scoped, log-only. Single-threaded.
class CommandRegistry {
public:
    void add(CommandSpec, const ScopeKey& scope);              // duplicate in scope throws
    [[nodiscard]] std::vector<CommandDescriptor> list(const AgentContext&) const;
    [[nodiscard]] const CommandSpec* resolve(const AgentContext&, std::string_view name) const;
    // Appends command/run, runs the handler, appends command/done, returns the outcome.
    CommandOutcome invoke(Agent&, std::string_view line, CommandSource);
};
```

### 5.7 Event payloads

```cpp
// include/ymh/session/events.hpp (amended)

// dsh command/run + command/done (dsh-commands types.d.ts:99-124).
struct CommandRun {
    CommandId                  command_id = 0;
    std::string                name;
    std::optional<std::string> args;       // absent when record_input == false
    CommandSource              source = CommandSource::User;
};
enum class CommandDoneKind : std::uint8_t { Success, Error };
struct CommandDone {
    CommandId                  command_id = 0;
    CommandDoneKind            kind = CommandDoneKind::Success;
    std::optional<std::string> text;
    std::optional<Sequence>    source_event_seq;
};

// dsh goal/change GoalChangeMeta (dsh-goal/lib/types/domain.d.ts:12-32).
enum class GoalOperation : std::uint8_t {
    Create, Edit, Pause, Resume, Complete, Block, Clear
};
struct GoalChange {
    GoalOperation               operation = GoalOperation::Create;
    std::optional<GoalSnapshot> goal;      // present for non-Clear
    std::optional<GoalRef>      cleared;   // present for Clear
    std::uint32_t               rounds_started = 0;   // non-Clear
};

// ymh-local job registry change (owner-scoped; no dsh session-event analogue).
struct JobChanged {
    std::optional<AgentId> owner;   // absent when the job is unowned (§3.2)
    std::string   kind;
    std::uint64_t ordinal = 0;
    JobStatus     status = JobStatus::Running;
    std::string   label;
};
```

---

## 6. Events, codecs, and consumers

### 6.1 New `EventType` values

Four durable types are added after `AgentPresetSelected`
(`include/ymh/core/event.hpp:76`), before the live-only
`McpServerStatusChanged` (`:79`):

| EventType | `wire_name` | Payload | Notes |
|---|---|---|---|
| `CommandRun` | `command/run` | `payload::CommandRun` | log-only, standalone append |
| `CommandDone` | `command/done` | `payload::CommandDone` | log-only, paired by `command_id` |
| `GoalChange` | `goal/change` | `payload::GoalChange` | CAS on `{id,revision}` |
| `JobChanged` | `job/changed` | `payload::JobChanged` | owner-scoped |

`26 §4.3.9:845-848` pins these names. The `01` errata must extend
`wire_name`/`parse_event_type`/`all_event_types`
(`include/ymh/core/event.hpp:84,87,91`), add the `SessionEventMap`
specializations (`:129`), add the `EventTraits` specializations (`:140`), and
add the payload `to_json`/`from_json` bodies.

### 6.2 Pinned JSON key schemas

Copied verbatim from `26 §4.3.9.1:955-959` (snake_case; enums lowercase;
optionals omitted when unset):

| Payload | JSON object keys |
|---|---|
| `payload::CommandRun` | `command_id` int, `name` string, `args` string (omitted when `record_input=false`), `source` string (`"user"｜"agent"`) |
| `payload::CommandDone` | `command_id` int, `kind` string (`"success"｜"error"`), `text` string (omitted), `source_event_seq` int (omitted) |
| `payload::GoalChange` | `operation` string (`"create"｜"edit"｜"pause"｜"resume"｜"complete"｜"block"｜"clear"`), `goal` `GoalSnapshot` object (omitted on clear), `cleared` object `{id int, revision int}` (omitted on non-clear), `rounds_started` int |
| `GoalSnapshot` (nested) | `id` int, `revision` int, `objective` string, `phase` string (`"active"｜"paused"｜"blocked"｜"complete"`), `blocked` object `{code string, message string}` (omitted unless blocked), `max_goal_rounds` int |
| `payload::JobChanged` | `owner` string (omitted when the job is unowned), `kind` string, `ordinal` int, `status` string (`"running"｜"stopping"｜"completed"｜"killed"｜"failed"`), `label` string |

`GoalChange` carries **no** `created_at`/`updated_at`/`version`: the envelope
already carries `timestamp`/`event_id` (`26 §4.3.9.1:936-944`), and
`GoalProjection.created_at`/`updated_at` derive from the create/latest
`goal/change` envelope timestamps. dsh's `version: 1` is redundant with
`kProtocolVersion`/`kSchemaVersion` and is not copied.

### 6.3 Consumer matrix

Per `26 §4.3.9.2:992-995` and `29-I2` (`:406-408`):

| EventType | `deriveMessages` projection | transcript / UI | token |
|---|---|---|---|
| `CommandRun` | ignore (log-only) | command row | excluded |
| `CommandDone` | ignore (log-only) | command row | excluded |
| `GoalChange` | ignore | goal row | excluded |
| `JobChanged` | ignore | job row | excluded |

Rules this spec must satisfy:

1. **Build gate.** The projection switch (`src/session/session.cpp:385`) has no
   `default:`; every new durable type gets an explicit `case` there. The other
   four consumer switches (`src/ui/ui_event_adapter.cpp:60`; 
   `src/ui/session_export.cpp:200`; `src/cli/session_cli.cpp:91`;
   `src/cli/headless.cpp:203`) have a `default:` and would silently drop; each
   gets a recorded explicit case (`29 §4.3`, `:356-366`).
2. **No silent drops.** All four are user-visible (`29-event-family-errata.md:363-366`);
   the UI must render them explicitly or record why not.
3. **Version neutrality.** No `kProtocolVersion`/`kSchemaVersion` bump
   (`29-I6`, `:417-419`).

### 6.4 Codec totality

`all_event_types()` must enumerate the four new values; the round-trip test at
`tests/unit/event_test.cpp` iterates it (`29-I1`, `:400-405`). An older binary
cannot read a newer session DB (`parse_event_type` rejects unknown strings
loudly, `include/ymh/core/event.hpp:86`), which is the intended durable fence
(`26 §4.6`); the **wire** receiver still skips an unknown type per `26-D24`.

---

## 7. Invariants

These extend `26-I1`..`26-I12`, `36-I1`..`36-I12`, and `42-I1`..`42-I12`.
`44-I*` are local to this spec.

- **44-I1 (the log is the only goal truth).** Goal state is a pure projection
  over `goal/change` + goal-sourced `user/message` events. No goal field is
  added to `SessionHeader` and no goal column is added to SQLite. Guard: the
  projection function is total and pure; `SessionHeader`
  (`include/ymh/session/session.hpp:44-63`) is unchanged (44-F1, §13 OQ-1).
- **44-I2 (compare-and-set).** Every durable mutation increments `revision`;
  a mutation whose `GoalRef.revision` is not the current one is rejected and
  appends nothing. Guard: the `prepare_mutation` revision check (44-F2).
- **44-I3 (one current goal).** At most one current goal per session; `create`
  is legal only when none exists or the current phase is `Complete`; a cleared
  goal's id is never reused. Guard: the projection's `seen_goal_ids` and the
  `AlreadyExists`/`NotFound` fences (44-F3).
- **44-I4 (contiguous admitted rounds).** An admitted round is exactly the next
  round of the current revision (`round == rounds_started + 1`) and never
  exceeds `max_goal_rounds`; `rounds_started` is the highest admitted round.
  Guard: the strict projection rejects any other goal-sourced `user/message`
  (44-F4).
- **44-I5 (cap blocks).** Reaching `max_goal_rounds` blocks the goal with
  `code == "round-limit"`; the driver never admits round `max + 1`. Guard:
  the driver's pre-check (44-F5).
- **44-I6 (activation is process-local).** `GoalActivation` is never persisted;
  session start (resume/fork/attach/create) sets `Disarmed`; only `create`/
  `resume` arm. Guard: no codec field exists for activation; the driver reads
  only live state (44-F6).
- **44-I7 (fail-closed admission).** A queued round is admitted only if it
  still owns the exact live revision, `phase == Active`, `activation == Armed`,
  and `round == rounds_started + 1`; otherwise it is rejected, and a rejection
  while the goal is live blocks with `code == "prompt-rejected"`. Guard: the
  `pre-step` re-check (44-F7).
- **44-I8 (owner fence).** A job is visible and controllable only by its owner
  (or any caller when unowned); `get`/`read`/`kill`/`wait` on a foreign or
  unknown job throw. Guard: the caller-vs-owner check on every accessor
  (44-F8).
- **44-I9 (first-wins settlement).** A job reaches exactly one terminal status;
  a late producer outcome cannot change it; completion is announced last, after
  the record is committed. Guard: the settlement latch (44-F9).
- **44-I10 (reported suppresses duplicates).** A terminal read/kill/teardown
  marks `reported`; completion reporters suppress a redundant notice. Guard:
  the `reported` flag (44-F10).
- **44-I11 (bounded wake chain).** `jobs.max_consecutive_wakes` bounds
  wake-opened turns; only user-authored input resets it. Guard: the per-owner
  wake counter, keyed on `MessageSource::Kind::User` (44-F11).
- **44-I12 (commands are log-only).** `command/run`/`command/done` never enter
  `deriveMessages()` output or the token estimate; they are appended
  standalone. Guard: the explicit `ignore` projection case and the token
  accounting exclusion (44-F12).
- **44-I13 (command pairing).** Every `command/run` has at most one
  `command/done`, correlated by `command_id`; a handler that throws still
  settles with `kind == "error"`. Guard: the invoke wrapper's single settle
  point (44-F13).
- **44-I14 (scope shadowing).** Resolution is nearest-scope-wins by name; a
  duplicate within one scope fails loud. Guard: `add`'s scope check and the
  chain walk (44-F14).
- **44-I15 (UI is a consumer).** No goal/job/command type enters the UI layer,
  and no UI type enters these seams (`26-I10`, `42-I11`). The UI reads
  `goal/change`/`job/changed`/`command/*` rows and calls `invoke` through the
  host. Guard: `00 §4.1`, `06 A17` (44-F15).
- **44-I16 (no schema/protocol bump).** `kSchemaVersion` and
  `kProtocolVersion` are unchanged (`29-I6`, `26-I12`). Guard: the DDL and the
  wire envelope are byte-identical.
- **44-I17 (single-threaded ownership).** The goal service, the round driver,
  the job registry, and the command registry are owned by `WorkspaceRuntime`
  and called only on the agent executor thread, inheriting `36`'s contract
  (`36-prompt-registry.md:153-154`). No lock is provided (44-F16).

---

## 8. Failure modes

### 8.1 Shared findings (`F1`..`F12`, `§54`)

This spec is subject to the project's shared failure taxonomy
(`00-architecture.md` §54). The relevant shared findings are: F1 (silent
degradation), F2 (state divergence between two sources of truth), F5 (unsafe
path resolution), F7 (unbounded growth), F9 (cross-session leakage), F10
(unauthorized action), and F11 (orphaned identity). Every `44-F#` below names
the shared finding it is an instance of.

### 8.2 Component-local failure modes

- **44-F1: goal state duplicated in the header.** A goal projection is written
  to `SessionHeader` in addition to the log, so the two diverge after a fork.
  **Guard:** 44-I1; no header field exists; the projection is the only reader.
  Instance of F2.
- **44-F2: lost update.** Two mutations race on the same revision and the
  later overwrites the earlier. **Guard:** 44-I2; the CAS check rejects a stale
  ref with `StaleRevision`. Instance of F2.
- **44-F3: duplicate/reused goal id.** A second `create` while a non-complete
  goal is current, or reuse of a cleared id. **Guard:** 44-I3; `AlreadyExists`
  and `seen_goal_ids`. Instance of F2.
- **44-F4: fabricated round.** A goal-sourced `user/message` that is not the
  next round of the current revision advances the counter, or a non-goal
  message is counted. **Guard:** 44-I4; the strict projection rejects it.
  Instance of F1.
- **44-F5: unbounded rounds.** The driver admits round `max + 1`. **Guard:**
  44-I5; the driver blocks at the cap. Instance of F7.
- **44-F6: activation resurrected across resume.** A resumed/forked session
  auto-continues because activation was persisted or inherited. **Guard:**
  44-I6; session start disarms; activation has no codec field. Instance of F2.
- **44-F7: stale round admitted.** A round queued against an old revision
  enters a step after an edit/pause. **Guard:** 44-I7; the `pre-step`
  fail-closed re-check. Instance of F2.
- **44-F8: foreign-job access.** A caller reads or kills another session's job
  because ids are predictable. **Guard:** 44-I8; the owner fence on every
  accessor. Instance of F9, F10.
- **44-F9: late-outcome resurrection.** A producer's late `done` flips a
  settled job. **Guard:** 44-I9; first-wins latch. Instance of F2.
- **44-F10: duplicate completion notice.** A job's completion opens two turns
  because a read and a listener both report it. **Guard:** 44-I10; `reported`.
  Instance of F1.
- **44-F11: self-exciting wake loop.** A woken turn starts the job whose
  completion wakes it again, unbounded. **Guard:** 44-I11; the wake counter
  degrades to injection after the bound. Instance of F7.
- **44-F12: command leaks into context.** `command/run`/`command/done` enter
  `deriveMessages()` or the token estimate. **Guard:** 44-I12; explicit
  `ignore` case and token exclusion. Instance of F1.
- **44-F13: orphaned/duplicated command.** A handler throws and no
  `command/done` is appended, or two are. **Guard:** 44-I13; one settle point
  that always appends exactly one `done`. Instance of F11.
- **44-F14: shadowing ambiguity.** Two commands with one name in one scope, or
  a nearer scope silently loses to a farther one. **Guard:** 44-I14; duplicate
  throws; the chain walk is nearest-first. Instance of F1.
- **44-F15: UI/domain type leak.** A `GoalSnapshot` or `JobSnapshot` appears in
  a UI header, or a UI type in the service seam. **Guard:** 44-I15. Instance
  of F1.
- **44-F16: cross-thread mutation.** A goal/job/command mutation runs off the
  agent executor thread. **Guard:** 44-I17; single-threaded ownership.
  Instance of F2.

---

## 9. dsh mapping

| ymh | dsh source | Notes |
|---|---|---|
| `GoalService` | `ctx.goals` (`dsh-goal/lib/types/index.d.ts:54-...`) | `get`/`create`/`edit`/`pause`/`resume`/`complete`/`clear`/`disarm` map 1:1; `block` is the tool path |
| `GoalSnapshot`/`GoalRef` | `dsh-goal/lib/types/types.d.ts:16-56` | numeric `GoalId` (`26 §4.3.8:750`) vs dsh branded string |
| `GoalProjection`/`GoalProjectionState` | `types.d.ts:90-108` | pure replay fold; `failure` retained |
| `GoalChange` event | `GoalChangeMeta` (`domain.d.ts:12-32`) | ymh drops `version`/`createdAt`/`updatedAt` (envelope carries them) |
| `GoalActivation` | `types.d.ts:57-58,74-83` | process-local, never persisted |
| `GoalRoundDriver` | `dsh-goal-round-driver` (`lib/types/index.d.ts`) | round prompt verbatim (`lib/index.js:12-19`); fail-closed fences |
| `GoalMessageRef` / `MessageSource::Kind::Goal` | `GoalMessageSource` (`domain.d.ts:33-40`) | additive to `36`/`37` |
| goal tools `get_goal`/`create_goal`/`update_goal` | `dsh-tool-goal` | `tool:goal` order 2400 (`include/ymh/prompt/order.hpp:36`) |
| `/goal` | `dsh-command-goal` (`lib/index.js:10-24,176-185`) | Codex-shaped grammar, verbatim |
| `JobRegistry`/`JobSnapshot`/`JobHooks`/`JobOutcome` | `dsh-jobs` (`types.d.ts`, `index.d.ts`) | one registry per workspace runtime; owner-relative |
| job tools `job_output`/`job_list`/`job_kill` | `dsh-tool-jobs` | `tool:jobs` order 1600 (`order.hpp:30`) |
| wakeup policy | `dsh-tool-jobs` `Config.completionDelivery`/`maxConsecutiveWakes` | `wakeup` default; bound 3 (`26 §4.9:1328-1329`) |
| `CommandRegistry` (host) | `ctx.commands` (`dsh-commands`) | agent-scoped shadowing; log-only events |
| `command/run`/`command/done` | `dsh-commands/lib/types/types.d.ts:99-117` | ymh wire names underscore; pairing by `commandId` |
| `CommandSource` `User`/`Agent` | dsh `CommandSourceMap` has only `user` (`types.d.ts:63-75`) | ymh adds `Agent` for agent-scoped shadowing (D20) |
| `ui::CommandRegistry` (existing) | (no dsh analogue) | supervisor-local, log-free; preserved (§4.2) |
| job durability `job/changed` | (no dsh session-event analogue) | ymh adds it (`26 §4.3.9:848`) — see §13 OQ-3 |

---

## 10. Dependencies

Wave 6 has hard spec dependencies and cross-spec contracts that must be pinned
by errata before any Wave-6 code, per the Stage-B gate
(`26-dsh-alignment-part2.md:1395`) and the two-gate rule (`AGENTS.md`).

### 10.1 Blocking dependencies (must be `verified` before code)

1. **`36-prompt-registry.md`** — verified (`DESIGN_STATUS.md:55`). Required for
   the prompt/context + provenance seam. Wave 6 activates the reserved
   `ToolGoal`/`ToolJobs` orders (`include/ymh/prompt/order.hpp:30,36`).
2. **`42-agent-presets.md` + `43-wave5-dependency-errata.md`** — verified
   (`DESIGN_STATUS.md:64-65`). Required for the subagent producer, the scope
   chain, and the `AgentContext` leaf the command registry scopes by.
3. **`14-pty-capability.md`** — verified (`DESIGN_STATUS.md:22`). Required for
   the PTY job producer.
4. **`29-event-family-errata.md`** — verified. Its §4.4 reserved rows
   (`:391-394`) are the four Wave-6 events; the `01` errata inherits its rules.

### 10.2 Cross-spec contracts this spec requires (not yet pinned)

These are additive errata that Wave-6 code needs. Each must be pinned by its
owning spec before code:

- **`01` errata — the four event types.** `EventType`/`wire_name`/
  `parse_event_type`/`all_event_types` (`include/ymh/core/event.hpp:51,84,87,91`),
  the `SessionEventMap`/`EventTraits` specializations (`:129,140`), the payload
  codecs, and the five consumer switches (§6.3). **Owning spec: `01`.**
- **`36`/`37` errata — `MessageSource::Kind::Goal`** (§5.4). Additive; the
  `GoalMessageRef` payload and its JSON keys. **Owning specs: `36`, `37`.**
- **`21` errata — the config keys** `goals.max_rounds` (256),
  `goals.blocked_after_consecutive_rounds` (3), `jobs.wait_timeout_ms`
  (30000), `jobs.max_wait_timeout_ms` (600000), `jobs.completion_delivery`
  (`wakeup`), `jobs.max_consecutive_wakes` (3) (`26 §4.9:1324-1329`).
  **Owning spec: `21`.**
- **`06` errata — the driver's loop hooks.** The `turn/end` observation, the
  no-initiator `followup`, the `pre-step` fail-closed re-check, and
  owner-disposal job cleanup. **Owning spec: `06`.**
- **`07` errata — the six new tools** (three goal, three job) into the shared
  tool registry, with their schemas and permission classes. **Owning spec: `07`.**
- **`05` errata (open) — the durable-command wire method.** A supervisor-side
  `/goal` must reach the host-side `CommandRegistry`. §13 OQ-2 records whether
  this is a new additive method (e.g. `command.invoke`) or a reuse of
  `agent.*`; **owning spec: `05`.**

### 10.3 Implementation order (within Wave 6)

`01` events + projection → `GoalService` + projection → round driver →
`GoalRoundDriver` wiring + goal tools → `/goal` → `JobRegistry` + `job/changed`
→ job tools + wakeup → command registry + `command/run`/`done` → UI rows.

---

## 11. Decision register

| ID | Decision | Class | Rationale |
|---|---|---|---|
| 44-D1 | **Goal state is a pure log projection, not a session-header field.** No `SessionHeader` goal field; no SQLite column. | New | `26-I1`, `01 §6.3` (`:726`); dsh `GoalProjection` (`types.d.ts:90-99`). Resolves §13 OQ-1. |
| 44-D2 | **`GoalChange` carries the whole snapshot (non-clear) or a `GoalRef` tombstone (clear), CAS on `{id,revision}`.** | New | `26 §4.3.9:847`, `:957`; dsh `domain.d.ts:12-32`. |
| 44-D3 | **Admitted rounds are goal-sourced `user/message` events; `rounds_started` is derived strictly and contiguously.** | New | dsh `index.js:273-278`; `26 §4.3.9.1:957`. |
| 44-D4 | **Activation is process-local and never persisted; session start disarms; create/resume arm.** | New | dsh `index.js:594,648,698`; `26 §5 Wave 6:1498`. |
| 44-D5 | **The round driver blocks with `round-limit` at the cap and `prompt-rejected`/`queue-failed` on failure.** | New | dsh `index.js:125-131,150-158,293-306`. |
| 44-D6 | **`/goal` grammar is `[<objective>|clear|edit <objective>|pause|resume]`; `complete`/`block` are tool verbs only.** | New | dsh `dsh-command-goal/lib/index.js:10-24`; `26-D18`. |
| 44-D7 | **Goal tools `get_goal`/`create_goal`/`update_goal` are in Wave 6**, section order `tool:goal` 2400, block gated by `blocked_after_consecutive_rounds`. | New | `include/ymh/prompt/order.hpp:36`; dsh-tool-goal; `26 §4.9:1325`. |
| 44-D8 | **`MessageSource::Kind::Goal` is added**; `is_blank` keys on the event type (`42-D4`, unchanged), the wake-counter reset keys on `Kind::User`. | Add. (`36`,`37`) | dsh `GoalMessageSource` (`domain.d.ts:33-40`); `42-D4`; §3.5. |
| 44-D9 | **The job registry is owner-scoped; ids are predictable; authorization is the fence.** | New | `26-I8` (`:83-85`); dsh `index.d.ts:30-31`. |
| 44-D10 | **`job/changed` is the durable job event; every registered job (owned or unowned) emits it, so registry state is process-local and re-derivable from the log.** | Add. (`01`) | `26 §4.3.9:848`; §3.2; §13 OQ-3. |
| 44-D11 | **Settlement is first-wins; `reported` suppresses redundant notices; owner disposal marks `reported`.** | New | dsh `index.d.ts:24-36`. |
| 44-D12 | **Wakeup defaults to `wakeup`, bounded by `max_consecutive_wakes` (3), reset only by user-authored input.** | New | dsh-tool-jobs `Config`; `26 §4.9:1328-1329`. |
| 44-D13 | **PTY and subagent are the two Wave-6 job producers.** | New | `14 §7.5` (`:1316`); `include/ymh/agent/subagent.hpp:24`. |
| 44-D14 | **The log-only command surface is a distinct host-side `CommandRegistry`; the existing `ui::CommandRegistry` is preserved for pure-presentation commands.** | New | `include/ymh/ui/command_registry.hpp:3-5`; `26-D20`. |
| 44-D15 | **`command/run`/`command/done` are durable, paired by `command_id`, and never model-visible.** | New | dsh `types.d.ts:87-117`; `26 §4.3.9.2:992-993`. |
| 44-D16 | **`CommandSource` adds `Agent`** to dsh's `user` for agent-initiated invocation. | Add. (`01`) | `26 §4.3.9:896`; dsh `types.d.ts:63-75`. |
| 44-D17 | **Agent-scoped shadowing is nearest-scope-wins; a duplicate within one scope fails loud.** | New | `26-D20`; `42 §2.2`; `26-I7`. |
| 44-D18 | **`record_input = false` omits `command/run.args`** when an authoritative domain event owns the payload. | New | dsh `types.d.ts:90-97`; `26 §4.3.9.1:955`. |
| 44-D19 | **No `kSchemaVersion`/`kProtocolVersion` bump**; four additive `EventType` values. | Add. | `29-I6` (`:417-419`); `26-I12`. |
| 44-D20 | **The goal service, round driver, job registry, and command registry are single-threaded, owned by `WorkspaceRuntime`.** | New | `36-prompt-registry.md:153-154`; `42-I12`. |

---

## 12. Test plan

The suite follows §44 (unit, integration with fakes, golden TUI render, replay)
and the `26 §5.2` additions. Wave-6 tests use `FakeLLM` and a fake job producer;
no live LLM is required.

### 12.1 Unit tests

1. **Projection.** Fold a `goal/change` create → `rounds_started` advances on
   each contiguous goal-sourced `user/message`; a non-contiguous, stale-revision,
   or over-cap message sets `failure` (44-I3/I4, 44-F4).
2. **CAS.** `edit`/`pause`/`resume`/`complete`/`block`/`clear` with a stale
   `GoalRef` throws `StaleRevision` and appends nothing; each successful
   mutation increments `revision` (44-I2, 44-F2).
3. **Transitions.** The §2.2 table: `resume` on `Complete` throws;
   `resume` on an exhausted goal throws; `create` on a non-complete current
   goal throws `AlreadyExists` (44-I3, 44-F3).
4. **Block reason.** A non-kebab `code` or empty `message` throws
   `InvalidBlockReason` (§2.2; dsh `index.js:471-483`).
5. **Activation.** Session start → `Disarmed`; `create`/`resume` → `Armed`;
   `pause`/`complete`/`block`/`clear` → `Disarmed`; activation is absent from
   every codec (44-I6, 44-F6).
6. **Driver.** On `turn/end` with an armed active goal, exactly one goal-sourced
   `user/message` is enqueued; at the cap, a `round-limit` block is appended and
   no round (44-I5, 44-F5).
7. **Fail-closed admission.** Mutate the goal between queue and `pre-step`; the
   round is rejected and a live goal blocks with `prompt-rejected`
   (44-I7, 44-F7).
8. **Job fence.** A foreign caller's `get`/`read`/`kill`/`wait` throws; an
   unowned job is visible to all; `list` never shows a foreign label
   (44-I8, 44-F8).
9. **First-wins.** A producer that settles late cannot flip a terminal job; a
   second `done` is ignored (44-I9, 44-F9).
10. **Wake bound.** `max_consecutive_wakes` wake-opened turns, then injection;
    a user-authored message resets the counter; a goal round does **not**
    (44-I11, 44-F11).
11. **Command pairing.** `invoke` appends exactly one `run` and one `done`; a
    throwing handler yields `kind == "error"` (44-I13, 44-F13).
12. **Shadowing.** A nearer-scope command shadows a farther one; a duplicate in
    one scope throws (44-I14, 44-F14).
13. **`record_input=false`.** `command/run.args` is omitted; the richer event
    carries the input (44-D18).
14. **Config.** Each `goals.*`/`jobs.*` key parses with its pinned default; an
    out-of-range value fails config load (44-D12, §10.2).

### 12.2 Integration tests (FakeLLM)

1. **Goal completion.** Create a goal, drive rounds with `FakeLLM` that calls
   `update_goal{complete}`; assert the `goal/change` sequence and that the
   driver stops (44-I5).
2. **Goal disarm across resume/fork.** Arm and run a round; resume and fork the
   session; assert the durable phase/revision survive, `activation` is
   `Disarmed`, and no round is enqueued until `/goal resume` (44-I6, 44-F6;
   `26 §5 Wave 6:1498`).
3. **Job completion wakes an idle owner.** Start a subagent job; on settlement,
   assert one wake turn under `wakeup` and none under `quiet` (44-D12).
4. **PTY producer.** A persistent `terminal` job's `read_output` drains the
   ring; `job_kill` terminates the PTY (`14 §7.5`; 44-D13).
5. **Owner disposal.** Dispose the owning agent; its live jobs cancel, settle,
   and are marked `reported` (44-D11, 44-F10).
6. **`/goal` round trip.** Type `/goal <objective>`; assert exactly one
   `command/run` + `command/done` and one `goal/change{create}` in order, and
   the rendered outcome (44-D6, 44-D15).
7. **Agent-scoped shadowing end to end.** Register a global and an agent-scoped
   command with one name; assert the agent invokes the nearer one and the log
   records the resolved name (44-I14).

### 12.3 Golden and replay tests

1. **`deriveMessages` unchanged.** A session with goal rounds, job changes, and
   command pairs projects the same message list as one without them
   (44-I12, 44-F12).
2. **Consumer rows.** Golden UI render for a goal row, a job row, and a command
   row; each of the four new types has an explicit case in all five switches
   (§6.3).
3. **Codec round trip.** `all_event_types()` covers the four new values;
   `encode`/`decode` round-trips each payload (`29-I1`).
4. **Replay.** Replaying a goal session reconstructs the same
   `GoalProjection` and the same `rounds_started` (44-I1, 44-I4).
5. **Compatibility.** A pre-Wave-6 DB opens under the exact-match rule; an
   unknown event type fails loud in the on-disk decode; the wire receiver skips
   it (`26-D24`; `29-I3`).

### 12.4 Failure-mode coverage matrix

| Failure | Test |
|---|---|
| 44-F1 | 12.3.4 |
| 44-F2 | 12.1.2 |
| 44-F3 | 12.1.3 |
| 44-F4 | 12.1.1 |
| 44-F5 | 12.1.6 |
| 44-F6 | 12.2.2 |
| 44-F7 | 12.1.7 |
| 44-F8 | 12.1.8 |
| 44-F9 | 12.1.9 |
| 44-F10 | 12.2.5 |
| 44-F11 | 12.1.10 |
| 44-F12 | 12.3.1 |
| 44-F13 | 12.1.11 |
| 44-F14 | 12.1.12, 12.2.7 |
| 44-F15 | 12.3.2 |
| 44-F16 | (TSan run over the four registries, per `26 §5.2` item 4) |

---

## 13. Open questions

These are disagreements or gaps Rev 1 could not resolve without touching a
verified spec or a user decision. They are recorded, not silently resolved;
OQ-7 is the one closed by a user decision.

- **OQ-1 — the "goal projection in the session header" claim contradicts the
  dsh design.** `26-dsh-alignment-part2.md` §4.4 (`:1088`) and
  `30-architecture-cascade-errata.md` §S3 (`:164-180`) both state that
  `23-session-lifecycle-errata.md` must change because "the session header gains
  a preset id **and a goal projection**." The dsh goal domain derives its
  projection purely from the session log (`dsh-goal/lib/types/types.d.ts:90-108`),
  its activation is explicitly process-local and never persisted
  (`types.d.ts:81-82`), and `26-D18` itself names only `01` (not `23`) as the
  second owning spec (`:156`). The tree's `SessionHeader`
  (`include/ymh/session/session.hpp:44-63`) already carries `agent_preset`/`depth`
  but **no** goal field. **Rev 1 pins 44-D1 (log projection, no header field).**
  If the `23` header change is genuinely required, it needs a `23` errata and a
  user decision; it is not silently dropped.
- **OQ-2 — the durable-command wire path.** The existing command surface is
  supervisor-local and log-free (`include/ymh/ui/command_registry.hpp:3-5`),
  while `/goal` must append durable state. `26-D20` names only `44` and `01` as
  owners; it does not name `05`. A supervisor-side `/goal` therefore needs an
  additive `05` method (e.g. `command.invoke`) or a reuse of the existing
  `agent.*` methods (`include/ymh/transport/protocol.hpp:516-521`). **Rev 1
  assumes a new additive `05` method; the exact name and payload are open.**
- **OQ-3 — durable jobs vs process-local registrations.** dsh's job
  registrations are process-local and do not survive process exit
  (`dsh-jobs/lib/types/index.d.ts:24-29`), but `26-D19` adds a durable
  `job/changed` event (`:848`) so the registry state is re-derivable. What a
  resumed session should show for a job that was `Running` when the daemon
  died (a tombstoned `Failed` row, a stale `Running` row, or no row) is a
  product decision. **Rev 1 pins the event and the projection but leaves the
  resume rendering open.**
- **OQ-4 — subagent jobs vs `42-D16`.** `42-D16` defers continuable subagents
  and control tools (`42-agent-presets.md:1168`). Wave 6 uses the existing
  one-shot `SubagentRunner` (`include/ymh/agent/subagent.hpp:17-24`) as a job
  producer; a `job_output` read of a subagent job returns its final summary
  only. If continuable subagents are wanted in Wave 6, `42-D16` must be
  revisited.
- **OQ-5 — goal-round UI attribution.** A goal round is a `user/message` with
  `MessageSource::Kind::Goal` (§5.4). Whether the transcript renders it as a
  user row, a distinct goal row, or not at all is a `10`/`17` UI decision.
  **Rev 1 pins only that it is not user-authored for the wake-counter reset**
  (§3.5); for `is_blank` it is ordinary session content (§5.4). The row kind is
  open.
- **OQ-6 — `goals.max_rounds` vs a per-goal cap.** `26 §4.9` pins
  `goals.max_rounds` (default 256) as the default when `create` omits a cap,
  and `GoalSnapshot.max_goal_rounds` carries the resolved per-goal cap
  (`26 §4.3.8:778`). Whether a config change mid-session re-caps existing goals
  is open; Rev 1 resolves the cap at `create`/`edit` time only.
- **OQ-7 — Wave-6 scope (resolved by user decision).**
  `26-dsh-alignment-part2.md` §6 OQ7 (`:1617-1620`) recommended shipping Waves 0
  through 4 first and deciding Waves 5 and 6 afterward. The user decided to keep
  Wave 6 in scope; this spec is that decision's artifact. Recorded so the
  deferral recommendation is not left unaddressed.

---

## 14. References

- `26-dsh-alignment.md` — §1.2 (`:61-63`), §2.4 (`:987`).
- `26-dsh-alignment-part2.md` — verified Rev 7, GATE PASS: `26-D18/D19/D20`
  (`:156-158`), §4.1 `26-I8`/`I10`/`I11` (`:83-98`), §4.3.8 (`:774-799`),
  §4.3.9 (`:845-848`), §4.3.9.1 (`:955-959`), §4.3.9.2 (`:992-995`), §4.4
  (`:1082-1088`), §4.9 (`:1324-1329`), §5 Stage B (`:1395`), §5 Wave 6
  (`:1494-1501`), §6 OQ7 (`:1617-1620`).
- `36-prompt-registry.md` — `AssembleContext.scope` (`:118-121`),
  `PromptSection` (`:123-128`), §2.1 (`:104-110`), §3.1 `MessageSource`
  (`:584-592`), single-threaded (`:153-154`).
- `42-agent-presets.md` — §3.1 `GoalId`/`AgentContext` (`:462-471`),
  `42-D16` (`:1168`), `42-I12` (`:989-993`).
- `43-wave5-dependency-errata.md` — verified per `DESIGN_STATUS.md:65`.
- `14-pty-capability.md` — §3.1 `PtySession` (`:299-334`), §7.5 (`:1316-1332`),
  §8.1 (`:1338-1345`).
- `01-session.md` — §4.2 `Event` (`:220-226`), §4.3 `EventType` (`:246`),
  §6.1 append (`:695-706`), §6.3 `deriveMessages` (`:726`).
- `29-event-family-errata.md` — §4.4 reserved rows (`:391-394`), §4.3 rules
  (`:356-366`), `29-I1`..`I6` (`:400-419`).
- `30-architecture-cascade-errata.md` — §S3 (`:164-180`), §S4 (`:185-205`).
- `DESIGN_STATUS.md` — spec 36 (`:55`), 42 (`:64`), 43 (`:65`), 14 (`:22`).
- Working tree: `include/ymh/core/event.hpp:51-91,129,140`;
  `include/ymh/session/events.hpp:89-93`;
  `include/ymh/session/session.hpp:44-63`;
  `include/ymh/session/session_manager.hpp:49-52`;
  `src/session/session_manager.cpp:43-49`;
  `src/session/session.cpp:372,385`; `src/ui/ui_event_adapter.cpp:60`;
  `src/ui/session_export.cpp:200`; `src/cli/session_cli.cpp:91`;
  `src/cli/headless.cpp:203`; `include/ymh/agent/agent.hpp:30-150`;
  `include/ymh/agent/agent_registry.hpp:27-66`;
  `include/ymh/agent/subagent.hpp:17-24`;
  `include/ymh/agent/preset.hpp:30-40,148-189`;
  `include/ymh/agent/agent_loop.hpp:50`;
  `include/ymh/execution/pty.hpp:336-376`;
  `include/ymh/tools/tool.hpp:25,45,58,63,66`;
  `include/ymh/tools/tool_registry.hpp:25,87,90`;
  `include/ymh/tools/tool_context.hpp:22,36,39`;
  `include/ymh/ui/command_registry.hpp:3-5,15-57`;
  `src/ui/command_registry.cpp:104,126,129-260`;
  `include/ymh/prompt/order.hpp:30,36`;
  `include/ymh/transport/protocol.hpp:516-521`.
- dsh packages: `dsh-goal` (`lib/types/{types,domain,runtime,index}.d.ts`,
  `lib/index.js`), `dsh-goal-round-driver` (`lib/types/{index,prompt}.d.ts`,
  `lib/index.js`), `dsh-tool-goal`, `dsh-command-goal`, `dsh-jobs`
  (`lib/types/{types,index}.d.ts`), `dsh-tool-jobs`, `dsh-commands`
  (`lib/types/{types,index}.d.ts`).

---

## 15. Revision log

- **Rev 2** — gate-fix rev, resolving the Rev 1 independent gate (2 MEDIUM /
  3 LOW). **MEDIUM:** §5.4 no longer asks `is_blank` to key on
  `MessageSource::Kind::User`; it keeps the event-type discriminator of `42-D4`
  and the shipped `is_blank`, so the unowned cross-spec change is gone (only the
  wake-counter reset discriminates on `Kind::User`). `JobChanged.owner` is now
  `std::optional<AgentId>`, omitted when the job is unowned, so unowned jobs
  stay re-derivable (44-D10). **LOW:** the `GoalId` citation is corrected to
  `42-agent-presets.md:462`; §1.1 cites the deferral recommendation as
  `26 §6 OQ7 (:1617-1620)` and §13 OQ-7 records the user's keep-decision; the
  phantom `agent/session-start` is replaced by `session/start`
  (`EventType::SessionStarted`) and the resume/fork/attach disarm seam is mapped
  to the `SessionManager` resident path.
- **Rev 1** — initial authoring. Pins the goal domain (`26-D18`), the job
  registry (`26-D19`), and the log-only command surface (`26-D20`) for Wave 6.
  Records six open questions (OQ-1..OQ-6); OQ-1 is a genuine contradiction
  between the dsh design and `26 §4.4:1088`/`30 §S3:164-180` over whether the
  session header gains a goal projection. Design only; no implementation code.
