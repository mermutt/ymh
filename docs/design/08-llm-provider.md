# 08 — LLM Provider

**Component 08 of 10.** The LLM seam is the boundary between the agent loop and
any model: a single streaming-first `LLMProvider` interface (`§12`) with
provider-specific HTTP/SSE adapters behind it (`§13`) and a deterministic
`FakeLLM` (`§45`) that implements the same seam for offline tests. This document
pins the request/response types, the stream-event algebra, cancellation, token
accounting, retry/backoff, model selection, credentials, the OpenAI-compatible
adapter as the first concrete provider (`§57` Step 7), the `FakeLLM` test
double, invariants (`L1`–`L17`), failure modes (`F1`–`F12` plus local `L-F#`),
the DeepSeek Harness (dsh) mapping, and the test plan.

It follows `00-architecture.md` (cited inline as `§n`), `01-session.md`
(`01 §n`), `02-persistence.md` (`02 §n`), `03-workspace-registry.md`
(`03 §n`), and `04-workspace-host-daemon.md` (`04 §n`). Where it cannot follow
them it records the conflict under §14 rather than choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

> **Naming note.** The invariants in this spec are numbered `L1`, `L2`, … (for
> "LLM"), so they cannot collide with the `D1`–`D23` design decisions in
> `00-architecture.md` §54 or with the `I#`/`S#`/`P#`/`R#`/`H#` namespaces of
> specs 01–04. Architecture decisions are always written with the `§54` prefix
> (`§54 D5`) and LLM invariants bare (`L3`). Component-local failure modes are
> `L-F#` and cannot collide with the shared `F1`–`F12`.

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
   Agent Loop (spec 06, §11)                         FakeLLM (tests, §45)
        │  LLMRequest{messages, tools, params}             │ same seam
        │  StreamSink  ▲   StreamEvent                     │
        ▼              │                                    ▼
   ┌──────────────────────────────────────────────────────────────┐
   │  LLMProvider seam  (this spec, §12)                           │
   │    stream(request, sink, cancel) -> Task<LLMResponse>          │
   │    provider-agnostic StreamEvent algebra · Usage · LLMError    │
   └───────┬───────────────────────────────┬──────────────────────┘
           │                               │
           ▼                               ▼
   OpenAICompatibleProvider          (future) Anthropic / Ollama /
     │  HttpTransport seam                    vLLM adapters
     ▼
   libcurl (v1)  ──►  HTTPS/SSE  ──►  provider endpoint
```

The seam is **inside the workspace daemon** (spec 04): each daemon owns its LLM
pool and its sessions. Spec 08 owns the `ProviderRegistry` and constructs the
`LLMProvider` from config; spec 04 supplies `LLMProviderConfig` and exposes no
provider handle of its own (`04 §8`); spec 06 owns the resolved provider
instance and schedules `stream()` on the daemon's `LLMPool` (§9.6, §9.11,
`04 §8`, decision (n)). The provider is a leaf: it never touches the session
log, the store, the registry, or the `EventBus` (L1). The agent loop is the
producer that turns stream events into durable events (`01 §4.5`,
`01 §16.1(c)`).

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 08)

This spec pins:

- The `LLMProvider` seam: request/response types, `stream()`, the
  `StreamSink`, and the `StreamEvent` algebra (`§12`).
- The streaming model and its mapping onto durable events: `AssistantChunk`
  coalescing, tool-call assembly, `Usage`/`TokenUsage`, and cancellation
  (`01 §4.5`, `§33`, `§34`).
- Model selection: provider registry, effective-model resolution from
  `SessionHeader.model` and layered config (`§37`, `§38`), and generation
  parameters.
- Error taxonomy, retry/backoff, timeouts, and resource-cap interaction
  (`§9.11`, F8).
- The OpenAI-compatible HTTP adapter as the **first and only v1 provider**
  (`§57` Step 7), and the adapter/transport seam that lets others be added
  without changing `LLMProvider`.
- `FakeLLM` (`§45`) as the deterministic test double, including its scripted
  tool-call transcript.
- Credential/secret handling: keys never logged, prompts never logged by
  default (`§40`, `§46`).

### 1.3 Boundaries — deferred to other specs

- **Coalescing cadence.** The exact `AssistantChunk` batch size / flush policy
  is owned by specs 02 and 06 (`01 §16.1(c)`); this spec pins the *shape* of
  the delta→chunk mapping and the rule that the provider does not coalesce.
- **The LLM worker pool.** The concrete `LLMPool`/`ResourceGovernor` type is
  owned by spec 04 (governor) and spec 06 (pool handle) (`04 §8`, `§9.11`);
  this spec pins only the provider-side contract (no internal threads, L13).
- **Tool schemas and validation.** `ToolSchema` and tool-name validation are
  owned by spec 07 (`§14`); this spec transports only the provider-visible
  fields (name/description/input_schema).
- **Context assembly / compaction.** System prompt, history selection, and
  summarization are owned by spec 06 (`§31`, `§32`); this spec receives an
  already-assembled `messages` list and reports `ContextLengthExceeded`.
- **Permissions.** Whether a streamed tool call may execute is spec 09
  (`§19`); the provider only surfaces the call.
- **The TUI rendering of streamed text.** Owned by spec 10 (`§20`); the
  provider emits core `StreamEvent`s, never `UiEvent`s (`§54 D15`, `§20.6`).

### 1.4 Seam ownership relative to 01/04/06/07

| Concern | Owner | This spec's role |
|---|---|---|
| `Message`/`Role`/`ContentBlock`/`Usage` types | 01 (`§12`, `§33`, reused per `01 §4.5`) | reused verbatim, never redefined |
| `AssistantChunk`/`AssistantMessage`/`TokenUsage` durability | 01 | defines the delta→event mapping contract |
| `TurnCancelled`/`TurnFailed` on cancellation/failure | 01/06 (`01 §4.5`, `§34`) | observes the token; never appends |
| `LLMPool` / per-host caps | 04/06 (`04 §8`, `§9.11`) | provider owns no executor/threads (L13) |
| `ToolSchema`, tool execution | 07 (`§14`) | transports schemas; never executes |
| Durable append / `EventBus` publish | 01/02 | never touched by a provider (L1) |
| HTTP transport | **08 (this)** | `HttpTransport` seam + libcurl v1 |
| Provider registry, model selection | **08 (this)** | pinned here |
| `FakeLLM` | **08 (this)** | pinned here (`§45`) |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`ToolCallId`, `MessageId`, `TurnId`, `StepId`, `SessionId`, and `Usage` are
frozen by `01 §2.1` and `01 §4.5` / `§12` / `§33`; they are reproduced for
reference and **not** redefined. LLM-local types:

```cpp
namespace ymh {

// Registry key and opaque model id. Neither is a path; neither is validated
// against a fixed list (a local endpoint may serve any model).
using ProviderId = std::string;   // e.g. "openai-compatible"
using ModelId    = std::string;   // e.g. "gpt-4o-mini", "qwen2.5-coder"

// Per-process monotonic correlation id. Used only for logs/diagnostics
// (§40) and to correlate a request with its retries. Never persisted.
using RequestId  = std::uint64_t;

// Why the model stopped producing tokens (§12). Total over the providers we
// support; `Other` is the forward-compatible escape hatch.
enum class FinishReason : std::uint8_t {
    Stop,           // natural end of turn
    Length,         // max_output_tokens reached
    ToolCalls,      // model requested one or more tools
    ContentFilter,  // provider-side refusal/filter
    Error,          // stream ended because of an error
    Other,          // provider-specific reason; mapped conservatively
};

// Terminal disposition of stream().
enum class StreamOutcome : std::uint8_t {
    Completed,      // a Finished event was delivered
    Cancelled,      // CancellationToken fired, or the sink returned Stop
    Failed,         // a terminal Error was delivered
};

} // namespace ymh
```

### 2.2 Error taxonomy

All provider failures are typed; no exception crosses the seam (L16). The
`LLMError` value is carried on the terminal `StreamEvent::Error` and on the
returned `LLMResponse`.

```cpp
namespace ymh {

enum class LLMErrorCode : std::uint8_t {
    None,                     // not an error
    Auth,                     // 401/403; missing/invalid key; non-retryable
    ConfigError,              // unknown provider, unknown model, bad base_url
    BadRequest,               // 400/422; malformed request or tool schema
    ContextLengthExceeded,    // provider says the prompt is too long (§32)
    RateLimited,              // 429 / quota; retryable, honor Retry-After
    ServerError,              // 5xx; retryable before any event is dispatched
    NetworkError,             // connect/reset/DNS/TLS failure; retryable pre-event
    Timeout,                  // connect/idle/total deadline; retryable pre-event
    MalformedResponse,        // undecodable SSE/JSON; retryable pre-event
    MalformedToolCall,        // tool-call argument fragments never assemble
    ContentFiltered,          // provider-side content filter/refusal
    UnsupportedModel,         // model does not support a requested capability
    ProviderInternal,         // invariant violation / unexpected provider shape
    Cancelled,                // cooperative cancellation (not a failure, §34)
};

struct LLMError {
    LLMErrorCode           code = LLMErrorCode::None;
    int                    http_status = 0;   // 0 when not HTTP
    std::string            provider_message;  // provider's own message, redacted
    std::string            detail;            // short, redacted diagnostic
    bool                   retryable = false; // see §3.7 / L7
};

} // namespace ymh
```

Rules:

- `provider_message` is the provider's `error.message` when present; it is
  passed through a redactor (§8) before it is logged or surfaced (L12).
- `retryable` is computed by the adapter from `code` **and** the
  first-delta state (§3.7); it is advisory to the caller, but the provider
  itself already applies the retry policy (§3.7).
- `Cancelled` is a terminal disposition, not a failure: it is reported as
  `StreamOutcome::Cancelled` with `LLMResponse.error.code == None`. Only
  `StreamOutcome::Failed` carries a non-`None` code (L16). The
  `LLMErrorCode::Cancelled` enumerator exists for internal/diagnostic
  accounting only and is **never** placed in `LLMResponse.error`. The agent
  loop treats `Cancelled` as cancellation, not error (§34,
  `01 §16.1(e)`).

---

## 3. The `LLMProvider` seam (pinned)

### 3.1 Request types

`Message`, `Role`, `ContentBlock`, `ToolSchema`, and `Usage` are owned by
specs 01/07 (`agent/message.hpp`, `§12`, `§14`); they are reused, not
redefined. This spec adds `GenerationParameters`, `RequestId`, and an explicit
deadline **additively** to the `§12` sketch (decision (f)).

```cpp
namespace ymh {

struct GenerationParameters {
    std::optional<double>        temperature;      // provider default when nullopt
    std::optional<double>        top_p;
    std::optional<std::uint32_t> max_output_tokens;
    std::vector<std::string>     stop;
    std::optional<std::string>   tool_choice;      // "auto"|"none"|"required"|name
    std::optional<std::string>   reasoning_effort; // "low"|"medium"|"high" (if supported)
    std::optional<std::uint32_t> seed;             // best-effort determinism
};

struct LLMRequest {
    ModelId                  model;        // effective model (§5.2), never empty
    std::vector<Message>     messages;     // already assembled by spec 06 (§31)
    std::vector<ToolSchema>  tools;        // empty => no tool calling
    GenerationParameters     parameters;
    RequestId                request_id = 0;             // additive (§2.1)
    std::chrono::milliseconds deadline{0};               // 0 => provider default
};

} // namespace ymh
```

`LLMRequest.model` is **required** and non-empty; the provider never
substitutes a model silently (L11, `§54 D5`). `messages` is a complete,
already-compacted list; the provider performs no context assembly and no
history trimming (that is `§31`/`§32`, spec 06).

### 3.2 Stream-event algebra

```cpp
namespace ymh {

struct TextDelta       { std::string text; };
struct ReasoningDelta  { std::string text; };   // only if capabilities().reasoning

struct ToolCallAssembled {                      // assembled by ToolCallAssembler (§4.2)
    ToolCallId     id;
    std::string    name;
    nlohmann::json arguments;                   // always a JSON object
};

struct ToolCallStarted {
    std::uint32_t index;      // provider-assigned position within the response
    ToolCallId    id;         // provider-assigned; becomes the tool_use block id
    std::string   name;
};
struct ToolCallDelta {
    std::uint32_t index;
    std::string   arguments_fragment;   // raw JSON text fragment, not parsed
};
struct ToolCallFinished {
    std::uint32_t     index;
    ToolCallAssembled call;             // id, name, parsed JSON object
};
struct UsageEvent { Usage usage; };

struct Finished {
    FinishReason        reason;
    std::optional<Usage> usage;         // convenience mirror; may be nullopt
};

struct StreamError { LLMError error; };

using StreamEvent = std::variant<TextDelta, ReasoningDelta, ToolCallStarted,
                                 ToolCallDelta, ToolCallFinished, UsageEvent,
                                 Finished, StreamError>;

} // namespace ymh
```

Event ordering contract (normative; see L3/L4):

```text
(optional ReasoningDelta*) (optional TextDelta*)
(optional (ToolCallStarted ToolCallDelta* ToolCallFinished) per index, in order)
(optional UsageEvent)                     # at most one, if usage_streaming
(Finished | StreamError)                  # exactly one terminal event, last
```

- `ToolCallFinished` for an index appears only after that index's
  `ToolCallStarted`; `ToolCallDelta` fragments appear only between them.
- Indices are dense from 0 in the order the provider reports them; a provider
  that reports an index twice is `ProviderInternal` (L-F14).
- `UsageEvent` appears at most once and only when the provider reports usage
  (L8). It precedes the terminal event.
- A `Finished` event never carries an `Error`, and a `StreamError` is always
  the last event; nothing is emitted after a terminal event.

### 3.3 The `StreamSink`

```cpp
namespace ymh {

enum class SinkFlow : std::uint8_t {
    Continue,   // keep streaming
    Stop,       // cooperative consumer-initiated stop (supersession, teardown)
};

// Invoked serially, in stream order, on the provider's execution context.
// MUST be cheap and MUST NOT throw, block, re-enter the provider, or touch
// the session store (L1, L5). Returning Stop aborts the stream (§3.6).
using StreamSink = std::function<SinkFlow(const StreamEvent&)>;

} // namespace ymh
```

The sink is deliberately synchronous and non-blocking. Coalescing into
bounded `AssistantChunk` batches is the **producer's** job (spec 06,
`01 §16.1(c)`); the provider emits one event per provider delta and never
batches, buffers, or persists. This keeps the provider trivially replayable
and keeps durability policy in one place.

Backpressure is transport-level: because dispatch is synchronous, the
provider does not read the next transport chunk until the previous `onEvent`
returns, so a slow consumer slows the HTTP read loop rather than growing an
unbounded provider-side queue. A consumer that cannot keep up applies its own
bounded policy (spec 06) and may return `Stop`.

### 3.4 Class shape

```cpp
namespace ymh {

struct ModelInfo {
    ModelId       id;
    std::string   display_name;             // may be empty
    std::uint64_t max_context_tokens = 0;   // 0 => unknown
};

struct ProviderCapabilities {
    bool streaming          = true;   // MUST be true (§12); non-streaming is emulated
    bool tool_calls         = false;
    bool parallel_tool_calls= false;
    bool reasoning          = false;
    bool usage_streaming    = false;
    bool prompt_caching     = false;
    std::optional<std::size_t> max_context_tokens;   // nullopt => unknown
};

class LLMProvider {
public:
    virtual ~LLMProvider() = default;

    // Stable registry identity, e.g. "openai-compatible".
    virtual ProviderId id() const = 0;

    // Streaming-first (§12). `sink` is invoked serially in stream order.
    // `cancel` is observed at every await point and during transport reads.
    // Never throws: all failures are delivered as a terminal StreamEvent::Error
    // and/or a non-`Completed` LLMResponse (§2.2, L16).
    virtual Task<LLMResponse> stream(
        const LLMRequest& request,
        StreamSink sink,
        CancellationToken cancel) = 0;

    // Truthful capability advertisement (L14); the caller may rely on it.
    virtual ProviderCapabilities capabilities() const = 0;

    // Enumerable models, if the provider can list them; empty => unknown.
    virtual std::vector<ModelInfo> models() const { return {}; }
};

} // namespace ymh
```

`LLMResponse` is the authoritative terminal value for the caller; text and
reasoning are **not** re-delivered here (they were streamed through the sink).
This avoids duplicating potentially large payloads and keeps the sink the
single delivery path (decision (f)). `error.code` is non-`None` **iff**
`outcome == Failed`; a `Cancelled` response carries `error.code == None`
(L16, §2.2).

```cpp
namespace ymh {

struct LLMResponse {
    StreamOutcome                     outcome = StreamOutcome::Completed;
    FinishReason                      finish  = FinishReason::Other;
    std::optional<Usage>              usage;             // nullopt if not reported
    std::vector<ToolCallAssembled>    tool_calls;        // index order
    LLMError                          error;             // code != None iff Failed
    RequestId                         request_id = 0;
    std::chrono::milliseconds         latency{0};        // wall time, diagnostics only
};

} // namespace ymh
```

`Task<T>` and `CancellationToken` are core types (`core/cancellation.hpp`,
`§34`, `§35`); this spec consumes them and does not redefine them (decision (q)).

### 3.5 Usage / token accounting

- `Usage{input_tokens, output_tokens, cached_tokens, reasoning_tokens}` is the
  `§33` type reused from `01 §4.5` (defined with the message types, `§12`).
- The provider emits at most one `UsageEvent` per request and mirrors it in
  `Finished.usage` / `LLMResponse.usage` when the provider reports it (L8,
  L17). Usage is **never fabricated or estimated** by the provider; context
  token *estimation* for compaction is a separate concern owned by spec 06
  (`§31`, `§32`).
- The agent loop is responsible for appending the durable `TokenUsage` event
  (`01 §4.5`, `§33`); the provider has no store access (L1).
- `cached_tokens`/`reasoning_tokens` are relayed verbatim from the provider;
  no cross-provider normalization is attempted in v1 (decision (s)).

### 3.6 Cancellation

- Every `stream()` call takes a `CancellationToken` (`§34`). The provider
  observes it at every await point, during transport reads/writes, and while
  sleeping between retries.
- On cancellation the provider: stops reading the transport, aborts the HTTP
  transfer (`curl_easy`/`curl_multi` abort), emits **no further** stream
  events, and completes with `StreamOutcome::Cancelled` (not `Failed`). A
  cancellation is not an error and is never logged as one (L6, L-F11).
- `SinkFlow::Stop` is equivalent to consumer-initiated cancellation for that
  request: the provider aborts and returns `Cancelled`. It does **not** cancel
  other requests (F9): tokens and sinks are per-request.
- The durable consequence (`TurnCancelled`, wire `turn/cancel`) is appended by
  the agent loop, never by the provider (`01 §4.5`, `01 §16.1(e)`, `§34`).
- Cancellation must be bounded: an aborted transfer must release its
  `LLMPool` slot promptly (target: within the transport's abort latency; see
  §13.2 tests).

### 3.7 Retry and backoff

```cpp
namespace ymh {

struct RetryPolicy {
    std::uint32_t             max_attempts = 3;      // total tries, incl. the first
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{30'000};
    double                    jitter = 0.25;         // ±25% full-jitter fraction
    bool                      honor_retry_after = true;
};

} // namespace ymh
```

Normative rules:

- **Retryable codes** (only before any event is dispatched): `RateLimited`, `ServerError`,
  `NetworkError`, `Timeout`, `MalformedResponse`.
- **Non-retryable codes**: `Auth`, `ConfigError`, `BadRequest`,
  `ContextLengthExceeded`, `ContentFiltered`, `UnsupportedModel`,
  `MalformedToolCall`, `ProviderInternal`, `Cancelled`.
- **No retry after the first dispatched event (L7).** Once **any**
  `StreamEvent` — including `UsageEvent` — has been dispatched to the sink, a
  mid-stream failure is terminal. Retrying would re-emit deltas (duplicate
  durable `AssistantChunk`s, an ambiguous `AssistantMessage`) and, when usage
  had already been reported, a second `UsageEvent` (duplicate `TokenUsage`).
  Because the barrier is any event, a retried request cannot double-count
  usage; the producer appends at most one `TokenUsage` per request (L17). If
  the caller wants to retry, it starts a **new step/request** explicitly
  (spec 06).
- Backoff is exponential with bounded jitter and is **cancellable**: a cancel
  during the sleep aborts immediately (L6).
- `RateLimited` honors `Retry-After` when `honor_retry_after` is set and the
  header is a valid delta-seconds or HTTP-date; otherwise it uses the backoff.
- Retries are logged under the `llm` category (`§40`) with
  `request_id`, provider, model, attempt, status, and delay — **never** prompt
  or response content (L12).
- Retry policy is config-driven (§5.3); a retry budget is bounded by
  `max_attempts` and the per-request deadline. `LLMPool` bounds concurrent
  requests across sessions (F8, L13).

### 3.8 Timeouts

Three distinct deadlines, all configurable and all cancellable:

| Timeout | Meaning | Default |
|---|---|---|
| `connect_timeout` | TCP+TLS connect | 10 s |
| `idle_timeout` | max gap between transport bytes (SSE keep-alive) | 60 s |
| `request_timeout` | total wall time for the request | 120 s |

`LLMRequest.deadline`, when non-zero, caps `request_timeout` for that call.
A `Timeout` is retryable only before any event is dispatched (L7).

---

## 4. Streaming model and durable-event mapping

The provider is **not** an event producer in the durable sense (L1). This
section pins the mapping the agent loop (spec 06) implements; it is part of
the seam contract because adapters must emit events that map unambiguously.

### 4.1 Text and reasoning deltas → `AssistantChunk`

- Each `TextDelta` is a candidate `payload::AssistantChunk{message, index,
  text, Kind::Text}`; each `ReasoningDelta` maps to `Kind::Reasoning`
  (`01 §4.5`).
- The **producer** (spec 06) coalesces adjacent deltas into bounded batches
  before append and assigns the monotonic `index` within the
  `AssistantMessage` (`01 §16.1(c)`). The provider assigns no `MessageId` and
  no chunk index; those are the producer's.
- `AssistantChunk` is durable so a streamed UI is replayable, but it is **not**
  projected into LLM messages when the assembled `AssistantMessage` exists
  (`01 §6.3`, `01` I14).
- On completion, the producer appends exactly one `AssistantMessage` whose
  `content` includes the text/reasoning and any `tool_use` blocks; this is the
  canonical, projected form (`01 §4.5`, `01 §6.3`).

### 4.2 Tool-call streaming and assembly

Tool-call fragments are provider-shaped; assembly is provider-agnostic and
shared so every adapter behaves identically.

```cpp
namespace ymh {

// Provider-agnostic assembler. One instance per in-flight response.
// Not thread-safe; the provider invokes it on its single stream context.
class ToolCallAssembler {
public:
    explicit ToolCallAssembler(std::size_t max_arguments_bytes);  // bounded (L-F16)

    // Returns nullopt while the call is incomplete; the finished call when the
    // final fragment has been appended. Errors (invalid JSON, unknown index,
    // duplicate finish, size cap) are surfaced by the provider as a terminal
    // StreamError{MalformedToolCall|ProviderInternal} (L10, L-F7, L-F14).
    std::optional<ToolCallAssembled> onStarted(std::uint32_t index,
                                               ToolCallId id,
                                               std::string name);
    void                             onDelta(std::uint32_t index,
                                             std::string_view fragment);
    std::optional<ToolCallAssembled> onFinished(std::uint32_t index);

    std::vector<ToolCallAssembled> take_ordered() &&;   // index order
};

} // namespace ymh
```

Rules:

- `ToolCallFinished.id` must equal the `ToolCallStarted.id` for the same
  index and becomes the `tool_use` block id (L9, `01 §4.5`).
- Arguments are parsed to a JSON **object** at `ToolCallFinished`; an object
  is required (L10). Invalid JSON or a non-object is `MalformedToolCall`; the
  call is never silently dropped and never executed.
- The provider does not validate that `name` exists in `request.tools`;
  registry/policy validation is spec 07/09 (L-F8). Silently filtering a call
  would desynchronize `tool_use` ↔ `tool_result` pairing (`01` I12).
- Fragments for one index are concatenated in arrival order before parsing; a
  fragment cap (`max_arguments_bytes`, default 1 MiB) prevents unbounded
  memory (F5 spirit, L-F16).
- The producer appends one durable `ToolCall` per assembled call (and the
  `tool_use` blocks in the `AssistantMessage`) after the step's stream
  completes (`01 §4.5`).

### 4.3 `Usage`, `Finished`, and step completion

- `UsageEvent` → the producer appends `payload::TokenUsage{usage, turn}`
  (`01 §4.5`, `§33`). Absent usage ⇒ no event (L8). At most one `TokenUsage` is
  appended per request (L17); because retries are barred after any dispatched
  event (L7), a retried request cannot double-count usage.
- `Finished{reason}` → the producer appends `StepEnded` and either starts the
  next step (if tool calls are pending) or ends the turn per `§11`.
- `FinishReason::Length` is not an error, but the producer may surface a
  truncation notice (spec 10); `ContentFilter` maps to `ContentFiltered`.

### 4.4 Cancellation terminates a stream

- Cancel ⇒ the provider returns `Cancelled` and emits no further events (§3.6).
- The producer appends `TurnCancelled{turn, reason}` (wire `turn/cancel`) as
  the single terminal event for the turn and synthesizes `Cancelled`
  `ToolResult`s for unmatched `tool_use` blocks (`01 §4.5`, `01` I12,
  `01 §16.1(e)`).
- Cancellation is scoped to one request/turn (F9); it never affects another
  session or another in-flight request in the same session.

### 4.5 Terminal provider errors

A terminal `StreamError` closes the stream. The producer (spec 06) closes the
turn with the distinct durable `TurnFailed{turn, code, message}` event
(`01 §4.5`, wire `turn/fail`) and emits a live `Error` event (`§8.1`).
**Failure is not cancellation:** a failed turn synthesizes
`ToolResult{outcome = Error}` for every unmatched `tool_use`, whereas a
cancelled turn keeps `ToolResult{outcome = Cancelled}` (`01 §4.5`, `01` I11,
`01` I12, `01 §16.1(e)`). The durable `TurnFailed.code` is an `AgentErrorCode`
produced by the loop's `mapAgentError` (06 §2.2), into which the provider-side
`LLMErrorCode` is mapped (e.g. `RateLimited` → `ProviderFailed`); `message` is the
redacted provider message (L12). The provider itself appends nothing (L1); this
mapping is the seam contract the loop implements.

---

## 5. Model selection and configuration

### 5.1 Provider registry and factory

```cpp
namespace ymh {

struct LLMProviderConfig {
    ProviderId   provider;                 // registry key, e.g. "openai-compatible"
    std::string  base_url;                 // e.g. "http://localhost:8000/v1"
    ModelId      model;                    // default model (may be empty)
    std::string  api_key_env;              // NAME of the env var, never the secret
    std::vector<std::pair<std::string,std::string>> headers;  // extra, non-secret
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::chrono::milliseconds request_timeout{120'000};
    RetryPolicy  retry;
    std::size_t  max_arguments_bytes = 1u << 20;   // tool-arg assembly cap (§4.2)
    std::size_t  sse_line_bytes      = 1u << 20;   // max single SSE data line (L-F16)
};

using ProviderFactory =
    std::function<std::expected<std::unique_ptr<LLMProvider>, LLMError>(
        const LLMProviderConfig&)>;

class ProviderRegistry {
public:
    void registerProvider(ProviderId, ProviderFactory);   // startup only

    // Returns ConfigError for an unknown provider or invalid config; never
    // throws and never performs network I/O (L-F12).
    std::expected<std::unique_ptr<LLMProvider>, LLMError>
    create(const LLMProviderConfig&) const;

    std::vector<ProviderId> names() const;
};

} // namespace ymh
```

**Ownership.** Spec 08 constructs the `ProviderRegistry` and registers the
built-in adapters; spec 04 supplies `LLMProviderConfig` (it exposes no provider
handle of its own, `04 §8`); spec 06 owns the resolved `LLMProvider` instance
and schedules `stream()` on the `LLMPool` (decision (n)).

**Key handling (pinned).** `create()` validates only the **name** in
`api_key_env` (non-empty, a legal environment-variable identifier) and fails
with `ConfigError` when it is not; it never reads the secret. The **value** is
read from the environment at each `stream()` call and is never cached in the
provider, so a rotated key takes effect without reconstructing the provider
(decision (m), §6.4). v1 registers only `openai-compatible` (`§57` Step 7);
`create()` validates `base_url`/`model` but does not contact the endpoint.

### 5.2 Effective model resolution

The effective model for a request is resolved in this order (first non-empty
wins), and is recorded in `SessionStarted.model` at session creation
(`01 §4.5`):

```text
1. explicit per-request override (compaction/summarization, §32)   [spec 06]
2. SessionHeader.model        (immutable for the session; 01 §3)
3. profile config model       (§38)
4. project config model       (§37)
5. global config model        (§37)
6. [llm.default].model        (§13, §37)
7. built-in default           (may be empty => ConfigError)
```

- `LLMRequest.model` is set by the caller to the resolved value; the provider
  never re-resolves (L11).
- `SessionHeader.model` is immutable (`01 §3`); switching models means a new
  session (or an explicit per-request override for auxiliary calls such as
  compaction), not a header mutation.
- If resolution yields an empty model, `create()`/the caller fails with
  `ConfigError` before any network I/O (L-F12).

### 5.3 Configuration layering

Config is TOML and layered exactly as `§37`:

```text
built-in defaults → global → project → profile → CLI overrides
```

```toml
[llm.default]
provider       = "openai-compatible"
base_url       = "http://localhost:8000/v1"
model          = "qwen2.5-coder"
api_key_env    = "OPENAI_API_KEY"     # the NAME; the secret lives in the env
max_concurrency = 4                    # informational; enforced by the host pool
connect_timeout_ms = 10000
idle_timeout_ms    = 60000
request_timeout_ms = 120000

[llm.default.retry]
max_attempts = 3
base_delay_ms = 500
max_delay_ms = 30000
jitter = 0.25
```

- `max_concurrency` is advisory to the host's `LLMPool` (04 §8); the provider
  itself never enforces or spawns concurrency (L13).
- Unknown keys are rejected at load (config strictness is spec 06/10, cited
  here only as the source of `LLMProviderConfig`).

### 5.4 Capability negotiation

- Before a request, the caller may consult `capabilities()` (L14). Requesting
  tools from a provider with `tool_calls == false`, or reasoning from a
  provider with `reasoning == false`, is a caller error surfaced as
  `UnsupportedModel`/`BadRequest` — the provider does not silently drop the
  request.
- `streaming` must be `true` for every provider (L2). A non-streaming endpoint
  is wrapped by an adapter that emits one `TextDelta` followed by `Finished`.
- Adapters gate optional fields on capability: e.g. the OpenAI-compatible
  adapter maps `reasoning_content`/`reasoning` only when
  `capabilities().reasoning` is true (§6.2); an unsupported field is ignored,
  never folded into another event type (L14).

---

## 6. OpenAI-compatible HTTP adapter (first concrete provider)

### 6.1 Transport seam

The adapter talks to HTTP through a narrow seam so the transport can change
without touching provider logic (`§36`).

```cpp
namespace ymh {

struct HttpRequest {
    std::string method;                                    // "POST"
    std::string url;                                       // fully-qualified
    std::vector<std::pair<std::string,std::string>> headers;
    std::string body;
    std::chrono::milliseconds connect_timeout;
    std::chrono::milliseconds idle_timeout;
    std::chrono::milliseconds total_timeout;
};

// Response body bytes as they arrive; returning false aborts the transfer.
using HttpBodySink = std::function<bool(const char* data, std::size_t len)>;

struct HttpResponse {
    int status = 0;
    std::vector<std::pair<std::string,std::string>> headers;
    std::string body;                  // populated only for non-streaming reads
    LLMError    transport_error;       // code != None on transport failure
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    // Streaming POST. `on_body` is invoked serially, in order. Never throws.
    virtual Task<HttpResponse> postStream(
        const HttpRequest& request,
        HttpBodySink on_body,
        CancellationToken cancel) = 0;
};

} // namespace ymh
```

- **v1 implementation** (`CurlHttpTransport`) uses libcurl on the calling
  thread — the host's bounded `LLMPool` worker (04 §8, `§35`). It sets
  `CURLOPT_WRITEFUNCTION` → `on_body`, a progress callback that checks
  `cancel`, and `CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST` on (never disabled).
  It owns no thread and no `io_context` (L13).
- **Future implementation** (Asio/Beast) implements the same seam with an
  async transfer; adapters are unaffected (`§36`).
- Dependencies: libcurl + OpenSSL. `§36` already lists OpenSSL and an HTTP
  library; `§48` says to add dependencies as features arrive, so this is
  consistent with — not a change to — the baseline dependency set (decision
  (b)).

### 6.2 Request/response mapping

Request (OpenAI chat-completions shape):

```text
POST {base_url}/chat/completions
Authorization: Bearer <key from api_key_env>     # never logged (L12)
Content-Type: application/json

{
  "model": <LLMRequest.model>,
  "messages": [ ...mapped Message/ContentBlock... ],
  "tools": [ ...ToolSchema mapped... ],          # omitted when empty
  "stream": true,
  "stream_options": { "include_usage": true },   # only if usage_streaming
  ...GenerationParameters...
}
```

Message mapping (normative):

| ymh | OpenAI chat message |
|---|---|
| `Role::System` | `role:"system"`, `content` text |
| `Role::User` | `role:"user"`, content parts (text/image) |
| `Role::Assistant` with `tool_use` blocks | `role:"assistant"`, `tool_calls:[{id,type:"function",function:{name,arguments}}]`, optional `content` |
| `Role::Tool` | `role:"tool"`, `tool_call_id`, `content` text |

Response parsing:

- SSE frames: lines `data: <json>` terminated by `\n\n`; a literal
  `data: [DONE]` ends the stream; comment lines (`:`) and unknown fields are
  ignored; CRLF and LF are both accepted. A single `data:` line exceeding
  `sse_line_bytes` (default **1 MiB**) aborts the stream with
  `MalformedResponse` (L-F16).
- `choices[0].delta.content` → `TextDelta`;
  `delta.reasoning_content`/`delta.reasoning` → `ReasoningDelta` **only when
  `capabilities().reasoning` is true**; otherwise the field is ignored and is
  never folded into `TextDelta` (L14);
  `delta.tool_calls[]` → `ToolCallStarted`/`ToolCallDelta`/`ToolCallFinished`
  via `ToolCallAssembler`;
  `usage` (final chunk) → `UsageEvent`;
  `choices[0].finish_reason` → `FinishReason`.
- A non-2xx response body is parsed for `error.message`/`error.code` and
  mapped by §2.2; the body is redacted before logging.

### 6.3 Adapter seam for future providers

Adding Anthropic/Ollama/vLLM must not change `LLMProvider`:

- A new adapter implements `stream()`, `capabilities()`, `models()`, and maps
  its native wire format to the `StreamEvent` algebra of §3.2.
- It registers a `ProviderFactory` under its `ProviderId`.
- It reuses `ToolCallAssembler`, the `LLMError` taxonomy, the retry policy,
  and the credential redactor.
- `vLLM` and `Ollama` are expected to be thin wrappers over the
  OpenAI-compatible adapter (many local servers speak that API, `§13`);
  `Anthropic` needs its own event mapping but the same seam.

### 6.4 Credentials and secret handling

- The secret is read from the environment variable named by `api_key_env` at
  **each request** (never cached in the provider; decision (m)); v1 never reads
  a literal key from a config file (decision (g)). `create()` validates only
  the env-var **name** (a bad name is `ConfigError`); a missing or empty
  **value** at request time yields `Auth` **before** any network I/O (L-F1,
  L-F12).
- Keys, `Authorization` headers, and full prompts are never logged and never
  persisted outside the session log (`§40`, `§46`, L12). Diagnostics redact
  header values and cap provider messages to a short redacted excerpt.
- The key is held only for the lifetime of a request; it is never written to
  the registry, the session DB, or the event log (`§9.10`, `§40`).
- TLS verification is always on; a certificate failure is `NetworkError`, not
  a silent `-k` (L-F18).

---

## 7. `FakeLLM` (§45)

`FakeLLM` implements the same `LLMProvider` seam and is the substrate for
unit, integration, golden, and replay tests (`§44`, `§45`). It is
deterministic: its output is a pure function of `(script, request)` with no
network, no wall clock, and no randomness (L15).

```cpp
namespace ymh {

struct FakeToolCallStep {
    std::string    name;
    nlohmann::json arguments;
    std::optional<std::string> id;     // else generated deterministically
};

struct FakeResponseStep {
    std::string                text;             // streamed as TextDelta(s)
    std::optional<std::string> reasoning;        // streamed as ReasoningDelta(s)
    std::vector<FakeToolCallStep> tool_calls;    // streamed as tool-call events
    std::optional<Usage>       usage;
    FinishReason               finish = FinishReason::Stop;
    std::optional<LLMError>    error;            // inject a terminal failure
    std::chrono::milliseconds  latency{0};       // simulated, not slept
};

struct FakeScript {
    std::vector<FakeResponseStep> steps;   // consumed one per stream() call
    std::size_t chunk_size = 4;            // max bytes per TextDelta; split on
                                           // UTF-8 codepoint boundaries
    bool        fail_after_first_delta = false;  // exercise L7
};

class FakeLLM final : public LLMProvider {
public:
    explicit FakeLLM(FakeScript);

    ProviderId id() const override;                       // "fake"
    ProviderCapabilities capabilities() const override;   // all true, configurable
    std::vector<ModelInfo> models() const override;
    Task<LLMResponse> stream(const LLMRequest&,
                             StreamSink,
                             CancellationToken) override;
};

} // namespace ymh
```

Rules:

- `stream()` consumes the next `FakeResponseStep` in order; when the script is
  exhausted it returns a deterministic `Stop` response (or a scripted error).
- Text is split into `chunk_size`-bounded deltas **on UTF-8 codepoint
  boundaries** — a multi-byte codepoint is never split across deltas — so
  coalescing/replay/rendering tests always see valid UTF-8; tool calls are
  emitted as `ToolCallStarted`/`ToolCallDelta`(fragmented)/`ToolCallFinished`.
- `CancellationToken` is honored between deltas; a cancelled FakeLLM stream
  returns `Cancelled` and emits no further deltas.
- `latency` is a *simulated* value used for accounting tests; FakeLLM never
  sleeps (tests stay fast).
- The scripted tool-call transcript mirrors `§45`: `read_file` → tool result →
  `edit_file` → tool result → final text.
- `FakeLLM` never reads the environment or the filesystem; it is a pure seam
  double.

---

## 8. Security and logging

- **Untrusted model output.** Model output is untrusted and enters the
  tool-parser → permission-policy → execution-environment chain (`§46`). The
  provider never executes anything; it only produces `StreamEvent`s.
- **No prompt/secret logging by default.** `§40`: never dump full prompts or
  sensitive tool output to normal logs. The session event log is the
  authoritative trace (`§40`, D2). LLM logs carry metadata only: `request_id`,
  provider, model, attempt, HTTP status, latency, token counts, error code.
- **Redaction.** A single redactor is applied to every provider-derived string
  before logging/surfacing: `Authorization`/`api-key` header values, known key
  patterns, and (when `YMH_LLM_LOG_PROMPTS` is explicitly opted in) prompt
  bodies — off by default.
- **Resource caps.** The provider is bounded by the host's `LLMPool` (F8,
  `§9.11`), by `max_arguments_bytes` (tool-call assembly, §4.2), by
  `sse_line_bytes` (default **1 MiB** per SSE `data:` line, L-F16), and by the
  three timeouts (§3.8). It never allocates unbounded per-stream state; SSE
  line and tool-call assembly buffers are bounded.

---

## 9. Concurrency and threading

- The provider is **synchronous with respect to its execution context**: it
  runs on the thread that invokes `stream()` (the host's bounded `LLMPool`
  worker, `§35`, 04 §8). It creates no threads, no `io_context`, and no
  detached tasks (L13).
- For one request, all `StreamSink` invocations happen serially on that one
  context, in stream order (L3). No lock is needed inside an adapter for a
  single stream.
- Different requests may run concurrently on different pool workers; the pool
  bounds the count (F8). A `ProviderRegistry`/`LLMProvider` instance is
  stateless with respect to requests and safe to share across workers, except
  where a config value is immutable after construction.
- Cancellation and `SinkFlow::Stop` are the only cross-thread signals into a
  stream; both are observed at defined checkpoints (§3.6).
- The agent loop owns coalescing and durable append; the provider never
  blocks on the store (L1, L5).

---

## 10. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**L1 — Provider purity.** An `LLMProvider` never reads or writes the session
log, `SessionStore`, workspace registry, or `EventBus`; it consumes an
`LLMRequest` and produces `StreamEvent`s/`LLMResponse` only. (D2/D3, `§4.2`,
`01 §4`)

**L2 — Streaming-first.** Every provider implements `stream()`; there is no
separate non-streaming entry point. A non-streaming backend is emulated as one
`TextDelta` + `Finished`. (`§12`)

**L3 — Serial, ordered dispatch.** For a single request the sink is invoked
serially, in stream order, on one execution context; no concurrent or
re-entrant invocation. (`§35`)

**L4 — Exactly one terminal event.** A stream ends with exactly one of
`Finished` or `StreamError` (or is aborted by cancellation/`Stop` with no
terminal event). No event follows a terminal event. (`§12`)

**L5 — Sink contract.** The sink is cheap, non-blocking, non-throwing,
non-reentrant, and never touches the store; the provider does not rely on the
sink for backpressure beyond synchronous dispatch. (L1, `§35`)

**L6 — Bounded cancellation.** `stream()` observes its `CancellationToken` and
`SinkFlow::Stop` at defined checkpoints; on either it aborts the transport,
emits no further events, releases its pool slot, and completes `Cancelled`
within a bounded time. (`§34`)

**L7 — No retry after the first dispatched event.** Retries occur only before
the first `StreamEvent` — **including `UsageEvent`** — is dispatched; after
that a failure is terminal. (§3.7; prevents duplicate durable
`AssistantChunk`s and `TokenUsage`s)

**L8 — Usage is never fabricated.** A `UsageEvent`/`TokenUsage` is produced
only when the provider reports usage; absent usage is `nullopt`. (`§33`)

**L9 — Stable tool-call ids.** `ToolCallFinished.id` equals the id emitted in
`ToolCallStarted` for that index and becomes the `tool_use` block id; it
round-trips verbatim. (`01 §4.5`, `01` I12)

**L10 — Valid assembled tool arguments.** `ToolCallFinished.arguments` is a
JSON object; an unassemblable call is a terminal `MalformedToolCall` error,
never a silent drop or a non-object. (§4.2)

**L11 — Explicit model, no silent substitution.** Every `LLMRequest` names a
non-empty effective model; the provider never substitutes a different model.
(`§54 D5`, §5.2)

**L12 — No secret/prompt leakage.** API keys, `Authorization` headers, and
full prompts are never logged or persisted outside the session log; provider
messages are redacted. (`§40`, `§46`)

**L13 — No provider-owned concurrency.** A provider creates no threads,
`io_context`, or detached tasks; concurrency is the host's `LLMPool`
(04 §8, `§9.11`, F8). (§9)

**L14 — Capability honesty.** `capabilities()` is truthful; a request that
needs an unsupported capability is rejected (`UnsupportedModel`/`BadRequest`),
never silently ignored. Optional stream fields are emitted only when the
matching capability is advertised — e.g. `reasoning_content` maps to
`ReasoningDelta` only when `reasoning == true`; otherwise it is ignored and
never folded into `TextDelta` (§5.4, §6.2).

**L15 — FakeLLM determinism.** `FakeLLM` output is a pure function of
`(script, request)` with no network, wall clock, or randomness. (`§45`)

**L16 — Total, typed errors.** Every failure maps to an `LLMErrorCode`; no
exception crosses the `LLMProvider` seam. `LLMResponse.error.code` is
non-`None` iff `outcome == Failed`; a `Cancelled` response carries `None`.
(§2.2)

**L17 — Single terminal accounting.** At most one `UsageEvent` and at most one
durable `TokenUsage` per request/step; a failed turn yields at most one
`TurnFailed` and a cancelled turn at most one `TurnCancelled`, never both.
(`§33`, `01 §4.5`, §4.5)

---

## 11. Failure modes

### 11.1 Shared findings (F1–F12, §54)

The LLM layer's responsibilities for the existing findings:

| F# | Finding | LLM-layer handling |
|---|---|---|
| **F1** | path/process isolation | provider performs no filesystem/path work; no `chdir`; endpoint is a configured URL (§9.7, §18) |
| **F2** | background permission | out of scope; the provider surfaces tool calls, policy is spec 09 (§9.9, §19) |
| **F3** | late event after close | out of scope; a cancelled stream emits nothing after abort (§3.6) |
| **F4** | edge-triggered attention | out of scope; live `LLMRequestStarted`/`LLMChunkReceived` feed the UI (§8.1, §9.9) |
| **F5** | output ring buffers | SSE line/tool-argument buffers are bounded; full model output lives in the durable log, not provider memory (§9.11) |
| **F6** | input/keybinding focus | out of scope; no UI state in the provider (§20.26) |
| **F7** | per-session dirty flags | out of scope; UI projection (§20.23) |
| **F8** | resource caps | provider owns no executor; bounded by the host `LLMPool` and per-stream byte/time caps (L13, §9.11) |
| **F9** | cancellation scoping | token/sink are per-request; cancelling one request cannot cancel another (§3.6, §34) |
| **F10** | resume-suspended | out of scope; resume does not call the provider (`01 §9.2`) |
| **F11** | subagent ID duality | out of scope; each subagent's loop makes its own requests (§30, §20.25) |
| **F12** | flash clock in model | out of scope; no timers beyond request timeouts (§9.9) |

**Explicitly out of scope:** F2, F3, F4, F6, F7, F10, F11, F12. Their owning
components are the permission policy (§19), the session/EventBus (01), the UI
(§20), and the subagent fan-in (§20.25). The provider supplies the live
`LLMRequestStarted`/`LLMChunkReceived` signals the UI projects, but owns none
of that logic.

### 11.2 Component-local failure modes (`L-F#`)

These are component-local to the LLM layer and must be covered by tests
(§13.5).

| L-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **L-F1** | Invalid/missing credentials | HTTP 401/403, or missing/empty `api_key_env` value at request time | `Auth`; a bad env-var **name** is `ConfigError` at create; non-retryable; key never logged (L12) |
| **L-F2** | Rate limited | HTTP 429 / quota | `RateLimited`; retry with `Retry-After`/backoff up to `max_attempts`; else terminal |
| **L-F3** | Provider 5xx | HTTP 5xx | `ServerError`; retry pre-event; terminal if exhausted |
| **L-F4** | Network/TLS failure | connect/reset/DNS/cert error | `NetworkError`; retry pre-event; TLS verification never disabled (L-F18) |
| **L-F5** | Timeout | connect/idle/total deadline | `Timeout`; retry pre-event; cancellable; releases pool slot |
| **L-F6** | Malformed response | undecodable SSE/JSON, truncated frame | `MalformedResponse`; retry pre-event; terminal after the first dispatched event (L7) |
| **L-F7** | Tool arguments never assemble | assembler JSON parse/non-object/size cap | terminal `MalformedToolCall`; the tool is never executed |
| **L-F8** | Tool name not in `request.tools` | model returns an unknown tool | provider passes it through unchanged; registry/policy rejects (spec 07/09); no silent filtering |
| **L-F9** | Context length exceeded | HTTP 400 with provider context-length code | `ContextLengthExceeded`; non-retryable; surfaced to spec 06 for compaction (§32) |
| **L-F10** | Content filtered / refusal | provider finish/error signal | `ContentFiltered`; terminal; no retry |
| **L-F11** | Cancel during connect/stream | token fired or `Stop` | abort transport, no `Error`, return `Cancelled`; loop appends `TurnCancelled` (§3.6) |
| **L-F12** | Bad config | unknown provider/model, empty model, bad `base_url` | `ConfigError` at create, before network I/O; never a crash |
| **L-F13** | Sink returns `Stop` | `SinkFlow::Stop` | abort transport; return `Cancelled`; no terminal `Error` |
| **L-F14** | Protocol violation by adapter | duplicate index, event after terminal, duplicate usage | `ProviderInternal`; sink ignores post-terminal events defensively |
| **L-F15** | Usage inconsistency | two `UsageEvent`s, or usage contradicting `Finished` | `ProviderInternal`; last consistent value retained for diagnostics only |
| **L-F16** | Unbounded provider buffer | SSE `data:` line > `sse_line_bytes` (default 1 MiB) or tool-arg fragment > `max_arguments_bytes` | abort with `MalformedResponse`; bounded memory (F5, F8) |
| **L-F17** | Retry storm | attempts exceed budget / thundering herd | bounded by `max_attempts` + jitter + deadline; `LLMPool` bounds concurrency |
| **L-F18** | TLS verification failure | certificate/hostname mismatch | `NetworkError`; never disabled; surface, do not fall back to plaintext |

---

## 12. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (`§55`). The LLM layer maps onto
it as follows.

| dsh concept | ymh LLM layer | Reference |
|---|---|---|
| LLM seam / model plugin | `LLMProvider` + `ProviderRegistry` | `§55`, `§12`, `§13` |
| Streaming model events | `StreamEvent` variant algebra | `§12` |
| Turn/step lifecycle | provider emits deltas; loop owns turn/step (`§11`) | `§11`, `01 §4.5` |
| Usage/cost accounting | `Usage` → `TokenUsage` durable event | `§33`, `01 §4.5` |
| Cancellation | `CancellationToken` propagation | `§34` |
| Approvals | provider surfaces tool calls; policy is separate | `§19`, `§46` |
| Fake model | `FakeLLM` scripted provider | `§45` |
| Profiles/bundles | config layering selects provider/model | `§37`, `§38` |
| Append-only trace | provider never writes; the session log is the trace | D2, `§9.1`, `§40` |

**Deliberate omissions** (accepted for v1, `§55`): only one concrete provider
(`§57` Step 7); no hot model reload; no cross-provider usage normalization; no
offline cache (a live endpoint or `FakeLLM` is required). The provider layer is
a small, stateless, headless seam — not a plugin runtime.

---

## 13. Test plan

Strategy is `§44`: unit, integration (mock HTTP / `FakeLLM`), golden, replay,
and a separate live PTY/real-LLM layer. The deterministic layers run offline
against `FakeLLM` (§45) and a local mock HTTP server; the live layer is opt-in
and API-key gated.

### 13.1 Unit tests

- **Codec / SSE parser**
  - `data:` frames split across TCP chunk boundaries reassemble correctly.
  - multi-line `data:` concatenation; comment (`:`) and unknown-field skipping.
  - `data: [DONE]` terminates; CRLF and LF both accepted; UTF-8 split across
    chunks is not corrupted (L-F6).
  - a frame exceeding the line cap aborts with `MalformedResponse` (L-F16).
- **`ToolCallAssembler`**
  - fragments in order → parsed object; `ToolCallFinished` id matches
    `ToolCallStarted` (L9).
  - interleaved indices; out-of-order fragments rejected; duplicate finish →
    `ProviderInternal` (L-F14).
  - invalid JSON / non-object / size cap → `MalformedToolCall` (L10, L-F7).
- **Error mapping**
  - status→code table (401/403→`Auth`, 429→`RateLimited`, 4xx→`BadRequest`,
    5xx→`ServerError`); provider `error.code` refinements (context length,
    content filter) map to `ContextLengthExceeded`/`ContentFiltered`.
  - transport errors map to `NetworkError`/`Timeout` (L-F4, L-F5).
- **Retry policy**
  - attempts bounded; exponential backoff with jitter within `[0.75,1.25]×`;
    `Retry-After` honored; non-retryable codes never retried.
  - **no retry after the first dispatched event** (L7): a scripted mid-stream failure yields
    one attempt and a terminal error.
  - cancel during backoff aborts immediately (L6).
- **Cancellation**
  - token fired at connect, mid-stream, and during backoff → `Cancelled`, no
    further events, bounded abort (L6, L-F11).
  - `SinkFlow::Stop` → `Cancelled`, no terminal `Error` (L-F13).
- **Model selection**
  - precedence chain of §5.2; empty result → `ConfigError` (L-F12).
  - unknown provider/model at `ProviderRegistry::create` → `ConfigError`.
- **Credentials / redaction**
  - `api_key_env` resolution; missing/empty → `Auth`/`ConfigError` (L-F1).
  - the redactor strips keys/`Authorization`; log capture contains no secret
    and no prompt by default (L12).
- **Capabilities**
  - `capabilities()` truthful; requesting tools from a non-tool provider →
    `UnsupportedModel`/`BadRequest` (L14).
- **`FakeLLM`**
  - script determinism (identical script+request ⇒ identical events);
    chunk-size splitting; simulated latency used only for accounting;
    scripted error injection; script exhaustion (L15).

### 13.2 Integration tests (mock HTTP server, `FakeLLM`)

- **Happy path.** A local mock server emits scripted SSE; the adapter produces
  the expected `StreamEvent` sequence and `LLMResponse` (L3, L4).
- **Tool calls.** Scripted tool-call SSE assembles to a parsed call; the
  `tool_use` id matches (L9, L10).
- **Retries.** 429→success and 500→success retry; 401→no retry; 400 context
  length→`ContextLengthExceeded` (L-F1–L-F3, L-F9).
- **Mid-stream disconnect.** Connection drops after the first dispatched event → no
  retry, terminal `Error` (L7, L-F4/L-F6).
- **Cancellation.** Cancel mid-stream aborts the transfer and releases the
  pool slot within the abort budget (L6, L-F11).
- **Usage.** Usage present → one `UsageEvent`; usage absent → no event (L8,
  L17).
- **Agent loop (with spec 06).** `FakeLLM` drives the full loop offline:
  `read_file` → tool result → `edit_file` → tool result → final text (§45);
  the durable event sequence is exactly the golden sequence (13.3).
- **HTTP transport seam.** A second, in-process `HttpTransport` fake proves
  the adapter is transport-agnostic (§6.3).

### 13.3 Golden tests

- **SSE fixture → `StreamEvent` sequence.** Checked-in SSE fixtures
  (OpenAI-style) produce a golden `StreamEvent` transcript, including tool
  calls and usage.
- **`StreamEvent` → durable event sequence.** Given a fixed coalescing cadence
  (spec 06), the golden `AssistantChunk`/`AssistantMessage`/`ToolCall`/
  `TokenUsage` event sequence is byte-stable (`01 §4.5`).
- **Error fixtures.** 401/429/500/context-length/filter bodies produce golden
  `LLMError` values with redacted messages (L12, L16).

### 13.4 Replay tests

- Record a live session's durable events; replay `deriveMessages()`
  (`01 §6.3`) to an identical message list. **The provider is not on the
  replay path** — replay must not construct or call an `LLMProvider`, proving
  L1 (the seam boundary is clean).
- `FakeLLM`-driven sessions replay deterministically across runs (L15).

### 13.5 Live end-to-end tests (real LLM, PTY-driven, §44)

- Spawn the real `ymh` binary under a PTY (`forkpty`/`posix_openpt`), write a
  prompt as keystrokes, read/parse rendered ANSI output, and assert observable
  behavior: session created, assistant text streamed, tool call rendered,
  permission prompt handled, cancellation works, session resumes (`§44`).
- Uses a **real LLM API through the same `LLMProvider` seam** as production
  (`§44`, `§45`).
- **Gated and opt-in:** requires an API key plus an explicit flag
  (e.g. `YMH_LIVE_LLM=1`); skipped — never failed — when absent, so the
  default suite stays hermetic and offline.
- **Model and cost cap:** the live model is pinned by `YMH_LIVE_LLM_MODEL`
  (provider and key come from config/env); each live run enforces a per-run
  token and cost cap and aborts (skips) when the cap is reached, so CI cost is
  bounded (decision (u)).
- **Nondeterminism-tolerant:** assert on structure and invariants (events
  emitted, tools invoked, final state), not exact prose; any retry/quorum is
  explicit, never silent.
- **Milestone-aware:** MVP live tests cover the single-process flow
  (Milestone 1, `§57`/`§58`); multi-workspace PTY tests target Milestone 2.
- Runs as a separate CI stage from the hermetic suite; never part of the fast
  default command (`§44`).

### 13.6 Failure-mode coverage matrix

| L-F# | Unit | Integration | Golden | Live |
|---|---|---|---|---|
| L-F1 | credentials | 401 no-retry | error fixture | auth failure path |
| L-F2 | retry policy | 429→success | — | — |
| L-F3 | retry policy | 500→success | — | — |
| L-F4 | error mapping | mid-stream drop | — | — |
| L-F5 | timeouts | idle timeout | — | — |
| L-F6 | SSE parser | malformed body | — | — |
| L-F7 | assembler | bad args | — | — |
| L-F8 | — | unknown tool passthrough | — | — |
| L-F9 | error mapping | 400 context length | error fixture | — |
| L-F10 | error mapping | filter body | error fixture | — |
| L-F11 | cancellation | cancel mid-stream | — | cancel in TUI |
| L-F12 | model selection | create bad config | — | — |
| L-F13 | sink stop | — | — | — |
| L-F14 | assembler | protocol violation | — | — |
| L-F15 | — | duplicate usage | — | — |
| L-F16 | SSE parser | oversized frame | — | — |
| L-F17 | retry policy | — | — | — |
| L-F18 | — | TLS failure | — | — |

### 13.7 Invariant coverage

| Invariant | Covered by |
|---|---|
| L1 | replay test (no provider on replay path), store-free unit fakes |
| L2 | `stream()`-only API; adapter compile-time check |
| L3 | integration ordering assertions; single-context sink test |
| L4 | terminal-event unit tests; golden sequences |
| L5 | sink-contract unit tests (no-throw/non-blocking fakes) |
| L6 | cancellation unit + integration; abort budget |
| L7 | retry unit (mid-stream failure ⇒ one attempt) |
| L8 | usage-present/absent integration + golden |
| L9 | assembler unit; tool-call integration |
| L10 | assembler unit; malformed-args golden |
| L11 | model-selection unit |
| L12 | redaction unit; log-capture assertions |
| L13 | static check: no thread creation in the provider target |
| L14 | capability unit tests |
| L15 | `FakeLLM` determinism unit + replay |
| L16 | error-taxonomy unit (total mapping) |
| L17 | usage single-event unit + golden |

---

## 14. Decisions and open questions

### 14.1 Decisions (pinned by this spec)

- **(a) One provider in v1: OpenAI-compatible HTTP.** `§57` Step 7 mandates
  one provider first; all others are deferred behind the unchanged
  `LLMProvider` seam (§6.3).
- **(b) v1 HTTP is libcurl on the host's `LLMPool` worker, behind an
  `HttpTransport` seam.** `§36` says not to over-engineer HTTP and lists
  OpenSSL + an HTTP library; `§48` allows adding deps as features arrive. A
  future Asio/Beast async transport swaps in without touching adapters.
- **(c) `StreamSink` is synchronous, non-blocking, non-throwing, and returns
  `SinkFlow`.** The provider never coalesces or persists; the producer
  (spec 06) owns coalescing (`01 §16.1(c)`).
- **(d) Retries are pre-first-delta only (L7).** A mid-stream failure is
  terminal; retrying is an explicit new step owned by spec 06.
- **(e) The provider never appends durable events.** The agent loop is the
  producer of `AssistantChunk`/`AssistantMessage`/`ToolCall`/`TokenUsage`/
  `TurnCancelled`/`TurnFailed` (L1, `01 §4.5`).
- **(f) `LLMRequest`/`LLMResponse` add `request_id`, `deadline`, `finish`,
  `usage`, and `tool_calls` additively to the `§12` sketch.** Text/reasoning
  are delivered only through the sink, not duplicated in `LLMResponse`.
- **(g) v1 credentials come from `api_key_env` only.** Literal keys in config
  files are rejected; a keyring is deferred (`§40`, `§46`).
- **(h) Usage is never fabricated (L8).** Context token estimation is a
  separate, spec-06 concern (`§31`, `§32`).
- **(i) Tool-call argument assembly is provider-agnostic and shared
  (`ToolCallAssembler`).** Adapters emit fragments; assembly semantics are
  identical everywhere (§4.2).
- **(j) The provider owns no threads or `io_context`; concurrency is the host
  `LLMPool` (04 §8, `§9.11`, F8).** (L13)
- **(k) A terminal `Error` closes the turn with `TurnFailed`.** The provider
  appends nothing (L1); the loop appends `TurnFailed{turn, code, message}` and
  synthesizes `ToolResult{outcome = Error}` for unmatched `tool_use`. Failure
  is never recorded as `TurnCancelled` (decision (p), `01 §4.5`).
- **(l) `FakeLLM` is the only test double on the seam; it never sleeps and
  never touches the network or filesystem** (`§45`, L15).
- **(m) `api_key_env` is validated by name at `create()`, read by value per
  request.** The provider never caches the secret, so key rotation needs no
  reconstruction; a missing value is `Auth` at request time (§5.1, §6.4).
- **(n) `ProviderRegistry` ownership.** Spec 08 constructs the registry and
  registers the built-in adapters; spec 04 supplies `LLMProviderConfig` and
  exposes no provider handle (`04 §8`); spec 06 owns the resolved provider and
  schedules `stream()` on the `LLMPool` (§1.1, §5.1).
- **(o) The retry barrier is any dispatched event, including `UsageEvent`.**
  Usage is emitted at most once per request and `TokenUsage` at most once per
  request/step (L7, L17, §3.7).
- **(p) `TurnFailed` is the durable failure event; failure ≠ cancel.** A failed
  turn yields `ToolResult{outcome = Error}`; a cancelled turn keeps
  `ToolResult{outcome = Cancelled}` (L17, §4.5, `01 §4.5`).
- **(q) `Task<T>`/executor is a core type.** Spec 08 consumes
  `Task<T>`/`CancellationToken` from the core (`§34`, `§35`); scheduling is
  spec 06's.
- **(r) The caller acquires the pool slot.** Spec 06 acquires/releases the
  `LLMPool` slot; the provider is synchronous and never acquires, releases, or
  owns a pool. The concrete `LLMPool` type stays deferred to spec 06
  (`04 §8`).
- **(s) `reasoning_tokens`/`cached_tokens` are relayed verbatim.** No
  cross-provider normalization in v1.
- **(t) `api_key_env` only.** A literal key in config and an OS keyring are
  deferred; v1 rejects literal keys (§6.4).
- **(u) Live model and cost cap.** The live layer pins the model via
  `YMH_LIVE_LLM_MODEL` and enforces a per-run token/cost cap (§13.5).

### 14.2 Open questions

None remain; every prior open question (OQ-1–OQ-7) is resolved in §14.1. A new
question reopens this subsection.

---

## 15. References

- `00-architecture.md` §4.2 (event stream is the runtime spine), §8.1
  (durable vs live events, `LLMRequestStarted`/`LLMChunkReceived`), §9.6
  (daemon/session model), §9.11 (resource caps, F5/F8), §10.1 (`Agent`),
  §11 (agent loop turn/step), §12 (LLM interface), §13 (provider adapters),
  §30 (subagents), §31 (context management), §32 (compaction), §33 (token
  accounting), §34 (cancellation), §35 (concurrency), §36 (networking), §37
  (configuration), §38 (profiles), §40 (logging), §44 (testing strategy), §45
  (Fake LLM), §46 (security model), §48 (dependency set), §49 (source tree),
  §54 (D1–D23, F1–F12), §55 (dsh comparison), §57 Step 7 (one provider first),
  §58 (milestones).
- `01-session.md` §2.1 (identifiers), §3 (`SessionHeader.model`), §4.3
  (`turn/fail`), §4.5 (durable payloads: `AssistantChunk`,
  `AssistantMessage`, `ToolCall`, `TokenUsage`, `TurnCancelled`,
  `TurnFailed`), §6.3 (`deriveMessages`), §12 (I11/I12/I14),
  §16.1(c)(e)(f) (coalescing, `TurnCancelled`/`TurnFailed` distinctness).
- `04-workspace-host-daemon.md` §8 (`ResourceGovernor`/`LLMPool`), §9
  (concurrency).
- `02-persistence.md` (session DB / event durability), `03-workspace-registry.md`
  (per-workspace ownership) as consumed by the daemon.
- `HANDOFF.md` §2 (the rule), §6 row 08 (component plan), §7 (definition of
  verified); `DESIGN_STATUS.md` (written/verified tracker).
