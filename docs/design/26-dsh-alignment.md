# 26 — dsh Alignment: Design-Copy of DeepSeek Harness Mechanics

Status: **draft for review — Rev 3** (design only; no implementation).
Scope owner: architecture.
Supersedes/amends: none yet (this is a proposal; §4 pins the interfaces and names
which verified specs it would amend).
Companion file: `26-dsh-alignment-part2.md` (target design, migration waves, open
questions, revision log).

Authority used in this document. dsh facts are quoted from the locally installed
published packages `@deepseek-ai/*` v0.1.5-rc.2. The `~/.dsh/profiles/node_modules/`
tree is a symlink farm; the real package root used for every citation below is
`/home/yury/.npm/_npx/1e7f6d9597241db0/node_modules/@deepseek-ai/` (citations are
written as `<pkg>/<path>`). Sources are `README.md`, `lib/types/*.d.ts`,
`*.cordis.yml`, and — for prompt prose and centrally allocated constants only — the
`lib/index.js` files. This is the **prompt-prose-only exception** to the general
rule: no other minified `lib/*.js` is used as evidence, and no `lib/index.js`
control-flow/algorithm is inferred from it. Every load-bearing quote was re-checked
against the package file named beside it in Rev 2. ymh facts are quoted from
`docs/design/` and the shipped C++ headers/sources with `file:line` hints.

---

## 1. Purpose and scope

### 1.1 What this document is

The user's instruction is: **"Do not copy code from dsh, but design-copy —
prompts, way of working, agent collaboration, basically all mechanics copy from
dsh. I want ymh to follow exactly the dsh way of interfacing LLM models and how
it processes LLM's output."**

This document is therefore a **mechanics-alignment specification**, not a port:

- It records **how dsh actually behaves**, faithfully, with verbatim prompt text
  and verbatim interface signatures where they are load-bearing.
- It records **where ymh already matches**, where ymh differs, and what the
  delta is, with `file:line` / spec-section evidence.
- It pins a **target design for ymh** as C++23 interface sketches and numbered
  decisions (`26-D1`…), plus invariants and failure modes in the project's
  existing style.
- It gives a **phased migration plan** where every wave is independently
  shippable and testable, and states what is deliberately excluded.

### 1.2 What "design-copy" means, and what it does not

**In scope (copy the design):**

- The **shape of the seams**: a provider-neutral LLM call service distinct from
  adapters; a request envelope that is frozen before dispatch; one provider
  attempt per stream; retry as a separate executor at a durable step boundary.
- The **data model**: message/content/attachment vocabulary, the streaming chunk
  protocol, terminal-result guarantees, usage accounting (dsh's disjoint cached
  and reasoning token convention), and the compact assistant-stream record.
- The **output-processing pipeline**: incremental chunk-to-message assembly,
  replay envelope, tool-call assembly, bounded tool output (retention), and
  replay-safe compaction/pruning.
- The **prompt system**: ordered sections with a centrally allocated order
  table, persona prefix/suffix, workspace instructions (`AGENTS.md`), runtime
  contexts, tool presentation, and the shipped prompt prose.
- The **way of working and collaboration**: presets that compose a session,
  subagents that join the parent composition, goals, background jobs, and
  slash commands that are not model messages.

**Out of scope / forbidden:**

- Copying dsh TypeScript source (types, classes, function bodies) into ymh.
  ymh is C++23; only the *design* and the *user-facing prompt text* are copied.
- Copying the Cordis plugin/composition runtime, the `cordis.yml` file format,
  or the `!!js` expression language. ymh keeps its own JSONC config and its own
  composition mechanism; only the *composition semantics* are mirrored.
- Building the dsh package graph. ymh keeps its existing component specs; this
  document adds the missing mechanics to them.
- Writing implementation code in this document. Interfaces are sketches to be
  pinned by the owning component spec before coding, per `AGENTS.md`.

### 1.3 Scale, stated honestly

This is **not a one-spec change**. It is a multi-spec program touching at least
`00` (architecture), `01` (session/events), `06` (agent loop), `07` (tools),
`08` (LLM provider), `09` (permissions, only at the seams), `13` (compaction),
`17` (transcript), `18` (context), `20` (skills), and `23` (session lifecycle),
plus new component specs for the prompt registry, output retention, presets, and
the goals/jobs/commands layer. Section 5 orders the work into waves; a realistic
count is **7 waves, ~6 new/amended specs, and a new session-event family**.

The single most important sentence in this document: **most of the LLM-facing
delta is not new behaviour — it is making the already-existing request and
response *explicit, frozen, and reconstructable from the log*.** ymh already
streams, already assembles tool calls, already has a terminal outcome, already
records cached/reasoning token counts (though not with dsh's disjoint
semantics — see §3 G9), and already has an event-sourced log. What it lacks is
the *service boundary and the logged request header* that make dsh's mechanics
deterministic and replayable.

---

## 2. dsh mechanics, faithfully

This section describes dsh as shipped. It is deliberately detailed; §3 then
measures ymh against it.

### 2.1 The LLM interface

#### 2.1.1 One provider-neutral call service, adapters below it

`dsh-llm` owns the provider-neutral service `LlmRuntime` (`ctx.llm`) and the
adapter base class `LlmAdapter`. The service is a **concrete** class
(`dsh-llm/lib/types/index.d.ts:231` — `export declare class LlmRuntime extends
TypertRemoteService`); its JSDoc calls it "the abstract `llm` service"
(`:228`), but the TypeScript declaration is not `abstract`. Only `LlmAdapter` is
declared abstract (`:126` — `export declare abstract class LlmAdapter`). The
README states the contract (`dsh-llm/README.md:12`):

> "Every dispatched request remains reconstructable from the session log.
> Requests are deep-frozen before dispatch, so extensions and adapters can read
> them but cannot rewrite them. Each stream is one provider attempt:
> provider-specific translation stays with its adapter, while the optional
> `@deepseek-ai/dsh-llm-retry` package re-runs failed requests. Streams always
> end with a terminal result, so callers can handle success, failure, and
> cancellation consistently."

The service surface (`dsh-llm/lib/types/index.d.ts`):

```ts
export declare class LlmRuntime extends TypertRemoteService {
    registerAdapter(providers: string[], adapter: LlmAdapter): AdapterRegistrationHandle;   // :248
    listProviders(): LlmProviderInfo[];                                                      // :267
    resolveModelInfo(provider: string, model: string, signal?: AbortSignal): Promise<LlmResolvedModelInfo>; // :354
    resolveCallConfig(config: LlmCallConfig, signal?: AbortSignal): Promise<LlmCallConfig>;  // :368
    prepareCall(config: LlmCallConfig, signal?: AbortSignal): Promise<PreparedLlmCall>;       // :380
    stream(options: GenerateOptions): AsyncIterable<StreamChunk>;                             // :406
    providerRetryPolicy(provider: string): ResolvedRetryPolicy;                               // :318
}
```

The adapter contract (`dsh-llm/lib/types/index.d.ts:126`):

```ts
export declare abstract class LlmAdapter {
    abstract stream(options: GenerateOptions): AsyncIterable<StreamChunk>;   // :182 — the only required method
    listModels(_provider: string): Promise<readonly LlmModelInfo[]>;          // :156
    resolveModel(provider: string, model: string, _signal?: AbortSignal): Promise<LlmResolvedModelInfo>; // :166
    prepareCall(provider: string, model: string, signal?: AbortSignal): Promise<PreparedAdapterCall>;    // :176
    imageRequestPricing(_provider: string, _model: string): LlmImageRequestPricing | undefined;          // :148
}
```

Two design rules are visible in this surface and matter for ymh:

1. **The service is the only path to a provider.** "it is the only supported
   path into provider adapters, and it keeps one vocabulary across the loop, the
   session log, and every consumer" (`dsh-llm/README.md:32`).
2. **Preparation is generation-bound.** `prepareCall` "Bind exact model metadata
   and the eventual request dispatch to one adapter generation" so that "settings
   changes between preparation and dispatch cannot combine one generation's
   capabilities with another's endpoint" (`index.d.ts:167-176`). The result
   (`index.d.ts:90-112`):

```ts
export interface PreparedLlmCall {
    readonly config: LlmCallConfig;                       // deep-frozen, adapter defaults materialized
    readonly retryPolicy: ResolvedRetryPolicy;            // captured with the adapter registration
    readonly context?: LlmModelContext;
    readonly inputModalities?: readonly ModelModality[];
    readonly systemPromptUpdate?: SystemPromptUpdate;
    readonly adapterDefaults: LlmCallConfigAdapterDefaults;
    stream(options: GenerateOptions): AsyncIterable<StreamChunk>;  // dispatch once; mismatch => INVALID_PREPARED_CALL
}
```

`PreparedLlmCall.stream` "Dispatch this call once through the registration
captured during preparation. The request's call-config fields must match
`config`; reuse or mismatch fails with `INVALID_PREPARED_CALL`"
(`index.d.ts:104-111`). This is the mechanism that makes "one provider attempt
per stream" enforceable rather than aspirational.

#### 2.1.2 The request envelope and call-config as logged header state

`GenerateOptions` is the fully assembled request (`types.d.ts:404-444`):

```ts
export interface GenerateOptions {
    provider: string;
    model: string;
    reasoningEffort?: ReasoningEffortId;
    messages: Message[];          // exactly as the provider sees them; leading system message carries the prompt
    system?: string;              // one-shot callers only; loop-built requests leave it undefined
    tools?: ToolSchema[];
    temperature?: number;
    maxTokens?: number;
    stop?: string[];
    signal?: AbortSignal;
    sessionId?: Branded<'SessionId'>;
    purpose?: 'compaction' | 'session-title';
}
```

`ToolSchema` is deliberately declared in `dsh-llm` "because it is part of
`GenerateOptions`; dsh-tools' ToolDefinition and dsh-system-prompt's
PromptAssembly both import it from this package" (`types.d.ts:390-402`):

```ts
export interface ToolSchema { name: string; description: string; parameters: Record<string, unknown>; }
```

Call configuration is a separate, **logged header** value, not per-call
sampling (`call-config.d.ts:1-23`):

```ts
/**
 * Conversation call configuration and freeze utilities. Provider routing,
 * model, reasoning effort, and sampling values are request-header state that
 * can affect cache reuse; request waterfalls replace them and the loop logs
 * changed snapshots instead of allowing silent per-call drift.
 */
export interface LlmCallConfig {
    provider: string;
    model: string;
    reasoningEffort?: ReasoningEffortId;
    temperature?: number;
    maxTokens?: number;
    stop?: string[];
}
export declare function callConfigEquals(a: LlmCallConfig, b: LlmCallConfig): boolean;  // field-wise incl. stop[]
```

`callConfigEquals` is "the comparison a caller runs to decide whether a proposed
configuration is a real change (worth a logged header snapshot) or the held one
restated" (`call-config.d.ts:33-40`). `markAgentLoopRequest` / `isAgentLoopRequest`
(`call-config.d.ts:46-52`) tag the loop-owned envelope so the `llm/stream`
waterfall can tell a loop request from a hand-built one-shot.

The request waterfall (`index.d.ts:34-45`) is the extension point:

```ts
'llm/stream'(this: LlmRuntime, options: GenerateOptions, next: () => AsyncIterable<StreamChunk>): AsyncIterable<StreamChunk>;
```

#### 2.1.3 The streaming chunk protocol and terminal result

`StreamChunk` (`types.d.ts:352-389`) is the single wire vocabulary:

```ts
export type StreamChunk =
  | { type: 'block-start'; index: number; blockType: ContentBlockType }
  | { type: 'text-delta'; index: number; text: string }
  | { type: 'reasoning-delta'; index: number; text: string }
  | { type: 'tool-call-delta'; index: number; id: ToolCallId; name?: string; argumentsDelta: string }
  | { type: 'block-end'; index: number; block: ContentBlock }
  | { type: 'usage'; usage: TokenUsage }
  | { type: 'finish'; reason: FinishReason; replayState?: ReplayEnvelope };
```

Rules carried by the type doc: "Adapters emit usage before the terminal finish
and nothing afterward; tool arguments remain raw JSON strings. An adapter
implementation may throw, but `LlmRuntime.stream()` normalizes that failure to a
terminal `error` or `aborted` finish before exposing it to consumers"
(`types.d.ts:354-357`). The final adapter boundary "Adapter selection, dispatch,
iterator construction, and iteration failures become one terminal failure chunk"
(`index.d.ts:389-394`).

Finish reasons are merge-extensible and carry failure facts
(`types.d.ts:107-127`):

```ts
export interface FinishReasonMap {
    'stop': { kind: 'stop' };
    'tool-calls': { kind: 'tool-calls' };
    'max-tokens': { kind: 'max-tokens' };
    'aborted': { kind: 'aborted'; failure: LlmFailure };
    'error': { kind: 'error'; failure: LlmFailure };
}
```

`LlmFailure` (`types.d.ts:26-37`) carries a stable provider-neutral `code`,
HTTP `status?`, `providerRetryAfterMs?`, `requestId?`. Stable codes include
`NO_ADAPTER`, `MISSING_CREDENTIAL`, `AUTH`, `RATE_LIMIT`,
`CONTEXT_WINDOW_EXCEEDED`, `EMPTY_RESPONSE`, `INVALID_CREDENTIAL`
(`dsh-llm/README.md:70-71`, `error.d.ts:17-46`). `EMPTY_RESPONSE` exists because
"an empty message silently ends the turn with nothing for the user or the loop
to act on. The attempt produced nothing durable, so retry policy treats it as
safe to repeat" (`error.d.ts:21-30`).

#### 2.1.4 The message / content / attachment model

One immutable message representation is shared by delivery, durable history, and
model requests (`message.d.ts:119-153`):

```ts
export interface Message {
    readonly id: MessageId;
    readonly role: 'system' | 'user' | 'assistant';
    readonly content: ContentBlock[];
    readonly source: MessageSource;
}
export interface AssistantMessage extends Message { readonly role: 'assistant'; readonly source: ModelMessageSource; }
export interface ToolResultMessage extends Message { readonly role: 'user'; readonly content: [ToolResultBlock]; readonly source: ToolMessageSource; }
export interface SystemMessage extends Message { readonly role: 'system'; readonly source: MessageSourceMap['plugin']; }
```

Note the deliberate inversion vs. the OpenAI wire: **a tool result is a
user-role message whose single content block is a `tool-result`**, correlated by
`toolCallId`; the adapter re-serializes it to the provider's `tool` role.
`createMessage`/`createUserMessage`/`createAssistantMessage`/
`createSystemMessage`/`createToolResultMessage` all "Detach and deep-freeze"
before publication (`message.d.ts:161-212`).

Content blocks (`types.d.ts:91-102`):

```ts
export interface ContentBlockMap {
    'text': TextBlock; 'reasoning': ReasoningBlock; 'image': ImageBlock; 'file': FileBlock;
    'tool-call': ToolCallBlock; 'tool-result': ToolResultBlock;
}
export type ContentBlockType = keyof ContentBlockMap;
export type ContentBlock = ContentBlockMap[ContentBlockType];
```

`ToolCallBlock.arguments` is "Raw JSON string as produced by the model"
(`types.d.ts:71-79`); `ToolResultBlock` carries `content: ContentBlock[]` and
`isError?` (`types.d.ts:80-86`). `FileBlock` never reaches a provider: "request
assembly projects every occurrence to deterministic handle text (name, byte
size, and the read-only saved path), so adapters and providers see text in its
place while the durable log keeps the structured reference"
(`types.d.ts:59-70`).

Message source is a merge-extensible sum with a **semantic** (never visual)
context form (`message.d.ts:42-101`): `'instructions' | 'catalog' | 'snapshot' |
'notice' | 'relay' | 'recall'`, where a `snapshot` carries named
`ContextSnapshotSection`s and a `notice` carries a bounded one-line summary
(`CONTEXT_SUMMARY_MAX_CHARS = 120`, `boundContextSummary`, `message.d.ts:105-116`).

#### 2.1.5 Usage accounting

`TokenUsage` (`types.d.ts:128-150`) is **disjoint**:

```ts
/**
 * Counts are DISJOINT: `inputTokens` is uncached input only; cached input is
 * reported separately as `cacheReadTokens`/`cacheWriteTokens` (billed input =
 * sum of the three). Adapters whose providers fold cache hits into a total
 * prompt count (DeepSeek's `prompt_tokens`) subtract them out.
 */
export interface TokenUsage {
    inputTokens: number;
    outputTokens: number;
    totalTokens?: number;
    cacheReadTokens?: number;
    cacheWriteTokens?: number;
    reasoningTokens?: number;
}
```

#### 2.1.6 The DeepSeek adapter

`dsh-llm-deepseek` is "transport-only: connection facts arrive through a thunk
resolved once per operation and the bearer token through a per-request resolver"
(`adapter.d.ts:3-6`). Its wire types (`dsh-llm-deepseek/lib/types/types.d.ts`)
show the translation contract:

```ts
export interface WireRequest {
    model: string; messages: WireMessage[];
    stream: true; stream_options: { include_usage: true };
    thinking?: { type: 'enabled' | 'disabled' };      // top level, NOT extra_body
    reasoning_effort?: 'low' | 'high' | 'max';
    tools?: WireTool[]; temperature?: number; max_tokens?: number; stop?: string[];
}
export interface WireAssistantMessage {
    role: 'assistant'; content: string | null;
    reasoning_content?: string;   // CoT passback; REQUIRED on tool-call turns in thinking mode
    tool_calls?: WireToolCall[];
}
export interface WireUsage {
    prompt_tokens: number; completion_tokens: number; total_tokens?: number;
    prompt_cache_hit_tokens?: number; prompt_cache_miss_tokens?: number;
    prompt_tokens_details?: { cached_tokens?: number };
    completion_tokens_details?: { reasoning_tokens?: number };
}
```

`translate.d.ts:1-5`: "Translate DeepSeek SSE payloads with one stateful harness
block per content, reasoning, or tool call index. An empty initial reasoning
delta does not open a block. Finish reason and the latest usage are deferred
until `[DONE]`, covering both finish-attached and trailing usage-only shapes
while ensuring no chunk follows `finish`." `mapUsage` "DeepSeek's `prompt_tokens`
INCLUDES cache hits ... the harness TokenUsage convention is DISJOINT counts, so
cache reads are subtracted out of `inputTokens`" (`translate.d.ts:19-22`).
`parseSse` is spec-strict: "an event dispatches only on its blank-line
terminator, so an unterminated tail at EOF is truncation, not a flushable
payload"; it throws `STREAM_CLOSED` if `[DONE]` is missing (`sse.d.ts:1-23`).
`mapFinishReason` maps unrecognized reasons (e.g. `content_filter`) to
`{kind: 'error'}` (`translate.d.ts:12-17`).

The adapter config default (`dsh-llm-deepseek/README.md:55-71`): `thinking`
default `enabled`, `reasoningEffort` default `high`, `maxTokens` 256,000,
`defaultContextWindow` 1,000,000, `retryPolicy` "normal, 5 retries".

#### 2.1.7 Retry is a separate package at a durable boundary

`dsh-llm` "has no configuration and no provider wire code" and
"**No retry execution, caching, or rate limiting ships in this service** —
provider registration stores the retry policy, but a stream remains a single
provider attempt; `@deepseek-ai/dsh-llm-retry` executes the policy at durable
agent-step boundaries" (`dsh-llm/README.md:28,153`).

`RetryPolicyConfig` (`retry-policy.d.ts:10-38`):

```ts
export interface BackoffConfig { initialDelayMs?: number; maxDelayMs?: number; jitterRatio?: number; }
export interface NormalRetryPolicyConfig { mode: 'normal'; maxRetries?: number; retryableCodes?: string[]; backoff?: BackoffConfig; }
export interface AlwaysRetryPolicyConfig { mode: 'always'; backoff?: BackoffConfig; }
export type RetryPolicyConfig = NormalRetryPolicyConfig | AlwaysRetryPolicyConfig;
export type ResolvedRetryPolicy = ResolvedNormalRetryPolicy | ResolvedAlwaysRetryPolicy;
```

Retry durability (`dsh-llm-retry/lib/types/types.d.ts:4-41`): the session event
map gains `'llm/retry'` ("Durable, non-surface record of one provider-routed
retry scheduled after a failed request attempt") and `'llm/retry-started'`
("Durable transition written after a retry wait succeeds and before the next
request attempt starts"). The retry payload is a **discriminated union on
`mode`** (`types.d.ts:12-34`):

```ts
export type LlmRetryEventData =
  | { retryId: RetryId; turn: number; step: number; provider: string; mode: 'normal';
      policyKey: string; retry: number; maxRetries: number; delayMs: number; failure: LlmFailure; }
  | { retryId: RetryId; turn: number; step: number; provider: string; mode: 'always';
      policyKey: string; retry: number; delayMs: number; failure: LlmFailure; };
export interface LlmRetryStartedEventData { retryId: RetryId; turn: number; step: number; retry: number; }
```

All fields in both union members are required; the only field difference is
`maxRetries`, present **only** in the `mode: 'normal'` member. The executor
"has no config; providers own `retryPolicy`" (`dsh-llm-retry/lib/types/index.d.ts:14`).

#### 2.1.8 Assistant-stream compaction and replay

`BlockAssembler` is "the single canonical assembly algorithm used by the agent
loop to build an assistant message from a chunk stream while logging the raw
chunks for replay fidelity" (`assembler.d.ts:1-6`). It is "Tolerant of delta-only
protocols (no block-start/end); deltas arriving for an index already closed by
`block-end` are ignored (malformed stream)" (`assembler.d.ts:17-20`).
`interruptedBlocks()` "Assemble the prefix an interrupted stream can safely
finalize ... Tool calls are omitted because interruption precedes dispatch;
retaining one would require a fabricated result" (`assembler.d.ts:50-57`).
`finish` returns `{kind: 'stop'}` "when the stream ended without one"
(`assembler.d.ts:60`).

`AssistantStreamAccumulator` compacts an attempt into `AssistantStreamRecord`s
(`assistant-stream.d.ts:15-63`): packed `text-chunks`/`reasoning-chunks`/
`tool-call-chunks` runs plus raw `chunk` records; `expandAssistantStream`
restores "the exact timed chunk sequence ... with every original delta boundary
preserved" (`assistant-stream.d.ts:65-71`). `ReplayEnvelope` is "Adapter-private
lossless-JSON state for replaying a successful response, carried by a terminal
`finish` chunk and stored on the assembled assistant message's model source"
(`types.d.ts:331-350`).

### 2.2 Output processing and the agent loop

#### 2.2.1 Step/turn model and the pre-step boundary

`dsh-agent-loop` "creates scoped ReactLoopAgents, publishes them through the
agent/session registries, and owns their ordered teardown"
(`dsh-agent-loop/lib/types/index.d.ts:1-5`). `ReactLoopAgent` "Drives one
session through turn and step boundaries" (`agent-loop/agent.d.ts:11-13`). The
inbox has two durable lists, `'next-turn'` and `'next-step'`
(`dsh-agent/types.d.ts:24-30`); `ReactLoopInbox` reconstructs pending input from
"durable inbox splices" (`agent-loop/inbox.d.ts:11-38`).

The agent surface (`agent-loop/agent.d.ts:40-45`):

```ts
send(message: UserMessage, target: InboxTarget, wakeup: boolean): void;
followup(input: UserMessage): void;
steer(input: UserMessage): void;
inject(input: UserMessage): void;
cancel(cause: AgentCancelCause, options?: CancelOptions): void;
runMaintenance<T>(job: (signal: AbortSignal) => Promise<T>): Promise<T>;
```

`steer` targets the nearest step boundary; `followup` queues a turn; `inject`
adds non-triggering context. The turn/step boundary is a durable projection
(`agent-loop/index.d.ts:13-25`, `dsh-agent/types.d.ts:52-63`):

```ts
export interface TurnBoundaryProjection {
    readonly openTurnStartSeq: OptionalSessionSeq;   // seq of the open turn's turn/start, or null
    ...
}
```

The loop reads "the latest durable routed request" for pressure policy
(`dsh-compaction/lib/types/index.d.ts:78-80`), i.e. the step decision is derived
from the log, not from in-memory drift. A `pre-step` listener is the documented
extension point: the goal round driver installs an `agent/pre-step` listener
that "verifies the complete claimed record against the current goal both before
and after downstream listeners" (`/tmp/opencode/dsh-collab.md:1509-1513`,
paraphrasing `dsh-goal-round-driver`). The system-prompt projection makes the
series decision at this boundary (`dsh-agent-loop/lib/types/runtime-context.d.ts:17-27`):

```ts
export interface SystemPromptDecisionInput {
    inHistory: boolean;      // does the prepared route read a later system message as the effective prompt?
    startsSeries: boolean;   // a pre-step listener declared one, the surface was replaced, or schemas differ
}
```

`SystemPromptProjection` decides "how a rendered system prompt reaches the
surface without owning the commit. The first prompt, even empty, reserves
surface node 0. A capable continuing series appends changed nonempty text after
the cached history. An incapable route, broken series, or cleared prompt instead
normalizes the first system node and empties later active nodes"
(`runtime-context.d.ts:28-35`). This is the KV-cache-preserving system-prompt
update path, and it depends on `LlmResolvedModelInfo.systemPromptUpdate ===
'in-history'` (`types.d.ts:313-330`).

#### 2.2.2 Tool declaration, invocation, and result feeding

`executeToolCalls` schedules "one assistant step's tool calls by their live
concurrency mode. Ordinary completion and abort commit started-call results in
order. Abort drains them, records synthetic results for unstarted calls ... An
internal scheduler failure stops new dispatches, drains already-started
dispatches, and rejects with the first failure without fabricating tool results"
(`agent-loop/tool-calls.d.ts:16-37`). Exclusive calls form barriers; parallel
calls use "a bounded rolling pool"; dispatch may overlap "while policy, results,
and result context remain model-ordered." The default bound is
`DEFAULT_MAX_PARALLEL_TOOL_CALLS = 10` (`agent-loop/constants.d.ts:5`).

The tool catalog is **stable across mode transitions**. Plan mode "keeps the
tool catalog stable and changes only the `plan:policy` section from order 500
onward, so the request-cache prefix before 500 stays valid; the exit tool
remains registered in both states" (`dsh-plan-mode/README.md:144-146`, quoted in
`/tmp/opencode/dsh-prompts.md:751-753`). Models see each permitted tool's "name,
description, and parameter schema; `output`, `execute`, `finalizeContent`,
`timeoutMs`, and presentation callbacks never leak onto the wire"
(`dsh-tools/README.md:103`, quoted in `/tmp/opencode/dsh-prompts.md:385-387`).

#### 2.2.3 Bounded output: deque, chunked-list, retention

Three dependency-light libraries implement bounded, replay-safe output:

- **`Deque<T>`** — "A circular deque with amortized constant-time insertion and
  removal. Removed entries are cleared immediately, and sparse storage shrinks
  after the live entry count reaches one quarter of its capacity"
  (`dsh-deque/lib/types/index.d.ts:5-38`). It backs "queues that retain entries
  across asynchronous work."
- **`ChunkedList<T>`** — "Persistent append-only lists with bounded copying and
  JSON checkpoint validation" (`dsh-chunked-list/lib/types/index.d.ts:1-33`).
  `appendChunkedList` copies "at most one 64-value chunk"; `chunkedListSchema`
  "reject[s] empty or oversized chunks and unknown fields."
- **`dsh-output-retention`** — "A dependency-light **retention** library:
  bounded model-facing output for tools that must cap how much context they
  return" (`dsh-output-retention/lib/types/index.d.ts:1-30`). Two retainers:
  `ItemRetainer` (ordered logical units, `head` only in v1) and `TextRetainer`
  (byte-oriented, `head`/`tail`/`headTail`, UTF-8-boundary safe). It "owns ONLY
  the mechanical question 'what did we keep, what did we omit?'"; tool-domain
  states (permission failures, provider partial failures) "stay in tool-domain
  fields, never folded into `truncated`" (`index.d.ts:6-15`). `formatRetentionNotice`
  joins a library-owned omission clause with tool-owned recovery words
  (`index.d.ts:200-224`).

#### 2.2.4 Compaction and tool-result pruning

Compaction is a **projection over the append-only log**, never a deletion, and
is recorded as log-only events (`dsh-compaction/lib/types/types.d.ts:14-99`):
`compaction/start` (holds the lock), `compaction/summary` (summary content,
`shadowedRange`, `shadowedSeqs`, `shadowedTokenCount`, `provider`, `model`),
`compaction/end`, and `compaction/prune` (shadow price of a model-free prune).
"a surface `replace` event is priced by the metering event immediately
preceding it" (`types.d.ts:79-98`). The summary "content is in `data.summary`;
the actual surface replacement is performed by the immediately following
`user/message` event that shadows the compacted range" (`types.d.ts:26-34`).

`CompactionEngine` (`dsh-compaction/lib/types/index.d.ts:75-110`):

```ts
abstract compactIfNeeded(agent, trigger: 'pressure' | 'context-overflow', signal): Promise<CompactionResult | null>;
abstract compactNow(agent, signal, sourceCommandId?): Promise<CompactionResult>;
```

"Pressure policy uses the latest durable routed request, while context-overflow
policy may force a useful balanced reduction even below the normal threshold"
(`index.d.ts:78-82`).

`dsh-compaction-tool-result-pruner` is a "Replay-safe, model-free tool-result
pruning service" (`.../lib/types/index.d.ts:1-4`). Defaults
(`.../lib/types/types.d.ts:3-11`): `thresholdChars = 8192`, `headChars = 4096`,
`tailChars = 1024`. Budgets are measured in **Unicode code points**, not bytes:
`codePointLength(text)` "Count Unicode code points without splitting surrogate
pairs" (`.../lib/types/config.d.ts:7-12`), and the removed middle span is
replaced with the fixed `PRUNE_MARKER = "\n\n[... tool result middle pruned
...]\n\n"` (`config.d.ts:4`). `pruneSession` "Prune[s] every over-budget tool
result from one stable current-surface snapshot. Each replacement preserves the
complete event data except for `content`, cites the shadowed node so replay can
recover the replacement input, and is immediately preceded by a
`compaction/prune` shadow-price event" (`index.d.ts:40-51`).

#### 2.2.5 Loop protection and steering

- **Repeat-tool reminder** (`/tmp/opencode/dsh-prompts.md:680-698`): thresholds
  default `[3,5,8]`. Gentle: *"You are repeating the exact same tool call with
  identical arguments. Carefully analyze the previous result before calling
  again: if the task is not complete, try a different approach or different
  arguments instead of repeating the call."* Detailed: *"Repeated tool call
  detected: ... The repeated calls are not making progress. Do not call this
  tool with these exact arguments again..."* Stamped `source: { kind: "plugin",
  plugin: "repeat-tool-reminder" }`, append-only.
- **Steering**: `steer` injects into the next-step inbox; `followup` queues a
  turn; the inbox is durable (`next-step` cleared before `next-turn`,
  `inbox.d.ts:49`). This is the same mechanism the goal round driver uses to
  yield to human work.

### 2.3 The prompt system

#### 2.3.1 The ordered-section registry

`dsh-system-prompt` is "Registry for ordered system sections, dynamic context,
tool schemas, and prompt variables" (`dsh-system-prompt/lib/types/index.d.ts:1-4`).
The exact declaration surface is:

```ts
// :37-45
export interface AssembleContext {
    scope?: ScopeKey;              // scope whose providers/listeners participate; absent => global only
    signal?: AbortSignal;
}
// :47-68
export interface PromptSection {
    readonly name: string;         // unique; duplicate registration throws
    readonly order: number;        // ascending; equal orders use code-unit name order
    readonly text: string | ((context: AssembleContext) => string);  // `{{var}}` interpolated later
    readonly complete?: boolean;   // at most one effective complete section; >1 fails assembly
}
// :70-77
export interface PromptContext {
    readonly name: string;         // unique; duplicate registration throws
    readonly order: number;        // contexts joined ascending
    readonly text: string | ((context: AssembleContext) => string);  // empty text contributes nothing
}
// :79-84 / :86-91
export interface AssembledSection { name: string; text: string; }
export interface AssembledContext { name: string; text: string; }
// :103-108
export interface PromptAssembly {
    sections: AssembledSection[];
    contexts: AssembledContext[];
    tools: ToolSchema[];                                    // already in canonical order
    variables: Record<string, string | undefined>;
}
// :193
export declare function renderPrompt(assembly: PromptAssembly): string;
```

The registry service is `SystemPrompt extends Service`
(`index.d.ts:220`). Every registration returns the **exact Cordis effect
disposer** `() => void`; there are no `SectionHandle`/`ContextHandle` types in
the package (verified by grep — the actual types are `PromptSection` and
`PromptContext`):

```ts
section(section: PromptSection): () => void;                       // :233
getSectionOrder(name: PromptSectionOrderName): number;             // :239
getContextOrder(name: PromptContextOrderName): number;             // :245
context(context: PromptContext): () => void;                       // :252
suppressRuntimeContext(): () => void;                              // :259
tools(provider: (context: AssembleContext) => ToolProviderResult): () => void;   // :267
variable(name: string, provider: (context: AssembleContext) => string | undefined): () => void; // :276
assemble(context?: AssembleContext): Promise<PromptAssembly>;      // :286
```

`assemble()` "Assemble global and scoped providers, detach tool parameters,
apply canonical ordering, then run the assembly waterfall. Scoped sections and
variables shadow globals. The returned waterfall value is authoritative except
that an effective complete section is restored afterwards as the sole prompt
section" (`index.d.ts:277-285`). The assembly waterfall is `system-prompt/assemble`
(`index.d.ts:14-27`); the `system-prompt/change` event is unfiltered.

Composition rules (all confirmed in `index.d.ts`): sections concatenate in
ascending order, ties by code-unit name order (`:50-54`); contexts join in
ascending order (`:73`); `renderPrompt` "Interpolate strict `{{variable}}`
references, drop empty sections, and join the rest with blank lines. Malformed,
unknown, or undefined references throw; a lone `{{` without any later `}}` is
literal prose, and substituted values are not scanned again"
(`:185-193`); "More than one effective complete section makes assembly fail"
(`:65`); `toolOrder` is "Model-facing tool names in order, with
{@link TOOL_ORDER_REST} exactly once. Invalid fields fail at load and unknown
names fail at assembly; known names hidden in one scope may be absent there.
Omitted means lexicographic order" (`:179-183`). The reserved
`TOOL_ORDER_REST` name is exported (`:161`), as are `PERSONA_PREFIX_SECTION`
(`:157`) and `PERSONA_SUFFIX_SECTION` (`:159`). The rendered prompt reaches the
model "as a **system-role message of derived history** (surface node 0, or the
latest system node after an in-history update) — not a separate request field."

#### 2.3.2 The canonical order tables (verbatim)

`SECTION_ORDERS` is defined at `dsh-system-prompt/lib/index.js:10-42` and
`CONTEXT_ORDERS` at `:43-47` (the compiled declarations are
`dsh-system-prompt/lib/types/index.d.ts:109-141` and `:144-148`). The values are
identical in both forms; the compiled JS writes round numbers as `1e3`, `2e3`,
`5e3`, `9e3`, `1e4`:

```ts
declare const SECTION_ORDERS: {
    readonly HARNESS_IDENTITY: -1000;
    readonly DEPLOYMENT_PERSONA_PREFIX: 0;
    readonly PLAN_POLICY: 500;
    readonly TEAM_POLICY: 600;
    readonly PTC_ONLY: 800;
    readonly FILE_REFERENCE: 900;
    readonly TOOL_BASH: 1000;
    readonly TOOL_PWSH: 1010;
    readonly TOOL_READ: 1100;
    readonly TOOL_WRITE: 1200;
    readonly TOOL_EDIT: 1300;
    readonly TOOL_GLOB: 1400;
    readonly TOOL_GREP: 1500;
    readonly TOOL_JOBS: 1600;
    readonly TOOL_PTY: 1700;
    readonly TOOL_WEB_SEARCH: 2000;
    readonly TOOL_WEB_FETCH: 2100;
    readonly TOOL_LSP: 2200;
    readonly TOOL_SESSION_QUERY: 2300;
    readonly TOOL_GOAL: 2400;
    readonly TOOL_CORDIS: 2500;
    readonly TOOL_WORKFLOW: 2600;
    readonly TOOL_RALPH: 2700;
    readonly TOOL_SUBAGENT: 2800;
    readonly TOOL_REPORT: 2900;
    readonly TOOLS_SDK: 5000;
    readonly DELIVERABLE_FILE_REFERENCES: 9000;
    readonly STRUCTURED_OUTPUT: 9900;
    readonly HARNESS_SOURCE: 10000;
    readonly WEB_SURFACE: 10100;
    readonly DEPLOYMENT_PERSONA_SUFFIX: 10200;
};
declare const CONTEXT_ORDERS: {
    readonly SANDBOX_POLICY: 110;
    readonly APPROVAL_POLICY: 115;
    readonly SUBAGENT_DELEGATION: 120;
};
```

"**'first-party prompt order 500'** means exactly
`SECTION_ORDERS.PLAN_POLICY === 500`." These orders are "repository-owned,
centrally allocated" constants; access is via `getSectionOrder(name)` /
`getContextOrder(name)` (`index.d.ts:239,245`).

#### 2.3.3 Persona (prefix order 0 / suffix order 10200)

Deployment persona is config on the registry row (`personaPrefix`,
`personaSuffix`); per-agent persona is the `dsh-persona` row, mounted **inside a
preset scope only** (`/tmp/opencode/dsh-prompts.md:210-234`):

```ts
const Config = z.object({
    prefix: z.string().required(),
    suffix: z.string().default(""),
    complete: z.boolean().default(false),
    includeRuntimeContext: z.boolean().default(true)
});
```

It registers `deployment:persona-prefix` (order 0, `complete: true` iff
configured) and `deployment:persona-suffix` (order 10200), shadowing the
deployment defaults for that scope. Shipped texts (verbatim,
`/tmp/opencode/dsh-prompts.md:236-260`):

```yaml
# standard and ptc presets
- id: persona
  name: '@deepseek-ai/dsh-persona'
  config:
    suffix: Your working directory is {{cwd}}.
    prefix: >-
      You are a coding agent powered by the {{model}} model.
```

```yaml
# minimal preset — persona is the complete system prompt
- id: persona
  name: '@deepseek-ai/dsh-persona'
  config:
    prefix: You are a helpful software engineer assistant.
    complete: true
    includeRuntimeContext: false
```

The minimal preset comment: *"The persona is the complete system prompt, so
global identity, Web orientation, tool guidance, and later assembly listeners
cannot add prompt text."* Subagents get a per-child
`deployment:persona-prefix` from their composition
(`dsh-subagent/lib/index.js:549-553`).

#### 2.3.4 Shipped prompt texts (verbatim)

Harness identity (order −1000, `dsh-system-prompt/lib/index.js:216`):

```
You are an AI agent powered by DeepSeek Harness.
```

Runtime-context header (`lib/index.js:130-134`, via
`/tmp/opencode/dsh-prompts.md:198-202`):

```
Current runtime context. This snapshot supersedes earlier runtime-context snapshots.

<body>
```

Plan-mode policy (order 500, `dsh-base/cordis.patch.yml:304-315`). The `section:`
block scalar begins at `:304`; the **six** paragraphs are the content lines
`:305`, `:307`, `:309`, `:311`, `:313`, `:315`, separated by blank lines. Quoted
here in full and complete:

```
You are in plan mode. Stay in plan mode until exit_plan_mode succeeds or the user switches the session mode. Imperative language to implement changes means plan the implementation, not execute it. A user's conversational agreement — including an answer confirming something you asked — approves nothing and does not end plan mode; fold the confirmed decision into the plan and submit it through exit_plan_mode.

Explore first. Use non-mutating reads, searches, static analysis, and checks to ground the plan in the actual repository. Do not edit or write files, change configuration, run formatters or code generation that rewrites tracked files, commit, or otherwise carry out the plan. Prefer existing functions and patterns over new machinery.

The tool catalog stays the same across modes for request-cache stability. These plan-mode rules override any later tool description or guidance that suggests using mutation tools; those tools remain listed only to keep the request shape stable. Do not use todo_write to track this planning phase: it tracks implementation after an approved plan, while the plan itself belongs in exit_plan_mode.

Resolve discoverable facts by inspection. Use ask_user_question only for user-owned choices or material ambiguity that inspection cannot answer. Do not ask the user where code lives or how current behavior works when you can find out.

Make the plan decision-complete: state the goal and success criteria; group implementation changes by subsystem; identify public API, schema, and data-flow changes; cover edge cases, failure modes, tests, acceptance criteria, and explicit assumptions. Keep it concise enough to review but detailed enough that another engineer can implement it without making design decisions.

When ready, call exit_plan_mode with the complete plan markdown, starting with a # title. Make exit_plan_mode the only and final tool call in that assistant response: it presents the plan for approval, and implementation begins only in a later step after approval. Do not paste the final plan as a plain reply or ask "should I proceed?" through prose or ask_user_question. If review rejects it, incorporate the feedback and present again. If the review channel is unavailable or aborted, stay in plan mode and ask the user to switch modes manually; do not proceed with implementation.
```

Tool-guidance sections (all from the tool packages' `lib/index.js`; each at its
`TOOL_*` order, via `/tmp/opencode/dsh-prompts.md:464-527`). Two of these texts
are **runtime interpolations**, not literals; the resolved forms are shown and
the substitution is named:

```
tool:bash   (1000): Check the [exit code: N] marker on every bash result; investigate failures before moving on.
tool:read   (1100): Use the read tool — not shell commands like cat — to inspect text files. Results include line numbers. Use offset and limit to continue reading large files.
tool:write  (1200): Use the write tool to create files or completely replace file contents. Existing files are overwritten, so read an existing file first (the default fs-observation-policy requires it) and prefer edit for targeted changes.
tool:edit   (1300): Use the edit tool for targeted changes to existing UTF-8 text files. It replaces literal old_string with new_string; by default old_string must appear exactly once. If old_string appears multiple times, provide a more specific old_string or set replace_all to true. Read the file first (the default fs-observation-policy requires it), unless you just created or edited it in this session.
tool:glob   (1400): Use the glob tool — not shell find — to discover files by path pattern. A pattern with no "/" matches basenames at any depth, so "*" matches every file in the tree rather than its top level. Results are files only, never directories, and include hidden and ignored files: a result that fits comes back in modification-time order, while a larger one is sampled across top-level entries, so it spans the tree instead of one subtree.
tool:grep   (1500): Use the grep tool — not shell grep or rg — to search file contents. Use read on a matched file when you need surrounding context.
tool:jobs   (1600): Track every background job id you start. You are notified in-session when a job finishes — do not busy-poll or sleep on one; keep working on independent steps and do not duplicate a running job's work. Before giving a final answer, collect every still-relevant job with job_output (set wait: true only when you are genuinely blocked on it), and job_kill jobs that stopped mattering.
tool:goal   (2400): Use goal tools for one long-running completion objective in the current session. create_goal may infer goal intent from a direct human request in any language; do not create a goal for routine single-turn work. Call get_goal before update_goal and copy its exact goal_id and revision. After session resume or fork, an active goal is disarmed: when a human asks to continue or resume in any wording or language, use update_goal action resume to rearm it. Mark complete only when the objective is actually achieved. Mark blocked only after the same blocking condition persists for at least <blockedAfterConsecutiveRounds> consecutive goal rounds, and report that concrete condition in blocked_reason; difficulty, uncertainty, or useful remaining work is not blocked.
tool:subagent (2800, continuable+background): Use <toolName> in the background by default. Start independent delegations together in one assistant message and continue useful work while they run. Set `run_in_background: false` only when your next action depends on that subagent's result. When a background run settles, the runtime sends you a notice containing its outcome and any final assistant message.
context:file-reference (900): Tokens prefixed with @ are workspace paths the user explicitly referenced, relative to the workspace root. A trailing slash marks a directory: list it when its contents matter. Anything else is a file: use the read tool when its contents are needed, and do not claim to have inspected it before reading. @"..." quotes a path containing spaces.
```

Substitution provenance: the glob guidance's trailing clause is
`${overCapGuidance}`, a `const` selected by `caps.sampleOverCapGlobResults`
(`dsh-tool-fs-search/lib/index.js:774`); the README shows the resolved text for
both settings (`dsh-tool-fs-search/README.md:139-149`), and the `true` form is
quoted above. The goal guidance interpolates
`blockedAfterConsecutiveRounds` (default `3`,
`dsh-tool-goal/lib/index.js:115,195-196,262`). The tokens
`<overCapGuidance>`/`<blockedAfter>` are **not shipped literals** (grep finds no
such token); they are used here only as named substitution points.

PTC collapse section (order 800, `dsh-tools/lib/index.js:2421`):

```
`run_code` is the only tool you can call directly — a tool call naming any other tool fails. Reach every tool the SDK declares below from inside the program.
```

Harness source section (order 10000, `dsh-app-boot/lib/index.js:1567-1572`):

```
The DeepSeek Harness implementation checkout is at <sourceRoot>. The checkout location and current working directory are separate values and may differ; never infer the working directory from this path. Use pwd to determine the current working directory. Use this checkout only to inspect or extend DSH itself.
```

Structured-output instruction (`dsh-subagent-in-process-driver/lib/index.js:27`):

```
When you have your final answer, you MUST report it by calling the `structured_output` tool with arguments matching its parameter schema exactly. Do not finish with a plain text answer: only the tool call counts as your result.
```

Skill catalog message (`dsh-tool-skill/lib/index.js:238-260`):

```
<system-reminder>
A skill is a reusable set of task-specific instructions. The following skills are available in this session:

<available_skills>
- `<name>`: <description>
...
</available_skills>

If the user names a skill, or the task clearly matches a skill's description, call the `skill` tool with the exact skill name before taking task actions. Load all applicable skills, then follow their full instructions. This catalog contains summaries only; do not infer or follow a skill's instructions until it has been loaded.
A user may also invoke a skill directly; its <skill_content> block then appears in this conversation. Follow it, and do not call the `skill` tool again for that skill.
</system-reminder>
```

Model-switch notice (`dsh-agent/README.md:141`, user-role on route change):

```
[model changed: assistant turns above this point were generated by <previous>; the session continues with <next>]
```

#### 2.3.5 Workspace instructions (`AGENTS.md`)

`dsh-agent-instructions` "does **not** register a system-prompt section; it
appends a durable **user-role** message via `createUserMessage`"
(`/tmp/opencode/dsh-prompts.md:274-276`). Defaults
(`lib/index.js:17-28`):

```js
const DEFAULT_PROJECT_ROOT_MARKERS = [".git"];
const DEFAULT_INSTRUCTION_FILE_CANDIDATES = ["AGENTS.md", "CLAUDE.md"];
const DEFAULT_LOCAL_INSTRUCTION_FILE_CANDIDATES = ["AGENTS.local.md", "CLAUDE.local.md"];
const DEFAULT_MAX_SOURCE_BYTES = 1048576;
```

`maxBytes` is **required** by the plugin config; the `standard` preset supplies
`65536`. Precedence: user-global `$DSH_HOME/AGENTS.md` first, then every existing
candidate from project root down to cwd, broad-to-specific. Truncation is
**not** a binary search: "Rendering keeps the most specific files first: it
drops whole broader files before truncating the most-specific file, and emits a
visible `Workspace instruction budget ...` notice naming the omitted and
truncated paths. The rendered bytes never exceed `maxBytes`. An over-budget
broad file is ignored; during refresh it is treated as temporarily unavailable
rather than removed." (`dsh-agent-instructions/README.md:72`). Verbatim framing
(`lib/index.js:111-133`, via `/tmp/opencode/dsh-prompts.md:302-356`):

```
The following workspace instructions may be relevant to your work. Use them as guidance when applicable. More specific instructions take precedence over broader ones. They do not override system, developer, or direct user instructions.
```

```
Instructions from: AGENTS.md

<project content>
```

```
Additional instructions from: <displayPath>

These instructions apply to work under `<scope>`. Use them as guidance when relevant; more specific instructions take precedence. They do not override system, developer, or direct user instructions.

<file content>
```

Removal/change texts:

```js
if (change.action === "remove") return `Instructions removed: ${change.path}\n\nThe previously loaded instructions from this file no longer apply.`;
return [`Updated instructions from: ${change.path}`, "", "This file changed after it was loaded. Use the following content instead of the previously loaded instructions from this file.", "", file.content].join("\n");
```

Budget marker: `Workspace instruction budget <maxBytes> bytes: omitted <paths>; truncated <path> from <n> to <m> bytes`. Loads on first request; **not** watched; successful `read`/`write`/`edit` reaching a deeper directory discover nested files. The plugin's exact tie-breaking among multiple candidates in one directory (e.g. `AGENTS.md` vs `CLAUDE.md`) and its treatment of the `*.local.md` candidates are not re-asserted here; §4 pins ymh's own deterministic rule.

#### 2.3.6 Tool presentation

`dsh-agent-tool-presentation` is "per-preset selector, config `mode` required:
`native` | `ptc` | `both`" (`/tmp/opencode/dsh-prompts.md:369-379`). `native`
presents each visible tool schema as a function definition; `ptc` presents only
`run_code` plus a generated SDK; `both` presents both. `dsh-tools` owns the
registry (`defaultMode = "native"`, `maxParallelSubCalls` default `10`). Tool
ordering: configured `toolOrder`, else lexicographic by name after restrictions.
Many tools ship both a schema description and a separate prompt guidance
section; the section is suppressed when the tool is restricted away.

#### 2.3.7 KV-cache discipline

From `/tmp/opencode/dsh-prompts.md:729-761` (quoting the package READMEs):

- "Prefix-stable while identity, persona, variables, section text, and order
  render identically ... a head rewrite loses prefix reuse from its first
  changed token; when the prepared call declares `systemPromptUpdate:
  'in-history'`, the agent loop appends a non-empty changed prompt after the
  cached history inside a continuing request series, so the prefix through that
  history stays reusable."
- "Schema tokens repeat on every request. Restricting a tool removes its entire
  schema cost for that agent but not a separate prompt section; reordering
  changes cache shape but not semantic content."
- Plan mode "deliberately keeps the tool catalog stable" and changes only
  `plan:policy` from order 500 onward.
- Dynamic material (workspace instructions, discovered scopes, repeat reminders,
  skill catalogs) is append-only. Exact shipped wording
  (`dsh-agent-instructions/README.md:181`): "Append-only; newly visible content
  follows the reusable request prefix and does not invalidate existing KV-cache
  entries."
- PTC ordering rationale: `tools:ptc-only` at 800 must precede per-tool guidance
  (1000+), otherwise "the model reads a catalog of tools it is told to use and no
  statement that only `run_code` may be called."

### 2.4 Agent collaboration

#### 2.4.1 Presets and per-session composition

"A preset is one directory containing a composition file `agent.cordis.yml` plus
display metadata and optional skill directories/assets. Mounting the preset
composes one agent session's tools, prompt sections, and skills"
(`/tmp/opencode/dsh-collab.md:19-21`). "Every session names a preset —
explicitly or through the configured default — and is composed from it." "A
child agent (subagent) joins its parent's composition, so it sees the same tools
and prompt sections as the agent that spawned it." Presets come from the
package's bundled `presets/` and `<dshHome>/.agent-presets`.

The file is "a YAML **list of named plugin rows**" with fields `id`, `name`,
`group`, `isolate`, `config`, `disabled`, `inject`. A preset "selects, for one
session: tools ..., prompt sections ..., skills ..., and the persona. It does
**not** own the registries themselves." The `standard` preset header
(verbatim, `/tmp/opencode/dsh-collab.md:119-147`):

```yaml
# The `standard` agent preset: the full coding agent, mounted once per process.
#
# This file is an AGENT-PLANE composition. The roster mounts it ONCE under a
# standing scope; every session naming it joins by scope parentage, so the
# tools and prompt sections registered here cover each joined agent while a
# session's own state stays keyed per Session/Agent inside the plugins. The
# host composition (`base.cordis.yml` + `web.cordis.yml`) keeps everything a
# preset must not own: the registries themselves, the sandbox and approval
# stack, persistence, and the model route.
...
- id: persona
  name: '@deepseek-ai/dsh-persona'
  config:
    suffix: Your working directory is {{cwd}}.
    prefix: >-
      You are a coding agent powered by the {{model}} model.

- id: agent-instructions
  name: '@deepseek-ai/dsh-agent-instructions'
  config:
    maxBytes: 65536
```

The roster service (`dsh-agent-presets/lib/types/index.d.ts:60-383`):
`list`, `resolve(id?)`, `mount(agentCtx, id?)`, `composeFrom(agentCtx,
parentCtx)`, `composedPreset(agentCtx)`, `select(agent, preset)`,
`standingKeyFor(id?)`. The precise standing/per-session model is: "**One
standing composition per preset.** A preset is mounted once per process under a
standing scope; agents join by parenting their scope key to the mount, so the
mount's registrations and listeners cover every joined agent and no sibling
preset's." (`dsh-agent-presets/README.md:99`). The per-session behavior is a
property of the **composition's scope**, not a second mount: the `ptc` preset's
own header says it "is mounted under one agent's scope context, so every tool
and prompt section it registers belongs to that session alone"
(`presets/ptc/agent.cordis.yml:13-15`), and the `cordis` preset's skill states
"One instance per session, mounted under that session's scope and unwound with
it" (`presets/cordis/skills/editing-cordis-compositions/SKILL.md:22`). The
package description calls the whole mechanism "Per-session agent composition
from preset cordis.yml files" (`package.json:3`). In short: one standing mount
per preset; the scoped layers joined by each session make the effective
composition per-session.

Preset switching is restricted (`dsh-agent-presets/README.md:81`): "A
session can switch to a different preset only while it has produced nothing —
no messages or tool calls. After that, the composition is fixed for the
session's life, because swapping tools mid-conversation would leave logged tool
calls the new composition cannot make." A committed switch emits
`tools/change` and appends `agent-preset/selected` after the swap commits,
"because the preset decides the tool schemas and prompt sections the model
s[ees]." The creation header names the preset a session started with; the
`agentPreset` projection names the one it runs.

#### 2.4.2 Subagents join the parent composition

`dsh-subagent` is the contract; providers and the model-facing tool are separate
packages. `SubagentRuntime` (`dsh-subagent/lib/types/index.d.ts:97-296`) offers
`start`, `startContinuable`, `sendMessage`, `interrupt`, `drainContinuable*`,
`listChildren`, `listDescendants`, `prompt`, `registerProvider`, `getProvider`,
`list`. Providers: `spawn` — "runs each child as a fresh child Agent on the same
cordis context (its own session, own system prompt, zero parent context)"; `fork`
— "runs each child as a child Agent SEEDED with a prefix of the parent's session
log ... The seed ends at the last `turn/end`" (`/tmp/opencode/dsh-collab.md:1255-1263`).

The child composition is applied in one call (`dsh-subagent/lib/types/child-agent.d.ts:84-106`):

```ts
export interface ChildComposition {
    readonly persona?: string | undefined;
    readonly toolFilter?: ToolRestriction | undefined;
}
export declare function applyChildComposition(childCtx: Context, parent: Agent, composition: ChildComposition): void;
```

"Compose one child inside its creation window: join its parent's preset,
register the fixed delegation-scope statement, then apply the child's own
shadowing persona section and tool restriction ... The join and the per-child
registrations live in ONE call because a child composed without the join is
exactly the defect this function exists to prevent: with every model-facing row
on the agent plane, a child that joins no preset sees an empty tool registry and
none of its parent's prompt sections." The preset is read "from the parent's
LIVE scope chain rather than from its header, because a parent that switched
preset while blank runs on the newer composition and its header still names the
older one."

The delegation tool config (`dsh-tool-subagent/lib/types/index.d.ts`):
`provider`, `toolName?`, `modelSelectionSettings?`, `enableRunInBackground?`,
`backgroundMode?: 'one-shot' | 'continuable'`, `agentOptions?`, `persona?`,
`toolFilter?`, `maxDepth?` (default 3; 0 forbids delegation). A child inherits
route/model/effort, gets "a fixed sandbox scope and `approvalPolicy: 'never'`",
and receives the fixed runtime-context statement. The shipped runtime constant
(`dsh-subagent/lib/index.js:519`, authoritative) reads:

```markdown
You are a delegated subagent: your permission scope was fixed when you were started and cannot be widened from inside this session — operations that require approval are rejected automatically. When the task needs access beyond that scope, do not retry the denied operation; state the limitation in your reply so the delegating agent can handle it.
```

The `dsh-subagent/README.md:152` prints the same statement with "the job needs
access beyond that scope"; source and README diverge, and the source is
authoritative.

Fan-in: one-shot foreground "gets the child's final answer as the tool result";
one-shot background is a Job; continuable settlement is "a runtime-owned
user-role message in the parent" opening with "`Background subagent <child-id>
finished and will do no further work unless you send it more.`", followed by
"Its closing message:" and the child's final assistant content, or "It left no
closing message." Control tools: `send_message`, `interrupt_agent`,
`list_agents`. `send_message` "if the target is still working, the message steers
its nearest step; if it is idle, the message starts a turn."

#### 2.4.3 Goals

`dsh-goal` "lets one long-running completion objective persist across turns,
session resume, fork, and process restarts ... A configurable round cap (256 by
default) bounds automatic continuation, and blocked goals retain a stable policy
code with a human-readable explanation. The package stores goal state but does
not schedule work, and continuation permission remains process-local rather than
durable" (`/tmp/opencode/dsh-collab.md:1410-1417`). Types
(`dsh-goal/lib/types/types.d.ts`):

```ts
export interface GoalRef { readonly id: GoalId; readonly revision: number; }
export type GoalPhase = 'active' | 'paused' | 'blocked' | 'complete';
export interface GoalBlockReason { readonly code: string; readonly message: string; }
export interface GoalSnapshot extends GoalRef { readonly objective: string; readonly phase: GoalPhase; readonly blockedReason?: GoalBlockReason; readonly maxGoalRounds: number; }
export type GoalActivation = 'armed' | 'disarmed';
```

"Every accepted change is recorded durably in the session log — the only store of
goal state." "an active goal is disarmed after any session-start edge." The
round driver "queues one goal-round prompt" at idle with an active armed goal;
"only an entered goal message consumes the cap, while human messages and stale
reservations do not." `renderGoalRoundPrompt(goal, round): ContentBlock[]`.
"completion, pause, and blocking suppress continuation ... at the cap it records
a blocker with the stable code `round-limit`." Goal tools: `get_goal`,
`create_goal`, `update_goal`; `blockedAfterConsecutiveRounds` default 3.
"Creating, editing, pausing, or resuming requires that direct request in a
top-level agent turn."

#### 2.4.4 Background jobs

`dsh-jobs` is the contract; `dsh-jobs-local` the in-process implementation;
`dsh-tool-jobs` the model-facing controls (`job_output`, `job_list`,
`job_kill`). Types (`dsh-jobs/lib/types/types.d.ts`):

```ts
export type JobStatus = 'running' | 'stopping' | 'completed' | 'killed' | 'failed';
export interface JobStart { kind: JobKind; label: string; outputLimitBytes?: number; owner?: Agent; run(): JobHooks; }
export interface JobHooks { cancel(reason?: string): void; done: Promise<JobOutcome>; readOutput?(): string; }
```

"Each job receives a stable `<kind>-N` id ... Ownership is scoped to the agent
session, so other agents cannot inspect or stop the job; completion arrives as
an in-session notice without polling." "A producer can start work only while a
controller that serves the owner is attached." `dsh-tool-jobs` config:
`waitTimeoutMs` 30s, `maxWaitTimeoutMs` 10min, `completionDelivery` default
`wakeup`, `maxConsecutiveWakes` default 3 ("bounds the self-exciting chain where
a woken turn starts the job whose completion wakes it again").

#### 2.4.5 Commands

`dsh-commands` "lets users run `/command [input]` actions in interactive
Harness UIs without turning the command or its result into a model message ...
Every admitted run is recorded in the receiving agent's session log, while the UI
renders the settled result outside model history" (`/tmp/opencode/dsh-collab.md:1627-1632`).
`ctx.commands.register({name, description, input, handler})`; the handler returns
`success`/`error` plus optional UI text. `execute()` "mints a `commandId`,
appends `command/run` before the handler runs, and appends `command/done` at
settlement"; both are "direct standalone log-only appends: no turn wraps them."
Agent-scoped registrations shadow global ones for that agent. Shipped commands:
`/goal` (create/edit/pause/resume/block/clear), `/compact`, `/feedback` (fixed
`FEEDBACK_CATEGORIES`), `/plan` (bare → active; `/plan off` → inactive; other
argument → active plus the text steered as the next ordinary logged user
message).

#### 2.4.6 The "way of working" end-to-end

`/tmp/opencode/dsh-collab.md:1737-1776`:

1. Session creation names a preset (or default); composition mounted once; header
   + `agentPreset` projection record it; switch only while blank.
2. Human turn enters the agent loop; slash commands intercepted by the command
   registry, run directly, logged `command/run`/`command/done`.
3. Delegation: `subagent` (spawn) or `subagent_fork` (seeded); shipped presets
   default `backgroundMode: continuable`; child joins parent composition, stamped
   with depth, lineage, pinned policy.
4. Fan-in as §2.4.2.
5. Background work via `ctx.jobs.start()`; owner-scoped; completion delivered to
   a busy owner's next step or wakes an idle owner under `maxConsecutiveWakes`.
6. Long-horizon work via goals; round driver at idle.
7. Planning via `/plan`; read-only explore; `exit_plan_mode` opens review.
8. Workflow/ralph are opt-in and out of scope for ymh (see §5 exclusions).

Ownership/authority summary (`/tmp/opencode/dsh-collab.md:1778-1787`): parent
starts/disposes children and owns jobs; child has a fixed non-widenable scope
with approval pinned `'never'`; job registry fenced by owner session id
("authorization, not secrecy"); goal service is compare-and-set on
`{id, revision}` with process-local activation; commands are log-only.

---

## 3. Gap analysis

The table measures ymh today against §2. "Already aligned" rows are called out
because they are the foundation the migration builds on, not work to redo.
Evidence is `file:line` or spec section. Where a prior draft overstated a
"already aligned" claim, Rev 2 states the exact delta.

| # | Mechanic | dsh | ymh today | Delta |
|---|---|---|---|---|
| G1 | **Provider-neutral call service** | `LlmRuntime` (`ctx.llm`) is the only path to adapters; adapters register routes | `LLMProvider` is the seam and is used directly by the loop; no service layer above adapters (`include/ymh/llm/llm_provider.hpp:61`; `AgentServices::provider` `include/ymh/agent/agent_loop.hpp:63`) | **Missing service layer.** The loop holds a raw `LLMProvider*`. Add a `LlmRuntime`-equivalent that owns route→adapter registration, model resolution, and the stream boundary. |
| G2 | **Request deep-frozen before dispatch** | "Requests are deep-frozen before dispatch" (`dsh-llm/README.md:12`); `PreparedLlmCall.config` is "Detached, deep-frozen" | `LLMRequest` is a plain value passed by const-ref; nothing freezes it (`llm_request.hpp:54`) | **Missing freeze.** Add immutable/frozen request envelope with a pinned canonical serialization (see §4). |
| G3 | **Every request reconstructable from the log** | dsh logs the routed request header; `compaction/summary` logs the summarizer `provider`/`model` "so the one-shot request is reconstructable from log + code" | ymh logs `AssistantMessage`/`TokenUsage`/`ContextCompaction` (`events.hpp:103,167,159`) but **no request-header event**; grep of `events.hpp` for request/header finds none | **Missing logged header.** Without it a request cannot be reconstructed or compared. |
| G4 | **Call-config as logged header state** | `LlmCallConfig` + `callConfigEquals`; loop builds from the logged header, "logs changed snapshots instead of allowing silent per-call drift" (`call-config.d.ts:1-6`) | `GenerationParameters` is per-request, mutable, not logged or compared (`llm_request.hpp:40-48`); `AgentConfig` carries `model`/`parameters` (`agent.hpp:94-104`) | **Missing `LlmCallConfig` + equality + logged snapshots.** |
| G5 | **One provider attempt per stream; retry separate** | `LlmRuntime` "remains a single provider attempt"; `dsh-llm-retry` re-runs at durable step boundaries with `llm/retry`/`llm/retry-started` events | Retry lives **inside** the adapter; retryable only before first dispatched event (`llm_provider.hpp:39-47`, `stream.hpp:111`, `openai_adapter.cpp:983`) and is **not logged** | **Aligned in intent, missing durability.** ymh's "retry only before the first event" barrier matches dsh's one-attempt semantics, but there is no durable retry record and no separate executor. |
| G6 | **Streams always end in a terminal result** | Every stream ends in exactly one terminal `finish`; adapter throw normalized to `error`/`aborted` | `LLMResponse` is the authoritative **loop-level** terminal value with `outcome`/`finish`/`usage`/`tool_calls`/`error` (`llm_provider.hpp:51-59`); `StreamOutcome::{Completed,Cancelled,Failed}` (`stream.hpp:33`). On cancellation the provider may emit no terminal **sink** event; the loop normalizes it (`agent_loop.cpp:742-746`) | **Aligned at the loop boundary, not at the sink.** State the guarantee where it actually holds (loop `LLMResponse`), not as an adapter-sink invariant. |
| G7 | **Chunk protocol** | `StreamChunk` union incl. `block-start`/`block-end`/`reasoning-delta`/`tool-call-delta`/`usage`/`finish` | `StreamEvent` variant with `TextDelta`/`ReasoningDelta`/`ToolCallStarted`/`ToolCallDelta`/`ToolCallFinished`/`UsageEvent`/`Finished`/`StreamError` (`stream.hpp:169-176`) | **Mostly aligned.** ymh has no `block-start`/`block-end` (blocks are implicit); `ToolCallFinished` carries the assembled call rather than a `block-end`. Acceptable, but the assembler must be the single canonical one (see G10). |
| G8 | **Message/content model** | Immutable `Message`; tool result is a user-role message with one `tool-result` block; merge-extensible `ContentBlockMap`; `MessageSource` with semantic `ContextForm` | `Message{Role, content, tool_call_id}`; `ContentBlock{kind, text, tool_call_id, tool_name, arguments, media_type, data}` (`message.hpp:111-127`); no `source`/provenance; `ContentBlockKind` is a closed enum (`message.hpp:70`) | **Missing provenance + source model.** ymh cannot distinguish "instructions" vs "catalog" vs "snapshot" context, which §2.3/§2.4 rely on. |
| G9 | **Usage accounting** | Disjoint `TokenUsage`: `inputTokens` is **uncached input only**; cache read/write and reasoning are separate (`types.d.ts:128-150`) | `Usage{input,output,cached,reasoning}` exists (`message.hpp:131-138`), but `parse_usage` assigns `input_tokens = prompt_tokens` **without subtracting cache hits**, and reads `cached_tokens`/`reasoning_tokens` independently (`openai_adapter.cpp:100-114`). No arithmetic enforces any subset relation | **NOT disjoint.** `cached ⊆ input` and `reasoning ⊆ output` are only provider conventions here; dsh's `inputTokens` excludes cache. Add `totalTokens?`/cache-write and the subtraction at the adapter boundary. This is a real delta, not "minor". |
| G10 | **Incremental chunk→message assembler** | `BlockAssembler` single canonical algorithm; `interruptedBlocks()` drops tool calls; `finish` defaults to `stop` | `ToolCallAssembler` only assembles tool calls (`tool_call_assembler.hpp:24`); the loop coalesces text via `ChunkCoalescer` (`chunk_coalescer.hpp`) | **Partial.** Text/reasoning/tool blocks are not assembled by one canonical component; interrupted-stream handling is not a single policy. |
| G11 | **Compact assistant-stream + replay envelope** | `AssistantStreamAccumulator` packs timed delta runs; `ReplayEnvelope` stored on the assistant message source | ymh persists `AssistantChunk{message,index,text,kind}` events (`events.hpp:96-101`); no packed runs, no replay envelope, no adapter-private replay state (`ReplayEnvelope`/`AssistantStreamRecord`/`TimedStreamEvent` do not exist in the tree) | **Missing replay envelope + compaction of stream records.** |
| G12 | **Tool-call assembly** | Adapter yields raw JSON string deltas; assembler parses; `ToolCallBlock.arguments` stays a raw string | `ToolCallAssembler` parses fragments into a JSON object; `ToolCallAssembled.arguments` is an object (`stream.hpp:134-138`); protocol violations → `ProviderInternal` | **Already aligned**, with one design difference: dsh keeps the raw string on the block and parses later; ymh parses in the assembler. Keep ymh's, document the equivalence. |
| G13 | **Tool result feeding / ordering** | `executeToolCalls`: exclusive calls form barriers, parallel-safe calls use a bounded pool (`maxParallelToolCalls` default 10); "policy, results, and result context remain model-ordered"; abort records synthetic results for skipped calls | `AgentLoop::executeToolCall` (singular) executes one call at a time (`agent_loop.hpp:138`); tool results appended as `ToolResult` events (`events.hpp:127`) | **Missing bounded parallel scheduling.** ymh is serial. Also missing: synthetic error results for skipped calls on abort (replay validity). |
| G14 | **Stable tool catalog across modes** | Plan mode changes only `plan:policy`; `exit_plan_mode` stays registered; KV-cache prefix stable | No plan mode; tool set is whatever `ToolRegistry::schemas()` returns, frozen once (`tool_registry.hpp:81-82`, `context_assembler.cpp:52`); MCP tools can join (`15-mcp-adapter.md`) | **Missing mode/tool-catalog stability rule.** ymh has no mode concept, so the rule must be stated for any future mode and for MCP changes. |
| G15 | **Bounded output (deque/chunked-list/retention)** | `Deque`, `ChunkedList`, `ItemRetainer`/`TextRetainer` with exact omission metadata and `formatRetentionNotice` | Only `clamp_tool_result` truncates a serialized result to `max_bytes` (`tool.hpp:79-82`); no item/text retainer, no omission notice, no persistent chunked list | **Missing retention library.** `ToolResult.truncated` is a bool (`events.hpp:132`); dsh reports exact omitted counts. |
| G16 | **Replay-safe tool-result pruning** | `dsh-compaction-tool-result-pruner`: deterministic head/middle/tail (8192/4096/1024) **measured in Unicode code points**, replacement cites shadowed node, preceded by `compaction/prune` shadow-price | None. `clamp_tool_result` caps at execution time and is not a surface replacement | **Missing pruner.** |
| G17 | **Compaction as projection** | `CompactionEngine` with `compactIfNeeded('pressure'|'context-overflow')`/`compactNow`; log-only `compaction/*` events; shadowed range/seqs/token count; summary `provider`/`model` logged | `CompactionPolicy` (`compactor.hpp:30-58`), `ContextCompactor`, `ContextCompaction` event with boundary/summary/tokenEstimate/model/createdAt (`events.hpp:159-165`); "projection over the append-only session log, never a deletion" (`compactor.hpp:1-7`) | **Already aligned in principle.** Delta: ymh has no trigger taxonomy (`pressure` vs `context-overflow`), no shadowed-seq accounting, no separate summary event family, and no `compactNow` vs `compactIfNeeded` split. Adding payload fields is **breaking** against spec 13 (see §4.4). |
| G18 | **Ordered prompt sections + canonical order table** | `SECTION_ORDERS`/`CONTEXT_ORDERS`, `PromptSection{name,order,text,complete?}`, `PromptContext`, `PromptAssembly`, `system-prompt/assemble` waterfall, strict `{{var}}`, `complete` singleton, `toolOrder` + `TOOL_ORDER_REST` | ymh has **no section registry, order table, variables, or `complete`/`toolOrder` rule**. What it does have: `SessionContextAssembler` prepends one system message built from `AgentConfig::system_prompt` (`context_assembler.cpp:36-50`), and `make_agent_config` appends the skill catalog's `index_section()` to that string (`workspace_runtime.cpp:50-57`); the base prompt is config or a hardcoded fallback (`wiring.cpp:48-52`) | **Missing registry; not "one hardcoded string".** The structural gap (orders, registry, variables, strict interpolation, `complete`, `toolOrder`) is real and is the second-highest-leverage gap; the current prompt is config-driven and already has one dynamic append (skills), so the delta is the registry, not the existence of any assembly. |
| G19 | **Persona prefix/suffix** | `dsh-persona` order 0/10200, shadowable per agent/preset, `complete` mode | None; the default prompt is a hardcoded English fallback (`wiring.cpp:48-52`) but is overridable by config | **Missing.** |
| G20 | **Workspace instructions (AGENTS.md)** | Loader with `.git` root, candidates, required `maxBytes`, broad-to-specific, `<system-reminder>` framing, durable user-role message, change/removal notices | None (grep found no instructions loader; `20-skills.md` is about skills, not AGENTS.md) | **Missing entirely.** Note ymh's `AGENTS.md` exists as a repo convention but is not loaded into context. |
| G21 | **Runtime-context snapshots** | Dynamic contexts become sourced user-role snapshots under "Current runtime context. This snapshot supersedes earlier runtime-context snapshots."; separate from sections | `ContextInjected` event with `role`/`text` (`events.hpp:153-157`); `role` **defaults to `Role::System`** and there is no snapshot/supersession or provenance model | **Partial.** ymh can inject context but has no sourced snapshot/supersede model and its default role is the opposite of dsh's user-role snapshot. |
| G22 | **Tool presentation modes** | `native`/`ptc`/`both`; per-tool guidance sections at `TOOL_*` orders; `toolOrder` canonicalization | Tools serialized from `ToolSchema{name,version,description,input_schema,destructive}` (`tool.hpp:39-45`); no presentation modes, no guidance sections, no ordering rule | **Missing.** ymh has `input_schema` vs dsh `parameters` naming only. |
| G23 | **KV-cache discipline** | Stable prefix; dynamic material appended; `systemPromptUpdate: 'in-history'` | No concept. System prompt is message 0 (`context_assembler.cpp:47`); no in-history update | **Missing.** Follows from G18. |
| G24 | **Presets / per-session composition** | `agent.cordis.yml` rows select tools/sections/skills/persona; standing mount with per-session joined scopes; switch only while blank; `agentPreset` projection | `AgentConfig` is a flat struct (`agent.hpp:94-104`); `WorkspaceRuntime` builds one config (`workspace_runtime.hpp`); no preset/roster concept | **Missing.** |
| G25 | **Subagents join parent composition** | `applyChildComposition` in one call; child inherits route, fixed sandbox, `approvalPolicy: 'never'`; depth default 3; spawn vs fork | `SubagentSpawned`/`SubagentFanIn` events (`events.hpp:174-190`); `subagent.hpp`; spec `06`/`01` | **Partial.** ymh has the event vocabulary and a `subagent.hpp` seam but no composition inheritance, no depth, no fixed-scope statement, no continuable control tools. |
| G26 | **Goals** | Durable objective per session, phases, round cap 256, round driver, `/goal`, disarm across resume/fork | None | **Missing entirely.** |
| G27 | **Background jobs** | Owner-scoped registry, `job_output`/`job_list`/`job_kill`, completion wakeup/quiet, `maxConsecutiveWakes` | PTY capability (spec `14`) and shell tools run synchronously; no job registry | **Missing.** |
| G28 | **Commands are not model messages** | `ctx.commands`, log-only `command/run`/`command/done`, agent-scoped shadowing | `ui/command_registry.hpp` exists for UI commands (e.g. `/context`, spec `18`) but the semantics are UI-local, not a durable log-only command surface | **Partial.** ymh has a command registry; it is not the dsh contract (no `command/run`/`command/done` events, no agent-scoped shadowing). |
| G29 | **Repeat-tool / loop protection** | Repeat-tool reminder at thresholds `[3,5,8]`, plugin-sourced append-only; step/inbox steering | `AgentConfig::max_steps = 100` (`agent.hpp:97`) and `StepLimitExceeded` (`agent.hpp:68`); no repeat detection, no steering-specific reminder | **Partial.** ymh has a hard step cap; dsh has graduated in-session reminders plus steering. |
| G30 | **Permissions / sandbox** | Host-plane sandbox + approval stack; `approvalPolicy: 'never'` for children | The `SandboxMode` enum exists (`environment.hpp:22-26`) with a `ReadOnly` value commented "inspection only: no writes, no subprocesses, no PTYs". But production hardcodes `SandboxMode::Workspace` at the only two wiring sites (`wiring.cpp:171`, `workspace_runtime.cpp:101`; also `supervisor.cpp:1487` default), and `ReadOnly` is **unreachable from production**. It is enforced **only** at the policy layer, as a hard deny of a fixed tool-name list (`permission_policy.cpp:204-207` → `tool_is_mutating` `:160-173`); `LocalEnvironment::resolve()` (`environment.cpp:60-91`) special-cases only `Unrestricted` (`:68`) and does not confine writes/subprocesses/PTYs for `ReadOnly`. `PermissionPolicy`/`PermissionGate`/`PermissionRequest`/durable `PermissionDecision` do exist (`permission_policy.hpp:54,116,191`; `events.hpp:145`) | **Not aligned as claimed.** The policy stack is real and richer than a denylist in other respects (glob rules, grant scopes, fail-closed gate), but `ReadOnly` is dead code in production and the environment does not enforce it. Any adoption of dsh's sandbox must either wire a real read-only environment or delete the mode; do not list it as "already aligned". |
| G31 | **Event-sourced session log** | Append-only typed event log is the durable source of truth; all the above records live in it | `events.hpp` typed `SessionEventMap`; `deriveMessages()` (`session.cpp:362,551`); append-only log is the source of truth (`00 §9.1`) | **Already aligned — the keystone.** The migration is mostly "add event types + a projection", not "add a log". |
| G32 | **Skills** | Skill catalog as a durable user-role `<system-reminder>` message; `skill` tool loads full instructions | Spec `20-skills.md` (verified): `SkillCatalog` with `model_visible_`, `SkillTool` (`include/ymh/skills/skill_catalog.hpp:34-60`) | **Already aligned in spirit.** Delta: ymh's catalog rendering/framing should match the verbatim dsh text (§2.3.4) and be logged as a `catalog`-form context. |

### 3.1 What ymh already gets right (do not rebuild)

- **Event-sourced, append-only session log** with a typed `SessionEventMap`
  (`events.hpp`) and `deriveMessages()` (`session.cpp:362`). dsh's mechanics are
  expressed as *events plus projections*; ymh already has both.
- **Loop-level terminal result** (`LLMResponse`) and a **retry barrier** that
  only retries before the first dispatched event (`stream.hpp:111`,
  `openai_adapter.cpp:983`). This matches dsh's one-attempt-per-stream rule in
  miniature. (The terminal guarantee holds at the loop boundary; see G6.)
- **Separate cached and reasoning token fields** (`message.hpp:131-138`). These
  are recorded, but ymh's `input_tokens` is the provider's `prompt_tokens` and
  includes cache hits, so the counts are **not disjoint** as dsh's are (G9).
- **Permission stack**: `SandboxMode`, `tool_is_mutating`, `PermissionPolicy`,
  `PermissionGate`, durable `PermissionDecision` (`permission_policy.hpp`,
  `events.hpp:145`). Reuse the policy stack; note that `SandboxMode::ReadOnly`
  is not production-reachable and is not enforced by the environment (G30).
- **Compaction as projection, never deletion**, with a `ContextCompaction` event
  (`compactor.hpp:1-7`, `events.hpp:159`).
- **A `ContextAssembler` seam** (`context_assembler.hpp:30-36`) — the right place
  to grow sections/contexts. The current assembler already prepends a config-driven
  system message and appends the skill index (G18).
- **A `ToolRegistry` with `freeze()`** (`tool_registry.hpp:25-82`) and a pinned
  JSON-Schema subset (`tool.hpp:69-77`).
- **A skill catalog** (spec `20`) and a UI command registry
  (`ui/command_registry.hpp`).

### 3.2 The five highest-leverage gaps

1. **G3/G4 — no logged, frozen request header.** Without a
   `LlmCallConfig`-equivalent logged as a durable event and compared with
   `callConfigEquals`, ymh cannot reconstruct a dispatched request, cannot
   detect silent per-call drift, and cannot give replay a header to rebuild
   from. Everything in §2.1 rests on this.
2. **G18/G19/G20/G21 — no prompt registry, persona, AGENTS.md, or runtime
   contexts.** ymh has a config-driven base prompt plus a skill-index append, but
   no section registry, order table, variables, persona slots, or sourced
   snapshots. This is the largest behavioural delta and the one that most changes
   model behaviour.
3. **G2/G10/G11 — no frozen request, no canonical block assembler, no compact
   assistant-stream/replay envelope.** ymh streams and persists chunks but does
   not have dsh's replay-fidelity model.
4. **G13/G15/G16 — no bounded-parallel tool scheduling, retention, or
   replay-safe pruning.** ymh is serial and caps results with a blunt byte
   clamp; dsh schedules a bounded pool, reports exact omissions, and prunes the
   surface with shadow-price accounting.
5. **G24–G28 — no collaboration layer** (presets, composition inheritance,
   goals, jobs, log-only commands). This is a program of its own and is
   explicitly phased last (§5).

---

## 7. Revision log

- **Rev 1 (2026-09-18).** Initial write.

- **Rev 2 (2026-09-18).** Repair pass after the independent gate
  (`/tmp/opencode/gate26.md`; HIGH = 13, MEDIUM = 27, LOW = 16). Every finding
  was re-verified against the packages and the ymh tree; where the gate's own
  premise did not survive re-verification, the corrected fact is recorded and
  noted below.
  - **Fidelity (F-H1, F-M1–F-M5, F-L1–F-L4).** Fixed the §2.3.2 citation to
    `dsh-system-prompt/lib/index.js:10-42`/`:43-47` (not `:38-75`); replaced the
    truncated plan-mode quote with all six paragraphs
    (`cordis.patch.yml:304-315`); replaced the `<overCapGuidance>`/`<blockedAfter>`
    placeholder tokens with their resolved texts and named substitutions;
    deleted the invented "(binary-search truncation)" and quoted the real
    `README.md:72` wording; replaced the blanket "ptc and cordis are per-session"
    with the sourced standing-mount/per-session-scope model; rewrote the retry
    payload as the `mode`-discriminated union with required `maxRetries` only on
    `normal`; corrected the KV-cache quote; corrected `LlmRuntime` to a concrete
    class; fixed citation drift (`dsh-agent/README.md:141`,
    `dsh-agent-presets/README.md:81`, `cordis.patch.yml:304-315`, ptc persona
    `:31-36`); recorded the `dsh-subagent` "task" (source) vs "job" (README)
    divergence; added the `lib/index.js` prompt-prose-only source exception.
  - **Gap analysis (G-H1, G-H2, G-M1, G-L1).** Downgraded G9: ymh's usage is
    **not** disjoint (`input_tokens == prompt_tokens`, cache not subtracted);
    downgraded G30: `ReadOnly` is unreachable in production and enforced only by
    a policy-layer tool-name denylist, not by `resolve()`; corrected G18's "one
    hardcoded string" to the actual config-driven prompt plus skill-index append;
    restated the terminal guarantee as a loop-boundary property (G6/§3.1).
  - **Target design (T-H1–T-H6, T-M1–T-M14, T-L1–T-L8).** Part 2 pins every
    previously undefined type, removes the persisted `RequestId` (spec 08 says
    "Never persisted"), adds the serialization/reconstruction contract, defines
    `ReplayEnvelope`/`AssistantStreamRecord`/`TimedStreamEvent` and their stream
    source, adds the prompt `assemble()` output path, gives `26-D14` a wave,
    fixes `PreparedCall` constness, `AssembleContext` usage, handle lifetimes,
    `render_prompt` enforcement placement, the `LlmCallConfig`/`GenerationParameters`
    dual source, the `llm/stream` interceptor registration, retention/pruner
    units, the `ContextCompactor` routing, the new-event wire codec table, the
    migration plan, and the test strategy.
  - **Classification/waves (B-H1–B-L2, W-H1–W-L1).** Reclassified `D11`, `D13`
    and `D14` as breaking; reconciled the register with §4.4; expanded Wave 0 to
    gate `07`/`13`/`17`/`23`/`29`/`30`; narrowed `D15` to `Native` and moved the
    PTC SDK generator to an explicit deferral, resolving the §5.1 contradiction;
    assigned `D14` to Wave 3; added the test strategy and migration sections.

- **Rev 3 (2026-09-18).** Repair pass after the re-gate
  (`/tmp/opencode/regate26.md`; remaining open HIGH = 1, MEDIUM = 6, LOW = 5).
  Every fix was re-verified against the packages and the ymh tree; no re-gate
  premise failed re-verification, so every remaining finding is addressed
  rather than disputed.
  - **N-H1 (HIGH).** Restated `26-I2` as "reconstructable from the log plus the
    running registries": the header carries the full rendered system prompt and
    tool *names* + per-tool *digests*, and the schema bodies are re-derived from
    the live `ToolRegistry` and verified against the digests. This matches dsh's
    own "reconstructable from log + code" (`dsh-compaction/types.d.ts:49-51`);
    deleted the "not one-way hashes" sentence and the false "schemas recoverable
    without a provider call" claim (§4.3.2).
  - **N-M1 (MEDIUM).** Reshaped `AssistantStreamRecord` to dsh's discriminated
    union (packed delta runs + raw non-delta `ChunkRecord`; no second raw list),
    embedded the compact stream in `assistant/message`, added
    `assistant/attempt` for non-message attempts, and made `AssistantChunk`
    legacy read-only — removing Rev 2's third overlapping representation.
  - **N-M2 (MEDIUM).** Defined `MessageSource`, `ContextForm` (all six dsh
    values + `None`), `ContextSnapshotSection`/`ContextFormed`, and the
    command-handler `CommandId`/`CommandInput`/`CommandOutcome` types; corrected
    the "unpinned types closed" claim.
  - **N-M3 (MEDIUM).** Pinned payload structs for all ten new events
    (`LlmRetry`, `LlmRetryStarted`, `AssistantAttempt`, `ContextPrune`,
    `AgentPresetSelected`, `CommandRun`, `CommandDone`, `GoalChanged`,
    `JobChanged`) and stated that ymh's slash/underscore wire names deliberately
    diverge from dsh's hyphenated names.
  - **N-M4 (MEDIUM).** Corrected the `LocalEnvironment::resolve()` citation in
    G30 to `environment.cpp:60-91` (`Unrestricted` at `:68`); Rev 2 had
    regressed it to `:99-101`, which is past the 93-line file.
  - **N-M5 (MEDIUM).** Added the schema 1→2 DDL delta (none: new events are rows
    and header fields ride `sessions.metadata`), the cross-wave versioning
    policy (one structural bump; unknown event types fail loud at
    `parse_event_type`), and the read-only `version <= kSchemaVersion`
    requirement for `/sessions`.
  - **N-M6 (MEDIUM).** Pinned adapter lifetime: `register_adapter` takes a
    `shared_ptr`, dispatches hold it for their lifetime, and handle destruction
    unregisters without freeing an in-flight adapter; replaced the scheduler's
    open questions with a mandatory (a)/(b)/(c) service classification owned by
    `06`/`07`/`09`/`13`.
  - **LOWs.** Added `00` to the Wave 0 list (N-L1); added the wire-affecting
    `top_p`/`seed`/`tool_choice` to `LlmCallConfig`/`canonical_json()` (N-L2);
    removed the unknowable `header_seq` and pinned retry correlation to
    `retryId` + `(turn, step)` (N-L3); completed the `Deque`/`ChunkedList`
    declarations, renamed prose `build_request` to the shipped `buildRequest`,
    and pinned `CallPurpose` values (N-L4); aligned the instructions loader to
    dsh's load-every-present-candidate rule, deleting the unsourced
    first-match-wins rule (N-L5).
  - **Consciously not changed.** `B-L1` (D10 still classified `Add.`) is left as
    the re-gate deemed it acceptable: `06` is gated in Wave 0.

