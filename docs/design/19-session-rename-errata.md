# 19 — Session Auto-Naming & Rename Errata (RB-03)

```
Status: written · verified: — · reviewer: —
Component: 19 (errata) — amends 01-session.md and 05-transport.md by reference only
Depends on: 01-session.md (verified), 05-transport.md (verified),
            10-supervisor-tui.md (verified), 17-ui-transcript-errata.md
            (verified; RB-08 Tab completion, RB-10 top-line title),
            16-daemon-ownership.md (verified; cross-supervisor visibility),
            REQUIREMENTS_BACKLOG.md (RB-03)
Scope: auto-name + rename of an existing session, on the live
       `ui::run_supervisor` path, the `ymh run` headless path, and the daemon
       wire that both share
```

## 1. Purpose, scope, and precedence

This errata amends the **verified** specs `01-session.md` (session + event log)
and `05-transport.md` (wire) for RB-03: *"Rename sessions after they started to
something meaningful and short."* It is **additive**: it pins new text,
interface names, invariants, and tests, and introduces no new semantics beyond
(a) one new durable event, (b) one new wire method, (c) one supervisor-local
slash command, and (d) a daemon-side heuristic auto-name. It does not rewrite
either spec; each item names the exact §/line it extends.

**Live vs. dead path (pinned).** `ui::run_supervisor` (`src/ui/supervisor.cpp`)
is live (spec 17 §1). `ui::run_tui` (`src/ui/ui_application.cpp`) is **dead
code** — no caller in `src/` (spec 17 §1 `:19-23`). All UI items change the live
path only. `src/ui/ui_render.cpp` and `src/ui/ui_model.cpp` are shared by both
paths and by the golden/PTY tests, so every renderer/model change is also what
the tests see (17 §1).

**Precedence (pinned).** This errata wins over 01 and 05 for the items in the
register below. Where it meets spec 16 on cross-supervisor visibility, **16
wins** (§6). Spec 17 is **untouched**: RB-08's completion algorithm and RB-10's
right-aligned title slot are reused as-is; `/rename` is merely a new registry
entry that RB-08 picks up automatically, and rename delivery reuses RB-10's
`setCellTitle` path.

**Gate (per `AGENTS.md`).** Independent Oracle review must mark this `verified`
with no open HIGH/MEDIUM findings **before any code**. Nothing here is verified.

**Honesty note — recon vs. code (round 2).** The first gate round FAILED on two
HIGH findings: (1) systematically stale `src/ui/supervisor.cpp` anchors, because
unrelated RB-07 (`/export`) work landed in that file and shifted every line; and
(2) a false fork-session reconciliation claim. Both are fixed:

- **All anchors were re-derived from the current tree.** The stale
  `supervisor.cpp` numbers were corrected (§3, §6); the shift is real and the
  other files were re-checked and were unchanged. See the Citation audit (§15).
- **The fork divergence is fixed** by resolving the title over the session's
  **own** log (`Session::ownEvents()`, `src/session/session.cpp:391-400`), not
  the resolved view (`events()`, `:387-389`), which for a fork is
  parent-prefix ++ own (`SessionPersistence::resolve_after_locked`,
  `src/session/session_persistence.cpp:342-382`). See §5.2 and RN2–RN4.
- RB-03's backlog text cites `src/ui/supervisor.cpp:347` for the `"tui"`
  placeholder and `src/ui/ui_render.cpp:219` for the session-bar title. Those
  are stale; in the current tree the `"tui"` create parameter is at
  `src/ui/supervisor.cpp:719` and the title is rendered in the **header** at
  `src/ui/ui_render.cpp:461-484` (title value at `:467-483`). The behaviour is
  correct; only the anchors were stale.
- RB-03's grep claim ("no `SessionRenamed`/`rename`/`set_title`") is correct for
  `src/` + `include/` in the sense that no code path defines, parses, or handles
  a rename. The only hits are **unrelated code/comment hits and docs** — the
  `GIT_STATUS_*_RENAMED` status letters (`src/execution/git_service.cpp:118,127`),
  the `kWireNames` table (`src/core/event.cpp:18,44,53,62-65`), the "must not
  rename the pinned types" comment (`include/ymh/agent/message.hpp:13`), and the
  design docs. Re-verified by grep (R2-06).
- **Request serialization (now verified).** 05 §7.1
  (`docs/design/05-transport.md:779-781`) claims mutating methods serialize
  through the daemon's single loop. Re-derived from the current tree:
  `TransportServer::start` spawns exactly **one** runner thread that calls
  `io_->run()` (`src/transport/transport_server.cpp:257-266`); socket reads
  dispatch synchronously inside that thread's `async_read_some` handler
  (`:65-83`); `ProtocolServer::dispatch_request` calls `handle_method`
  synchronously (`src/transport/protocol_server.cpp:220-253,351`); and
  `WorkspaceHost` shares its one `io_` with the transport without running it
  anywhere else (`src/host/workspace_host.cpp:349,482,558-561`). Request
  **handling** is single-threaded; turn **bodies** are not (they run on
  `TurnExecutor` workers). §5.3 pins the auto-name check under `appendMutex_`
  so it is race-free regardless of that serialization.

**Round-2 gate fixes (R2-01…R2-06, C2).** The second gate round left six
findings plus one arbitrated change. All are applied here; none undo a round-1
fix:

- **R2-01:** §5.2's residual-window text no longer implies `reload()` repairs
  the persisted `sessions.title` column. It now says reload fixes only the
  in-memory header, the column is never repaired, and RN3b is the sole
  guarantee. §5.2, invariant R-F10 and the §12.3 test now say the same thing.
- **R2-02:** §5.3/§7.2 no longer read `ownEvents()`/`header()` unsynchronized
  from the io thread. The check and the append are one critical section on
  `appendMutex_` (`Session::appendAutoRename`).
- **R2-03:** §4.3 pins `first_text_of` to the **first** `Text` content block of
  the `message_from_json` result.
- **R2-04:** §5.4 pins the echoed result title to `HostRuntime`'s own
  `normalize_title` output; `SessionManager::renameSession` still returns
  `Sequence`.
- **R2-05:** §11's decision numbering is made contiguous (decision 19-D13
  added).
- **R2-06:** §1 and §3's grep claim now lists the unrelated code/comment hits.
- **C2:** §5.4 pins the method-catalog size **relatively** (`29 + N`, mirroring
  18 §3.4.1), never as an absolute `30`.

**Concurrent-work note.** `src/ui/supervisor.cpp`, `src/ui/command_registry.cpp`,
and `include/ymh/ui/command_registry.hpp` were changed by unrelated RB-07
(`/export`) work, now committed as `38c8a6214`. Every anchor in this spec is
against the tree **with** that commit, so it is stable at that revision; the next
gate round must re-run the Citation audit (§15) if those files change again.

## 2. Amendment register

| ID | Amended clause | Verified code anchors | New behaviour |
|---|---|---|---|
| RN-A1 | 01 §4.3 `EventType` | `include/ymh/core/event.hpp:51-74`; `src/core/event.cpp:18-39,61-71` | add `SessionRenamed` (wire `session/renamed`) |
| RN-A2 | 01 §4.4 `SessionEventMap` | `include/ymh/session/events.hpp:197-350` | add the payload ↔ type specialization |
| RN-A3 | 01 §4.5 payloads | `include/ymh/session/events.hpp:26-30,353-360`; `src/session/events.cpp:19-21,114-138,190-202,212-222` | add `payload::SessionRenamed` + `payload::RenameOrigin` |
| RN-A4 | 01 §3 header title semantics | `include/ymh/session/session.hpp:49`; `src/session/session.cpp:128-171` | title becomes a materialized projection of the session's **own** log (no new field) |
| RN-A5 | 01 §6.1 append / 01 §7 store seam | `src/session/session.cpp:370-400,406-426`; `src/session/session_persistence.cpp:457-519,703-742` | append materializes `sessions.title` in the same transaction; no new virtual |
| RN-A6 | 01 §8 `SessionManager` | `include/ymh/session/session_manager.hpp:34-66`; `src/session/session_manager.cpp:45-141` | add `renameSession`, `maybeAutoName` |
| RN-A7 | 01 §12 invariants / §13 failure modes / §15 tests | `docs/design/01-session.md:1092-1250,1286-1494` | add RN1–RN15 (incl. RN3b), R-F1–R-F12, tests |
| RN-A8 | 05 §2.2 error codes | `include/ymh/transport/protocol.hpp:169-190` | **no new code** (reuse `InvalidParams`, `UnknownSession`, `LeaseLost`, `StoreUnavailable`) |
| RN-A9 | 05 §6.3 profile gating / §7.4 lifecycle | `src/transport/protocol.cpp:641-647`; `docs/design/05-transport.md:883-926` | add `session.rename` (allowed in both profiles) |
| RN-A10 | 05 §5.1 core event / §7.7 streaming | `src/core/event.cpp:73-99`; `docs/design/05-transport.md:997-1077` | the new event rides `event.stream` unchanged |
| RN-A11 | 10 §8.1/§8.2 + 17 §6 (RB-10) | `src/ui/ui_event.hpp:193-214`; `src/ui/ui_model.cpp:413-426,468-483`; `src/ui/ui_render.cpp:461-484` | add `UiEvent::SessionTitleChanged` → `setCellTitle` |
| RN-A12 | 10 §8.2 + 17 §5 (RB-08) | `include/ymh/ui/command_registry.hpp:15-27`; `src/ui/command_registry.cpp:113-196`; `src/ui/supervisor.cpp:901-918,942-981` | add `/rename`; Tab completion and hints pick it up |
| RN-A13 | 04 host runtime | `src/host/host_runtime.cpp:441-489,601-618`; `include/ymh/host/host_runtime.hpp:155-165`; `include/ymh/transport/host.hpp:48-56,78` | add `renameSession` (+ `protocol::SessionRenamedResult`); call `maybeAutoName` from `agentPrompt` |
| RN-A14 | 01 §6.1 `Session::append*` API | `include/ymh/session/session.hpp:186-200,226`; `src/session/session.cpp:406-455` | add `Session::appendAutoRename` (RN5/RN6 check + append in one `appendMutex_` critical section) and split out `appendEventLocked` (R2-02) |

**Untouched (pinned).** 01 §5 (`EventBus`), §6.2 (read side), §6.3
(`deriveMessages` *semantics* — a rename is not a message source), §9.2–§9.5
(create/resume/fork/replay), §10 (lease), §11 (concurrency); 05 §3 (framing),
§4 (handshake), §5.2–§5.5, §8 (multiplexing), §9 (threading). `kSchemaVersion`
(`include/ymh/session/session_persistence.hpp:68`) and `kProtocolVersion`
(`include/ymh/transport/protocol.hpp:153`) are **not** bumped (§4.6).

---

## 3. Verified current state (before this errata)

Every claim here is a `file:line` citation from the working tree.

**The `title` field exists end-to-end.**

- `SessionHeader.title` — `include/ymh/session/session.hpp:49`; pinned by
  01 §3 (`docs/design/01-session.md:166`).
- `SessionOptions.title` — `include/ymh/session/session_manager.hpp:31`.
- DDL: `title TEXT NOT NULL DEFAULT ''` —
  `src/session/session_persistence.cpp:41`.
- Create binds it — `src/session/session_persistence.cpp:713-719` (`:719`);
  read maps it — `:298`.
- `SessionManager::createSession` copies `options.title` into the header
  (`src/session/session_manager.cpp:53`) and into the `SessionStarted` payload
  (`:62-66`).
- `SessionHeader` JSON round-trips `title` — `src/session/session.cpp:134,151`.
- On the wire it is carried as `SessionSummary.title`
  (`include/ymh/transport/protocol.hpp:359`), populated from `header.title`
  (`src/host/host_runtime.cpp:143`), served by `session.list`
  (`src/transport/protocol_server.cpp:387-392`), and read by the supervisor
  (`src/ui/supervisor.cpp:668`) into `SessionCell.title`
  (`src/ui/ui_model.hpp:219`) via `UiModel::setCellTitle`
  (`src/ui/ui_model.cpp:413-426`, called at `src/ui/supervisor.cpp:688`).
- Rendered right-aligned in the header — `src/ui/ui_render.cpp:461-484`
  (`session_title` at `:467-483`, fallback `short_id` at `:478`); the switcher
  leaf uses `session.title` at `src/ui/ui_render.cpp:438-440`.

**A heuristic auto-name exists for the headless path only.**

- `src/cli/headless.cpp:159`:
  `session_options.title = first_line(options.task, 60);`
- Helper `first_line(const std::string&, std::size_t)` —
  `src/cli/headless.cpp:45-56` (first line; byte-truncate + `"..."` at `:52-54`).

**The TUI sets a fixed placeholder title.**

- `src/ui/supervisor.cpp:719`: `nlohmann::json create_params{{"title", "tui"}};`
- Dead path: `src/ui/ui_application.cpp:476`: `session_options.title = "main";`.
- The daemon accepts a `title` param — `src/host/host_runtime.cpp:452`.
- Forks are created with `child.title = ""` — `src/session/session.cpp:499`.

**There is no rename event and no rename method.**

- `EventType` has no `SessionRenamed` — `include/ymh/core/event.hpp:51-74`.
- The wire-name table has no `session/renamed` — `src/core/event.cpp:18-39`.
- The method catalog has no `session.rename` —
  `include/ymh/transport/protocol.hpp:476-506`;
  `src/transport/protocol.cpp:612-624`.
- Grep for `SessionRenamed|rename|set_title` in `src/` + `include/` returns
  only unrelated code/comment hits: the `GIT_STATUS_*_RENAMED` status letters
  (`src/execution/git_service.cpp:118,127`), the `kWireNames` table
  (`src/core/event.cpp:18,44,53,62-65`), and the "must not rename the pinned
  types" comment (`include/ymh/agent/message.hpp:13`). No code path defines or
  handles a rename, and no code path mutates `header_.title` after create
  (R2-06).
- `append_batch_locked` advances **only** `updated_at`
  (`src/session/session_persistence.cpp:491-494`); `Session::appendEvent`
  advances **only** `header_.updatedAt` (`src/session/session.cpp:417`).
  `header_.title` is never reassigned after construction.
- **The read path distinguishes the resolved view from the own log.**
  `Session::reload` sets `log_ = store_->read(id)` (`src/session/session.cpp:371`),
  and for a fork the store resolves **parent-prefix ++ own**
  (`SessionPersistence::resolve_after_locked`,
  `src/session/session_persistence.cpp:342-382`, parent branch `:355-375`).
  `Session::events()` returns that resolved view (`:387-389`), while
  `Session::ownEvents()` slices off the parent prefix using
  `header_.seedLength` (`:391-400`). Any per-session metadata must be resolved
  over `ownEvents()`, never `events()`, or a fork will appear to inherit it.

**Exhaustive switches that a new `EventType` must satisfy (compile-time).**

- `deriveMessages` — `src/session/session.cpp:235-342`; the `switch` has **no
  `default`**, so a new enumerator is a `-Wswitch` error under
  `-Werror` unless a `case` is added.
- `wire_type` — `src/session/session_persistence.cpp:224`.
- `kWireNames` — `src/core/event.cpp:18-39` (fixed-size array; adding an entry
  requires bumping the `20`).
- The exhaustive wire round-trip test — `tests/unit/event_test.cpp:49-59`
  iterates `all_event_types()`.

The UI adapter switches are **not** exhaustive: `project_state`'s switch ends
`default: return old;` (`src/ui/ui_event_adapter.cpp:75-76`) and `adapt`'s ends
`default: break;` (`:153-154`), so an unhandled `SessionRenamed` would be
silently dropped — the errata adds an explicit case (§5.3).

---

## 4. Auto-naming design (decision 19-D1: **heuristic**, not LLM-derived)

### 4.1 Options considered

| Option | Determinism | Cost / latency | Offline / FakeLLM | Failure surface | Verdict |
|---|---|---|---|---|---|
| **Heuristic** (first user text, like headless `first_line`) | total | zero | total | none (pure function) | **chosen** |
| **LLM-derived** (reuse `CompactionPolicy::summarizer_model`, spec 13) | none | one extra provider call per session; on the critical path or a background turn | needs scripted `FakeLLM`; breaks hermetic determinism | provider down / rate-limited / empty output / non-title output; races with a user rename | rejected (§4.5) |

**Why heuristic wins.** RB-03's requirement is "meaningful and short". The
first user turn *is* the most meaningful short description available at zero
cost, and the existing headless path already ships this exact idea
(`src/cli/headless.cpp:159`). A heuristic is a pure function of the prompt, so
it is deterministic (RN7), works under `FakeLLM` with no scripting, adds no
provider latency, and has no failure mode. The auto-name is **advisory**: a user
can always override it with `/rename`, which is the escape hatch for a poor
guess. An LLM-derived name would be *better* on average but strictly worse on
every engineering axis the project gates on (determinism, cost, offline, test
surface), and it would introduce a rename race (§4.5).

### 4.2 The heuristic (pinned)

A pure helper in the session layer (declared in
`include/ymh/session/session.hpp`, defined in `src/session/session.cpp`):

```cpp
namespace ymh {

// 19 §4.2: the auto-name byte budget (content only; an appended "..." may
// exceed it by 3 bytes) and the hard cap every rename-path title obeys (RN8).
// Create-time SessionStarted.title is not validated by this errata (RN8).
inline constexpr std::size_t kAutoTitleBytes     = 60;
inline constexpr std::size_t kMaxSessionTitleBytes = 120;

// 19 §4.2: a title the daemon may auto-replace. `"tui"` is the live supervisor
// placeholder (src/ui/supervisor.cpp:719); `"main"` is the dead `run_tui`
// placeholder (src/ui/ui_application.cpp:476, spec 17 §1); `""` is the fork
// default (src/session/session.cpp:499).
[[nodiscard]] bool is_placeholder_title(std::string_view title) noexcept;

// 19 §4.2: pure. Returns the normalized auto-title, or nullopt when the prompt
// yields no usable text (RN7: no clock, no I/O, no randomness).
[[nodiscard]] std::optional<std::string> derive_auto_title(std::string_view prompt);

// 19 §4.2: user-supplied titles. The single validator for the wire and the
// auto path (RN8). Trims the R-F4 whitespace set (space/tab/CR/FF/VT -- NOT
// '\n'), rejects empty-after-trim, any remaining C0 control byte or DEL,
// non-RFC-3629 UTF-8, and > kMaxSessionTitleBytes; throws
// std::invalid_argument (mirrors validateHeader). Returns the normalized
// title. The exact contract is pinned below.
[[nodiscard]] std::string normalize_title(std::string_view raw);

} // namespace ymh
```

`derive_auto_title` algorithm (pinned, deterministic):

1. Take the substring up to the first `'\n'` (the headless `first_line`
   behaviour, `src/cli/headless.cpp:46-50`).
2. Trim ASCII whitespace (`' '`, `'\t'`, `'\r'`, `'\f'`, `'\v'`) from both
   ends.
3. Collapse every internal run of ASCII whitespace to a single `' '`.
4. If the result contains any C0 control byte (`< 0x20`) or DEL (`0x7F`), or is
   not valid UTF-8 under the RFC 3629 acceptance rule pinned below, return
   `std::nullopt` (no rename is appended). Steps 1–3 already remove `'\n'` and
   collapse `'\t'`/`'\r'`/`'\f'`/`'\v'`, so this rejects only the remaining
   controls and malformed byte sequences — an auto title is never persisted
   torn or control-bearing (RN8).
5. If the result is empty, return `std::nullopt` (no rename is appended).
6. If it exceeds `kAutoTitleBytes`, cut at the largest UTF-8 code-point
   boundary `<= kAutoTitleBytes` and append `"..."` (matching the headless
   suffix, `src/cli/headless.cpp:52-54`).
7. Return the result.

The step-3 collapse is a deliberate, documented divergence from the headless
`first_line` (which does not collapse). The headless create-time title is left
**unchanged** by this errata (no behaviour churn); only the daemon-side
auto-name uses `derive_auto_title`.

**`normalize_title` contract (pinned, L5/L6).** `normalize_title` is the single
validator for user titles (RN8); the auto path produces a conforming title or
appends nothing. Two implementers must not be able to diverge:

1. **Trim set.** Only `' '` (0x20), `'\t'` (0x09), `'\r'` (0x0D), `'\f'` (0x0C),
   and `'\v'` (0x0B) are trimmed from both ends — exactly the
   `derive_auto_title` set above. `'\n'` (0x0A) is **not** trimmed: a
   leading/trailing newline survives to the control scan and is rejected. This
   is what makes the "ASCII whitespace" wording consistent with R-F4 (the
   header is single-line).
2. **Control scan.** After trimming, reject if any byte is `< 0x20` or `== 0x7F`
   (C0 controls, including `'\n'`/`'\t'`, and DEL). This runs **before** the
   UTF-8 scan, so a control byte is always `InvalidParams` (R-F4) regardless of
   the surrounding bytes.
3. **UTF-8 scan (RFC 3629 acceptance).** Accept exactly the well-formed
   sequences:
   - `0x00`–`0x7F` (ASCII; controls already rejected by step 2),
   - `0xC2`–`0xDF` + `0x80`–`0xBF`,
   - `0xE0` + `0xA0`–`0xBF` + `0x80`–`0xBF`; `0xE1`–`0xEC` + `0x80`–`0xBF` ×2;
     `0xED` + `0x80`–`0x9F` + `0x80`–`0xBF`; `0xEE`–`0xEF` + `0x80`–`0xBF` ×2,
   - `0xF0` + `0x90`–`0xBF` + `0x80`–`0xBF` ×2; `0xF1`–`0xF3` + `0x80`–`0xBF`
     ×3; `0xF4` + `0x80`–`0x8F` + `0x80`–`0xBF` ×2.
   Reject overlong encodings (`0xC0`/`0xC1` starts; `0xE0` + `0x80`–`0x9F`;
   `0xF0` + `0x80`–`0x8F`), UTF-16 surrogates (`0xED` + `0xA0`–`0xBF`, i.e.
   U+D800–U+DFFF), anything above U+10FFFF (`0xF4` + `0x90`–`0xBF`,
   `0xF5`–`0xFF`), truncated sequences, and stray continuation bytes. A torn
   code point is never persisted (R-F5).
4. **Length.** After trim and both scans, the byte length must be
   `<= kMaxSessionTitleBytes` (120). The check is on **bytes**, not code points;
   reject, never truncate (R-F3).
5. **Empty.** Empty-after-trim → reject (R-F2). Every rejection throws
   `std::invalid_argument`; `HostRuntime::renameSession` maps it to
   `InvalidParams` (§5.4, R-F1).

### 4.3 When it runs (pinned)

**Trigger.** The daemon evaluates the auto-name **once per session, on the
first `agent.prompt` request**, in `HostRuntime::agentPrompt`
(`src/host/host_runtime.cpp:601-618`), immediately after
`sessionExists(id)`/`ensureAgent(id)` and **before** `turns_.submit(...)`:

```cpp
// src/host/host_runtime.cpp — HostRuntime::agentPrompt (amended)
ensureAgent(id);
const Message parsed = message_from_json(message);
// 19 §4.3: advisory first-turn auto-name. `maybeAutoName` is a no-op unless
// RN5/RN6 hold. It performs the own-log scan, the title read, and the append as
// one critical section on the session's appendMutex_ (I18, §5.3), so it cannot
// race a TurnExecutor worker append. The append is synchronous (store append),
// so the second prompt cannot re-fire it.
//
// Auto-naming MUST NOT fail the user's prompt: a LeaseLost/StoreError here
// means only that the cosmetic name was not written (the append transaction
// rolled back, RN3/RN13), so swallow it and log at Warn. Any other exception is
// a genuine bug and still propagates.
try {
    static_cast<void>(runtime_.sessions().maybeAutoName(id, first_text_of(parsed)));
} catch (const StoreError& error) {
    category_logger(LogCategory::Session)
        .warn(std::string{"auto-name skipped: "} + error.what());
}
if (!turns_.submit(...)) { ... }
```

`StoreError` is `ymh::StoreError` (`include/ymh/session/errors.hpp:12`; `LeaseLost`
derives from it at `:43`) and `category_logger`/`LogCategory` come from
`include/ymh/core/logging.hpp:55`/`:24`, so `host_runtime.cpp` gains one include.
The auto path is deliberately **fail-soft**: it is a cosmetic projection, so it
can never turn a valid prompt into an error. This is the auto-path counterpart
of R-F6 (unknown session is a no-op, not a throw); the wire path is unchanged —
a user `/rename` still surfaces `LeaseLost`/`StoreUnavailable` (R-F7/R-F8,
RN13). Catching `StoreError` (not all `std::exception`) keeps genuine bugs loud.

`first_text_of` is a small `host_runtime`-local helper. The trigger parses the
prompt exactly once with `message_from_json`
(`src/host/host_runtime.cpp:105-116`), which normalizes a bare JSON string into a
single `Role::User` message carrying one `ContentBlockKind::Text` block
(`:106-114`) and otherwise decodes a `Message` object (`:115`). `first_text_of`
is **pinned** to return the `text` of the **first** content block whose
`kind == ContentBlockKind::Text` (`include/ymh/agent/message.hpp:111-127`), and
the empty string when there is none; it does **not** concatenate later text
blocks (R2-03). Both prompt shapes (bare string, `Message` object) are therefore
covered by that single parse. (There is no daemon-side equivalent today; the
UI's `flatten_content` lives in `src/ui/ui_event_adapter.cpp:14`.) Auto-naming
is **not** evaluated on `agent.followup`, `agent.steer`, or `agent.inject`:
those are not the first user turn (05 §7.5).

**Policy (RN5/RN6).** `SessionManager::maybeAutoName(id, text)` appends
`SessionRenamed{origin = Auto}` **iff all** hold; otherwise it returns
`std::nullopt` and appends nothing:

1. The session's current title `is_placeholder_title(...)` (RN6);
2. the session's **own log** (`Session::ownEvents()`,
   `src/session/session.cpp:391-400`) contains **no**
   `SessionRenamed{origin = User}` (RN5);
3. the session's own log contains **no** `SessionRenamed` at all — i.e. the
   auto-name fires at most once (RN5);
4. `derive_auto_title(text)` returns a value.

Condition 3 subsumes condition 2: `Session::appendAutoRename` (§5.3) returns on
the first own `SessionRenamed` regardless of origin, so a single check covers
both "no user rename" and "fire at most once". The whole list is evaluated, and
the append performed, under one `appendMutex_` critical section (R2-02).

The own-log scope (not the resolved view) is deliberate: a fork must not be
blocked from naming itself because its parent was renamed (RN2, 19-D14).

**Interaction with a user rename (pinned, the load-bearing rule).** A manual
rename must win and must never be overwritten. Because the auto-name fires at
most once and only when no user rename exists (conditions 2–3), a later auto
attempt on a session that a user has renamed is suppressed **by construction**.
Concretely, for the TUI sequence *prompt → auto-name → `/rename` → later
prompts*, the later prompts find a `SessionRenamed{origin = User}` in the own
log and no-op. For the sequence *`/rename` before any prompt → prompt*, the
`maybeAutoName` check sees the user rename and no-ops. The rule is derived from
the append-only own log, so it survives resume/replay (RN2/RN4).

**Headless.** `ymh run` sets a non-placeholder title at create
(`src/cli/headless.cpp:159`), so `is_placeholder_title` is false and the
auto-name is a no-op. Headless behaviour is unchanged.

### 4.4 Why the daemon (and not the agent loop)

The trigger is the daemon's `agent.prompt` handler because (a) it has the raw
prompt text and the `SessionManager` (`runtime_.sessions()`,
`include/ymh/agent/workspace_runtime.hpp:129`, already used at
`src/host/host_runtime.cpp:512,537,548`); (b) it runs on the daemon's
request-serialized path, so the first-prompt rename is appended before the
second prompt is examined; and (c) it does **not** touch spec 06. The rejected
alternative — hooking `AgentLoop` right after `appendUserMessage`
(`src/agent/agent_loop.cpp:523`) — is recorded in §11.

### 4.5 Rejected: LLM-derived naming

Rejected for five concrete reasons, each tied to shipped code:

1. **Cost/latency.** It adds a provider round-trip per new session. The
   compaction path already budgets one summarizer call per threshold crossing
   (`CompactionPolicy`, `include/ymh/agent/compactor.hpp:30-58`); a per-session
   call for a cosmetic title is disproportionate.
2. **Determinism/offline.** The hermetic suite is `FakeLLM`-backed (00 §44/§45).
   A non-deterministic name would have to be scripted per test and could not be
   asserted exactly.
3. **Failure.** A provider error/empty/over-long output is a new failure mode on
   the first-turn path for zero functional gain.
4. **Race.** An async LLM name computed after the first turn could land *after*
   a user `/rename`, violating RN5 unless it is gated by a user-rename check —
   at which point the LLM result is discarded in exactly the case it matters.
5. **No seam.** There is no session-layer LLM/provider dependency today; the
   session header is deliberately free of agent/UI types
   (`include/ymh/session/session.hpp:5`, "No UI/agent-loop type appears here"),
   and 01 §1.3 (`docs/design/01-session.md:61-71`) keeps event *emission* in 06.
   Introducing a provider call into 01 would invert the layering.

The heuristic keeps the door open: `RenameOrigin` and the append-only event make
a future `origin = AutoLlm` a purely additive change.

---

## 5. Rename mechanics

### 5.1 The event (RN-A1/RN-A3)

Add to `namespace ymh::payload` (`include/ymh/session/events.hpp`, beside
`SessionStarted` at `:26-30`):

```cpp
// ---- rename (19 §5.1) ------------------------------------------------------

// 19 §5.1: who produced a rename. `User` is an explicit /rename or
// session.rename; `Auto` is the daemon's first-turn heuristic (19 §4.3).
enum class RenameOrigin : std::uint8_t {
    User,
    Auto,
};

// 19 §5.1: the only way a title changes after creation. Append-only; the
// latest SessionRenamed in the session's own log is authoritative (RN2).
struct SessionRenamed {
    std::string  title;                     // normalized, non-empty (RN8)
    RenameOrigin origin = RenameOrigin::User;
};
```

`EventType` (`include/ymh/core/event.hpp:51-74`) gains one durable enumerator,
before the live-only `McpServerStatusChanged`:

```cpp
    SubagentFanIn,       // wire: subagent/fan_in
    SessionRenamed,      // wire: session/renamed   <-- 19 §5.1
    // Live-only (15 §4.7, AM-1): never in the durable SessionEventMap ...
    McpServerStatusChanged,  // wire: mcp/server_status_changed
```

Wire table (`src/core/event.cpp:18-39`): add `{SessionRenamed, "session/renamed"}`
and bump the array bound `20` → `21`. `all_event_types()`
(`src/core/event.cpp:61-71`) derives from the table, so it stays in sync.

`SessionEventMap` / `EventTraits` (`include/ymh/session/events.hpp:197-350`):

```cpp
template <>
struct SessionEventMap<EventType::SessionRenamed> {
    using type = payload::SessionRenamed;
};
// ...
template <>
struct EventTraits<payload::SessionRenamed> {
    static constexpr EventType type = EventType::SessionRenamed;
};
```

Name mapping (declaration `include/ymh/session/events.hpp:353-360`, definition
`src/session/events.cpp:114-138`), mirroring `turn_origin_name`:

```cpp
[[nodiscard]] std::string_view rename_origin_name(payload::RenameOrigin origin) noexcept;
// src/session/events.cpp
std::string_view rename_origin_name(payload::RenameOrigin origin) noexcept {
    switch (origin) {
        case payload::RenameOrigin::User: return "user";
        case payload::RenameOrigin::Auto: return "auto";
    }
    return {};
}
```

JSON (`src/session/events.cpp`, mirroring `TurnStarted` at `:212-222`):

```cpp
void to_json(nlohmann::json& json, const SessionRenamed& value) {
    json = nlohmann::json{
        {"title", value.title},
        {"origin", std::string{rename_origin_name(value.origin)}},
    };
}
void from_json(const nlohmann::json& json, SessionRenamed& value) {
    value.title  = json.at("title").get<std::string>();
    value.origin = parse_rename_origin(json.at("origin").get<std::string>());
}
```

`parse_rename_origin` rejects any other string (S3, `src/session/events.cpp`
`reject_enum` at `:19-21`). The JSON wire form is therefore exactly:

```json
{ "title": "fix the flaky PTY test", "origin": "user" }
```

**`deriveMessages` (RN-A5).** Add `case EventType::SessionRenamed: break;` to
the switch at `src/session/session.cpp:235-342` (grouped with
`SessionStarted`/`SessionEnded`/`McpServerStatusChanged` at `:236-239`). A rename
is not a message source; it changes no projection (RN4: messages are
byte-identical before and after a rename).

### 5.2 Replay, ordering, and the separately-persisted header

The log is append-only (I1) and the header is persisted in the `sessions` table
(`src/session/session_persistence.cpp:36-51`). Two representations of the title
therefore exist and must agree (RN3):

- **In-memory** `Session::header_.title`.
- **Durable** `sessions.title`.

**In-memory update (pinned).** In `Session::appendEvent`
(`src/session/session.cpp:406-426`) — split into a locking wrapper and
`appendEventLocked` by §5.3 (R2-02); the mirror goes in the shared
`appendEventLocked` body so both `appendEvent` and `appendAutoRename` get it —
after the successful store append and alongside the existing `header_.updatedAt`
update (`:417`):

```cpp
// 19 §5.2: mirror updatedAt materialization for the title.
if (event.type == EventType::SessionRenamed) {
    header_.title = event.payload.get<payload::SessionRenamed>().title;
}
```

The same mirror must be applied by `Session::appendBatch`
(`src/session/session.cpp:428-455`): after the batch commits, if any event is a
`SessionRenamed`, set `header_.title` from the **last** such event in the batch.
This keeps the in-memory header consistent with the store's per-event `UPDATE`
(§5.2 below) regardless of whether a rename is appended singly or batched.
(No current caller batches a rename; the mirror is pinned so a future one cannot
diverge.)

`Session::reload()` (`src/session/session.cpp:370-385`) gains a defensive
reconciliation pass that scans **`ownEvents()`** (not `log_`/`events()`) for the
highest-sequence `SessionRenamed`; if present, set `header_.title` from it. This
makes the **own log authoritative** on resume/replay even if a legacy DB's
`sessions.title` disagrees (R-F10). If no own `SessionRenamed` exists, the
loaded `sessions.title` is kept (backward compatible with every existing
session). The own-events scope is required for forks — see below.

**Fork semantics (pinned, resolves gate finding #2).** A fork's `log_` is the
**resolved view** = parent prefix ++ own events
(`SessionPersistence::resolve_after_locked`,
`src/session/session_persistence.cpp:342-382`; parent branch `:355-375`), and
`Session::events()` returns exactly that (`src/session/session.cpp:387-389`).
If the title were resolved over `events()`, a fork whose parent was renamed
inside the seed window would "inherit" the parent's `SessionRenamed` in memory,
while the fork's own `sessions.title` column (written only by the fork's own
appends) would remain `""` — a genuine divergence, and RN4 would be false.
The errata therefore pins:

- **The effective title is resolved over `Session::ownEvents()`**
  (`src/session/session.cpp:391-400`), which slices off the parent prefix using
  `header_.seedLength`. The slice is exact because
  `resolve_after_locked` throws `CorruptionError` when the parent prefix is
  shorter than `seedLength` (`src/session/session_persistence.cpp:363-365`), so
  the prefix has exactly `seedLength` records or the load fails loudly. A fork
  does **not** inherit its parent's title (19-D14); its title is its own
  `SessionStarted.title` (`""`, `src/session/session.cpp:499`) until it renames
  or auto-names.
- **Divergence is impossible for every session created through
  `SessionManager`** (the only creator in the daemon). Proof by the real write
  path: the only production writers of `sessions.title` are
  `SessionPersistence::create`'s `INSERT`
  (`src/session/session_persistence.cpp:713-719`) and the amended
  `append_batch_locked` `UPDATE` (below); `SessionManager::createSession` sets
  `header.title = options.title` and appends
  `SessionStarted{title = header.title}` in one call
  (`src/session/session_manager.cpp:53,62-66`); `Session::fork` sets
  `child.title = ""` and appends `SessionStarted{title = ""}`
  (`src/session/session.cpp:499,510-514`); and every later title change is a
  `SessionRenamed` appended to **that** session's own log, whose `UPDATE`
  targets **that** session's row. A parent's rename updates the parent's row
  only. Hence, by induction over appends, `sessions.title(S)` equals the last
  own `SessionRenamed.title(S)` if any, else `SessionStarted.title(S)` — the
  same value `ownEvents()` yields on `reload()`.

**Residual window (explicit).** The invariant is enforced only if nothing
writes `sessions.title` outside the two materialization points. Today there is
no other writer (verified by grep: the only `UPDATE sessions` is
`src/session/session_persistence.cpp:491`, the only `INSERT INTO sessions` is
`:713`, plus the test-only writers in `tests/`). RN3b pins this as a rule.

If a future change bypassed `SessionManager`/the store, the column could go
stale. **`Session::reload` resolves only the in-memory `header_.title` from the
own log; it neither compares against nor repairs the persisted `sessions.title`
column** (there is no store write seam for it, and RN3b forbids one).
`session.list`/`session.show` read the column without replaying
(`src/host/host_runtime.cpp:397-438`), so they keep returning the stale value on
every subsequent load; a reload never makes the column agree with the own log.
R-F10's reconciliation therefore makes the **in-memory** header follow the own
log on load, but the column stays wrong. **RN3b is the sole guarantee**:
forbidding any other writer is what keeps the column correct, not
reconciliation. The §12.3 test pins exactly this: after a legacy-DB reload,
`header_.title` follows the own log while `session.list`'s column is unchanged.

**Durable update (pinned).** The store is "the only mutating layer"
(`include/ymh/session/session_persistence.hpp:6`), and it already materializes a
header field (`updated_at`) inside the append transaction
(`src/session/session_persistence.cpp:491-494`). Extend `append_batch_locked`
(the same `BEGIN IMMEDIATE` block, `:457-519`) so that a committed
`SessionRenamed` also writes `sessions.title`:

```cpp
// src/session/session_persistence.cpp, inside append_batch_locked's event loop,
// before the existing UPDATE sessions SET updated_at = ? statement (:491-494):
if (event.type == EventType::SessionRenamed) {
    Statement set_title{db, "UPDATE sessions SET title = ? WHERE id = ?"};
    set_title.bindText(1, event.payload.get<payload::SessionRenamed>().title);
    set_title.bindText(2, id.value);
    set_title.step();
}
```

Consequences (all pinned):

- **Atomic (RN3).** Event insert + `title` update + `updated_at` update commit
  together or not at all. A crash cannot leave `sessions.title` inconsistent
  with the own log.
- **No new store seam (RN-A5).** `SessionStore` (`include/ymh/session/session.hpp:99-156`)
  gains **no** virtual. In-memory fakes keep their current shape; the durable
  store gains one statement. The default `appendBatch` loop
  (`session.hpp:118-125`) needs no change because `Session` performs the
  in-memory mirror.
- **Replay/read consistency (RN4).** `session.list`/`session.show`
  (`src/host/host_runtime.cpp:143,397-438`) read the materialized column and
  return the post-rename title without replaying the log; the value equals the
  own-log resolution above.
- **`updatedAt` (I22).** A rename advances `updated_at` like any append; no
  exception is carved out.
- **Ordering (RN10).** Within a session's own log, rename events are totally
  ordered by `Sequence` (I2); "the title" is the `title` of the
  **highest-sequence own** `SessionRenamed`. A fork's parent prefix is excluded,
  so a parent rename never affects the child (19-D14).

### 5.3 `SessionManager` / `Session` API (RN-A6, RN-A14)

Additive to `include/ymh/session/session_manager.hpp:34-66`:

```cpp
class SessionManager {
public:
    // ... existing createSession/resumeSession/forkSession/replaySession ...

    // 19 §5.4: append a User rename. Loads the session if not resident (like
    // forkSession). Validates `title` via normalize_title (RN8); throws
    // std::invalid_argument on a bad title and UnknownSession when absent.
    // LeaseLost / StoreError propagate from the store (RN13).
    Sequence renameSession(const SessionId& id, std::string title);

    // 19 §4.3: daemon-only. Appends SessionRenamed{origin=Auto} iff RN5/RN6
    // hold and derive_auto_title yields a value; otherwise a no-op. Never
    // throws for a suppressed name. May still throw LeaseLost/StoreError from
    // the store append; the sole call site (HostRuntime::agentPrompt) swallows
    // those so advisory auto-naming cannot fail the prompt (§4.3). Returns the
    // assigned Sequence when an event was appended. Delegates the policy +
    // append to `Session::appendAutoRename` so the check is synchronized (R2-02).
    std::optional<Sequence> maybeAutoName(const SessionId& id,
                                          std::string_view firstUserText);
};
```

**Atomic check + append (R2-02).** `maybeAutoName` must not scan `ownEvents()`
or read `header()` unsynchronized: `agent.prompt` runs on the daemon's single
io runner thread (§1), while a `TurnExecutor` worker can concurrently call
`Session::append`/`appendEvent` for the same session, which `push_back`s onto
`log_` (`src/session/session.cpp:418,444`) and would invalidate the span returned
by `ownEvents()` mid-scan. The check is therefore pinned **under the session's
own append mutex** (`appendMutex_`, I18, `include/ymh/session/session.hpp:226`),
and — to also close the check→append window — the check and the append are a
**single critical section**. Add to `class Session`
(`include/ymh/session/session.hpp`):

```cpp
class Session {
public:
    // ... existing header()/events()/ownEvents()/append()/appendEvent() ...

    // 19 §5.3: atomically evaluate the auto-name policy (RN5/RN6/RN7) and
    // append SessionRenamed{origin = Auto} iff it holds. The own-log scan, the
    // header_.title read, and the append all run under appendMutex_ (I18), so
    // the check cannot race a TurnExecutor worker-thread append and no rename
    // can interleave between the check and the append. Returns the assigned
    // Sequence, or std::nullopt when suppressed. Never throws for a suppressed
    // name (R-F6); LeaseLost / StoreError propagate from the store append
    // (RN13) and are swallowed by the advisory caller (§4.3).
    [[nodiscard]] std::optional<Sequence> appendAutoRename(std::string_view firstUserText);

private:
    // 19 §5.3: the body of today's `appendEvent` minus the lock. `appendEvent`
    // and `appendAutoRename` both call it while holding appendMutex_; the §5.2
    // title mirror lives here so every single-event append materializes it.
    Sequence appendEventLocked(Event event);
};
```

This adds no store seam (RN-A5 holds) and no virtual. It requires splitting the
current `Session::appendEvent` (`src/session/session.cpp:406-426`) into a locking
wrapper plus `appendEventLocked`; the §5.2 in-memory title mirror goes into
`appendEventLocked` so both callers get it. `SessionManager::renameSession`
(user) and `SessionManager::maybeAutoName` (auto) are then the only producers of
`SessionRenamed`; both run on the io thread, and a turn worker never constructs
one.

`renameSession` body (sketch):

```cpp
Sequence SessionManager::renameSession(const SessionId& id, std::string title) {
    const std::string normalized = normalize_title(title);   // RN8
    if (const auto it = sessions_.find(id.value); it == sessions_.end()) {
        const auto header = store_->load(id);
        if (!header.has_value()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        loadInto(*header);
    }
    return session(id).append(
        payload::SessionRenamed{std::move(normalized), payload::RenameOrigin::User});
}
```

`maybeAutoName` body (sketch):

```cpp
std::optional<Sequence> SessionManager::maybeAutoName(const SessionId& id,
                                                      std::string_view firstUserText) {
    if (sessions_.find(id.value) == sessions_.end()) {
        const auto header = store_->load(id);
        if (!header.has_value()) {
            return std::nullopt;                 // R-F6: never throw for auto
        }
        loadInto(*header);
    }
    // 19 §5.3: the RN5/RN6/RN7 policy and the append are one critical section
    // inside Session, under appendMutex_ (R2-02).
    return session(id).appendAutoRename(firstUserText);
}
```

`appendAutoRename` body (sketch, R2-02):

```cpp
std::optional<Sequence> Session::appendAutoRename(std::string_view firstUserText) {
    std::lock_guard<std::mutex> lock(appendMutex_);

    // 19 §5.3: scan the OWN log (fork prefix excluded) under the same mutex
    // appendEvent/appendBatch take (I18), so a worker-thread push_back cannot
    // invalidate log_ mid-scan.
    const std::size_t prefix =
        (header_.kind == SessionKind::Fork && header_.seedLength.has_value())
            ? *header_.seedLength
            : 0;
    const std::size_t first = std::min(prefix, log_.size());
    for (std::size_t index = first; index < log_.size(); ++index) {
        if (log_[index].event.type == EventType::SessionRenamed) {
            return std::nullopt;                 // RN5: fire once; user wins
        }
    }
    if (!is_placeholder_title(header_.title)) {  // RN6
        return std::nullopt;
    }
    const std::optional<std::string> derived = derive_auto_title(firstUserText);
    if (!derived.has_value()) {
        return std::nullopt;
    }
    TypedEvent<payload::SessionRenamed> typed;
    typed.id         = make_event_id();
    typed.session_id = id();
    typed.timestamp  = std::chrono::system_clock::now();
    typed.payload    = payload::SessionRenamed{*derived, payload::RenameOrigin::Auto};
    return appendEventLocked(encode(typed));     // lock already held
}
```

`header_.title` is only ever written under `appendMutex_` (the §5.2 mirror), so
reading it inside the same critical section is consistent. The `any_rename`
condition subsumes the `user_renamed` condition (RN5: the auto-name fires at
most once), so the scan returns on **any** own `SessionRenamed`.

### 5.4 Wire method (RN-A9)

Add to the catalog (`include/ymh/transport/protocol.hpp:476-506`):

```cpp
inline constexpr std::string_view kSessionRename = "session.rename";
```

and to `kMethodCatalog` (`src/transport/protocol.cpp:612-624`). The catalog size
is **not** pinned absolutely: this errata **adds one method**, so the size at
landing is `29 + N`, where `N` is the number of additive-method errata already
landed (spec 18 `context.show` is one; this mirrors 18 §3.4.1). The catalog is
`std::array<std::string_view, 29>` today (`src/transport/protocol.cpp:612`) and
`tests/unit/transport_protocol_test.cpp:41` pins `29u`. The errata that lands
**first** sets the array size template argument and that assertion to `30`; the
one that lands **second** sets them to `31`. **Neither errata pins an absolute
total**, and no existing entry is renumbered or reordered — each only appends
its own constant at the end of the `method` namespace
(`include/ymh/transport/protocol.hpp:476-506`). Do **not** write `30` here.
Extend 05 §7.4:

```text
session.rename    params: { session, title }    result: { session, title }
```

- `session`: UUIDv4; `title`: non-empty UTF-8 string (RN8). **`origin` is not a
  parameter**: the daemon always stamps `origin = User` (RN12), so a client
  cannot forge an `Auto` rename.
- Result echoes the normalized `title` (and the `session`). The echoed value's
  source is pinned in the `HostRuntime::renameSession` sketch below: it is the
  string returned by `normalize_title`, not a re-derivation from the manager's
  `Sequence` (R2-04).
- Profile: **both** Interactive and Automation. It is a session mutation like
  `session.create`/`session.delete` and is not in the gated set
  (`src/transport/protocol.cpp:641-647` gates only `session.activate`,
  `session.suspend`, `session.compact`, `host.shutdown`).
- Error mapping (05 §2.2 / §7.1):

| Condition | Code | `data.kind` |
|---|---|---|
| missing/non-string `title`; empty/whitespace-only; control char; invalid UTF-8; `> kMaxSessionTitleBytes` | `InvalidParams` (−32602) | `InvalidParams` |
| session not in the store | `UnknownSession` (−32006) | `UnknownSession` |
| lease lost at append | `LeaseLost` (−32007) | `LeaseLost` |
| store write failure | `StoreUnavailable` (−32015) | `StoreUnavailable` |

No new `AppCode` is introduced (RN-A8). `InvalidParams` is exactly the §54
"schema/type/size validation" code (T-F6, `docs/design/05-transport.md:1336`).

Add a typed result DTO beside `SessionResumed`
(`include/ymh/transport/host.hpp:53-56`) and a `TransportHost` virtual beside
`createSession` (`:78`):

```cpp
namespace ymh::protocol {

// 19 §5.4: the `session.rename` result. Named `...Result` to avoid colliding
// with `payload::SessionRenamed` (the durable event payload).
struct SessionRenamedResult {
    SessionId   session;
    std::string title;
};

} // namespace ymh::protocol
```

`HostRuntime` gains (RN-A13):

```cpp
// include/ymh/host/host_runtime.hpp (beside createSession :155)
protocol::SessionRenamedResult renameSession(const nlohmann::json& params) override;
```

`HostRuntime::renameSession` validates the params and **normalizes the title
itself** by calling `normalize_title` (the same session-layer validator the
manager uses, §4.2): a missing/non-string `title`, or a `std::invalid_argument`
thrown by `normalize_title`, is translated to `WireError{InvalidParams}` (it
must **not** rely on `translate`'s generic `std::exception` → `map_store_error`
path, which maps `std::invalid_argument` to `InternalError` —
`src/host/host_runtime.cpp:822-849`). It passes the **already-normalized**
string to `runtime_.sessions().renameSession(id, title)` — whose internal
`normalize_title` is idempotent (RN8), so the second pass is a no-op — and
returns `protocol::SessionRenamedResult{id, normalized}`. **The echoed result
title is therefore exactly the normalized string produced by
`HostRuntime::renameSession`, not a value re-derived from the manager's
`Sequence` return** (R2-04). `SessionManager::renameSession` keeps returning
`Sequence`; its signature is not widened. Sketch:

```cpp
// src/host/host_runtime.cpp — HostRuntime::renameSession (amended, R2-04)
protocol::SessionRenamedResult HostRuntime::renameSession(const nlohmann::json& params) {
    return translate([&]() -> protocol::SessionRenamedResult {
        if (!params.is_object()) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                                   "InvalidParams"});
        }
        const auto session_it = params.find("session");
        const auto title_it   = params.find("title");
        if (session_it == params.end() || !session_it->is_string() ||
            title_it == params.end() || !title_it->is_string()) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                                   "InvalidParams"});
        }
        const SessionId id{session_it->get<std::string>()};
        std::string normalized;
        try {
            normalized = normalize_title(title_it->get<std::string>());   // §4.2, RN8
        } catch (const std::invalid_argument&) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                                   "InvalidParams"});
        }
        (void)runtime_.sessions().renameSession(id, normalized);   // Sequence return kept
        return protocol::SessionRenamedResult{id, std::move(normalized)};
    });
}
```

The dispatcher (`src/transport/protocol_server.cpp`, beside `kSessionCreate`
at `:395-399`) answers:

```json
{ "session": "1a7b...uuid", "title": "fix the flaky PTY test" }
```

Because `renameSession` is a new pure virtual on `TransportHost`, the test double
`tests/support/fake_transport_host.hpp` must implement it too (returning
`SessionRenamedResult{session, title}` after recording the call); the errata's
wire tests (§12.4) assert the fake and the real host agree on the shape.

### 5.5 Compatibility rules

- **Additive event type.** A binary that predates `session/renamed` fails
  **loudly** on decode (01 S3; 05 T-F20,
  `docs/design/05-transport.md:1350`), never silently skips. This is the
  project's pinned "no lossy replay" rule.
- **No version bump (19-D9).** `kProtocolVersion` stays `1` and
  `kSchemaVersion` stays `1`. Rationale: the daemon and supervisor are spawned
  from the same binary (`ymh`), there is no mixed-version support contract, and
  there is no schema change (the `title` column already exists). A protocol
  bump to `2` would force every peer to renegotiate for an additive change with
  no migration. **Rejected:** bumping `kProtocolVersion` (churn without a
  compatibility need).
- **Backward-compatible reads.** Every existing session has zero
  `SessionRenamed` events, so `sessions.title` and `SessionStarted.title` are
  unchanged; `reload()`'s reconciliation is a no-op (RN2 fallback).
- **`SessionStarted.title` is never rewritten.** The create-time title remains
  the immutable first value; renames are new events (I1, RN1).

---

## 6. The `/rename` command (RN-A12)

### 6.1 Syntax and semantics

`CommandRegistry` gains one builtin (`src/ui/command_registry.cpp:113-196`):

```cpp
registry.add(Command{
    "rename", "rename the current session",
    [](CommandContext& context, const std::string& args) {
        if (context.session == nullptr) {
            append_system(context, "no active session");
            return;
        }
        if (args.empty()) {
            append_system(context, "usage: /rename <title>");
            return;
        }
        if (context.rename_session) {
            context.rename_session(args);
        }
    }});
```

`CommandContext` (`include/ymh/ui/command_registry.hpp:15-27`) gains one
callback beside `compact`/`export_session`:

```cpp
std::function<void(const std::string&)> rename_session;
```

`SupervisorApp::dispatch_command` (`src/ui/supervisor.cpp:901-918`) wires it to
the daemon in the same callback slot as `compact` (`:907-915`), but — unlike
`compact`, which passes `nullptr` — it passes a reply lambda so a rejected
rename is surfaced:

```cpp
context.rename_session = [this](const std::string& title) {
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace == nullptr || workspace->activeSessionId.value.empty()) {
        return;
    }
    const SessionId session = workspace->activeSessionId;
    // 19 §6.1 (M1): the reply is non-null and surfaces a rejected rename. It
    // runs on the pump thread, so it marshals to the UI thread via enqueue and
    // reuses the existing ErrorOccurred path, which appends a Role::System
    // entry (src/ui/ui_model.cpp:641-648). Success is silent here: the
    // session/renamed event updates the title via the stream (RN9/RN11).
    submit_to(workspace->id, std::string(protocol::method::kSessionRename),
              nlohmann::json{{"session", session.value}, {"title", title}},
              [this, session](SupervisorReply reply) {
                  if (reply.ok) {
                      return;
                  }
                  enqueue([this, session, error = reply.error] {
                      model_.apply(UiEvent{ErrorOccurred{
                          session, "rename failed: " + error}});
                  });
              });
};
```

**Rules (pinned):**

1. Syntax is `/rename <title>`; the title is the entire trimmed argument string
   (spaces allowed). `dispatch` already splits the command name from `args`
   (`src/ui/command_registry.cpp:96-98`).
2. **Empty args** → a local system notice with usage; **no wire call**
   (`append_system`, `src/ui/command_registry.cpp:23-33`).
3. **Invalid/too-long title** → the daemon returns `InvalidParams`; the reply
   lambda (non-null, §6.1 sketch) runs on the pump thread, marshals with
   `enqueue` (`src/ui/supervisor.cpp:488-496`), and applies
   `UiEvent{ErrorOccurred{...}}`, which appends a `Role::System` entry through
   the existing error path (`src/ui/ui_model.cpp:641-648`) and marks
   `Conversation | Status | Attention`. The notice is therefore **not** dropped.
   The draft is still cleared by `handle_input`'s Return branch
   (`src/ui/supervisor.cpp:1130-1143`), consistent with other commands.
4. **Length cap:** `kMaxSessionTitleBytes = 120` (RN8). The client does not
   pre-validate; the daemon is the single validator (RN8), so the cap cannot
   drift between UI and wire.
5. **No inline editor.** `/rename` takes inline arguments. A modal
   pre-filled editor is **rejected/deferred** (§11): it would need new dialog
   mode/state (`UiMode` is `Conversation|Switcher|Dialog|ExitConfirm`,
   `include/ymh/ui/ui_event.hpp:47-52`) for no correctness gain.
6. The command is **supervisor-local** in the sense that it originates in the
   UI, but the actual mutation is the durable wire method — unlike `/clear`
   (view-only) or `/model` (supervisor-local), `/rename` must go to the daemon
   so every supervisor sees it (§7).

**Error-surfacing mechanism (pinned, M1).** The reply lambda is the *only* way a
rejected rename reaches the user (the client does not pre-validate, rule 4). It
must not touch the model directly: it is invoked from
`SupervisorConnection::process_requests` on the pump thread
(`src/ui/supervisor_connection.cpp:339-374`), so it captures by value and calls
`enqueue` (`src/ui/supervisor.cpp:488-496`) — the same marshal `create_session`'s
reply already uses (`:723-760`). On the UI thread it applies `ErrorOccurred`
(`include/ymh/ui/ui_event.hpp:161-164`) via `UiModel::apply`
(`include/ymh/ui/ui_model.hpp:375`). This reuses an existing path; **no new
helper is introduced**. Success appends nothing: the daemon's `session/renamed`
event updates the header and switcher through `SessionTitleChanged` (§6.3). The
side effect of `ErrorOccurred` (it also sets `status.last_error` and raises the
session attention flag) is intended: a rejected rename is not silent.

### 6.2 Interaction with `<Tab>` completion (RB-08)

No new completion code. `CommandRegistry::complete`
(`src/ui/command_registry.cpp:60-69`) matches by prefix over
`commands()`, and `refresh_hints`/`complete_command`
(`src/ui/supervisor.cpp:920-933` / `:942-981`) consume it. Consequences
(pinned):

- Typing `/re` and pressing Tab yields `/rename ` (unique-match terminal
  completion appends a trailing space, `src/ui/supervisor.cpp:962-967`), so the
  user lands ready to type the title.
- `/r` is **unambiguous**: only `/rename` starts with `r` (`/clear`, `/compact`,
  `/exit`, `/help`, `/model`, `/new` do not) → a unique match. No new cycle
  behaviour is introduced; RB-08's pinned cycle rules are untouched.
- The command hint line (`render_command_hints`, referenced by 17 §5) lists
  `/rename` automatically because it is a registry command.

### 6.3 Interaction with the top-line session name (RB-10)

The rename must update the right-aligned header slot that RB-10 pinned
(`src/ui/ui_render.cpp:461-484`). The path is event-driven:

1. The daemon commits `SessionRenamed` and publishes it (I4); the transport
   fans it out on `event.stream` to every subscriber (05 §7.7/§8.2).
2. `UiEventAdapter::adapt` gains an explicit case (the adapter's `default` at
   `src/ui/ui_event_adapter.cpp:153` would otherwise drop it):

```cpp
case EventType::SessionRenamed: {
    const auto payload = event.payload.get<payload::SessionRenamed>();
    events.push_back(UiEvent{SessionTitleChanged{session, payload.title}});
    break;
}
```

3. `UiEvent` (`include/ymh/ui/ui_event.hpp:193-214`) gains one alternative:

```cpp
struct SessionTitleChanged {
    SessionId   session;
    std::string title;
};
```

4. `UiModel::apply` (`src/ui/ui_model.cpp:468-483`) gains a branch. The
   `SessionUiState` already carries its `workspace`
   (`include/ymh/ui/ui_model.hpp:201`), so:

```cpp
} else if constexpr (std::is_same_v<T, SessionTitleChanged>) {
    setCellTitle(state.workspace, e.session, e.title);   // ui_model.cpp:413-426
}
```

`render_header` reads `SessionCell.title` (`src/ui/ui_render.cpp:471-476`) with
the existing `short_id` fallback (`:477-479`), so the header updates on the
next frame. `setCellTitle` already marks
`UiDirtyFlag::Layout | UiDirtyFlag::SessionBar` (`src/ui/ui_model.cpp:422`).

`refresh_sessions` (`src/ui/supervisor.cpp:653-701`) remains the
reconnect/attach source of truth: it reads `SessionSummary.title` (`:668`) and
calls `setCellTitle` (`:688`), so a supervisor that attaches *after* a rename
sees the new title without replaying the event.

---

## 7. Switcher, aggregate, and cross-supervisor interaction

### 7.1 Propagation to other supervisors

Two or more supervisors attached to the same workspace daemon each subscribe to
every tracked session (`SupervisorConnection::track` →
`subscribe_tracked` → `subscribe_one`,
`src/ui/supervisor_connection.cpp:61-67,233-276`), and the daemon fans every
committed durable event out to all subscribers (05 §8.2). Therefore:

- A rename issued by supervisor A is committed once by the shared daemon and
  delivered to supervisor B (and C, …) as a normal `event.stream` carrying a
  `session/renamed` envelope.
- B's `UiEventAdapter` turns it into `SessionTitleChanged`, which updates B's
  `SessionCell.title` and hence B's header and switcher leaf
  (`src/ui/ui_render.cpp:438-440`).
- A supervisor attaching later reads the materialized `sessions.title` via
  `session.list` (§5.2). **No polling and no new notification kind are
  required** (16 O8's "live visibility" is satisfied by the existing stream).

`host.event`/`HostNotice` is **not** used for rename: `HostNoticeKind`
(`include/ymh/transport/protocol.hpp:230-236`) is for open-set/lifecycle changes,
not per-session metadata, and the event stream already carries the payload.

### 7.2 Concurrent renames: last-writer-wins (decision 19-D8)

If two supervisors rename the same session concurrently, both requests reach the
**same** daemon (only the lease holder may append, I5; the daemon holds the
lease). Request handling is serialized on the daemon's single transport runner
thread (05 §7.1:779-781; **verified** in §1 from
`src/transport/transport_server.cpp:257-266` and
`src/transport/protocol_server.cpp:220-253,351`), and every append additionally
takes the session's `appendMutex_` (I18, `src/session/session.cpp:407`). The
auto-name **check** takes that same mutex, held across the check and the append
(`Session::appendAutoRename`, §5.3): it therefore never reads a `log_` that a
`TurnExecutor` worker is concurrently `push_back`ing into, and no rename can
interleave between the check and the append (R2-02). Both events commit; their
`Sequence`s are totally ordered (I2). The projection takes the highest-sequence
`SessionRenamed` (RN2/RN10). Therefore:

- **Last-writer-wins by log order**, not by wall clock. The outcome is
  deterministic once the commit order is fixed and identical on every
  supervisor and on replay.
- **No conflict detection and no error.** Rename is a "last-writer-wins
  register" over an append-only log; both events are retained (audit), and no
  write is lost from the log even though one loses the projection race. This
  mirrors how a session's other last-writer state (e.g. `updatedAt`) behaves.
- **Convergence.** Because every subscriber applies events in per-session
  publish order (T6, I6) and the highest sequence wins, all supervisors
  converge to the same title (RN11).

**Rejected:** optimistic concurrency (compare-and-set on the previous title) —
it would add a conflict error code, a client retry loop, and a second source of
truth (the title the client thinks it saw), all to prevent a benign cosmetic
collision. **Rejected:** first-writer-wins — it would silently discard a user's
explicit rename, violating the spirit of "user rename must win".

### 7.3 Aggregate / switcher

`aggregate.recompute` (`src/ui/ui_model.cpp`, invoked from
`applyAndMark` at `src/ui/ui_event_adapter.cpp:172`) is keyed on agent state,
not titles; a rename changes no counts (RB-11's counts-only bottom line is
untouched). The switcher leaf text changes via the same `setCellTitle` path.

---

## 8. Invariants

Numbered `RN1`–`RN15`; testable and cited. Any code that can violate one is a
defect.

**RN1 — Append-only title change.** A title changes only by appending a
`SessionRenamed` event. `SessionStarted` and every prior event are never
rewritten, and `sessions.title` is only ever updated as a materialization of a
committed `SessionRenamed`. (extends I1; §5.2)

**RN2 — Title resolution (own log).** For a session `S`, the effective title is
the `title` of the highest-`Sequence` `SessionRenamed` in **`S.ownEvents()`**
(`src/session/session.cpp:391-400`), if any; otherwise `SessionStarted.title`
from `S`'s own log; otherwise the stored header title. The **resolved view**
`S.events()` (`:387-389`, parent prefix ++ own for a fork,
`src/session/session_persistence.cpp:342-382`) is **never** used to resolve the
title. (§5.2, RN15)

**RN3 — Atomic materialization.** When an append commits a `SessionRenamed`,
`sessions.title` equals that event's `title` in the same transaction as the
event insert and the `updated_at` update. A crash cannot desynchronize the
column from the own log. (§5.2)

**RN3b — Single writer.** `sessions.title` is written by exactly two statements:
the `INSERT` in `SessionPersistence::create`
(`src/session/session_persistence.cpp:713-719`) and the `UPDATE` in
`append_batch_locked` for a committed `SessionRenamed` (§5.2). No other
statement may write the column (the only other `sessions` writers are the
`updated_at` `UPDATE` at `:491` and the `DELETE` at `:793`). (§5.2)

**RN4 — Replay/read consistency (fork-safe).** For every session created
through `SessionManager`, the own-log resolution (RN2), the in-memory
`Session::header_.title` after `reload()`, and the column read by
`session.list`/`session.show` (`src/host/host_runtime.cpp:143,397-438`) are the
same value. Renames change no `deriveMessages()` output. **Fork corollary:** a
fork's resolved view may contain a parent `SessionRenamed` inside the seed
window, but because resolution is over `ownEvents()` the fork's title is its own
(`""` until it renames/auto-names); `sessions.title` for the fork is likewise
its own. The fork's resolved-view prefix therefore cannot make RN4 false.
(§5.2)

**RN5 — Fire-once + user-wins.** An auto rename is appended at most once per
session, and only if no `SessionRenamed{origin = User}` exists in the session's
**own log** (`ownEvents()`). A user rename is never overwritten by an auto
rename. The own-log scan, the title read, and the append are **one critical
section** under the session's `appendMutex_` (`Session::appendAutoRename`, I18),
so the decision cannot race a worker-thread append and no rename can interleave
between the check and the append (R2-02). (§4.3/§5.3)

**RN6 — Placeholder rule.** An auto rename is considered only when the current
title `is_placeholder_title(...)` (`""`, `"tui"`, `"main"`). (§4.2/§4.3)

**RN7 — Determinism.** `derive_auto_title` is a pure function of the prompt
text: no clock, no I/O, no randomness, no locale-dependent Unicode tables.
(§4.2)

**RN8 — Bounded, normalized rename-path title.** Every title written through the
rename path — the `SessionRenamed.title` payload, the `sessions.title`
materialization of it (RN3), and the `header_.title` projection (RN2) — is
non-empty, valid UTF-8 (RFC 3629), free of C0 control bytes and DEL, and
`<= kMaxSessionTitleBytes` (120) bytes. User input is normalized and
**rejected** (not silently truncated) when invalid (`normalize_title`, §4.2);
auto titles are sanitized and cut on a code-point boundary with a `"..."` suffix
(`derive_auto_title`, §4.2). **Not covered:** the `SessionStarted.title` written
by `session.create` — including legacy and headless-created sessions
(`src/cli/headless.cpp:159` truncates to 63 bytes but neither validates UTF-8 nor
strips control bytes) — is **not** validated by this errata and may be empty,
control-bearing, or invalid UTF-8. RN8 constrains a session's title only once
that title is (re)written through the rename path. (§4.2/§5.1)

**RN9 — Durable-before-observable.** `SessionRenamed` is published on the
`EventBus`/wire only after commit (extends I4, T7); `updatedAt` advances (I22).
(§5.2)

**RN10 — Total rename order.** Within a session's own log, rename events are
totally ordered by `Sequence`; the projection uses the maximum own
`SessionRenamed`. A parent prefix is excluded, so a parent rename never affects
a child. (§5.2/§7.2)

**RN11 — Cross-supervisor convergence.** A committed rename reaches every
attached subscriber of the session via `event.stream` in publish order, and all
supervisors converge on the same title. (T6, 16 O8; §7.1/§7.2)

**RN12 — Wire origin is not forgeable.** `session.rename` always produces
`origin = User`; clients cannot submit `Auto`. Auto renames are daemon-internal.
(§5.4)

**RN13 — Read-only degrade.** On lease loss, a rename fails with `LeaseLost`
and appends nothing; `sessions.title` and `header_.title` are unchanged.
(I5, S4; §5.4)

**RN14 — No path effect.** Rename never resolves a path, never `chdir()`s, and
never touches `cwd`. (I8, I15; §5.2)

**RN15 — Fork non-inheritance.** A fork does not inherit its parent's title.
Its title is resolved over its own log (RN2), it is created with
`SessionStarted.title == ""` (`src/session/session.cpp:499,510-514`), and a
parent `SessionRenamed` inside the fork's seed window never changes the child's
title or `sessions.title`. (I10; §5.2, 19-D14)

---

## 9. Failure modes

### 9.1 Shared findings (F1–F12, §54)

| F# | Finding | Rename-layer handling |
|---|---|---|
| **F1** | path/process isolation | rename carries no path; it mutates one column in the daemon's own store and never resolves from `getcwd()` (RN14) |
| **F2** | background permission | rename needs no permission; it is not a tool and never blocks on a client (T19) |
| **F3** | late event after close | a rename committed before `SessionEnded` is delivered before the stream closes; a rename after delete is `UnknownSession` (R-F12) |
| **F4** | edge-triggered attention | rename does not change `AgentState`; no attention edge is produced (adapter `project_state` `default` at `src/ui/ui_event_adapter.cpp:75`) |
| **F5** | output ring buffers | rename output is bounded by RN8; no unbounded buffer |
| **F6** | input/keybinding focus | `/rename` is a normal input line; no new keybinding or focus state |
| **F7** | per-session dirty flags | rename marks `Layout | SessionBar` via `setCellTitle` (`src/ui/ui_model.cpp:422`) |
| **F8** | resource caps | rename respects the existing payload cap (`PersistenceConfig::max_payload_bytes`, `include/ymh/session/session_persistence.hpp:62`); the title cap is far smaller |
| **F9** | cancellation scoping | rename is not a turn; `agent.cancel` cannot affect it |
| **F10** | resume-suspended | resume appends nothing (I10/F10); a resumed session's title is re-derived on load (RN2) |
| **F11** | subagent ID duality | rename targets one `SessionId`; it never routes to a parent |
| **F12** | flash clock in model | rename introduces no timer |

### 9.2 Component-local failure modes (`R-F1`–`R-F12`)

Component-local to this errata; not the top-level F1–F12 set. Detection +
required behaviour.

| R-F# | Failure | Detection | Required behaviour |
|---|---|---|---|
| **R-F1** | `title` missing or not a string | JSON schema check in `HostRuntime::renameSession` | `InvalidParams`; no side effect (T-F6) |
| **R-F2** | empty / whitespace-only title | `normalize_title` after trim | `InvalidParams`; no append (§6.1) |
| **R-F3** | title > `kMaxSessionTitleBytes` | byte length after normalization | `InvalidParams`; **reject, never silently truncate** user input (RN8) |
| **R-F4** | title contains a control character / newline | byte scan in `normalize_title` | `InvalidParams`; the header is single-line (§6.3) |
| **R-F5** | title is not valid UTF-8 | UTF-8 validation in `normalize_title` | `InvalidParams`; never persist a torn code point |
| **R-F6** | unknown session | `store_->load(id)` empty | `UnknownSession` for the wire; a **no-op** (not a throw) for auto-name |
| **R-F7** | lease lost at append | `SessionStore::append` throws `LeaseLost` (`src/session/session.cpp:412-413`) | wire rename: `LeaseLost`; header and log unchanged (RN13). auto path: **swallowed + logged** (§4.3), the prompt proceeds |
| **R-F8** | store write failure | store throws `StoreError`/SQLite failure | wire rename: `StoreUnavailable`; the transaction rolls back — no partial rename (RN3). auto path: **swallowed + logged** (§4.3) |
| **R-F9** | auto-name races a user rename | `maybeAutoName` re-scans the **own** log under `appendMutex_` (`Session::appendAutoRename`, §5.3) | suppressed; no `Auto` event is appended (RN5) |
| **R-F10** | column/own-log disagreement (legacy DB or a writer bypassing RN3b) | `Session::reload()` resolves `header_.title` from the own log; the disagreement is visible because that value differs from the loaded `sessions.title` (reload does **not** compare or repair the column) | own log wins **in memory**; `header_.title` is set from the own log on load (RN2/RN4); the column is **not** rewritten by reload — RN3b is the real fix |
| **R-F11** | two supervisors rename concurrently | both requests reach the one lease-holding daemon; two committed sequences | last-by-sequence wins; both events retained; all subscribers converge (RN10/RN11, §7.2) |
| **R-F12** | rename after delete | `store_->load(id)` empty (delete erases the row, `src/session/session_manager.cpp:111-125`) | `UnknownSession`; no append; no zombie title |

`R-F2`–`R-F5` are all `InvalidParams` by design (T-F6); `R-F6`–`R-F8` reuse the
existing `UnknownSession`/`LeaseLost`/`StoreUnavailable` codes (RN-A8). No new
error code exists.

---

## 10. dsh mapping

dsh is the architectural reference (00 §55). Rename maps as follows.

| dsh concept | ymh rename | Reference |
|---|---|---|
| Session metadata update event | `payload::SessionRenamed` (wire `session/renamed`) appended to the typed `SessionEventMap` log | 01 §4.3–§4.5 |
| Last-writer-wins register over an append-only log | highest-`Sequence` `SessionRenamed` is the title; both writes retained | RN2/RN10, §7.2 |
| Session-store materialized field | `sessions.title`, updated in the same transaction as the append | §5.2 |
| Typed event bus / fan-out | `EventBus` + `event.stream` deliver the rename to N supervisors | 05 §8.2, RN11 |
| Slash command surface | `/rename <title>` in `CommandRegistry`, completed by RB-08 | §6 |
| Cordis service capability | `SessionManager::renameSession` / `maybeAutoName` | 01 §8 |
| dsh session titling | heuristic first-turn auto-name (`Auto`), no LLM titling | 19-D1, §4 |
| Distributed metadata sync | not modeled; the single workspace daemon is the arbiter | §7.2 |

**Deliberate omissions** (accepted for v1): no rename history UI, no title
uniqueness, no title search/index, no LLM titling, no cross-workspace rename.

---

## 11. Decisions and rejected alternatives

| ID | Decision | Rationale |
|---|---|---|
| **19-D1** | Auto-naming is **heuristic** (first user turn), not LLM-derived | §4.1/§4.5: deterministic, free, offline/FakeLLM-safe, no failure mode |
| **19-D2** | Trigger is the daemon's first `agent.prompt` | §4.3/§4.4: has the text + manager, serialized path, no spec-06 change |
| **19-D3** | Fire-once, user-wins, via `RenameOrigin` | §4.3: satisfies "manual rename is never overwritten" |
| **19-D4** | New durable event `SessionRenamed{title, origin}` | append-only + typed + replayable; the only way to change a title (RN1) |
| **19-D5** | `sessions.title` materialized inside the append transaction; no new store virtual | §5.2: atomic, mirrors `updated_at`, keeps the store seam stable |
| **19-D6** | Wire method `session.rename {session, title}`; origin always `User` | §5.4/RN12: simple, non-forgeable, both profiles |
| **19-D7** | `/rename <title>` inline args; **no** inline editor | §6.1: reuses the command registry; a modal adds UI state for no gain |
| **19-D8** | Concurrent rename = last-writer-wins by `Sequence` | §7.2: deterministic, convergent, no lost log entries, no new error code |
| **19-D9** | No `kProtocolVersion` / `kSchemaVersion` bump | §5.5: additive; single-binary deployment; no schema change |
| **19-D10** | Placeholder set `{"", "tui", "main"}` | §4.2: the two shipped placeholders + the fork default |
| **19-D11** | Cap 120 bytes; user titles rejected, auto titles cut | §4.2: one daemon-side validator (RN8) |
| **19-D12** | `derive_auto_title` collapses whitespace; headless `first_line` unchanged | §4.2: cleaner titles without churning the headless path |
| **19-D13** | The auto-name check + append is one critical section on `appendMutex_` (`Session::appendAutoRename`); the io thread never reads `ownEvents()`/`header()` unsynchronized | §5.3/§7.2: closes the io-thread read vs. worker-thread append race (R2-02); race-free by construction, independent of the request-loop serialization claim |
| **19-D14** | Title resolution is over the session's **own** log; a fork does **not** inherit its parent's title | §5.2/RN2/RN15: makes column/own-log divergence impossible; matches `Session::fork`'s `SessionStarted.title == ""` and the per-session nature of `cwd`/`model`/`kind` |

**Rejected alternatives (recorded):**

1. **LLM-derived auto-name** — §4.5 (cost, latency, determinism, FakeLLM,
   failure, race, layering).
2. **Rewrite `SessionStarted`** to carry the new title — violates I1
   (append-only) and I4; rejected.
3. **Update `SessionHeader` only (no event)** — the title would not survive
   replay and would not reach other supervisors; violates RN1/RN4/RN11.
4. **Two-write persistence (`append` then `UPDATE title`)** — a crash between
   them desynchronizes the header from the log; rejected for RN3.
5. **New `SessionStore::setTitle` virtual** — unnecessary (the append path
   already materializes a header field) and it would push atomicity onto
   callers; rejected.
6. **New `AppCode::InvalidTitle`** — `InvalidParams` is exactly T-F6's
   validation code; a new code would be redundant; rejected (RN-A8).
7. **Client-supplied `origin`** — forgeable; auto-names are an implementation
   detail; rejected (RN12).
8. **Inline editor modal** — deferred; §6.1.
9. **Optimistic compare-and-set / first-writer-wins** — §7.2.
10. **`previousTitle` in the payload** — inert, adds an invariant and coupling
    for no consumer; rejected.
11. **Hooking the agent loop after `appendUserMessage`** — would touch spec 06
    and would interleave the auto-name after `UserMessage` (either order is
    correct, but the daemon trigger is shallower and keeps 06 untouched);
    rejected (§4.4).
12. **Bumping the protocol/schema version** — §5.5.
13. **Resolving the title over the resolved view (`events()`)** — for a fork this
    includes the parent prefix, so the in-memory title could differ from the
    fork's own `sessions.title` column; it makes RN4 false. Rejected in favour of
    own-log resolution (19-D14).
14. **Inheriting the parent's title in a fork** (materialize the parent's last
    rename into the child at fork time) — rejected: it changes the child's
    display silently at fork time, needs a fork-time extra write, and is
    confusing UX; `Session::fork` already pins `SessionStarted.title == ""`
    (`src/session/session.cpp:499`).

---

## 12. Test plan

Strategy is 00 §44: unit, integration (Fake LLM / fake FS / fake shell),
persistence/replay, golden, and a separate opt-in live PTY layer. The
deterministic layers run offline against `FakeLLM`; the live layer is opt-in
(`YMH_LIVE_LLM=1`). No `sleep`-based synchronisation (16 §8.4).

### 12.1 Unit — session / events

| Test | Asserts |
|---|---|
| `event_test.cpp` `WireNameIsInverseOfParseForEveryType` (`:49-59`) | passes for the extended `all_event_types()` including `SessionRenamed`/`session/renamed` (RN-A1) |
| `events.cpp` round-trip | `SessionRenamed` ↔ JSON preserves `title` and both `origin` values; `parse_rename_origin("bogus")` throws (S3) |
| `deriveMessages` | a `SessionRenamed` between messages yields **byte-identical** messages (RN4) |
| `derive_auto_title` | first line only; trim; whitespace collapse; a result carrying a C0 control/DEL byte or invalid UTF-8 → `nullopt`; empty → `nullopt`; 60-byte cut on a code-point boundary + `"..."`; multi-byte input never splits a code point (RN7/RN8) |
| `normalize_title` | trims only `' '`/`'\t'`/`'\r'`/`'\f'`/`'\v'` (a leading/trailing `'\n'` is **rejected**, not trimmed); rejects empty/whitespace-only, any C0 control/DEL, invalid UTF-8, and 121 bytes; accepts exactly 120 bytes; the RFC 3629 cases accept `0xC2`–`0xF4` starts and reject overlong (`0xC0`/`0xC1`), surrogates (`0xED`+`0xA0`–`0xBF`), `> U+10FFFF` (`0xF4`+`0x90`–`0xBF`), stray/truncated continuation bytes (RN8, R-F2..R-F5) |
| `is_placeholder_title` | true for `""`/`"tui"`/`"main"`; false for any other string (RN6) |

### 12.2 Unit — `SessionManager` / auto-name policy

| Test | Asserts |
|---|---|
| `renameSession` appends + returns a sequence | `header().title` updates; one `SessionRenamed{origin=User}` in the session's own log (RN1/RN2) |
| `renameSession` on an unknown id | throws `UnknownSession` (R-F6) |
| `renameSession` with a bad title | throws `std::invalid_argument`; no append (R-F2..R-F5) |
| `maybeAutoName` on a placeholder, no renames | appends exactly one `SessionRenamed{origin=Auto}` with the derived title (RN5/RN6) |
| `maybeAutoName` twice | the second call returns `nullopt`; still exactly one rename (fire-once, RN5) |
| **`maybeAutoName` after `renameSession`** | returns `nullopt`; the user title is **unchanged** — the required "manual rename is never overwritten" case (RN5) |
| `maybeAutoName` when the title is non-placeholder (headless-style) | `nullopt`; no event (RN6) |
| `maybeAutoName` with a prompt that derives nothing | `nullopt`; no event |
| **`maybeAutoName` is race-free (R2-02)** | `Session::appendAutoRename` performs the own-log scan, the `header_.title` read, and the append under **one** `appendMutex_` acquisition (it calls `appendEventLocked`, not the locking `appendEvent`); a two-thread smoke test (a worker appending `UserMessage` events in a loop while the io thread calls `maybeAutoName`) reports no race under ThreadSanitizer and still appends exactly one `SessionRenamed{origin=Auto}` (RN5) |
| **auto-name failure is swallowed (L7)** | with a store whose append throws `LeaseLost`/`StoreError`, `HostRuntime::agentPrompt` still submits the turn (the exception does not escape), no `SessionRenamed` is appended, and a Warn is logged (§4.3) |

### 12.3 Persistence / replay

| Test | Asserts |
|---|---|
| `SessionPersistence::append` of a `SessionRenamed` | `sessions.title` equals the payload title in the **same** commit; `updated_at` advanced (RN3/RN9) |
| reopen + `load` | the new title is returned; `session.list` shows it (RN4) |
| crash injection mid-append | force a rollback and assert **neither** the event nor the title changed (R-F8/R-F10) |
| legacy DB (own event present, column stale) | `Session::reload` reconciles `header_.title` from the **own** log; assert `session.list`'s column is unchanged by reload (R-F10/RN2) |
| **fork does not inherit** | rename the parent, then `fork(parent, seed)` where the parent's `SessionRenamed` is inside the seed window; assert the child's `header().title == ""`, `ownEvents()` contains no `SessionRenamed`, `sessions.title(child) == ""`, and `session.list` shows `""` for the child (RN2/RN15/RN4) |
| **fork renames itself** | rename the child; assert only the child's column/own log change, and the parent's title is untouched (RN3b/RN10) |
| **single writer (RN3b)** | a source/DDL audit asserts the only `sessions.title` writers are the `create` `INSERT` and the `append_batch_locked` `UPDATE` (RN3b) |
| replay | replaying a renamed session reproduces the final title and identical messages (RN4) |

### 12.4 Wire

| Test | Asserts |
|---|---|
| `transport_protocol_test.cpp` catalog | `session.rename` is known and in `all_methods()` (RN-A9) |
| `session.rename` happy path | result `{session, title}`; a `session/renamed` envelope follows on `event.stream` (RN9/RN11) |
| missing / non-string `title` | `InvalidParams`; no append (R-F1) |
| empty / oversize / control / invalid UTF-8 title | `InvalidParams`; no append (R-F2..R-F5) |
| unknown session | `UnknownSession` (R-F6/R-F12) |
| lease lost | `LeaseLost`; header/log unchanged (R-F7/RN13) |
| Automation profile | `session.rename` is **allowed** (not in the gated set) (RN-A9) |
| old-peer decode | a client that does not know `session/renamed` fails **loudly** on decode (S3/T-F20) (§5.5) |

### 12.5 UI unit / golden

| Test | Asserts |
|---|---|
| `command_registry` | `/rename` is listed by `/help`; `complete("re")` → `/rename`; `dispatch("/rename")` emits usage and makes **no** wire call |
| `dispatch("/rename my title")` | invokes `rename_session("my title")` |
| `ui_model_test.cpp` `SessionTitleChanged` | `setCellTitle` updates `SessionCell.title` and marks `Layout | SessionBar` (RN-A11) |
| `ui_event_adapter_test.cpp` | a `session/renamed` envelope adapts to exactly one `SessionTitleChanged` and no conversation entry |
| `ui_model_test.cpp` `ErrorOccurred` (M1) | `apply(ErrorOccurred{...})` appends exactly one `Role::System` entry carrying the message, sets `status.last_error`, marks `Conversation \| Status \| Attention`, and changes no title |
| golden | `render_header` shows the renamed title right-aligned, replacing the `"tui"` placeholder; the switcher leaf shows it too (RB-10) |

### 12.6 Integration / PTY

| Test | Asserts |
|---|---|
| PTY: prompt, then `/rename short-name` | the header updates to `short-name`; the rename is visible in `session.list` |
| PTY: `/rename` first, then prompt | the auto-name does **not** overwrite `short-name` (RN5) — the required end-to-end case |
| PTY: prompt only | the header shows the heuristic auto-name, not `"tui"` |
| PTY: Tab | `/re` + Tab → `/rename ` with a trailing space (RB-08) |
| integration: two supervisors, one workspace | A renames; B's header converges via the stream; a supervisor that attaches after the rename sees it via `session.list` (RN11) |
| integration: concurrent renames | two supervisors rename in one tick; both events are in the log; every subscriber converges to the highest-sequence title (RN10/R-F11) |
| integration: delete then rename | `session.rename` returns `UnknownSession` (R-F12) |
| integration: `/rename <oversize title>` (M1) | the daemon returns `InvalidParams`; the supervisor surfaces a `Role::System` notice (not silent) and the title is unchanged |
| integration: auto-name append failure (L7) | a store failure during the first-prompt auto-name is swallowed; the prompt still runs and the session keeps its placeholder title |

### 12.7 Failure-mode coverage matrix

| Failure | Test layer | Asserts |
|---|---|---|
| R-F1 | wire unit | non-string `title` → `InvalidParams`, no side effect |
| R-F2 | unit + wire | empty/whitespace → reject |
| R-F3 | unit | 121 bytes → reject; 120 bytes → accept |
| R-F4 | unit | `\n`/control → reject |
| R-F5 | unit | invalid UTF-8 → reject |
| R-F6 | unit + wire | unknown id → `UnknownSession`; auto-name no-op |
| R-F7 | integration | wire rename: lease loss → `LeaseLost`, unchanged header; auto path: swallowed, prompt proceeds |
| R-F8 | persistence | wire rename: injected failure → full rollback; auto path: swallowed, prompt proceeds |
| R-F9 | unit | auto after user rename → suppressed |
| R-F10 | persistence/replay | own log wins on reload; column unchanged by reload |
| R-F11 | integration | two renames → converge on highest sequence |
| R-F12 | integration | rename after delete → `UnknownSession` |
| RN3b | persistence + audit | no `sessions.title` writer beyond `create`/`append_batch_locked` |
| RN15 | persistence/replay | fork after a parent rename stays `""`; child rename leaves the parent untouched |
| M1 (error surfacing) | UI + integration | a rejected `session.rename` reply appends a `Role::System` notice via `ErrorOccurred`; never silently dropped |
| L7 (advisory fail-soft) | integration | auto-name `LeaseLost`/`StoreError` is swallowed + logged; the prompt proceeds |

---

## 13. Non-goals and open items

- No implementation code; no edits to `01-session.md`, `05-transport.md`, or any
  other spec. `DESIGN_STATUS.md`, `HANDOFF.md`, `AGENTS.md`, and
  `REQUIREMENTS_BACKLOG.md` are **not** touched by this errata.
- Out of scope: rename history UI, title uniqueness/search, LLM titling,
  cross-workspace rename, renaming a subagent from its parent, inline editor.
- Open for the Oracle gate:
  1. Confirm `RenameOrigin` should live in `payload` (beside the payload) rather
     than in `ymh` — chosen for symmetry with `SessionEndReason`/`TurnOrigin`.
  2. Confirm the `derive_auto_title` whitespace-collapse divergence from the
     headless `first_line` is acceptable (19-D12).
  3. Confirm no `kProtocolVersion` bump is acceptable given the fail-loud
     forward-incompatibility (§5.5).

  (The round-1 open item "confirm the daemon request-serialization claim" is
  **resolved**: re-derived in §1 from
  `src/transport/transport_server.cpp:257-266`,
  `src/transport/protocol_server.cpp:220-253,351`, and
  `src/host/workspace_host.cpp:349,482,558-561`, and the auto-name check no
  longer depends on it — `Session::appendAutoRename` is atomic, §5.3.)

---

## 14. Revision log

```
Round 0 (author) — initial draft, written against the working tree.
  All file:line citations re-read; RB-03 backlog line anchors corrected
  (supervisor.cpp:347 -> :689; ui_render.cpp:219 -> :467-483). One claim
  flagged unverified (daemon request serialization). No Oracle round yet.

Round 1 (author) — fixes for the failed gate:
  * HIGH #1: re-derived every citation from the CURRENT tree. The stale
    supervisor.cpp anchors (RB-07 /export landed in the same file) were
    corrected: :689->:719, :638->:668, :658->:688, :623-671->:653-701,
    :795-811->:901-918, :801-809->:907-915, :813-874->:920-933/:942-981,
    :855-860->:962-967, :1023-1034->:1130-1143. Also bumped the
    command_registry ranges (:15-24->:15-27, :113-184->:113-196) and
    event_test/events.cpp anchors.
  * HIGH #2: fixed the fork divergence. Title resolution is now over
    Session::ownEvents(), not the resolved view. RN2/RN3/RN4/RN5/RN10
    rewritten; RN3b (single writer) and RN15 (fork non-inheritance) added;
    decision 19-D14 added; §5.2 gained a proof against the real write path
    and an explicit residual window.
  * MEDIUM/LOW: corrected the persistence/replay test plan (fork
    non-inheritance, single-writer audit, own-log reconciliation), fixed the
    R-F9/R-F10 wording, and added the Citation audit (§15).

Round 2 (author) — fixes for the round-2 gate (R2-01…R2-06 + arbitrated C2):
  * R2-01: §5.2's residual window no longer implies reload repairs the persisted
    column. It states reload fixes only the in-memory header, the column is
    never repaired, and RN3b is the sole guarantee. §5.2 / R-F10 / §12.3 now
    agree.
  * R2-02: the auto-name check is pinned under the session append mutex. New
    Session::appendAutoRename performs the own-log scan + title read + append in
    one critical section (appendEvent is split into a locking wrapper and
    appendEventLocked); SessionManager::maybeAutoName delegates. §7.2 now argues
    the check, not just the append.
  * R2-03: first_text_of is pinned to the first Text content block of the
    message_from_json result (both string and object prompts covered).
  * R2-04: §5.4 pins the echoed result title to HostRuntime's own normalize_title
    output; SessionManager::renameSession still returns Sequence.
  * R2-05: added decision 19-D13 (atomic check) — numbering is contiguous.
  * R2-06: §1/§3's grep claim now lists the unrelated code/comment hits.
  * C2: §5.4 pins the method-catalog size relatively (29 + N, mirrors 18 §3.4.1),
    never an absolute 30.
  * Re-audit: verified the daemon request-serialization claim (was the round-1
    open item); added register row RN-A14; fixed RN-A7's "RN1–RN14" to
    "RN1–RN15 (incl. RN3b)"; added the R2-02 race test to §12.2.

Round 3 (author) — fixes for the round-3 gate (M1 + L1–L8):
  * M1: §6.1's sketch no longer passes a nullptr reply. The reply lambda is
    non-null, runs on the pump thread, marshals with enqueue (supervisor.cpp
    :488-496), and surfaces a rejected rename through the existing ErrorOccurred
    path (ui_model.cpp:641-648) on the UI thread. Rule 3 and the new
    "Error-surfacing mechanism" paragraph agree with the sketch; tests added to
    §12.5/§12.6/§12.7.
  * L1: HostNoticeKind anchor :232 -> :230-236 (protocol.hpp).
  * L2: AppCode anchor :169-193 -> :169-190 (191-193 are code_value).
  * L3: the quoted adapter default is now split: `default: return old;`
    (ui_event_adapter.cpp:75-76) and `default: break;` (:153-154).
  * L4: RN8 is scoped to rename-path titles; legacy/headless SessionStarted.title
    (headless.cpp:159) is explicitly excluded as unvalidated.
  * L5/L6: the normalize_title contract is pinned (trim set excludes '\n';
    control scan; full RFC 3629 acceptance; byte-length cap).
  * L7: the auto path is fail-soft — HostRuntime::agentPrompt swallows+logs
    StoreError (covers LeaseLost) so an advisory auto-name can never fail the
    prompt; R-F7/R-F8 and the §5.3 sketches say so.
  * L8: "`/r` is ambiguous" -> "unambiguous" (§6.2).
  * Re-audit: re-derived every touched anchor against the current tree; no
    sketch-vs-rule contradiction, overstated invariant, or unpinned contract
    remains.
```

---

## 15. Citation audit

**All `file:line` anchors in this spec were re-derived from the working tree on
2026-09-16**, after the round-1 gate failure, by grepping/reading each cited
file directly (not from any earlier draft). The **round-2 fixes re-derived every
anchor they touch, again on 2026-09-16**, and the **round-3 fixes re-derived
every anchor they touch, again on 2026-09-16** (M1 + L1–L8). `src/ui/supervisor.cpp`
is unchanged since `38c8a6214` (`git log` confirms it is the last commit to touch
the file), so its anchors did not move. Files whose anchors changed in round 1 are
listed below; all other files were re-checked and were unchanged.

| File | Round-0 anchor | Round-1 anchor (current) | Why |
|---|---|---|---|
| `src/ui/supervisor.cpp` | `:347`, `:638`, `:658`, `:689`, `:623-671`, `:795-811`, `:801-809`, `:813-874`, `:855-860`, `:1023-1034` | `:719`, `:668`, `:688`, `:653-701`, `:901-918`, `:907-915`, `:920-933`, `:942-981`, `:962-967`, `:1130-1143` | unrelated RB-07 (`/export`, commit `38c8a6214`) added ~107 lines, shifting all anchors |
| `include/ymh/ui/command_registry.hpp` | `:15-24`, `:15-30` | `:15-27` | `export_session` callback added (same commit) |
| `src/ui/command_registry.cpp` | `:113-184` | `:113-196` | `/export` builtin added (same commit) |

**Round-2 newly cited / re-derived anchors (2026-09-16).**

| File | Anchors used | What it backs |
|---|---|---|
| `src/execution/git_service.cpp` | `:118,127` | the unrelated `GIT_STATUS_*_RENAMED` grep hits (§1/§3, R2-06) |
| `include/ymh/agent/message.hpp` | `:13`, `:111-127` | the "must not rename" comment; `ContentBlock`/`Message` shape for `first_text_of` (§4.3, R2-03) |
| `src/host/host_runtime.cpp` | `:105-116`, `:822-849` | `message_from_json` (string ↔ `Message`); `map_store_error` mapping `std::invalid_argument` → `InternalError` (§4.3/§5.4) |
| `src/transport/transport_server.cpp` | `:65-83`, `:257-266` | the single io runner thread and synchronous read dispatch (§1, R2-02) |
| `src/transport/protocol_server.cpp` | `:220-253`, `:351` | synchronous request dispatch (§1, R2-02) |
| `src/host/workspace_host.cpp` | `:349`, `:482`, `:558-561` | one shared `io_`, no second `run()` (§1, R2-02) |
| `tests/unit/transport_protocol_test.cpp` | `:41` | the `29u` catalog-size assertion (§5.4, C2) |
| `src/ui/ui_event_adapter.cpp` | `:14` | `flatten_content` (§4.3) |

**Round-3 newly cited / re-derived anchors (2026-09-16).**

| File | Anchors used | What it backs |
|---|---|---|
| `include/ymh/transport/protocol.hpp` | `:169-190` (was `:169-193`), `:230-236` (was `:232`) | `AppCode` enum (L2); `HostNoticeKind` enum (L1) |
| `src/ui/ui_event_adapter.cpp` | `:75-76`, `:153-154` | the two non-exhaustive adapter defaults (L3) |
| `include/ymh/session/errors.hpp` | `:12`, `:43` | `StoreError` / `LeaseLost` for the fail-soft auto path (L7) |
| `include/ymh/core/logging.hpp` | `:24`, `:55` | `LogCategory` / `category_logger` (L7) |
| `src/ui/supervisor_connection.cpp` | `:339-374` | `process_requests` — the pump-thread reply invocation (M1) |
| `include/ymh/ui/ui_model.hpp` | `:375` | `UiModel::apply(const UiEvent&)` (M1) |
| `src/ui/ui_model.cpp` | `:641-648` | `ErrorOccurred` → `Role::System` entry (M1) |
| `include/ymh/ui/ui_event.hpp` | `:161-164` | `ErrorOccurred` struct (M1) |
| `src/ui/supervisor.cpp` | `:488-496`, `:723-760` | `enqueue`; `create_session`'s reply→enqueue precedent (M1) |
| `src/cli/headless.cpp` | `:159` | headless create title, unvalidated (RN8/L4) |

**Re-checked, unchanged (anchors remain valid):** `include/ymh/core/event.hpp`,
`src/core/event.cpp`, `include/ymh/session/events.hpp`, `src/session/events.cpp`,
`include/ymh/session/session.hpp`, `include/ymh/session/session_manager.hpp`,
`src/session/session.cpp`, `src/session/session_manager.cpp`,
`src/session/session_persistence.cpp`, `include/ymh/session/session_persistence.hpp`,
`src/cli/headless.cpp`, `src/ui/ui_model.cpp`, `src/ui/ui_render.cpp`,
`include/ymh/ui/ui_model.hpp`, `include/ymh/ui/ui_event.hpp`,
`src/ui/ui_event_adapter.cpp`, `src/ui/supervisor_connection.cpp`,
`include/ymh/transport/protocol.hpp`, `src/transport/protocol.cpp`,
`src/transport/protocol_server.cpp`, `src/host/host_runtime.cpp`,
`include/ymh/host/host_runtime.hpp`, `include/ymh/agent/workspace_runtime.hpp`,
`include/ymh/agent/compactor.hpp`, `src/ui/ui_application.cpp`,
`tests/unit/event_test.cpp`, `docs/design/01-session.md`,
`docs/design/05-transport.md`.

**Verification method.** `grep -n` / `read` against the live files for every
symbol named in a citation (e.g. `create_params{{"title"`, `refresh_sessions`,
`dispatch_command`, `ownEvents`, `resolve_after_locked`, `kWireNames`,
`kMethodCatalog`, `message_from_json`, `appendMutex_`, `io_->run()`). Any future
edit to `src/ui/supervisor.cpp`, `src/ui/command_registry.cpp`,
`src/transport/transport_server.cpp`, or `src/host/host_runtime.cpp` invalidates
the corresponding anchors; re-run this audit before the next gate round.
