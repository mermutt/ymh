# 74 — LLM Wire Transcript Errata: The `logging.llm_transcript` Key (spec-08 / spec-21 amendment)

```
Status: **verified (Rev 2)** · reviewer: independent review team, round 3 (2026-10-05): PASS · gate: 0 HIGH / 0 MEDIUM (6 LOW)
Verification status: verified (Rev 2) — `DESIGN_STATUS.md` row 74 tracks this spec as **verified (Rev 2)**; Rev 2 adds `74-D11`–`74-D14` (correlation-id source, `usage_missing` marker, oversized-marker field set) and passed an independent round-3 gate at 0 HIGH / 0 MEDIUM (6 LOW). Rev 1 carried an Oracle round-2 PASS at 0 HIGH / 0 MEDIUM.
Revision: Rev 2 — adds `74-D11` (adapter-minted monotonic `request_id`),
          `74-D12` (`usage_missing` response marker), `74-D13` (additive record
          field, no schema bump), `74-D14` (oversized-marker field set;
          `usage_missing` undefined on markers), with invariants
          `74-I11`–`74-I13` and tests `74-T10`–`74-T13`; supersedes the Rev-1
          field-source wording in §3.3 and the Rev-1 "the
          `LLMRequest::request_id`" request-id reading. Every Rev-1 pin (headers
          excluded, unredacted body, best-effort/never-throw, rotation) is
          retained unchanged. Rev 1 was the initial authoring: an opt-in,
          default-OFF, bounded, secrets-free JSONL transcript of the exact LLM
          request body and the exact response bytes, recorded at the
          OpenAI-compatible adapter seam.
Component: 74 (errata) — amends `08-llm-provider.md` §6/§8 and
           `21-config-jsonc-errata.md` §3.3/§7.6/§9/§10/§12 **by reference**; it
           does not edit `08` or `21` in place.
Amends:     `08` §6 (the adapter's request build / response decode path) and §8
            (its security rule): adds 74-D1–74-D14 (§3.1–§3.4, §11),
            74-I1–74-I13, 74-F1–74-F8, 74-T1–74-T13;
            `21` §3.3 (key table), §7.1 (top-level object), §7.6 (`logging`),
            §9 (adds `74-I*`), §10 (adds `74-F*`), §12 (adds `74-T*`).
            `21`'s in-place text is not rewritten (the `39` convention).
Retained:   `08`'s `LLMProvider` seam, its `L1`–`L17` invariants, its
            `L-F#` failure modes, the stream-event algebra, and `21`'s JSONC-only
            contract, strict unknown-key rejection, and layer order. This errata
            adds one opt-in sink, two keys, and a writer; it supersedes only the
            implicit "there is no wire body record" reading of `08` §8.
Depends on: `08-llm-provider.md` (verified) §6 :860-942, §8, §14.1;
            `21-config-jsonc-errata.md` (verified Rev 8) §3.3 :372-491,
            §7.6 :1237-1245, §9 :1628-1652, §10 :1657-1678, §12 :1699-1736;
            `26-dsh-alignment-part2.md` (verified Rev 7) 26-D23 :161, 26-I11;
            `39-session-persist-prompt-text-errata.md` (verified Rev 1);
            `00-architecture.md` §40 (logging security rule);
            the working tree at authoring time: `src/llm/openai_adapter.cpp`
            :969 (body builder `build_chat_completions_body`), :1143-1363
            (`OpenAICompatibleProvider::stream` incl. its retry loop),
            :1209-1216 (adapter request headers) / :1048-1051 (curl applies
            them),
            `include/ymh/llm/provider_registry.hpp` :27-43,
            `include/ymh/config/config.hpp` :202-207/:344-348,
            `src/config/config.cpp` :542-553/:1352-1373,
            `src/agent/workspace_runtime.cpp` :481-516/:596-651,
            `src/core/logging.cpp` (`log_prompts` sink wiring).
```

This document is an **errata**: it pins a new opt-in diagnostic surface that
ships no code until this spec is `verified`. It is written because a recurring
failure — a model answering with its base persona instead of the coding agent —
cannot be reproduced without the **exact bytes** `ymh` put on the wire, and no
existing key records them (`26-D23`/`39` record only the system-prompt text in
the session DB; `logging.log_prompts` writes redacted, truncated lines to
spdlog). The error below is the same class `21`/`39` fixed: a shipped surface
with no owning spec.

---

## 1. Purpose and the problem

The operator needs to diff *what he believes he sent* against *what the model
actually received*, verbatim, including the system prompt, every message, tool
definitions, and sampling parameters — and the full response, including any
identity/refusal text. The session event log is the authoritative *semantic*
trace, but it records assembled messages, not the wire body; it cannot show a
persona injected by a stale system prompt, a dropped tool, or a silently added
parameter. Only the adapter sees both the exact request bytes and the exact
response bytes (08 §6), so the record must be taken there.

## 2. Relationship to the existing prompt-capture keys

Three distinct mechanisms exist after this errata; they are **not** aliases.

| Mechanism | Layer | Destination | Content | Default |
|---|---|---|---|---|
| `logging.log_prompts` (existing) | global + workspace | spdlog (`debug`), **redacted + truncated** (`core/logging.hpp:60-62`) | already-redacted prompt excerpts | OFF |
| `session.persist_prompt_text` (existing, `39`) | **global only** | session DB `llm/request_header` | full rendered **system prompt** text | OFF |
| `logging.llm_transcript` (this errata) | **global only** | `<workspace>/.ymh/transcripts/*.jsonl` | **exact** request body + **exact** response bytes, headers excluded | OFF |

`74-D1`: none of the three enables another. In particular the transcript is
**not** routed through `log_prompt()` (it is not spdlog and is not redacted —
redaction would destroy the exact bytes the feature exists to capture), and it
does not write to the session DB (that would make a diagnostic into durable
state and inflate the store). `logging.log_prompts` cannot substitute: it
redacts and truncates. `session.persist_prompt_text` cannot substitute: it
stores only the system prompt, in the DB, not the wire body or the response.

## 3. Design decisions

### 3.1 Configuration surface (pinned)

Two new keys in the existing `logging` object (`21` §7.6):

| Key | Type | Default | Layer | Meaning |
|---|---|---|---|---|
| `logging.llm_transcript` | bool | `false` | **global only** | Opt in to the wire transcript. |
| `logging.transcript_dir` | string | `""` | **global only** | Directory for the JSONL file. Empty ⇒ `<workspace>/.ymh/transcripts/`. |

- `74-D2`: `llm_transcript` is **off unless an explicit truthy value** is
  present. No other key, no profile, no CLI flag, and no default turns it on.
- `74-D3`: both keys are **global-layer only**, mirroring
  `session.persist_prompt_text` (`21` §9, `39`). A workspace/cloned-repo
  `config.jsonc` that names either key fails with the existing global-only
  `ConfigError` (the `permissions.presets`/`session` precedent,
  `config.cpp:456,1361-1371`); this prevents a checked-out repository from
  causing the operator's full prompts to be written to a repo-local path.
- `74-D4`: `transcript_dir` is resolved as-is when non-empty (tilde/relative
  paths are **not** expanded — it must be an absolute path or a path relative to
  the process cwd, matching every other path-valued key); when empty the
  workspace runtime derives `<workspace-root>/.ymh/transcripts/`.
- `74-D5`: the env mirror is `YMH_LLM_TRANSCRIPT` (truthy) and
  `YMH_LLM_TRANSCRIPT_DIR` (string), applied in the env layer **after** the file
  layers, exactly like `YMH_LLM_LOG_PROMPTS` (`config.cpp:1863-1865`). Because
  the env layer is process-wide, `YMH_LLM_TRANSCRIPT=1` enables the transcript
  even though the file key is global-only.

### 3.2 The seam (pinned)

- `74-D6`: recording happens **only** in
  `OpenAICompatibleProvider::stream()` (`src/llm/openai_adapter.cpp:1143-1363`,
  the retry loop included), the one function that builds the exact body
  (`build_chat_completions_body`, `:969`) and owns the transport call
  (`postStream`; the request headers are applied at `:1048-1051`). The agent
  loop is not instrumented (it cannot see the wire body). No other adapter
  exists in `main`'s built-ins
  (`register_builtin_providers`).
- `74-D7`: the writer is a new class `ymh::LlmTranscript`
  (`include/ymh/llm/llm_transcript.hpp`), held as
  `std::shared_ptr<LlmTranscript>` on `LLMProviderConfig`
  (`include/ymh/llm/provider_registry.hpp`). `nullptr` ⇒ disabled ⇒ the adapter
  performs one `if (transcript_)` test per attempt and nothing else. One writer
  instance per workspace runtime is shared by every per-endpoint provider
  (`workspace_runtime.cpp:481-516`) so a single mutex serializes appends and the
  file set stays bounded.

### 3.3 Content (pinned)

Each dispatched HTTP attempt produces up to two JSONL records: a request record
before the transport call and a response record once the attempt completes
(transport, decode, or HTTP error). A cancel/abort that unwinds before the
response is recorded may leave the request record alone.

**Request record** (`direction:"request"`):

| Field | Value |
|---|---|
| `ts` | RFC 3339 UTC, millisecond precision |
| `direction` | `"request"` |
| `pid` | process id (distinguishes daemon runs sharing a stable filename) |
| `request_id`, `attempt` | the **adapter-minted** id and 1-based attempt. **Superseded by `74-D11`**: the Rev-1 source `LLMRequest::request_id` is dead (`= 0`, never assigned) and is not read; the id is minted from the adapter's process-local sequence. |
| `model` | the effective model id sent (`body.model`) |
| `endpoint_host` | scheme-stripped `host[:port]` derived from the URL |
| `body_bytes` | byte length of `body` |
| `body` | the **exact** `payload` string handed to the transport (full JSON: `model`, `messages`, `tools`, sampling params) |
| `truncated` | `true` iff `74-D10` replaced the record with the oversized marker |

**Response record** (`direction:"response"`):

| Field | Value |
|---|---|
| `ts`, `direction`, `pid`, `request_id`, `attempt`, `model`, `endpoint_host` | as above |
| `status` | HTTP status (`0` on transport failure) |
| `raw_bytes` | byte length of `raw` |
| `raw` | the **exact** concatenated bytes the body sink received (for streaming: every SSE frame, `[DONE]` included) |
| `assembled` | object: `content` (all `TextDelta` concatenated), `reasoning` (all `ReasoningDelta`), `tool_calls` (the finalized `ToolCallAssembled` list), `finish_reason` (lowercase wire token or `null`), `usage` (object or `null`) |
| `truncated` | `true` iff `74-D10` replaced the record with the oversized marker |

- `74-D8`: **streaming and non-streaming.** The response is always the raw
  bytes, so both work. For a backend that ignores `stream:true` and returns a
  single JSON body, `raw` is that exact body and the adapter parses
  `choices[0].message` (or `.delta`) best-effort to fill `assembled`; when the
  body is SSE, `assembled` comes from the decoder's own events. Parsing never
  affects the returned `LLMResponse`.
- `74-D9`: **headers are excluded by construction.** The record builder never
  receives the `HttpRequest::headers` vector; it receives the body string and
  the URL only. `Authorization`, `api_key_env` **names**, and `api_key` **values**
  are therefore absent from every record. `endpoint_host` is derived from the
  URL, never from a header. A test asserts the secret's absence (`74-T3`).

### 3.4 Bounding and rotation (pinned)

- `74-D10`: one stable file per transcript directory: `llm-transcript.jsonl`.
  On startup the writer seeds its byte counter from the existing file size.
  Before appending a line, if `bytes_written + line_bytes >
  transcript_max_bytes_per_file` the writer rotates: the oldest retained file
  `llm-transcript.(max_files-1).jsonl` is deleted, each `.(i-1)` is renamed to
  `.i`, `llm-transcript.jsonl` → `.1`, and a fresh current file is opened (with
  the default `max_files = 4` that is `.3` deleted and `.2`→`.3`, `.1`→`.2`,
  current→`.1`). The retained set is the current file plus `.1`…`.(max_files-1)`,
  so the bound is `transcript_max_bytes_per_file × transcript_max_files`.
  Defaults: 16 MiB and 4 files ⇒ a hard 64 MiB ceiling per workspace,
  independent of session length or number of daemon runs.
- A single serialized record that alone exceeds the per-file cap is not written
  in full: it is replaced by a small marker record carrying `"truncated": true`
  and `"note"` (so one gigantic image body cannot escape the bound). This is the
  only lossy path and it is flagged in the record; every ordinary record is
  written verbatim with `"truncated": false`.

## 4. State and lifetime

| State | Created | Destroyed / evicted | Owner | Survives restart? |
|---|---|---|---|---|
| `LlmTranscript` object | `WorkspaceRuntime::create`, when `logging.llm_transcript` is truthy | with the runtime/daemon | `shared_ptr` on `LLMProviderConfig`, shared by all endpoint providers | no (rebuilds from config) |
| `llm-transcript.jsonl` (+ `.1`…`.N-1`) | first record | rotation deletes the oldest; process exit leaves files | operator (diagnostic artefact) | yes (bounded, intentionally) |
| `bytes_written_` counter | writer ctor (seeded from `file_size`) | writer dtor | writer | no (re-seeded) |
| `transcript_max_*` | compile-time defaults in `LlmTranscriptOptions` | n/a | writer | n/a |

The transcript is **best-effort diagnostic state**: losing it, failing to open
it, or rotating early never changes a turn's outcome (`74-I7`).

## 5. Invariants

- `74-I1` **Default OFF.** With no explicit opt-in, no transcript file is
  created and no record is written (`74-T2`).
- `74-I2` **Exactness.** When enabled, the request record's `body` equals the
  exact string passed to `HttpTransport::postStream`, and the response record's
  `raw` equals the exact concatenation of the bytes passed to the body sink
  (asserted by equality, `74-T1`, `74-T5`).
- `74-I3` **No secrets.** `api_key`, the `Authorization` header, and the value
  of `api_key_env` never appear in any record (`74-T3`). This holds structurally
  because headers are not an input to the record builder (`74-D9`).
- `74-I4` **Bounded.** The retained byte total per directory never exceeds
  `transcript_max_bytes_per_file × transcript_max_files` (`74-T4`).
- `74-I5` **Streaming complete.** A streamed response is recorded in full:
  `raw` contains every frame and `assembled.content` equals the concatenation of
  every text delta (`74-T5`).
- `74-I6` **Non-streaming works.** A non-SSE JSON body is recorded verbatim in
  `raw` and its `choices[0]` content is best-effort assembled (`74-D8`).
- `74-I7` **Best-effort.** No filesystem operation in the transcript path throws;
  an open/write/rotate failure disables further writes for that writer (a single
  content-free `debug` line may be emitted) and the stream outcome is unaffected
  (`74-F1`–`74-F4`).
- `74-I8` **Zero overhead when off.** Disabled, the only cost is one shared-ptr
  null check per attempt; no file is opened, no string is copied, no clock is
  read.
- `74-I9` **Global-layer only.** A workspace-layer `config.jsonc` naming either
  key is a `ConfigError` (`74-D3`).
- `74-I10` **Not spdlog.** Transcript content never reaches spdlog; the writer
  emits at most a content-free failure diagnostic (`74-I7`).

## 6. Failure modes

| ID | Condition | Behaviour |
|---|---|---|
| `74-F1` | `create_directories` fails (permission, ENOSPC) | writer disables itself; no throw; turn continues; no spdlog content (`74-I7`) |
| `74-F2` | file open fails mid-run | that record is skipped; writer disables; turn continues |
| `74-F3` | rotate rename fails | current file is truncated in place and the counter reset; bound preserved |
| `74-F4` | write short-writes / disk full | bytes actually written are counted; writer disables; turn continues |
| `74-F5` | record line exceeds the per-file cap | replaced by a small `"truncated":true` marker record; the full payload is not written |
| `74-F6` | response is not valid JSON and not SSE | `raw` recorded verbatim; `assembled` left empty; the turn's own decode result is unchanged |
| `74-F7` | `YMH_LLM_TRANSCRIPT=0` while config is true | env layer wins (env is last), transcript disabled |

## 7. dsh mapping

| dsh concept | ymh mirror | Non-mirror justification / anchor |
|---|---|---|
| `GenerateOptions` request capture | request `body` verbatim | Mirrored. The adapter is the only wire owner (`74-D6`, 08 §6). |
| provider debug/trace hook | `LlmTranscript` JSONL file sink | Divergence: dsh has no first-class on-disk wire transcript; ymh needs a no-spdlog, operator-diffable artefact for the persona bug. Reason: architectural absence of a durable wire record (session log stores assembled messages, `00` §12); seam `OpenAICompatibleProvider::stream`, missing consumer filled by the operator. `74-D7`. |
| redacted prompt logging | **not** used for the transcript | Deliberate scope: redaction/truncation defeats exactness; `logging.log_prompts` remains the spdlog-only redacted path (`core/logging.hpp:60`). Anchor `74-D1`, 26-D23. |

## 8. Test plan

Hermetic, via the existing `FakeTransport` in
`tests/unit/openai_adapter_test.cpp` (no network, no real LLM).

- `74-T1` **Enabled round-trip equality.** Enable with a temp dir; run one
  streaming request; assert the request record `body` equals
  `build_chat_completions_body(...).dump()` (exact sent messages/tools/params)
  and the response `raw` equals the scripted SSE bytes.
- `74-T2` **Default OFF.** Construct without a transcript; run a request; assert
  no file exists anywhere under the temp root.
- `74-T3` **Secret absence.** Set `YMH_TEST_API_KEY` to a sentinel via
  `scoped_env`; enable; run; assert the sentinel, `"Authorization"`, `"Bearer"`,
  and `"api_key_env"` do not occur in the file bytes.
- `74-T4` **Bounded rotation.** Configure small caps and write enough records to
  cross them; assert the retained file count ≤ `max_files` and every retained
  file ≤ `max_bytes_per_file`.
- `74-T5` **Streamed completeness.** Script the SSE with `chunk_size=1`; assert
  `raw` equals the full body and `assembled.content` equals the exact
  concatenation of the deltas.
- `74-T6` **Config parsing (`tests/unit/config_test.cpp`).** `logging.llm_transcript`
  and `logging.transcript_dir` parse from the global layer; a workspace-layer
  occurrence fails with the global-only `ConfigError`; `YMH_LLM_TRANSCRIPT`
  enables and `YMH_LLM_TRANSCRIPT=0` disables.
- `74-T7` **Catalog + drift.** `config::logging.llm_transcript` and
  `config::logging.transcript_dir` have catalog entries pointing at this spec;
  the tracker row and this header agree.
- `74-T8` **Best-effort.** A writer whose directory cannot be created is
  disabled; the provider still completes the turn and never throws (`74-I7`,
  would catch an escaping serialize/alloc failure).
- `74-T9` **Non-streaming.** A `200` JSON (non-SSE) body is recorded verbatim in
  `raw` and `choices[0].message.content` fills `assembled.content`, with no
  change to the returned `LLMResponse` (`74-D8`, `74-I6`).

Pre-fix evidence (captured): `74-T2` passes trivially pre-fix (nothing exists);
`74-T1`, `74-T3`–`74-T5`, `74-T8`, `74-T9` fail pre-fix once they assert a
non-empty request record (the writer created an empty file but the provider
never recorded); `74-T6` fails pre-fix (unknown key ⇒ `ConfigError`).

## 9. Rejected alternatives

1. **Reuse `logging.log_prompts`.** Rejected: spdlog-only, redacted, truncated,
   no response (`74-D1`).
2. **Extend `session.persist_prompt_text`.** Rejected: session DB, system prompt
   only, wrong lifetime and wrong content (`74-D1`, `39`).
3. **Record from the agent loop.** Rejected: the loop never sees the wire body;
   recording there would record a reconstruction, not the bytes (`74-D6`).
4. **Record headers, then redact `Authorization`.** Rejected: a denylist can
   miss a custom auth header; excluding all headers is structural, not
   pattern-based (`74-D9`).
5. **Unbounded append.** Rejected: a long session or a loop could fill the disk
   (`74-D10`).
6. **Fail the turn when the transcript cannot be written.** Rejected: a
   diagnostic must never break the agent (`74-I7`).

## 10. Open items

- `74-OQ-1`: the per-file cap and file count are compile-time defaults
  (`16 MiB × 4`); they are not operator-configurable in this revision. A
  follow-up may add `logging.transcript_max_bytes` / `transcript_max_files` if
  operators need tuning.
- `74-OQ-2`: the filename is stable per directory, not per session; sessions are
  distinguished by the `request_id`/`timestamp` fields. Per-session files were
  deferred because providers are constructed before a session is selected and a
  daemon serves multiple sessions. A follow-up may add the session id to the
  record once it is threaded through `LLMRequest`.

---

## 11. Rev 2 — correlation id and `usage_missing` (pinned)

This section is additive. It supersedes only two Rev-1 readings: (a) §3.3's
request/response `request_id` rows, which named `LLMRequest::request_id` as the
source, and (b) the absence of an explicit usage-absence marker in the §3.3
response table. Every other Rev-1 pin stands unchanged: headers are never
recorded (`74-D9`/`74-I3`), the body/`raw` are unredacted (`74-D1`/`74-I2`),
the writer is best-effort and never throws (`74-I7`), and rotation is bounded
(`74-D10`/`74-I4`).

Defect observed in the shipped Rev-1 capture: `LlmTranscript::record_request`/
`record_response` receive `request.request_id` (`src/llm/openai_adapter.cpp:1283,
1325,1335`), but `LLMRequest::request_id` is declared `= 0` and **never
assigned** anywhere in `src/` or `include/` (it is a dead field pinned additively
by `08` §14.1(f), `include/ymh/llm/llm_request.hpp:61`). Every record therefore
carried `request_id: 0` and a request could not be paired with its response; the
capture had to be paired by file ordinal.

### 11.1 Correlation id source (pinned)

- `74-D11`: the transcript's `request_id` is minted by the adapter from a
  **process-local monotonic sequence** and is independent of the (dead)
  `LLMRequest::request_id`. `LlmTranscript::record_request`/`record_response`
  keep their existing `std::uint64_t request_id` parameter; only the value's
  source changes.

  Interface change (exact):
  - `include/ymh/llm/openai_adapter.hpp`: add `#include <atomic>`; in the private
    section of `class OpenAICompatibleProvider`, after `transcript_` (`:110`):
    ```cpp
    // 74-D11: process-local monotonic correlation id source. One per daemon
    // process, shared by every provider instance, so ids are unique within the
    // workspace transcript file. 0 is reserved and is never minted.
    static std::atomic<std::uint64_t> request_seq_;
    ```
  - `src/llm/openai_adapter.cpp`: at namespace scope, the definition
    ```cpp
    std::atomic<std::uint64_t> OpenAICompatibleProvider::request_seq_{0};
    ```
  - `OpenAICompatibleProvider::stream()` (`:1143`): as the first action —
    before the `failed` (`:1151`) and `cancelled` (`:1163`) closures and before
    the retry loop (`:1242`) — mint exactly once:
    ```cpp
    const std::uint64_t request_id =
        OpenAICompatibleProvider::request_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    ```
    `fetch_add(...) + 1` starts at 1 so `0` stays the "unset" sentinel and is
    never minted. `memory_order_relaxed` suffices: the id publishes no other
    memory.

  Concrete callers of the minted value:
  - `:1283` `transcript_->record_request(request_id, attempt, ...)`
  - `:1324` / `:1335` `transcript_->record_response(request_id, attempt, ...)`
  - `:1158` / `:1167` `response.request_id = request_id;` (the `failed` and
    `cancelled` closures capture `request_id` by value) and `:1321`
    `decoded.request_id = request_id;`

  so `LLMResponse::request_id` (`include/ymh/llm/llm_provider.hpp:57`) returned
  to the caller equals the id the transcript records.

  Id lifetime and record multiplicity: one `stream()` invocation is one logical
  request, and the id is minted **once, outside** the `for (attempt ...)` loop
  (`:1242`). The write path is **request-per-attempt**: `record_request`
  (`:1282-1285`) is called *inside* the loop, once for every dispatched attempt,
  so a retried call yields **multiple request records** — one per attempt — with
  the **same** `request_id` and increasing `attempt` (`1`, `2`, ...). Each
  attempt's response record (`:1324`/`:1335`) carries that same id and the same
  `attempt` as the request it answers. A logical request retried N times
  therefore contributes up to N request records and N response records, all
  correlated by one id. A consumer groups by `request_id` and orders by
  `attempt`; correlation is id-scoped across the whole retry sequence, not one
  request record paired with one response record. (The oversized marker is the
  one exception — see `74-D14`.)

  Rejected alternative: populate `LLMRequest::request_id` at the build site.
  Rejected because (1) there is more than one `LLMRequest` construction site —
  `AgentLoop::buildRequest` (`src/agent/agent_loop.cpp:733`) and the compactor
  summarizer (`src/agent/compactor.cpp:313`) — and any future caller (title,
  trim, ...) must remember to mint; the shipped defect is precisely that no site
  does, so moving the mint to a caller re-creates the same class of bug for the
  next caller. (2) The adapter is already pinned as the only wire owner
  (`74-D6`) and is the only place that knows how many attempts a logical request
  produced; minting there makes request↔response correlation structural rather
  than a per-caller convention, and keeps the frozen request
  (`FrozenRequest::freeze`, `src/llm/llm_runtime.cpp:151`) free of per-call
  mutable data.

### 11.2 `usage_missing` field and the oversized marker (pinned)

- `74-D12`: the response record gains one top-level boolean, `"usage_missing"`:

  | Field | Value |
  |---|---|
  | `usage_missing` | `true` iff no provider usage object was assembled; `false` iff a usage object (possibly all-zero) was present |

  Placement: a sibling of `"assembled"` and `"truncated"` in the response record
  built by `LlmTranscript::record_response`
  (`src/llm/llm_transcript.cpp:110`; the shipped `assembled` field is `:132` and
  `truncated` is `:133`), added in source order after `"assembled"`. This is a
  source-list placement only: `nlohmann::json` serializes object keys
  lexicographically, so in the written JSONL line `"usage_missing"` sorts after
  every other key (last); its byte position in the line is not significant.

  Computed at the single write site from the existing `assembled` argument (no
  new parameter, so the two adapter call sites cannot drift):
  ```cpp
  const bool usage_missing =
      !(assembled.contains("usage") && assembled["usage"].is_object());
  ```
  This is exact for both call sites: the success path passes
  `assembled_response_json(...)` (`src/llm/openai_adapter.cpp:113`), which always
  sets `assembled["usage"]` to an object or `null` (`:122`); the error path
  (`:1336`) passes `nlohmann::json::object()`. An endpoint that ignores
  `stream_options.include_usage` (`:994-996`) leaves `response.usage` empty, so
  the record carries `"usage": null` **and** `"usage_missing": true`; a provider
  that returned a zero-valued usage object yields `false`. A reader can thus
  distinguish "no usage returned" from "usage returned as zero".

  Lifetime: `usage_missing` exists only as a local in `record_response`, is
  serialized into one JSONL line, and is not retained or cached.

- `74-D13`: the field is purely additive; there is no record `schema_version`
  field to bump. A Rev-1 record (written before this revision) simply lacks
  `"usage_missing"`; a reader must treat an absent field as *unknown*, not as
  `false`.
- `74-D14`: the **oversized marker** that `LlmTranscript::append()` substitutes
  when one serialized record alone exceeds `max_bytes_per_file`
  (`src/llm/llm_transcript.cpp:152-163`, `74-D10`/`74-F5`) is deliberately
  minimal. It carries exactly these fields and no others:

  | Marker field | Value |
  |---|---|
  | `ts` | copied from the replaced record (`record.value("ts", "")`) |
  | `direction` | copied from the replaced record (`"request"` or `"response"`) |
  | `pid` | process id |
  | `truncated` | `true` (the sole discriminator of a marker) |
  | `note` | `"record exceeded per-file cap"` |
  | `original_bytes` | size of the dropped serialized line |

  The marker carries **no** `request_id`/`attempt`, **no** `body`, **no** `raw`,
  **no** `assembled`, and **no** `usage_missing`. Two consequences are pinned:
  (a) a marker does **not** participate in `74-D11` correlation — it is a loss
  event, not a correlated request/response record, and a reader detects it only
  by `truncated == true`; (b) `usage_missing` is defined **only** on an ordinary
  (`truncated:false`) response record and is never present on a request record
  or on a marker, so an absent `usage_missing` means *not applicable*, never
  `false`. This is descriptive of the Rev-1 builder; Rev 2 pins it so a reader
  is never ambiguous about the marker's shape.

### 11.3 State and lifetime (Rev 2 additions)

| State | Created | Destroyed / evicted | Owner | Survives restart? |
|---|---|---|---|---|
| `OpenAICompatibleProvider::request_seq_` (`static std::atomic<std::uint64_t>`) | static init before `main`, value `0` | never (process lifetime) | `OpenAICompatibleProvider` (one per daemon process, shared by all instances) | no — restarts at `0`; the record's `pid` field (§3.3) distinguishes daemon runs sharing one directory |
| minted `request_id` | first statement of `stream()` | end of the `stream()` invocation (stack) | the invocation | no |
| `usage_missing` | each `record_response` call | after the record is serialized (local) | none (record field) | n/a — the value is baked into the JSONL line |

Uniqueness scope: within one daemon process, ids are `1,2,3,...`; across
processes the `pid` field disambiguates; `0` is never minted for an
adapter-written record.

### 11.4 Invariants (Rev 2 additions)

- `74-I11` **Correlated.** Every ordinary (`truncated:false`) record the adapter
  emits for one `stream()` invocation carries the same nonzero `request_id`,
  minted once per invocation from the process-local sequence before the retry
  loop; a retried invocation emits one request record and one response record
  per attempt, all sharing that id with distinct `attempt` values; two distinct
  invocations get distinct ids; the value is never read from
  `LLMRequest::request_id`; `LLMResponse::request_id` returned to the caller
  equals it (`74-T10`, `74-T11`). The oversized marker (`74-D14`) is not an
  ordinary record and is out of scope for this invariant.
- `74-I12` **Usage absence is explicit.** A response record whose assembled usage
  is absent/null carries `"usage_missing": true`; a record with a usage object
  (even all-zero) carries `false` (`74-T12`).
- `74-I13` **Marker is unambiguous.** An oversized marker (`74-D14`) always
  carries `truncated: true` and only the six pinned fields; it never carries
  `assembled` or `usage_missing`, and `usage_missing` never appears on a request
  record. A reader distinguishes a marker from a normal record solely by
  `truncated`; an absent `usage_missing` is *not applicable*, never `false`
  (`74-T13`).

### 11.5 Failure modes (Rev 2 additions)

| ID | Condition | Behaviour |
|---|---|---|
| `74-F8` | provider returns no usage object (e.g. it ignored `stream_options.include_usage`) | response record is still written; `assembled.usage` is `null` and `usage_missing` is `true`; the turn and the returned `LLMResponse` are unaffected (`74-I7`) |

### 11.6 Test plan (Rev 2 additions)

Tests live in the existing `LlmTranscriptTest` fixture in
`tests/unit/openai_adapter_test.cpp` (`:881-1000`); the `read_records(...)`
helper (`:904`) already parses the JSONL.

- `74-T10` **Distinct ids.** In `LlmTranscriptTest`
  (`tests/unit/openai_adapter_test.cpp`), enable the transcript; run two
  `stream()` invocations (two scripted responses) through one provider; assert
  both request records carry a nonzero `request_id`, the two ids differ, and each
  response record's `request_id` equals its request's.
- `74-T11` **Shared across attempts (request-per-attempt).** Model the script on
  the shipped retry fixture `OpenAiAdapterTest.RetriesBeforeFirstEventThenSucceeds`
  (`tests/unit/openai_adapter_test.cpp:448`; `429` with `"{}"` then `200` with an
  SSE body, `:449-457`; the fixture already asserts two dispatched requests at
  `:462`), but construct the provider with `Options{.transcript = transcript}`.
  Read the records and assert: **four** records total, in order
  `request@1, response@1, request@2, response@2`; both request records and both
  response records share **one** nonzero `request_id`; the two request records
  carry `attempt` `1` and `2`; each response record's `attempt` equals its
  request's; and the returned `LLMResponse::request_id` equals that id.
- `74-T12` **`usage_missing` true/false.** (a) Script an SSE body with no usage
  frame ⇒ response record `usage_missing == true` and `assembled.usage == null`.
  (b) Script an SSE body with a usage frame ⇒ `usage_missing == false` and
  `assembled.usage` is an object carrying the parsed token counts. (c) A
  non-streaming `200` JSON body with `usage` ⇒ `false` (paired with `74-T9`).
- `74-T13` **Oversized marker shape.** Enable the transcript with a tiny
  `LlmTranscriptOptions{.max_bytes_per_file = ...}` so one record exceeds the cap;
  assert the substituted record has `truncated == true`, carries only the
  `74-D14` fields (`ts`, `direction`, `pid`, `note`, `original_bytes` plus
  `truncated`) and `direction` matches the dropped record, and has **no**
  `assembled`, **no** `usage_missing`, and **no** `request_id`/`attempt`.

Rev-1 tests are unaffected: `74-T1`/`74-T5` assert `body`/`raw` equality, not
ids.

### 11.7 Open item (Rev 2)

- `74-OQ-3`: `LLMRequest::request_id`
  (`include/ymh/llm/llm_request.hpp:61`) remains unassigned. This revision
  deliberately does not repopulate it (the adapter owns the id); if a caller-side
  id is ever needed, thread the adapter-minted id back through `LLMResponse` —
  do not add a second minting site.
