# 82 - Handoff Command Errata: structured session summary for continuing elsewhere

```
Status: verified (Rev 3)
Revision: 3
Verification status: adversarial reviewer (independent, Rev 3): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Component: 82 (errata) - adds the `/handoff` durable command. `/handoff` distills
           the active session into a structured context summary, writes it to a
           durable Markdown doc under the workspace, and (by default) seeds a new
           root session from that summary. The seed is **stored-only** at
           creation: it is a durable `kind='root'` row in the workspace session
           store, is not added to the daemon's live open set, emits no
           `SessionCreated`, and becomes live only when the user resumes it
           (sec 4.2). It is an additive feature: one new UI builtin, one new
           durable-command wire method, one new host-side service, one additive
           host notice kind, and no new event types.
Depends on: 00-architecture.md (verified) sec 9.2 / sec 18 (path safety) / sec 44
             / sec 54 (F1-F12);
             01-session.md (verified) sec 3 (SessionHeader kind/parent/seed
             matrix) / sec 4.3 (EventType extension) / sec 6.3 (deriveMessages);
             05-transport.md (verified) sec 6 / sec 7 (RPC + host notices);
             10-supervisor-tui.md (verified) sec 4.1 / sec 8.1 / sec 8.2
             (UiModel, overlay/command registry);
             11-m2-errata.md (verified) - the frozen method catalog this errata
             extends additively;
             13-context-compaction.md (verified) - the summarization-call
             discipline and the `ContextCompaction` projection precedent
              (src/agent/compactor.cpp:17-50, :285-430);
             18-context-errata.md (verified) - the `/context` sibling read-only
             command pattern;
             21-config-jsonc-errata.md (verified) - the config layers the new
             `[handoff]` keys join;
             22-switcher-sessions-errata.md (verified) and
             57-switcher-sessions-popup-errata.md (verified) - the live-vs-stored
             split and focus-only selection this errata preserves;
             24-agent-lifetime-errata.md (verified) - `session.resume` and agent
             handle ownership;
             37-message-provenance-errata.md (verified) / 38-transcript-provenance
             errata (verified) - `MessageSource` / `ContextInjected` semantics the
             seed uses;
             44-goals-jobs-commands.md (verified) sec 4.1 / sec 4.2 / sec 5.6 /
             sec 6.1 / sec 13 OQ-2 - the durable `command/run`+`command/done`
             surface and the wire-method question this errata resolves.
             `include/ymh/commands/command_registry.hpp`,
             `include/ymh/commands/command.hpp` and
             `include/ymh/session/events.hpp:326-341` ship that surface; the
             host-side registry is currently **uninstantiated** (no
             `add(CommandSpec,...)` caller exists in `src/`), so this errata
             supplies its first concrete owner and caller.
Scope:      the `/handoff [--no-seed] [--out <path>] [--title <text>]` command on
            the live `ui::run_supervisor` path: (A) the command grammar and its
            forwarding to the daemon as a durable command (82-D1, 82-D2);
            (B) the six-section summary and the event fields that feed each
            section, plus the empty-session no-op (82-D3, 82-D4); (C) the durable
            Markdown doc at `<ws>/.ymh/handoffs/<session-id>-<stamp>.md`
            (82-D5); (D) the default seed session - a new root session whose
            first durable event is a `context/injected` carrying the summary
            (82-D6, 82-D7); (E) the additive `command.invoke` wire method and the
            additive `HostNoticeKind::HandoffResult` completion notice (82-D8,
            82-D9); (F) the `[handoff]` config keys (82-D10). The command does
            NOT mutate the source conversation, does NOT clear it, and does NOT
            change its focus.
Supersedes: (a) spec 44 sec 4.2 / sec 13 OQ-2's open wire-method question: the
            exact name and payload are pinned here as the additive `command.invoke`
            (`{session, line, source?}`), which the host resolves in the
            `ymh::CommandRegistry` (`include/ymh/commands/command_registry.hpp:40`).
            This is an additive resolution of an explicit open question, not a
            reversal of any 44 decision.
            (b) The implicit "the host-side `ymh::CommandRegistry` has no
            construction site" state: this errata pins `WorkspaceRuntime` as its
            owner (82-D11) and `HostRuntime::invokeCommand` as its first caller.
            Nothing in 44 is superseded: `command/run`/`command/done` payloads,
            pairing, and log-only semantics are unchanged (44-I12/44-I13).
            No clause of 13, 18, 22, or 57 is superseded. `/export`
            (`include/ymh/ui/session_export.hpp`) and `/clear`
            (`src/ui/command_registry.cpp:157-168`) are unchanged.
            (c) The revision-1 live-seed claim is withdrawn in this revision:
            the seed is stored-only and becomes live on resume (sec 4.2, M6).
            This is a correction inside this draft, not a supersession of any
            other spec, so 22/57's live-vs-stored split is preserved unchanged.
Amends:     05 sec 7 / 11-m2-errata's frozen method catalog (additively):
            `protocol::method::kCommandInvoke = "command.invoke"` is added to the
            `method` namespace (`include/ymh/transport/protocol.hpp:506-549`) AND
            to the routing array `kMethodCatalog` that `all_methods()` /
            `is_known_method()` iterate (`src/transport/protocol.cpp:637-653`,
            size `38 -> 39`). Without the array entry `ProtocolServer` answers
            `MethodNotFound` at `src/transport/protocol_server.cpp:242`, and
            without a `handle_method` branch the known name falls through to
            `MethodNotFound: "unhandled method"` (`:561-564`); the branch is
            pinned in sec 5.2. It is allowed in both profiles (not in the
            Automation deny list, `src/transport/protocol.cpp:668-675`). The
            host-notice wire enum
            table `kHostNoticeKinds` (`src/transport/protocol.cpp:53-59`, size
            `5 -> 6`) gains `HostNoticeKind::HandoffResult` (sec 5.2, M1/M2).
```

## 1. Purpose, scope, and precedence

This errata adds a **user-invoked handoff**: a single command that turns the
current session into a portable, structured context summary. The summary is
useful in two ways, and `/handoff` produces both by default:

1. a **durable document** a human can read, edit, commit, or paste into a
   different tool (a new ymh session, a PR description, a colleague), and
2. a **seeded session** in the same workspace that starts from the summary, so
   the model can continue with a fresh, small context.

The feature is an **extension**: Claude Code ships no native `/handoff` (the
official commands and CLI reference contain no such command; only community
plugins and an open `/handover` request exist - see the recon artifact
`/tmp/opencode/recon-claude-code.md` sec 4.2, and note it is a scratch input, not
a repo file). dsh is a headless harness with no interactive command surface
(`00-architecture.md:69`). Therefore `/handoff` is modeled on ymh's own durable
command surface (spec 44) and its own compaction-summary precedent (spec 13); it
claims **no Claude Code parity** anywhere.

**Precedence.** Where this errata and a component spec disagree about `/handoff`
itself, this errata wins for the new surface. Spec 13 (compaction), spec 18
(`/context`), spec 22/57 (switcher/focus), and spec 44 (durable commands) remain
authoritative for their own contracts; this errata only adds to them.

### 1.1 Non-goals (explicit)

- **No rewind, no truncation, no branch.** `/handoff` never removes or rewinds
  events; the source log is appended to, never rewritten.
- **No `/clear` coupling.** The source session's presented conversation is
  untouched; the user runs `/clear` separately if they want a blank view.
- **No full-carry fork.** The seed does not inherit the source's event log; it
  starts from the distilled summary. The general `/fork` model is spec 78
  (`docs/design/78-fork-command-errata.md`; authored as a draft, not verified and
  not implemented), a separate, richer primitive; see sec 4.4 for the
  composition rule. This spec does **not** depend on spec 78: the seed is a
  stored root session created by shipped primitives (`SessionManager::
  createSession`), never a `Session::fork`/`session.fork` copy, so an
  unimplemented `/fork` UI cannot affect it.
- **No code/file checkpoints.** Only the conversation is summarized.

## 2. The `/handoff` contract

### 2.1 Grammar (pinned)

```
/handoff [--no-seed] [--out <path>] [--title <text>]
```

- **`--no-seed`**: write the doc only; do not create a seed session.
- **`--out <path>`**: override the doc path. The path is workspace-root-relative
  and resolved through `ExecutionEnvironment::resolve()` exactly like `/export`
  (`include/ymh/ui/session_export.hpp:38-45`; `00` sec 18 X1-X4). An escape
  (`PathEscape`) is a command error; nothing is written.
- **`--title <text>`**: override the seed session's title. The text is capped by
  `kMaxSessionTitleBytes` (`include/ymh/session/session.hpp:91`), truncated
  deterministically; it never affects the source session.
- Any other token is an error (`handoff: unknown option <token>`), and no work
  is done. There are no aliases (the `ui::CommandRegistry` table tolerates
  aliases, `include/ymh/ui/command_registry.hpp:56-61`, but none is added).

### 2.2 Invocation flow (pinned)

The command's effect is **durable and host-side**: the TUI entry point is a thin
forwarder, not a self-contained presentation builtin. Per spec 44 sec 4.2, the UI
forwards the raw line to the host, the host appends `command/run`, runs the
handler, and appends `command/done`.

1. The TUI parses the line in `ui::CommandRegistry::dispatch`
   (`src/ui/command_registry.cpp:125-145`) and calls the new `/handoff` builtin,
   which is registered **before** the `/help` snapshot at
   `src/ui/command_registry.cpp:285` (anything registered later is usable but
   invisible in `/help`).
2. The builtin invokes `CommandContext::handoff(args)`
   (`include/ymh/ui/command_registry.hpp:16-50`, new field), wired in
   `SupervisorApp::dispatch_command` (`src/ui/supervisor.cpp:2665-2747`) to
   `SupervisorApp::request_handoff(args)`.
3. `request_handoff` submits the additive `command.invoke` RPC to the active
   workspace with `{session, line:"handoff <args>", source:"user"}`, mirroring
   `context.compact`'s `submit_to(...)` (`src/ui/supervisor.cpp:2672-2680`). The
   reply is `{outcome:"Queued"}`; the TUI appends **nothing** on submission
   (exactly `context.compact`, which passes `nullptr` as the reply and appends
   no line) and is **never** blocked by the LLM call. The only user-visible
   feedback is the later `HandoffResult` notice (step 6).
4. The daemon's `HostRuntime::invokeCommand` resolves the owning `Agent` handle
   (mirroring `HostRuntime::agentPrompt`, `src/host/host_runtime.cpp:1005-1010`),
   submits the work to the per-session turn executor `turns_.submit(...)`
   (mirroring `HostRuntime::compactSession`, `src/host/host_runtime.cpp:965-979`),
   and replies immediately.
5. On the turn executor, `ymh::CommandRegistry::invoke`
   (`include/ymh/commands/command_registry.hpp:40`) appends `command/run`,
   runs the `handoff` `CommandSpec` handler (which calls
   `HandoffService::run`), and appends `command/done
   {command_id, kind, text}`. The handler is single-threaded
   (44-I17) and runs to settlement before the next turn task.
6. After settlement the daemon emits an additive
   `HostNotice{kind=HandoffResult, workspace, session=<source>, detail=<text>}`
   through the new `ProtocolServer::onHandoffResult(session, detail)` seam
   (mirroring `onMcpServerStatus`, `src/transport/protocol_server.cpp:926-938`).
   Because the handler settles on the turn executor while `ProtocolServer`
   requires every notification to run on its owning io thread
   (`include/ymh/transport/protocol_server.hpp:12-14`), `HostRuntime` marshals
   the notice through the daemon's `TransportServer::post` seam
   (`include/ymh/transport/transport_server.hpp:91`; precedent
   `WorkspaceHost::Impl::forwardEvent`, `src/host/workspace_host.cpp:1120-1128`),
   so `onHandoffResult` is never called on the turn-executor thread (sec 5.2,
   sec 6). The supervisor's `UiEventAdapter::onHostNotice`
   (`src/ui/ui_event_adapter.cpp:394-431`, new `switch` case for the enum, else
   `-Wswitch` fails the build) appends `detail` as a System notice on the source
   session when it is modeled, else to the notice ring (`CommandContext`'s
   `append_system_entry` path, `include/ymh/ui/command_registry.hpp:52-54`).

### 2.3 Session mutation (pinned)

`/handoff` **mutates no derived conversation state** of the source session: it
does not append a user/assistant message, does not compact, does not refine the
title, and does not change the header. The only source-session writes are the
two log-only command events, which `deriveMessages` ignores
(`src/session/session.cpp:419-420`; 44-I12) and which token accounting excludes.
It does **not** clear the current view (`/clear` remains the only view-clear,
`src/ui/command_registry.cpp:157-168`) and does **not** change focus. The seed is
**stored-only**: it is not added to the daemon's live open set and emits no
`SessionCreated`, so the Ctrl-S live switcher does not list it; the user opens it
from `/sessions` (stored list read from disk, 22/57), which resumes it and only
then makes it live. Focus stays on the source session, exactly as `focusSession`
keeps focus-only with no `session.activate`
(`src/ui/supervisor.cpp:1669-1672`).

## 3. Summary content (pinned)

The summary is a single Markdown block whose sections are fixed and ordered.
Every section is present; an empty section renders `(none)`. The summarizer model
emits exactly this structure (the prompt pins it; the parser asserts it).

| # | Section | What it holds |
|---|---|---|
| 1 | `## Goal` | the user's original and evolving intent |
| 2 | `## Key Decisions` | decisions made and their rationale, including corrections |
| 3 | `## Current State` | precisely what is done and where the work stands |
| 4 | `## Next Steps` | the single next action, then any queued follow-ups |
| 5 | `## Open Questions` | unresolved choices, blockers, and assumptions |
| 6 | `## Relevant Files` | exact workspace-relative paths that matter |

### 3.1 Evidence mapping (deterministic; which fields feed the summary)

`HandoffService::run` builds a **bounded evidence digest** from the source
session's durable log, then sends that digest plus the transcript to the
summarizer. The mapping from event fields to digest fields is fixed:

| Digest field | Source events / fields |
|---|---|
| Session identity | `SessionHeader{id,title,model,kind,parentSession}` (`include/ymh/session/session.hpp:44-71`) |
| Turn count / last turn | `turn/start`/`turn/end`/`turn/cancel`/`turn/fail` (`include/ymh/session/events.hpp:52-84`); an unmatched `TurnStarted` is an interrupted turn |
| Goal evidence | text extracted from the `payload::UserMessage.content` blocks (`events.hpp:98-102`), first and most recent; `payload::GoalChange` when present (`events.hpp:314-319`) |
| Decision evidence | `payload::AssistantMessage` text and folded reasoning (`events.hpp:116-124`), plus later user corrections |
| File evidence | `payload::ToolCall` arguments carrying path-like keys (`read`/`write`/`edit`/`grep`/`glob`/`git`) (`events.hpp:136-143`), canonicalized workspace-relative; deduplicated, capped at `kMaxHandoffFiles` |
| Error evidence | `payload::ToolResult{outcome != Ok}` (`events.hpp:145-158`) and user corrections |
| Prior summary | the latest `payload::ContextCompaction.summary` (`events.hpp:184-198`) when one exists |
| Current state | the last completed turn plus any interrupted turn |

The digest is a **deterministic** projection: same log in, same digest out, so a
FakeLLM hermetic test can assert it byte-for-byte (sec 12).

### 3.2 Bounding (pinned)

The transcript sent to the summarizer is bounded by `HandoffPolicy`
(sec 6). The base is the latest committed `ContextCompaction` summary plus the
events after its boundary (`src/session/session.cpp:533-547`); with no
compaction, the base is the full resolved view (`deriveMessages`,
`src/session/session.cpp:397`). If the serialized evidence still exceeds
`max_input_bytes`, oldest turns are dropped deterministically (never a partial
turn, never the most recent `keep_recent_turns`) until it fits. If it cannot fit
even at the floor, the command fails with `ContextTooLarge` (sec 8).

### 3.3 Empty session (pinned no-op)

If the source session has **no** user or assistant message events (a
freshly created, never-prompted session), `/handoff` performs **no LLM call, no
file write, and no seed**. It appends the usual `command/run`+`command/done`
pair with `kind=Error` and `text="handoff: nothing to summarize (empty session)"`,
and emits a `HandoffResult` notice carrying the same text. Rationale: paying for
an empty summary and creating an empty seed session is pure waste, and an empty
doc would be misleading. This is the pinned no-op.

## 4. Output artifacts

### 4.1 The doc (pinned)

- **Default path**: `<ws>/.ymh/handoffs/<session-id>-<YYYYMMDD-HHMMSS>.md` in UTC.
  The directory is created (mkdir -p semantics) under the resolved workspace root
  if absent. The filename convention mirrors `/export`'s
  `<stem>-<stamp>.md` (`include/ymh/ui/session_export.hpp:34-36`), but uses the
  **session id** (stable, collision-free across titles) rather than a title stem.
- **Collision handling**: if the default path already exists (two handoffs in the
  same second), append `-2`, `-3`, ... deterministically before the extension.
  `/export` has no such rule; handoff's default is machine-generated and must not
  silently overwrite.
- **Content**: YAML-free Markdown with a one-line provenance header
  (`<!-- handoff of session <id> (<title>) at <UTC> -->`), the six summary
  sections, and a `## Source` footer with the source session id, turn count, and
  the workspace-relative paths already listed in section 6. UTC timestamps only.
- **Resolution**: `resolve_handoff_path()` uses
  `ExecutionEnvironment::resolve()` with the workspace root (never `getcwd()`;
  `00` sec 18). `LocalEnvironment{root}` is the fallback, as in `/export`
  (`src/ui/supervisor.cpp:2302-2330`).
- **Write durability**: the file is written with a temp-file + `rename` in the
  same directory, then the seed is created. The write happens **before** the
  seed (82-D7), so an existing seed always has its backing doc.

### 4.2 The seed session (pinned, default)

By default `/handoff` creates a new session and appends the summary to it as its
first conversation event.

- **Kind/parent/seed**: `kind='root'`, `parent_session=NULL`, `seed_length=NULL`.
  This satisfies the shipped SQL CHECK
  (`src/session/session_persistence.cpp:35-51`, the `CHECK ((kind='root' ...))`
  row) with **no schema change**. It is deliberately **not** a fork: a fork
  copies the parent's events (`Session::fork`, `src/session/session.cpp:766-801`)
  and would carry the very context the handoff exists to shed.
- **Creation**: `SessionManager::createSession(const SessionOptions&)`
  (`include/ymh/session/session_manager.hpp:29-50`, `:59`), same workspace/cwd.
  Inherited from the source: `model`, `model_name`, `endpoint`, `profile_id`,
  `serverProfile`, `agent_preset`, `permission_preset`; `depth` stays `0`
  (root). Title is `handoff: <title>` (or `--title`), capped by
  `kMaxSessionTitleBytes`. This is the **stored-only** path: it does NOT go
  through `AgentRegistry::create` + `acquireLeaseOrThrow` +
  `registry_.addSession`, so no live `AgentLoop` is constructed and no registry
  open-set row is created. It **is** created through
  `SessionManager::createSession`, whose durable store
  `SessionPersistence::create` acquires the per-session lease
  (`src/session/session_persistence.cpp:883-885`; `acquire_locked` inserts the
  `session_leases` row, `:573-590`), held by the daemon until exit/expiry; the
  same process re-acquires cleanly on resume.
  `HostRuntime::listSessions`/`session.list` enumerates the **whole store**
  (`src/host/host_runtime.cpp:528-535,547`), so the seed **is** returned, with
  `live=false` and `registered=false` (store-only ordering,
  `src/registry/registry.cpp:609-644`); only the Ctrl-S **live** switcher, which
  filters `live==true`, omits it (M6, N2).
- **Seed event**: immediately after `session/start`, append a
  `context/injected` payload
  `ContextInjected{id, role=Role::User, text=<summary>, source, context}`
  (`include/ymh/session/events.hpp:176-181`). `deriveMessages` materializes it
  into the projection (`src/session/session.cpp:525-532`), so the model sees the
  handoff as session context. Provenance is pinned:
  - `source.kind = MessageSource::Kind::Plugin`
    (`include/ymh/agent/provenance.hpp:150-167`) - the content is
    harness-produced, not user-authored;
  - `source.context.form = ContextForm::Recall` (`provenance.hpp:40-48`) - a
    recall of prior work;
  - `source.plugin = "ymh.handoff"` (non-empty as 36-I8 requires).
- **Surfacing (stored-only)**: because creation bypasses the live path, the
  daemon emits **no** `SessionCreated` notice and the Ctrl-S live switcher does
  not list the seed. It is surfaced by `/sessions`, which lists stored sessions
  read directly from disk for every registered workspace, live or not
  (22/57; `src/ui/supervisor.cpp:1574`). Selecting it submits `session.resume`,
  which attaches the daemon and registers it in the open set (the durable lease
  is already held from create, sec 4.2), and only then emits `SessionCreated`.
  Until that resume the seed is durable but not live, and is listed by
  `/sessions` with `live=false` (M6). Focus is **not** changed (sec 2.3).
- **Live-on-resume**: the seed becomes an ordinary live session the moment it
  is resumed; `/handoff` itself never resumes it.
- **`--no-seed`**: only the doc is written. Rationale for offering it: the doc is
  useful on its own (paste elsewhere, commit), and a user who wants to keep one
  session should not accumulate seed sessions.

### 4.3 What "continue elsewhere" means here

The doc is the durable, tool-independent artifact. The seed session is the
zero-friction in-ymh continuation. Both come from the same summary text, so the
doc and the seed can never disagree about content.

### 4.4 Composition with `/fork` (spec 78, authored draft; not a dependency)

Spec 78 (`docs/design/78-fork-command-errata.md`) is **authored as a draft, not
verified and not implemented** (`Status: draft - gate: not yet run` at its
header). This spec does not depend on it. At a high level the two are
complementary and must not be conflated:

- `/fork` (when it lands) carries the **full** parent event log into a child
  (`kind='fork'`, `parent_session`, `seed_length`); it is a fidelity primitive.
- `/handoff` seeds a **root** session from a **distilled** summary; it is a
  compression primitive, created stored-only via `SessionManager::createSession`
  (sec 4.2) and never via `Session::fork`/`session.fork`.

78's `/fork` reuses the shipped `session.fork` path that already exists end to
end; `/handoff` shares none of that plumbing, so this spec stays implementable
regardless of 78's status. Reconciliation note: 78 amends 01/22/57/10 by
reference for the fork surface; those amendments do not touch `/handoff`'s
summary, doc path, or stored-only root seed.

If spec 78 later introduces a `fork` UI, it must not claim the `context/injected`
seed shape and must not reuse the handoff doc path; the seed's root kind is
pinned here.

## 5. Durable command surface and non-blocking execution

### 5.1 Why the durable surface

The summarization call is long (an LLM round-trip). Spec 44's durable command
surface (`command/run`+`command/done`,
`include/ymh/session/events.hpp:326-341`; 44 sec 4.1) gives the invocation a
durable, replayable audit trail on the **source** session and, crucially, moves
the work off the TUI thread: the handler runs on the daemon's per-session turn
executor (`turns_.submit`, `src/host/host_runtime.cpp:972`), while the TUI only
submits an RPC and receives `Queued`.

### 5.2 The additive wire method (resolves 44 OQ-2)

Spec 44 left the durable-command wire path open (44 sec 13 OQ-2: "the exact name
and payload are open"). This errata pins it:

- `protocol::method::kCommandInvoke = "command.invoke"`, added inside the
  `namespace method { ... }` catalog (`include/ymh/transport/protocol.hpp:506-549`)
  AND to the routing array `kMethodCatalog` that `all_methods()` and
  `is_known_method()` iterate (`src/transport/protocol.cpp:637-653`), bumping its
  `std::array` size `38 -> 39`. A header constant alone is not enough:
  `ProtocolServer::dispatch_request` rejects any name not in `kMethodCatalog`
  with `MethodNotFound` (`src/transport/protocol_server.cpp:242-246`). The method
  is allowed in both profiles (it is not in the Automation deny list,
  `src/transport/protocol.cpp:668-675`). See the header `Amends:` field.
- Request params: `{session: <id>, line: <the raw command line, with or without
  the leading slash>, source?: "user"|"agent"}`. The host's
  `CommandRegistry::invoke` strips a leading slash and splits the name and the
  verbatim argument tail exactly as
  `src/commands/command_registry.cpp:87-102` already does.
- Response: `{command_id?}` is **not** required; the reply is a queue
  acknowledgement `{outcome:"Queued"}` (mirroring `session.compact`,
  `src/transport/protocol_server.cpp:443-445`). The TUI correlates the eventual
  result through the additive `HandoffResult` notice (sec 2.2 step 6), not
  through the reply.
- **Dispatch branch (the concrete caller).** `ProtocolServer::handle_method`
  (`src/transport/protocol_server.cpp:351-564`) must gain a branch beside the
  `session.compact` analogue (`:443-445`), before the fallback:
  `} else if (method_name == method::kCommandInvoke) { const SessionId session =
  session_param(request.params); const CommandSource source =
  parse_command_source(request.params.value("source", std::string{"user"}))
  .value_or(CommandSource::User); host_.invokeCommand(session,
  string_param(request.params, "line"), source); respond(conn, request.id,
  nlohmann::json{{"outcome", "Queued"}}); }`. This branch is the pinned caller of
  `TransportHost::invokeCommand` (sec 6); `parse_command_source` is the shipped
  helper (`include/ymh/commands/command_types.hpp:26-35`). A catalog name with
  **no** branch falls through to `MethodNotFound: "unhandled method"`
  (`:561-564`), so the header constant + catalog entry (M1) alone are
  insufficient.
- Errors before queueing (unknown session, unknown command name, inbox full) are
  normal JSON-RPC errors.
- The additive `HostNoticeKind::HandoffResult` must also be added to the
  `kHostNoticeKinds` wire table (`src/transport/protocol.cpp:53-59`, size
  `5 -> 6`) beside the enum value in `include/ymh/transport/protocol.hpp:240-246`.
  Without it `wire_name` returns `{}`, `to_json(HostNotice)` emits `"kind": ""`,
  and the supervisor's `from_json` throws
  `std::invalid_argument("unknown host notice kind")`
  (`src/transport/protocol.cpp:426-441`).

The method is generic (any registered durable command may be forwarded), but
this errata only registers `handoff`; the future `/goal` UI may adopt the same
path. Generic `command.invoke` is preferred over a one-off `session.handoff`
because spec 44 sec 4.2 already specifies "the UI forwards the raw line to the
host", and a per-command RPC would fork that contract.

### 5.3 The handler is a `CommandSpec`

`make_handoff_command(HandoffService&)` returns a `CommandSpec`
(`include/ymh/commands/command.hpp:38-44`) named `handoff`, registered into the
host-side `ymh::CommandRegistry`
(`include/ymh/commands/command_registry.hpp:28-50`) at the global scope. The
handler resolves the source session from the injected `SessionManager`
(`sessions_->sessionPtr(agent.session())`, mirroring
`src/commands/command_registry.cpp:115`) and its derived view
(`deriveMessages`, `src/session/session.cpp:397`), then calls
`HandoffService::run` and maps its result to a `CommandOutcome`
(`command.hpp:29-33`): success -> `kind=Success, text=<result line>`;
failure -> `kind=Error, text=<reason>`. This mirrors `make_goal_command`
(`include/ymh/goal/goal_command.hpp:27`), which previously had no registration
caller; this errata supplies the first concrete registry owner and caller
(82-D11). The concrete caller of `make_handoff_command` is the
`WorkspaceRuntime` construction (`src/agent/workspace_runtime.cpp`, sec 6);
the registry's `invoke` is reached from `HostRuntime::invokeCommand` (sec 6).

## 6. Interface sketches (C++)

```cpp
// include/ymh/agent/handoff.hpp (new; 82-D3..82-D10)
//
// 82-D1/D10: the parsed options and the bounded policy. The policy mirrors
// CompactionPolicy (include/ymh/agent/compactor.hpp:43-58) and is built from the
// layered config ([handoff], 82-D10).
struct HandoffOptions {
    bool        seed  = true;    // --no-seed clears
    std::string out;             // --out <root-relative path>; empty => default
    std::string title;           // --title; empty => "handoff: <source title>"
};

struct HandoffPolicy {
    bool        enabled = true;
    std::string summarizer_model;          // empty => runtime default route
    std::size_t max_summary_tokens = 2048;
    std::size_t max_summary_bytes  = 256u * 1024u;
    std::size_t max_input_bytes    = 256u * 1024u;
    std::size_t keep_recent_turns  = 2;
};

inline constexpr std::size_t kMaxHandoffFiles = 64;   // 82-D3 file cap

// 82-D5: default path and collision rule. Resolves under `env.root()` through
// ExecutionEnvironment::resolve(); throws ToolError{PathEscape} on escape,
// mirroring resolve_export_path (include/ymh/ui/session_export.hpp:38-45).
// Caller: HandoffService::run (and resolve_handoff_path's unit test HS-U6/U7/U8).
[[nodiscard]] std::filesystem::path resolve_handoff_path(
    const ExecutionEnvironment& env,
    const std::string&          requested,
    const SessionId&            session,
    std::time_t                 utc_now,
    const std::filesystem::path& default_dir);   // <ws>/.ymh/handoffs

// 82-D3: the six-section prompt (string; unit-testable, no LLM).
// Caller: HandoffService::run, which folds it into the summarizer request; HS-U1.
[[nodiscard]] std::string_view handoff_instruction() noexcept;

struct HandoffResult {
    enum class Outcome : std::uint8_t {
        Ok, Empty, Disabled, BadOption,
        NoRoute, ContextTooLarge, WriteFailed, StoreUnavailable,
        Cancelled,   // HF10: CancellationToken fired (cf. CompactionError::Code)
        Internal,
    };
    Outcome              outcome = Outcome::Ok;
    std::string          detail;                    // command/done text + notice
    std::string          doc_relative;              // workspace-relative (Ok)
    std::optional<SessionId> seed_session;          // present when seeded
};

// 82-D3/D6: the deterministic evidence digest (pure over the log).
// Caller: HandoffService::run; HS-U2 asserts it is byte-stable.
[[nodiscard]] std::string build_handoff_evidence(const SessionHeader&,
                                                 const EventRange&);

// One instance per WorkspaceRuntime, shared; thread-affine to the turn executor
// that calls it (44-I17). Holds injected seams; no global state.
// Construction caller: `WorkspaceRuntime` (src/agent/workspace_runtime.cpp, sec 6).
// run() caller: the `handoff` CommandSpec handler from make_handoff_command.
class HandoffService {
public:
    HandoffService(LlmRuntime&, LLMPool&, SessionManager&,
                   ExecutionEnvironment&, ModelCatalog*, HandoffPolicy,
                   WallClock = std::chrono::system_clock::now);

    // Long-running: builds the digest, calls the summarizer, writes the doc, and
    // (unless seed=false) creates the stored-only root seed session (sec 4.2).
    // Never throws for expected failures; returns them in HandoffResult::outcome.
    HandoffResult run(const Session& source,
                      const std::vector<Message>& view,
                      const HandoffOptions& options,
                      CancellationToken);
};
```

```cpp
// include/ymh/handoff/handoff_command.hpp (new; mirrors goal_command.hpp:27)
// Caller: WorkspaceRuntime construction (commands_->add(make_handoff_command(...))).
[[nodiscard]] CommandSpec make_handoff_command(HandoffService&);
```

```cpp
// include/ymh/transport/host.hpp (amended; beside compactSession :115)
// CommandSource lives in ymh/commands/command_types.hpp:18; host.hpp currently
// includes only core/event.hpp + transport/protocol.hpp (:20-21), so add
// #include "ymh/commands/command_types.hpp" (or a `namespace ymh { enum class
// CommandSource : std::uint8_t; }` forward declaration) so the signature sees it.
//
// M5: TransportHost is abstract; the only implementers are HostRuntime
// (include/ymh/host/host_runtime.hpp:74) and the test double FakeTransportHost
// (tests/support/fake_transport_host.hpp:19). This pure virtual MUST be
// overridden in FakeTransportHost too (a stub that records the call beside
// `compactSession`, :198), or -Werror stops the hermetic suite building.
virtual void invokeCommand(const SessionId& id,
                           std::string_view line,
                           CommandSource source) = 0;
```

```cpp
// include/ymh/transport/protocol.hpp (amended)
namespace method {
inline constexpr std::string_view kCommandInvoke = "command.invoke"; // 82-D8
} // namespace method

enum class HostNoticeKind : std::uint8_t {
    SessionClosed, SessionCreated, LeaseLost, DaemonShuttingDown, McpServerStatus,
    HandoffResult,   // 82-D9: `detail` is the handoff result line
};
```

```cpp
// include/ymh/transport/protocol_server.hpp (amended; beside onMcpServerStatus :74)
// Emits HostNotice{HandoffResult,...} to Interactive connections on the transport
// io thread. Caller: the daemon's marshalled HandoffNoticeForwarder (sec 6), never
// directly from HostRuntime::invokeCommand's turn-executor lambda
// (protocol_server.hpp:12-14).
void onHandoffResult(const SessionId& session, std::string detail);
```

```cpp
// include/ymh/ui/command_registry.hpp (amended, add to CommandContext)
// Forwards the raw argument tail of `/handoff` to the supervisor, which submits
// the durable `command.invoke` RPC (82-D2).
std::function<void(const std::string&)> handoff;
```

```cpp
// src/ui/command_registry.cpp (amended; registered BEFORE :285 so /help lists it)
registry.add(Command{
    "handoff",
    "summarize this session for continuation (writes a doc, seeds a session)",
    [](CommandContext& context, const std::string& args) {
        if (context.handoff) { context.handoff(args); }
    }});
```

```cpp
// src/ui/supervisor.cpp (amended; wired in dispatch_command :2665-2747)
context.handoff = [this](const std::string& args) { request_handoff(args); };

void SupervisorApp::request_handoff(const std::string& args) {
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
        // append_system_entry(model_, state, "handoff: no active session")
        return;
    }
    nlohmann::json params{{"session", workspace->activeSessionId().value},
                          {"line", "handoff " + args}, {"source", "user"}};
    submit_to(workspace->id, std::string(protocol::method::kCommandInvoke),
              std::move(params), nullptr);
}
```

```cpp
// src/host/host_runtime.cpp (amended; mirrors compactSession :965-979, and its
// inline throw_mapped forms at :968-969 / :975-976)
void HostRuntime::invokeCommand(const SessionId& id, std::string_view line,
                                CommandSource source) {
    translate([&]() {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        ensureAgent(id);
        if (!turns_.submit(id, [this, id, text = std::string(line), source]() {
                std::shared_ptr<AgentLoop> agent = runtime_.agents().findShared(id);
                if (agent == nullptr) { return; }
                const CommandOutcome outcome =
                    runtime_.commands().invoke(*agent, text, source);
                // v1 has exactly one registered command (`handoff`), so the
                // completion notice kind is HandoffResult. If a second command
                // adopts command.invoke, the notice must become per-command
                // (recorded seam; not implemented here).
                // io-thread contract (include/ymh/transport/protocol_server.hpp:12-14):
                // this lambda runs on the turn executor, so the notice is
                // marshalled through handoff_notice_ (the daemon wires it to
                // TransportServer::post, like forwardEvent at
                // src/host/workspace_host.cpp:1120-1128); onHandoffResult then
                // runs on the transport io thread and is never called here.
                if (handoff_notice_) {
                    handoff_notice_(id, outcome.text);
                } else if (server_ != nullptr) {
                    // no daemon io thread installed: single-threaded test wiring
                    // only (cf. handleCommittedRecord, src/host/host_runtime.cpp:437-441)
                    server_->onHandoffResult(id, outcome.text);
                }
            })) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InternalError),
                                   "InboxFull"});
        }
    });
}
```

```cpp
// include/ymh/host/host_runtime.hpp (amended; beside the event forwarders :79-85)
// io-thread marshalling seam for the handoff completion notice. The daemon wires
// it to `TransportServer::post` (`include/ymh/transport/transport_server.hpp:91`),
// exactly like EventForwarder/LiveEventForwarder (:79-85), so ProtocolServer
// fan-out stays on the io thread; when unset, HostRuntime calls
// ProtocolServer::onHandoffResult directly (single-threaded tests only).
using HandoffNoticeForwarder = std::function<void(const SessionId&, std::string)>;
// Construction caller: WorkspaceHost::Impl (src/host/workspace_host.cpp:903-906,
// the same site that installs the two event forwarders) passes
// `[this](const SessionId& id, std::string detail) {
//      forwardHandoffNotice(id, std::move(detail)); }`;
// forwardHandoffNotice posts via transport_->post(...) like forwardEvent (:1120-1128).
HandoffNoticeForwarder handoff_notice_;   // ctor param, beside :102-113
```


```cpp
// include/ymh/agent/workspace_runtime.hpp + src/agent/workspace_runtime.cpp
// (amended; beside compactor_ :413/:551, 82-D11)
std::unique_ptr<HandoffService>   handoff_;
std::unique_ptr<CommandRegistry>  commands_;   // ymh::CommandRegistry
// The accessor HostRuntime::invokeCommand calls (declared on WorkspaceRuntime,
// beside sessions()/agents()/pool() at include/ymh/agent/workspace_runtime.hpp:157-166):
[[nodiscard]] CommandRegistry& commands() noexcept;   // returns *commands_

// construction (scope resolution is pinned for v1: one global scope):
const ScopeKey   global_scope{};                         // == ScopeKey{} (empty)
const ScopeFor   scope_for = [](const Agent&) { return ScopeKey{}; };
const ScopeParent parent   = {};                         // nullopt => chain is {global}
handoff_  = std::make_unique<HandoffService>(runtime_, pool_, sessions_,
                                             environment(), &model_catalog_,
                                             to_handoff_policy(config));
commands_ = std::make_unique<CommandRegistry>(sessions_, scope_for, parent);
commands_->add(make_handoff_command(*handoff_), global_scope);
// No AgentServices amendment: AgentServices has no `handoff`/`commands` fields
// (include/ymh/agent/agent_loop.hpp:52-91) and none is added. The handler holds
// its HandoffService& directly (make_handoff_command(*handoff_)), and
// HostRuntime reaches the registry through runtime_.commands().
```

```cpp
// include/ymh/cli/wiring.hpp (amended; beside to_compaction_policy :46)
// 82-D10: map the layered `[handoff]` config onto HandoffPolicy.
// Caller: WorkspaceRuntime construction above (src/agent/workspace_runtime.cpp).
[[nodiscard]] HandoffPolicy to_handoff_policy(const Config& config);
```

## 7. Events and consumers

### 7.1 No new event types

`/handoff` reuses three shipped durable types and **adds none**
(`EventType`, `include/ymh/core/event.hpp:51-85`):

- `command/run` + `command/done` (`events.hpp:326-341`) on the **source**
  session, log-only (44-I12/44-I13);
- `context/injected` (`events.hpp:176-181`) on the **seed** session.

Because no `EventType` or payload changes, `deriveMessages`, the codecs
(`src/session/events.cpp:765-810`), and the export renderer need no case changes.
`deriveMessages` already ignores `CommandRun`/`CommandDone`
(`src/session/session.cpp:419-420`) and already materializes `ContextInjected`
(`:525-532`).

### 7.2 Additive notice consumer

`HostNoticeKind::HandoffResult` is rendered by
`UiEventAdapter::onHostNotice` (`src/ui/ui_event_adapter.cpp:394-431`, new
`switch` case; the existing switch has no `default`, so a missing case is a
`-Wswitch` error) as a System notice on the source session (or the notice ring
when it is not modeled). It is live-only, like `McpServerStatus`; a
resumed/attached supervisor relies on the durable `command/done` row, the doc,
and the stored seed session selectable from `/sessions` instead - so no
completion notice is lost to durability, only the transient toast is.

## 8. Failure modes (HF1-HF10)

Tagged per `00` sec 54. Each is a pinned `HandoffResult::Outcome` plus the
`command/done` `kind`/`text` and the notice.

| ID | Failure | Detection | Handling |
|---|---|---|---|
| HF1 | Empty session | no user/assistant events | `Empty`; no LLM call, no file, no seed; `command/done{kind=Error, text="handoff: nothing to summarize (empty session)"}`; notice |
| HF2 | Disabled | `handoff.enabled == false` | `Disabled`; `command/done{Error}`; no file/seed |
| HF3 | Bad option / path escape | unknown token; unparsable `--out`; `resolve_handoff_path` throws `ToolError{PathEscape}` (`include/ymh/ui/session_export.hpp:38-45`) | `BadOption`; `command/done{Error, text="handoff: unknown option <t>"}`; nothing written (an escape writes no file, sec 2.1/H3) |
| HF4 | No provider route / no key | route resolution or pool acquisition fails | `NoRoute`; `command/done{Error, text="handoff: no model route"}`; no file/seed. Mirrors compaction `C-F12` (`include/ymh/agent/compactor.hpp:102-116`, `NoProviderRoute` at :111) |
| HF5 | Context too large | bounded digest still exceeds floor | `ContextTooLarge`; no file/seed. Never truncates the most recent turns |
| HF6 | Summary bounding failure | model output exceeds `max_summary_bytes` | `bound_summary`-style deterministic truncation (`src/agent/compactor.cpp:271-283`); if still over, `Internal` |
| HF7 | Disk full / write failure | temp-write or rename fails | `WriteFailed`; **no seed** (write-before-seed, 82-D7); `command/done{Error, text="handoff: cannot write <path>"}` |
| HF8 | Store unavailable | the source `command/run`/`command/done` append, or the seed store create, throws | `StoreUnavailable`; no seed; `command/done{Error}` |
| HF9 | Seed-create failure after doc | `SessionManager::createSession`/append throws | `WriteFailed`/`StoreUnavailable` with a doc-present detail: the doc stays, no seed. The result text names the doc so the work is not lost |
| HF10 | Cancellation | user cancels / daemon stops mid-call | `Cancelled`; `CancellationToken` fires; `command/done{Error, text="handoff: cancelled"}`; if already written, the doc stays; the seed is attempted only after the doc, so a cancelled run either has a doc and no seed, or neither |

**Invariant with HF10**: a handoff is *atomic at the seed boundary* - a seed
session exists only if its doc was fully written (82-D7). The doc may exist
without a seed (HF7/HF9/HF10), but never the reverse.

## 9. Invariants (H1-H16)

| ID | Invariant |
|---|---|
| H1 | **No conversation mutation.** `/handoff` appends only `command/run`+`command/done` to the source log; it adds no user/assistant/tool event and changes no header. |
| H2 | **No clear.** `/handoff` never clears the presented conversation; `/clear` is the only view-clear (`src/ui/command_registry.cpp:157-168`). |
| H3 | **Path safety.** Every doc path resolves through `ExecutionEnvironment::resolve()` under the workspace root; `getcwd()` is never a resolution base; escape is `PathEscape` (`00` sec 18; `include/ymh/ui/session_export.hpp:38-45`). |
| H4 | **Durable pairing.** Each `/handoff` produces at most one `command/done` per `command/run`, correlated by `command_id` (44-I13); both are log-only (44-I12). |
| H5 | **Empty is a no-op.** An empty session performs no LLM call, no write, no seed (HF1). |
| H6 | **Root seed, stored-only.** The seed is `kind='root'`, `parent_session=NULL`, `seed_length=NULL`; its first non-lifecycle event is `context/injected` with `MessageSource::Kind::Plugin`, `ContextForm::Recall`, `plugin=="ymh.handoff"`. It is created stored-only (no live agent, no registry open-set row; the durable per-session lease is taken by `SessionPersistence::create`, `src/session/session_persistence.cpp:883-885`) and emits no `SessionCreated`; it becomes live only on resume (sec 4.2). |
| H7 | **Write-before-seed.** A seed session exists only if its doc was fully written and renamed (sec 8). |
| H8 | **Non-blocking.** The TUI submits `command.invoke` and returns on `{outcome:"Queued"}`; no LLM call runs on the UI thread or the RPC/io thread. |
| H9 | **Focus unchanged.** `/handoff` never calls `session.activate` and never changes focus; the stored-only seed is surfaced by `/sessions` (stored list) and becomes live only when resumed (`session.resume`; `src/ui/supervisor.cpp:1574`, focus-only rule at `:1669-1672`). |
| H10 | **Six sections.** Every summary contains the six pinned headings in order; empty sections render `(none)`; the doc and the seed text are byte-identical summaries. |
| H11 | **No event-type growth.** No `EventType` is added; `CommandRun`/`CommandDone`/`ContextInjected` are reused unchanged. |
| H12 | **`/export` independent.** `/handoff` shares no file or state with `/export`; both read the log independently and neither affects the other (`include/ymh/ui/session_export.hpp`). |
| H13 | **ASCII doc.** The doc body is ASCII (Markdown from the summarizer; non-ASCII codepoints are replaced with `?` deterministically before write). |
| H14 | **Deterministic digest.** Same log + same header => identical evidence digest (needed for FakeLLM byte assertions). |
| H15 | **Single writer.** The handler runs on the session's turn executor and is serialized with other turn work (44-I17); it never races an agent turn append. |
| H16 | **Title cap.** Any `--title`/derived title obeys `kMaxSessionTitleBytes` (`include/ymh/session/session.hpp:91`); the source title is never modified. |

## 10. State lifetime

Every state element this errata introduces, with create/update/destroy, owner,
and restart behavior (AGENTS.md "State lifetime is part of the spec").

| State | Created | Updated | Destroyed / evicted | Owner | Process restart |
|---|---|---|---|---|---|
| `HandoffOptions` / `HandoffPolicy` | parsed per invocation / built at daemon start | never | end of `HandoffService::run` / daemon exit | turn executor (transient) / `WorkspaceRuntime` (policy) | policy rebuilt from config; options gone |
| `HandoffResult` | `run` return | never | end of the calling task | turn executor (transient) | gone |
| Summary buffer (evidence digest + six-section summary string) | `HandoffService::run` (stack-local) | never | end of `run`; the text is persisted only as the doc file | turn executor (transient) | gone; the doc on disk is the durable copy |
| Doc file `<ws>/.ymh/handoffs/<id>-<stamp>.md` | first successful handoff | never (immutable once written) | only by the user / `session prune`-style external cleanup; never by ymh automatically | user-owned file on disk | survives; not tracked in any DB |
| `command/run` / `command/done` rows | `CommandRegistry::invoke` | never | only with the source session (`session.delete`) | source session log | survives (durable) |
| Seed `SessionHeader` row (`kind=root`) | `SessionManager::createSession` (stored-only) | `updated_at` per appended event | `session.delete` / spec-23 cleanup if never prompted | seed session in `sessions.db` | survives (durable); not in the registry open set; the durable `session_leases` row is taken at create (`src/session/session_persistence.cpp:883-885`) |
| Seed `context/injected` event | `HandoffService::run` (post-create) | never | with the seed session | seed session log | survives (durable) |
| `ymh::CommandRegistry commands_` | `WorkspaceRuntime` construction | only `add()` at construction; `next_command_id_` monotonically increments per invoke | daemon exit | `WorkspaceRuntime` (process-local) | rebuilt; `next_command_id_` restarts at 0 (ids are per-daemon, not durable keys) |
| `HandoffService handoff_` | `WorkspaceRuntime` construction | none (immutable policy) | daemon exit | `WorkspaceRuntime` (process-local) | rebuilt |
| `HandoffResult` host notice (wire) | daemon after settlement | never | consumed by `onHostNotice` | daemon io -> supervisor UI thread | n/a (transient) |
| In-flight work item in `turns_` | `HostRuntime::invokeCommand` | n/a | when the task completes / daemon stops | per-session turn executor | lost on crash; the durable `command/run` without a `command/done` is the observable orphan (a resumed supervisor sees a dangling run; sec 13 R1) |

None of the new state is written to `registry.db`, config, or any file other
than the doc and the two session logs. No new durable state can be corrupted by a
crash: the worst crash case is an orphaned `command/run` (H4's at-most-one-done
still holds), which is audit-only and never enters context.

## 11. dsh (DeepSeek Harness) mapping

dsh is a headless harness whose UI is a plugin (`00-architecture.md:69`); it has
no interactive command surface and no `/handoff`. Every non-mirror cell carries
its reason and a concrete anchor (56-D6).

| dsh concept | ymh realization (82) | Anchor / non-mirror justification |
|---|---|---|
| `command/run` + `command/done` pairing | reused unchanged; `/handoff` appends them on the source session | Mirrors spec 44 sec 4.1 (`docs/design/44-goals-jobs-commands.md:417-425`; `include/ymh/session/events.hpp:326-341`). No divergence. |
| Durable command wire path | additive `command.invoke{session,line,source}` -> `CommandRegistry::invoke` | **Non-mirror, deliberate.** dsh has no interactive client; spec 44 left the wire open (44 sec 13 OQ-2, `:1235-1241`). This errata pins the method (`include/ymh/commands/command_registry.hpp:40`). |
| Context recall into a session | seed session's first event is `context/injected` (`ContextForm::Recall`, plugin `ymh.handoff`) | Mirrors dsh `ContextInjected` / `ContextForm` (`include/ymh/agent/provenance.hpp:38-48`, `:150-167`; `include/ymh/session/events.hpp:176-181`). No divergence in shape; the producer is new. |
| Handoff summary | six-section Markdown from the summarizer | **Non-mirror.** dsh defines no handoff artifact; the closest ymh precedent is the compaction checkpoint prompt (`src/agent/compactor.cpp:17-50`), which inspired but does not equal this structure. |
| On-disk handoff doc | `<ws>/.ymh/handoffs/<id>-<stamp>.md` | **Non-mirror, ymh extension.** dsh is headless and writes no such doc; Claude Code has no native handoff either (`/tmp/opencode/recon-claude-code.md` sec 4; scratch input, not a repo file). Reading/editing/grepping it is the portable continuation channel. |
| Live completion notice | additive `HostNoticeKind::HandoffResult` | **Non-mirror, deliberate.** dsh has no live UI; ymh mirrors its own `SessionCreated`/`McpServerStatus` live-notice channel (`include/ymh/transport/protocol.hpp:240-253`). The durable `command/done` remains authoritative for replay. |
| Compaction as the compression primitive | `/handoff` is orthogonal; it does not emit `ContextCompaction` | **Non-mirror, deliberate scope.** Compaction mutates the source's derived context (13 sec 3.4); handoff must not (H1). Reusing the event would violate H1. |
| Fork (`session.fork`) | **not used**; seed is a stored-only `kind='root'` session from a summary, not a copy | **Non-mirror, deliberate.** A fork copies parent events (`src/session/session.cpp:766-801`); the handoff exists to shed them. `/fork` (spec 78) reuses that path; `/handoff` shares none of it (sec 4.4). The seed is live only after `session.resume`. |

## 12. Test plan (hermetic; FakeLLM)

All unit/integration tests use the deterministic FakeLLM (sec 45) and a temp
workspace; no network. `HS-*` are hermetic, `HSL-*` live.

### 12.1 Unit - summary and digest (`tests/unit/handoff_test.cpp`)

- **HS-U1 (sections).** `handoff_instruction()` contains the six headings in
  order; a FakeLLM reply missing a heading is rejected/prepended deterministically
  ("(none)") so H10 holds.
- **HS-U2 (deterministic digest).** `build_handoff_evidence` over a fixed log
  returns a byte-identical string across two calls (H14); it includes the turn
  count, the last turn's status, the deduplicated file list (capped at
  `kMaxHandoffFiles`), and failed tool calls.
- **HS-U3 (empty).** Empty session -> `Outcome::Empty`, `command/done{Error}`,
  no file, no seed (H5/HF1).
- **HS-U4 (bounding).** An oversized log with a prior `ContextCompaction` uses
  the compaction summary base plus the post-boundary tail; without compaction it
  drops oldest turns and keeps the most recent `keep_recent_turns`.
- **HS-U5 (too large).** A floor-exceeding digest -> `ContextTooLarge`, no file,
  no seed (HF5).

### 12.2 Unit - path and doc (`tests/unit/handoff_test.cpp`)

- **HS-U6 (default path).** `resolve_handoff_path` yields
  `<ws>/.ymh/handoffs/<id>-<stamp>.md`; the dir is created.
- **HS-U7 (escape).** `--out ../../etc/passwd` throws `PathEscape` (H3).
- **HS-U8 (collision).** Two handoffs in the same second produce `-2` on the
  second file (sec 4.1).
- **HS-U9 (write-before-seed).** A forced write failure -> `WriteFailed`, no seed
  (H7/HF7).
- **HS-U10 (ASCII).** A FakeLLM reply with a non-ASCII codepoint yields an ASCII
  doc (H13).

### 12.3 Unit/integration - seed session (`tests/unit/handoff_seed_test.cpp`)

- **HS-U11 (root seed).** After a successful handoff the seed header is
  `kind=root`, `parent_session=nullopt`, `seed_length=nullopt`; the SQL CHECK
  accepts it (H6).
- **HS-U12 (seed event).** The seed's first non-lifecycle event is
  `context/injected` with `form=Recall`, `plugin=="ymh.handoff"`, `role=User`,
  and `text == doc summary` (H10/H6).
- **HS-U13 (replay).** Replaying the seed session (`Session::replay`,
  `src/session/session.cpp:762-764`) renders the summary as the first projected
  message (`deriveMessages`, `:525-532`).
- **HS-U14 (no-seed).** `--no-seed` writes the doc and creates no session.
- **HS-U15 (stored-only then live on resume).** Immediately after the handoff the
  seed **is** returned by `HostRuntime::listSessions`/`session.list` with
  `live==false` (`registered==false`); it is absent from the Ctrl-S **live**
  switcher, which filters `live==true`; and no `SessionCreated` notice was
  emitted (`src/host/host_runtime.cpp:528-535,547`; store-only ordering
  `src/registry/registry.cpp:609-644`); a following `session.resume` of the seed
  makes it live and emits `SessionCreated` (H6/H9, M6).
- **HS-I15 (command pair).** The source log gains exactly one `command/run` and
  one `command/done` with the same `command_id`; `deriveMessages` output is
  byte-identical before and after (H1/H4).
- **HS-I16 (notice).** A `HandoffResult` host notice appends the result line to
  the source session (or the ring) and never changes focus (H9).

### 12.4 Integration - wire and non-blocking (`tests/unit/host_runtime_test.cpp`, `tests/unit/command_registry_test.cpp`)

- **HS-I17 (Queued first).** `command.invoke` replies `{outcome:"Queued"}`
  before the FakeLLM handler settles (use a blocking FakeLLM gate); the RPC
  thread is not blocked (H8).
- **HS-I18 (routing).** `command.invoke` with `line="handoff"` resolves the
  `handoff` spec in the global scope; an unknown name is a JSON-RPC error.
- **HS-I19 (help).** `/handoff` appears in `/help` output (registered before
  `src/ui/command_registry.cpp:285`).
- **HS-I20 (no-route).** A missing provider route -> `NoRoute`, no file, no seed
  (HF4).

### 12.5 Live (opt-in) (`tests/unit/handoff_live_test.cpp`)

- **HSL-1.** `YMH_LIVE_LLM=1` real-binary PTY run: `/handoff` writes a doc whose
  six sections are non-empty on a real session, creates the stored-only root
  seed, and the seed resumes with the summary visible. Skipped without the
  env/key.

## 13. Scope boundaries and recorded risks

- **R1 - orphaned `command/run` on crash.** A daemon crash mid-handoff leaves a
  `command/run` with no `command/done` in the source log. It is log-only and
  never enters context; a resumed supervisor shows no completion notice. Not
  repaired in v1 (matching how in-flight `tool/call`s are handled today); the
  doc either exists or does not.
- **R2 - no doc cleanup.** Docs accumulate under `<ws>/.ymh/handoffs/`. ymh never
  deletes them in v1; a future `session prune --handoffs` (not this spec) is the
  seam. Recorded, not implemented.
- **R3 - seed inherits presets.** The seed copies the source's `agent_preset`
  and `permission_preset`. Within one workspace/user this is intended; a future
  cross-workspace handoff (out of scope) would need a re-resolve.
- **R4 - summarizer cost/quality.** A handoff costs one summarizer call. Quality
  is bounded by the same route as compaction; no new model requirement is added.
- **R5 - no global `/handoff` CLI.** v1 is TUI-only (the CLI `ymh run` path is
  unaffected). A `ymh session handoff` subcommand is out of scope.

## 14. Decision list (82-D#)

| ID | Decision | Rationale / anchor |
|---|---|---|
| 82-D1 | `/handoff` is a **durable** command (not a presentation-only builtin), forwarded to the host. | Summarization is long and must be attributed in the log; spec 44 sec 4.2 (`docs/design/44-goals-jobs-commands.md:427-442`). |
| 82-D2 | The TUI registers `/handoff` via `CommandContext::handoff` and `SupervisorApp::request_handoff`, mirroring `context.compact`. | Same submit/reply shape as `/compact` (`src/ui/supervisor.cpp:2672-2680`); one new callback field. |
| 82-D3 | Summary has exactly the six pinned sections; empty renders `(none)`. | Matches the compaction checkpoint precedent (`src/agent/compactor.cpp:17-50`) but adds Open Questions and Relevant Files and drops compaction-only framing. |
| 82-D4 | Empty session is a pinned no-op (no call, no file, no seed). | Avoids cost and a useless artifact; sec 3.3. |
| 82-D5 | Default doc is `<ws>/.ymh/handoffs/<session-id>-<stamp>.md`, collision-suffixed. | Session-id avoids title collisions; `/export`'s stem+stamp is the precedent (`include/ymh/ui/session_export.hpp:34-36`), minus its overwrite gap. |
| 82-D6 | Default output creates a **stored-only root** session from the summary via `context/injected`; it becomes live only on resume. | Root satisfies the shipped CHECK with no schema change (`src/session/session_persistence.cpp:49-50`); `ContextInjected` is the shipped projection seam (`src/session/session.cpp:525-532`). Stored-only avoids the live-creation path (`AgentRegistry::create` + registry open-set row) that `HandoffService` (a `SessionManager&` owner) cannot reach; the durable per-session lease is still taken by `SessionPersistence::create` (`:883-885`) (M6/N2). |
| 82-D7 | Write-before-seed: a seed exists only if its doc exists. | A handoff without its durable artifact is half a contract; HF7/HF9. |
| 82-D8 | Additive wire method `command.invoke{session,line,source?}`, reply `{outcome:"Queued"}`. | Resolves 44 OQ-2 (`docs/design/44-goals-jobs-commands.md:1235-1241`); generic, so `/goal` may reuse it. Added to BOTH `namespace method` and the `kMethodCatalog` routing array (`src/transport/protocol.cpp:637-653`, size 38 -> 39), plus the `ProtocolServer::handle_method` dispatch branch (`src/transport/protocol_server.cpp:443-445` analogue, fallback `:561-564`); a header constant alone yields `MethodNotFound` (`src/transport/protocol_server.cpp:242`). |
| 82-D9 | Additive `HostNoticeKind::HandoffResult` renders the outcome live via `ProtocolServer::onHandoffResult`. | Reuses the shipped live-notice channel (`include/ymh/transport/protocol.hpp:240-253`); added to `kHostNoticeKinds` (`src/transport/protocol.cpp:53-59`, size 5 -> 6); durable `command/done` stays authoritative. |
| 82-D10 | New `[handoff]` config keys with the `HandoffPolicy` defaults; empty `summarizer_model` uses the runtime route. | Mirrors `CompactionPolicy` (`include/ymh/agent/compactor.hpp:43-58`); amends 21. |
| 82-D11 | `WorkspaceRuntime` owns `HandoffService` and the host-side `ymh::CommandRegistry`; a new `WorkspaceRuntime::commands()` accessor exposes it; `HostRuntime::invokeCommand` is the registry's first caller. | Supplies the missing construction site (grep: no `add(CommandSpec,...)` caller) and a concrete caller for `make_handoff_command`, per AGENTS.md. No `AgentServices` field is added (M3). |
| 82-D12 | The seed inherits model/route/presets but is **not** a fork. | A fork copies parent events (`src/session/session.cpp:766-801`), defeating the handoff; sec 4.4. |
| 82-D13 | `/handoff` does not clear or mutate the source conversation, and does not change focus. | Preserves `/clear`'s uniqueness (`src/ui/command_registry.cpp:157-168`) and the focus-only rule (`src/ui/supervisor.cpp:1669-1672`). |
| 82-D14 | No new `EventType`; reuse `CommandRun`/`CommandDone`/`ContextInjected`. | Minimizes codec/consumer churn; H11. |
| 82-D15 | `--no-seed` is provided; the doc is always the durable artifact. | Lets a user keep one session; sec 4.2. |
| 82-D16 | The interface/enum amendments are mirrored in every implementer/consumer: `FakeTransportHost::invokeCommand` (test double) and the `HostNoticeKind::HandoffResult` case in `UiEventAdapter::onHostNotice`. | A pure virtual with no override and an unhandled enum case both fail `-Wall -Wextra -Werror`; the shipped fake has no `invokeCommand` (`tests/support/fake_transport_host.hpp:19,198`) and the adapter switch has no `default` (`src/ui/ui_event_adapter.cpp:394-431`), so both are amended (M2/M5). |
| 82-D17 | The seed is surfaced stored-only via `/sessions` and becomes live on resume; no `SessionCreated` is emitted at creation. | Corrects revision 1's live-seed claim (M6); preserves 22/57's live-vs-stored split and keeps `/handoff` off the `AgentRegistry`/registry-open-set path (the durable lease is still taken at create, 82-D6/N2). |

## 15. Revision log

| Revision | Date | Change |
|---|---|---|
| 1 | 2026-10-07 | Initial draft. Pins the `/handoff` grammar, six-section summary with evidence mapping, the `<ws>/.ymh/handoffs/` doc, the default root seed via `context/injected`, the additive `command.invoke` wire method (resolving 44 OQ-2) and `HostNoticeKind::HandoffResult`, the `[handoff]` policy, invariants H1-H16, failure modes HF1-HF10, the state-lifetime table, the dsh mapping, the hermetic test plan, and decisions 82-D1-82-D15. No implementation. |
| 2 | 2026-10-07 | Gate-82 MEDIUM/LOW fixes. M1: `command.invoke` added to `kMethodCatalog` (`src/transport/protocol.cpp:634-651`, 38 -> 39) and `namespace method`, with the real routing anchor. M2: `HandoffResult` added to `kHostNoticeKinds` (5 -> 6). M3: `WorkspaceRuntime::commands()` accessor, `ProtocolServer::onHandoffResult` seam, dropped the nonexistent `AgentServices` fields, single wiring story. M4: `Outcome::Cancelled` for HF10. M5: `FakeTransportHost::invokeCommand` stub pinned. M6/82-D17: seed is stored-only, surfaced via `/sessions`, live only on resume. M7: spec 78 is an authored draft, not unauthored. L1-L11: path/ref/anchor/test-file/include/scope corrections; added `to_handoff_policy` signature + caller; ASCII preserved; header `Amends:` and revision bumped. |
| 3 | 2026-10-07 | Gate-82 re-review (FAIL) fixes. N1: pinned the `ProtocolServer::handle_method` `command.invoke` dispatch branch (the concrete caller of `TransportHost::invokeCommand`). N2a: corrected the lease claim - `SessionManager::createSession`/`SessionPersistence::create` does take the per-session lease (`session_persistence.cpp:883-885`); fixed H6, the state-lifetime row, 82-D6, and 82-D17. N2b: HS-U15 now asserts the seed is returned by `listSessions` with `live=false` (absent only from the Ctrl-S live switcher). N3: inlined the `throw_mapped` forms in the `invokeCommand` sketch. N4: added `serverProfile` to the seed's inherited options. N5: pinned the io-thread marshal for `onHandoffResult` via `TransportServer::post`/`HandoffNoticeForwarder`. N6: mapped `PathEscape` to `BadOption` in HF3. N7: anchor corrections (`kMethodCatalog` :637-653, `compact()` :285-430, `SessionOptions` :29-50). Revision bumped. |
