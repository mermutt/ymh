# 70 — MCP Failure Diagnosability Errata

```
Status: verified (Rev 7) · reviewer: Oracle gate (round 6) + post-gate LOW audit · gate: 0 HIGH / 0 MEDIUM
Revision: 7
Component: 70 (errata) — amends 15-mcp-adapter.md (§4.2, §4.3, §4.7, §5.1,
           §9.2 MCP-F1/F2/F3/F7), 07-tools-execution.md §6.4
           (`ProcessRequest::stderr_fd`, `ChildProcessHandle::tryReap`,
           `ChildProcessHandle::reaped`),
           45-ui-interaction-errata.md (45-D6.5, 45-D6.9, 45-I14),
           21-config-jsonc-errata.md (the `mcp.log_child_stderr` interaction), and
           18-context-errata.md (M8, retained); reuses the presentation
           conventions pinned by 69-host-startup-diagnosability-errata.md
           (Rev 1 + Rev 2) by reference. Adds no new RPC, no event type, no
           config key, no persistence schema change; three additive process-seam
           members (`ProcessRequest::stderr_fd`, `ChildProcessHandle::tryReap`,
           `ChildProcessHandle::reaped`) and one additive transport seam.
Depends on: 15-mcp-adapter.md (verified), 07-tools-execution.md (verified;
            `ProcessRequest`/`ChildProcessHandle`), 45-ui-interaction-errata.md
            (verified), 18-context-errata.md (verified), 21-config-jsonc-errata.md
            (verified), 69-host-startup-diagnosability-errata.md (verified)
Scope: why a configured MCP server failed. Every MCP failure path — spawn,
       child exit before handshake, handshake timeout, initialize rejection,
       unsupported revision, transport close, tools/list failure — must reach
       the user as a specific cause per server in `/mcp`, with a bounded,
       redacted context block, instead of the bare `McpErrorCode` token the
       current code stores and ships as nothing.
```

## 1. Purpose, scope, and precedence

The user report is *"`/mcp` shows that all configured mcps failed. But without
additional info I can't figure out why."* The report is correct, and it is the
same defect class `69-host-startup-diagnosability-errata.md` fixed for the
daemon-startup path: an opaque aggregate failure that discards the cause.

Four independent discards stack up so the user cannot see why:

1. `StdioMcpTransport::start` throws
   `McpError{SpawnFailed, to_string(error.code())}`
   (`src/mcp/mcp_transport.cpp:149`), dropping `error.what()` (which carries
   `exec: <strerror>`), and it sets `request.capture_stderr = false` with no
   `stderr_path` (`:118-119`), so `spawn()` routes the child's stderr to
   `/dev/null` (`src/execution/process.cpp:555-557`). A child that starts and
   then exits — e.g. `python3 /missing/script.py` — leaves **no** trace.
2. `DefaultMcpClient::onClose` discards the `McpDisconnectReason` (`(void)reason;`,
   `src/mcp/mcp_client.cpp:312-313`), and the `initialize`/`tools/list` rejection
   messages drop the server's own JSON-RPC `error.message`
   (`src/mcp/mcp_client.cpp:104`, `:114`, `:155`).
3. `McpManager` stores only the enum token:
   `setStateLocked(slot, Failed, std::string{to_string(error.code())})`
   (`src/mcp/mcp_manager.cpp:290-291`, `:343-344`, `:412`) — the site the user
   sees. `error.what()` is discarded.
4. `mcp_status_json` deliberately drops the text from the wire
   (`src/host/host_runtime.cpp:237`, `:251`: "`last_error` is never read into the
   JSON"), and `format_mcp_block` renders only `not connected  <state>`
   (`src/ui/status_format.cpp:128-130`). So even a populated reason would not reach
   `/mcp`.

This errata pins a **diagnosability** fix, not a behavioural one:

- the numeric/`McpErrorCode`/`McpDisconnectReason`/`max_servers`/exit-code
  contracts stay frozen (70-D1);
- a stdio child's stderr is captured to a bounded diagnostic sink instead of
  `/dev/null`; a bounded, redacted tail — plus the child's exit status when known
  — is surfaced on failure (70-D3/70-D4);
- every failure path composes its own specific cause (70-D1/70-D2/70-D5/70-D6);
- a single bound/redact choke point (70-D7) makes the stored/emitted reason
  secret-free before it reaches storage, the bus, the wire, or the UI;
- `mcp.status` gains a bounded, redacted `reason` per failing server (70-D9,
  additive; the raw `last_error` is still never shipped), and `/mcp` renders it
  in the 69-D9 Rev 2 style (70-D10).

It is **additive**: no `McpServerStatus` struct field is added (the `reason` is
derived on demand from the existing `last_error` in the serializer), the only new
state is a per-transport capture fd/offset/exit status whose lifetime is pinned
(§7), the process seam gains three defaulted/additive members (70-A13/70-A14), and no
transport protocol, registry schema, config key, or event type changes.

**Precedence.** This errata wins over `15 §4.3` for the *content* of
`McpServerStatus::last_error` (it must be the specific cause, bounded/redacted per
70-D7, not a bare enum token), over `15 §4.2` additively for the
`McpDiagnosticTransport` seam, over `45-D6.5`/`45-I14` for the new `reason` wire
field (the raw `last_error`/`server_name`/`server_version` remain unshipped), and
over `69` only for reusing its Rev 2 presentation conventions (cause leads,
labelled bounded context, newlines preserved). It does not change `45-D6`'s result
schema beyond the additive `reason` field, `18-M8`, or `15 §2.2`'s error taxonomy.
The 69 conventions are reused **by reference**, not re-specified.

**Rev 2 (gate round 1).** An adversarial Oracle gate round 1 returned **FAIL
(1 HIGH / 6 MEDIUM / 5 LOW)**. Rev 2 fixes all of them: the HIGH is that 70-D7 did
not guarantee valid UTF-8 while the reason is `dump()`-ed and persisted
(`type_error.316` on raw child-stderr bytes) — 70-D7 now coerces to valid UTF-8 and
truncates on codepoint boundaries (70-I12, `BoundReasonStaysValidUtf8`); the
MEDIUMs are: the transport-write path still threw a bare `ToolErrorCode` token
(70-A4/70-D2, 70-I1 broadened); `failureContext()` had no pinned caller (70-D8 now
pins `diagnostic_` and the call sites); the disconnect-token map was an unnamed
symbol (70-D5 now declares `mcp_disconnect_token`); the transient temp-file crash
row was factually wrong; the `failureContext()` read was unbounded (70-D3 now caps
at ≤64 KiB); and the `mcp_status_detail` change was unregistered (70-A12). The LOWs
(anchor off-by-one, 45-D6.9/18 anchors, 70-I3 wording, the fake-server extension)
are folded in.

**Rev 3 (gate round 2).** Oracle round 2 returned **FAIL (1 HIGH / 3 MEDIUM /
5 LOW)**, independent of the round-1 findings. Rev 3 fixes all of them:

- **H1 (new path, same class):** the 70-D11 live-notice byte `resize(256)` could
  split a codepoint and then `HostNotice::dump()` throws — 70-D11 now truncates the
  composed detail through the new `mcp_truncate_utf8` on a codepoint boundary, and
  70-I12 covers `mcp_status_detail` (`NoticeDetailWithLongMultibyteReasonDumps`).
- **M1 (`mkstemp`+unlink race):** the child reopened the path after `spawn()`, with
  no success signal, so a descheduled child could recreate a named file and lose the
  capture. Rev 3 adds the defaulted `ProcessRequest::stderr_fd` (70-A13) and passes
  the already-open capture fd; no path is reopened, no name is ever created
  (70-D3/70-I14).
- **M2 (free-`tryReap` pid reuse):** the free `tryReap` did not set
  `SpawnedChild::reaped_`, so `~SpawnedChild` could still signal a reaped pid. Rev 3
  adds the additive `ChildProcessHandle::tryReap()` (70-A14) that sets the handle's
  flag (70-D4/70-I9/70-I15).
- **M3 (bare empty-`what()` fallback):** the fallback is now the non-bare
  `"mcp failure: " + to_string(code)` (70-D1).
- **LOWs:** `\r` normalization ordered before control replacement (70-D7); the
  "struct field" over-claim removed (§1); the durable §7 row now lists
  `capture_fd_`; 70-I1 scoped to the server status channel (excluding
  `mcp_tool.cpp` tool-call errors); and the §5 write example matches
  `write stdin: …`.

**Rev 4 (gate round 3).** Oracle round 3 returned **FAIL (0 HIGH / 3 MEDIUM /
5 LOW)**: the H1/M3 fixes held, but the new fd design introduced hazards. Rev 4
fixes all of them:

- **M-1:** 70-I9's `close()` guarantee was unachievable because `tryReap()` returns
  `nullopt` after reaping, so `close()` could not tell "reaped" from "running" and
  still signalled. Rev 4 adds `ChildProcessHandle::reaped()` (70-A14) and pins
  `close()`'s short-circuit (70-D4/70-I9).
- **M-2:** the transient parent fd shares its open file description with the child
  (fork + `dup2`), so an `lseek`+`read` would move the offset under a live child.
  Rev 4 pins `pread` (70-D3/70-I13).
- **M-3:** a re-invoked `start()` (the `refresh` reconnect) would leak the prior
  `capture_fd_`. Rev 4 pins the close-before-reopen and the §7 eviction
  (70-D3/70-I13).
- **LOWs:** F14 dropped from the header/§8 (no register row); the disconnect-token
  claim moved from 70-A4 to 70-D5; the `dup2`-before-`close_inherited_fds` ordering
  pinned (70-A13); `O_TMPFILE|O_RDWR` pinned and the "no named temp file" claim
  tightened (70-D3/L-5); the `stderr_path`-fallback wording corrected.

**Rev 5 (gate round 4).** Oracle round 4 returned **FAIL (0 HIGH / 1 MEDIUM /
4 LOW)**: the Rev 4 M-1 fix covered the entry-reaped case but not `close()`'s
**false** branch, whose graceful loop still used the free `tryReap(pid)` and left
`SpawnedChild::reaped_` false, so `child_.reset()` could still SIGKILL a reaped
pgid. Rev 5 pins that `close()`'s loop and `failureContext()` both call
`child_->tryReap()` (70-D4/70-A14), with a monotonic flag
(`ProcessHandle.TryReapIsMonotonic`), and adds the loop's flag-setting to 70-I9.
LOWs: header/§1 "two"→"three" process-seam members; the §1 `/dev/null` anchor
corrected to `555-557`; the §7 durable row gains the reconnect close; and the
close-before-reopen placement is pinned after the `child_` early exit.

**Rev 6 (gate round 5).** Oracle round 5 returned **FAIL (0 HIGH / 1 MEDIUM /
1 LOW)**: the same defect class one branch over — `close()`'s post-SIGKILL
`(void)reap(pid)` used the free `reap(int)`, leaving `SpawnedChild::reaped_` false,
so `~SpawnedChild` could still SIGKILL a reaped pgid when a child wedged past the
grace window. Rev 6 pins that the post-SIGKILL disposal does not use the free
`reap(int)` (it drops the reap so `~SpawnedChild` reaps, or drains via
`child_->tryReap()`), and 70-I9 now enumerates that path; the header's `Amends`
parenthetical lists all three process-seam members.

**Rev 7 (post-gate LOW audit).** An independent Oracle re-audit returned
**PASS (0 HIGH / 0 MEDIUM)** with seven LOW accuracy findings; this revision
closes them: the §10 test labels are corrected to the shipped
`McpDiagnosticIntegration.*` / `ProcessService.TryReapIsMonotonic` names; the
`McpLiveStatusTest` row now states it pins the `mcp_truncate_utf8` primitive
(the host wrapper is translation-unit local); and 70-I3/70-F8 describe the
sink-selection fallback accurately (when the selected sink — the durable
`stderr_path` under `log_child_stderr`, else the anonymous capture fd — cannot be
created, neither `stderr_path` nor `stderr_fd` is set and the child degrades to
`/dev/null`). The diagnostic read end **is** set non-blocking
(`src/host/workspace_host.cpp:1459`), so the 69-D7 comment is accurate. Adds
`McpClientTest.HandshakeAndToolListFailuresNameTheirCause` pinning the server's
own JSON-RPC error text in the reason.

**Gate (per `AGENTS.md`).** Independent Oracle review marks this `verified` once
the reason channel, the stderr-capture/lifetime decision, the bound/redaction
decision, the wire-field amendment to 45-I14, and the per-server render are
confirmed (see `DESIGN_STATUS.md` row 70).

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 70-A1 | 15 §4.3 (`McpServerStatus::last_error`) and §4.7 (`McpManager::statuses`) | `include/ymh/mcp/mcp_types.hpp:143-152`; `src/mcp/mcp_manager.cpp:291,343,412,479-486` | pin that `last_error` is the **specific, bounded, redacted** cause; no path stores a bare `to_string(McpErrorCode)` |
| 70-A2 | 15 §4.2 (`McpTransport`) | `include/ymh/mcp/mcp_transport.hpp:45-57` | add the additive `McpDiagnosticTransport` seam (mirrors the additive `McpPollableTransport` precedent at `:50-57`) and the `mcp_disconnect_token` helper |
| 70-A3 | 15 §5.1 (process ownership, stdio spawn) | `src/mcp/mcp_transport.cpp:110-155`; `src/execution/process.cpp:555-557` | a stdio child's stderr is captured (transient unnamed file, or the durable `<root>/.ymh/mcp/<id>.stderr.log` when `mcp.log_child_stderr` is on), never `/dev/null` |
| 70-A4 | 15 §9.2 MCP-F1/F7 | `src/mcp/mcp_transport.cpp:146-150,170-172` | `SpawnFailed` includes `error.what()` (`exec: <strerror>`) and the command; the transport-write `TransportClosed` includes `error.what()` (`<strerror>`); no site throws a bare `to_string(error.code())` |
| 70-A5 | 15 §9.2 MCP-F2/F3 | `src/mcp/mcp_client.cpp:95-116,152-159` | handshake failure reasons include the server's JSON-RPC `error.code`/`error.message`, the received `protocolVersion`, and a bounded frame preview; plus the captured child context |
| 70-A6 | 15 §9.2 MCP-F7 | `src/mcp/mcp_client.cpp:312-322,369-377` | the `McpDisconnectReason` is retained and surfaced in the "transport closed" reason (70-D5 owns the disconnect token) |
| 70-A7 | 45-D6.5 / 45-I14 / 45-D6.9 | `src/host/host_runtime.cpp:237-255`; `docs/design/45-ui-interaction-errata.md:667-671,726-727,1668` | `mcp.status` **adds** `reason` (bounded, redacted, present iff there is a cause); raw `last_error`/`server_name`/`server_version` remain unshipped. Supersedes 45-D6.9's "it must not read `last_error` into the JSON" **only for the new, pre-redacted `reason` string**, not for the raw field |
| 70-A8 | 45-D6.9 (`format_mcp_block`) | `src/ui/status_format.cpp:100-138` | a failing server with a `reason` renders it: `reason:` leads on its own line, context lines follow, newlines preserved (69-D9/69-D10 by reference) |
| 70-A9 | 18 §4 / 18-M8 (`ContextServerEntry`); 18's rejection log | `src/agent/context_snapshot.cpp:219-226`; `docs/design/18-context-errata.md:1927-1933` | **retained unchanged**: `context.show` still ships only `{id,state,tool_count,skipped,has_error}`; the failure text is never in `context.show`. 18 rejected a redacted reason because it "cannot be bounded reliably"; 70-D7 changes that calculus (a total, valid-UTF-8, ≤4-line/≤512-byte choke point applied before storage) and confines the field to `mcp.status` |
| 70-A10 | 69-D9/D10/D11 (presentation conventions) | `docs/design/69-host-startup-diagnosability-errata.md:192-234` | reused by reference for the reason/context block and the newline-preserving render; no new style invented |
| 70-A11 | 21 §6 (`mcp.log_child_stderr`) | `include/ymh/mcp/mcp_types.hpp:206-208`; `src/mcp/mcp_transport.cpp:130-144` | the flag still selects the **durable** per-server raw log; when off, the diagnostic capture is a transient, immediately-unlinked temp file so the flag's contract is not widened |
| 70-A12 | 18-M8's `mcp_status_detail` notice | `src/host/host_runtime.cpp:178-195` | the live single-line notice uses only the first line of the bounded reason (70-D11), truncated on a UTF-8 boundary (70-I12); the full block is `/mcp`'s. The 18-M8 "only free-text reason surface" statement is narrowed to "the only *live-notice* free-text surface" |
| 70-A13 | 07 §6.4 (`ProcessRequest`) | `include/ymh/execution/process.hpp:30-45`; `src/execution/process.cpp:543-557,565` | add the defaulted `std::optional<int> stderr_fd`; when set, the child `dup2`s it onto fd 2 in the existing stderr block, **before** `close_inherited_fds()` (which closes non-CLOEXEC fds >2), so the capture survives regardless of the source fd's CLOEXEC |
| 70-A14 | 07 §6.4 (`ChildProcessHandle`) | `include/ymh/execution/process.hpp:76-92`; `src/execution/process.cpp:166-292` | add the non-blocking `std::optional<ProcessResult> tryReap()` handle method that sets the handle's internal `reaped_` flag **monotonically**, plus `bool reaped() const noexcept`; `close()`'s graceful loop and `failureContext()` call `tryReap()` (not the free `ymh::tryReap(int)`), and `SpawnedChild::wait()`'s free call is qualified |

## 3. Decision register

**70-D1 — Every MCP failure carries its own specific reason.** `McpManager` stores
`bound_mcp_reason(error.what())` at every failure site (`src/mcp/mcp_manager.cpp:290-291`,
`:343-344`, `:412`; the `std::exception` arms are bounded the same way), and
`statuses()` (`:479-486`) returns that bounded reason. When `what()` is empty, the
reason is the **non-bare** fallback `"mcp failure: " + to_string(error.code())` (not
the token alone). A bare enum token — whether an `McpErrorCode` token
(`"SpawnFailed"`) or a `ToolErrorCode` token (`"Io"`, via `to_string(ToolErrorCode)`
at `src/mcp/mcp_transport.cpp:171`) — is never the whole stored/shipped reason.
`McpErrorCode` and `McpDisconnectReason` values are unchanged. Scope: this decision
and 70-I1 cover the **server status channel** (`last_error`/`reason`), not the
per-tool-call `ToolResult.error` strings (`src/mcp/mcp_tool.cpp:57,68,72`), which
stay as today.

**70-D2 — Spawn and write failures preserve the OS error and the command.**
`StdioMcpTransport::start`'s `ToolError` catch composes
`"spawn failed: " + config_.command + ": " + error.what()`; `error.what()` already
carries `exec: <strerror(errno)>` from `LocalProcessService::spawn`
(`src/execution/process.cpp:595-596`). A missing executable therefore yields a
reason containing `No such file or directory` and the command, never `SpawnFailed`.
The `send` failure catch (`src/mcp/mcp_transport.cpp:170-172`) likewise composes
`"transport write failed: " + error.what()` instead of
`to_string(error.code())`.

**70-D3 — Bounded child-stderr capture (never `/dev/null`).** For a stdio server,
`StdioMcpTransport::start` arranges a diagnostic sink:
when `mcp.log_child_stderr` is on, it uses the durable
`<root>/.ymh/mcp/<sanitized-id>.stderr.log` via `ProcessRequest::stderr_path`
(existing 50-D3.5 behavior), recording the pre-spawn file size; otherwise it opens
an **anonymous** capture file in the parent (`O_TMPFILE | O_RDWR` where supported,
else `mkstemp` — which is `O_RDWR` — immediately followed by `unlink`; `O_RDONLY`
alone would make the child's `dup2` write-end unusable and `O_WRONLY` would make the
parent unable to read) and passes its fd through the new defaulted
`ProcessRequest::stderr_fd` (70-A13). The child `dup2`s that fd onto `fd 2`; the
parent keeps its own read fd. Because the child never reopens a path, there is no
fork/unlink race; the `mkstemp` variant has a sub-nanosecond named window before the
`unlink`, after which **no named temp file persists** — a crash cannot leak one. If
`start()` is called again on the same transport (the `refresh` reconnect path,
after the `if (child_ != nullptr) return;` early exit), it first closes any prior
`capture_fd_` — placed **after** that early exit so a redundant `start()` on a live
transport never closes the live child's capture (70-I13). The transport records the parent read
fd, the path (durable case), and the pre-spawn offset; only bytes at or after that
offset are read, and the read is **capped at the last ≤ 64 KiB** of the capture (a
`fstat`-sized window, like 69-D8's bounded scan). The transient fd shares one open
file description with the child (fork + `dup2`), so the parent MUST read it with
**`pread`** (positional), never `lseek`+`read`, or it would move the shared offset
out from under a live child; the durable path is a separate parent `open` and is
unaffected. The sink is selected **before** spawn: the durable `stderr_path` when
`log_child_stderr` is on, otherwise the anonymous capture fd. If the selected sink
cannot be created (the durable file open fails, or `O_TMPFILE`/`mkstemp` fails),
the code sets neither `stderr_path` nor `stderr_fd`, so the child's stderr degrades
to today's `/dev/null` (70-F8) and the reason is still specific. This is the
actionable cause for a server that spawns successfully and then exits (a missing
script argument, a bad interpreter path, a server banner on stderr).

**70-D4 — Exit status is captured when known, safely.** `failureContext()` and
`close()` both reap **through the handle** (`child_->tryReap()`, the new 70-A14
method), never via the free `tryReap(int)`; `tryReap()` records the `ProcessResult`
and sets the handle's internal `reaped_` flag **monotonically** (once true it is
never cleared by a later call). `close()` first consults `child_->reaped()` and,
when true, skips the SIGTERM path, the graceful loop, and the kill path, resetting
the handle without ever signalling. When false it runs SIGTERM, then its graceful
loop calls `child_->tryReap()` (so a child that exits inside the grace window sets
the handle's flag), then — only if still not reaped — SIGKILL. The post-SIGKILL
disposal does **not** call the free `reap(int)`; it either drops the reap and lets
`~SpawnedChild` reap (it already does, setting the flag) or drains via
`child_->tryReap()`. Because the flag is set by every reap path, both
`~SpawnedChild` and `close()` never signal a reaped (possibly recycled) pid. The exit
status, when known, leads the captured context.

**70-D5 — The disconnect reason is surfaced.** `DefaultMcpClient` retains the
`McpDisconnectReason` from `onClose` in an `std::optional<McpDisconnectReason>
disconnect_reason_` member and includes its token in the transport-closed reason
(`transport closed (server closed the connection (EOF))`), replacing
`(void)reason;`. A new total helper
`mcp_disconnect_token(McpDisconnectReason) -> std::string_view` (declared in
`mcp_types.hpp`, defined in `mcp_types.cpp`, alongside `mcp_state_token`) maps
`ClientClose`→`client closed`, `ServerEof`→`server closed the connection (EOF)`,
`SpawnFailed`→`spawn failed`, `ProtocolError`→`protocol error`,
`TransportError`→`transport I/O error`. When no close has been seen
(`disconnect_reason_ == nullopt`), the reason is `transport closed`.

**70-D6 — Handshake and tools errors include the server's own payload.** An
`initialize`/`tools/list` JSON-RPC error object contributes its `code` and
`message`; an unsupported revision contributes the received `protocolVersion`
string; a missing `result` contributes a bounded, redacted frame preview. This
turns `initialize was rejected` into
`initialize was rejected: 405 method not allowed` and `unsupported protocol
revision` into `unsupported protocol revision: 1999-01-01`.

**70-D7 — A single bound/redact choke point.** New total function
`bound_mcp_reason(std::string_view)` (declared in `mcp_types.hpp`, defined in
`mcp_types.cpp`) returns a bounded, redacted string:

- normalizes `\r\n` and a lone `\r` to `\n` **first**, then coerces the text to
  **valid UTF-8**: a byte that is not part of a valid UTF-8 sequence is replaced
  with `?`; NUL and other C0 control bytes except `\n` and `\t` are replaced with a
  space (this pass never sees `\r`, which was already normalized). This is mandatory
  because the reason is JSON-serialized and persisted (`event.payload.dump()` at
  `src/session/session_persistence.cpp:601,621`; `message.dump()` at
  `src/transport/protocol_server.cpp:673`), and nlohmann `dump()` throws
  `type_error.316` on invalid UTF-8 — an un-sanitized child-stderr byte would turn
  the diagnosability feature into a serialization crash;
- preserves newlines;
- splits into lines; drops empty lines; drops any line whose text
  (case-insensitively) contains a marker from the 69-D6 set (`api_key`, `api-key`,
  `apikey`, `authorization`, `bearer `, `secret`, `password`, `passwd`,
  `credential`, `token=`);
- truncates each surviving line to 240 bytes **on a UTF-8 codepoint boundary**
  (never inside a multibyte sequence);
- keeps at most 4 non-empty lines and at most 512 bytes total, on the same
  boundary rule;
- returns an empty string for empty input, and `failure reason withheld` when every
  line was dropped;
- never throws and is idempotent.

The manager applies it before storing `last_error`/creating the status event, so no
secret reaches the event log, the bus, the wire, or the UI.

**70-D8 — The composed reason is cause-first with labelled context.** The client
composes `"<specific cause>\nrecent server output:\n  <line>"` (no `" / "` join),
and the manager's 70-D7 bound may append nothing else. This mirrors 69-D9's
cause-leads/labelled-context shape without re-pinning it. The seam is wired with a
pinned caller: `DefaultMcpClient`'s constructor sets
`diagnostic_ = dynamic_cast<McpDiagnosticTransport*>(transport_.get())` (next to the
existing `pollable_ = dynamic_cast<McpPollableTransport*>(…)` at
`src/mcp/mcp_client.cpp:59`; `diagnostic_` is `nullptr` for transports without the
seam, e.g. the scripted test transport). The private helper
`std::string failure_suffix()` returns `""` when `diagnostic_ == nullptr` or its
`failureContext()` is empty, else the labelled block. It is appended in exactly two
places: `start()`'s `McpError` catch (wrapping the body that throws at
`src/mcp/mcp_client.cpp:95-116`) and `listTools()`'s `McpError` catch
(`:152-159`). The `sendRequest` closed/abandoned branches (`:369-377`) additionally
carry the 70-D5 disconnect token inside their message.

**70-D9 — `mcp.status` ships the bounded, redacted `reason` (additive).** The
`mcp.status` serializer emits `"reason": <string>` on a server object **iff** the
server is not connected and its `last_error` is non-empty. `has_error` is retained
unchanged. The raw `last_error`, `server_name`, and `server_version` remain
unshipped (45-I14 retained in that respect). `context.show`'s shared schema is
**unchanged** — it still ships only `{id,state,tool_count,skipped,has_error}`
(70-A9, 18-M8). Only `mcp.status` gains the field.

**70-D10 — `/mcp` renders the reason per server.** `format_mcp_block` appends, for a
server with a `reason`, a `\n    reason: <first line>` followed by each subsequent
line prefixed with 4 spaces (so the client's `recent server output:` label and its
2-space-indented tail align under the server row). Newlines inside the reason are
preserved. Connected/disabled servers render exactly as today.

**70-D11 — The live notice stays single-line, bounded, and UTF-8-safe.**
`mcp_status_detail` uses only the first line of the reason (the cause), and the
**composed detail is truncated on a UTF-8 codepoint boundary** to ≤256 bytes
(reusing the same boundary rule as 70-D7 — not a byte `resize()`), so the live
status-bar notice cannot grow multi-line, be truncated mid-codepoint, or make the
subsequent `HostNotice` `dump()` throw `type_error.316`. The full block is available
from `/mcp` (70-D10).

**70-D12 — No secret, environment, prompt, or credential leakage.** The reason
contains only: the server `id` (grammar-validated), the command, `strerror(errno)`,
the server's own JSON-RPC error text, the received revision, and the redacted child
stderr tail. The child environment is never read back or included; `resolve_mcp_env`
values are never logged (15-M12 retained); the 70-D7 marker filter drops
secret-shaped lines even when a server echoes its own environment.

## 4. C++ interfaces (additive)

```cpp
// include/ymh/mcp/mcp_transport.hpp — additive, mirrors McpPollableTransport.
class McpDiagnosticTransport {
public:
    virtual ~McpDiagnosticTransport() = default;
    // Bounded, redacted context for the most recent failure: the child's exit
    // status (when known) and a bounded tail of its captured stderr. Empty when
    // unavailable. Never contains secrets (70-D7). Never throws.
    [[nodiscard]] virtual std::string failureContext() = 0;
};
// StdioMcpTransport : public McpTransport, public McpPollableTransport,
//                     public McpDiagnosticTransport
```

```cpp
// include/ymh/mcp/mcp_types.hpp — the 70-D7 choke point.
[[nodiscard]] std::string bound_mcp_reason(std::string_view reason);

// include/ymh/mcp/mcp_types.hpp — the 70-D5 total token map (mirrors
// mcp_state_token at :56).
[[nodiscard]] std::string_view mcp_disconnect_token(McpDisconnectReason reason) noexcept;

// include/ymh/mcp/mcp_types.hpp — the 70-D7/70-D11 UTF-8-safe truncation used by
// bound_mcp_reason and by mcp_status_detail (host_runtime.cpp). Returns a prefix
// of `text` of at most `max_bytes` bytes, never splitting a UTF-8 codepoint;
// invalid bytes are treated as one byte. Callers: bound_mcp_reason
// (src/mcp/mcp_types.cpp) and mcp_status_detail (src/host/host_runtime.cpp:178-195).
[[nodiscard]] std::string mcp_truncate_utf8(std::string_view text, std::size_t max_bytes);
```

New `StdioMcpTransport` private state (lifetime in §7):
`std::filesystem::path capture_path_; int capture_fd_{-1};
std::uintmax_t capture_offset_{0};`. The transient/durable distinction is
`capture_path_.empty()` (empty ⇔ transient/anonymous), and the reaped/exit-status
indications come from the handle (`child_->reaped()` and the on-demand
`child_->tryReap()` in `failureContext()`), so no separate `transient_capture_`,
`reaped_`, or `exit_status_` members are stored.

`DefaultMcpClient` gains two private members: `McpDiagnosticTransport* diagnostic_
= nullptr;` (set in the constructor) and
`std::optional<McpDisconnectReason> disconnect_reason_;` (set in `onClose`), plus
the private helper `std::string failure_suffix()`.

```cpp
// include/ymh/execution/process.hpp — additive to the 07 §6.4 seam.
struct ProcessRequest {
    // ...
    std::optional<int> stderr_fd;   // spawn()-only; when set, the child dup2s it
                                    // onto fd 2 (takes precedence over stderr_path)
};
class ChildProcessHandle {
    // ...
    // 70-A14: non-blocking reap that also sets the handle's internal reaped_ flag,
    // so close()/~SpawnedChild never signal a reaped pid.
    virtual std::optional<ProcessResult> tryReap() = 0;
    [[nodiscard]] virtual bool reaped() const noexcept = 0;
};
```

## 5. Per-path reason table

| Path | State | Reason source | Example reason text |
|---|---|---|---|
| `execvp` fails (missing binary) | `Failed` | `ToolError::what()` + command | `spawn failed: /opt/mcp: exec: No such file or directory` |
| Child spawns then exits before handshake | `Failed` | disconnect reason + exit status + stderr tail | `transport closed (server closed the connection (EOF))` / `recent server output:` / `server exited with code 2` / `python3: can't open file '/tools/mcp-hub/src/x.py': [Errno 2] No such file or directory` |
| `initialize` times out | `Failed` | timeout + child context | `initialize timed out (handshake_timeout=200ms)` + context |
| `initialize` returns an error | `Failed` | RPC `error.code`/`message` | `initialize was rejected: -32601 method not found` |
| Unsupported revision | `Failed` | received revision | `unsupported protocol revision: 1999-01-01` |
| `initialize` result missing | `Failed` | bounded frame preview | `initialize had no result: {...}` |
| Request sent after the transport closed | `Failed` | disconnect token (70-D5) | `transport closed (server closed the connection (EOF))` |
| Transport write fails | `Failed` | `ToolError::what()` (70-D2) | `transport write failed: write stdin: Broken pipe` |
| `tools/list` error | `Failed`/`Degraded` | RPC `error.code`/`message` | `tools/list error: -32000 server busy` |
| `max_servers` reached | `Failed` | existing | `max_servers reached` |
| Child stderr contains a secret | any | dropped by 70-D7 | (line omitted; if all lines, `failure reason withheld`) |

## 6. Invariants

| ID | Invariant |
|---|---|
| **70-I1** | No MCP failure stores or emits a bare enum token — whether an `McpErrorCode` token or a `ToolErrorCode` token — as the whole `last_error`/`reason`; every failure names a cause (70-D1). |
| **70-I2** | A spawn failure's reason contains the OS error text (`strerror`) and the command; a missing executable yields text containing `No such file or directory` (70-D2). |
| **70-I3** | When a stdio transport is active and its selected sink (the durable `stderr_path` when `log_child_stderr` is on, else the anonymous capture fd) could be created, the child's stderr is never routed to `/dev/null`; a bounded tail is available to `failureContext()` on failure. When the selected sink cannot be created (70-F8), neither `stderr_path` nor `stderr_fd` is set and the child's stderr degrades to `/dev/null`, and the reason is still specific. |
| **70-I4** | The stored `reason` is ≤4 non-empty lines / ≤512 bytes total / ≤240 bytes per line and contains no line matching the 70-D7 marker set (70-D7, 70-D12). |
| **70-I5** | `mcp.status`'s `reason` is present iff `!connected` and a cause exists; ready and disabled servers ship no `reason`; `has_error` is unchanged (70-D9). |
| **70-I6** | `context.show`'s `mcp_servers` schema is byte-for-byte unchanged: only `{id,state,tool_count,skipped,has_error}` and no reason text (70-A9, 18-M8). |
| **70-I7** | `format_mcp_block` preserves every `'\n'` inside a server's reason; no join with `" / "` (70-D10, 69-I10 by reference). |
| **70-I8** | No environment value, token, header, credential, or prompt text appears in any reason or context; the marker filter is applied before storage (70-D7, 70-D12). |
| **70-I9** | Every reap path — `failureContext()`, `close()`'s graceful loop, `close()`'s post-SIGKILL disposal, and `~SpawnedChild` — goes through `ChildProcessHandle::tryReap()`/`reaped()` or a handle-owned wait (whose flag is monotonic); the free `ymh::tryReap(int)`/`ymh::reap(int)` is never used by the transport, so no reaped pid is ever signalled (70-D4, 70-I15). |
| **70-I10** | `bound_mcp_reason` is total (never throws), preserves newlines, and is idempotent (70-D7). |
| **70-I11** | The `McpErrorCode`, `McpDisconnectReason`, `HostExitCode`, and `max_servers` numeric/token contracts are unchanged (70-D1). |
| **70-I12** | Every string `bound_mcp_reason` returns, and every `mcp_status_detail` detail, is valid UTF-8 (invalid bytes replaced, control bytes normalized) so `event.payload.dump()` / `message.dump()` never throw `type_error.316`; truncation never splits a codepoint (70-D7, 70-D11). |
| **70-I13** | The `failureContext()` read is bounded to the last ≤64 KiB of the capture, never the whole file, and uses `pread` on a transient fd that shares its open file description with the child; a re-invoked `start()` closes any prior `capture_fd_` so a reconnect cannot leak fds (70-D3). |
| **70-I14** | The transient capture is passed to the child as an already-open fd (`stderr_fd`, `O_RDWR`); no path is reopened after fork, so there is no fork/unlink race and no named temp file persists (70-D3, 70-A13). |
| **70-I15** | The child exit status is reaped through `ChildProcessHandle::tryReap()`, which sets the handle's own `reaped_` flag, and `close()`/`~SpawnedChild` consult it so a reaped pid is never signalled (70-D4, 70-A14). |

## 7. State lifetime

| State | Created | Destroyed / evicted | Owner | Survives restart? | Crash path |
|---|---|---|---|---|---|
| `capture_fd_` (transient, `log_child_stderr` off) | `StdioMcpTransport::start` (`O_TMPFILE\|O_RDWR`, else `mkstemp`+`unlink`), passed to the child via `stderr_fd` | closed when `start()` re-runs (the `refresh` reconnect path) and in `~StdioMcpTransport`; the inode is reclaimed then (and by the kernel on any crash) | the transport | no | **no named temp file persists**: the file is anonymous from `O_TMPFILE` (or `unlink`ed immediately), and even a crash leaves only an anonymous inode reclaimed at process death |
| `capture_path_`/`capture_fd_` (durable `<root>/.ymh/mcp/<id>.stderr.log`, `log_child_stderr` on) | `start` via `stderr_path`; the parent also keeps a read fd | the parent fd is closed when `start()` re-runs (the `refresh` reconnect path) and in `~StdioMcpTransport`; the named log itself is never removed (append-only, 50-D3.5) | the workspace | yes | durable by design; redaction is applied only to the surfaced tail, never to the durable raw log |
| `capture_offset_` (+ the handle's reaped flag / on-demand exit status) | `start` / `failureContext` | transport destruction | the transport / the child handle | no | in-memory only |
| `disconnect_reason_` | `onClose` | client destruction | the client | no | in-memory only |
| `slot.last_error` / status event `reason` | `McpManager::setStateLocked` (bounded by 70-D7 **before** the event is created) | overwritten on the next state change; lost on daemon exit | the manager | no | the bounded text is durable insofar as the event log persists it (session.cpp routes `McpServerStatusChanged` through the standard event path); it is already redacted |
| `mcp.status` result `reason` | per `mcp.status` call | per reply | the daemon | n/a | derived on demand |

## 8. Failure modes (`70-F#`)

Component-local to this errata; they refine `15 §9.2` MCP-F1/F2/F3/F7 and add
the MCP instance of `69-F1`/`69-F2`.

| 70-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **70-F1** | Spawn fails (`ENOENT`/`EACCES`/`ENOEXEC`) | `ToolError` from `spawn()` | reason `spawn failed: <cmd>: exec: <strerror>`; server `Failed` (70-I2) |
| **70-F2** | Child exits before handshake | stdout EOF, `onClose(ServerEof)` | reason `transport closed (server closed the connection (EOF))` + `recent server output:` + exit status + redacted stderr tail (70-D3/70-D4/70-D8) |
| **70-F3** | Handshake timeout | `CallTimeout` inside `initialize` | reason `initialize timed out (handshake_timeout=<n>ms)` + child context (70-D6) |
| **70-F4** | Unsupported revision | `is_supported_mcp_revision` false | reason `unsupported protocol revision: <received>` (70-D6) |
| **70-F5** | `initialize` rejected | `response["error"]` present | reason `initialize was rejected: <code> <message>` (70-D6) |
| **70-F6** | `tools/list` error | `response["error"]` present | reason `tools/list error: <code> <message>` (70-D6) |
| **70-F7** | Secret-shaped child output | 70-D7 marker match | the line is dropped; the rest survives; all-dropped ⇒ `failure reason withheld` (70-I4, 70-I8) |
| **70-F8** | Capture fd uncreatable | `O_TMPFILE`/`mkstemp` fails (anonymous mode), or the durable file open fails (`log_child_stderr` on) | the transport sets neither `stderr_path` nor `stderr_fd`, so the child's stderr degrades to `/dev/null` (today's behavior) and the reason is still specific; a warning is logged; no throw (70-I3 degraded) |
| **70-F9** | Child reaped by `failureContext()` / the graceful loop | `tryReap` returns a result | `close()` short-circuits signalling on `reaped()`, and the post-SIGKILL disposal leaves the reap to `~SpawnedChild`; no free `reap(int)` in the transport (70-D4, 70-I9) |

## 9. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification |
|---|---|---|
| MCP server spawn/init error propagation | `McpDiagnosticTransport::failureContext()` + 70-D2/70-D3/70-D6 | dsh surfaces a stdio MCP server's captured stderr and exit status alongside the typed init error; ymh mirrors the *capability* (captured child stderr + exit status + typed cause) through its existing `stderr_path` seam, not the exact mechanism. Anchor: `07 §6.4` (`ProcessRequest`), `15 §4.2` (the additive-seam precedent). |
| "do not leak secrets in errors" | 70-D7 bound/redaction + 70-D12 | Non-mirror: dsh has no explicit MCP redaction set; ymh adds one because a server may echo its own environment and the reason is persisted in the session event log. Anchor: `AGENTS.md` logging rule; 69-D6; `15-M12`. |
| human-readable per-server failure | 70-D9 wire `reason` + 70-D10 per-server render | dsh renders a typed error in its MCP list; ymh pins the *presentation* (cause first, labelled bounded context, newlines preserved) because its `/mcp` block is text. Anchor: 45-D6.9, 69-D9/69-D10. |
| MCP disconnect cause | 70-D5 disconnect token | dsh distinguishes transport-vs-protocol-vs-EOF closes; ymh already has `McpDisconnectReason` but discarded it (`(void)reason;`). Anchor: `include/ymh/mcp/mcp_types.hpp:111-117`. |

## 10. Test plan

Hermetic (no real LLM, no network; real stdio via the fake server where noted):

| Test | Setup | Assertion |
|---|---|---|
| `McpDiagnosability.BoundReasonRedactsAndBounds` (unit) | `bound_mcp_reason` with a multi-line input containing an `api_key=` line and 300-byte lines | the secret line is gone; ≤4 lines / ≤512 bytes; newlines preserved; idempotent (70-D7, 70-I4, 70-I10) |
| `ProcessService.TryReapIsMonotonic` (unit, `tests/unit/process_test.cpp:310`) | a `spawn()`ed child that exits immediately; `handle->tryReap()` until it returns a result, then again | the first call returns the status and `handle->reaped() == true`; a subsequent `tryReap()` returns `nullopt` and `reaped()` stays `true` (70-A14, 70-I15) |
| `McpDiagnosability.BoundReasonStaysValidUtf8` (unit) | `bound_mcp_reason` with a lone `0xFF` byte, a truncated multibyte sequence, a NUL, and a 240-byte multibyte line | the result is valid UTF-8, contains no NUL/control byte, and `nlohmann::json({{"reason", out}}).dump()` does not throw (70-I12) |
| `McpDiagnosability.TruncateUtf8NeverSplitsCodepoint` (unit) | `mcp_truncate_utf8` over a multibyte string at every byte length boundary | the result is always valid UTF-8 and never longer than the requested cap (70-D11, 70-I12) |
| `McpLiveStatusTest.NoticeDetailWithLongMultibyteReasonDumps` (unit, `tests/unit/mcp_live_status_test.cpp:89`) | the primitive `ymh::mcp_truncate_utf8` (the one `mcp_status_detail` composes with) applied to a >256-byte multibyte reason; the test then serializes a `HostNotice` carrying the bounded detail | the result is ≤256 bytes on a codepoint boundary, `nlohmann::json(notice).dump()` does not throw, and the `detail` is valid UTF-8 (70-D11, 70-I12). The host wrapper `mcp_status_detail` itself is not invoked (it is translation-unit local); this pins the shared truncation primitive. |
| `McpClientTest.HandshakeAndToolListFailuresNameTheirCause` (unit, `tests/unit/mcp_client_test.cpp`) | the scripted transport returns an `initialize` JSON-RPC error object in one harness and a `tools/list` error object in another | `start()` throws `HandshakeRejected` whose `what()` contains `initialize was rejected: -32602 bad params`; `listTools()` throws `RpcError` whose `what()` contains `tools/list error: -32000 listing boom` (70-D6, 70-I2) |
| `McpDiagnosability.ManagerPreservesSpecificReason` (unit, `mcp_manager_test.cpp`) | a factory client whose `start()` throws `McpError{SpawnFailed, "spawn failed: /opt/mcp: exec: No such file or directory"}` | `statuses().front().last_error` contains `No such file or directory` and is **not** the bare `SpawnFailed` (70-D1, 70-I1) |
| `McpDiagnosability.DistinctServersYieldDistinctReasons` (unit) | two servers, two factories throwing different reasons | the two `last_error` strings differ and each contains its own cause (70-D1) |
| `McpDiagnosability.ManagerRedactsSecretReason` (unit) | factory throws a reason containing a `token=` line | stored `last_error` does not contain the secret (70-D7, 70-I8) |
| `McpDiagnosticIntegration.MissingCommandReasonIsSpecific` (integration, `tests/integration_mcp_test.cpp:289`) | `command = "/nonexistent/ymh-mcp-does-not-exist"` | `status().state == Failed`; `last_error` contains `No such file or directory` and the command (70-D2, 70-I2) |
| `McpDiagnosticIntegration.ChildExitStderrIsSurfaced` (integration, `:316`) | fake server **extended** with a scenario `exit_stderr` that writes a marker to stderr and `_exit(2)` on `initialize` (`tests/support/fake_mcp_server.cpp:85-90`) | `last_error` contains the marker and the exit code (70-D3, 70-D4, 70-F2) |
| `McpDiagnosticIntegration.BadRevisionReasonNamesRevision` (integration, `:332`) | fake server scenario `bad_revision` | `last_error` contains `1999-01-01` (70-D6, 70-F4) |
| `McpDiagnosability.RenderShowsReasonPerServer` (unit, `status_format_test.cpp`) | `format_mcp_block` with a failed server carrying a multi-line `reason` | the block contains `reason: <cause>` and the context line; newlines preserved; a ready server has no reason row (70-D10, 70-I7) |
| `McpRpcTest.UI45_D6_FailedServerShipsReason` (unit, `mcp_rpc_test.cpp`) | a fixture whose client fails with a specific reason | `mcp.status` server object contains `reason` with the cause; a ready server object has no `reason`; the raw `last_error` key is still absent (70-D9, 70-I5) |
| existing `ContextSnapshot.McpServerListIsBoundedAndRedacted` | unchanged | still passes: `context.show` does not ship the text (70-I6) |

Pre-fix failure evidence: every new test above is added **before** the
implementation and demonstrated to fail on the unmodified tree (the manager stores
`SpawnFailed`/`HandshakeTimeout` tokens; `mcp_status_json` has no `reason`;
`format_mcp_block` has no reason row; `bound_mcp_reason` does not exist). Evidence
is captured in the change commit.

## 11. Open questions

- **OQ-70.1**: should a *successful* server's captured stderr tail ever surface
  (e.g. a warning banner)? **Decision: no** — 70-D3 surfaces it only on failure, to
  avoid growing the success notice and to keep the redaction surface minimal. A
  future "MCP server log" surface is a separate additive spec.
- **OQ-70.2**: should `context.show` eventually gain the reason? **Decision: no**
  — it is the model-facing schema and 18-M8 deliberately omits diagnostic text;
  changing it needs its own spec (70-A9).
