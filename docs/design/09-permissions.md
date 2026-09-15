# 09 — Permissions & Policy

**Component 09 of 10.** This spec owns the **policy layer** that sits between the
agent loop and execution (`§19`, `§46`, `§54 D7`): the `PermissionPolicy` that
classifies a tool call as `Allow` / `Ask` / `Deny`, the rule model and its
precedence, the **decision flow over transport** (how an `Ask` becomes a durable
`PermissionDecision`), and the **background permission policy** (F2) that keeps a
background session from deadlocking invisibly (`§9.9`).

This document follows `00-architecture.md` (cited inline as `§n`),
`01-session.md` (`01 §n`), `02-persistence.md` (`02 §n`),
`03-workspace-registry.md` (`03 §n`), `04-workspace-host-daemon.md`
(`04 §n`), `05-transport.md` (`05 §n`), `06-agent-loop.md` (`06 §n`),
`07-tools-execution.md` (`07 §n`), and `08-llm-provider.md` (`08 §n`); where it
cannot follow them it records the conflict under §12 rather than choosing
silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

> **Naming note.** The invariants in this spec are numbered `Q1`, `Q2`, … (for
> "policy"), so they cannot collide with the `D1`–`D23` design decisions in
> `00-architecture.md` §54 nor with any other component's invariant namespace
> (`S#`/`I#` in 01, `P#` in 02, `R#` in 03, `H#` in 04, `T#` in 05, `A#` in 06,
> `X#` in 07, `L#` in 08). Component-local failure modes are `Q-F#`, disjoint
> from `P-F#` (02), `R-F#` (03), `D-F#` (04), `T-F#` (05), `A-F#` (06), `E-F#`
> (07), and `L-F#` (08). Architecture decisions are always written with the `§54`
> prefix (`§54 D7`); this spec's invariants are written bare (`Q3`). **Caveat:**
> `00-architecture.md` itself uses bare `Q3`/`Q6`/`Q7`/`Q8` for its *own* internal
> open questions (§9.9, §20.22), so this spec's `Q#` is disjoint from every
> *component* namespace above but **collides textually** with `00`'s internal
> question labels; a bare `Qn` here always means 09's invariant, and architecture
> references always carry the `§` prefix (`§19`).

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
   LLM output (08)
        │
        ▼
   Tool parser → payload::ToolCall (07 §4, 08 §4.2)
        │
        ▼
   ┌──────────────────────────────────────────────────────────────────────┐
   │ AgentLoop (06 §5.1)                                                    │
   │   append payload::ToolCall                          (durable, X7)      │
   │   verdict := PermissionPolicy::evaluate(request)    (this spec, Q1)    │
   │   if Ask: PermissionBroker::resolve(request)        (this spec)        │
   │   append payload::PermissionDecision{call, kind}    (durable, 01 §4.5) │
   │   if granted: ToolRegistry::execute (07)  else: ToolResult{Denied}     │
   └──────────────────────────────────────────────────────────────────────┘
        │                                        ▲
        │ permission.request (notification)     │ permission.decide (request)
        ▼                                        │
   TransportServer (05 §7.6) ── broadcast ──► attached supervisors (10)
```

The permission layer is **mandatory**: the chain is
`LLM → tool parser → permission policy → execution environment → OS`, and the
`LLM → shell → OS` shortcut is never legal (`§46`). This is `§54 D7`: the policy
sits between the agent and execution.

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 09)

- `PermissionPolicy`: the pure decision model (`Allow` / `Ask` / `Deny`), the
  rule model, the scope model, and rule precedence (§3).
- `PermissionRequest`: the core request type 07 deferred to this spec
  (`07 §5.4`, decision (o)) — pinned here (§3.2).
- The **decision flow over transport**: `PermissionBroker`, the
  `permission.request` / `permission.decide` round-trip, multi-supervisor
  fan-out (resolving `05` OQ-4), timeouts, and cancellation (§4).
- The **background permission policy (F2)**: surface-via-attention with a
  fail-closed timeout (§5).
- Grant persistence and scope semantics (`Once` / `Session` / `Always`) (§3.6).
- The final `PermissionHandle` shape co-owned with 07 (`07 §5.4`, `07` (o))
  (§3.7).

### 1.3 Boundaries — deferred to other specs

This table lists only concerns **fully deferred** to another spec. Cross-spec
seams (a type this spec produces/consumes at a boundary another spec owns) are
listed once, in §1.4.

| Concern | Owner | This spec's role |
|---|---|---|
| `ToolCall` / `ToolResult` payloads | 01 (`01 §4.5`) | produces the decision; never appends |
| `PermissionDecisionKind` enum | 01 (`01 §4.5`) | reuses `{Allow, Deny, AllowAlways}` verbatim |
| `permission.request` / `permission.decide` wire DTOs | 05 (`05 §7.6`) | consumes; maps to the core request (§4.3) |
| `ToolSchema.destructive`, `SandboxMode`, `resolve()` | 07 (`07 §3.2`, `§6.8`) | reads the hint and mode; never re-resolves paths |
| Path canonicalization and containment | 07 (`07 §5.1`, `§6.2`) | matches against the resolved path 07 supplies |
| RPC method catalog and profiles | 05 (`05 §6.2`, `§7.6`) | pins the permission subset (decision (m)) |

### 1.4 Seam ownership relative to 01/02/04/05/06/07/08/10

| Concern | Owner | This spec's role |
|---|---|---|
| `payload::PermissionDecision` durability | 01 | produces the record; the loop appends it |
| `ToolOutcome{Denied}` | 01 (`01 §4.5`) | the loop maps a `Deny` verdict to it (07 §8.1) |
| Session write lease | 02 | the decision append obeys the lease like any append |
| `AgentState` / activation | 04 (`04` (m)), 06 | a blocked session is surfaced, never silently dropped |
| Wire round-trip / fan-out | 05 | defines the fan-out policy (`05` OQ-4) |
| Loop call site | 06 (`06 §5.1`) | defines the verdict and the await contract |
| `PermissionHandle` seam | 07 (`07 §5.4`) | finalizes the shape (07 (o)) |
| Provider tool calls | 08 | classifies what the provider emits, unchanged |
| Attention / waiting count / prompt | 10 | the broker supplies the request; 10 renders it |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId`, `TurnId`, `StepId`, `ToolCallId`, and `WorkspaceId` are frozen by
`01 §2.1` / `01 §4.5` and `03`; they are reproduced for reference and **not**
redefined. Policy-local types:

```cpp
namespace ymh {

// A request id is a correlation handle only (05 §7.6); it is minted by the
// broker, never by a client, and never persisted.
struct PermissionRequestId { std::string value; };   // UUIDv4

// The rule-set generation bumps whenever the effective rule set changes
// (config reload, new grant). Used for cache invalidation and golden stability.
struct PolicyGeneration { std::uint64_t value; };

} // namespace ymh
```

### 2.2 Decision taxonomy — verdict vs. recorded kind

`§19` sketches a `PermissionDecision { Allow, Deny, Ask }` enum, while `01 §4.5`
freezes a durable `PermissionDecisionKind { Allow, Deny, AllowAlways }`. These
are **two different things** and this spec keeps them distinct (decision (a)):

- **`PolicyVerdict`** is the **policy evaluation result** — what the rules say
  *before* any human is involved. It has three values: `Allow`, `Ask`, `Deny`.
  `Ask` is transient; it is never durable.
- **`PermissionDecisionKind`** is the **recorded outcome** — what was actually
  decided and is written to the session log. It has three values: `Allow`,
  `Deny`, `AllowAlways`. It has no `Ask` (an `Ask` is always resolved to a
  terminal kind before the turn proceeds, Q3).

```cpp
namespace ymh {

enum class PolicyVerdict : std::uint8_t {
    Allow,   // §19: run without asking
    Ask,     // §19: request a human decision (transient)
    Deny,    // §19: do not run
};

// Reused verbatim from 01 §4.5 — do NOT redefine:
//   enum class PermissionDecisionKind : std::uint8_t { Allow, Deny, AllowAlways };

} // namespace ymh
```

Mapping (Q3, Q13):

| Policy / human input | Recorded `PermissionDecisionKind` | Grant persisted? |
|---|---|---|
| rule `Allow` | `Allow` | no (rule already allows) |
| human `Allow`, scope `Once` | `Allow` | no |
| human `Allow`, scope `Session` | `Allow` | session memory only |
| human `Allow`, scope `Always` | `AllowAlways` | workspace-local grants file |
| rule `Deny` / human `Deny` / timeout / cancel | `Deny` | no |
| `ReadOnly` hard deny | `Deny` | no (never grantable) |

`AllowAlways` is treated as `Allow` for execution; its only difference is that it
is also recorded in the grant store so a later evaluation returns `Allow` without
asking.

### 2.3 Scope model

Three orthogonal notions of "scope" appear in the architecture; this spec pins
each so they do not blur:

1. **Match dimensions** — what a rule is *about*: tool name, a root-relative
   path glob, and (for `shell`) a command pattern. A rule may use one or more
   dimensions (§3.3).
2. **Rule layers** — where a rule is *declared*, from broadest to most specific:
   built-in defaults → global config → project/workspace config → profile
   config → command-line overrides → workspace-local grants (written by
   "Always") → session grants (in-memory). Later layers win on ties (§3.4).
   `Profile` and `CommandLine` are first-class layers in the fold, not merged
   into `Global`/`Project`. This is the "per-session / per-workspace / global"
   axis.
3. **Grant scope** — how long an `Allow` *persists* after a human decision:
   `Once`, `Session`, or `Always`. This is the wire axis `05 §7.6` freezes.

```cpp
namespace ymh {

// Mirrors protocol::PermissionScope (05 §7.6) 1:1 so the core stays
// transport-agnostic; the daemon maps the two at the RPC boundary.
enum class GrantScope : std::uint8_t { Once, Session, Always };

} // namespace ymh
```

Persistence of each grant scope (Q13):

```text
Once      nothing is persisted; the decision is only the durable
          PermissionDecision record (01 §4.5)
Session   an in-memory set in the daemon, keyed (session, tool, path-pattern);
          it does NOT survive a daemon restart (a restart re-asks — fail-safe)
Always    a durable grant appended to the workspace-local grants file
          <workspace>/.ymh/permissions.local.toml, owned by the daemon
          (one writer per workspace, §54 D18); effective for every session in the
          workspace, including sessions created later
```

`Always` is therefore **workspace-scoped** in v1; a truly **global** grant is
expressed by editing the global/project config, not by clicking "Always"
(decision (e), OQ-P1). The `Session` grant scope is named for the wire enum
(`protocol::PermissionScope`, `05 §7.6`) but is **daemon-lifetime**: 01 sessions
are durable, so a `Session` grant is lost on daemon restart and re-asks
(Q13, decision (s), OQ-P2).

---

## 3. `PermissionPolicy` (pinned)

### 3.1 Class shape

The policy is **pure**: it classifies a request and reads its own rule/grant
state. It performs no I/O, never blocks, never talks to the transport, and never
touches the TUI (`§19`: "The policy must be independent of the TUI"; `§54 D7`,
§54 D14, Q1, Q15).

```cpp
namespace ymh {

class PermissionPolicy {
public:
    virtual ~PermissionPolicy() = default;

    // Pure classification of one request. Same (request, generation) => same
    // verdict (Q5). Never blocks, never performs I/O, never emits events.
    virtual PolicyVerdict evaluate(const PermissionRequest&) const = 0;

    // Persist the grant produced by a human decision with scope != Once.
    // Idempotent: re-remembering the same (tool, path-pattern, scope) is a
    // no-op. Called by the broker after a decision, never on a hot path.
    virtual void remember(const PermissionRequest&,
                          PermissionDecisionKind,
                          GrantScope) = 0;

    // Monotone generation: bumps on any rule or grant change. A cached verdict
    // keyed on a stale generation is invalid (Q5).
    virtual PolicyGeneration generation() const noexcept = 0;

    // For replay/tests: the effective rule set, ordered, byte-stable.
    virtual const std::vector<PolicyRule>& rules() const noexcept = 0;
};

} // namespace ymh
```

Rules:

- **No transport, no clock, no I/O.** The policy cannot wait, so it cannot
  deadlock. Waiting is the broker's job (§4.2).
- **`evaluate` is total.** Every request yields a verdict; an unmatched request
  yields the configured default, never an implicit `Allow` (Q3).
- **`remember` is the only mutation.** It persists an `Always` grant or records
  a `Session` grant in memory; it never changes the outcome of an already-issued
  decision (the decision is durable before it is applied, Q2).
- A missing/unwired policy is a **startup** error, never a runtime bypass
  (`07` E-F17, Q-F1).

### 3.2 `PermissionRequest` (07 deferred it here)

`07 §5.4` intentionally left `PermissionRequest` undefined and required spec 09
to define it and reuse `PermissionDecisionKind` (`07` decision (o)). It is pinned
here as a **core** type. The wire projection is `protocol::PermissionRequest`
(`05 §7.6`), which is a strict subset (no sandbox/path internals).

```cpp
namespace ymh {

struct PermissionRequest {
    // The call being classified, verbatim from the durable log (01 §4.5).
    ToolCallId             call;
    SessionId              session;
    TurnId                 turn;
    StepId                 step;

    std::string            tool;          // == payload::ToolCall.name
    nlohmann::json         arguments;     // == payload::ToolCall.arguments

    // Supplied by 07, never recomputed here (07 §5.1): the canonical workspace
    // root and, when the tool targets a path, the resolved root-relative path.
    // The policy matches on `path`; it never calls resolve() itself (Q7).
    std::filesystem::path  root;          // canonical (07 X3)
    std::optional<std::filesystem::path> path;   // resolved, root-relative

    SandboxMode            sandbox;       // 07 §6.8 (Workspace|ReadOnly|Unrestricted)
    bool                   destructive;   // ToolSchema hint (07 §3.2)
};

} // namespace ymh
```

Rules:

- The loop constructs exactly one `PermissionRequest` per dispatched
  `payload::ToolCall` and passes it to `evaluate`; the `evaluate(call, session)`
  shorthand in `06 §5.1` / `07 §8.1` denotes this call site, not a two-argument
  signature (decision (b)).
- `path` is present only when the tool has a path dimension; a request without
  it matches only tool/command rules.
- The policy **never** resolves, canonicalizes, or stats a path; it trusts the
  07-supplied resolved path (Q7). A containment failure is 07's `PathEscape`
  and never reaches the policy (`07 §5.1`, X2/X3).

### 3.3 Rule model and configuration (`§37`)

A rule is a `(match, effect)` pair. `match` has up to three dimensions; `effect`
is a `PolicyVerdict`.

```cpp
namespace ymh {

struct PolicyRule {
    // Match dimensions. Empty means "any" for that dimension. At least one
    // dimension must be non-empty (a rule with no dimensions is a config error).
    std::string            tool;      // exact name, or a glob ("git_*", "*")
    std::string            path;      // root-relative glob ("src/**", "*.lock")
    std::string            command;   // shell command glob ("git push*")

    PolicyVerdict          effect;    // Allow | Ask | Deny

    // Provenance: which layer declared this rule (for precedence and audit).
    enum class Layer : std::uint8_t {
        Builtin, Global, Project, Profile, CommandLine, LocalGrant, SessionGrant
    } layer = Layer::Global;

    std::string            id;        // stable, human-readable rule id for `reason`
};

} // namespace ymh
```

Configuration follows the `§37` layering (`built-in defaults → global config →
project config → profile config → command-line overrides`), extended with the
workspace-local grants file and in-memory session grants (§2.3). The loader
stamps each rule's `layer` from its source, so `Profile` and `CommandLine` are
first-class layers in the precedence fold (§3.4), not merged into
`Global`/`Project`. The simple `§37` shorthand stays valid, and the structured
form is added for path/command rules (decision (f)):

```toml
[permissions]
default = "ask"          # verdict when no rule matches (never "allow" by default)
shell   = "ask"          # shorthand: { tool = "shell", effect = "ask" }
write   = "ask"          # tool-group shorthand (07 §4.3: edit_file/write_file)
read    = "allow"        # tool-group shorthand

# Structured rules; evaluated by the precedence in §3.4, not source order.
[[permissions.rule]]
tool   = "write_file"
path   = "src/**"
effect = "allow"

[[permissions.rule]]
tool    = "shell"
command = "git push*"
effect  = "ask"

[[permissions.rule]]
tool   = "shell"
command = "rm -rf *"
effect = "deny"
```

- **Tool groups.** The shorthands `read`/`write` expand to the concrete tool
  names from `07 §4.3` (`read_file`/`grep`/`glob` → `read`;
  `edit_file`/`write_file` → `write`). The expansion is fixed at load and
  recorded in `rules()` for golden stability.
- **Invalid rules fail loud at startup**, never at execution: an unknown tool
  name, a malformed glob, an unknown `effect`, or a rule with no match
  dimension is a `PolicyConfigError` (Q-F4). A missing/unreadable config file
  is a startup error, never a silent default-to-`Allow` (Q-F12).
- **Glob syntax** is a single documented dialect (OQ-P5, §3.3): `*`
  matches within one path segment, `**` matches across segments, `?` one
  character; matching is on the canonical root-relative path with `/`
  separators. For the `command` dimension the same `*`/`**`/`?` tokens apply to
  the raw command string, but `/` is **not** a segment separator and spaces are
  literal: `git push*` matches `git push origin main`, and `**` behaves as `*`
  (there are no command segments). This pins the spaces-vs-`/` question for
  OQ-P5.

**`PermissionConfig` and `PolicyConfigError` (pinned).** The tunables named
across this spec have no owning type elsewhere; 09 pins them here (cf.
`ToolConfig` in `07 §5.5`, `PersistenceConfig` in `02 §4.1`, `HostConfig` in
`04 §4.1`). The daemon constructs `PermissionConfig` at startup from the
layered config and injects it into the policy and broker; nothing reads global
state.

```cpp
namespace ymh {

struct PermissionConfig {
    // Server-side Ask deadline (§4.5). Built-in default 5 min (OQ-P6). 0
    // disables the deadline ONLY with an attached, subscribed client; a
    // detached daemon always uses a bounded deadline (§4.5, §5.2, decision (n)).
    std::chrono::milliseconds permission_timeout{5 * 60 * 1000};

    // Bound on serialized `arguments`/`summary` on the wire and in the broker's
    // summary projection (§4.3, §6.2). Over the cap the broker truncates and
    // flags it (Q-F14); the durable payload::ToolCall is never truncated.
    std::size_t               arguments_max_bytes{64u * 1024u};

    // Effective, layer-stamped rule set (the §3.4 fold input); the loader
    // expands tool-group shorthands and orders it byte-stably for `rules()`
    // and golden tests.
    std::vector<PolicyRule>   rules;

    // Verdict when no rule matches (§3.5). Built-in `Ask`; `Allow` only as an
    // explicit, recorded opt-out (Q3).
    PolicyVerdict             default_verdict{PolicyVerdict::Ask};
};

// Thrown at config load only, never at execution (Q-F4, Q-F12). Carries the
// offending config path / rule id for diagnostics; never request arguments.
class PolicyConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace ymh
```

### 3.4 Evaluation algorithm (precedence)

`evaluate` is a deterministic fold over the effective rule set. The order is
**specificity → layer → effect**, never source order (Q6).

```text
evaluate(request):
  1. HARD DENY (short-circuits all rules; Q7):
       if request.sandbox == ReadOnly and tool_is_mutating(request.tool):
           return Deny                                  (07 §6.8, decision (j))

  2. MATCH:
       matches := [ r in effective_rules
                    if r.tool matches request.tool
                    and r.path matches request.path        (when r.path != "")
                    and r.command matches request.command  (when r.command != "") ]

  3. NO MATCH:
       return config.default_verdict                    (built-in default: Ask; Q3)

  4. WINNER: pick the greatest rule under this total order:
       a. SPECIFICITY — the lexicographic key below, greatest wins:
            1. number of non-empty match dimensions (3 > 2 > 1);
            2. per present dimension, in fixed order tool → path → command:
                 - a literal beats a glob (`exact` > `glob`);
                 - literals: more literal characters wins;
                 - globs: more literal (non-`*`/`?`/`**`) characters wins, then
                   longer pattern, then greater pattern byte-order.
          An absent dimension scores 0. Two globs never tie ambiguously:
          `git_*` > `*` (one literal char vs none), and a `path`/`command`
          literal beats any glob for that dimension.
       b. LAYER — later layer wins on a tie:
          SessionGrant > LocalGrant > CommandLine > Profile > Project > Global >
          Builtin
       c. EFFECT — on a full tie, the safe effect wins:
          Deny > Ask > Allow

  5. return winner.effect
```

Properties:

- **Total and deterministic.** The specificity key of (a) is a lexicographic
  tuple over finite strings, and layer/effect are finite ordered sets; there is
  exactly one winner (Q5, Q-F5).
- **Most-specific wins.** A workspace-local `allow` for `write_file:src/**` can
  override a global `write_file = ask`, because the path dimension makes it more
  specific.
- **Fail-safe on exact ties.** When two rules are equally specific and in the
  same layer, `Deny` beats `Ask` beats `Allow`, so adding a broad `allow` rule
  can never silently widen a `deny` at the same specificity.
- **Hard deny is not overridable.** `ReadOnly` mutating calls return `Deny`
  before any rule is consulted; no grant or config can override it (Q7).

### 3.5 Hard denies and defaults

- **Built-in default.** The `[permissions] default` value applies when no rule
  matches. Its built-in value is **`ask`** (fail-closed, Q3). `default = "allow"`
  is permitted only as an explicit, config-selected opt-out and is recorded in
  the policy generation for audit.
- **`ReadOnly` hard deny.** As `07 §6.8` requires, `ReadOnly` rejects mutating
  tools at the policy layer; the loop records `PermissionDecision{Deny}` and
  `ToolResult{outcome = Denied}`, and `execute` is never called (`07` (u)).
  This verdict precedes and cannot be overridden by rules (decision (j)).
- **No model-selected policy.** Sandbox mode and rules come from configuration,
  never from the model (`07` (r), `§46`).

### 3.6 Grant store (`Once` / `Session` / `Always`)

- **`Session` grants** live in an in-memory map in the daemon, keyed
  `(session, tool, path-pattern)`. They are dropped on daemon restart, so a
  restart re-asks (fail-safe, OQ-P2). The name mirrors
  `protocol::PermissionScope::Session` (`05 §7.6`) and denotes **daemon
  lifetime**, not the durable 01 session (decision (s)).
- **`Always` grants** are appended to the workspace-local grants file
  `<workspace>/.ymh/permissions.local.toml` (decision (e), OQ-P1). The daemon is
  the single writer for its workspace (`§54 D18`), so no cross-process locking is
  needed; the file is read back into the `LocalGrant` layer at startup.
- A grant **narrows or widens** only the rules it is about: a `Session` grant for
  `(edit_file, src/**)` does not allow `edit_file` elsewhere, and never affects
  another tool or path.
- **Grant-store write failure is non-fatal**: log the failure (no arguments, no
  secrets — Q11), keep the decision, and degrade the grant to `Session`
  in-memory only. The turn proceeds with the human's decision; only the
  persistence is lost (Q-F11).
- Grants are **never** created without a human decision: `remember` is only
  called by the broker after a `permission.decide` (§4.2). `AllowAlways` never
  appears in the log without a corresponding decision event.

### 3.7 `PermissionHandle` (07 §5.4 co-owned)

`07 §5.4` pinned a provisional handle and deferred `PermissionRequest` to this
spec (`07` decision (o)). The shape is finalized here; it matches 07's
provisional pin exactly so nothing in 07 changes (decision (l)).

```cpp
namespace ymh {

class PermissionHandle {
public:
    virtual ~PermissionHandle() = default;

    // The decision already recorded for the current call (06 §5.1). By the time
    // a tool runs, the decision is durable and non-Ask (07 §5.4, Q2).
    virtual PermissionDecisionKind decision() const noexcept = 0;

    // Optional sub-operation seam: a tool that can only discover a sub-operation
    // at execution time (e.g. a decomposed pipeline) asks again. v1 tools do not
    // call it; the single pre-execution decision suffices (07 §5.4).
    //
    // When the sub-operation classifies as Ask, the concrete handle resolves it
    // through the same decision flow (§4) and returns the recorded kind; a tool
    // never observes Ask. Returns the resolved PermissionDecisionKind.
    virtual Task<PermissionDecisionKind> evaluate(const PermissionRequest&) = 0;
};

} // namespace ymh
```

- The concrete implementation (`LoopPermissionHandle`) is owned by the loop and
  bound to one `payload::ToolCall`; `ToolContext::permission()` returns it
  (`07 §5.1`).
- A tool **never** sees `Ask`; the pre-execution decision is terminal (Q2).
- A tool **never** executes a `Deny` and never returns `Denied`; the loop
  produces `ToolResult{Denied}` (`07 §5.4`, `§8.3`).
- **Sub-operation abort obligation.** A sub-operation `evaluate` call (the seam
  above) may resolve to `Deny` *after* the tool has already performed part of
  its work. When it does, the tool **must stop immediately**, perform no further
  sub-operation, and surface the denied step to the loop (which records the
  `ToolResult`); it must not continue with a partially applied effect. Q4 covers
  the primary pre-execution call only; this is the tool's obligation for the
  mid-execution sub-operation seam (decision (l)).

---

## 4. Decision flow over transport

### 4.1 Sequence

The loop drives the pipeline (`06 §5.1`); the broker owns the wire wait.

```text
AgentLoop (06 §5.1)                 PermissionBroker            Transport (05 §7.6)
────────────────────                ────────────────            ───────────────────
append payload::ToolCall ────────────────────────────────────►  (durable)
build PermissionRequest
verdict := policy.evaluate(req)
  ├─ Allow  ─────────────────────────────────────────────────►  (no round-trip)
  ├─ Deny   ─────────────────────────────────────────────────►  (no round-trip)
  └─ Ask
      state := WaitingForPermission
      emit live PermissionRequested
      await broker.resolve(req, cancel) ──►  mint request_id
                                             register pending
                                             broadcast ─────────►  permission.request
                                                                  (all subscribers)
                                             ◄───────────────────  permission.decide
                                             first decision wins
                                             policy.remember(...)
                                             resolve pending
      ◄── PermissionOutcome{kind, reason}
append payload::PermissionDecision{call, kind, reason}           (durable)
  ├─ granted ──► ToolRegistry::execute (07) ──────────────────►  ToolResult
  └─ denied  ──► append payload::ToolResult{call, Denied}        (execute skipped)
```

The loop is the **sole** appender of durable events (`06` A1/A2, `07` X8). The
broker returns a `PermissionOutcome`; it never appends.

### 4.2 `PermissionBroker`

```cpp
namespace ymh {

struct PermissionOutcome {
    PermissionDecisionKind decision;   // Allow | Deny | AllowAlways (never Ask)
    std::string            reason;     // rule id, "user", "timeout", "cancelled", …
};

class PermissionBroker {
public:
    PermissionBroker(PermissionPolicy&, TransportServer&, Clock&, PermissionConfig);

    // Called ONLY for an Ask verdict. The loop has already transitioned to
    // WaitingForPermission and emitted the live PermissionRequested. Resolves
    // the request to a terminal outcome via the wire, the timeout, or the
    // cancellation token. Never throws; a failure resolves fail-closed (Q3).
    Task<PermissionOutcome> resolve(const PermissionRequest&, CancellationToken);

    // Transport callback: a client answered. Applies the first-decision-wins
    // rule atomically at the daemon's single-threaded dispatch point (§4.4).
    void onDecision(const protocol::PermissionDecisionParams&);

    // A newly subscribed client receives the session's pending requests so a
    // late-attaching supervisor can answer (decision (o)).
    std::vector<protocol::PermissionRequest> pending(SessionId) const;

    // Number of unresolved requests (used by tests and the daemon's diagnostics;
    // the UI count is spec 10's projection, §5.3).
    std::size_t pendingCount() const noexcept;
};

} // namespace ymh
```

Rules:

- **The broker never blocks the daemon's main loop.** It registers a pending
  request and returns a future/continuation; the transport dispatch resolves it
  (`05 §8.1`: no blocking waits on clients; `04` D-F8).
- **`resolve` is the only entry to the wire.** `Allow`/`Deny` verdicts never
  produce a `permission.request`; a request is emitted only for `Ask`.
- **A missing transport is a startup error**, not a runtime bypass (Q-F1).

### 4.3 Wire mapping (`05 §7.6`)

The core request maps to the frozen wire DTO without changing it:

| Core (`ymh::PermissionRequest`) | Wire (`protocol::PermissionRequest`, 05 §7.6) |
|---|---|
| `session` | `session` |
| `tool` | `tool` |
| `arguments` (bounded, redacted) | `arguments` |
| (derived) | `summary` — bounded, redacted human string |
| `request_id` (broker-minted) | `request_id` |
| deadline | `expires_at_ms` — server-side deadline |

```cpp
namespace ymh::protocol {

// Frozen by 05 §7.6 — reproduced for reference only; do NOT redefine.
// enum class PermissionAnswer : std::uint8_t { Allow, Deny };
// enum class PermissionScope  : std::uint8_t { Once, Session, Always };
// struct PermissionRequest   { request_id, session, tool, arguments, summary, expires_at_ms };
// struct PermissionDecisionParams { request_id, decision, scope };

} // namespace ymh::protocol
```

- **`summary`** is built by the broker from the tool name and a bounded,
  redacted argument projection (decision (p)): for `shell`, the command is shown
  because it is the thing being approved; for file writes, the path plus
  size/diff stats are shown, not the file body. `summary` is never logged (Q11).
- **`arguments`** are bounded by `PermissionConfig::arguments_max_bytes` and
  delivered only over the owner-only, same-UID socket (`05` (f)); over the cap,
  the broker truncates and records `truncated` in the summary (Q-F14). The full
  arguments remain in the durable `payload::ToolCall` in the session log
  (`01 §4.5`); the wire copy is a bounded view.
- **`scope`** maps 1:1 to `GrantScope` (`Once`/`Session`/`Always`, §2.3).
- **`decision`** maps `Allow`/`Deny`; the broker derives `AllowAlways` from
  `decision == Allow && scope == Always` (Q13). There is no wire value that
  yields `AllowAlways` without a human decision.

### 4.4 Multi-supervisor fan-out (05 OQ-4, resolved)

`05 §7.6` pinned "broadcast; first decision wins" provisionally and delegated the
authoritative policy to this spec (`05` OQ-4). This spec resolves it:

- The daemon **broadcasts** `permission.request` to every attached client that
  is subscribed to the session (Interactive) or attached (Automation) —
  `05 §7.6`, `05 §8.2`.
- The **first valid `permission.decide` wins** and is applied atomically at the
  daemon's single-threaded dispatch point (`05 §8.1`: dispatch runs on the main
  loop). The pending request transitions `Pending → Resolved` exactly once.
- Later decisions for the same `request_id` are **ignored** and return
  `InvalidParams` (`05 §7.6`); the operation is idempotent at the policy layer
  (`05` (l), Q10, Q-F8).
- A decision for an **unknown or expired** `request_id` returns `InvalidParams`
  and changes nothing (`05 §7.6`, Q-F9).
- A client that subscribes **after** the broadcast receives the pending request
  (decision (o)); it may still answer if not yet resolved. A client that was
  attached but not subscribed to the session does not receive it (Interactive).
- Fan-out isolation is 05's: one slow/dropped client never blocks the request
  (`05` T14, `05 §8.2`).

### 4.5 Timeouts

- Every `Ask` carries a server-side deadline `expires_at_ms` (`05 §7.6`),
  computed from `PermissionConfig::permission_timeout` (built-in default: five
  minutes; value-only, OQ-P6).
- **On expiry the request resolves fail-closed**: `Deny`, reason `"timeout"`, a
  durable `PermissionDecision` is recorded, and the session is surfaced via
  attention (§5). No turn waits forever **by default**; the single unbounded
  case is the explicit `permission_timeout = 0` opt-in below.
- `permission_timeout = 0` disables the deadline **only** while at least one
  supervisor is attached and subscribed; in that configuration the turn waits
  for that subscriber's decision with no server-side deadline. A detached daemon
  always uses a bounded deadline (§5.2, decision (n)).
- The deadline is evaluated by the broker's clock, never by a widget
  (`§20.23`/§54 D16: timers live in models, not `Render()`). The broker owns the
  policy-side deadline; the UI's flash phase is spec 10's (`§9.9` F12).

### 4.6 Cancellation (`§34`)

- `resolve` accepts the turn's `CancellationToken`; a `WaitingForPermission`
  turn remains cancellable (`06 §3.5`, A9, F9).
- **Cancel while waiting** resolves the pending request fail-closed:
  `PermissionDecision{Deny, reason="cancelled"}` is recorded (audit
  completeness, Q14) and the turn closes with `TurnCancelled` (`06` A2/A10).
  The call's `ToolResult` is `Cancelled` (token fired, `07 §2.2`); if the loop
  never reaches the call, `deriveMessages()` synthesizes it (`01` I12).
- Cancellation is **not** an error and is never recorded as `TurnFailed`
  (`06` A10, `08` (k)).
- A pending request whose session is disposed is resolved the same way
  (`06` A14); no request outlives its session.
- The broker removes the pending entry on every terminal path (decision,
  timeout, cancel, dispose), so `pendingCount()` never leaks (Q12, Q-F10).

### 4.7 Durable event and audit

The recorded event is `01 §4.5`'s frozen payload, appended by the loop:

```cpp
struct PermissionDecision {                 // 01 §4.5 — reproduced, not redefined
    ToolCallId              call;           // pairs with payload::ToolCall.id
    PermissionDecisionKind  decision;       // Allow | Deny | AllowAlways
    std::string             reason;         // rule id / "user" / "timeout" / "cancelled"
};
```

- **Durability.** The decision is appended before execution (Q2, `07` X7) and is
  replayed on resume; replay never re-issues a `permission.request` (Q-F16).
- **Totality.** Every `payload::ToolCall` has exactly one recorded decision once
  its turn reaches the tool pipeline; a crash mid-wait is repaired on resume by
  the projection's synthesized `ToolResult{Cancelled}` (`01` I12, `02` recovery,
  Q14).
- **`reason` is bounded and redacted.** It is a rule id, a short enum-like token
  (`"user"`, `"timeout"`, `"cancelled"`), or a superseded-rule id; it never
  contains request arguments, prompt content, or a typed user note (Q11). A
  permission `Ask` is answered only by `permission.decide`
  (`{request_id, decision, scope}`, `05 §7.6`), which carries no free-text
  field, so the agent stays in `WaitingForPermission` until decision, timeout, or
  cancel — there is no typed-reason path (decision (q)).
- **Never logged.** The decision's tool name and reason may appear in the
  `agent`/`tool` log categories; arguments, file bodies, and prompt text never
  do (`§40`: never dump full prompts or sensitive tool output; `§46`). The
  session event log is the authoritative trace.

---

## 5. Background permission policy (F2)

### 5.1 The pinned policy: surface-via-attention, fail-closed on timeout

`§9.9` leaves the background permission policy as an explicit decision to be made
before background execution ships (F2), listing three candidate mechanisms:
auto-deny, auto-allow-per-tool, or surface-via-attention. **This spec pins
surface-via-attention (badge + waiting count) with a fail-closed timeout
auto-deny as the single v1 mechanism** (decision (g)):

```text
An Ask on a background session:
  1. does NOT block invisibly — the request is broadcast to every attached
     subscriber and the session enters WaitingForPermission (06 §3.3), which is
     in the waiting set (§20.23), so the session contributes to the aggregate
     waiting count and the ~1s attention flash (§9.9 F4/F12);
  2. resolves within permission_timeout (default 5 min);
  3. on timeout with no decision → Deny, reason "timeout", durable decision.
```

- The session is **surfaced**, not silenced: its badge and the aggregate
  `waitingCount` reflect the blocked state until it resolves (`§20.23`, spec 10).
- The fallback is **fail-closed** (auto-deny), so a model-initiated mutation
  never runs just because no human was watching.
- `auto-allow-per-tool` is **not** a background mechanism in v1; it is
  expressible as an ordinary config rule (`shell = "allow"`), which the operator
  chooses deliberately. The background policy never silently allows (Q3).

### 5.2 Detached daemon (zero attached clients)

- A fully detached daemon (no attached client) has no one to answer, so the
  broker does not block: the request is registered with its deadline and
  resolves fail-closed at `permission_timeout` (never waits indefinitely,
  decision (n)).
- While detached, the session is still in `WaitingForPermission`; when a
  supervisor attaches, the pending request is re-broadcast (decision (o)) and the
  session's badge/waiting count appear immediately (`§9.9`: attachment never
  gates work; it only changes what is displayed).
- The daemon survives TUI detach (`§54 D23`); detach neither cancels the request
  nor changes its deadline.

### 5.3 Attention and the waiting count (F4/F12)

- `WaitingForPermission` is in the waiting set `{WaitingForInput,
  WaitingForPermission, Error}` (`§20.23`, `06 §3.3`), so a blocked session is
  counted by `AggregateStatusModel::recompute()` — a **level snapshot**, not a
  per-edge increment (`§9.9`).
- The **flash** is a UI-only edge notification owned by spec 10
  (`§9.9` F12, `§20.23`); this spec emits the live `PermissionRequested` event
  and the `permission.request` notification, and never advances a timer in a
  widget (`§54 D16`).
- Focusing a session does **not** remove it from the waiting count; the count
  drops only when the decision moves the session's `AgentState` out of the
  waiting set (`§20.23`).

### 5.4 Single-active-session interaction (`04` (m), `06 §6`)

- The daemon keeps exactly one active session per workspace and activates the
  next session with pending work when the active one finishes or blocks awaiting
  input (`§9.9`, `04` (m), H17).
- A session blocked in `WaitingForPermission` is **blocked**, so it does not
  hold the active slot; the arbiter may activate another pending session. The
  blocked session is surfaced in the waiting count (§5.3) — never dropped.
- **Resolving a permission does not itself move the active slot.** The decision
  is durable and the blocked session becomes activatable; the arbiter promotes
  it when the active slot is free (the current active session is `Idle` or
  blocked). This respects one-active-session and avoids cancelling an unrelated
  running turn merely to deliver a permission resume (`06 §6`: moving the slot
  cancels the current turn with `reason="superseded"`).
- The precise scheduling (queue vs. preempt) is owned by 04/06; this spec pins
  only that (i) the decision is durable, (ii) the blocked session is
  activatable, and (iii) the session is surfaced. See OQ-P3.

---

## 6. Security and logging (`§40`, `§46`)

### 6.1 The mandatory chain

`§46` requires that model-generated actions are treated as untrusted and that
the policy layer cannot be bypassed:

```text
LLM output → Tool parser → Permission policy → Execution environment → OS
```

- No path skips `evaluate` (Q2, Q4). A missing policy is a startup error
  (`07` E-F17, Q-F1).
- `ReadOnly` is enforced at the policy layer as `Deny` (`07 §6.8`, decision (j)).
- The policy is **independent of the TUI** (`§19`), which is what makes headless
  and Automation operation possible (`§42`, `§43`).

### 6.2 Redaction and the no-log invariant

- **Never log request arguments, file bodies, prompt text, or tool output** in
  normal logs (`§40`). The decision's tool name and a bounded `reason` may be
  logged; the arguments may not (Q11, Q-F15).
- The wire request is delivered **only** to attached, same-UID clients over the
  owner-only socket (`05` (f), `§46`); it is never broadcast on a shared
  transport. `summary`/`arguments` are bounded by
  `PermissionConfig::arguments_max_bytes` (Q-F14).
- The durable `payload::PermissionDecision` carries no arguments (`01 §4.5`);
  the authoritative arguments live in the durable `payload::ToolCall`, which is
  the same session log the operator already owns.
- A grant file (`permissions.local.toml`) records tool/path patterns and effects
  only — never argument values (Q13).

### 6.3 Authentication

- v1 uses same-UID `SO_PEERCRED` on the owner-only socket plus the mandatory
  hello handshake; there is no bearer token (`05` (f), `§46`). A client that
  cannot authenticate never receives a `permission.request`.
- The Automation profile (`§9.6`) is the prompt/cancel/permission profile; it
  includes the permission pair subject to the profile's method subset
  (decision (m), OQ-P4).

---

## 7. Concurrency and threading

- `evaluate` is pure and may run on the loop's executor; it is cheap and
  allocation-bounded (it never does I/O).
- `PermissionBroker::onDecision` runs on the daemon's single-threaded dispatch
  point (`05 §8.1`), so the first-decision-wins transition is atomic without a
  lock on the hot path (§4.4, Q10).
- `remember` mutates policy state; a config reload or grant write bumps
  `generation()` monotonically. A verdict computed under a stale generation is
  invalidated (Q5).
- The broker holds only the pending map; it owns no threads and never blocks the
  main loop (`04` D-F8, Q8).

---

## 8. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**Q1 — Pure, TUI-independent policy.** `PermissionPolicy::evaluate` is
synchronous, side-effect-free, performs no I/O, never blocks, and references no
UI type. (`§19`, `§54 D7`/D14, Q15)

**Q2 — Decision before execution.** A tool executes only after a durable
`payload::PermissionDecision` with a non-`Ask` kind is recorded. (`07` X7,
`§46`, `06 §5.1`)

**Q3 — Fail-closed.** No rule match yields the configured default (built-in
`Ask`), never an implicit `Allow`; timeout, cancel, and detached resolution
yield `Deny`. (`§9.9` F2, `§46`)

**Q4 — Denied is never executed.** A `Deny` verdict, a `ReadOnly` hard deny, or
a human deny yields `ToolResult{outcome = Denied}` and `Tool::execute` is not
called. (`07 §5.4`, `§8.1`, `07` (u))

**Q5 — Deterministic and total.** `evaluate` is a pure function of
`(request, effective rules, generation)`; the precedence is a total order and
the same inputs always yield the same verdict. (`§44` unit tests)

**Q6 — Specificity, then layer, then effect.** The winner is the most specific
match; ties break to the later layer; remaining ties break `Deny > Ask >
Allow`. (§3.4)

**Q7 — Hard deny is absolute.** `ReadOnly` mutating calls are denied before
rules and are not overridable by any rule or grant; the policy never
re-resolves a path (07 supplies it). (`07 §6.8`, X2/X3)

**Q8 — Background never deadlocks invisibly.** Every `Ask` on a
background/detached session resolves within a bounded deadline and records a
durable decision; no turn waits forever. (`§9.9` F2, `04` D-F8)

**Q9 — Waiting sessions are surfaced.** `WaitingForPermission` is in the
waiting set; a blocked session contributes to the aggregate waiting count and is
never hidden by focus. (`§9.9`, `§20.23`)

**Q10 — One decision per request; first wins.** The fan-out transition
`Pending → Resolved` is atomic and happens exactly once; later decisions are
ignored and return `InvalidParams`. (`05 §7.6`, `05` OQ-4)

**Q11 — No sensitive content in logs.** Request arguments, file bodies, prompt
text, and tool output are never written to normal logs; only tool name and a
bounded reason are. (`§40`, `§46`)

**Q12 — Cancellation resolves pending requests.** Cancel/dispose while waiting
resolves the request fail-closed, removes the pending entry, and closes the turn
`TurnCancelled`; no orphan request or leak. (`§34`, `06` A9/A14, F9)

**Q13 — Grant scope honesty.** `Once` persists nothing; `Session` is
daemon-lifetime in-memory only; `Always` is workspace-durable and never
outlives its workspace. No `AllowAlways` is recorded without a human decision.
(§2.3, §3.6, `05 §7.6`)

**Q14 — Audit completeness.** Every `payload::ToolCall` reaching the pipeline has
exactly one recorded decision; a crash mid-wait is repaired by the projection's
synthesized `ToolResult{Cancelled}`. (`01` I12, `02` recovery)

**Q15 — No UI dependency.** The policy and broker targets never include or
reference UI types; timers live in models, never in `Render()`. (`§4.1`,
`§20.1`, `§54 D14`/D16)

---

## 9. Failure modes

### 9.1 Shared findings (F1–F12, `§54`)

The permission layer's responsibilities for the existing findings:

| F# | Finding | Permission-layer handling |
|---|---|---|
| **F1** | path/process isolation | the policy matches on the 07-resolved root-relative path; it never resolves or `chdir`s (Q7, `§9.7`, `§18`) |
| **F2** | background permission | **owned here**: surface-via-attention + fail-closed timeout auto-deny; a background `ASK` records a durable decision and is surfaced, never an invisible deadlock (§5, Q8) |
| **F3** | late event after close | a decision for a disposed session resolves fail-closed and is removed; the loop appends any durable record before close (`06` A14, Q12) |
| **F4** | edge-triggered attention | the policy emits the live `PermissionRequested`; the waiting count/flash are spec 10's projection (Q9, §5.3) |
| **F5** | output ring buffers | the request's `arguments`/`summary` are bounded by `arguments_max_bytes`; the policy holds no large buffers (Q-F14) |
| **F6** | input/keybinding focus | out of scope; the policy never sees focus |
| **F7** | per-session dirty flags | out of scope; UI projection (spec 10) |
| **F8** | resource caps | out of scope; the policy allocates nothing unbounded and owns no executor |
| **F9** | cancellation scoping | the pending request is scoped to its session; cancelling one session never resolves another (Q12, `§34`) |
| **F10** | resume-suspended | a resumed session re-asks; `Session` grants are in-memory and do not survive restart (Q13) |
| **F11** | subagent ID duality | a subagent has its own policy/session (`06 §7`); its requests carry its own `SessionId` |
| **F12** | flash clock in model | out of scope; the broker owns only the request deadline, never a UI timer (Q15, §5.3) |

**Explicitly out of scope:** F1's resolution arithmetic (07), F3's tombstone
(01), F4's flash arithmetic (10), F5/F7/F8 (07/10), F6/F12 (10), F11 (06).

### 9.2 Component-local failure modes (`Q-F#`)

These are component-local to the permission layer and must be covered by tests
(§11.6). The `Q-F#` namespace is disjoint from every other component's failure
namespace (`P-F#`, `R-F#`, `D-F#`, `T-F#`, `A-F#`, `E-F#`, `L-F#`).

| Q-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **Q-F1** | Policy not wired | startup check | startup failure, never a runtime bypass (Q2, `07` E-F17) |
| **Q-F2** | No rule matches | empty match set | configured default (built-in `Ask`); never implicit `Allow` (Q3) |
| **Q-F3** | `ReadOnly` + mutating tool | sandbox check before rules | `Deny`, not overridable; `execute` skipped (Q7) |
| **Q-F4** | Invalid rule (bad glob/tool/effect, no dimension) | config load | `PolicyConfigError` at startup, fail loud (Q5) |
| **Q-F5** | Conflicting rules | precedence fold | deterministic winner (specificity→layer→effect); no nondeterminism (Q6) |
| **Q-F6** | Request expires | deadline fired | fail-closed `Deny{"timeout"}`; durable decision; surfaced (Q8) |
| **Q-F7** | Detached daemon / zero subscribers | `hasSubscriber == false` | never blocks; bounded deadline then auto-deny; re-broadcast on attach (Q8) |
| **Q-F8** | Multi-supervisor race | two decisions for one id | first wins atomically; later ignored → `InvalidParams` (Q10) |
| **Q-F9** | Decision for unknown/expired id | pending-map miss | `InvalidParams`; no state change (`05 §7.6`) |
| **Q-F10** | Cancel while waiting | token fired | fail-closed decision; pending entry removed; `TurnCancelled` (Q12) |
| **Q-F11** | Grant-store write failure | file write error | log (no arguments), keep the decision, degrade to `Session`-only; never fail the turn (Q13) |
| **Q-F12** | Rule file unreadable/corrupt | config parse | startup failure for workspace config; never silently default to `Allow` (Q3) |
| **Q-F13** | Path-pattern evasion | match on canonical root-relative path | a symlinked path cannot dodge a deny rule; matching uses the 07-resolved path (Q7) |
| **Q-F14** | Oversized request arguments | size check | truncate/redact to `arguments_max_bytes`; `summary` notes truncation; never fail (Q11) |
| **Q-F15** | Sensitive content in logs | log inspection test | arguments/bodies never logged; only tool name + bounded reason (Q11) |
| **Q-F16** | Replay re-asks | replay of a log with decisions | replay projects durable decisions; never re-issues `permission.request` (Q14) |

---

## 10. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (`§55`). The permission layer maps
onto it as follows.

| dsh concept | ymh permissions | Reference |
|---|---|---|
| Approvals | `PermissionPolicy` | `§19`, `§46`, `§55` |
| Approval request | `PermissionRequest` (core) / `protocol::PermissionRequest` (wire) | `§19`, `05 §7.6` |
| Approval outcome | `PermissionDecisionKind` → durable `PermissionDecision` | `01 §4.5` |
| Approval prompt event | live `PermissionRequested` + `permission.request` | `§8.1`, `05 §7.6` |
| Approval scope | `GrantScope{Once,Session,Always}` | `05 §7.6`, §2.3 |
| Background approvals | surface-via-attention + fail-closed timeout | `§9.9`, F2 |
| Append-only trace | the loop appends the decision; the policy never writes | §54 D2, `01 §4.5` |
| Profiles/bundles | policy selected by config layering | `§37`, `§38` |

**Deliberate omissions** (accepted for v1, `§55`): no OS sandbox (`§52`), no
per-command shell parsing beyond glob matching (OQ-P5), no global ("all
workspaces") grant from the prompt (OQ-P1), no permission delegation to
subagents beyond their own policy (`06 §7`), and no automation-specific approval
protocol beyond the `permission.*` pair (OQ-P4).

---

## 11. Test plan

Strategy is `§44`: unit, integration (fake supervisor / `FakeLLM`), golden,
replay, and a separate live PTY/real-LLM layer. The deterministic layers run
offline; the live layer is opt-in and API-key gated (`§44`, `§45`).

### 11.1 Unit tests

- **Precedence (table-driven, Q5/Q6)**
  - most-specific wins: global `write_file = ask` vs. local
    `write_file:src/** = allow` → `Allow` for `src/x`, `Ask` for `docs/x`.
  - layer tie-break: same specificity in `Project` vs. `Global` → `Project`;
    the full chain `SessionGrant > LocalGrant > CommandLine > Profile >
    Project > Global > Builtin` is exercised, including `CommandLine` >
    `Profile` and `LocalGrant` > `CommandLine`.
  - effect tie-break: equal specificity and layer, `Allow` vs. `Deny` → `Deny`.
  - dimension count: `(tool,path)` beats `(tool)`; `(tool,command)` beats
    `(tool)`.
  - exact tool beats tool glob at equal dimension count; two globs with equal
    dimension count break on literal-char count (`git_*` beats `*`), then
    longer pattern, then byte-order (Q6 total order).
  - no match → default; `default="allow"` is honored only when explicitly set.
  - a `Deny` rule cannot be widened by an equal-specificity `Allow` (Q6).
- **Hard deny (Q7, Q-F3)**
  - `ReadOnly` + `edit_file`/`write_file`/`shell` → `Deny` regardless of any
    matching `allow` rule or grant; `execute` is not called.
  - `Unrestricted` skips containment but is still policy-gated.
- **Rule parsing (Q-F4/Q-F12)**
  - malformed glob, unknown tool, unknown effect, dimension-less rule →
    `PolicyConfigError` at load.
  - unreadable/corrupt workspace config → startup failure, never default-allow.
  - tool-group shorthands expand to the fixed 07 §4.3 name sets.
- **Grant store (Q13, Q-F11)**
  - `Once` persists nothing; `Session` is in-memory and gone after a simulated
    restart; `Always` round-trips through `permissions.local.toml`.
  - grant for `(edit_file, src/**)` does not allow `edit_file` elsewhere nor
    another tool.
  - a write failure degrades to `Session` and does not fail the turn.
  - `remember` is idempotent.
- **Path matching (Q7, Q-F13)**
  - `src/**` matches `src/a/b`, not `srcx/a`; `**/*.lock` matches nested locks.
  - a symlinked in-root path that resolves outside is already rejected by 07;
    the policy sees only the resolved path and applies the deny rule.
- **`PermissionHandle` (07 §5.4)**
  - `decision()` returns the recorded kind, never `Ask`; `evaluate` resolves a
    sub-operation `Ask` through the broker and returns a terminal kind.
- **No-log / redaction (Q11, Q-F14/Q-F15)**
  - a `write_file` with a large body produces a bounded `summary`/`arguments`
    and no body in any log sink; a `shell` command is shown but not logged.
- **Determinism (Q5)**
  - `rules()` is ordered and byte-stable; a cached verdict under a stale
    `generation()` is rejected.

### 11.2 Integration tests (fake supervisor, `FakeLLM`, `§45`)

- **Allow path.** `FakeLLM` emits a tool call a rule allows; no
  `permission.request` is emitted; the decision is `Allow`; the tool runs.
- **Deny path.** A rule denies; `PermissionDecision{Deny}` + `ToolResult{Denied}`
  are durable; `execute` is not called (Q4).
- **Ask → allow over transport.** A fake supervisor receives
  `permission.request` and sends `permission.decide{Allow, Once}`; the durable
  decision is `Allow`; the turn resumes.
- **Ask → deny over transport.** `Deny` yields `ToolResult{Denied}`; the turn
  continues (`06` A-F6).
- **Scope semantics.** `{Allow, Session}` allows the next identical call with no
  new request; `{Allow, Always}` writes a grant and survives a simulated
  restart; `{Allow, Once}` re-asks.
- **First-decision-wins (Q10).** Two fake supervisors answer the same
  `request_id`; the first is applied, the second gets `InvalidParams`; exactly
  one durable decision exists.
- **Timeout (Q-F6).** No answer; at `permission_timeout` the decision is
  `Deny{"timeout"}` and the session was in the waiting count throughout.
- **Detached (Q-F7).** Zero attached clients; the request resolves fail-closed
  at the deadline; a late attach receives the re-broadcast and the session badge.
- **Cancel (Q-F10).** Cancel while `WaitingForPermission` → fail-closed
  decision, `TurnCancelled`, no pending leak.
- **Replay (Q-F16).** Replaying a log with decisions never issues a new
  `permission.request` and reproduces the same projected state.
- **Lease interaction.** A decision append obeys the session write lease like
  any append (02); a lost lease degrades to read-only (`06` A11).

### 11.3 Golden tests

- A recorded `permission.request` fixture (tool, bounded `summary`, redacted
  arguments) rendered by spec 10's permission overlay produces a stable terminal
  representation; the fixture is owned here, the renderer by 10 (`§44`).
- The aggregate line fixture for a session in `WaitingForPermission` shows the
  expected waiting count (jointly with 10, `§20.23`).
- Redaction golden: a request with a large/sensitive body renders the bounded
  summary and never the body (Q11).

### 11.4 Replay tests

- Given a recorded event stream containing `ToolCall`/`PermissionDecision`/
  `ToolResult`, replay reproduces the same projected messages and the same
  decision kinds without any live I/O (Q14).
- A stream truncated mid-wait (crash) replays with a synthesized
  `ToolResult{Cancelled}` for the unresolved call (`01` I12, Q14).

### 11.5 Live end-to-end tests (real LLM, PTY-driven, opt-in, `§44`)

- Spawn the real `ymh` binary under a PTY; a real LLM emits a tool call that
  classifies as `Ask`; the PTY driver (script or `tmux send-keys` +
  `capture-pane`) answers allow and deny; assert observable behavior: the
  permission prompt rendered, the tool ran/blocked accordingly, the durable
  decision exists, and the session resumes (`§44`).
- Uses a **real LLM API through the same `LLMProvider` seam** (`§44`).
- **Gated and opt-in:** requires an API key plus `YMH_LIVE_LLM=1`; skipped —
  never failed — when absent.
- **Nondeterminism-tolerant:** assert on structure and invariants (events
  emitted, decision kind, final state), not exact prose.
- **Milestone-aware:** MVP live tests cover the single-process flow
  (Milestone 1, `§57`/`§58`); multi-workspace/multi-supervisor PTY tests target
  Milestone 2.
- Runs as a separate CI stage; never part of the fast default command (`§44`).

### 11.6 Failure-mode coverage matrix

| Q-F# | Unit | Integration | Golden | Replay | Live |
|---|---|---|---|---|---|
| Q-F1 | startup check | — | — | — | — |
| Q-F2 | default table | no-match request | — | — | — |
| Q-F3 | ReadOnly table | ReadOnly tool | — | — | — |
| Q-F4 | config parse | — | — | — | — |
| Q-F5 | precedence table | conflicting rules | — | — | — |
| Q-F6 | clock fake | timeout | waiting badge | — | prompt timeout |
| Q-F7 | — | detached daemon | badge | — | — |
| Q-F8 | — | two-supervisor race | — | — | multi-supervisor |
| Q-F9 | — | unknown/expired id | — | — | — |
| Q-F10 | — | cancel mid-wait | — | — | cancel in TUI |
| Q-F11 | grant write fail | degrade to session | — | — | — |
| Q-F12 | corrupt config | — | — | — | — |
| Q-F13 | path glob table | symlink-resolved path | — | — | — |
| Q-F14 | size clamp | large arguments | bounded summary | — | — |
| Q-F15 | log inspection | — | redaction golden | — | — |
| Q-F16 | — | — | — | replay decisions | — |

### 11.7 Invariant coverage

| Invariant | Covered by |
|---|---|
| Q1 | purity unit (no I/O, no UI include) |
| Q2 | decision-before-execute integration; static append-order check |
| Q3 | default/no-match/timeout/cancel unit + integration |
| Q4 | deny/ReadOnly unit + integration (`execute` not called) |
| Q5 | precedence table + generation-cache unit |
| Q6 | precedence tie-break table |
| Q7 | hard-deny unit; path-glob table |
| Q8 | background timeout/detached integration |
| Q9 | waiting-count golden (with 10) |
| Q10 | first-decision-wins integration |
| Q11 | redaction unit + log-inspection test |
| Q12 | cancel/dispose integration; pending-leak check |
| Q13 | grant-scope unit + restart simulation |
| Q14 | replay decisions + crash-truncation replay |
| Q15 | static check: no UI include in the policy/broker targets |

---

## 12. Decisions and open questions

### 12.1 Decisions (pinned by this spec)

- **(a) The policy verdict is `PolicyVerdict {Allow, Ask, Deny}`; the durable
  kind is `01`'s `PermissionDecisionKind {Allow, Deny, AllowAlways}`, reused
  verbatim.** `§19`'s `PermissionDecision {Allow, Deny, Ask}` sketch is
  reconciled as the *verdict*, not a second durable enum; no `Ask` is ever
  durable (§2.2).
- **(b) `PermissionRequest` is a core `ymh` type.** This resolves the 07 (o)
  deferral; `protocol::PermissionRequest` (`05 §7.6`) is its wire projection
  (§3.2, §4.3). The loop builds one per call and calls `evaluate`; the
  `evaluate(call, session)` shorthand in 06/07 denotes that call site.
- **(c) `PermissionPolicy::evaluate` is pure and transport-agnostic; the
  decision flow lives in `PermissionBroker`.** The policy never blocks, so it
  cannot deadlock; the broker owns the wire, timeouts, and cancellation
  (§3.1, §4.2).
- **(d) Precedence is total: specificity → layer → effect (`Deny > Ask >
  Allow`).** Source order is irrelevant; an exact tie is fail-safe (§3.4, Q6).
- **(e) Grant scopes are `{Once, Session, Always}`, mapping 1:1 to
  `protocol::PermissionScope`.** `Session` is in-memory only; `Always` persists
  to `<workspace>/.ymh/permissions.local.toml` (workspace-scoped). Global grants
  are config-only (§2.3, §3.6, Q13).
- **(f) Configuration extends `§37` with `[[permissions.rule]]` while keeping
  the `[permissions]` shorthand.** Tool groups expand to fixed 07 §4.3 name
  sets; invalid rules fail loud at startup (§3.3).
- **(g) Background policy (F2) is surface-via-attention with a fail-closed
  timeout auto-deny.** This resolves the `§9.9` open decision. Auto-allow is
  not a background mechanism; it is an explicit config rule (§5.1).
- **(h) Multi-supervisor fan-out: broadcast; first valid decision wins
  atomically; later decisions return `InvalidParams`.** This resolves `05`
  OQ-4 (§4.4, Q10).
- **(i) `PermissionDecision` is durable and argument-free; request content is
  delivered only over the owner-only same-UID socket and never logged.**
  (`01 §4.5`, `§40`, `§46`, Q11)
- **(j) `ReadOnly` hard deny precedes rules and is not overridable.** The loop
  records `Deny` and `ToolResult{Denied}`; `execute` is not called
  (`07 §6.8`, `07` (u), Q7).
- **(k) A denied tool is never executed.** The loop appends
  `payload::ToolResult{outcome = Denied}` (07 §8.1); a tool never returns
  `Denied` (`07 §8.3`, Q4).
- **(l) `PermissionHandle` is finalized to 07's provisional shape.**
  `decision()` returns the recorded non-`Ask` kind; `evaluate` resolves a
  sub-operation `Ask` through the broker. Nothing in 07 changes (§3.7).
- **(m) The Automation profile includes the `permission.request`/
  `permission.decide` pair**, subject to the profile's method subset (`05 §6.2`,
  `§9.6`, OQ-P4).
- **(n) The default `permission_timeout` is five minutes; `0` disables it only
  with an attached subscriber.** A detached daemon always uses a bounded
  deadline (§4.5, §5.2).
- **(o) A newly subscribed client receives the session's pending requests** so a
  late-attaching supervisor can answer (§4.4, §5.2).
- **(p) `summary` is built by the broker from the tool name and a bounded,
  redacted argument projection**, not by changing 07's `Tool` interface; tools
  may register a summarizer with the broker at startup (§4.3).
- **(q) A permission `Ask` never transitions to `WaitingForInput`.** The wire
  decision `protocol::PermissionDecisionParams` is `{request_id, decision,
  scope}` with no free-text field (`05 §7.6`), so there is no typed-reason path;
  the agent remains in `WaitingForPermission` until decision, timeout, or cancel
  (§4.5–§4.7). `06 §3.3`'s `WaitingForPermission → WaitingForInput` row and its
  "typed choice" sentence are superseded and have been removed from 06.
- **(r) `PermissionPolicy::evaluate` takes one argument and supersedes `§19`'s
  two-argument sketch.** 09 pins `evaluate(const PermissionRequest&) const`;
  `PermissionRequest` carries the execution context (`root`, `path`, `sandbox`,
  `destructive`), so `§19`'s `evaluate(const ToolRequest&, const
  ExecutionContext&)` is superseded (see the `00-architecture.md` §19 errata).
  This closes OQ-P7's signature gap.
- **(s) `Session` grant scope means daemon lifetime.** The name mirrors
  `protocol::PermissionScope::Session` (`05 §7.6`), but the grant lives only in
  daemon memory and is lost on restart (Q13); 01 sessions are durable, so the
  wire name is not the durability boundary (§2.3, §3.6, OQ-P2).

### 12.2 Open questions (adjudicated)

Each item records its disposition; none blocks the component gate. OQ-P7 is
resolved and moved to decision (a)/(r) above.

- **OQ-P1 — Grant-file location. [deferred to v2]** v1 pins
  `<workspace>/.ymh/permissions.local.toml` (decision (e)). An alternative is a
  grants table in the session/workspace store; if chosen, §3.6, `02`, and `03`
  must change together. The file approach is inspectable and single-writer per
  workspace (`§54 D18`), so it stays the v1 default.
- **OQ-P2 — `Session` grant durability across daemon restart. [deferred]** Pinned
  as in-memory daemon-lifetime only (re-ask on restart, fail-safe; decision (s),
  Q13). Making `Session` grants durable is deferred; 01 sessions are durable, so
  the wire name overstates persistence.
- **OQ-P3 — Background resume scheduling. [deferred]** This spec pins that a
  decision is durable and the blocked session is activatable; whether the arbiter
  queues it behind the current active turn or preempts is owned by 04 (m) /
  `06 §6`. Deferred to those specs; the current pin avoids cancelling an
  unrelated running turn (§5.4).
- **OQ-P4 — Automation/ACP permission subset. [deferred]** `05` OQ-3 defers the
  exact Automation method subset to specs 09 and 06. This spec pins that the
  permission pair is included (decision (m)); the exact surrounding subset
  (e.g. whether Automation may set `Always` scope) stays `05` OQ-3.
- **OQ-P5 — Pattern dialect. [deferred]** The path/command glob dialect
  (OQ-P5, §3.3) is a single documented `*`/`**`/`?` form; the command-dimension
  behavior over spaces vs `/` is pinned in §3.3. Shell command matching remains
  heuristic (prefix/glob over the raw command), so `shell` should stay `Ask` by
  default; a robust command parser is out of scope.
- **OQ-P6 — `permission_timeout` default value. [deferred]** The *invariant*
  (bounded, fail-closed) and the `PermissionConfig` field are pinned; the numeric
  default (5 min) is provisional and finalizes with the UI/CLI specs.

---

## 13. References

- `00-architecture.md` §4.1 (agent ≠ UI), §4.2 (event stream is the spine),
  §4.4 (execution environment is a capability boundary), §8.1 (event
  categories; durable `PermissionDecision`), §9.6 (supervisor/daemon topology,
  profiles), §9.7 (isolation, path safety, write lease), §9.8 (lifecycle), §9.9
  (background execution, attention, **background permission policy F2**), §9.11
  (resource limits), §10.1 (`Agent` handle), §11 (agent loop), §14 (tool
  system), §18 (execution environment), **§19 (permissions and policy)**, §20.15
  (`AgentState`), §20.23 (aggregate status line and flash), §20.24
  (cancellation scoping), §34 (cancellation), §35 (concurrency), §37
  (configuration), §38 (profiles), §40 (logging), §42/§43 (headless/RPC),
  §44 (testing strategy), §45 (Fake LLM), §46 (security model), §54 (D1–D23,
  F1–F12), §55 (dsh comparison), §57 Step 6 (shell + permissions), §58
  (milestones).
- `01-session.md` §2.1 (identifiers), §4.3 (`EventType`/wire names), §4.5
  (`ToolCall`/`ToolResult`/`ToolOutcome`/`PermissionDecisionKind`/
  `PermissionDecision`), §5 (EventBus), §9.2 (resume), §13.2 (S1–S14), I12
  (synthesized results), §16.1 (decisions).
- `02-persistence.md` §4.1 (`PersistenceConfig`), §5 (write lease), §6
  (flush/checkpoint), crash recovery.
- `03-workspace-registry.md` (workspace identity, single writer, WAL readers).
- `04-workspace-host-daemon.md` §4.1 (`HostConfig`), §3.5/§3.6 (signals, process
  tree), §8 (`ResourceGovernor`), decision (m) (single-active-session), D-F8
  (client disconnect), H17 (reopen suspended), §11.1 (F2).
- `05-transport.md` §6.2 (profiles), §7.6 (permissions), §8.1/§8.2 (dispatch,
  fan-out), T14 (fan-out isolation), decision (f) (auth), decision (l)
  (permission pair), §14.1 (decisions), §14.2 (OQ-3, OQ-4).
- `06-agent-loop.md` §3.3 (state machine; `WaitingForPermission`), §3.5
  (cancellation), §5.1 (turn pipeline), §5.5 (tool pipeline), §6
  (single-active-session policy), §7 (subagents), A2/A9/A10/A14, A-F6/A-F7.
- `07-tools-execution.md` §3.2 (`ToolSchema.destructive`), §4.3 (initial tool
  set), §5.1 (`ToolContext`), §5.4 (`PermissionHandle`), §6.8 (sandbox modes,
  `ReadOnly`), §8.1 (pipeline mapping), §8.3 (outcome taxonomy), decision (o)
  (`PermissionRequest` deferral), decision (u) (`ReadOnly`), E-F17, X2/X3/X7.
- `08-llm-provider.md` §4.2 (tool-call assembly), decision (k) (`TurnFailed`),
  L-F8 (unknown tool passed through).
