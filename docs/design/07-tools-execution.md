# 07 — Tools & Execution

Tools are the agent's only way to affect the world, so this component is the
**capability boundary** of the whole system (§4.4, §46). It pins the `Tool`
contract, the `ToolRegistry`, the per-call `ToolContext`, the canonical
`ExecutionEnvironment` (§18) that every filesystem/process/PTY/git/LSP path is
resolved through, the sandbox modes, and the resource caps (F8, §9.11). It also
pins how a tool call becomes durable `ToolCall`/`ToolResult` events without any
tool ever appending to the log itself.

This is a **design-only** artifact. It is `written`, not `verified`; no code may
be written for this component until the gate in `HANDOFF.md` §7 passes
(`DESIGN_STATUS.md` row 07).

The design-first rule (`AGENTS.md`) applies: `ToolRegistry`, `ToolContext`, and
`ExecutionEnvironment` signatures here are **pinned** and must not churn after
verification.

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
AgentLoop (spec 06)
   │  builds call from AssistantMessage.tool_use (08 §4.2)
   │  appends payload::ToolCall
   │  PermissionPolicy::evaluate (§19, spec 09)
   │  appends payload::PermissionDecision
   ▼
ToolRegistry (this spec)
   │  validate name + arguments
   ▼
Tool::execute(const ToolContext&, const ToolArguments&)
   │
   ├── ToolContext.execution() ──► ExecutionEnvironment (§18)
   │        ├── Filesystem    (this spec)
   │        ├── ProcessService(this spec)
   │        ├── PtyService    (this spec)
   │        ├── GitService    (this spec)
   │        └── LspService    (Phase 2, §28/§51)
   │
   ├── ToolContext.governor() ──► ResourceGovernor (04 §8, F8/§9.11)
   ├── ToolContext.output()   ──► OutputSink + OutputRing (F5/§9.11)
   └── ToolContext.permission() ─► §19 seam (spec 09)
   │
   ▼
ToolResult  ──► loop appends payload::ToolResult (01 §4.5)
```

The loop (spec 06) drives the pipeline and is the **only** appender of durable
events (`06` A1/A2); this component produces results and live events, never log
records.

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 07)

- The `Tool` contract and the `ToolSchema` type that is exposed to the model
  (`§14.1`; 08 §3.1 defers `ToolSchema` to this spec).
- `ToolRegistry`: registration, lookup, versioning, the initial tool set
  (`§14.3`, `§50`), and deterministic schema exposure to the LLM.
- `ToolContext`: everything a tool receives (session id, the rooted
  `ExecutionEnvironment` — `root()`/`resolve()`, there is no separate `cwd()`
  accessor (§5.1) — cancellation token, permission handle, output sink/ring,
  resource caps).
- `ExecutionEnvironment`: the canonical `§18` interface (`root()`, `resolve()`,
  `fs()`, `process()`, `pty()`, `git()`, `lsp()`), rooted-cwd resolution, and
  realpath canonicalization.
- Filesystem, process, PTY, and git service seams (concrete `LocalEnvironment`).
- Sandbox modes and resource caps (subprocess/PTY/output; F8, §9.11).
- Tool subprocess/PTY **ownership mechanics** (process groups,
  `PR_SET_CHILD_SUBREAPER`, PTY teardown) per `04` OQ-6.
- The tool-output → live-event coalescing contract and the durable
  `ToolResult` mapping (`01` §4.5).

### 1.3 Boundaries — deferred to other specs

- **Durable append, event log, projection.** `Session`, `appendEvent`,
  `deriveMessages`, and all payload types are owned by 01; this spec calls them.
- **Store, payload cap, lease.** `SessionPersistence`, `max_payload_bytes`, and
  the lease are owned by 02 (`02 §4.1`, `§5`).
- **Daemon lifecycle, process tree, SIGCHLD policy, `ResourceCaps` values.**
  Owned by 04 (`04 §3`, `§8`); this spec consumes the governor and owns the tool
  children *inside* the daemon's tree.
- **Transport / RPC surface.** Owned by 05.
- **Loop order, permission timing, terminal events.** Owned by 06
  (`06 §5.1`, `§5.5`).
- **Provider streaming and tool-call assembly.** Owned by 08 (`08 §4.2`); this
  spec receives assembled calls only.
- **Permission policy rules and the decision flow.** Owned by 09 (`§19`); this
  spec references the policy handle and never decides policy.
- **UI projection of tool output.** Owned by 10 (`§20.10`); this spec emits
  bounded live events and a bounded durable result, never `UiEvent`s.

### 1.4 Seam ownership relative to 01/02/04/05/06/08/09/10

| Concern | Owner | This spec's role |
|---|---|---|
| `ToolCall` / `ToolResult` payloads | 01 (`01 §4.5`) | produces `ToolResult`; never appends |
| `ToolOutcome` enum | 01 (`01 §4.5`) | reuses `{Ok, Error, Denied, Cancelled}` verbatim |
| `max_payload_bytes` / payload cap | 02 (`02 §4.1`) | clamps durable output below it |
| Daemon process tree, `setsid`, SIGCHLD | 04 (`04 §3.2`, `§3.6`) | owns tool children, groups, reaping mechanics (OQ-6) |
| `ResourceGovernor` / `ResourceCaps` | 04 (`04 §8`) | acquires/releases caps around spawns |
| `session.activate` / `agent.*` RPCs | 05 | none (tools are reached through the loop) |
| Tool pipeline order, `ToolCall` append timing | 06 (`06 §5.1`, `§5.5`) | provides `execute`; loop appends |
| `ToolSchema`, tool-name validation | **07 (this)** | pinned here (08 §3.1 defers it) |
| `ToolSchema` transport | 08 (`08 §3.1`) | this spec defines; provider maps `name`/`description`/`input_schema` only |
| Permission policy rules | 09 (`§19`) | references `PermissionHandle`; never decides |
| Tool-output rendering | 10 (`§20.10`) | emits bounded live events; never renders |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId`, `TurnId`, `StepId`, `ToolCallId`, and `ToolOutcome` are frozen by
`01 §2.1` / `01 §4.5` and are **reproduced for reference only**. Tool-layer
identities:

```cpp
namespace ymh {

// Stable, provider-visible tool identifier. Dot-namespaced so plugin/MCP tools
// share one namespace without collision (§27): "read_file", "mcp.github.search".
// Grammar: [a-z][a-z0-9_]* ( "." [a-z0-9_]+ )*
struct ToolName {
    std::string value;
    auto operator<=>(const ToolName&) const = default;
};

// The tool's contract version (schema + behavior), independent of the binary
// version. A registration pins (name, version); lookup is by name alone.
struct ToolVersion {
    std::uint32_t major{1};
    std::uint32_t minor{0};
    auto operator<=>(const ToolVersion&) const = default;
};

// Stable machine-readable codes for a failed tool execution. Serialized into
// payload::ToolResult.error as a stable string (01 §4.5); mirrors the error-code
// discipline of 02 §2.2 / 06 §2.2.
enum class ToolErrorCode : std::uint8_t {
    UnknownTool,        // no registration for the name (08 §4.2, L-F8)
    InvalidArguments,   // arguments violate input_schema
    PathEscape,         // resolve() rejected an out-of-root path (F1, §18)
    NotFound,           // fs/git target does not exist
    ResourceExhausted,  // subprocess/PTY cap exhausted (F8, §9.11)
    Timeout,            // process/output deadline exceeded
    Io,                 // filesystem/process I/O failure
    Internal,           // tool invariant violated / unexpected exception
};

std::string_view to_string(ToolErrorCode) noexcept;

class ToolError final : public std::runtime_error {
public:
    ToolError(ToolErrorCode code, std::string message);
    ToolErrorCode code() const noexcept;
};

// Registry errors are fail-loud at load time (never at execution time).
enum class ToolRegistryErrorCode : std::uint8_t {
    DuplicateName,      // same name already registered at another version
    DuplicateVersion,   // identical (name, version) already registered
    InvalidName,        // ToolName grammar violation
    InvalidSchema,      // input_schema malformed or outside the pinned subset
    RegistryFrozen,     // add/remove after freeze()
};

class ToolRegistryError final : public std::runtime_error {
public:
    ToolRegistryError(ToolRegistryErrorCode code, std::string message);
    ToolRegistryErrorCode code() const noexcept;
};

} // namespace ymh
```

`ToolCallId` is provider-supplied and must round-trip verbatim (`01 §2.1`); the
tool layer never mints or rewrites it. `ToolName` is validated at registration
(grammar above) and at execution (`UnknownTool` on miss).

### 2.2 Tool error taxonomy

`ToolErrorCode` is the **tool-layer** vocabulary; the durable vocabulary is
`01`'s `ToolOutcome`:

| `ToolErrorCode` | Resulting `ToolOutcome` | Durable `error` string |
|---|---|---|
| `UnknownTool` | `Error` | `"UnknownTool"` |
| `InvalidArguments` | `Error` | `"InvalidArguments"` |
| `PathEscape` | `Error` | `"PathEscape"` |
| `NotFound` | `Error` | `"NotFound"` |
| `ResourceExhausted` | `Error` | `"ResourceExhausted"` |
| `Timeout` | `Error` | `"Timeout"` |
| `Io` | `Error` | `"Io"` |
| `Internal` | `Error` | `"Internal"` |
| — (policy `Deny`) | `Denied` | (none; `ToolResult.outcome` carries it) |
| — (token fired) | `Cancelled` | (none; `ToolResult.outcome` carries it) |

`ToolOutcome` is reused from `01 §4.5` (`Ok, Error, Denied, Cancelled`); this
spec introduces **no** parallel `Success` value — `Ok` is the success case.

---

## 3. The `Tool` interface and `ToolSchema` (pinned)

### 3.1 `Tool` (§14.1)

`§14.1` is preserved verbatim; this spec pins the concrete argument/result
types and adds identity/version accessors **additively** (the same pattern 06
used for the `§10.1` handle).

```cpp
namespace ymh {

class Tool {
public:
    virtual ~Tool() = default;

    // §14.1 (preserved): the model-facing contract.
    virtual ToolSchema schema() const = 0;

    // §14.1 (preserved): pure execution; no append, no UI, no policy decision.
    virtual Task<ToolResult> execute(const ToolContext&,
                                     const ToolArguments&) = 0;

    // Additive: identity and version, derived from schema() by default.
    virtual ToolName    name() const { return schema().name; }
    virtual ToolVersion version() const { return schema().version; }
};

} // namespace ymh
```

Rules:

- `execute` is an asynchronous task and must observe `ctx.cancellation()`.
- `execute` **must not** append durable events, touch the TUI, or decide
  policy; it returns a `ToolResult` and may emit live events through
  `ctx.emit(...)`.
- A thrown `ToolError` is mapped by the registry to
  `ToolResult{outcome = Error, error = to_string(code)}`; any other exception is
  mapped to `Internal` (E-F16). The loop never sees a tool exception.
- `execute` is called at most once per `payload::ToolCall`; it is not
  idempotent in general, so the loop must not retry a dispatched call (mirrors
  `08` L7's retry barrier).

### 3.2 `ToolSchema` (owned here)

`08 §3.1` states `ToolSchema` is owned by 01/07; 01 does not define it, so this
spec pins it. The provider **maps** it to its wire form (`08 §3.1`, `08 §1.4`):
only `name`, `description`, and `input_schema` are sent; `version` and
`destructive` stay internal to ymh. It is assembled into the request by the
`ContextAssembler` (`06 §5.2`).

```cpp
namespace ymh {

struct ToolSchema {
    ToolName       name;
    ToolVersion    version;             // contract version, not binary version
    std::string    description;         // model-facing; bounded (§4.4)
    nlohmann::json input_schema;        // JSON Schema object (pinned subset)
    bool           destructive{false};  // hint for the permission layer (§19)
};

} // namespace ymh
```

`input_schema` is always a JSON **object** with `"type": "object"` and the keys
`properties`, `required`, and `additionalProperties`. The supported subset is
deliberately small so every provider can map it losslessly:

```text
type: object | string | integer | number | boolean | array
properties: { <name>: <subschema> }
required: [<name>...]
additionalProperties: false          (required; no open objects)
enum: [<scalar>...]                  (optional)
items: <subschema>                   (arrays only)
description: <string>                (per property)
default: <scalar>                    (optional)
```

Unknown/unsupported JSON Schema keywords are rejected at registration
(`InvalidName` is not reused for this; the registry raises
`ToolRegistryError{InvalidName, ...}` for a malformed name and
`ToolRegistryError{InvalidSchema, ...}` for a schema outside the pinned subset,
both at load time — a fail-loud startup error, never a runtime surprise).

### 3.3 `ToolArguments` and validation

```cpp
namespace ymh {

struct ToolArguments {
    nlohmann::json value;    // always a JSON object (08 §4.2, L10)
};

} // namespace ymh
```

The provider already parses fragments to a JSON object at `ToolCallFinished`
(`08 §4.2`, L10), but the **durable** `payload::ToolCall.arguments` is the
source of truth and may be replayed, so the registry re-validates:

```text
validate(name, arguments):
  tool := find(name)
  if tool == null            -> Error{UnknownTool}          (E-F1)
  if arguments is not object -> Error{InvalidArguments}      (E-F2)
  if !schema_validate(tool.schema().input_schema, arguments)
                             -> Error{InvalidArguments}      (E-F2)
  else                        -> proceed to execute
```

- A call is **never** silently dropped and **never** executed unvalidated
  (`§46`, `06` A18, `08 §4.2`).
- Validation is pure and deterministic; it is unit-testable without I/O.
- An unknown tool is a normal `Error` result (the model can recover), not a
  protocol error.

---

## 4. `ToolRegistry` (pinned)

### 4.1 Class shape

```cpp
namespace ymh {

class ToolRegistry {
public:
    // ---- registration (plugin load / daemon startup only) ------------------
    // Returns a move-only RAII handle; dropping it unregisters the tool, which
    // is how a plugin unload reverses its registrations (§7.3).
    [[nodiscard]] Registration add(std::unique_ptr<Tool>);

    // Freeze after the startup wiring completes. After freeze() every add/remove
    // throws ToolRegistryError{RegistryFrozen}; lookup is lock-free.
    void freeze();
    bool frozen() const noexcept;

    std::size_t   size() const noexcept;
    std::uint64_t generation() const noexcept;   // bumps on add/remove

    // ---- lookup ------------------------------------------------------------
    Tool* find(const ToolName&) const noexcept;   // nullptr when absent
    bool  contains(const ToolName&) const noexcept;
    std::vector<ToolName> names() const;          // lexicographic, stable

    // ---- schema exposure to the LLM (§14, 06 §5.2) -------------------------
    std::vector<ToolSchema> schemas() const;      // stable for a generation

    // ---- execution (called only by the loop, 06 §5.1) ----------------------
    Task<ToolResult> execute(const payload::ToolCall&, const ToolContext&);

    class Registration {
    public:
        Registration() noexcept;                     // empty
        Registration(Registration&&) noexcept;
        Registration& operator=(Registration&&) noexcept;
        ~Registration();                             // unregisters if held
        bool held() const noexcept;
    };
};

} // namespace ymh
```

### 4.2 Registration, lookup, and versioning

- **Registration** is confined to plugin load / daemon startup (static plugins,
  `§7.1`: "Do not begin with `dlopen()`"). There is **no hot reload**
  (`§54 D11`, `§55`). `add()` validates the name grammar and the schema subset
  and rejects duplicates before mutating the map (strong guarantee).
- **Duplicate policy.** `(name)` already present at a different version →
  `DuplicateName`; identical `(name, version)` → `DuplicateVersion`. Two tools
  with the same name are never ambiguous: lookup is by name and a second
  version must replace the first only through unload→load, not coexist.
- **Versioning.** `ToolVersion` is the tool's **contract** version. It is
  surfaced in `ToolSchema` so a provider/prompt change is observable, and it is
  used by golden tests to detect accidental schema drift. It is *not* a
  compatibility negotiation in v1 (one binary, one tool set).
- **Lookup.** `find`/`contains` are O(1) and lock-free because the registry is
  frozen before serving; `names()` returns a lexicographically sorted copy so
  `schemas()` is deterministic (`X14`).
- **Generation.** `generation()` increments on every add/remove. The
  `ContextAssembler` (06 §5.2) may cache `schemas()` keyed by generation; a
  mismatch invalidates the cache (E-F15).
- **Lifetime.** Tools are owned by the registry; `Registration` is the RAII
  unregister handle. A plugin owns all registrations it creates (`§7.3`).

### 4.3 Initial tool set (§14.3, §50)

The `§14.3` set, split by MVP (`§50`, `§57` Steps 5/6/9) and Phase 2 (`§51`):

| Tool | Kind | MVP | Permission default (§19) | Notes |
|---|---|---|---|---|
| `read_file` | fs | ✓ | ALLOW | `{path, offset?, limit?}`; bounded read |
| `edit_file` | fs | ✓ | ASK | `{path, old_string, new_string, replace_all?}`; exact-match |
| `write_file` | fs | Phase 2 | ASK | `{path, content}`; whole-file write (see OQ-7) |
| `grep` | search | ✓ | ALLOW | `{pattern, path?, glob?, output_mode?, line_numbers?}` |
| `glob` | search | ✓ | ALLOW | `{pattern, path?}`; sorted relative paths |
| `shell` | process | ✓ | ASK | `{command, timeout_ms?}` → `/bin/bash -lc` |
| `git_status` | git | ✓ | ALLOW | `{}` |
| `git_diff` | git | ✓ | ALLOW | `{path?, staged?, ref?}` |
| `git_log` | git | Phase 2 | ALLOW | `{max_count?, path?}` |
| `git_show` | git | Phase 2 | ALLOW | `{ref}` |
| `git_branch` | git | Phase 2 | ALLOW | `{}` |
| `git_checkout` | git | Phase 2 | ASK | `{ref, create?}`; mutates the worktree |
| `spawn_subagent` | agent | v1 | ASK | provided via 06 §7; awaits `whenIdle()` synchronously. `00 §51` schedules subagents in Phase 2 — see OQ-8 |
| `lsp_*` | lsp | Phase 2 | ALLOW | `§28`; only when `lsp() != nullptr` |

Concrete MVP schemas (abridged; the full JSON is the golden fixture):

```text
read_file   { path: string, offset?: integer, limit?: integer }
edit_file   { path: string, old_string: string, new_string: string,
              replace_all?: boolean }
grep        { pattern: string, path?: string, glob?: string,
              output_mode?: "content"|"files_with_matches"|"count",
              line_numbers?: boolean }
glob        { pattern: string, path?: string }
shell       { command: string, timeout_ms?: integer }
git_status  { }
git_diff    { path?: string, staged?: boolean, ref?: string }
```

Rules:

- **Small Git surface.** Per `§14.3`, the model gets `git_status`/`git_diff`
  in MVP and `shell` covers the rest; the extra Git tools arrive in Phase 2
  rather than one tool per trivial operation.
- **`shell` never builds a shell string when argv suffices** (`§16`). The
  `shell` tool intentionally requires a shell and therefore runs
  `/bin/bash -lc "<command>"`; the `command` string is passed as a **single
  argv element**, never concatenated, and still passes the permission gate
  (`§16`, `§46`).
- **`spawn_subagent`** is a tool, but the subagent machinery (registry, child
  session, fan-in) is owned by 06 §7; this spec owns only its schema and that it
  returns a `ToolResult` reflecting `SubagentFanIn.outcome`.
- **Phase 2 tools are absent from `schemas()`** until the backing service
  exists, so the model is never offered a tool that cannot run (`LspService` is
  `nullptr` in v1).
- **Phases follow `00`, with one recorded conflict.** PTY-backed tooling, LSP,
  and the extra Git tools are Phase 2 (`00 §51`, `§57` Step 12); the PTY/LSP
  seams are pinned now so no tool changes when they land (§6.5, §6.7, decision
  (s)). `write_file` is Phase 2 here per `00 §50`/`§57` Step 5 even though
  `00 §14.3`/G4 list it as initial (OQ-7). `spawn_subagent` is **v1** per the
  verified `06 §7` (synchronous subagent), which conflicts with `00 §51`'s
  Phase 2 listing — surfaced as OQ-8, not silently resolved either way.

### 4.4 Schema exposure to the LLM

- `schemas()` is the **only** source of model-facing tool definitions; the
  `ContextAssembler` calls it (`06 §5.2`) and the provider **maps** each
  `ToolSchema` to its wire form (`08 §3.1`): only `name`, `description`, and
  `input_schema` cross the process boundary; `version` and `destructive` stay
  internal.
- Ordering is lexicographic by `ToolName.value` and stable for a generation, so
  provider prompts and golden tests are reproducible (`X14`, `§44`, `§45`).
- `description` strings are bounded by `ToolConfig::tool_description_max_bytes`
  (default 4 KiB, §5.5) and must not embed secrets or machine-local absolute
  paths (`§40`).
- Empty registry ⇒ `LLMRequest.tools` is empty ⇒ no tool calling
  (`08 §3.1`), which is the correct behavior for a pure-chat configuration.

---

## 5. `ToolContext` (pinned)

### 5.1 Class shape (`§14.2` + additive)

`§14.2` is preserved, with its accessors made `const` so a `const ToolContext&`
— the `Tool::execute` parameter (§3.1) — is usable; this spec adds the fields
the task requires (session id, environment root, cancellation token, permission
handle, output sink/ring, resource caps) **additively**.

```cpp
namespace ymh {

class ToolContext {
public:
    // Loop-owned (06 §5.1): the loop constructs exactly one per dispatched
    // payload::ToolCall and destroys it once execute() returns. Non-owning —
    // every reference must outlive the call — and never constructed by a tool.
    ToolContext(ExecutionEnvironment& execution,
                Session&              session,
                Logger&               logger,
                CancellationToken     cancellation,
                ResourceGovernor&     governor,
                OutputSink&           output,
                PermissionHandle&     permission,
                ToolCallId            call_id,
                TurnId                turn,
                StepId                step);

    // ---- §14.2 (preserved; accessors const) -------------------------------
    ExecutionEnvironment& execution() const noexcept;
    Session&              session() const noexcept;
    Logger&               logger() const noexcept;
    CancellationToken     cancellation() const noexcept;
    void                  emit(Event) const;            // live events only

    // ---- additive pins (this spec) ----------------------------------------
    ToolCallId            callId() const noexcept;      // == ToolCall::id
    SessionId             sessionId() const noexcept;
    TurnId                turn() const noexcept;
    StepId                step() const noexcept;

    // Rooting is delegated, never recomputed: both forward to execution().
    const std::filesystem::path& root() const noexcept;             // §18
    std::filesystem::path        resolve(std::string_view) const;   // §18

    ResourceGovernor&     governor() const noexcept;    // caps (04 §8, F8)
    OutputSink&           output() const noexcept;      // streaming + ring (F5)
    PermissionHandle&     permission() const noexcept;  // §19 seam (spec 09)
};

} // namespace ymh
```

Rules:

- **No UI access** (`§14.2`): tools never include or construct UI types
  (`§4.1`, `§20.1`, `D15`, `X15`).
- **Live-only emit.** `emit()` publishes a **live** event (`§8.1`) routed to the
  owning session (`06 §5.5`); it is best-effort and coalesced. Durable
  `ToolCall`/`ToolResult`/`PermissionDecision` records are appended **only** by
  the loop (`X8`).
- **Lifetime.** The context is per-call, non-owning, and valid only for the
  duration of `execute`; a tool must not retain references to it.
- **Loop-owned construction.** The loop is the **only** constructor of
  `ToolContext` (06 §5.1 `toolRegistry.execute(call, ctx)`); there is no default
  constructor and no tool-facing factory. The context is not copyable or
  movable once handed to `execute`.
- **Rooting.** `root()` and `resolve()` forward to `execution()`; a tool never
  recomputes a root from `getcwd()` (`X1`, `§9.7`, `§18`). There is deliberately
  no `cwd()` accessor — a tool that needs a working directory calls
  `resolve(".")`.

### 5.2 Output sink and ring buffer (F5)

```cpp
namespace ymh {

class OutputSink {
public:
    // Streaming delta from a tool (stdout/stderr interleave, raw/PTY bytes). The
    // sink is the UTF-8 boundary: it lossy-converts bytes to valid UTF-8 before
    // they enter the ring, because ToolResult.output is JSON text (01 S10).
    void write(std::string_view chunk);
    void writeErr(std::string_view chunk);   // stderr channel; same ring, tagged
    void close();                            // idempotent; final flush barrier

    std::size_t bytesWritten() const noexcept;   // total produced (may exceed cap)
    bool        truncated() const noexcept;      // ring wrapped or UTF-8 loss (F5)

    // Bounded UTF-8 snapshot for the durable payload::ToolResult.output. The
    // escaping-aware serialized-size clamp is applied by the loop (below).
    std::string materialize(std::size_t maxBytes) const;
};

// Owned by 07 (decision (t); 04 §8 returns it). One per session; bounded,
// tail-keeping, non-blocking, live-only (never durable).
class OutputRing {
public:
    explicit OutputRing(std::size_t capacity_bytes);

    // Appends one UTF-8 chunk; on overflow evicts the oldest bytes and sets
    // truncated. Never blocks and never grows beyond capacity.
    void        append(std::string_view chunk, bool is_stderr);
    std::size_t size() const noexcept;              // bytes currently retained
    bool        truncated() const noexcept;         // any bytes were evicted
    std::string tail(std::size_t max_bytes) const;  // most recent bytes
};

} // namespace ymh
```

- **One ring per session**, obtained from `ResourceGovernor::ringFor(SessionId)`
  (`04 §8`). The ring type is `OutputRing`, **defined here** (decision (t));
  capacity is `session_output_ring_bytes` (1 MiB) or `active_output_ring_bytes`
  (4 MiB) for the active session (`04 §2.1`).
- `write`/`writeErr` append to the ring, increment `bytesWritten`, and set
  `truncated` once the total exceeds capacity. The ring keeps the **tail** (most
  recent bytes) because the end of a build/test log is what matters.
- **UTF-8 boundary.** Raw process and PTY bytes are not guaranteed to be UTF-8,
  but `ToolResult.output` is JSON text (`01` S10). The sink therefore
  lossy-converts each chunk to valid UTF-8 before appending; bytes dropped by
  that conversion set `truncated` (E-F10, §8.2). A non-UTF-8 fixture must not
  produce invalid JSON downstream.
- `materialize(maxBytes)` returns at most `maxBytes` of UTF-8, preferring the
  tail, and reports truncation through the caller; it never allocates unbounded
  memory.
- **The ring is live-only.** It is not durable and not itself an event; only the
  loop's `payload::ToolResult` is durable (`§8.1` persistence rule).
- **Durable cap is on the serialized payload, not raw bytes.** `01` S10 (with `02 §4.1`) caps the
  *serialized* payload JSON (`max_payload_bytes`), and JSON string escaping of
  control characters can inflate text roughly 6×, so clamping raw output to
  1 MiB does **not** guarantee the payload fits. The loop therefore clamps on
  the serialized size: it serializes the complete `payload::ToolResult` envelope
  (with `output` embedded) and truncates `output` until
  `json(result).dump().size()` ≤ `tool_result_max_bytes` (default 1 MiB), which
  **must** be ≤ `PersistenceConfig::max_payload_bytes` (4 MiB, `02 §4.1`) minus
  envelope headroom. Equivalently, budget raw `output` at ≤
  `max_payload_bytes / 6` for the worst-case-escaped payload. A tool result can
  therefore never trigger `PayloadTooLarge` (`01` S10, `02 §2.2`, `X10`, E-F11).
  Values live in `ToolConfig` (§5.5).

### 5.3 Cancellation scoping (F9, §34)

- `cancellation()` is the **session-scoped** token (`§34`, `F9`). Ctrl-C cancels
  only the active session's in-flight turn and its tool children; other sessions
  are untouched (`06` A9, `§20.24`).
- On cancel the tool returns `ToolResult{outcome = Cancelled}`; the loop appends
  it (`01 §4.5`). Cancellation is **not** an error (`06` A10).
- Subprocess teardown: SIGTERM to the tool's **process group**, wait
  `ToolConfig::terminate_grace` (default 2 s, §5.5), then SIGKILL. PTY teardown:
  SIGHUP to the session, then SIGKILL. A tool that ignores the token past the
  grace is force-killed and still reports `Cancelled`.
- Partial output produced before cancellation is retained (best-effort) and
  marked `truncated` if the stream was cut mid-write.
- The second Ctrl-C may be defined as force termination (`§34`); this spec
  guarantees the first one is always sufficient.

### 5.4 Permission handle integration (`§19`)

The primary decision is made **by the loop before execution** (`06 §5.1`:
append `ToolCall` → `evaluate` → append `PermissionDecision` → `execute`).
`ToolContext::permission()` exposes the §19 seam to the tool:

```cpp
namespace ymh {

// Referenced, not owned, by this spec: the concrete shape is co-owned with
// spec 09 (§19). This pin is provisional until 09 lands (decision (o)).
class PermissionHandle {
public:
    virtual ~PermissionHandle() = default;   // referenced polymorphically

    // The decision already recorded for this call (06 §5.1).
    virtual PermissionDecisionKind decision() const noexcept = 0;

    // Optional: evaluate a sub-operation a tool can only discover at execution
    // time (e.g. a decomposed pipeline). v1 tools do not call this; the single
    // pre-execution decision suffices. Policy rules live in spec 09.
    // Deferred to spec 09: `PermissionRequest` is NOT defined here — spec 09 must
    // define it and reuse `PermissionDecisionKind`; only the seam is pinned now.
    Task<PermissionDecisionKind> evaluate(const PermissionRequest&) = 0;
};

} // namespace ymh
```

Rules:

- A tool **never** executes a `Deny` decision; on `Deny` the loop has already
  produced `ToolResult{outcome = Denied}` and `execute` is not called
  (`06 §5.5`). A tool never returns `Denied` itself (§8.3).
- `Ask` is resolved by the loop (`WaitingForPermission`, `06 §3.3`); by the
  time `execute` runs the decision is durable (`F2`).
- A missing/unwired policy is a **startup** error, never a runtime bypass
  (E-F17). The policy itself is spec 09; this spec only wires the handle.
- **Deferred to 09.** `PermissionRequest` is intentionally undefined here; spec
  09 (§19) must define it and reuse `PermissionDecisionKind`. This is a recorded
  deferral, not an open question (decision (o)).

### 5.5 `ToolConfig` (pinned)

The tunable bounds named across this spec have no owning type elsewhere; 07 pins
them in one struct (cf. `PersistenceConfig` in `02 §4.1`, `HostConfig` in
`04 §4.1`). The daemon constructs `ToolConfig` alongside `ResourceCaps` and
injects it into the loop; `ToolContext`/services read it, never global state.

```cpp
namespace ymh {

struct ToolConfig {
    // Durable output clamp on the serialized payload (§5.2, X10). Must be
    // <= PersistenceConfig::max_payload_bytes minus envelope headroom.
    std::size_t               tool_result_max_bytes{1u * 1024u * 1024u};

    // Filesystem read bound (§6.3).
    std::size_t               read_file_max_bytes{256u * 1024u};

    // glob/grep result bound (§6.3).
    std::size_t               search_max_results{1000};

    // Model-facing description bound (§4.4).
    std::size_t               tool_description_max_bytes{4u * 1024u};

    // Live output coalescing (§8.2).
    std::chrono::milliseconds output_flush_interval{33};      // ~30 Hz
    std::size_t               output_flush_bytes{64u * 1024u};

    // Subprocess/PTY teardown grace (§5.3, §6.5, §9.3).
    std::chrono::milliseconds terminate_grace{2'000};
};

} // namespace ymh
```

- Ring **capacity** is not here: it is `session_output_ring_bytes` /
  `active_output_ring_bytes` from `ResourceCaps` (`04 §2.1`), because the ring is
  a per-host resource. `shutdown_grace` is 04's (`04 §4.1`), not a tool bound.
- `tool_result_max_bytes` must be ≤ `PersistenceConfig::max_payload_bytes`
  (`02 §4.1`) minus envelope headroom; the serialized-size clamp (§5.2) enforces
  the rest at runtime.

---

## 6. `ExecutionEnvironment` (pinned, §18)

### 6.1 Canonical interface

`§18` is the canonical interface and is reproduced here with the concrete
implementations and semantics pinned. `LocalEnvironment` is the only v1
implementation; the environment root is baked in **before any tool is written**
(`§18`, `§57` Step 11). `mode()` is an **additive** pin (the same pattern 06 used
for `§10.1`): `§18`'s members are unchanged, and `mode()` only makes the
confinement policy `resolve()` already needs explicit (decision (r)).

```cpp
namespace ymh {

class ExecutionEnvironment {
public:
    virtual ~ExecutionEnvironment() = default;

    virtual const std::filesystem::path& root() const = 0;
    virtual std::filesystem::path resolve(std::string_view) const = 0;

    // The confinement policy `resolve()` obeys (§6.2, §6.8). Immutable per
    // environment; selected by config, never by the model (decision (r)).
    virtual SandboxMode mode() const noexcept = 0;

    virtual Filesystem&      fs() = 0;
    virtual ProcessService&  process() = 0;
    virtual PtyService&      pty() = 0;
    virtual GitService&      git() = 0;
    virtual LspService*      lsp() = 0;      // nullptr until Phase 2 (§28/§51)
};

// v1 implementation; the root is canonical and immutable for the daemon's life.
class LocalEnvironment final : public ExecutionEnvironment {
public:
    explicit LocalEnvironment(std::filesystem::path canonical_root,
                              SandboxMode mode = SandboxMode::Workspace);
    // ...
};

} // namespace ymh
```

Implementations named by `§18`:

```text
LocalEnvironment       v1 (this spec)
SandboxEnvironment     Phase 3 (§52)
ContainerEnvironment   Phase 3
SSHEnvironment         Phase 3 / Mode B (§47)
```

- The daemon constructs exactly one `LocalEnvironment` rooted at the canonical
  workspace root during startup (`04 §3.3` step 6) and shares it across sessions
  (`04 §3.7`); every `ToolContext` holds a reference.
- `lsp()` is `nullptr` in v1 and non-null only when the LSP service is built
  (`§51`); the `lsp_*` tools are absent from the registry while it is null.

### 6.2 Rooting and path resolution (no `getcwd()`)

`root()` and `resolve()` are the **rooting contract** (`§9.7`, `§18`):

```text
resolve(p):
  if p is empty                               -> throw ToolError{PathEscape}
  base := root()                              # canonical absolute realpath (X3)
  candidate := (p is absolute) ? p : base / p

  if mode() == Unrestricted:                  # explicit, config-selected opt-out
      return lexically_normal(candidate)      # still cleaned; containment skipped

  # root-confined modes: Workspace, ReadOnly
  parent := weakly_canonical(candidate.parent_path())   # handles non-existent writes
  require is_within(parent, base)             -> else PathEscape

  final := parent / candidate.filename()
  if exists(final):
      final := canonical(final)               # realpath: resolves a final symlink
  require is_within(final, base)              -> else PathEscape   # symlink-escape
  return final

is_within(x, base):                             # component-wise; BOTH operands realpath
  x := weakly_canonical(x)                      # idempotent when already canonical
  b := weakly_canonical(base)
  # Compare path components from the filesystem root; require b to be a
  # component-wise prefix of x. Never a string-prefix test: /a/bc is NOT in /a/b.
  return component_prefix(b, x)
```

Rules:

- **Canonicalization.** Every workspace path is normalized with
  `std::filesystem::canonical()` (realpath: trailing slashes, `.`/`..`, and
  symlinks resolved) at registration/attach time; uniqueness is canonical
  string equality (`§9.7`, DIV-8, `§18`). `weakly_canonical` is used for the
  parent of a write target that does not exist yet.
- **The final component is canonicalized when it exists**, so a symlink at the
  last component cannot escape root; when it does not exist, the canonical
  parent plus the lexical filename is returned (a create path). `is_within`
  normalizes **both** operands with `weakly_canonical`, so containment cannot be
  defeated by a non-canonical `base` or a `.`/`..`-laden argument.
- **Containment is component-wise**, not a string prefix, so `/a/bc` is never
  treated as inside `/a/b`.
- **`getcwd()` is never a resolution base in tool code** (`§9.7`, `§18`, `X1`).
  The daemon's one-time `chdir(workspace_root)` at startup (`04 §3.3` step 2)
  makes relative paths *incidentally* correct, but tools must never depend on
  it; resolution is root-relative and `getcwd()`-independent.
- **`Unrestricted` is the single explicit exception.** Only when
  `mode() == Unrestricted` (config-selected, never model-selected; §6.8) does
  `resolve()` skip containment; it still lexically normalizes and still passes
  the permission policy. `X2` is qualified accordingly.
- **Every path** used by filesystem, process `cwd`, PTY, git, and LSP builders
  goes through `resolve()` (`§9.7`, `§18`, `X1`).
- **Defense-in-depth against TOCTOU.** `resolve()` alone cannot close the window
  between the containment check and the actual syscall, so the concrete
  filesystem/process builders open the final component with
  `openat2(dirfd = root(), rel, RESOLVE_BENEATH)` (Linux ≥ 5.6) and `O_NOFOLLOW`
  on the final open; `openat` + `O_NOFOLLOW` is the fallback. `RESOLVE_BENEATH`
  rejects any resolution (including an absolute symlink target) that leaves the
  root dirfd, and `O_NOFOLLOW` rejects a final component swapped to a symlink
  after resolution. This is optional in v1 (`§18`) but is the pinned hardening.
- An escape throws `ToolError{PathEscape}` (E-F3); the tool maps it to an
  `Error` result.

### 6.3 `Filesystem`

```cpp
namespace ymh {

class Filesystem {
public:
    virtual Task<FileData> read(const std::filesystem::path&) = 0;
    virtual Task<void>     write(const std::filesystem::path&, const Data&) = 0;
    virtual Task<void>     remove(const std::filesystem::path&) = 0;
    virtual Task<FileInfo> stat(const std::filesystem::path&) = 0;
    virtual Task<std::vector<std::filesystem::path>> glob(const GlobQuery&) = 0;
    virtual Task<std::vector<GrepMatch>>             grep(const GrepQuery&) = 0;
};

} // namespace ymh
```

- Methods receive **already-resolved** absolute paths; `LocalEnvironment::fs()`
  re-verifies containment on writes as defense-in-depth (`§15`).
- `read` is bounded by `ToolConfig::read_file_max_bytes` (default 256 KiB,
  §5.5); a longer file is read in `offset`/`limit` windows and reports
  truncation.
- `glob`/`grep` results are bounded by `ToolConfig::search_max_results`
  (default 1000, §5.5) and sorted deterministically for replay/golden
  stability.
- Policy can restrict `workspace root`, `allowed paths`, `denied paths`, and
  `read-only paths` (`§15`); path handling prevents traversal outside the
  workspace (`§15`).

### 6.4 `ProcessService`

```cpp
namespace ymh {

struct ProcessRequest {
    std::string               executable;      // absolute or PATH-resolved
    std::vector<std::string>  argv;            // never a concatenated shell string
    std::filesystem::path     cwd;             // MUST be resolve()d under root()
    std::vector<std::pair<std::string, std::string>> environment;
    std::chrono::milliseconds timeout{0};      // 0 => no deadline
    bool                      capture_stdout{true};
    bool                      capture_stderr{true};
    OutputSink*               sink{nullptr};   // streaming; null => buffered
};

struct ProcessResult {
    int  exit_code{-1};
    bool signalled{false};
    int  signal{0};
    bool timed_out{false};
};

class ProcessService {
public:
    virtual Task<ProcessResult> run(const ProcessRequest&,
                                    CancellationToken) = 0;
};

} // namespace ymh
```

Rules:

- **argv-first** (`§16`): avoid constructing shell command strings whenever an
  argv representation suffices. The `shell` tool uses `/bin/bash -lc` explicitly
  and passes the command as one argv element (`§16`).
- **Process group.** The child is placed in its own process group
  (`setpgid(0, 0)` between fork and exec) so the tool can signal the whole tree
  without touching the daemon or sibling tools (`X13`, `§9`).
- **cwd** must be a `resolve()`d path; the child never inherits the daemon's
  cwd implicitly (`§18`).
- **fd hygiene**: close all fds > 2 not marked `O_CLOEXEC` (matches `04 §3.2`).
- Output is streamed to `sink` when present and also captured up to the caps.
- A process that runs to completion with a non-zero exit is `outcome = Ok` with
  the exit code in the output; only spawn failure/timeout/cancel produce
  `Error`/`Cancelled` (decision (h), E-F5/E-F6).

### 6.5 `PtyService`

```cpp
namespace ymh {

class PtySession {
public:
    virtual void write(std::string_view) = 0;
    virtual Stream<std::string> output() = 0;
    virtual void resize(int rows, int cols) = 0;
    virtual void terminate() = 0;   // SIGHUP -> grace -> SIGKILL
};

class PtyService {
public:
    virtual Task<std::unique_ptr<PtySession>> open(const PtyRequest&,
                                                   CancellationToken) = 0;
};

} // namespace ymh
```

- PTY is a **separate capability** from one-shot process execution (`§17`); it
  enables interactive shells, curses apps, build progress, long-running
  commands, and a future persistent terminal tool.
- **Phase.** `00 §51` schedules PTY in Phase 2. This spec pins the
  `PtyService`/`PtySession` seam now (so no tool changes when it lands) but no
  v1 tool is PTY-backed; PTY-backed tooling and its live tests are Phase 2
  (decision (s)).
- The PTY slave is opened with `O_NOCTTY` (`04 §3.2`, H3) so the daemon never
  acquires a controlling terminal.
- `terminate()` is idempotent; closing the master delivers SIGHUP to the
  foreground process group, then SIGKILL after `ToolConfig::terminate_grace`
  (§5.5).
- PTY ownership, teardown, and cap accounting are this spec's (§9).

### 6.6 `GitService`

```cpp
namespace ymh {

class GitService {
public:
    virtual Task<GitStatus> status(const GitQuery&) = 0;
    virtual Task<GitDiff>   diff(const GitQuery&) = 0;
    virtual Task<GitLog>    log(const GitQuery&) = 0;
    virtual Task<GitShow>   show(const GitQuery&) = 0;
    virtual Task<GitBranch> branch(const GitQuery&) = 0;
    virtual Task<void>      checkout(const GitQuery&) = 0;
};

} // namespace ymh
```

- Backed by libgit2 (`§48`) or by `ProcessService` invoking `git` with an argv
  (never a shell). Repository paths are `resolve()`d through the environment.
- `checkout` mutates the worktree and is `ASK` by default (`§19`); it is Phase 2
  (MVP ships `git_status`/`git_diff` only, `§50`, `§57` Step 9).

### 6.7 `LspService`

- Phase 2 (`§51`); `lsp()` returns `nullptr` in v1. LSP lives behind the
  execution environment (`§28`).
- When built, it exposes `definition`, `references`, `hover`, `diagnostics`,
  `symbols`, `completion` (`§28`) and the model-facing tools
  `lsp_definition`, `lsp_references`, `lsp_diagnostics`, `lsp_symbols` register
  with it (`§28`).

### 6.8 Sandbox modes

```cpp
namespace ymh {

enum class SandboxMode : std::uint8_t {
    Workspace,     // v1 default: root-confined FS; daemon uid; caps apply
    ReadOnly,      // inspection only: no writes, no subprocesses, no PTYs
    Unrestricted,  // explicit opt-out: resolve() skips root containment
};

} // namespace ymh
```

- `SandboxMode` is selected per daemon/session from configuration and carried in
  the execution context; it is **not** model-controlled (decision (r)). It is
  exposed through `ExecutionEnvironment::mode()` and read by `resolve()` (§6.2).
- **v1 enforcement is layered and honest** (`§46`, `X16`): root containment via
  `resolve()` (`X2`), the permission policy (`§19`), and resource caps
  (`§9.11`). It is **not** an OS jail — no `seccomp`, `landlock`, or namespaces
  in v1.
- `ReadOnly` rejects mutating tools (`write_file`/`edit_file`/`shell`/`pty`/
  `checkout`) at the **policy layer**: the loop records `PermissionDecision{Deny}`
  and `ToolResult{outcome = Denied}`, and `execute` is **not** called (`X7`).
  `Denied` is produced only by the loop; a tool never returns it (§8.3). A
  read-only violation that somehow reaches a tool is an `Error` (§2.2), never
  `Denied`.
- `Unrestricted` makes `resolve()` skip root containment (§6.2) but **not** the
  permission policy or resource caps; it is an explicit, config-selected opt-in,
  never model-controlled.
- OS-level isolation (`SandboxEnvironment`, `ContainerEnvironment`) is Phase 3
  (`§52`, `§53`); the `ExecutionEnvironment` seam exists so it can arrive without
  touching a single tool (`§4.4`).

---

## 7. Resource caps and scheduling (F8, §9.11)

### 7.1 `ResourceGovernor` integration

Caps are **per host** (one daemon), because each daemon owns its LLM pool,
subprocesses, PTYs, and buffers (`§9.11`, `04 §8`). The daemon constructs
exactly one `ResourceGovernor` from `ResourceCaps` and passes it to the
session/agent layer; this spec consumes it and **never re-implements a cap**
(`04 §8`).

```cpp
namespace ymh {

class ResourceGovernor {                  // owned by 04; consumed here
public:
    LLMPool& llm();                       // pinned by 06 §5.9 (not used by tools)
    bool tryAcquireSubprocess(SessionId);
    void releaseSubprocess(SessionId);
    bool tryAcquirePty(SessionId);
    void releasePty(SessionId);
    OutputRing& ringFor(SessionId);       // F5 output ring (this spec)
};

} // namespace ymh
```

The concrete `ResourceCaps` values are owned by `04 §2.1`:

```text
max_global_subprocesses   32     F8
max_session_subprocesses   8     F8
max_global_ptys            8     F8
max_session_ptys           2     F8
session_output_ring_bytes  1 MiB F5
active_output_ring_bytes   4 MiB F5
```

### 7.2 Subprocess and PTY caps

- **Acquire before spawn, release on every path** (`X11`): a tool calls
  `tryAcquireSubprocess`/`tryAcquirePty` immediately before spawning and holds
  the slot in an RAII guard that releases on completion, timeout, cancellation,
  and exception.
- Exhaustion returns `false`; the tool yields
  `ToolResult{outcome = Error, error = "ResourceExhausted"}` (E-F8/E-F9).
- **The rejection is durable** (`04 §8`): the loop appends the `ToolResult`, so
  a cap rejection is never a silent drop.
- A bounded LLM cap does **not** bound tool subprocesses; the two are
  independent (`04 §8`, F8).
- `tryAcquirePty` is exercised only once PTY tooling lands (Phase 2, `00 §51`);
  the cap and seam are pinned now (§6.5).
- `fd`/memory soft limits (`§9.11`) are applied to children in Phase 2
  (`setrlimit(RLIMIT_NOFILE/RLIMIT_AS)`).

### 7.3 Output ring buffers (F5)

The normative ring contract is §5.2 (`OutputSink`/`OutputRing`). This section
records only the scheduling rationale, not a second set of rules:

- Per-session rings bound memory when N sessions × M subagents each run
  `grep -R`, `cmake`, `ninja`, or `pytest` (`§9.11`, F5, `§20.10`).
- Full tool output is materialized **only** for the active session and expanded
  subagents; others keep the bounded ring (`§9.11`, `04 §8`).

### 7.4 Cancellation scoping (F9)

Cancellation scoping is normative in §5.3. From the governor's view there is one
additional rule, and no other: a cancelled child releases its subprocess/PTY slot
through the §7.2 RAII guard, so cancellation never leaks a cap (`X11`, `X12`).

---

## 8. Tool pipeline → durable events

### 8.1 `ToolCall` and `ToolResult` mapping

The loop is the sole appender (`06 §5.1`, `X8`). The mapping is:

```text
loop:  append payload::ToolCall{id, turn, step, name, arguments, requestedAt}
loop:  decision := policy.evaluate(call, session)          (§19, spec 09)
loop:  append payload::PermissionDecision{call, decision, reason}
       if Denied:
loop:      append payload::ToolResult{id, name, Denied}    (execute skipped)
       else:
registry:  result := ToolRegistry::execute(call, ctx)      (this spec)
loop:      append payload::ToolResult{id, name, result.outcome,
                                       result.output, result.truncated,
                                       result.error, result.duration}
```

The durable payloads are frozen by `01 §4.5`:

```cpp
struct ToolCall {
    ToolCallId      id;          // == the tool_use block id in the assistant message
    TurnId          turn;
    StepId          step;
    std::string     name;
    nlohmann::json  arguments;
    std::chrono::system_clock::time_point requestedAt;
};

enum class ToolOutcome : std::uint8_t { Ok, Error, Denied, Cancelled };

struct ToolResult {
    ToolCallId                 id;         // pairs with ToolCall::id
    std::string                name;
    ToolOutcome                outcome;
    std::string                output;     // possibly ring-truncated (F5, §9.11)
    bool                       truncated = false;
    std::optional<std::string> error;
    std::chrono::milliseconds  duration{0};
};
```

Rules:

- **Pairing is exact.** `payload::ToolResult.id` equals `payload::ToolCall.id`
  equals the `tool_use` block id (`01 §2.1`, `01` I12, `08` L9).
- **Totality.** Every call produces exactly one `ToolResult` with
  `outcome ∈ {Ok, Error, Denied, Cancelled}` (`X9`); the projection synthesizes
  results for unmatched `tool_use` blocks on cancel/failure (`01` I12).
- **Tools never append**; they return a `ToolResult` and may `emit` live events
  (`X8`, `06 §5.5`).
- `output` is clamped on the **serialized** payload size (escaping-aware) before
  append, so the whole `payload::ToolResult` fits `tool_result_max_bytes`
  (§5.2, `X10`).

### 8.2 Output streaming → coalescing contract

```text
tool writes ──► OutputSink ──► OutputRing (bounded, tail)
                    │
                    ├── live: coalesced batch ──► emit(TerminalOutput/Progress)
                    │        (bounded by bytes + ~30–60 Hz, §20.25)
                    │
                    └── durable: one bounded snapshot at close() ──►
                             loop appends payload::ToolResult.output
```

- **Live** flushes are coalesced: at most one flush per
  `ToolConfig::output_flush_interval` (default 33 ms ≈ 30 Hz, §5.5) or per
  `ToolConfig::output_flush_bytes` (default 64 KiB), whichever first. This
  mirrors the `AssistantChunk` coalescing discipline (`01 §16.1(c)`, `02 §6.2`)
  and the `§20.25` 30–60 Hz subagent cadence.
- **UTF-8 at the sink.** Raw/PTY bytes are lossy-converted to UTF-8 at the
  `OutputSink` (§5.2) before any live flush or durable snapshot, so neither the
  coalesced `emit` payload nor `ToolResult.output` can carry invalid UTF-8 into
  JSON serialization (`01` S10). Loss sets `truncated` (E-F10).
- **Durable** output is written **once**, by the loop, at the `ToolResult`
  barrier — never incrementally. A live consumer can therefore observe output
  that a replay would not (live-only), but never the reverse (`01` I4,
  durable-before-observable).
- The contract is testable with a fake sink: given a known chunk sequence and a
  fixed clock, the live batch sequence and the final durable snapshot are
  byte-stable golden fixtures.
- The ring never blocks a tool: `write` is non-blocking and bounded; when the
  ring is full it overwrites the oldest bytes and sets `truncated` (F5).

### 8.3 Permission integration and outcome taxonomy

- The §46 chain is enforced structurally:
  **LLM output → tool parser → permission policy → execution environment → OS**
  (`§46`). `LLM → shell → OS` without the policy layer is impossible because
  `Tool::execute` is reachable only through `ToolRegistry::execute`, which the
  loop calls only after appending a `PermissionDecision` (`X7`).
- The decision→outcome mapping (`Allow`/`Deny`/`Ask`) and the `§19` seam are
  normative in §5.4 and are not restated here.
- `Denied` is a **policy** outcome produced only by the loop, never by a tool;
  `Cancelled` is a cancellation outcome, not a failure (`06` A10, `01`
  I11/I12). `ToolResult.outcome ∈ {Ok, Error, Denied, Cancelled}` (`01 §4.5`).

---

## 9. Subprocess / PTY ownership and the daemon process tree (04 OQ-6)

### 9.1 Ownership split

`04` OQ-6, quoted verbatim:

> **OQ-6 — Orphaned tool children and PTYs after a daemon crash (ownership
> pinned; mechanics deferred to spec 07).** On crash, the daemon's tool
> subprocesses/PTYs are reparented to `init`; on graceful shutdown the daemon
> signals its process group. **04 owns the daemon process tree** (the daemon and
> its process group); **spec 07 owns the tool subprocesses and PTYs** (process
> groups, `PR_SET_CHILD_SUBREAPER`, PTY teardown). This spec pins only the
> daemon's signal policy (SIGCHLD reaped, §3.6) and the shutdown step
> (§3.5 step 4).

This spec therefore owns the **mechanics** inside the daemon's tree:

```text
WorkspaceHost daemon (04 owns)  ── setsid, process group, SIGCHLD policy
   │
   ├── Tool subprocess A        (07 owns) ── own process group (setpgid)
   │      └── grandchild        reparented to the daemon (subreaper)
   │
   ├── Tool subprocess B        (07 owns)
   │
   └── PTY session C            (07 owns) ── master fd + slave O_NOCTTY
          └── foreground group (SIGHUP on master close)
```

### 9.2 Process groups and reaping

- **Own process group.** Each tool subprocess calls `setpgid(0, 0)` between
  `fork` and `exec`, so the tool can signal/kill the whole tree
  (`kill(-pgid, SIG)`) without touching the daemon or a sibling tool (`X13`).
- **Subreaper.** The daemon sets `PR_SET_CHILD_SUBREAPER` (prctl) so
  grandchildren orphaned by an intermediate shell are reparented to the daemon
  rather than to `init` while the daemon is alive; this is what makes
  `waitpid`-based reaping possible for `bash -lc` trees.
- **SIGCHLD.** The daemon handles SIGCHLD (never `SIG_IGN`/`SA_NOCLDWAIT`, which
  would destroy `waitpid`) via an async-signal-safe self-pipe (`04 §3.6`). This
  spec provides the `ChildReaper` seam the handler notifies:

  ```cpp
  namespace ymh {

  // Registered with the daemon's SIGCHLD self-pipe. The handler only writes a
  // byte; reaping happens on the event loop, never in signal context.
  class ChildReaper {
  public:
      // Called on the event loop; drains waitpid(WNOHANG) (or pidfd readiness)
      // and routes exit status to the owning ProcessService/PtyService.
      void drain() noexcept;
  };

  } // namespace ymh
  ```

- No tool spawns a child outside `ProcessService`/`PtyService`, so every child
  is known to the reaper (`X13`); no zombies accumulate while the daemon lives.
- `pidfd` (Linux ≥ 5.3) is the preferred exit-status source; `waitpid` is the
  fallback.

### 9.3 PTY teardown

- The slave is opened `O_NOCTTY` (`04 §3.2`, H3) so the daemon never acquires a
  controlling terminal; the child `setsid`s and takes the slave as its
  controlling terminal explicitly.
- `PtySession::terminate()` is idempotent: close the master → SIGHUP to the
  foreground group → wait `ToolConfig::terminate_grace` (§5.5) → SIGKILL the
  group.
- A PTY child that exits on its own leaves the master readable until drained;
  the tool drains to EOF, then closes (E-F13).

### 9.4 Crash and orphan behavior

- **Daemon crash.** Tool children reparent to `init`; PTY masters close, so
  slaves receive SIGHUP. This spec does **not** attempt cross-process recovery:
  an orphan is `init`'s concern. There are no PID files and no "kill on `ps`
  evidence" logic (`04 §7.3`, `04 §7.5`).
- **Graceful shutdown.** `04 §3.5` step 4 signals the daemon's process group;
  this spec supplies the mechanics: SIGTERM each tool group, wait bounded by
  04's `shutdown_grace` (default 10 s, `04 §4.1`), then SIGKILL; PTY sessions get
  SIGHUP then SIGKILL. The daemon never leaves a tool child alive after
  `Stopped`.
- **Detach.** A supervisor detach does not touch tools; the daemon keeps its
  in-flight session running (`§9.9`, D23).

---

## 10. Concurrency and threading

- **One Asio loop per daemon** (`§35`, `04 §9`): the accept loop, signals,
  timers, and event fan-out share one `io_context`. There is **no thread per
  tool call** (`§35`).
- Tools run as asynchronous tasks on the daemon's loop; a CPU-heavy tool may use
  the bounded worker pool (`§35`). Blocking syscalls are offloaded, never run on
  the loop thread.
- **Per-session serialization.** In v1 the loop iterates a step's tool calls
  sequentially (`06 §5.1`), so at most one tool executes per session at a time;
  parallel tool calls are deferred to Phase 2 (decision (q)) and must respect F8
  and per-session output ordering when they arrive.
- **Registry immutability.** Registration completes before serving and is frozen
  (`X5`); `find`/`contains`/`schemas` are lock-free reads.
- **Output ring** is written only from the session's owning execution context;
  no cross-thread mutation.
- **Cap counters** (`ResourceGovernor`) are the single synchronization point for
  subprocess/PTY accounting; acquire/release are loop-affine.

---

## 11. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**X1 — Rooted resolution.** Every filesystem, process `cwd`, PTY, git, and LSP
path goes through `ExecutionEnvironment::resolve()`; `getcwd()` is never a
resolution base in tool code. (`§9.7`, `§18`, F1)

**X2 — No escape (root-confined modes).** In `Workspace` and `ReadOnly`,
`resolve()` returns a path contained in `root()` — containment is component-wise
over `weakly_canonical` of **both** operands, and the final component is
canonicalized when it exists — or throws `ToolError{PathEscape}`. `Unrestricted`
is the single explicit, config-selected opt-out (§6.2, §6.8). (`§15`, `§18`)

**X3 — Canonical root.** `root()` is realpath-canonical; workspace uniqueness is
canonical string equality. (`§9.7`, DIV-8, `§18`)

**X4 — One environment per daemon.** Exactly one `LocalEnvironment` rooted at
the workspace is shared by all sessions; each `ToolContext` is per-call and
non-owning. (`§9.7`, `04 §3.3`, `04 §3.7`)

**X5 — Registry immutability.** After `freeze()` no add/remove succeeds; lookup
and `schemas()` are stable for a generation. (`§7.3`, `§54 D11`)

**X6 — Validated execution.** A tool executes only after name and argument
validation; an unknown name or invalid arguments yields `Error`, never a silent
drop. (`§46`, `08 §4.2`)

**X7 — Permission before execution.** `ToolCall` and `PermissionDecision` are
durable before `execute`; a tool never runs without a recorded decision.
(`§19`, `§46`, `06 §5.1`, F2)

**X8 — Tools never append.** Tools emit live events only; the loop is the sole
durable appender. (`01` I4, `06` A1/A2)

**X9 — Outcome totality.** Every call yields exactly one `payload::ToolResult`
with `outcome ∈ {Ok, Error, Denied, Cancelled}`, paired by `ToolCallId`.
(`01 §4.5`, `01` I12)

**X10 — Bounded output.** Tool output is bounded by the per-session ring (F5);
the durable `ToolResult.output` is clamped on the **serialized** payload size
(escaping-aware) so the whole `payload::ToolResult` is ≤ `tool_result_max_bytes`
< `max_payload_bytes`, and `truncated` reflects loss. (`§9.11`, `01` S10,
`02 §4.1`, F5)

**X11 — Caps before spawn.** Every subprocess/PTY acquires a governor slot
before spawn and releases it on completion, timeout, cancellation, and
exception. (`§9.11`, `04 §8`, F8)

**X12 — Cancellation scoping.** Cancellation cancels only the owning session's
in-flight tool and its children; no other session is affected. (`§34`, `06` A9,
F9)

**X13 — Process-group ownership and reaping.** Each tool subprocess/PTY is in a
group the daemon can signal, and every child is reaped while the daemon lives;
no zombies accumulate. (`04 §3.6`, `04` OQ-6)

**X14 — Deterministic schemas.** `schemas()` is ordered and byte-stable for a
generation, so provider prompts and golden tests are reproducible. (`§44`,
`§45`)

**X15 — No UI dependency.** The `tools/` and `execution/` targets never include
or reference UI types. (`§4.1`, `§20.1`, D15)

**X16 — Sandbox honesty.** v1 sandboxing is root containment + permission policy
+ resource caps; it is not an OS jail, and neither docs nor schema claim
otherwise. (`§46`, `§52`)

---

## 12. Failure modes

### 12.1 Shared findings (F1–F12, §54)

The tools/execution responsibilities for the existing findings:

| F# | Finding | Tools/execution handling |
|---|---|---|
| **F1** | path/process isolation | `ExecutionEnvironment::resolve()` is the only path base; one workspace per daemon; `getcwd()` never used (X1–X4, `§9.7`, `§18`) |
| **F2** | background permission | the loop records `PermissionDecision` before `execute`; a background `ASK` is surfaced by attention, never an invisible deadlock (`§9.9`, `06 §5.5`) |
| **F3** | late event after close | tools emit live events best-effort; the durable `ToolResult` is appended by the loop, so a closed session never gets a tool-written late durable event (`01 §5`, `06` A14) |
| **F4** | edge-triggered attention | tools do not own attention; the loop's `AgentState` transitions drive it (`06 §3.3`, spec 10) |
| **F5** | output ring buffers | per-session rings bound tool output; full output only for active/expanded sessions (X10, `§9.11`, `§20.10`) |
| **F6** | input/keybinding focus | out of scope; tools never see focus |
| **F7** | per-session dirty flags | out of scope; live coalescing is per-session (spec 10 projection) |
| **F8** | resource caps | subprocess/PTY caps enforced by `ResourceGovernor` (X11, `§9.11`, `04 §8`) |
| **F9** | cancellation scoping | session-scoped token; only the owning tool's children are killed (X12, `§34`) |
| **F10** | resume-suspended | out of scope; no tool runs on resume until activation (`06` A15) |
| **F11** | subagent ID duality | `spawn_subagent` returns a result reflecting `SubagentFanIn`; child ids stay durable (`06 §7`, `§20.25`) |
| **F12** | flash clock in model | out of scope; tools emit no flash state (spec 10) |

**Explicitly out of scope:** F6, F7, F12 (UI projection), F4's flash arithmetic
(spec 10), F2's policy rules (spec 09), and F10 (spec 06).

### 12.2 Component-local failure modes (`E-F#`)

These are component-local to tools/execution and must be covered by tests
(§14.6). The `E-F#` namespace is **execution-local**: it is disjoint from spec
05's transport namespace `T-F#` (`05 §11.2`) and from `D-F#` (spec 04), and it
deliberately avoids `X-F#` because `X1`–`X16` are this spec's invariants (§11).

| E-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **E-F1** | Unknown tool name | registry miss | `Error{UnknownTool}`; no execution (`08 §4.2`) |
| **E-F2** | Invalid arguments | schema validation | `Error{InvalidArguments}`; no execution |
| **E-F3** | Path escape | `resolve()` containment check | `Error{PathEscape}`; no I/O outside root (X2) |
| **E-F4** | Filesystem I/O failure | `Filesystem` error | `Error{Io}`; partial writes never reported as `Ok` |
| **E-F5** | Subprocess non-zero exit | `ProcessResult.exit_code != 0` | `outcome = Ok` with the exit code in the output; the model decides (decision (h)) |
| **E-F6** | Subprocess timeout | deadline fired | `Error{Timeout}`; SIGKILL the group; slot released |
| **E-F7** | Cancellation mid-execution | token fired | `Cancelled`; children SIGTERM→SIGKILL; partial output kept |
| **E-F8** | Subprocess cap exhausted | `tryAcquireSubprocess == false` | `Error{ResourceExhausted}`; durable via the loop (F8) |
| **E-F9** | PTY cap exhausted | `tryAcquirePty == false` | `Error{ResourceExhausted}`; durable (F8) |
| **E-F10** | Output ring overflow / UTF-8 loss | total bytes > capacity, or bytes dropped by lossy UTF-8 conversion | overwrite oldest and/or sanitize, set `truncated`; never fail (F5) |
| **E-F11** | Payload too large | serialized payload measured before append | truncate `output` until the serialized `payload::ToolResult` ≤ `tool_result_max_bytes`; never `PayloadTooLarge` (X10) |
| **E-F12** | Orphaned/zombie child | `ChildReaper` drain | subreaper + `waitpid(WNOHANG)`; no zombie accumulation (X13) |
| **E-F13** | PTY child exit | master read EOF | drain to EOF; `terminate()` idempotent |
| **E-F14** | Duplicate/frozen registration | `add()` after freeze / duplicate | fail-loud `ToolRegistryError` at load, never at execution |
| **E-F15** | Schema cache drift | `generation()` mismatch | invalidate the cached schema snapshot; re-read `schemas()` |
| **E-F16** | Unexpected tool exception | `catch (...)` at the registry boundary | `Error{Internal}`; the loop continues (`06` A-F5) |
| **E-F17** | Policy not wired | startup check | startup failure, never a runtime bypass (X7) |
| **E-F18** | Interleaved stdout/stderr | single tagged ring | preserve per-channel write order; coalesced batches never reorder |

---

## 13. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (`§55`). Tools/execution map onto
it as follows.

| dsh concept | ymh tools/execution | Reference |
|---|---|---|
| Tool registry | `ToolRegistry` (this spec) | `§14`, `§55` |
| Tool | `Tool` / `ToolSchema` | `§14.1`, `§55` |
| Tool invocation context | `ToolContext` | `§14.2` |
| Execution world / capability seam | `ExecutionEnvironment` | `§4.4`, `§18`, `§55` |
| Filesystem provider | `Filesystem` | `§15` |
| Subprocess provider | `ProcessService` | `§16` |
| PTY provider | `PtySession` / `PtyService` | `§17` |
| Approvals | `PermissionHandle` (policy in 09) | `§19`, `§46` |
| Append-only trace | loop appends `ToolCall`/`ToolResult`; tools never append | D2, `01 §4.5` |
| Sandbox provider | `SandboxMode` / `SandboxEnvironment` (Phase 3) | `§52`, `§53` |

**Deliberate omissions** (accepted for v1, `§55`): no `dlopen` plugins
(static plugins, `§7.1`), no hot reload (`§54 D11`), no MCP adapter (`§27`,
Phase 2), no LSP tools (`§28`, Phase 2), no OS sandbox (`§52`), no parallel tool
calls (Phase 2, decision (q)).

---

## 14. Test plan

Strategy is `§44`: unit, integration (fake FS / fake shell / `FakeLLM`), golden,
replay, and a separate live PTY/real-LLM layer. The deterministic layers run
offline against fakes; the live layer is opt-in and API-key gated (`§44`,
`§45`).

### 14.1 Unit tests

- **`ToolRegistry`**
  - `add`/`find`/`contains`/`names`; lexicographic `names()`/`schemas()` (X14).
  - duplicate `(name, version)` → `DuplicateVersion`; same name, new version →
    `DuplicateName`; `InvalidName` grammar; malformed/unsupported `input_schema`
    → `InvalidSchema`; `freeze()` then `add` → `RegistryFrozen` (E-F14).
  - `generation()` bumps on add/remove; `schemas()` stable across reads.
- **Validation**
  - unknown name → `UnknownTool`; non-object args → `InvalidArguments`; schema
    violation → `InvalidArguments`; valid args pass (E-F1/E-F2, X6).
- **`resolve()` / path safety**
  - table-driven: relative, absolute-under-root, `..` that stays in root, `..`
    that escapes → `PathEscape`, symlink pointing outside root → `PathEscape`,
    **a symlink at the final component pointing outside root → `PathEscape`
    (the final component is canonicalized when it exists)**, an in-root symlink
    that stays inside → accepted, non-existent write target under root, `/a/bc`
    vs `/a/b` prefix boundary (X1/X2/X3).
  - `Unrestricted` skips containment (an out-of-root path resolves) but still
    requires a recorded permission decision (X2, §6.8).
  - assert `getcwd()` is never called (static check + a chdir-perturbation test).
- **`OutputSink` / ring**
  - total under cap ⇒ no truncation; over cap ⇒ tail retained + `truncated`
    (E-F10); `materialize(maxBytes)` never exceeds the cap; interleaved
    stdout/stderr ordering (E-F18).
  - a **non-UTF-8 byte fixture** (raw/PTY bytes) is lossy-converted so
    `materialize()` is valid UTF-8 and `truncated` records the loss (E-F10,
    §5.2/§8.2).
- **Durable clamp**
  - a huge tool output is clamped so the serialized `payload::ToolResult` is ≤
    `tool_result_max_bytes`; the store never raises `PayloadTooLarge` (X10,
    E-F11).
  - a **control-character fixture** (e.g. NUL/ESC bytes that JSON-escape to ~6×)
    still fits after clamping, proving the clamp is on the serialized size, not
    raw bytes (X10, E-F11).
- **Caps**
  - `tryAcquireSubprocess`/`tryAcquirePty` honor global and per-session caps;
    RAII release on success/throw/cancel (X11, E-F8/E-F9).
- **`ToolContext` wiring**
  - `root()`/`resolve()` forward to `execution()`; `emit()` publishes live
    only; no UI include (X8/X15).
- **Sandbox modes**
  - `ReadOnly` rejects mutating tools at the policy layer (`ToolResult{Denied}`,
    `execute` not called, §6.8); `Unrestricted` skips containment but still
    requires a permission decision (X2, X16).
- **Tool errors**
  - thrown `ToolError` maps to `Error{code}`; unknown exception maps to
    `Internal` (E-F16).

### 14.2 Integration tests (fake FS / fake shell / `FakeLLM`, `§45`)

- **Full tool pipeline.** `FakeLLM` scripts a tool call; the durable sequence is
  `ToolCall → PermissionDecision → ToolResult` with the id pairing preserved
  (X7/X9, `06 §13.2`).
- **Permission paths.** `Allow` executes; `Deny` yields `ToolResult{Denied}`
  with `execute` never called; `Ask` blocks then resolves (F2).
- **Cancellation.** Cancel mid-`shell` kills the child group and yields
  `Cancelled`; a second session is untouched (X12, E-F7).
- **Cap exhaustion.** Saturate the subprocess cap; the next tool yields
  `Error{ResourceExhausted}` and the turn continues (E-F8/E-F9).
- **Fake shell.** A fake `ProcessService` returns scripted exit codes/timeouts;
  non-zero exit is `Ok` with the code in output (E-F5/E-F6).
- **Fake filesystem.** `read_file`/`edit_file`/`glob`/`grep` over a fixture tree;
  exact-match edit failure is `Error`, not a partial write (E-F4).
- **Git.** `git_status`/`git_diff` against a fixture repo; `git_checkout` is
  `ASK` (Phase 2).
- **PTY (Phase 2).** A fake child under a real PTY: write/read/resize/terminate;
  EOF drain and idempotent terminate (E-F13, `00 §51`).
- **Subagent (v1).** `spawn_subagent` awaits `whenIdle()`; a failed child yields
  a result reflecting `SubagentFanIn{Failed}` (`06 §7`; `00 §51` schedules
  subagents in Phase 2 — OQ-8).

### 14.3 Golden tests

- **Tool schema JSON.** A byte-stable golden of `schemas()` for the MVP set,
  proving ordering and shape stability (X14).
- **Tool output.** Golden `ToolResult.output` for `read_file`, `grep`, `glob`,
  and `shell` over a fixture tree.
- **Tool event stream.** Golden `ToolCall`/`PermissionDecision`/`ToolResult`
  sequence for a scripted `FakeLLM` turn, including the `Denied` and
  `Cancelled` fixtures (`01` I12).
- **Coalescing.** A known output chunk stream at a fixed clock yields a golden
  live-batch sequence plus a final durable snapshot (§8.2).

### 14.4 Replay tests

- Record a `FakeLLM`-driven session containing tool calls; `deriveMessages`
  over the log reproduces the message list exactly, with **no tool execution
  and no provider construction** (`§44`, `01 §6.3`).
- `resolve()` is deterministic: the same relative inputs over the same root
  yield identical absolute paths across runs (X1/X3).
- Replay after crash (open turn) synthesizes `ToolResult{Error}`/`Cancelled`
  per `01` I12 and is deterministic (`06 §13.4`).

### 14.5 Live end-to-end tests (real LLM, PTY-driven, `§44`)

- Spawn the real `ymh` binary under a PTY; drive it with scripted keystrokes and
  parse the rendered ANSI (`§44`).
- Prompt a real model to read a file, edit it, grep, run a shell command, and
  show `git diff`; assert observable behavior: session created, tool calls
  rendered, permission prompt handled, durable events emitted, session resumes.
- **Gated**: requires an API key plus an explicit opt-in flag; skipped (not
  failed) when absent, so the default suite stays hermetic (`§44`).
- **Tolerant of model nondeterminism**: assert structure/invariants (tools
  invoked, events emitted, final state), never exact prose (`§44`).
- **Milestone gating**: MVP live tests cover the single-process flow; the
  multi-workspace daemon/supervisor live tests are Milestone 2 (`§57` Step 13,
  `§58`).
- Runs as a separate CI stage, never part of the fast default command (`§44`).

### 14.6 Failure-mode coverage matrix

| E-F# | Covered by |
|---|---|
| E-F1/E-F2 | unit validation; integration unknown-tool |
| E-F3 | unit `resolve()` table |
| E-F4 | integration fake FS |
| E-F5/E-F6 | integration fake shell; unit timeout |
| E-F7 | integration cancellation |
| E-F8/E-F9 | unit caps; integration cap exhaustion |
| E-F10 | unit ring overflow + non-UTF-8 fixture |
| E-F11 | unit durable clamp + control-character fixture |
| E-F12 | integration child reaper; crash tests |
| E-F13 | integration PTY EOF/terminate |
| E-F14 | unit registry freeze/duplicate |
| E-F15 | unit generation/cache |
| E-F16 | unit exception mapping |
| E-F17 | startup check |
| E-F18 | unit interleaved channels |

### 14.7 Invariant coverage

| Invariant | Covered by |
|---|---|
| X1 | unit `resolve()` + static `getcwd()` check |
| X2 | unit escape table (incl. final-component symlink) |
| X3 | unit canonical root |
| X4 | integration one-environment wiring |
| X5 | unit freeze/generation |
| X6 | unit + integration validation |
| X7 | integration pipeline order |
| X8 | integration no-append; static check |
| X9 | golden + replay event stream |
| X10 | unit ring + durable clamp |
| X11 | unit cap acquire/release |
| X12 | integration cancellation scoping |
| X13 | integration reaper/orphan |
| X14 | golden schema ordering |
| X15 | static check: no UI include |
| X16 | unit sandbox modes; docs review |

---

## 15. Decisions and open questions

### 15.1 Decisions (pinned by this spec)

- **(a) `ToolSchema` is owned by 07 and its shape is pinned here.** `08 §3.1`
  defers `ToolSchema` to this spec; `input_schema` is a restricted JSON Schema
  object subset so every provider maps it losslessly (§3.2, `08 §1.4`).
- **(b) The outcome taxonomy is `01`'s `ToolOutcome {Ok, Error, Denied,
  Cancelled}`; there is no separate `Success`.** `Ok` is success; `Denied` and
  `Cancelled` are distinct from `Error` (`01 §4.5`, `06` A10, §2.2).
- **(c) The registry is frozen after startup; there is no hot reload.** Static
  plugins only (`§7.1`, `§54 D11`); `add`/`remove` after `freeze()` throws
  `RegistryFrozen` (§4.1/§4.2, X5).
- **(d) `resolve()` canonicalizes the parent via `weakly_canonical` and the
  final component when it exists; `is_within` normalizes both operands and
  compares component-wise; escapes throw `PathEscape`.** `getcwd()` is never
  consulted; `Unrestricted` is the only mode that skips containment (§6.2,
  §6.8, X1/X2).
- **(e) Output rings are live-only; the durable `ToolResult.output` is a single
  bounded snapshot clamped on the serialized payload size below
  `max_payload_bytes`.** A tool result can never trigger `PayloadTooLarge`
  (§5.2, §8.2, X10).
- **(f) `SandboxMode` is a three-value enum; v1 enforcement is root containment
  + permission + caps, not an OS jail.** OS isolation is Phase 3
  (`§52`) (§6.8, X16).
- **(g) Each tool subprocess gets its own process group and the daemon sets
  `PR_SET_CHILD_SUBREAPER`; `ChildReaper` drains SIGCHLD on the loop.** This is
  the 07 half of `04` OQ-6 (§9.2, X13).
- **(h) A subprocess that runs to completion with a non-zero exit is
  `outcome = Ok` with the exit code in the output.** Only spawn failure,
  timeout, and cancellation are `Error`/`Cancelled` (E-F5).
- **(i) Tools emit live events only; the loop appends every durable record.**
  (`§8.1`, X8, `06 §5.5`).
- **(j) `PermissionHandle` is a §19 seam co-owned with spec 09; this spec pins a
  provisional shape with a virtual destructor.** The primary decision is made by
  the loop before `execute`; the handle exposes it and an optional sub-operation
  seam (§5.4).
- **(k) `spawn_subagent` is a registered tool whose schema is owned by 07 and
  whose machinery is owned by 06 §7.** It is **v1** per the verified `06 §7`
  (synchronous subagent); `00 §51`/`§57` Step 12 schedule subagents in Phase 2 —
  a genuine `00`↔`06` conflict recorded as OQ-8. This spec owns only the schema
  and result mapping (§4.3).
- **(l) Caps are acquired through the daemon's `ResourceGovernor`; no tool
  re-implements a cap.** Values come from `04 §2.1` (§7.1, X11).
- **(m) LSP tools are Phase 2 and absent while `lsp() == nullptr`.** The model
  is never offered a tool that cannot run (§4.3, §6.7).
- **(n) The environment root is a constructor parameter of `LocalEnvironment`,
  baked in before any tool is written.** (`§18`, `§57` Step 11) (§6.1).
- **(o) `PermissionRequest` is deferred to spec 09.** 09 must define it (§19)
  and reuse `PermissionDecisionKind`; 07 pins only the `PermissionHandle` seam
  and its virtual destructor (§5.4). Recorded deferral, not an open question.
- **(p) The durable clamp is on the serialized payload, not raw bytes.** The
  loop truncates `output` until the serialized `payload::ToolResult` fits
  `tool_result_max_bytes`; worst-case JSON escaping (~6×) is budgeted for, so a
  control-character-heavy result cannot trigger `PayloadTooLarge` (§5.2, §8.1,
  X10, `01` S10).
- **(q) v1 tool calls are sequential.** `06 §5.1` iterates a step's calls one at
  a time; parallel tool calls are deferred to Phase 2, where concurrent cap
  accounting and per-session output ordering must be specified (§10).
- **(r) `resolve()` is mode-aware; `Unrestricted` is the single containment
  opt-out.** `ExecutionEnvironment::mode()` exposes the mode; `resolve()` skips
  containment only in `Unrestricted`, which remains permission- and cap-gated
  and is config-selected, never model-selected (§6.1, §6.2, §6.8, X2).
- **(s) 07 owns the tool schema and registration; 06 owns the subagent
  machinery; phases follow `00` except where `00` and `06` conflict.**
  `write_file`, PTY-backed tooling, LSP, and the extra Git tools are Phase 2
  (`00 §50`, `§51`, `§57` Steps 5/6/9/12); seams are pinned now so no tool
  changes when a Phase-2 capability lands (§4.3, §6.5, §6.7). `spawn_subagent`
  is v1 per `06 §7` (OQ-8).
- **(t) `OutputRing` is owned by 07.** `04 §8`'s ambiguous
  ownership sentence is corrected to name 07 for the ring and 06 for `LLMPool`;
  02 owns only chunk coalescing (§5.2).
- **(u) `ReadOnly` is enforced at the policy layer as `Denied`.** The loop
  records the decision and `ToolResult{Denied}`; `execute` is not called. A tool
  never returns `Denied` — that value is produced only by the loop (§6.8, §8.3).

### 15.2 Open questions

- **OQ-7 — `write_file` phase is inconsistent within `00`.** `00 §14.3` and G4
  list `write_file` in the initial tool set, but the `§50` MVP definition and
  `§57` Step 5 omit it. This spec schedules it in Phase 2 (matching `§50`/`§57`)
  and records the internal `00` tension rather than silently disagreeing. Resolve
  when `00` is re-verified.
- **OQ-8 — `spawn_subagent` phase conflicts between `00` and verified `06`.**
  `00 §51` and `§57` Step 12 schedule subagents in Phase 2, but verified
  `06 §7` (decision (i)) pins a **synchronous v1** subagent spawned by a tool.
  This spec follows `06` (v1) — it owns the machinery and is the later, more
  specific, verified artifact — and flags the `00` divergence here instead of
  silently overriding either. Resolve by amending `00 §51`/`§57` or `06 §7` at
  the next re-verification.

---

## 16. References

- `00-architecture.md` §4.1 (agent ≠ UI), §4.2 (event stream is the runtime
  spine), §4.4 (execution environment as a capability boundary), §5 (components),
  §7.1–§7.3 (plugin model), §8.1–§8.3 (events, `EventBus`), §9.7 (path safety),
  §9.11 (resource limits), §11 (agent loop), §12 (LLM interface), §14.1–§14.3
  (tool system), §15 (filesystem), §16 (process/shell), §17 (PTY), §18
  (execution environment), §19 (permissions), §20.10 (tool output streaming),
  §27 (MCP), §28 (LSP), §30 (subagents), §34 (cancellation), §35 (concurrency),
  §40 (logging), §44 (testing), §45 (Fake LLM), §46 (security model), §48
  (dependencies), §49 (source tree), §50 (MVP), §51–§53 (phases), §54
  (D1–D23, F1–F12), §55 (dsh comparison), §57 (implementation order), §58
  (milestones).
- `01-session.md` §2.1 (identifiers), §4.5 (`ToolCall`/`ToolResult`,
  `ToolOutcome`), §6.3 (`deriveMessages`), §10.2 (path safety), I4/I11/I12
  (append/pairing invariants), §13.2 (S-failure modes), §15 (test plan).
- `02-persistence.md` §2.2 (`PayloadTooLarge`), §4.1 (`max_payload_bytes`),
  §4.4 (`append`), §4.8 (`SessionHandle`), §6.2 (chunk coalescing).
- `03-workspace-registry.md` (workspace identity and the canonical workspace root
  the environment is built from; consumed through 04).
- `04-workspace-host-daemon.md` §2.1 (`ResourceCaps`), §3.2 (`setsid`, fd
  hygiene), §3.3 (startup order), §3.5 (shutdown), §3.6 (signals), §3.7
  (hosted services), §4.1 (`HostConfig`), §8 (`ResourceGovernor`), §14.2 (OQ-6).
- `05-transport.md` §5 (`session.activate` / `agent.*` RPCs reach the loop, never
  a tool), §11.2 (`T-F#` namespace, disjoint from this spec's `E-F#`).
- `06-agent-loop.md` §1.4 (seam ownership), §5.1 (pipeline order), §5.2
  (context assembly), §5.5 (tool pipeline), §5.9 (`LLMPool`), §7 (subagents),
  §10 (A-invariants), §11.2 (A-F failure modes), §13 (test plan).
- `08-llm-provider.md` §1.3/§1.4 (`ToolSchema` ownership), §3.1 (`LLMRequest`),
  §3.2 (stream events), §4.2 (tool-call assembly), L9/L10 (tool-call ids and
  valid arguments).
- `09-permissions.md` §19 (permission policy and `PermissionRequest`; planned —
  09 must define `PermissionRequest` and reuse `PermissionDecisionKind`).
- `10-tui.md` §20.10 (tool-output projection), §20.25 (subagent output cadence;
  planned).
