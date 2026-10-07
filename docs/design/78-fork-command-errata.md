# 78 - Fork Command Errata: a TUI `/fork`

```
Status: verified (Rev 4)
Revision: 4
Verification status: adversarial reviewer (independent, Rev 4): GATE PASS (0 HIGH / 0 MEDIUM, 4 LOW); Oracle confirm PASS
Component: 78 (errata) - adds a supervisor-local `/fork` command surface; amends
           01-session.md, 42-agent-presets.md, 22-switcher-sessions-errata.md,
           57-switcher-sessions-popup-errata.md, 10-supervisor-tui.md,
           05-transport.md, and 11-m2-errata.md by reference only. It does not
           rewrite them.
Depends on: 00-architecture.md (verified), 01-session.md (verified),
            02-persistence.md (verified), 03-workspace-registry.md (verified),
            04-workspace-host-daemon.md (verified), 05-transport.md (verified),
            06-agent-loop.md (verified), 10-supervisor-tui.md (verified),
            13-context-compaction.md (verified),
            16-daemon-ownership.md (verified), 19-session-rename-errata.md
            (verified), 22-switcher-sessions-errata.md (verified),
            23-session-lifecycle-errata.md (verified),
            24-agent-lifetime-errata.md (verified), 42-agent-presets.md
            (verified), 55-multi-agent-delegation-errata.md (verified),
            57-switcher-sessions-popup-errata.md (verified)
Scope: one user-requested feature: a TUI slash command `/fork` that branches the
       focused session into a new, independent session inside the SAME workspace
       daemon, leaving the original running. The fork plumbing already exists end
       to end below the UI (`Session::fork`, `SessionManager::forkSession`,
       `HostRuntime::forkSession`, the `session.fork` RPC, the `ymh fork` CLI);
       the missing surface is a `CommandContext::fork` hook, a supervisor RPC
       caller beside the resume path, and the focus/subscribe of the child. The
       spec also fixes the shipped inheritance gap in `Session::fork`
       (`agent_preset`, `endpoint`, `profile_id` are not copied). Design only: it
       contains no implementation code.
       Out of scope: `/rewind` (spec 79) and any file/code checkpoint; a
       turn-aligned seed picker; the `--fork-session`/`/branch` in-process switch;
       a per-session background process.
Supersedes: 05 sec.7.4 :892 ("session.fork  params: { session, seed_length }" -
            the wire `seed_length` is no longer required; an absent field means
            "full resolved view"); 11 sec.4.2 :467 ("forkSession(const
            protocol::SessionId&, std::int64_t seed)" - the frozen
            `TransportHost` override is replaced by the `std::optional<std::int64_t>`
            form pinned in sec.4.3). No other prior normative text is withdrawn.
Amends:     01 sec.9.3 (:974-978) - the fork header construction becomes "copy the
            parent's fixed route and composition" (adds `model_name`, `endpoint`,
            `profile_id`, `agent_preset`, `permission_preset`, and pins `depth`
            inheritance explicitly; the shipped code already copies
            `model_name`/`permission_preset`/`depth`);
            42 sec.3.4 (:726-732) - the "a forked session inherits the source's
            depth" rule is retained and extended to `agent_preset` (a fork is the
            same agent lineage);
            22 sec.7 SW13 and 57 sec.3/sec.4 - additive only, explicitly NOT
            superseding either: `/fork` reuses the "focus only; no
            `session.activate`" rule and the live-vs-stored split; it changes
            neither switcher's pinned behaviour;
            10 sec.9.2 (:1064) - the UI input layer delegates a leading `/` to
            `CommandRegistry` (spec 26); `/fork` is a built-in registered there
            before the `/help` snapshot (`src/ui/command_registry.cpp:285`).
            (Re-anchored from `10 sec.8.2`, which is the renderer subsection,
            not the command surface - see NEW-7 in the Rev-4 log);
            05 sec.7.4 (:887-908) - the `session.fork` wire contract and mapping
            prose: `seed_length` becomes optional (absent = full resolved view)
            and the mapping at :906 passes `std::nullopt`;
            11 sec.4.3 :522 - the `forkSession` -> `SessionManager::forkSession(parent,
            seed)` mapping now receives `std::optional<std::int64_t>` (`nullopt` =
            the parent's current resolved-view length).
```

---

## 1. Purpose and scope

Claude Code's `/fork` copies the current conversation into a new **background**
session and leaves the original running. ymh already has every substrate piece:

| Layer | Symbol | Anchor |
|---|---|---|
| Session | `Session::fork(parent, seedLength, store, bus)` | `src/session/session.cpp:766-801` |
| Manager | `SessionManager::forkSession(const SessionId&, std::size_t)` | `src/session/session_manager.cpp:98-116` |
| Daemon | `HostRuntime::forkSession(const SessionId&, std::int64_t)` | `src/host/host_runtime.cpp:827-852` |
| Wire | `method::kSessionFork = "session.fork"` | `include/ymh/transport/protocol.hpp:532`; `src/transport/protocol_server.cpp:424-434` |
| CLI | `ymh fork SESSION` | `src/cli/session_cli.cpp:325-365` |
| Schema | `sessions.kind/parent_session/seed_length` + CHECK matrix | `src/session/session_persistence.cpp:36-51` |
| Header | `SessionHeader.kind/parentSession/seedLength/depth/agent_preset/permission_preset` | `include/ymh/session/session.hpp:44-71` |

What is missing is only the **UI surface and caller**: there is no `kSessionFork`
caller under `src/ui/` (the TUI cannot fork), no `/fork` command, and no
focus/subscribe of the child. This spec pins that surface.

This spec is deliberately small and additive:

1. A supervisor-local `/fork` command (78-D1).
2. A `session.fork` RPC invoked beside the resume path, with the child focused
   and subscribed (78-D1, 78-D5).
3. A default seed boundary of "the parent's current resolved-view length", with
   an optional explicit integer boundary; **no turn picker** (78-D2).
4. A fix to the shipped `Session::fork` inheritance gap (78-D3).
5. A decision that fork **inherits** delegation depth and does **not**
   increment it (78-D4).

## 2. Current behaviour (verified against the tree, not trusted from the brief)

**2.1 The fork chain is complete and atomic on the parent side.** `Session::fork`
reads `parent.events()` (the resolved view), rejects `seedLength > view.size()`
with `InvalidForkBoundary` (`session.cpp:768-771`), builds a child header
(`:775-789`), calls `store.create(child)` (`:791`), and appends the child's own
`SessionStarted` (`:794-799`). It never appends to the parent; `parent` is a
`const Session&`. The child's resolved view is the parent prefix
`[0, seedLength)` followed by the child's own events (`01` I10;
`session_persistence.cpp:486-514`, `resolve_after_locked`; `Session::ownEvents`
at `session.cpp:617-627` strips the prefix).

**2.2 The daemon side owns the child independently.** `HostRuntime::forkSession`
holds an owning parent handle, calls `SessionManager::forkSession`, then
`acquireLeaseOrThrow(child)`, `runtime_.agents().resume(child)`, and
`registry_.addSession(workspace, child)` (`host_runtime.cpp:834-847`). The child
therefore gets its own `session_leases` row, its own agent, and its own
`workspace_sessions` junction row. `forkSession` does **not** touch
`active_session_`; only `HostRuntime::activateSession` does
(`host_runtime.cpp:937-948`). The reply is `protocol::SessionCreated{session,
header}` (`include/ymh/transport/host.hpp:48-51`), and
`ProtocolServer::onSessionCreated` broadcasts a `SessionCreated` host notice to
every Interactive connection (`protocol_server.cpp:842-851`), which the
supervisor consumes by calling `refresh_sessions(workspace)`
(`src/ui/supervisor.cpp:962-966`).

**2.3 The wire handler requires `seed_length`.** `protocol_server.cpp:424-434`
throws `InvalidParams` when `seed_length` is absent or not an integer. A `/fork`
with no argument therefore cannot simply omit it today; sec.4 pins the additive
default.

**2.4 The inheritance gap.** `Session::fork`'s header block
(`session.cpp:775-788`) copies `cwd`, `createdAt`, `updatedAt`, `title` ("") ,
`model`, `model_name`, `serverProfile`, `kind`, `parentSession`, `seedLength`,
`depth`, and `permission_preset` - but **not** `agent_preset`, `endpoint`, or
`profile_id`. The route pair (`endpoint`/`profile_id`) is the durable
`55-A7` pair that rebuilt the exact route on resume; `agent_preset` is the
creation-frozen composition (`42 sec.4.1`/`session.hpp:61-64`). A forked child that
loses them gets a fresh default composition and possibly a different route.
`Session::fork` is the single implementation behind both the CLI and the daemon,
so this gap is shared.

**2.5 The focus-only rule.** `apply_resume_success` models the session
(`ensureSessionIn`), subscribes the connection (`track`), then calls
`model_.focusSession(session)` with the pinned comment "focus only; no
`session.activate`" (`supervisor.cpp:1651-1672`). The daemon's `active_session_`
is distinct from the supervisor's focus (`workspace_host.cpp:599,618`;
`host.hpp:45`).

**2.6 The live-vs-stored split is preserved.** `/sessions` (`open_sessions`,
`supervisor.cpp:1066-1075`) reads the stored catalog; Ctrl+S is the live-only
switcher (specs 22/57). A fork must appear in both: as a stored row
(kind='fork') and in the live subset once the daemon reports it.

## 3. Decisions

### 78-D1 - Add `/fork` as a built-in command with a `CommandContext::fork` hook

A new `Command` named `fork` is registered in `CommandRegistry::builtin()`
**before** the `/help` snapshot line (`src/ui/command_registry.cpp:285`), so it
appears in `/help` and completion. Its handler calls `context.fork(args)`. The
supervisor wires `context.fork` in `dispatch_command` beside `context.sessions`
(`src/ui/supervisor.cpp:2743`) to a new `SupervisorApp::fork_session(args)`,
which submits `session.fork` beside the resume call (`supervisor.cpp:1574`). No
new wire method, no new command file.

### 78-D2 - Seed boundary = the parent's current resolved-view length by default

`/fork` with no argument seeds at the parent's **current resolved-view length**
(the full conversation), matching `ymh fork` (`session_cli.cpp:356` uses
`store->read(header->id).size()`). `/fork <N>` seeds at the resolved-view index
`N`, `0 <= N <= view length`; out of range is the daemon's existing
`InvalidForkBoundary` (`session.cpp:769-771`). A **turn-aligned picker is not
offered here** - selecting a `TurnStarted` boundary is spec 79's `/rewind`
concern (see sec.13). To let the no-argument form work without the TUI reading the
parent's view length, the `session.fork` handler accepts an **absent**
`seed_length` as "full resolved view" (sec.4.3); the CLI keeps sending an explicit
integer.

### 78-D3 - Fix `Session::fork` inheritance (agent_preset, endpoint, profile_id)

`Session::fork` MUST copy the parent's `agent_preset`, `endpoint`, and
`profile_id` in addition to the fields it already copies. Rationale: these are
the session's creation-frozen route and composition, not per-turn state
(`session.hpp:61-68`; `55-A7`; `42 sec.4.1`). This makes the fork a true branch of
the same lineage rather than a fresh default-composed session. The existing
`permission_preset` copy (`session.cpp:788`) and the `depth` inheritance
(`:787`) are retained. This is a code change to one function; it amends `01
sec.9.3`'s header list.

### 78-D4 - Fork inherits depth; it does NOT increment it

The task brief phrases the child row as "depth+1". **This is rejected.** Spec 42
`sec.3.4` (`:728-730`) and `55 sec.3.4` (`:912-913`) already pin: "A spawned child is
`parent.depth + 1`. A forked session **inherits** the source's depth (a fork is
the same agent lineage, not a delegation)." The shipped `Session::fork` sets
`child.depth = parent.header().depth` (`session.cpp:787`), which matches. The
child's row therefore carries `depth == parent.depth`; only a spawn increments.
Adopting `depth+1` would contradict 42/55 and the shipped code.

### 78-D5 - Focus the child without `session.activate`

After the fork reply, the supervisor models the child (`ensureSessionIn`),
subscribes the connection (`track`), and calls `focusSession(child)` - the same
focus-only shape as `apply_resume_success` (`supervisor.cpp:1669-1672`). It does
**not** send `session.activate` (contrast `protocol.hpp:534`). The daemon's
`active_session_` is left unchanged by `forkSession` (`host_runtime.cpp:827-852`
vs `:937-948`).

### 78-D6 - The original keeps running

`/fork` sends no `agent.cancel`, no turn cancellation, and no
close/detach of the parent. An in-flight parent turn continues under the
existing `TurnExecutor` admission (`24` AL6/AL19); the fork does not alter turn
scheduling. The TUI remains subscribed to both sessions.

### 78-D7 - No new RPC, no schema change, one additive wire default

The feature reuses `session.fork`. The only wire change is that an absent
`seed_length` defaults to the full resolved view (sec.4.3). No new method, no new
table, no new column, no new registry field.

### 78-D8 - Claude-Code parity, stated honestly

CC `/fork` copies to a new **background** session; CC also has `/branch`
(switch into the copy in-process), `--fork-session` (new id in a separate
process), an optional first prompt, and an agent-view listing background
sessions. ymh's model differs: there is no per-session background process and no
in-process branch switch. In ymh, "background" means **a live session in the
same workspace daemon that is not currently focused**. `/fork` creates that live
session and focuses it. The optional-prompt and `/branch` behaviours are **not
mirrored** (recorded in sec.11 and sec.13, with anchors).

### 78-D9 - Refuse to fork a subagent

If the focused `SessionUiState` has `subagent == true` (`ui_model.hpp:349`;
`58-D7`), `/fork` refuses with a notice, mirroring `select_history`'s rejection
of a subagent resume (which reads the parallel `SessionNode.kind == "subagent"`,
`supervisor.cpp:2995-2998`; `58-E20`). The guard is evaluated on the focused
state (`UiModel::activeSession()`, `ui_model.hpp:691`) before the RPC. Forking a
child is done from its parent.

### 78-D10 - Failure surfaces a status-bar notice, never a phantom focus

Any transport/daemon error surfaces through the notice ring
(`push_notice`/`surface_notice`). The TUI focuses a child only when the reply
names a session id; a failed fork leaves the focus on the parent.

## 4. Interface sketches

### 4.1 `CommandContext::fork` (new field)

```cpp
// include/ymh/ui/command_registry.hpp, in CommandContext (after :42)
// 78-D1: branch the focused session into a new independent session in the
// same workspace daemon. `args` is the trimmed command-line tail: empty, or a
// non-negative integer resolved-view boundary.
std::function<void(const std::string&)> fork;
```

Concrete caller (the built-in handler, registered before
`command_registry.cpp:285`):

```cpp
registry.add(Command{
    "fork", "branch the current session into a new independent session",
    [](CommandContext& context, const std::string& args) {
        if (context.fork) {
            context.fork(args);
        }
    }});
```

### 4.2 `SupervisorApp::fork_session` (new method)

```cpp
// src/ui/supervisor.cpp, member of SupervisorApp; wired at dispatch_command
// beside `context.sessions` (:2743).
void fork_session(const std::string& args);
```

Body sketch (comment-level; no implementation here):

```cpp
void fork_session(const std::string& args) {
    // 78-D9: the focused session; no focused session -> notice.
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
        push_notice("no active session to fork");
        return;
    }
    // 78-D9: a subagent focus is never forkable (SessionUiState::subagent,
    // ui_model.hpp:349; parallel to select_history's SessionNode.kind check).
    const SessionUiState* focused = model_.activeSession();
    if (focused != nullptr && focused->subagent) {
        push_notice("subagent sessions are not forkable (/subagents)");
        return;
    }
    // 78-D2: empty args -> omit seed_length (daemon defaults to full view);
    //         an integer -> explicit boundary; anything else -> usage.
    std::optional<std::int64_t> seed;
    if (!args.empty()) {
        std::int64_t n = 0;
        const auto [ptr, ec] =
            std::from_chars(args.data(), args.data() + args.size(), n);
        if (ec != std::errc{} || ptr != args.data() + args.size() || n < 0) {
            push_notice("usage: /fork [<resolved-view-index>]");
            return;
        }
        seed = n;
    }
    const WorkspaceId id = workspace->id;
    const SessionId   parent = workspace->activeSessionId();
    nlohmann::json params{{"session", parent.value}};
    if (seed.has_value()) {
        params["seed_length"] = *seed;
    }
    submit_to(id, std::string(protocol::method::kSessionFork), std::move(params),
              [this, id, parent](SupervisorReply reply) {
                  enqueue([this, id, parent, reply = std::move(reply)]() mutable {
                      if (!reply.ok) {
                          // FK-F4..FK-F10: the daemon error text is surfaced.
                          surface_notice(id, parent, "fork failed: " + reply.error);
                          return;
                      }
                      const std::string child = reply.result.value("session", std::string{});
                      if (child.empty()) {
                          surface_notice(id, parent, "fork failed: empty reply");
                          return;
                      }
                      apply_fork_success(id, SessionId{child});
                  });
              });
}
```

Caller sites: the `context.fork` lambda in `dispatch_command`
(`supervisor.cpp:2743`), and the subagent refusal is evaluated against
`SessionUiState::subagent` (`ui_model.hpp:349`) on the focused state
(`model_.activeSession()`, `ui_model.hpp:691`) before the RPC (78-D9).

### 4.3 Frozen-interface amendment: `forkSession` (base virtual + daemon default)

This changes the FROZEN transport interface, so the delta is pinned here in full
(old -> new); an implementer cannot satisfy it by copying the current
`host.hpp`. It amends `05 sec.7.4` and `11 sec.4.2/sec.4.3` (see the header
`Supersedes:`/`Amends:` blocks).

**Base pure virtual** - `include/ymh/transport/host.hpp:109`:

```cpp
// OLD (frozen; 05 sec.7.4 / 11 sec.4.2 :467):
virtual SessionCreated forkSession(const SessionId& id, std::int64_t seed_length) = 0;

// NEW (78-D2): the seed is optional; nullopt means "the parent's current
// resolved-view length".
virtual SessionCreated forkSession(const SessionId& id,
                                   std::optional<std::int64_t> seed_length) = 0;
```

**Daemon override** - `include/ymh/host/host_runtime.hpp:166-167`
(`src/host/host_runtime.cpp:827-828`):

```cpp
// OLD:
protocol::SessionCreated forkSession(const SessionId& id,
                                     std::int64_t seed_length) override;

// NEW:
protocol::SessionCreated forkSession(const SessionId& id,
                                     std::optional<std::int64_t> seed_length) override;
```

**Complete implementer set.** `TransportHost` has exactly two implementers in
the tree (`include/ymh/host/host_runtime.hpp:74`, production;
`tests/support/fake_transport_host.hpp:19`, test double). The base change is a
frozen-interface change, so every implementer must be updated in the same
change set or the tree does not build (an unchanged `override` no longer
overrides any base virtual):

| Implementer | Method site | Old -> new |
|---|---|---|
| `HostRuntime` (production daemon) | `include/ymh/host/host_runtime.hpp:166-167` / `src/host/host_runtime.cpp:827-828` | `std::int64_t seed_length` -> `std::optional<std::int64_t> seed_length` (pinned above) |
| `FakeTransportHost` (test double) | `tests/support/fake_transport_host.hpp:150` | `std::int64_t seed_length` -> `std::optional<std::int64_t> seed_length` (pinned below) |

**Test-double override** - `tests/support/fake_transport_host.hpp:150` (body
`:151-167`):

```cpp
// OLD:
protocol::SessionCreated forkSession(const SessionId& id, std::int64_t seed_length) override {

// NEW (78-D2):
protocol::SessionCreated forkSession(const SessionId& id,
                                     std::optional<std::int64_t> seed_length) override {
```

The fake must resolve `nullopt` exactly as `HostRuntime` does - "the parent's
current resolved-view length" (sec.5): its seed-loop bound at `:159-161` becomes
`seed_length.value_or(static_cast<std::int64_t>(parent.size()))` (the fake holds
the parent's full event vector in `logs_`). With the old signature the `override`
at `:150` overrides nothing and every TU that includes the fake fails to compile:
`tests/unit/transport_server_test.cpp`, `tests/unit/transport_socket_test.cpp`,
and `tests/unit/errata76_startup_residue_test.cpp`. This mirrors the sibling
convention (specs 18 `CX-13` :1860, 19 :968-969, 25 :764-765/:2163), which
always enumerates `tests/support/fake_transport_host.hpp` alongside `HostRuntime`
on a `TransportHost` virtual change; the fake half is covered by FK-U9 (sec.12.1).

**Wire `seed_length` type/default** - `05 sec.7.4 :892` (the method table) and
`protocol.hpp:532` (`method::kSessionFork`). The exact new binding:

```text
session.fork   params: { session, seed_length? }   result: { session, header }
```

`seed_length` is an **optional** JSON integer (`std::optional<std::int64_t>` in
C++): absent (`nullopt`) => seed at the parent's current resolved-view length; a
present integer `N` => the explicit resolved-view index (`N < 0` rejected,
`host_runtime.cpp:830-833`; `N > view length` => `InvalidForkBoundary`,
`session.cpp:769-771`). The wire params are read as `nlohmann::json` in
`protocol_server.cpp:424-434`; there is no typed `ForkParams` DTO in
`protocol.hpp`, so this optionality lives in the handler, not a struct field.

Handler change: absent `seed_length` => pass `std::nullopt`; a present integer =>
pass it. This is an additive default: existing callers that always send an
integer are unaffected.

### 4.4 `SupervisorApp::apply_fork_success` (new method)

```cpp
// src/ui/supervisor.cpp. Mirrors apply_resume_success (:1641-1674): the child
// is already live in the daemon (lease + agent + junction), so no resume.
void apply_fork_success(const WorkspaceId& workspace, const SessionId& child);
```

Body sketch:

```cpp
void apply_fork_success(const WorkspaceId& workspace, const SessionId& child) {
    if (model_.workspaces.count(workspace) == 0) {
        surface_notice(workspace, child,
                       "forked session in a workspace that is no longer open");
        return;
    }
    SessionUiState& state = model_.ensureSessionIn(workspace, child);  // model the child
    // 78-D5: mirror apply_resume_success (:1651-1660). A child modeled into a
    // fresh process would otherwise show a blank status line until the next
    // `refresh_sessions` (which hydrates only daemon-reported `live` sessions).
    if (state.status.model.empty()) {
        state.status.model =
            display_model(options_.config, stored_session_model(workspace, child));
    }
    model_.ensureCellIn(workspace, child);
    if (const auto connection = connections_.find(workspace);
        connection != connections_.end()) {
        connection->second->track(child);          // subscribe
    }
    // 78-D5, 22 sec.5.2: focus only; never session.activate.
    model_.focusSession(child);
    sync_subagent_subscriptions();
    push_notice("forked -> " + child.value);       // observable confirmation
}
```

### 4.5 Fixed `Session::fork` header block

`src/session/session.cpp:775-789` gains three assignments so the child matches
`01 sec.9.3` as amended by 78-D3:

```cpp
child.model_name    = parent.header().model_name;      // already present
child.endpoint      = parent.header().endpoint;        // NEW (78-D3, 55-A7)
child.profile_id    = parent.header().profile_id;      // NEW (78-D3, 55-A7)
child.agent_preset  = parent.header().agent_preset;    // NEW (78-D3, 42 sec.4.1)
child.permission_preset = parent.header().permission_preset; // already present
child.depth         = parent.header().depth;           // retained (78-D4)
```

## 5. Seed boundary and resolution

- **Default**: the parent's resolved-view length at fork time. The daemon
  computes it as `Session::events().size()` for the parent held by the
  `SessionManager` (the same value `ymh fork` uses, `session_cli.cpp:356`).
- **Explicit**: `/fork <N>` sends `seed_length: N`; the daemon validates
  `0 <= N <= parentView.size()` or returns `InvalidForkBoundary` (S5,
  `session.cpp:769-771`).
- **Projection**: `resolve_after_locked` yields
  `read(parent)[0, seed) ++ read(child)`; `ownEvents()` returns only the child's
  suffix (`session.cpp:617-627`). Fork-of-fork composes because the parent's
  resolved view already includes its own prefix (`session_persistence.cpp:490-510`;
  `02 sec.4.5` :657; `01 sec.9.3`).
- **Compaction folds through**: a `Compacted`/shadowed range inside the seed
  prefix is inherited and folds in the child exactly as in the parent - the fork
  inherits the parent's compacted context generation for free, and inherited
  boundaries stay valid because `Sequence` values are preserved by the fork
  (`13 sec.4.4` :532-537; `13` I10/C5; `01 sec.6.2`; test
  `13-context-compaction.md:1584`).
- **No turn picker**: this spec does not enumerate `TurnStarted` events. The
  boundary is an event index; a turn-aligned picker belongs to `/rewind`
  (spec 79, not yet written), which builds on this spec's resolved-view-index
  seam (78-D2).

## 6. The forked session's row

The child row is `sessions(kind='fork', parent_session=parent.id,
seed_length=N)`, `depth = parent.depth`, `permission_preset =
parent.permission_preset`, plus the 78-D3 additions. The SQL CHECK
(`session_persistence.cpp:49-50`) and `validateHeader`
(`session.cpp:362-368`) enforce `kind='fork' => parent_session IS NOT NULL AND
seed_length IS NOT NULL`; `Session::fork` calls `validateHeader(child)` before
`store.create` (`session.cpp:789-791`), so a malformed child never persists.
`status` is not stored: liveness is the `session_leases` row plus the daemon's
agent registry.

## 7. Focus, the original, and the live-vs-stored split

- The child is focused via `focusSession` (78-D5); `activeSessionId` is
  supervisor-local and never written to the registry (`ui_model.hpp:386-391`).
- The parent keeps its lease, its agent, its stream, and its turn; `/fork` sends
  no cancel/close. The TUI subscribes to the child and remains subscribed to the
  parent.
- The `SessionCreated` broadcast (2.2) drives `refresh_sessions`, so the child
  appears in `/sessions` (stored catalog) and in Ctrl+S (live subset) without a
  bespoke refresh. The split (22 sec.3/sec.4; 57) is preserved.

## 8. Failure modes

Continue the repo's `F1-F12` convention with a spec-local `FK-F` prefix
(disjoint from `00 sec.54 F1-F12`).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| FK-F1 | No focused session / no active workspace | `/fork` prints `"no active session to fork"` | no RPC; focus unchanged |
| FK-F2 | Focused `SessionUiState::subagent == true` (`ui_model.hpp:349`) | notice `"subagent sessions are not forkable (/subagents)"` | no RPC (78-D9; `58-E20`) |
| FK-F3 | Malformed arg (non-integer) | notice `"usage: /fork [<resolved-view-index>]"` | no RPC |
| FK-F4 | `seed_length > parent resolved-view length` | daemon `AppCode::InvalidForkBoundary` (`-32008`) | notice `"fork boundary out of range"`; `Session::fork` throws before `store.create` (`session.cpp:769-771`), so no row is created |
| FK-F5 | Unknown/evicted parent | `AppCode::UnknownSession` (`-32006`) | notice; `refresh_sessions` drops the stale row |
| FK-F6 | Lease/store admission fails inside `SessionPersistence::create` (a competing writer is excluded by the sidecar `flock`, `session_persistence.cpp:768`; the row insert and `acquire_locked` share one transaction, `:881-885`, so the failure surfaces as a C++ `StoreError` mapped to `AppCode::StoreUnavailable` `-32015`) | notice `"session busy"` | `create` rolls back, so no row and no lease are created and nothing is focused. The later `acquireLeaseOrThrow` (`host_runtime.cpp:838`) re-acquires an already-held lease, so a `LeaseLost` `-32007` is unreachable via `/fork` |
| FK-F7 | Store write fails mid-fork (disk full, `AppCode::StoreUnavailable` `-32015`, thrown as the C++ `StoreError`, `session/errors.hpp:12`) **after** `store.create` | reply is an error; a row without its own `SessionStarted` may exist | recorded partial-fork: the row is resolvable (prefix + no own events), visible in `/sessions`; the user deletes it; the parent is untouched (state-lifetime, sec.10) |
| FK-F8 | Child route/composition not resolvable (`AppCode::EndpointNotRouted` `-32021`, or an agent error) | notice from `map_agent_error`/`map_registry_error` | child row may exist; recorded; no parent mutation |
| FK-F9 | Workspace evicted between submit and reply | notice `"forked session in a workspace that is no longer open"` | `apply_fork_success`'s workspace guard (mirrors `apply_resume_success` `:1646-1650`); the child is durable on disk and re-appears on the next scan/resume |
| FK-F10 | Transport failure to the daemon | notice `"fork failed: <error>"` | no phantom focus; the user retries when attached (mirror `SW-F1`) |

## 9. Invariants

Spec-local `FK` numbering. Every row is testable and cites the code that would
be violated.

| ID | Invariant |
|---|---|
| FK1 | `/fork` never mutates the parent: no event is appended to the parent, and the parent's header, lease, agent, stream, and turn are unchanged. Only a new `sessions` row and the child's own events are created (`session.cpp:766-800`; `parent` is `const`). |
| FK2 | The child satisfies the kind/parent/seed matrix: `kind='fork'`, `parent_session=parent.id`, `seed_length` in `[0, parent resolved-view length]` (`session_persistence.cpp:49-50`; `session.cpp:362-368`; `validateHeader` runs before `store.create`). |
| FK3 | The default seed boundary equals the parent's current resolved-view length; an explicit `N > view length` is rejected with `InvalidForkBoundary` and creates nothing (`session.cpp:768-771`). |
| FK4 | The child's resolved view is exactly the parent prefix `[0, seed_length)` followed by the child's own events; `ownEvents()` returns only the suffix (`01` I10; `session_persistence.cpp:486-514`; `session.cpp:617-627`). |
| FK5 | The child inherits the parent's fixed route and composition: `model`, `model_name`, `endpoint`, `profile_id`, `serverProfile`, `agent_preset`, `permission_preset`. It is a lineage branch, not a default-composed session (78-D3; `55-A7`; `42 sec.4.1`). |
| FK6 | Fork inherits `depth`; it does not increment it. `child.depth == parent.depth` (78-D4; `42 sec.3.4 :728-730`; `55 sec.3.4 :912-913`; `session.cpp:787`). |
| FK7 | A fork is a new session in the same workspace daemon with its own `session_leases` row, its own agent, and its own `workspace_sessions` junction row; the parent's lease and agent are untouched (`host_runtime.cpp:835-847`; `session_manager.cpp:98-116`). |
| FK8 | `/fork` performs no `session.activate`: the daemon's `active_session_` is unchanged (`host_runtime.cpp:827-852` vs `:937-948`). |
| FK9 | `/fork` focuses the child only through `focusSession` after `ensureSessionIn`; it subscribes the connection via `track` and writes no registry state (`supervisor.cpp:1661-1672`; `ui_model.hpp:386-391`). |
| FK10 | An in-flight parent turn is not cancelled by `/fork`; no `agent.cancel` / turn-cancel is sent, and the parent is neither closed nor detached (`24` D10; 78-D6). |
| FK11 | Fork creation is atomic with respect to the workspace single-writer: the daemon serializes the `session.fork` handler and the sidecar writer `flock(LOCK_EX|LOCK_NB)` (`session_persistence.cpp:768`, honored by `flock_held_by_me()` `:404,:552`) keeps it the sole writer of `<ws>/.ymh/sessions.db`; a competing supervisor's RPC serializes behind it (`host_runtime.cpp`; `session_persistence.cpp:768`; 16). |
| FK12 | `/fork` introduces no durable supervisor-local state: focus is `activeSessionId` (never persisted); the only durable addition is the child's `sessions` row and its events (plus the additive registry junction). |
| FK13 | The child appears consistently in both surfaces: the stored catalog (`/sessions`, kind='fork') and the live subset (Ctrl+S) after `refresh_sessions`; the 22/57 live-vs-stored split is preserved. |
| FK14 | Fork-of-fork is allowed and forms an acyclic chain; `resolve_after_locked`'s cycle detection is the backstop (`session_persistence.cpp:484-486`). |
| FK15 | A focused session with `SessionUiState::subagent == true` is never forked from `/fork` (78-D9; `ui_model.hpp:349`; `58-E20`; parity with `select_history` `:2995-2998`). |

## 10. State lifetime

Every new state introduced by `/fork`. "Daemon" is the workspace daemon
(supervisor-owned; spec 16). No new persistent file or table is added.

| State | Created | Destroyed / evicted | Owner | Survives daemon restart | Survives supervisor restart / session switch | Crash path |
|---|---|---|---|---|---|---|
| Child `sessions` row (`kind='fork'`) | `Session::fork` -> `store.create` (`session.cpp:791`) | `session.delete` / `erase` (refused while fork children exist, `02 sec.4.7`) | Workspace daemon's `SessionPersistence` | yes (durable) | yes | WAL keeps the committed row; prefix still resolves from the parent via `resolve_after_locked` |
| Child own `SessionStarted` event | `Session::fork` `childSession.append` (`session.cpp:794-799`) | deleted with the row | daemon store | yes | yes | if the append fails after `create`, the partial-fork case FK-F7 |
| Child `session_leases` row | `store.create` -> `SessionPersistence::create`'s `acquire_locked` in the same transaction (`session_persistence.cpp:881-885`; called from `session.cpp:791`), renewed by `acquireLeaseOrThrow(child)` (`host_runtime.cpp:838`) | `session.close` / daemon exit; TTL expiry if the daemon dies uncleanly | daemon | no (re-acquired on resume) | yes (daemon keeps it) | stale lease expires (`lease_ttl`); a re-attach resumes the child |
| Child agent (`AgentRegistry` entry) | `agents().resume(child)` (`host_runtime.cpp:839`) | `close` / `dispose` (`24` D1/AL4) | daemon | no | yes while the daemon lives | recreated by `session.resume` on demand |
| Child `workspace_sessions` junction row | `registry_.addSession(workspace, child)` (`host_runtime.cpp:844`) | `session.delete` | shared `registry.db` | yes | yes | durable; stale rows are pruning targets (23) |
| Supervisor `SessionUiState` for the child | `apply_fork_success` -> `ensureSessionIn` | `eraseSession` / eviction / process exit | supervisor process (in-memory) | n/a | no; re-hydrated from disk on resume/replay | rebuilt from `sessions.db`; no loss of durable state |
| Supervisor subscription to the child | `connection->track(child)` in `apply_fork_success` | `untrack` on close / reconnect clears subscriptions | `SupervisorConnection` | n/a | no | re-subscribed on the next `attach`/`track` |
| Focus (`activeSessionId`) | `focusSession(child)` (78-D5) | workspace promotion / `eraseSession` / process exit | supervisor process | n/a | no (never persisted) | no durable effect; the daemon's `active_session_` is independent |
| `CommandContext::fork` callback | `dispatch_command` (`supervisor.cpp:2666`) | end of the dispatch call | supervisor (stack) | n/a | n/a | none (transient) |

## 11. dsh mapping

The DeepSeek Harness separates durable session state from ephemeral client
presentation. `/fork` stays inside that separation and adds only a control-plane
caller:

| dsh concept | ymh mapping here | justification |
|---|---|---|
| Durable session store | `<ws>/.ymh/sessions.db`; a fork adds one `sessions` row plus its own events | `02 sec.9` P10 (physical strategy at sec.4.5): a fork stores **no copied event rows**; the prefix is shared and resolved recursively (`resolve_after_locked`, `src/session/session_persistence.cpp:477-517`) |
| Session prefix / COW | `seed_length` + recursive `resolve_after_locked` | same mechanism the CLI fork uses (`session_cli.cpp:356`); no new substrate |
| Open set / membership | shared `registry.db` `workspace_sessions` junction | `registry_.addSession` (`host_runtime.cpp:844`; `registry.cpp:1210`); additive only |
| Client-local focus | `UiModel::activeSessionId` | never written to the registry (`ui_model.hpp:386-391`); fork focuses locally (78-D5) |
| Ownership / liveness | supervisor-owned daemon (spec 16); child is a new session in the same daemon | `HostRuntime::forkSession` holds the parent and creates an independent child; no ownership/coordinator change (`host_runtime.cpp:827-852`) |
| Control plane | existing `session.fork` RPC; the TUI adds the caller | method exists (`protocol.hpp:532`); only the absent-`seed_length` default is added (78-D7) |
| Event stream is truth | child own-log + parent prefix; the TUI is one consumer | `onSessionCreated` broadcasts; the TUI subscribes and replays (`protocol_server.cpp:842-851`) |
| Agent lifetime | child gets its own agent; the parent's is untouched | `agents().resume(child)` (`host_runtime.cpp:839`); `24` reference-counted agent ownership |
| **Non-mirror**: CC `/fork` optional first prompt | **NOT mirrored** | scope decision: ymh `/fork` is a pure branch + focus; sending a prompt is the user's next keystroke. Reason anchored in this spec 78-D2/78-D8; no CC capability is claimed |
| **Non-mirror**: CC `/branch` in-process switch and `--fork-session` separate process | **NOT mirrored** | ymh has no in-process session switch and no per-session process: the TUI fork keeps the child live in the same daemon, and the `ymh fork` CLI creates the row and releases its lease (`session_cli.cpp:359`). Reason anchored in 78-D8 |
| **Non-mirror**: CC agent-view "background sessions" | **NOT mirrored as a new surface** | ymh's live analogue is already Ctrl+S (spec 22/57), which lists live sessions including the new fork. Reason anchored in `22 sec.3/sec.4` + `57 sec.3/sec.4` (the live-vs-stored split) and `78-D8` (ymh's "background" is a live-but-unfocused session in the same daemon), not a new surface |

## 12. Test plan

Strategy is `00 sec.44`: unit, integration (Fake LLM / fake daemon), persistence,
golden render, and an opt-in live PTY layer (`YMH_LIVE_LLM=1`). ID scheme:
`FK-U*` unit, `FK-I*` integration, `FK-G*` golden render, `FK-P*` PTY/live.
This spec does not edit tests; the implementation phase does.

### 12.1 Unit

| ID | Test (`file`) | Assertion |
|---|---|---|
| FK-U1 | `Session.FK_U1_ForkInheritsRouteAndPreset` (`tests/unit/session_test.cpp`) | `Session::fork` copies `agent_preset`, `endpoint`, `profile_id`, `permission_preset`, `model_name`; `depth` equals the parent's (FK5/FK6) |
| FK-U2 | `Session.FK_U2_ForkBoundaryOutOfRangeCreatesNothing` (`tests/unit/session_test.cpp`) | `seedLength > view` throws `InvalidForkBoundary`; no child row (FK3) |
| FK-U3 | `Persistence.FK_U3_ForkViewIsParentPrefixPlusOwn` (`tests/unit/persistence_test.cpp`) | `read(child) == read(parent)[0,seed) ++ own`; `ownEvents` strips the prefix (FK4) |
| FK-U4 | `CommandRegistry.FK_U4_ForkRegisteredBeforeHelp` (`tests/unit/command_registry_test.cpp`) | `/help` lists `fork`; dispatch reaches the `context.fork` hook (78-D1) |
| FK-U5 | `CommandRegistry.FK_U5_ForkArgParsing` (`tests/unit/command_registry_test.cpp`) | empty arg -> hook called with `""`; `"12"` -> `"12"`; `"abc"` -> usage notice, no call (FK-F3) |
| FK-U6 | `HostRuntime.FK_U6_ForkDefaultsToFullView` (`tests/unit/host_runtime_test.cpp`) | `forkSession(id, nullopt)` seeds at the parent resolved-view length (78-D2/78-D7) |
| FK-U7 | `HostRuntime.FK_U7_ForkLeavesActiveSessionUnchanged` (`tests/unit/host_runtime_test.cpp`) | after a fork, `hostState().active_session` is unchanged (FK8) |
| FK-U8 | `HostRuntime.FK_U8_ForkChildHasOwnLeaseAgentJunction` (`tests/unit/host_runtime_test.cpp`) | the child has a lease row, an agent, and a registry junction; the parent's are intact (FK7) |
| FK-U9 | `TransportHost.FK_U9_FakeForkOverrideOptionalSeed` (`tests/unit/transport_server_test.cpp`) | `FakeTransportHost::forkSession` (`tests/support/fake_transport_host.hpp:150`) overrides the new `std::optional<std::int64_t>` base virtual and resolves `nullopt` to the parent resolved-view length (sec.4.3); the three TUs that include the fake compile (NEW-5) |

### 12.2 Integration (fake daemon / Fake LLM)

Extend `tests/integration_host_harness_test.cpp` (in-process host via
`tests/support/host_harness.hpp`) and `tests/unit/ui_driver_test.cpp` (FakeLLM
driver). A new `tests/unit/errata78_ui_test.cpp` may hold the TUI-level cases.

| ID | Test | Assertion |
|---|---|---|
| FK-I1 | `/fork` on a live fake daemon creates a `kind='fork'` row and focuses the child; the parent row is byte-identical (FK1/FK2/FK9) |
| FK-I2 | `/fork` on a `subagent` focus is refused with a notice; no RPC (FK15) |
| FK-I3 | Daemon unreachable -> notice, focus stays on the parent, no phantom cell (FK-F10) |
| FK-I4 | `/fork` with an in-flight parent turn (FakeLLM latch) does not cancel it; the parent turn completes (FK10) |
| FK-I5 | The child appears in `/sessions` and in the Ctrl+S live list after `refresh_sessions` (FK13) |
| FK-I6 | A `StoreError`/lease conflict surfaces a notice and focuses nothing (FK-F6/FK-F9) |
| FK-I7 | Fork-of-fork produces a composable resolved view (FK14) |
| FK-I8 | Recorded partial-fork: an injected append failure after `create` leaves a resolvable row visible in `/sessions`, parent untouched (FK-F7) |

### 12.3 Golden render

| ID | Test (`file`) | Assertion |
|---|---|---|
| FK-G1 | `RenderGolden.FK_G1_HelpListsFork` (`tests/unit/ui_render_golden_test.cpp`) | the `/help` golden gains the `/fork` row; no other golden changes (78-D1) |
| FK-G2 | `RenderGolden.FK_G2_ForkNotice` (`tests/unit/render_golden_test.cpp`) | the "forked -> <id>" status notice renders in the status bar |

### 12.4 PTY / live (opt-in)

| ID | Test (`file`) | Assertion |
|---|---|---|
| FK-P1 | `ui_supervisor_pty_test.cpp` (extend) | `/fork` in a PTY against the fake host focuses the child and the transcript switches (FK9) |
| FK-P2 | `tests/integration_live_e2e_test.cpp` (opt-in, `YMH_LIVE_LLM=1`) | `/fork` against real DeepSeek yields an independent child while the parent turn completes (FK1/FK10) |

### 12.5 Invariant and failure-mode coverage

| Invariant | Tests |
|---|---|
| FK1 | FK-I1, FK-P2 |
| FK2, FK3 | FK-U2, FK-U3, FK-U6 |
| FK4, FK14 | FK-U3, FK-I7 |
| FK5, FK6 | FK-U1 |
| FK7, FK8 | FK-U7, FK-U8 |
| FK9, FK13 | FK-I1, FK-I5, FK-P1 |
| FK10 | FK-I4, FK-P2 |
| FK11, FK12 | FK-I1, FK-I6 |
| FK15 | FK-I2 |

| Failure mode | Tests |
|---|---|
| FK-F1, FK-F2, FK-F3 | FK-U5, FK-I2 |
| FK-F4 | FK-U2 |
| FK-F5 | FK-I6 |
| FK-F6 | FK-I6 |
| FK-F7 | FK-I8 |
| FK-F8 | FK-I6 |
| FK-F9, FK-F10 | FK-I3, FK-I6 |

## 13. Out of scope, parity notes, and recorded risks

- **`/rewind` (spec 79) is not designed here.** It will build on `/fork`: a
  conversation rewind is a fork at a chosen `TurnStarted` boundary plus focus,
  reusing this spec's resolved-view-index seam (78-D2). This spec deliberately
  offers only an event-index boundary, not a turn picker.
- **No file/code checkpoints.** `session_snapshots` is a derived cache with no
  authority over `events` (`02 sec.6` :298,:327), not a restore point; this spec
  does not wire it.
- **CC parity gaps (not mirrored).** The optional first prompt, the in-process
  `/branch` switch, `--fork-session` separate-process semantics, and a dedicated
  agent-view background list are not provided. ymh's "background" is a live,
  unfocused session in the same daemon; Ctrl+S already lists live sessions. See
  sec.11 non-mirror rows and 78-D8.
- **Recorded risk (FK-F7).** If the child's `SessionStarted` append fails after
  `store.create`, a partially-created fork row persists. It is resolvable
  (prefix + zero own events) and harmless to the parent; the user can delete it.
  A future hardening pass could make `create + first append` one transaction.
- **Recorded risk (subagent focus).** If a future change makes a `subagent`
  session focusable as the active session, 78-D9's guard is the required
  backstop; it is implemented in `fork_session` before the RPC.
- **No daemon-ownership change.** The child lives in the existing
  supervisor-owned daemon (spec 16); the last supervisor's exit still tears the
  daemon down, taking the child's live agent with it. The durable row survives.

## 14. Revision log

| Rev | Change |
|---|---|
| 1 | Initial draft: `/fork` command + `CommandContext::fork` hook; TUI `session.fork` caller beside resume; default seed = full resolved view with an additive daemon default; `Session::fork` inheritance fix (`agent_preset`/`endpoint`/`profile_id`); depth inherited (78-D4, rejecting the "depth+1" phrasing per 42 sec.3.4/55 sec.3.4 :912-913); focus-only child selection; failure modes, invariants, state-lifetime table, dsh mapping, and test plan. |
| 2 | Gate-78 fixes: added `Verification status:` + `DESIGN_STATUS.md` tracker-row instruction (M1); re-anchored the agent-view non-mirror cell off the out-of-tree brief to `22 sec.3/sec.4` + `57 sec.3/sec.4` + 78-D8, and re-anchored the `/rewind` and `session_snapshots` notes to in-tree decisions/`02 sec.6` (M2); fixed fork-of-fork persistence anchor to `session_persistence.cpp:490-510` + `02 sec.4.5` (L1); `-32015` is `StoreUnavailable`, not `StoreError` (L2); pinned the subagent refusal to `SessionUiState::subagent` (`ui_model.hpp:349`) via `UiModel::activeSession()` (L3); reworded FK-F6 to the shipped `create` transaction order and the unreachable `LeaseLost` re-acquire (L4); `usage` string now says `resolved-view-index`, not `event-count` (L5); FK11 cites the writer `flock` (`session_persistence.cpp:768`) not the liveness probe (L6); corrected citation offsets (`session.hpp:61-64`; `55 sec.3.4 :912-913`) (L7); added spec 13 to `Depends on` and a compaction-fold-through bullet in sec.5 (L8); `apply_fork_success` now hydrates `state.status.model` like `apply_resume_success` (L9); marked the 22/57 amendments explicitly non-superseding. |
| 3 | Oracle MEDIUM fix: declared the frozen-interface amendments in the header (`Supersedes: 05 sec.7.4 :892` + `11 sec.4.2 :467`; `Amends: 05 sec.7.4 :887-908` + `11 sec.4.3 :522`) and pinned the `TransportHost::forkSession` base-virtual change (`include/ymh/transport/host.hpp:109`) plus the `HostRuntime` override (old -> new) and the exact optional `seed_length` type/default in sec.4.3. Gate-78 LOWs: FK14 cycle anchor now `session_persistence.cpp:484-486` (was 481-483); `session/errors.hpp:12` (was ambiguous `errors.hpp:12`); `02 sec.9` P10 (was `02 sec.4.5`); the lease `Created` cell now attributes `store.create` (`session_persistence.cpp:881-885`, renewed by `acquireLeaseOrThrow`). Also corrected the `method::kSessionFork`/`kSessionActivate` anchors to `protocol.hpp:532`/`:534` (were 520/522). |
| 4 | Gate-78 Rev-3 re-review fixes: **NEW-5 (MEDIUM)** - sec.4.3 now enumerates the complete `TransportHost` implementer set (`HostRuntime` + the test double `FakeTransportHost`) and pins the `FakeTransportHost::forkSession` override old -> new (`tests/support/fake_transport_host.hpp:150`), with the `nullopt` -> parent-resolved-view-length resolution and the three affected TUs; added the matching test-plan work item FK-U9 (sec.12.1). **NEW-7 (LOW)** - re-anchored the `Amends:` cell for the command surface from `10 sec.8.2` (renderers) to `10 sec.9.2 :1064` + `command_registry.cpp:285`. **NEW-6 (LOW)** - strengthened the header's tracker instruction to require row 78 `Rev 4` and the added `05`/`11` Notes; the tracker row remains orchestrator-owned (no other file edited). |
