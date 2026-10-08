# 83 - Output Ring Per-Call Isolation Errata

```
Status: verified (Rev 4)
Verification status: adversarial reviewer (independent, Rev 4): GATE PASS (0 HIGH / 0 MEDIUM, 4 LOW); Oracle confirm PASS
Revision: 4
Component: 83 (corrective errata) -- execution/output, tools, agent loop
Depends on: 07-tools-execution.md (normative) Sec. 5.1, Sec. 5.2, Sec. 7.3, Sec. 8.1, Sec. 8.2,
            Sec. 11 X10, Sec. 12.1 F5, Sec. 13, Sec. 14; 14-pty-capability.md Sec. 5.2 (E-P4), Sec. 12;
            06-agent-loop.md Sec. 5.5 (ToolContext pipeline); 04-workspace-host-daemon.md
            Sec. 2.1, Sec. 8; 40-output-retention.md Sec. 5 (retain_tool_result);
            01-session.md S10; 02-persistence.md Sec. 4.1; 00-architecture.md Sec. 9.11,
            Sec. 55; 56-D6
Scope: one confirmed defect -- a tool result (payload::ToolResult.output) is
       polluted with the accumulated output of every prior tool call in the
       workspace. This spec pins per-call output isolation: each call streams
       into its own bounded buffer, and the shared session ring remains the
       live-streaming buffer only. Design only; no implementation code.
Supersedes: 07 Sec. 11 X10's phrase "bounded by the per-session ring" -- the
            durable result bound is now per-call (the live ring remains
            per-session). See Sec. 1.4. Also supersedes the 07 Sec. 7.1
            `OutputRing& ringFor(SessionId)` signature (:1040) and the
            `OutputRing& ringFor(SessionId)` line in 07 Sec. 5.2's referenced
            04 Sec. 8 seam: the return becomes an owning
            `std::shared_ptr<OutputRing>` and the call takes an owner-liveness
            token (A6, NEW-1, NEW-2).
Amends: 07 Sec. 5.2 (additive CallOutputSink; OutputRing unchanged);
        include/ymh/agent/agent_loop.hpp:73 (AgentServices::output retired);
        04 Sec. 8 (ResourceGovernor: `shared_ptr` ring map, owning sticky
        `ringFor(session, owner_alive)`, additive `releaseSession`); 14 Sec. 5.2
        (clarifying cross-reference only -- no PTY change)
Tracker: DESIGN_STATUS.md row 83 is orchestrator-owned and added only at
         promotion (AGENTS.md rule); it is not created or edited by this spec.
Revision note: Rev 4 closes the gate-83 Rev 3 review (1 MEDIUM NEW-2 + 2 LOW):
         NEW-2 makes eviction sticky -- `ringFor` takes the owning agent's
         liveness token and, under the governor's `mutex_`, refuses to insert a
         map entry once the owner is disposed, so a late call cannot resurrect a
         dead-session ring (the Sec. 2.5 / OP5 bound holds); LOW-3 updates the
         OP/OP-F ranges to OP1-OP13 / OP-F1-OP-F13; LOW-4 pins
         `rings_.erase(key)`. See the Rev 3/Rev 2 notes below.
Revision note (Rev 3): NEW-1 makes the live ring a `shared_ptr` so
         `releaseSession` cannot free a ring an in-flight call still holds (no
         use-after-free, no join required); the eviction caller is reduced to
         `AgentRegistry::dispose`/`finalizeAll` (LOW-1).
Revision note (Rev 2): added ResourceGovernor::releaseSession + eviction and
         corrected the Sec. 2.5 bound (MEDIUM-1); redesigned T4/T5 to execute
         concurrency (MEDIUM-2); re-anchored, pinned, and restated LOW-1..LOW-6.
```

This document is the design gate for a single, confirmed correctness defect. It
amends the owning specs by reference; every "current state" claim quotes a
`file:line` in the shipped tree so it is reproducible. Naming: invariants local
to this spec are `OP1`-`OP13`; failure modes are `OP-F1`-`OP-F13`; decisions are
`83-D1`-`83-D10`. The prefixes `X`, `E-F`, `O`, `P-F`, `R`, `Q`, `H`, `T`, `A`,
`U` are already owned by specs 07, 16, 14, 03, 09, 04, 05, 06, 10 and are not
reused here.

---

## 1. Purpose and scope

### 1.1 The defect (confirmed root cause)

Every `ToolResult` that materializes the output sink is polluted with the
accumulated output of all prior tool calls in the same workspace.

Evidence, in call order:

1. The `WorkspaceRuntime` owns **one** `OutputRing` and **one** `RingOutputSink`
   for the whole daemon, constructed once and never reset:
   - `src/agent/workspace_runtime.cpp:284-285`:
     `ring_(governor_.caps().session_output_ring_bytes), sink_(ring_)`;
   - members at `src/agent/workspace_runtime.cpp:564-565`;
   - the sink is installed into the shared services at
     `src/agent/workspace_runtime.cpp:394`: `services_.output = &sink_;`.
2. `RingOutputSink::materialize()` returns the tail of the ring it wraps, i.e.
   the tail of **all output ever appended in this workspace**:
   `src/execution/output.cpp:157-158` (`return ring_->tail(max_bytes);`).
3. `OutputRing::clear()` exists (`src/execution/output.cpp:125-129`) but has
   **no caller** anywhere in `src/` (grep: definition + declaration only); the
   sink's `close()` is a no-op (`src/execution/output.cpp:149`). Nothing ever
   resets the ring between calls.
4. The one sink pointer is copied into every `ToolContext`:
   `src/agent/agent_loop.cpp:983-985` (`*services_.output`); each
   `AgentLoop` (root or child) receives the same workspace `AgentServices` copy
   (`src/agent/agent_registry.cpp:219` `AgentServices services = services_;`,
   `:235` `make_shared<AgentLoop>(...)`), and the field is
   `OutputSink* output` (`include/ymh/agent/agent_loop.hpp:73`). So parent and
   every subagent share the single persistent sink.
5. `shell` streams the process into that shared sink and then materializes it:
   `src/tools/builtin_tools.cpp:419` (`request.sink = &context.output();`) and
   `:445-446` (`context.output().materialize(config_.tool_result_max_bytes)`),
   with a second materialize on cancellation at `:438`.
6. `ToolRegistry::execute` also materializes the shared sink when a tool's own
   `result.output` is empty, and on every error/cancel path:
   `src/tools/tool_registry.cpp:357-359, 370, 376, 381`.

Consequence: a `shell` result contains the tail of **all** prior output up to
the ring cap (`session_output_ring_bytes`, 1 MiB) or `tool_result_max_bytes`,
and parent/subagent calls cross-contaminate. Any tool that returns an empty
`result.output` inherits stale bytes through the `tool_registry.cpp:357-359`
fallback.

The header contract already states the intended split:
`include/ymh/execution/output.hpp:5-10` -- `OutputRing` is "the live-only
bounded buffer (one per session, owned by the `ResourceGovernor`)"; `OutputSink`
is "the per-call writer a tool streams into". The implementation violates it:
there is one persistent sink and it is never reset.

### 1.2 Correcting the record

One claim in the defect report is **wrong** and must not drive the fix:

- **`terminal` / PTY does not use `context.output()`.** `src/tools/terminal_tool.cpp`
  contains zero calls to `context.output()` (grep: only `result.output =`
  assignments, e.g. `:170`, `:195`, `:237`, `:258`, `:286`, `:303`). The PTY
  pump writes to a **per-`PtySession`** `PtyOutputRing`
  (`src/execution/pty.cpp:185` constructor, `:548` append) and `read` consumes
  it through a FIFO cursor (`src/execution/pty.cpp:286-311`). That ring is
  `ResourceCaps::pty_output_ring_bytes` (256 KiB; 14 Sec. 5.2 E-P4;
  `include/ymh/execution/resource_governor.hpp:28`). PTY output therefore
  already isolates per terminal and per read and is **unaffected** by this
  defect. See Sec. 2.7 and OP9.

Two further facts sharpen the blast radius:

- **Concurrency is real.** Parallel tool calls are dispatched through
  `std::async` (`src/agent/agent_loop.cpp:1659-1660`, bounded by
  `schedule.max_parallel_tool_calls`, `:1583`), and subagents run concurrently
  in distinct sessions. A single shared mutable sink is therefore unsafe beyond
  sequential pollution.
- **The live ring is not yet surfaced.** A `UiEvent::ToolOutput` live-streaming
  event exists (`include/ymh/ui/ui_event.hpp:120,259`) but **no producer emits
  it** in `src/` (grep: only `src/ui/ui_model.cpp` consumes it, and tests
  produce it). So "live streaming" is a design contract (07 Sec. 8.2) with an
  as-yet-unwired consumer, not a shipped byte path. The fix must keep the live
  buffer populated and bounded; it cannot regress a visible stream because none
  is wired.

### 1.3 What this spec changes, in one sentence

Each tool call streams into its **own** bounded ring (the sole source of its
`materialize()`), while each sanitized chunk is fanned out, unchanged and in
order, into the **session's** live ring, which stays live-only and bounded.

### 1.4 Supersession and amendment map

| # | Clause superseded / amended (file:line) | New text (this spec) |
|---|---|---|
| A1 | 07 Sec. 11 **X10** (`docs/design/07-tools-execution.md:1344-1348`): "Tool output is bounded by the **per-session ring**" | The durable `ToolResult.output` is sourced from a **per-call** ring; the per-session ring survives as the live-only buffer. Serialized clamp + retention unchanged. See Sec. 2.5, OP5. |
| A2 | 07 Sec. 5.2 (`:543-608`): sink/ring contract | **Additive** `CallOutputSink` (per-call ring + optional live tee). `OutputSink`/`OutputRing`/`RingOutputSink` semantics unchanged. See Sec. 2.2. |
| A3 | `include/ymh/agent/agent_loop.hpp:73`: `AgentServices::output` (06 Sec. 5.5 owns the `ToolContext` pipeline; 06 Sec. 5.1 is "Turn/step cycle" and contains no such field) | **Retired.** The loop resolves the session live ring from `ResourceGovernor::ringFor(session, owner_alive)` and constructs the per-call sink itself. See Sec. 2.3. |
| A4 | `src/agent/workspace_runtime.cpp:284-285, 394, 564-565`: workspace `ring_`/`sink_` | **Deleted.** No workspace-global output sink remains. See Sec. 2.3. |
| A5 | 14 Sec. 5.2 (`:946-964`): PTY ring distinctness | **Clarification only**: this spec confirms PTY bytes never enter the call sink or `ringFor()`. No behavioral change. See Sec. 2.7. |
| A6 | 04 Sec. 8 (`include/ymh/execution/resource_governor.hpp:48,60` `ringFor`/`rings_`; `src/execution/resource_governor.cpp:57-64`): `OutputRing& ringFor`, `unique_ptr` map, no per-session eviction | **Changed additively:** the map becomes `unordered_map<string, shared_ptr<OutputRing>>`; `ringFor` takes the owner's liveness token and returns an owning `shared_ptr<OutputRing>` (or `nullptr` without inserting once the token is false -- sticky eviction, NEW-2); new `releaseSession(SessionId) noexcept` does `rings_.erase(key)` (the **map entry only**). An in-flight call's owning handle keeps the ring alive, so eviction can never free a live ring (NEW-1). Called from `AgentRegistry::dispose`/`finalizeAll` only (Sec. 2.3). |
| A7 | 07 Sec. 7.1 (`docs/design/07-tools-execution.md:1040`): `OutputRing& ringFor(SessionId);   // F5 output ring` | **Superseded by A6** (return becomes `std::shared_ptr<OutputRing>`). The F5 `OutputRing` semantics are unchanged; only ownership of the per-session handle moves to the caller for the call's duration. |

Nothing else is superseded. 07 Sec. 8.2's coalescing contract, Sec. 7.3's per-session
backpressure rationale, and 40's `retain_tool_result` are preserved.

### 1.5 Scope boundaries (out of scope)

- **Wiring the `ToolOutput` live emitter.** This spec keeps the live ring
  populated and bounded; it does not add the producer that reads it. That is 10
  Sec. 20.10's job (a named, unbuilt seam).
- **`active_output_ring_bytes` (4 MiB).** Already unimplemented --
  `ResourceGovernor::ringFor` uses `session_output_ring_bytes` only
  (`src/execution/resource_governor.cpp:57-64`). This spec does not change that;
  F5's active-session sizing stays recorded.
- **PTY output isolation.** Already correct (14 Sec. 5.2 E-P4); no change.
- **Durable truncation/retention policy.** Owned by 40; unchanged.

---

## 2. The fix: per-call output isolation

### 2.1 Chosen mechanism, and the alternatives rejected

**Chosen: a per-call `OutputRing` owned by a per-call `CallOutputSink`, with an
optional fan-out tee into the session's live `OutputRing`.**

Rejected alternatives, with the anchor that rules them out:

- **Per-call start-cursor into the shared ring.** `OutputRing` cannot express a
  cursor: it exposes only `tail()` (no byte offset) and `truncated()` latches
  permanently on first eviction (07 Sec. 5.2;
  `include/ymh/execution/output.hpp:22-43`). 14 Sec. 5.2 / E-P4 states this
  explicitly: "`OutputRing` ... cannot express a consuming cursor ... it
  carries no byte offset ... so it can serve neither FIFO consumption nor a
  per-read 'bytes evicted since the previous read' signal"
  (`docs/design/14-pty-capability.md:946-951`). A cursor would also interleave
  under the parallel `std::async` dispatch (`agent_loop.cpp:1659-1660`),
  producing cross-call bytes in the window.
- **Clear the shared ring at the start of each call.** Not concurrency-safe
  (one call clears another's in-flight bytes) and it destroys the session's
  live buffer between calls, regressing 07 Sec. 8.2. It also cannot distinguish a
  subagent's bytes from the root's.
- **Serialize all tool calls so a shared ring is safe.** Regresses the shipped
  parallel dispatch and the subagent fan-out; contradicts 16's concurrent
  supervisor/daemon model and the `schedule.max_parallel_tool_calls` seam.

The tee is what preserves live streaming: the per-call ring is the *result*
source; the session ring stays the *live* buffer. One sanitization pass feeds
both (07 Sec. 8.2 "UTF-8 at the sink" is satisfied once, not twice).

### 2.2 Interface sketch

New type in `include/ymh/execution/output.hpp` (07 Sec. 5.2 amendment, A2). ASCII:

```cpp
namespace ymh {

// Per-call writer (83 Sec. 2.2). Owns the call's own bounded ring, which is the
// ONLY source materialize() reads, so a ToolResult can never contain another
// call's, session's, or subagent's bytes. Each sanitized chunk is optionally
// fanned out to the session's live ring (`live`), which stays the UI live
// buffer (07 Sec. 8.2) and never feeds materialize(). `live` is held by OWNING
// shared_ptr (NEW-1): the sink keeps the ring alive for the call even if the
// governor's map entry is evicted mid-call, so eviction cannot dangle. Built on
// the call's stack; not copyable or movable (the async task's stack frame,
// std::async at agent_loop.cpp:1659-1660, holds it for the call's whole
// duration; OP3).
class CallOutputSink final : public OutputSink {
public:
    // `per_call_capacity` is ResourceCaps::session_output_ring_bytes;
    // `live` may be null (governor-less loop, tests) => no live tee.
    CallOutputSink(std::size_t per_call_capacity,
                   std::shared_ptr<OutputRing> live);

    CallOutputSink(const CallOutputSink&) = delete;            // LOW-3: pinned
    CallOutputSink& operator=(const CallOutputSink&) = delete; // (a copy would
    CallOutputSink(CallOutputSink&&) = delete;                 //  share live_ and
    CallOutputSink& operator=(CallOutputSink&&) = delete;      //  split call_ring_)

    void write(std::string_view chunk) override;     // sanitize once; append
    void writeErr(std::string_view chunk) override;  //   to call_ring_ + *live_
    void close() override;                           // idempotent no-op barrier

    [[nodiscard]] std::size_t bytesWritten() const noexcept override;
    [[nodiscard]] bool        truncated() const noexcept override;
    [[nodiscard]] std::string materialize(std::size_t max_bytes) const override;

private:
    OutputRing                 call_ring_;   // authoritative result buffer
    std::shared_ptr<OutputRing> live_;       // nullable; keeps the live ring
                                             //  alive across the call (NEW-1)
    std::size_t bytes_written_ = 0;
    bool        utf8_loss_ = false;
};

} // namespace ymh
```

Contract (normative):

- `write`/`writeErr` call `sanitize_utf8` **once** (07 Sec. 5.2, `output.cpp:55-76`),
  append the clean chunk to `call_ring_`, and -- iff `live_ != nullptr` --
  append the same clean chunk to `*live_`, in the same order. `bytes_written_`
  counts raw input bytes (parity with `RingOutputSink::bytesWritten`,
  `output.cpp:151`).
- `materialize(max_bytes)` returns exactly `call_ring_.tail(max_bytes)`; `live_`
  is never read. This is the isolation property (OP1).
- `truncated()` returns `utf8_loss_ || call_ring_.truncated()`. The live ring's
  `truncated()` does **not** participate (OP2): a previous call's wrap or
  another session's pressure cannot mark this result truncated.
- `close()` is a no-op final barrier, matching `RingOutputSink::close()`
  (`output.cpp:149`); the process layer already calls it
  (`src/execution/process.cpp:518-519`).
- The constructor routes capacity through `OutputRing`, which floors zero to 1
  (`output.cpp:87-88`), so a misconfigured cap cannot divide-by-zero.
- **Owning live handle and synchronization (NEW-1).** `live_` is an owning
  `shared_ptr`; the sink holds the reference from construction until
  `runToolCall` returns (after `execute(...).get()`). The governor's map holds
  the other reference; `releaseSession` drops only the map's reference, so the
  ring object survives until the last in-flight sink releases it. A concurrent
  `releaseSession`/`ringFor` pair is serialized by the governor's `mutex_`, and
  `shared_ptr` reference counting is thread-safe. The ring's own `append` is
  still unsynchronized (07 Sec. 5.2), which is why streaming tools must be
  `Exclusive` (Sec. 2.6).
- **Liveness token, sticky eviction (NEW-2).** `ringFor` takes the owning
  agent's liveness flag (`AgentLoop::disposed_`) and reads it **under the same
  `mutex_`** that `releaseSession` erases under. If the flag is false, `ringFor`
  returns `nullptr` and does **not** insert a map entry: a late call for a
  disposed session cannot resurrect the ring. The loop passes `&disposed_`; a
  null token means "always live" (governor-only tests). See Sec. 2.1, OP5, OP13.

Amended loop construction in `src/agent/agent_loop.cpp` (`runToolCall`,
replacing `:983-985`):

```cpp
payload::ToolResult AgentLoop::runToolCall(const PreparedToolCall& plan) {
    // ... unchanged preconditions; drop the `services_.output == nullptr` term ...
    // `disposed_` is AgentLoop's atomic liveness flag; pass it so a late call
    // for a disposed session gets a null live ring and never inserts an entry.
    std::shared_ptr<OutputRing> live =
        services_.governor->ringFor(session_.id(), &disposed_);
    CallOutputSink call_output(
        services_.governor->caps().session_output_ring_bytes, std::move(live));
    ToolContext context(*services_.execution, session_, *services_.logger,
                        plan.token, *services_.governor, call_output, handle,
                        plan.call.id, plan.call.turn, plan.call.step, deadline);
    result = services_.tools->execute(plan.call, context).get();
    // ... unchanged tail ...
}
```

`ResourceGovernor::ringFor(SessionId)` already exists
(`include/ymh/execution/resource_governor.hpp:48`,
`src/execution/resource_governor.cpp:57-64`); it was previously **dead** (no
caller in `src/`). This spec gives it its concrete caller (AGENTS.md "New
symbols are normative") and changes its return to an owning
`std::shared_ptr<OutputRing>` (A6/A7): the sink holds a reference for the call's
duration, so erasing the map entry can never free a ring an in-flight call still
writes (NEW-1). The map is only touched under `mutex_`; the added liveness token
makes eviction sticky (NEW-2).

Amended governor seam in `include/ymh/execution/resource_governor.hpp`
(A6; NEW-1):

```cpp
class ResourceGovernor {
public:
    // CHANGED (was `OutputRing&`): returns an owning handle. The returned
    // shared_ptr keeps the ring alive for the caller; the map holds one
    // reference too.
    // NEW-2: `owner_alive` is the owning agent's liveness flag
    // (AgentLoop::disposed_). Read under mutex_ together with the map. If it is
    // non-null and reads false, return nullptr WITHOUT inserting -- a disposed
    // session cannot be resurrected by a late call. Null means "always live"
    // (governor-only tests).
    std::shared_ptr<OutputRing> ringFor(SessionId session,
                                        const std::atomic<bool>* owner_alive = nullptr);
    // NEW: erase the map entry under mutex_ (`rings_.erase(key)`); drops the
    // GOVERNOR's reference only. The ring is destroyed by the last shared_ptr
    // holder (an in-flight CallOutputSink, or nothing if idle). Idempotent.
    void releaseSession(SessionId session) noexcept;
    // ... unchanged caps()/subprocess/PTY methods ...
private:
    // CHANGED: unique_ptr -> shared_ptr (was resource_governor.hpp:60).
    std::unordered_map<std::string, std::shared_ptr<OutputRing>> rings_;
};
```

`releaseSession` erases `rings_.erase(session.value)` under `mutex_`
(`src/execution/resource_governor.cpp:55-64`) and is idempotent; it MUST be
called only after the owner's liveness token reads false (dispose sets
`disposed_` before `releaseSession`), so together with NEW-2's token check the
eviction is sticky. It **never frees a ring that an in-flight call holds**: the
call's `CallOutputSink` retains the owning `shared_ptr` until `runToolCall`
returns, so `dispose`'s documented "never waits" behavior
(`agent_registry.hpp:73-77`) is safe with no join (NEW-1). Concrete caller
(LOW-1: the single eviction point):

- `AgentRegistry::dispose` and `finalizeAll`
  (`src/agent/agent_registry.cpp:285, 314`; `services_.governor` is in scope) --
  root and child agent teardown. `HostRuntime::deleteSession` already calls
  `runtime_.agents().dispose(agent->id())` (`src/host/host_runtime.cpp:1007`),
  so it needs **no** separate `releaseSession`; a non-resident session has no
  registered ring.

Sticky eviction closes the NEW-2 TOCTOU: a `ringFor` that runs after
`releaseSession` reads `owner_alive == false` and returns `nullptr` without
`rings_` insertion; a `ringFor` that runs before it either inserts (and is then
erased by `releaseSession`) or reads false (and inserts nothing). Either way the
map ends empty, so the Sec. 2.5 bound holds. The only residual is an in-flight
holdover: a ring erased while a call is in flight stays alive via that call's
sink until it returns, bounded by the in-flight call count.

### 2.3 Construction, ownership, and lifetime (who, where)

- **Who constructs the per-call ring/sink: `AgentLoop::runToolCall`**, at the
  point it already constructs the per-call `ToolContext`
  (`src/agent/agent_loop.cpp:963-985`). Rationale: 07 Sec. 5.1 pins the loop as the
  **only** constructor of `ToolContext` (`07:534-537`), and `runToolCall` is the
  only site with both the governor and the owning session. Constructing in
  `ToolRegistry::execute` is rejected: the registry takes `const ToolContext&`,
  has no session-scoped ring access, and is called by tests that supply their
  own context.
- **Ownership: the call's stack frame owns `CallOutputSink`, hence its
  `call_ring_`.** `ToolContext` holds `OutputSink&` (non-owning, per 07 Sec. 5.1). The
  sink outlives the `execute(...).get()` join, which is the only access window
  (`09`/`11`: the loop is the sole appender).
- **Lifetime: exactly the call's duration.** Constructed before `ToolContext`,
  destroyed when `runToolCall` returns. Each parallel `std::async` invocation
  (`agent_loop.cpp:1659-1660`) gets its own stack frame, hence its own sink --
  no sharing (OP8).
- **Live-ring ownership: `ResourceGovernor` holds one `shared_ptr` per
  `SessionId`** (04 Sec. 8; A6). Parent and each subagent have distinct sessions
  (`createChild` calls `services_.sessions->createSession(options)`,
  `src/agent/agent_registry.cpp:147-149`), hence distinct live rings (OP4). An
  in-flight call's `CallOutputSink` holds a second `shared_ptr` for the call's
  duration, so ownership is shared and eviction cannot dangle (NEW-1).
- **Live-ring eviction: `ResourceGovernor::releaseSession(session)`** at agent
  dispose (`AgentRegistry::dispose`/`finalizeAll`,
  `src/agent/agent_registry.cpp:285, 314`) only. It erases the **map entry**
  (drops the governor's reference); the ring object is destroyed by the last
  `shared_ptr` holder -- an in-flight `CallOutputSink` if one exists, else
  immediately. `HostRuntime::deleteSession` needs no separate call because it
  already disposes the agent (`src/host/host_runtime.cpp:1007`; LOW-1). Eviction
  is **sticky** (NEW-2): `ringFor` reads the owning agent's liveness token under
  the same `mutex_` and refuses to insert once the agent is disposed, so a late
  call cannot re-create the entry. The ring is live-only: a resumed/reconnected
  session (a new agent with a live token) gets a fresh empty ring, and the map
  never retains a dead session's 1 MiB (Sec. 2.5, Sec. 3, OP13).
- **`AgentServices::output` is retired** (`include/ymh/agent/agent_loop.hpp:73`)
  and the workspace members `ring_`/`sink_`
  (`src/agent/workspace_runtime.cpp:284-285, 564-565`) and their assignment
  (`:394`) are deleted. The loop needs no output field: it has the governor and
  the session. (Historical quote at
  `docs/design/46-permissions-ui-errata.md:1718` is a prior-tree excerpt and is
  not edited.)

### 2.4 Data flow (ASCII)

```text
                 per call C (its own stack frame)
  process/PTY? --no (PTY uses PtyOutputRing, 14)-->  CallOutputSink
  shell stdout/stderr bytes --> sanitize_utf8 --> call_ring_ (authoritative)
                                           \--> live_ (session ring, live-only)

  ToolRegistry::execute -- materialize(tool_result_max_bytes) --> call_ring_.tail
       |                                                        (call C only)
       v
  payload::ToolResult.output                 live consumer (10, unwired) reads
  (durable; loop appends)                    ResourceGovernor::ringFor(session,
                                             owner_alive)
```

### 2.5 Capacity and truncation semantics

- **Per-call ring capacity** = `ResourceCaps::session_output_ring_bytes`
  (1 MiB; `resource_governor.hpp:26`). This is the same numeric cap the shared
  ring had, so a single call's tail behavior is byte-identical to the intended
  pre-defect behavior, now isolated. A smaller per-call cap (e.g.
  `tool_result_max_bytes`) is rejected for v1 because it would change which
  calls latch `OutputRing::truncated()` and require re-deriving
  `retain_tool_result`'s Unknown-branch (40); keeping 1 MiB is
  behavior-preserving.
- **`tool_result_max_bytes` is preserved end-to-end.** `materialize` is still
  called with `config_.tool_result_max_bytes` at `tool_registry.cpp:358,370,376,381`
  and `builtin_tools.cpp:438,446`, and the serialized clamp + retention notice
  still run in `retain_tool_result` (`src/tools/tool.cpp:263-295`,
  `tool_registry.cpp:329`). Nothing in this spec changes 40's policy.
- **Truncation after the fix is per-call**: `result.truncated` derives only from
  this call's `call_ring_` wrap / UTF-8 loss, then `retain_tool_result` sets the
  final `omitted_kind`/`truncated`. The live ring's latched truncation is
  excluded (OP2, OP-F9).
- **Memory bound.** Total retained bytes <= (live agents + in-flight calls) x
  `session_output_ring_bytes`. The "live agents" term is the governor map
  (`ringFor` entries for sessions whose liveness token is still true); the
  "in-flight calls" term covers both each call's own `call_ring_` and any
  evicted-session ring still held alive by an in-flight sink's `shared_ptr`
  (NEW-1). In-flight calls are bounded by `schedule.max_parallel_tool_calls` x
  subtree fan-out and by the subprocess/PTY caps (07 Sec. 7.2, F8). A session's
  map entry is **evicted** by `ResourceGovernor::releaseSession(session)` when
  the agent is disposed (Sec. 2.3). Crucially, `ringFor` is **sticky** (NEW-2):
  it refuses to create an entry once the owning agent's liveness token is false,
  so a released session cannot be re-inserted by a late call, and a holdover
  cannot outlive its call. The map therefore holds no 1 MiB ring for a
  dead session -- the defect MEDIUM-1 identified, re-opened by NEW-2, and closed
  here. This is a bounded, modest increase over the old single 1 MiB buffer and
  is the price of isolation; it is still O(1) per call and never grows with
  output length (OP5, OP13).

### 2.6 Live streaming preserved

- Every byte written to the call sink is appended, sanitized once and in order,
  to the session live ring. A future `ToolOutput` emitter reading
  `ringFor(session, owner_alive)` observes the same stream as today, now
  **session-scoped** rather than workspace-scoped (a strict improvement; see
  1.2).
- `write`/`writeErr` remain non-blocking and bounded by `OutputRing::append`
  (`output.cpp:90-106`), preserving 07 Sec. 8.2's "the ring never blocks a tool".
- 07 Sec. 8.2's coalescing contract (33 ms / 64 KiB, `output_flush_interval` /
  `output_flush_bytes`) is a **consumer** concern and is untouched.
- **Streaming-tool concurrency rule (LOW-2).** Any `Tool` whose `execute` calls
  `context.output()` MUST declare
  `concurrency = ToolConcurrencyMode::Exclusive` (the enum default,
  `include/ymh/tools/tool.hpp:32,53`). `ShellTool` satisfies it: its `schema()`
  sets only `destructive` (`src/tools/builtin_tools.cpp:391-402`), so it keeps
  the default `Exclusive`. Therefore at most one call per session streams into
  that session's live ring at a time, and the unsynchronized
  `OutputRing::append` (`src/execution/output.cpp:90-106`; no lock, by design,
  14 Sec. 5.2) has a single writer per session. A future ParallelSafe streaming
  tool MUST add a live-ring lock before it is registered; that is a stated
  precondition, not part of this spec (OP8(iii)). This removes the previously
  unstated dependency the gate flagged.

### 2.7 `terminal`/PTY, `shell`, and direct-output tools

- **`shell`** (`src/tools/builtin_tools.cpp:405-451`) is the only tool that
  streams into the sink (`:419`). After the fix it streams into the per-call
  sink; its three `materialize` sites (`:438,446` and the registry fallbacks)
  read only that call's ring.
- **`terminal`/PTY** is **unaffected and needs no change**: it writes to
  `PtyOutputRing` and reads with a consuming cursor (14 Sec. 5.2 E-P4), returning
  `result.output` directly (`terminal_tool.cpp:237`). It never touches the call
  sink. A5 records the clarification only.
- **Direct-output tools** set `result.output` themselves and never use the
  sink: `read_file`/`write_file`/`edit_file`/`grep`/`glob`
  (`builtin_tools.cpp:139,177,251,332,378`), `git_*`
  (`git_tools.cpp:77,121`), `terminal`, `skill`, `subagent`/`job`, and MCP
  tools. They are unaffected. Crucially, the fallback at
  `tool_registry.cpp:357-359` now reads the per-call ring (empty for these
  tools), so an empty direct result no longer inherits stale bytes -- an extra
  defect closed by the same fix (OP7).
- **Error/cancel paths** (`tool_registry.cpp:370,376,381`) materialize the
  per-call ring, so an error result carries only the failing call's partial
  output.

---

## 3. State lifetime table (new and changed state)

Every row is normative. "Restart" means daemon process restart; "switch" means
session switch within a live daemon.

| State | Type | Created | Owned by | Destroyed / evicted | Survives restart? | Survives switch? | Crash path |
|---|---|---|---|---|---|---|---|
| Per-call output ring | `OutputRing` (`call_ring_`) | `CallOutputSink` ctor in `AgentLoop::runToolCall` (agent_loop.cpp:963) | the call's stack frame | `runToolCall` returns, after `execute().get()` | no (live-only) | no | unwinds with the exception; `execute` `catch` (agent_loop.cpp:988-994) still returns a result |
| Per-call sink | `CallOutputSink` | same | same | same | no | no | same |
| Session live ring | `OutputRing` | first `ResourceGovernor::ringFor(session, owner_alive)` while `owner_alive` is true | shared: governor map `shared_ptr` + each in-flight `CallOutputSink` `live_` | the LAST `shared_ptr` holder releases it: the in-flight sink if one exists (on `runToolCall` return), else `releaseSession` at `AgentRegistry::dispose`/`finalizeAll` (`agent_registry.cpp:285,314`); else daemon exit | no (live-only; rebuilt empty) | yes (keyed by SessionId; a resumed session has a live agent token, so rebuilt empty) | process death discards it; a reconnect/resume gets a fresh empty ring |
| `ringFor` map entry | `unordered_map<std::string, shared_ptr<OutputRing>>` | first `ringFor` for a SessionId while its owner token is true | governor | erased by `releaseSession` at agent dispose (same callers); **never re-inserted once the owner token is false** (NEW-2); else daemon exit | no | yes | discarded with the process |
| Live-ring eviction | `releaseSession` call | agent dispose (after the owner token flips false) | governor | erases the map entry immediately; the ring object is freed by the last `shared_ptr` holder (never while a call holds it); a subsequent `ringFor` returns `nullptr` (no re-insert) | no | n/a (entry gone) | n/a |
| `live_` owning handle | `shared_ptr<OutputRing>` | `CallOutputSink` ctor | the call's sink | with the sink (after `execute().get()`); keeps an evicted ring alive across the call | no | no | released on unwind |
| `bytes_written_` / `utf8_loss_` | `size_t` / `bool` | sink ctor | the call's sink | with the sink | no | no | discarded |
| Live-ring content | bytes | every `write`/`writeErr` tee | governor ring | evicted oldest-first on overflow; daemon exit | no | shared across calls of one session | lost |

Eviction/call ordering (NEW-1, NEW-2). `releaseSession` MUST be safe against an
in-flight call, and is: it only unregisters the map entry, while the in-flight
`CallOutputSink` holds its own `shared_ptr` until `runToolCall` returns. For
**use-after-free**, ordering is irrelevant -- `ringFor` returns a strong ref
before any erase, and the ring is destroyed only when both references are gone;
no join or quiescence is required, matching `AgentRegistry`'s documented "never
waits" semantics (`include/ymh/agent/agent_registry.hpp:73-77`). For the
**bound** (NEW-2), ordering is made irrelevant by the liveness token: `dispose`
flips `AgentLoop::disposed_` before `releaseSession`, and `ringFor` reads that
token under the governor's `mutex_`. Interleaving (D = dispose thread, T = a
late async call): (i) T locks before D flips -> inserts; D then flips and erases
-> empty; (ii) T locks after D flips, before/after `releaseSession` -> reads
false, returns `nullptr`, inserts nothing -> empty. In both cases the map ends
empty, so OP5's bound holds; a session's ring is only rebuilt when a **new live
agent** for that SessionId calls `ringFor` with a true token (resume/re-open).

Removed state: `WorkspaceRuntime::ring_`, `WorkspaceRuntime::sink_`
(`src/agent/workspace_runtime.cpp:284-285,564-565`) and the
`AgentServices::output` pointer (`include/ymh/agent/agent_loop.hpp:73`). Their
lifetime notes are void; the workspace no longer owns any output buffer.

---

## 4. Invariants

| ID | Invariant | Where |
|---|---|---|
| **OP1** | **Result isolation.** `ToolResult.output` for a call C contains only bytes C wrote to its own sink (then clamped/retained). No byte from another call, session, or subagent. | Sec. 2.1-2.2, Sec. 2.7 |
| **OP2** | **Live/result separation.** The session live ring never feeds any call's `materialize()` or `truncated()`; it is a write-only fan-out target for the result path. | Sec. 2.2, Sec. 2.5 |
| **OP3** | **One sink per call.** Exactly one `CallOutputSink` exists per dispatched `ToolContext`; it is constructed by `runToolCall`, lives on the call's stack, and is never shared or retained by a tool. | Sec. 2.3, 07 Sec. 5.1 |
| **OP4** | **Per-session live ring.** `ringFor(session, owner_alive)` yields one live ring per live `SessionId` as an owning `shared_ptr`; root and child sessions have distinct rings. | Sec. 2.3, 04 Sec. 8, A6 |
| **OP5** | **Bounded.** Per-call ring <= `session_output_ring_bytes`; live ring <= `session_output_ring_bytes`; `materialize` clamps to `tool_result_max_bytes`; serialized-size retention (40) unchanged. No ring grows with output length, and the live-ring map holds an entry only for a session with a live owner token: `releaseSession` erases, and `ringFor` is **sticky** -- it will not re-insert once the token is false (NEW-2). | Sec. 2.5, Sec. 3, X10 |
| **OP6** | **Non-blocking.** `write`/`writeErr` never block and never exceed capacity; overflow evicts oldest-first and latches truncated. | Sec. 2.2, 07 Sec. 5.2 |
| **OP7** | **Direct-output pass-through.** A tool that assigns `result.output` is returned unchanged; the empty-output fallback materializes the per-call ring (empty for such tools), never the live ring. | Sec. 2.7 |
| **OP8** | **Concurrency safety.** (i) Every `runToolCall` owns its sink and per-call ring, so concurrent calls cannot bleed into each other's results, in one session or across sessions. (ii) Live rings are per-session, so cross-session concurrency cannot interleave live bytes. (iii) Within a session, at most one streaming call runs at a time because every output-streaming tool is `Exclusive` (Sec. 2.6), so the unsynchronized live ring has a single writer; a ParallelSafe streaming tool MUST add a live-ring lock before registration. (iv) Eviction is safe against concurrent calls by `shared_ptr` ownership and is sticky against late calls by the liveness token (OP13, NEW-1/NEW-2). | Sec. 2.3, Sec. 2.6, OP13, agent_loop.cpp:1659; T4/T5/T6 |
| **OP9** | **PTY isolation preserved.** `terminal` reads `PtyOutputRing` per `PtySession` with a consuming cursor; PTY bytes never enter the call sink or `ringFor()`. | Sec. 2.7, 14 Sec. 5.2 |
| **OP10** | **Live parity.** The live ring receives the same sanitized byte sequence, in the same order, as the call sink; no byte that reached the old sink is dropped from the live buffer. | Sec. 2.6 |
| **OP11** | **No cross-session bleed.** A call in session A cannot observe session B's bytes in its result, regardless of the live rings' contents. | OP1, OP2 |
| **OP12** | **Totality preserved.** Every call still yields exactly one `ToolResult` (X9); unknown tool, invalid args, timeout, and cancellation materialize the per-call ring, never a stale one. | Sec. 2.7, X9 |
| **OP13** | **Eviction safety and stickiness (NEW-1, NEW-2).** (a) `releaseSession` never frees a per-session live ring that an in-flight call can still reference: the call's `CallOutputSink` owns a `shared_ptr` for the call's duration, and the ring is destroyed only when the last holder releases it; no join/quiescence is required. (b) A released session cannot be resurrected: `ringFor` reads the owner's liveness token under the governor's `mutex_` and returns `nullptr` without inserting once the token is false, so the map entry stays gone. | Sec. 2.2, Sec. 2.3, Sec. 3 |

Any code path violating OP1-OP13 is a defect.

---

## 5. Failure modes

Component-local; disjoint from the top-level F1-F12 of 00 Sec. 54 and from 07's
`E-F#`, 14's `P-F#`, 16's `O-F#`. `OP-F#` is used only here.

| ID | Failure mode | Handling | Test |
|---|---|---|---|
| **OP-F1** | A non-streaming tool with empty `result.output` inherits stale ring bytes via `tool_registry.cpp:357-359`. | OP7: fallback reads the per-call ring (empty). | Sec. 6.1 T2 |
| **OP-F2** | Two sequential `shell` calls in one session: the second result contains the first's output. | OP1: each call has its own ring. | Sec. 6.1 T2 |
| **OP-F3** | Concurrent tool calls (parent + subagent, or two agents) share/interleave a result buffer. | OP3/OP8: one sink and per-call ring per `runToolCall` invocation; live rings are per-session. | Sec. 6.1 T4, Sec. 6.2 T5 |
| **OP-F4** | Root and subagent calls cross-contaminate through the shared `AgentServices::output`. | OP3/OP4: per-call sink + per-session live ring; field retired. | Sec. 6.2 T5 |
| **OP-F5** | Session A's output appears in session B's result. | OP1/OP11: `materialize` reads only the caller's ring. | Sec. 6.2 T5 |
| **OP-F6** | Clearing/resetting the live ring between calls would break live streaming. | Rejected mechanism; OP2 forbids result coupling, OP10 keeps the live buffer appended. | Sec. 6.1 T1 |
| **OP-F7** | `session_output_ring_bytes` misconfigured to 0, or memory exhaustion from per-call buffers. | `OutputRing` floors capacity to 1; OP5 bounds total memory. | Sec. 6.1 T1 |
| **OP-F8** | The sink outlives its `ToolContext` and dangles. | OP3: sink is on `runToolCall`'s stack and `.get()` joins before return. | code review + T3 |
| **OP-F9** | A prior call's ring wrap marks a later result `truncated` (historical latch). | OP2/Sec. 2.5: truncation is per-call; live latch excluded. | Sec. 6.1 T1 |
| **OP-F10** | `terminal` output regresses (e.g. routed through the call sink). | OP9: terminal keeps using `PtyOutputRing`; no change. | `tests/unit/pty_test.cpp` (existing) |
| **OP-F11** | `tool_result_max_bytes` clamp/retention regresses. | Sec. 2.5: clamp + `retain_tool_result` unchanged. | `tests/unit/output_test.cpp`, `unit/retention_test.cpp` (existing) |
| **OP-F12** | Live buffer becomes empty/unbounded. | OP5/OP6/OP10: tee preserves content; ring bound unchanged. | Sec. 6.1 T1 |
| **OP-F13** | Eviction hazards: (a) `releaseSession` frees a live ring an in-flight call still references (use-after-free / data race; NEW-1); (b) a late `ringFor` after `releaseSession` re-inserts a dead-session entry that is never evicted (bound broken; NEW-2). | OP13(a): the call's `CallOutputSink` owns a `shared_ptr` for the call's duration; `releaseSession` drops only the map's reference. OP13(b): `ringFor` reads the owner token under `mutex_` and returns `nullptr` without inserting once it is false. No join needed. | Sec. 6.2 T6 |

---

## 6. Test plan

Strategy is 00 Sec. 44: unit, hermetic integration (fake LLM / fake FS / fake
shell), golden/replay, and an opt-in live layer. The deterministic layers run
offline; live tests are opt-in (`YMH_LIVE_LLM=1`). No test uses sleep-based
synchronisation. New hermetic tests land in a new file
`tests/unit/errata83_output_isolation_test.cpp` and must be added to the
`ymh_tests` source list in `tests/CMakeLists.txt` (alongside
`unit/output_test.cpp`, currently line 21).

### 6.1 Unit tests (`tests/unit/errata83_output_isolation_test.cpp`)

| Test | Covers |
|---|---|
| **T1 -- CallOutputSink isolation + live tee.** Two `CallOutputSink`s sharing one live ring: sink A writes `"alpha"`, sink B writes `"beta"`; assert `A.materialize()` == `"alpha"`, `B.materialize()` == `"beta"`, and the shared live ring's tail == `"alphabeta"`. Assert B's `truncated()` is false even when A's per-call ring wrapped (live latch excluded). | OP1, OP2, OP9, OP-F6/7/9 |
| **T2 -- Two `shell` calls, one session, no bleed.** Reuse `ToolEnv` for the workspace, governor, session, logger, and registry, but **do not call `ToolEnv::context()`**: it binds a `RingOutputSink` (`tests/support/test_env.hpp:173-182`) and cannot hand back a `CallOutputSink`-backed context. Instead construct each `ToolContext` directly over a fresh `CallOutputSink` (per-call ring + the session live ring), mirroring `runToolCall` (`agent_loop.cpp:254-264`). Execute `shell` `"printf FIRST"` then `shell` `"printf SECOND"` through `ToolRegistry::execute`. Assert result 2's `output` contains `SECOND` and does **not** contain `FIRST`; assert result 2's `truncated`/`omitted_kind` are independent of result 1. A companion case asserts the empty-`result.output` fallback (`tool_registry.cpp:357-359`) does not inherit bytes. (Alternatively extend `ToolEnv` to accept an injected `OutputSink`; either is acceptable.) | OP1, OP7, OP11, OP-F1/2 |
| **T3 -- Sink lifetime.** A `CallOutputSink` used inside a scope is destroyed after the registry call; `materialize()` is called only while alive; a copy/move is a compile error (the deleted overloads, Sec. 2.2). | OP3, OP-F8 |

Existing unit tests stay green: `tests/unit/output_test.cpp` (ring/sink
semantics, `materialize` cap), `tests/unit/retention_test.cpp` and
`tests/unit/output_test.cpp` (retention), `tests/unit/tools_test.cpp` (shell/PTY
tool behavior), `tests/unit/pty_test.cpp` (PTY isolation unaffected).

### 6.2 Concurrency tests (same new file, via `tests/support/agent_test_env.hpp`)

The gate found the original T4 vacuous: `shell` is `Exclusive`
(`include/ymh/tools/tool.hpp:53` default; `src/tools/builtin_tools.cpp:391-402`),
and `execute_tool_calls` drains every in-flight call before each Exclusive call
(`src/agent/agent_loop.cpp:1625-1633`), so two `shell` calls never overlap.
Both tests below use a **deterministic rendezvous seam** (a `std::barrier`, not
sleep) so the two writers are provably in flight at the same time.

| Test | Covers |
|---|---|
| **T4 -- `CallOutputSink` concurrent isolation.** Construct two `CallOutputSink`s (each its own per-call ring; `live` null or two distinct session rings, so the unsynchronized `OutputRing` is never cross-written) sharing a `std::barrier(2)`. On two `std::thread`s, each thread writes its marker, `arrive_and_wait()`s, then materializes; assert each `materialize()` returns only its own marker and never the other's. This exercises the result path under true concurrency, independent of the tool scheduler; run under TSan. | OP1, OP3, OP8(i), OP-F3 |
| **T5 -- Parent + subagent concurrent streaming (cross-session).** Register a test-only `RendezvousStreamTool` in the shared `ToolRegistry` (both agents see it, 07 Sec. 4). Its `execute` writes `label` to `context.output()`, waits on a shared `std::barrier(2)`, then returns `context.output().materialize(cap)`. Create a child session (`AgentRegistry::createChild`, `src/agent/agent_registry.cpp:147-149`) and run the parent turn and the child turn on two `std::thread`s, each `FakeLLM`-scripted to call `rendezvous{label:"PARENT"}` / `"CHILD"`. The barrier proves both `execute` bodies overlap. Assert (i) each durable `ToolResult.output` contains only its own label, and (ii) the ring returned by `governor.ringFor(parent)` holds only `PARENT` bytes while the ring returned by `governor.ringFor(child)` holds only `CHILD` bytes. Different sessions => different live rings, so the unsynchronized ring has one writer each. | OP1, OP4, OP8(ii), OP11, OP-F3/4/5 |
| **T6 -- Eviction safety and stickiness (NEW-1, NEW-2).** With `std::atomic<bool> alive{true}`: (a) hold `auto ring = governor.ringFor(session, &alive);`, then `alive = false; governor.releaseSession(session);` while `ring` is still alive; assert `ring` stays valid, `ring->append(...)` still succeeds and `ring->tail(...)` returns the bytes (no use-after-free, OP13a). (b) After dropping `ring`, call `governor.ringFor(session, &alive)` again and assert it returns **`nullptr`** and inserts nothing (the map stays empty) -- the released session is not resurrected (OP13b). (c) A race variant: an async `runToolCall` for the disposed session starts after `releaseSession`; assert it produces its result but leaves the map empty (no re-created entry). Also drive a variant where the entry is released while a `CallOutputSink` bound to it is mid-`write`; run under ASan/TSan. | OP5, OP13, OP-F13 |

### 6.3 Live (opt-in)

A live variant of T2/T5 (`YMH_LIVE_LLM=1` + `DEEPSEEK_API_KEY`) runs two real
`shell` calls in one session and asserts no bleed; skipped otherwise. This is a
regression guard, not the primary coverage.

### 6.4 CMake

Add `unit/errata83_output_isolation_test.cpp` to the `ymh_tests` target in
`tests/CMakeLists.txt`. Test support `tests/support/agent_test_env.hpp:86`
(`services.output = &output;`) must be updated to the retired-field shape (no
output sink; optional `ResourceGovernor` is already supplied). This is test
scaffolding, not production behavior.

---

## 7. dsh (DeepSeek Harness) mapping

dsh is the architectural reference (00 Sec. 55; 07 Sec. 13; 14 Sec. 12). This errata is
implementation-internal: it changes how ymh bounds a tool result, not a dsh
capability boundary. Rows below cite the shipped anchor for the disposition
(56-D6 requires every non-mirror cell to carry a reason and an anchor).

| dsh concept | ymh equivalent | Disposition |
|---|---|---|
| execution world / capability seam (`Sec. 4.4`, `Sec. 18`, 07 Sec. 13) | `ExecutionEnvironment` | **Mirrored, unchanged.** This spec does not move output capture across the seam; it stays in `execution/output`. Anchor: 07 Sec. 13 `:1439`. |
| tool invocation context (07 Sec. 13 `:1438`) | `ToolContext`, per-call and non-owning (07 Sec. 5.1 `:532-537`) | **Mirrored, tightened.** The context already promised "per-call"; the sink it carries is now per-call too. Anchor: `include/ymh/tools/tool_context.hpp:3-6`, 07 Sec. 5.1. |
| per-tool-invocation result capture | per-call `OutputRing` inside `CallOutputSink` | **Additive mirror.** ymh defines its own bounded per-invocation capture; dsh's reference names the invocation context, not the buffer. Anchor: 07 Sec. 5.2 `:543-608`; this spec Sec. 2.2. |
| live output streaming / coalescing (00 Sec. 9.11, 07 Sec. 8.2, 10 Sec. 20.10) | per-session `OutputRing` via `ringFor`, live-only; `ToolOutput` emitter unbuilt | **Mirrored buffer, unbuilt consumer (justified deferral).** The live ring stays populated and bounded; the `ToolOutput` producer is 10 Sec. 20.10's seam and no consumer is wired (grep: `ui_event.hpp:120` consumed only by `ui_model.cpp`). Anchor: 10 Sec. 20.10; `include/ymh/ui/ui_event.hpp:120`. |
| append-only trace; results, not live events, are durable (07 Sec. 13 `:1444`; 01 I4) | loop appends one `payload::ToolResult`; live ring never durable | **Mirrored, unchanged.** X8/X9 and 07 Sec. 8.2's durable-at-barrier rule stand. Anchor: 07 Sec. 11 X8/X9; `agent_loop.cpp:1007`. |
| capability PTY stream (14 Sec. 12 `:1727-1732`) | `PtyOutputRing` per `PtySession` (E-P4) | **Mirrored, unchanged.** Already isolated; distinct from `ringFor`. Anchor: 14 Sec. 5.2 `:946-964`, `src/execution/pty.cpp:185,548`. |
| durable output clamp (01 S10, 02 Sec. 4.1, 40) | `retain_tool_result` + serialized-size clamp | **Mirrored, unchanged.** This spec preserves the clamp; it changes only the source ring. Anchor: 07 Sec. 5.2 `:599-608`; `src/tools/tool.cpp:263-295`. |
| per-session output backpressure (07 Sec. 7.3) | per-session live ring bounded by `session_output_ring_bytes` | **Mirrored.** The result path adds a per-call bound; total memory remains cap-bounded. Anchor: 07 Sec. 7.3 `:1074-1082`. |

**Deliberate omission.** `active_output_ring_bytes` (4 MiB active-session ring)
remains unimplemented, as before; `ringFor` uses
`session_output_ring_bytes` for every session (`resource_governor.cpp:57-64`).
Reason: pre-existing unbuilt feature with no consumer; changing it is out of
scope and would not affect isolation. Anchor: 07 Sec. 7.1 `:1053-1054`.

---

## 8. Decisions

| # | Question | Recommendation | If rejected |
|---|---|---|---|
| **83-D1** | Per-call buffer or a cursor into the shared ring? | **Per-call ring.** `OutputRing` cannot express a cursor (no offset, latched truncation) and would interleave under parallel dispatch. | Cursor gives cross-call bleed under concurrency and cannot reconstruct evicted bytes; OP1 fails. |
| **83-D2** | Isolate by clearing the shared ring per call? | **No.** Not concurrency-safe and destroys the live buffer between calls. | Regresses live streaming; root/subagent still interleave. |
| **83-D3** | Where is the per-call sink constructed? | **`AgentLoop::runToolCall`** (the sole `ToolContext` constructor, 07 Sec. 5.1). | `ToolRegistry::execute` has no session-scoped ring and takes a const context; tests supply their own context. |
| **83-D4** | Does the live ring stay workspace-wide or become per-session? | **Per-session**, via the previously-dead `ResourceGovernor::ringFor(session, owner_alive)`. | Workspace-wide live ring mixes sessions (X10/F5 say per-session) and leaves `ringFor` dead. |
| **83-D5** | Keep `AgentServices::output`? | **Retire it**; the loop resolves the live ring from the governor. | A persistent sink field invites re-sharing; the workspace `ring_`/`sink_` would remain. |
| **83-D6** | Per-call ring capacity: `session_output_ring_bytes` or `tool_result_max_bytes`? | **`session_output_ring_bytes`** (behavior-preserving; same numeric cap as before). | A smaller cap changes `OutputRing::truncated()` latching and re-opens 40's Unknown-branch derivation. |
| **83-D7** | Does the fix touch `terminal`/PTY? | **No.** PTY already uses `PtyOutputRing` (E-P4); only a clarifying cross-reference is added. | Routing PTY through the call sink would regress the per-terminal consuming cursor (14 Sec. 5.2). |
| **83-D8** | How is the per-session live-ring map bounded? | **Unregister on teardown:** `ResourceGovernor::releaseSession(session)` from `AgentRegistry::dispose`/`finalizeAll` (single eviction point; `HostRuntime::deleteSession` disposes the agent, so no separate call -- LOW-1). | The map retains one 1 MiB ring per SessionId ever created for the daemon lifetime, falsifying the Sec. 2.5 bound (MEDIUM-1). |
| **83-D9** | May an output-streaming tool be ParallelSafe? | **Not until it locks the live ring.** Streaming tools are `Exclusive` (the default; `ShellTool` qualifies), giving the unsynchronized ring one writer per session. | Two same-session tees race `OutputRing::append` (UB); OP8(iii) would be false (LOW-2). |
| **83-D10** | How is eviction safe against in-flight and late calls (NEW-1, NEW-2)? | **Owning `shared_ptr` + sticky liveness token.** The map holds one reference and each in-flight `CallOutputSink` another, so `releaseSession` (which does `rings_.erase(key)`) never frees a ring a call holds; and `ringFor(session, owner_alive)` reads the owner token under the governor's `mutex_`, returning `nullptr` without inserting once the owner is disposed, so a late call cannot resurrect a dead-session entry. No join/quiescence needed, so `dispose`'s "never waits" is preserved. | A raw `live_` pointer (Rev 2) dangles when `dispose` erases the entry mid-call (UAF); a token-free `ringFor` (Rev 3) re-inserts the entry after release, breaking the Sec. 2.5 / OP5 bound. |

---

## 9. References

- `docs/design/07-tools-execution.md` Sec. 5.1 (`ToolContext`), Sec. 5.2
  (`OutputSink`/`OutputRing`), Sec. 7.1 (`ResourceCaps`), Sec. 7.3 (F5 rationale), Sec. 8.2
  (streaming/coalescing), Sec. 11 X8-X10, Sec. 12.1 F5, Sec. 13 (dsh), Sec. 14 (tests).
- `docs/design/14-pty-capability.md` Sec. 5.2 (`PtyOutputRing`, E-P4 `:946-964`),
  Sec. 12 (dsh).
- `docs/design/06-agent-loop.md` Sec. 5.5 (`ToolContext` pipeline). Note: 06
  Sec. 5.1 is "Turn/step cycle" and owns no `AgentServices` output field; the
  retired field is anchored at `include/ymh/agent/agent_loop.hpp:73` (LOW-1).
- `docs/design/04-workspace-host-daemon.md` Sec. 2.1 (`ResourceCaps`), Sec. 8
  (`ResourceGovernor`; this spec changes `ringFor` to take an owner-liveness
  token and return `std::shared_ptr<OutputRing>`, changes `rings_` to
  `shared_ptr`, and adds `releaseSession` with `erase` semantics).
- `docs/design/07-tools-execution.md` Sec. 7.1 (`:1040`
  `OutputRing& ringFor(SessionId)`) -- superseded by A6/A7.
- `docs/design/40-output-retention.md` Sec. 5 (`retain_tool_result`).
- `docs/design/00-architecture.md` Sec. 9.11 (resource caps), Sec. 44 (testing), Sec. 55
  (dsh), Sec. 54 (F1-F12).
- `docs/design/56-dsh-fidelity-gap-closure-errata.md` Sec. 3 (56-D6: a non-mirror
  carries a reason and an anchor).
- Code: `include/ymh/execution/output.hpp:5-80`,
  `src/execution/output.cpp:87-159`,
  `src/agent/workspace_runtime.cpp:284-285,394,564-565`,
  `src/agent/agent_loop.cpp:445-463,963-996,1625-1633,1659-1660`
  (`:445-463` `dispose()` sets `disposed_` before any release),
  `include/ymh/agent/agent_loop.hpp:73,255` (`:73` retired output field; `:255`
  the `disposed_` liveness atomic passed to `ringFor`),
  `src/agent/agent_registry.cpp:147-149,219,235,285,314`,
  `include/ymh/agent/agent_registry.hpp:73-77`,
  `src/host/host_runtime.cpp:945-951,961-1016` (`:1007` disposes the agent),
  `include/ymh/agent/workspace_runtime.hpp:148`,
  `src/tools/builtin_tools.cpp:391-402,405-451`,
  `src/tools/terminal_tool.cpp:170-303`,
  `src/tools/tool_registry.cpp:323-388`,
  `src/tools/tool.cpp:263-295`,
  `include/ymh/tools/tool.hpp:32,53`,
  `include/ymh/execution/resource_governor.hpp:26,48,60`,
  `src/execution/resource_governor.cpp:55-64` (`ringFor` map access; token
  checked under `mutex_`),
  `include/ymh/tools/tool_context.hpp:3-6`,
  `tests/support/test_env.hpp:165-182`,
  `tests/support/agent_test_env.hpp:86`,
  `tests/CMakeLists.txt:21`.
