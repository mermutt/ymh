# 39 — Session Prompt-Text Persistence Errata: The `session.persist_prompt_text` Key (spec-21 amendment)

```
Status: written · verified: — · reviewer: — (DESIGN_STATUS.md row 39)
Revision: Rev 1 — initial authoring. Pins the shipped config key
          `session.persist_prompt_text`, which no spec owned. Closes the Wave-1
          design-first debt recorded by `28 §13.5` item 5 and `30 §6 P2`, and the
          gap flagged by the mechanical catalog test
          (`tests/unit/spec_catalog_test.cpp` §6; the
          `config::session.persist_prompt_text` entry in
          `tests/fixtures/spec_symbol_catalog.json`).
Component: 39 (errata) — amends `21-config-jsonc-errata.md` §3.3, §7.1, §9, §10,
           and §12 **by reference**; it does not edit `21` in place. It consumes
           `26-D23` / `26-I11` (`26-dsh-alignment-part2.md` §4.9) and reconciles
           with `30-architecture-cascade-errata.md` §6 P2,
           `36-prompt-registry.md` §4.1-§4.4, and
           `34-assembler-replay-errata.md` §9.4.
Depends on: `21-config-jsonc-errata.md` (Rev 8) §3.3 :372-491, §7.1 :1128-1149,
            §7.6 :1233-1240, §9 :1624-1652, §10 :1653-1678, §12 :1695-1736;
            `30-architecture-cascade-errata.md` (verified) §5.3 :315-328,
            §6 P2 :358-380;
            `36-prompt-registry.md` (verified Rev 1) §4.1 :725-753, §4.2 :754-781,
            §4.3 :782-789, §4.4 :790-804;
            `28-llm-service-boundary-errata.md` (verified) §13.5 item 5 :955-956;
            `26-dsh-alignment-part2.md` (verified Rev 7) 26-D2 :140, 26-D23 :161,
            26-I11 :91-110, §4.3.9.1 :948, §4.3.2 :364-383;
            `34-assembler-replay-errata.md` (Rev 2) §9.4 :809-817, §9.5 item 7
            :831-832;
            the working tree at authoring time: `include/ymh/config/config.hpp`
            :158-163, `src/config/config.cpp` :366-370 and :755-827,
            `src/cli/wiring.cpp` :174-191, `include/ymh/agent/agent.hpp` :110,
            `src/agent/agent_loop.cpp` :535-538, `tests/unit/config_test.cpp`
            :645-674, `tests/unit/llm_runtime_test.cpp` :264-287 and :346-370,
            `tests/unit/spec_catalog_test.cpp` :536-560.
Amends:     `21` §3.3 (adds the missing `[session]` row; its key table predates
            the section), §7.1 (adds `"session"` to the top-level object), §7.12
            (new: the `[session]` schema), §9 (adds `39-I*`), §10 (adds `39-F*`),
            §12 (adds `39-T*`). `21`'s in-place text is not rewritten.
Retained:   `21`'s base contract in full: JSONC-only, strict unknown-key
            rejection, the defaults → global → workspace → env → CLI layer order,
            `ConfigError` on a wrong type, and no hot reload. This errata adds one
            section and one key; it supersedes nothing in `21`.
```

This document is an **errata**: it pins a shipped surface that the verified
config spec `21` does not own. It is **additive**. It does not rewrite
`21-config-jsonc-errata.md`; each clause it adds is stated here and cross-
referenced. The by-reference convention matches `35-live-notification-errata.md`
(the `event.live` amendment of `05`).

**Naming note.** Invariants local to this spec are **`39-I1`–`39-I10`**; failure
modes are **`39-F1`–`39-F9`**; decisions are **`39-D1`–`39-D6`**. The prefix `J`
belongs to `21`, `O` to `16`, `U-RB*` to `17`, and `35-I`/`35-F` to `35`. `39` is
unused by every spec in `docs/design/`.

## 1. Purpose, numbering, scope

### 1.1 The gap this closes

The config key `session.persist_prompt_text` ships and is read, but no spec
section owned it. The config errata `21` is the natural owner, yet its top-level
allowlist and its §3.3 key table predate the `[session]` section, so neither lists
the key (`21` §7.1 :1130-1141 enumerates `ui`, `agent`, `workspace`,
`permissions`, `logging`, `llm`, `mcp`, and `skills`; `session` is absent). The
prompt-registry spec `36` declines ownership explicitly: "This spec does not touch
`session.persist_prompt_text`; it only changes what the digest is computed over"
(`36` §4.3 :782-789).

The result was the third silent design-gate failure: a spec-bearing code symbol
shipped without a normative home. The mechanical catalog test
(`tests/unit/spec_catalog_test.cpp` §6 :536-560) now flags exactly this, and the
fixture records it as `gap: true` with a reason (the pre-fix state; the marker is
now a normal ownership pointer to this errata). This errata closes that gap: the
key gains an owner, and the catalog entry becomes a normal ownership pointer.

The debt is on record in two places. `28-llm-service-boundary-errata.md` §13.5
item 5 :955-956 states "`session.persist_prompt_text` is 21-owned. Pinned in `26`
§4.9 :1326; this errata consumes it." `30-architecture-cascade-errata.md` §5.3
:319 lists "`21` errata — the new `session.persist_prompt_text` key (D23)" as a
Wave-1 prerequisite. This spec is that errata.

### 1.2 What this pins

1. The key's **location, type, and default** (§3.1).
2. The **global-layer-only rule** and its `ConfigError` on a workspace occurrence
   (§3.2).
3. **What the key governs** — the session-DB copy only (§3.3).
4. Its **deliberate distinction from `logging.log_prompts`** (§3.4).
5. The **absent-vs-present behavior** — digest-only default vs full-text opt-in
   (§3.5), consistent with `30` §6 P2 and `36`'s digest/replay contract.
6. Its **interaction with the digest/replay contract** (§3.6).
7. The **invariants** (§5), **failure modes** (§6), **dependency list** (§8),
   **decision register** (§9), and **test plan** (§10).

### 1.3 In scope / out of scope

**In scope.** The `session.persist_prompt_text` config key and the durable
behavior it gates.

**Out of scope (named, not dropped).**

- **The `LlmRequestHeader` payload shape** — owned by `26` §4.3.9.1 :948 and
  `28`; cited, not restated.
- **The digest algorithm and the reconstruction contract** — owned by `28` §4.1-
  §4.4 and `34` §9; cited, not restated.
- **The spdlog surface** — owned by `21` §7.6 :1233-1240 and the redaction module;
  cited, not restated.
- **Any future `[session]` key** — this errata pins only the one shipped key.

## 2. Verified tree facts (the basis for §3)

Every `file:line` below was checked against the working tree at authoring time.

### 2.1 The shipped config surface

- `include/ymh/config/config.hpp:158-163`:

  ```cpp
  // [session] — durable session-DB options (26-D23, 28 §5.4). Global layer only.
  struct SessionSettings {
      // Store the full rendered system prompt in `llm/request_header` instead of
      // only its digest. Off by default and distinct from `logging.log_prompts`.
      bool persist_prompt_text = false;
  };
  ```

- `src/config/config.cpp:366-370` (`apply_session`):

  ```cpp
  void apply_session(Config& config, const Json& table, const std::filesystem::path& source) {
      reject_unknown(table, "session", {"persist_prompt_text"}, source);
      config.session.persist_prompt_text = read_bool(
          table, "persist_prompt_text", "session", config.session.persist_prompt_text, source);
  }
  ```

- `src/config/config.cpp:757-760`: the top-level allowlist passed to
  `reject_unknown(table, "", …)` includes `"session"`.
- `src/config/config.cpp:812-817`: the `[session]` section is gated on
  `global_layer`; a workspace occurrence calls
  `fail(source, "'session' is global-layer only")` before `apply_session` runs.

### 2.2 The shipped consumption path

- `src/cli/wiring.cpp:180`: `agent.persist_prompt_text =
  config.session.persist_prompt_text;` (inside `to_agent_config`).
- `include/ymh/agent/agent.hpp:110`: `bool persist_prompt_text = false;` on
  `AgentConfig`.
- `src/agent/agent_loop.cpp:535-538`:

  ```cpp
  header.system_prompt_digest = prompt_digest;
  if (config_.persist_prompt_text) {
      header.system_prompt = system_text;
  }
  ```

  The digest is written unconditionally; the full text is written only under the
  flag.

### 2.3 The by-reference relationship to 21

`21` §3.3 :372-491 is titled "Every config key read today" and is a historical
baseline captured at the RB-09 format switch. It is not the owner of keys added
after RB-09. This errata supplies the missing `[session]` row and the missing
top-level entry by reference rather than editing `21` in place, so the verified
`21` text stays byte-identical.

## 3. The pinned contract (frozen)

### 3.1 Location, type, default (39-D1)

| Property | Value | Evidence |
|---|---|---|
| JSONC location | top-level `"session"` object, key `"persist_prompt_text"` | `config.cpp:812-817`, `:757-760` |
| C++ member | `ymh::SessionSettings::persist_prompt_text` | `config.hpp:158-163` |
| Type | `bool` | `read_bool`, `config.cpp:368-369` |
| Default | **`false`** | `config.hpp:162`; `30` §6 P2 :364 |
| Layer | **global only** | `config.cpp:813-814` |

- An **unknown key** under `[session]` is a `ConfigError` (`reject_unknown`,
  parity with `21` J-F2).
- A **non-bool value** is a `ConfigError` (`read_bool`, parity with `21` J-F3).
- An **absent key** leaves the built-in default `false` in force.

### 3.2 Global-layer-only rule (39-D2)

A `[session]` section in the **workspace** layer is a `ConfigError` at load,
regardless of which key it carries. The rule is enforced at the **section**, not
per key (`config.cpp:812-817`). A repo-controlled `<workspace>/.ymh/config.jsonc`
therefore cannot silently enable durable prompt capture.

The thrown message is `config <path>: 'session' is global-layer only` (the
`ConfigError` prefix is added by `fail`, `config.cpp:43`). The run aborts with
exit 2 through the standard `ConfigError` path (`21` J-F5 handling).

### 3.3 What the key governs: the session-DB copy only (39-D3)

The key controls exactly one thing: whether the full rendered system-prompt text
is written into the durable `llm/request_header` event
(`payload::LlmRequestHeader::system_prompt`, `26` §4.3.9.1 :948) in the session
DB. It changes **no other persisted field, no wire field, and no runtime
behavior**. In particular:

- The `system_prompt_digest` is always written, in both states.
- The `template_digest` is always computed over the in-memory rendered prompt,
  in both states.
- No tool schema, message, config snapshot, or envelope field is affected.

This is the `26-D23` / `26-I11` policy (`26` §4.3.2 :364-383).

### 3.4 Distinct from `logging.log_prompts` (39-D4)

The two keys are deliberately separate and share no code path:

| | `session.persist_prompt_text` | `logging.log_prompts` |
|---|---|---|
| Governs | the session DB (durable event log) | the spdlog/log surface |
| Default | `false` | `false` |
| Layer | global only | global or workspace |
| When on | stores the rendered text byte-for-byte, **unredacted** | logs prompt bodies, still **redacted** |
| Evidence | `config.hpp:158-163`, `config.cpp:366-370` | `config.hpp:87-92`, `config.cpp:359-364` |

`logging.log_prompts` is documented as "off by default and must never be enabled
implicitly; prompt bodies are redacted even when it is on"
(`config.hpp:87-88`; `21` §7.6 :1233-1240). `session.persist_prompt_text` has its
own name and its own documented semantics: store the rendered text byte-for-byte,
with no redaction. Enabling one never enables the other. This separation is what
keeps the config comment's "redacted even when it is on" promise true.

### 3.5 Absent vs present: digest-only default vs full-text opt-in (39-D5)

| State | `system_prompt_digest` | `system_prompt` (full text) | Replay behavior |
|---|---|---|---|
| **Absent / `false`** (shipped default) | always written (SHA-256 hex) | **omitted** | re-derive the prompt from the live registries and verify `sha256(rendered) == digest`; if the rendered prompt is unknowable, `ReplayStatus::PromptUnavailable`; **never fabricate** |
| **`true`** (explicit opt-in) | always written | **written**, byte-for-byte | assert the stored text equals the in-memory rendered prompt **and** still verify the digest |

This is exactly the `30` §6 P2 policy (:358-380): "The `llm/request_header` event
**always** carries `system_prompt_digest`. The full rendered `system_prompt`
**text** is stored only under the dedicated `session.persist_prompt_text` key
(bool, default `false`, new `[session]` section, global layer only)." It is
consistent with `36`'s digest/replay contract (`36` §4.1 :725-753, §4.2 :754-781,
§4.4 :790-804) and with the replay harness's prompt-known/prompt-unavailable split
(`34` §9.4 :809-817).

The default durable record is therefore the digest, and the mechanism is safe by
default.

### 3.6 Interaction with the digest/replay contract (39-D6)

Because the digest is computed over the in-memory rendered prompt in both states,
`system_prompt_digest` and `template_digest` are **invariant** under this key. The
key affects only durable text presence, never the digests. That is precisely what
makes the default safe: a default install persists only the digest and honours the
no-prompt-logging policy, while a caller who enables the opt-in gets a
byte-identical copy that replay can additionally cross-check.

The user decision is recorded and unchanged: **keep the default `false`**; enable
the key only per test run when a baseline needs to be diffed (`30` §6 P2
:377-380; `36` §4.3 :782-789). The shipped default does not change.

### 3.7 JSONC schema snippet

```jsonc
{
  "session": {
    // Session-DB only (not the log surface); global layer only; default false.
    "persist_prompt_text": false
  }
}
```

## 4. Required `21` amendments (by reference)

`21` is not edited in place. These are the additive rows this errata pins; when
`21` is next revised they fold in verbatim.

| `21` clause | Current | Amendment |
|---|---|---|
| §3.3 :372-491 | key table has no `session` row | add `| session | persist_prompt_text | bool | false | config.cpp:366-370 |` |
| §7.1 :1130-1141 | top-level object lists eight sections | add `"session": { /* §7.12 */ }` |
| §7.12 (new) | absent | the `[session]` schema (§3.7) plus the global-only rule (§3.2) |
| §9 :1624-1652 | `J1`-`J22` | add `39-I1`-`39-I10` |
| §10 :1653-1678 | `J-F1`-`J-F16` | add `39-F1`-`39-F9` |
| §12 :1695-1736 | `J-T*` | add `39-T1`-`39-T9` |

## 5. Invariants

| ID | Invariant | Where |
|---|---|---|
| **39-I1** | **Default-off.** `SessionSettings::persist_prompt_text` defaults to `false`; an absent key yields `false`. | §3.1, §3.5 |
| **39-I2** | **Global-only.** A `[session]` section in the workspace layer is a `ConfigError`; no repo-controlled file can enable the key. | §3.2, `config.cpp:812-817` |
| **39-I3** | **Session-DB-only scope.** The key governs only whether the full rendered prompt text is written into `llm/request_header`; it changes no other persisted field, no wire field, and no runtime behavior. | §3.3 |
| **39-I4** | **Distinct from `logging.log_prompts`.** The two keys share no code path; `log_prompts` governs spdlog only and never the session DB. | §3.4 |
| **39-I5** | **Digest always present.** `system_prompt_digest` is always written; `system_prompt` is written iff the key is `true`. | §3.5, `agent_loop.cpp:535-538` |
| **39-I6** | **Byte-for-byte, unredacted when on.** When the key is `true`, the stored text equals the in-memory rendered prompt exactly; no redaction is applied. | §3.4, §3.5 |
| **39-I7** | **Replay never fabricates.** Absent the text, replay re-derives the prompt and verifies the digest, failing loud on mismatch and reporting `PromptUnavailable` when the text is unknowable. | §3.5, `34` §9.4 |
| **39-I8** | **No hot reload.** The value is read at config load; it cannot change mid-session. | §3.1, `21` §1.3.3 :235, §11 :1685; `00` §53 :4725 |
| **39-I9** | **Mapping fidelity.** `AgentConfig::persist_prompt_text` is assigned from `Config::session.persist_prompt_text` only; no other source sets it. | `wiring.cpp:180`, `agent.hpp:110` |
| **39-I10** | **Owner of record.** This spec is the normative owner of `config::session.persist_prompt_text`; the catalog entry points here, not at a gap marker. | §1.1, §10 |

## 6. Failure modes

Convention per `00-architecture.md` §54 (F1–F12 reference). Each mode names
detection and handling.

| ID | Failure | Detection | Handling |
|---|---|---|---|
| **39-F1** | A workspace-layer `[session]` section is written (an attempt to enable durable capture from a repo-controlled file) | `apply_document` checks `global_layer` before `apply_session` (`config.cpp:812-817`) | `ConfigError`: `config <path>: 'session' is global-layer only`; run aborts (exit 2). (39-I2) |
| **39-F2** | The key has a non-bool value | `read_bool` type check (`config.cpp:368-369`) | `ConfigError`; run aborts. Parity with `21` J-F3. |
| **39-F3** | An unknown key appears under `[session]` (typo) | `reject_unknown` allowlist `{"persist_prompt_text"}` (`config.cpp:367`) | `ConfigError` naming the qualified key; run aborts. Parity with `21` J-F2. |
| **39-F4** | The key is `true` but the full text is not written (regression) | replay finds no `system_prompt` while the header should carry it | digest re-derivation still runs; a mismatch is `PromptDigestMismatch`, an unknowable prompt is `PromptUnavailable`. Guard: 39-I5, test 39-T3. |
| **39-F5** | The stored text is not byte-identical to the in-memory rendered prompt | replay's text-equality assertion when the text is known (`34` §9.4) | `ReplayMismatch`; guard: 39-I6, test 39-T3. |
| **39-F6** | A caller conflates the key with `logging.log_prompts` (expects log redaction, or expects the session DB to be redacted) | review / test 39-T6 | neither key implies the other; the session-DB copy is unredacted when on, the log copy is redacted when on. Guard: 39-I4. |
| **39-F7** | A workspace file silently enables durable capture | same gate as 39-F1 | blocked by the global-only rule; guard: 39-I2, test 39-T2. |
| **39-F8** | The text is persisted while the key is `false` (safe-default violation) | replay/code inspection | violation of 39-I5; guard: test 39-T4. |
| **39-F9** | The key is expected to change mid-session | `load_config` runs once; no hot reload (`21` §1.3.3 :235, §11 :1685; `00` §53 :4725) | not supported; a change takes effect on the next process start. Guard: 39-I8. |

## 7. dsh mapping

| dsh concept | ymh realization | Note |
|---|---|---|
| session-scoped durable prompt policy | `session.persist_prompt_text` (`[session]`, global-only) | new section, additive to `21` |
| digest-only default | `LlmRequestHeader::system_prompt_digest` always written | `26-I11` |
| explicit full-text opt-in | `LlmRequestHeader::system_prompt` iff the key is `true` | `26-D23` |
| log surface kept separate | `logging.log_prompts` | never the session DB |

## 8. Dependencies

- **`21-config-jsonc-errata.md`** (Rev 8) — the amended spec. §3.3, §7.1, §7.6,
  §9, §10, §12 cited. `21` is not edited in place.
- **`30-architecture-cascade-errata.md`** (verified) — §5.3 :319 names this errata
  as a Wave-1 prerequisite; §6 P2 :358-380 records the user decision this spec
  consumes.
- **`26-dsh-alignment-part2.md`** (verified Rev 7) — 26-D2 :140, 26-D23 :161,
  26-I11 :91-110, §4.3.9.1 :948, §4.3.2 :364-383. The policy origin.
- **`28-llm-service-boundary-errata.md`** (verified) — §13.5 item 5 :955-956
  records the ownership.
- **`36-prompt-registry.md`** (verified Rev 1) — §4.1-§4.4; declines ownership and
  pins the digest/replay contract.
- **`34-assembler-replay-errata.md`** (Rev 2) — §9.4 :809-817, §9.5 item 7
  :831-832; the replay prompt-known/prompt-unavailable split.
- **The working tree** — the `file:line` list in the header block.

## 9. Decision register

| ID | Decision | Rationale | Status |
|---|---|---|---|
| **39-D1** | The key lives in a top-level `[session]` object, is a `bool`, and defaults to `false`. | Matches the shipped code and `26-D23`. | pinned |
| **39-D2** | The key is **global-layer only**; a workspace occurrence is a `ConfigError`. | A repo-controlled file must not silently enable durable prompt capture. | pinned |
| **39-D3** | The key governs the **session-DB copy only**, not the log surface and not any other persisted or wire field. | `26-D23` / `26-I11`; keeps the change minimal and auditable. | pinned |
| **39-D4** | The key stays **distinct from `logging.log_prompts`**, with no shared code path and no implication either way. | Keeps the log-redaction promise intact and the two surfaces independently controllable. | pinned |
| **39-D5** | The default is **digest-only**; the full text is an explicit opt-in. | Safe by default; `30` §6 P2; user decision 2026-09-19 to keep `false`. | pinned |
| **39-D6** | This errata owns the key **by reference**; `21` is not edited in place, and the catalog points here. | Preserves the verified `21` text while giving the key a normative home. | pinned |

## 10. Test plan

Existing tests already cover most of the contract; the additions close the
ownership and cross-key gaps. All run in the hermetic suite (no live LLM).

| ID | Test | Concrete assertion | Status |
|---|---|---|---|
| **39-T1** | `Config.SessionPersistPromptTextDefaultsOffAndParsesGlobally` | defaults `false`; a global `{ "session": { "persist_prompt_text": true } }` loads `true`. | existing (`config_test.cpp:645-657`) |
| **39-T2** | `Config.SessionSectionRejectedInWorkspaceLayer` | a workspace `[session]` throws `ConfigError` whose message contains `global-layer only`. | existing (`config_test.cpp:659-674`) |
| **39-T3** | `AgentLoopLlmHeader.PersistPromptTextStoresAssembledSystemPrompt` | with the key on, `header.system_prompt` is present, equals the assembled text, and its digest matches. | existing (`llm_runtime_test.cpp:346-370`) |
| **39-T4** | `AgentLoopLlmHeader.FreshSessionLogsExactlyOneHeader` | with the default off, `header.system_prompt` is `std::nullopt` and the digest is 64 hex chars. | existing (`llm_runtime_test.cpp:264-287`) |
| **39-T5** | `SpecCatalog.HistoricalDefectsRemainOwned` | `config::session.persist_prompt_text` is **owned** by `39-session-persist-prompt-text-errata.md` §3.1, not marked as a gap. | updated by this errata |
| **39-T6** | `Config.LogPromptsDoesNotAffectSessionPersist` (new) | set `logging.log_prompts` only; assert `session.persist_prompt_text` is `false` and no `system_prompt` is stored. | proposed |
| **39-T7** | `Config.SessionPersistPromptTextWrongTypeRejected` (new) | `{ "session": { "persist_prompt_text": "yes" } }` throws `ConfigError`. | proposed |
| **39-T8** | `Config.SessionUnknownKeyRejected` (new) | `{ "session": { "persist_prompt": true } }` throws `ConfigError`. | proposed |
| **39-T9** | `AgentLoopLlmHeader.PersistPromptTextOffOmitsTextEvenWhenLogPromptsOn` (new) | with `log_prompts` on and the session key off, the durable header omits `system_prompt`. | proposed |

## 11. Open items

1. **Tracker registration.** `DESIGN_STATUS.md` should gain a row for spec 39 and
   record that `21` is re-gated together with it. The tracker edit is the lead's
   (consistent with `28` §13.5 item 7).
2. **`21` fold-in.** When `21` is next revised, the §4 rows fold in verbatim; no
   separate gate is needed for the fold-in.
3. **`26` §4.9 :1326 citation.** `28` §13.5 cites `26` §4.9 :1326 for this key;
   the live policy text is at `26` §4.3.2 :364-383. The citation is stale, not
   wrong in substance. Flagged for the `26` tracker.

## 12. References

- `docs/design/21-config-jsonc-errata.md` — §3.3 :372-491, §7.1 :1128-1149,
  §7.6 :1233-1240, §9 :1624-1652, §10 :1653-1678, §12 :1695-1736.
- `docs/design/26-dsh-alignment-part2.md` — 26-D2 :140, 26-D23 :161, 26-I11
  :91-110, §4.3.9.1 :948, §4.3.2 :364-383.
- `docs/design/28-llm-service-boundary-errata.md` — §13.5 item 5 :955-956.
- `docs/design/30-architecture-cascade-errata.md` — §5.3 :315-328, §6 P2
  :358-380.
- `docs/design/34-assembler-replay-errata.md` — §9.4 :809-817, §9.5 item 7
  :831-832.
- `docs/design/35-live-notification-errata.md` — the by-reference errata form.
- `docs/design/36-prompt-registry.md` — §4.1 :725-753, §4.2 :754-781, §4.3
  :782-789, §4.4 :790-804.
- Code: `include/ymh/config/config.hpp:158-163`, `src/config/config.cpp:366-370`,
  `:755-827`, `src/cli/wiring.cpp:174-191`, `include/ymh/agent/agent.hpp:110`,
  `src/agent/agent_loop.cpp:535-538`.
- Tests: `tests/unit/config_test.cpp:645-674`, `tests/unit/llm_runtime_test.cpp`
  :264-287, `:346-370`, `tests/unit/spec_catalog_test.cpp:536-560`,
  `tests/fixtures/spec_symbol_catalog.json`.

## 13. Revision log

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-09-19 | Initial authoring. Pins `session.persist_prompt_text`; closes the Wave-1 debt (`28` §13.5 item 5; `30` §6 P2) and the catalog gap. |
