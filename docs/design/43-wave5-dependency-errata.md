# 43 — Wave-5 Dependency Errata: `01` Header/Event, `23` Creation Path, `21` Config Keys, `06` Child Creation

```
Status: Rev 1 written · verified: — · reviewer: —
Component: 43 (errata). Owns the four cross-spec contracts that
           `42-agent-presets.md` §9 lists as blocking prerequisites (PR-1..PR-4)
           for Wave 5. It amends `01-session.md` (the preset/depth header fields
           and the `AgentPresetSelected` codec), `23-session-lifecycle-errata.md`
           (the creation-path copy), `21-config-jsonc-errata.md` (the
           `presets.*` keys), and `06-agent-loop.md` (the additive
           child-creation seam) — all by reference, not in place.
Authority: the owning artifact for `42-agent-presets.md` §9 PR-1..PR-4. No
           Wave-5 code may land before this spec is verified and its four
           contracts are merged into the named specs (`AGENTS.md` two-gate
           rule). Numbered 43 because 31–42 are taken (`29-event-family-
           errata.md:19-25` records the 27–30 renumber).
Depends on: `42-agent-presets.md` (the Wave-5 owning spec; §2.1.1, §2.5, §3,
            §4.1–§4.3, §5); `26-dsh-alignment-part2.md` (verified Rev 7, GATE
            PASS) §4.3.8/§4.3.9/§4.3.9.1/§4.3.9.2/§4.4/§4.6/§4.9;
            `29-event-family-errata.md` (verified per `DESIGN_STATUS.md:41`; the
            event-family contract); `01-session.md`, `06-agent-loop.md`,
            `23-session-lifecycle-errata.md`, `21-config-jsonc-errata.md` (all
            verified); the working tree at authoring time.
Scope: pin the four contracts that Wave-5 code cannot be written without: the
       `01` header fields + reserved-key codec + the `AgentPresetSelected`
       event, the `23` creation-path copy, the `21` `presets.*` keys, and the
       `06` additive child-creation seam. Design only, no code.
Supersedes: nothing. This is a new artifact. Where it disagrees with a verified
            spec, `42` §12 records the resolution; the amendment is explicit
            here, never silent.
```

This document is a **pin**, not a proposal. It exists because `42-agent-presets.md`
§9 names four contracts that must be frozen before Wave-5 code and that no
existing verified spec carries. It pins those contracts; it does not restate the
Wave-5 design, which is `42`.

---

## 1. `01` — the header fields and the `AgentPresetSelected` codec (PR-1)

### 1.1 The `SessionHeader` fields

`01 §3` (`:160-173`) fixes the field list ending at `metadata`. Wave 5 grows it:

```cpp
// include/ymh/session/session.hpp
struct SessionHeader {
    /* ... existing fields (01 §3, :162-172) ... */
    std::optional<std::string> metadata;       // unchanged: opaque; no schema (01 I23)
    std::optional<std::string> agent_preset;   // NEW: the preset the session STARTED with
    std::uint32_t              depth = 0;      // NEW: monotone delegation depth
};
```

Semantics (pinned by `42 §4.1`/`§3.4`):

- `agent_preset` is the **start** preset, frozen at creation. A blank-session
  switch does not rewrite it (`42-I4`).
- `agent_preset == nullopt` means the session predates presets, or was created
  with no default configured. It is **never** synthesized on read (`42-F13`).
- `depth` is written once at creation: root `0`, spawn `parent + 1`, fork
  inherits. It never decreases (`42-I6`).

### 1.2 The `metadata` amendment (reserved-key encoding)

`01 §3` (`:188-190`) says `metadata` is "an opaque JSON string with no schema
commitments" and "not part of the projection"; `01 I23` says it is inert. Wave 5
keeps both properties for **consumers** and adds a **header-codec-owned
reserved-key namespace** inside the `sessions.metadata` JSON object, exactly as
pinned by `42 §4.3` (42-D19):

```jsonc
{ "agent_preset": "standard",   // reserved: SessionHeader.agent_preset
  "depth": 0,                   // reserved: SessionHeader.depth
  "<reserved opaque key>": { }  // the existing SessionHeader.metadata string,
                                // stored verbatim under a reserved key
}
```

Rules:

- The header codec (the `sessions.metadata` read/write path,
  `src/session/session_persistence.cpp:284-308`, `:731-747`) writes
  `agent_preset`/`depth` as the reserved keys and reads them back into the typed
  fields.
- The existing `SessionHeader.metadata` string is stored verbatim under a
  reserved key and **stripped** from the reserved-key namespace on read, so a
  consumer of `metadata` still sees only its own opaque keys. `metadata` stays
  inert (`01 I23`) and never influences `deriveMessages()`.
- The reserved keys `agent_preset` and `depth` are reserved. If the opaque bag
  itself carries either key, the read fails loud (`CorruptionError`, 43-F1); the
  codec never silently picks one.
- `01`'s "no schema" sentence is amended by this errata to: *the user bag has no
  schema; the header codec owns a small reserved-key namespace in the same
  object, and unknown reserved keys are ignored on read.* No DDL changes:
  `kSchemaVersion` stays `1` (`42-I10`).

### 1.3 The `AgentPresetSelected` event and codec

Pinned by `26 §4.3.9` (`:844`), §4.3.9.1 (`:891`), and `29 §3.1`/§4.4 (`:390`
reserves the row); `42 §5.1` restates it. The `01` amendment is:

- Add `AgentPresetSelected` to `enum class EventType`
  (`include/ymh/core/event.hpp:51-75`).
- `wire_name(EventType::AgentPresetSelected) == "agent_preset/selected"`.
- `parse_event_type("agent_preset/selected")` returns it.
- `all_event_types()` includes it.
- `SessionEventMap<EventType::AgentPresetSelected>` =
  `payload::AgentPresetSelected`; an `EventTraits<payload::AgentPresetSelected>`
  specialization exists.

```cpp
// include/ymh/session/events.hpp
namespace payload {
// dsh agent-preset/selected (dsh-agent-presets session.d.ts:25-29).
struct AgentPresetSelected { std::string agent_preset; };
} // namespace payload
```

**JSON keys:** exactly `{ "agent_preset": <string> }` and nothing else. The core
envelope already carries `event_id` and `timestamp`; the payload adds neither
(`26 §4.3.9.1`, `:942-944`).

**Consumers.** The projection switch in `deriveMessages`
(`src/session/session.cpp:363`, switch at `:376`) has **no `default:`**, so it
gains an explicit case:

```cpp
case EventType::AgentPresetSelected:   // metadata; not model-visible
    break;
```

The four `default:`-bearing switches (`ui_event_adapter`, `session_export`,
`session_cli`, `headless`) render the preset row explicitly
(`29 §4.3.9.2` rule 2, `:1006-1010`). The `all_event_types()` round-trip test
covers the new type.

### 1.4 Invariants

- **43-I1.** `AgentPresetSelected` round-trips through `to_json`/`from_json`
  with exactly the `agent_preset` key.
- **43-I2.** `deriveMessages` ignores it; it is metadata, never model-visible.
- **43-I3.** `metadata` remains inert (`01 I23`); reserved keys never influence
  `deriveMessages()`.
- **43-I4.** No DDL change; `kSchemaVersion` stays `1`.

---

## 2. `23` — the creation-path copy (PR-2)

`23 §4.3` (`:846-856`) pins `SessionOptions` with `kind`/`parentSession` and
`:858-866` pins the copy into the header. Wave 5 extends both additively:

```cpp
// include/ymh/session/session_manager.hpp (additive; 43 §2)
struct SessionOptions {
    std::filesystem::path      cwd;
    std::string                serverProfile;
    std::string                model;
    std::string                title;
    SessionKind                kind          = SessionKind::Root;
    std::optional<SessionId>   parentSession = std::nullopt;
    std::uint32_t              depth         = 0;           // NEW (26-D17)
    std::optional<std::string> agent_preset;                // NEW (26-D16)
};
```

`SessionManager::createSession` (`src/session/session_manager.cpp:52-77`) copies
them into the header before `validateHeader`, exactly as it copies
`kind`/`parentSession`:

```cpp
header.kind         = options.kind;
header.parentSession = options.parentSession;
header.depth        = options.depth;
header.agent_preset = options.agent_preset;
validateHeader(header);
```

Rules:

- `validateHeader` accepts the new fields; they are **not** part of the
  kind/parent/seed matrix (`01 I9`), so the matrix is unchanged.
- The roster resolves `agent_preset == nullopt` to the configured default
  **before** the copy (`42 §4.2`), so a preset-enabled workspace never writes
  `nullopt`.
- The fields are persisted through the §1.2 reserved keys; no new column.
- `resume` reads them unchanged and does not rewrite the header; `fork` inherits
  `depth` and records its own `agent_preset` from the parent's live composition
  (`42-D13`).

---

## 3. `21` — the `presets.*` config keys (PR-3)

`26 §4.9` (`:1320-1323`) pins four `presets.*` keys. Wave 5 adds one
(`42 §2.5`, 42-D18). All are **additive**; the required-global-layer rule, the
JSONC-only rule, and the "never auto-create an explicit `--config`" rule
(`21 §6`, `21-D11`/`21-D17`) are untouched.

| Key | Type | Default | Notes |
|---|---|---|---|
| `presets.root` | path | unset | extra root; OQ8 (`26 §4.9:1320`) |
| `presets.default` | string | unset | required when any preset is configured |
| `presets.include_shipped_root` | bool | `true` | |
| `presets.include_user_root` | bool | `true` | |
| `presets.max_depth` | int >= 0 | `3` | NEW; 0 forbids delegation |

Rules:

- The five keys parse into `PresetConfig` (`42 §3.1`): `root`, `default_id`,
  `include_shipped_root`, `include_user_root`, `max_depth`.
- A `presets.max_depth` that is not a non-negative integer fails config load
  loud (`26-I7`); it is never clamped or defaulted silently.
- `presets.default` is required when any preset is configured: `resolve(nullopt)`
  with no default throws `PresetNotFound` (`42-F1`), not an empty preset.
- `presets.max_depth` is the single source of truth for the delegation
  pre-flight; there is no second copy on the tool config (`42-D18`).

---

## 4. `06` — the additive child-creation seam (PR-4)

`06 §7` (`:922-945`) pins subagent spawn/fan-in but has no composition seam. The
Wave-5 additive amendment:

- `AgentServices` (`include/ymh/agent/agent_loop.hpp:49-75`) gains one pointer,
  in the same style as `SystemPrompt* prompt`:

```cpp
// 42 §3.2: the Wave-5 roster. When null, Wave-5 composition is off.
AgentPresetRoster* presets = nullptr;
```

- The delegation tool (`spawn_subagent`, `06 §7` / `07 §5.2`) runs this ordered
  creation window:
  1. Build the child's `AgentContext` (`child AgentId`) and its
     `ChildComposition`.
  2. Run the depth pre-flight
     `check_delegation_depth(parent_depth, presets->config().max_depth)`; refuse
     when `child_depth > max_depth` (`42 §3.4`, 42-I6). No session is created on
     refusal.
  3. Call `presets->apply_child_composition(child_ctx, parent_agent, composition)`
     **before** `registry.create`/`SessionManager::createSession`, so the join,
     the fixed delegation statement, the persona shadow, and the tool shadow all
     exist before the child's first request (`42-I5`; `26-F9`).
  4. Read `presets->composed_preset(child_ctx)` and write it as the child
     header's `agent_preset` (`42 §4.2`).
- `SubagentRunner` (`include/ymh/agent/subagent.hpp:17-31`) passes
  `kind = Subagent`, `parent = parent_.id()`, `depth = parent_depth + 1`, and the
  composed preset id.
- If `AgentServices::presets == nullptr`, the delegation tool spawns through the
  legacy path and applies no composition. It must **not** partially apply: when
  a roster is present, the join is mandatory (`26-F9`).

### 4.1 Invariants

- **43-I5.** No child is created before `apply_child_composition` returns; the
  creation window is the join window (`26-F9`).
- **43-I6.** Child depth is `parent.depth + 1`; a fork inherits the source depth
  (`42-I6`).

---

## 5. Failure modes

- **43-F1: reserved-key collision.** The opaque `metadata` bag carries
  `agent_preset` or `depth`. **Guard:** the read fails loud (`CorruptionError`);
  the codec never silently picks one source (§1.2).
- **43-F2: missing projection case.** `AgentPresetSelected` has no case in the
  `deriveMessages` switch. **Guard:** the switch has no `default:`, so
  `-Werror=switch` is a hard build break (§1.3).
- **43-F3: creation drops the preset.** `createSession` does not copy
  `agent_preset`/`depth`. **Guard:** §2 pins the copy; a preset-enabled workspace
  resolves the default before the copy and never writes `nullopt` (`42-F13`).
- **43-F4: child created without the join.** The delegation tool creates the
  child before `apply_child_composition`. **Guard:** §4 pins the ordered window;
  the join is step 3, before `registry.create` (`26-F9`).
- **43-F5: silent depth default.** `presets.max_depth` is invalid and is clamped
  instead of rejected. **Guard:** §3 fails config load loud (`26-I7`).

---

## 6. Test plan

Wave-5 tests use `FakeLLM` and a fake preset root; no live LLM is required.

### 6.1 Unit

1. `SessionHeader` codec round-trip: `agent_preset`/`depth` survive
   write→read; the opaque `metadata` string is preserved verbatim and stripped of
   reserved keys (43-I1, §1.2).
2. A reserved-key collision in the opaque bag fails loud (43-F1).
3. `metadata` stays inert: `deriveMessages` is unchanged by the presence of
   `agent_preset`/`depth` (43-I3).
4. `AgentPresetSelected` `to_json`/`from_json` emits exactly `{agent_preset}`
   (43-I1).
5. `all_event_types()` includes `AgentPresetSelected`; the wire round-trip
   covers it (43-I1).
6. `createSession` copies `depth`/`agent_preset` into the header; `validateHeader`
   accepts them (43-F3).
7. `presets.max_depth` parses into `PresetConfig`; a negative/non-integer value
   fails config load (43-F5).

### 6.2 Integration (FakeLLM)

1. Spawn a child through the delegation path and assert the child's first request
   carries the parent's composition (the join ran before creation; 43-I5, 43-F4).
2. Spawn to `max_depth` and assert the next attempt is refused with no session
   created (43-I6).
3. Fork a session and assert the fork inherits `depth` and records its own
   `agent_preset` (43-I6, §2).

### 6.3 Build gate

`ctest` builds with `-Werror=switch`; the `deriveMessages` switch must compile
with the new case (43-F2).

---

## 7. Decision register

| ID | Decision | Class | Rationale |
|---|---|---|---|
| 43-D1 | **`agent_preset`/`depth` are typed `SessionHeader` fields**, persisted as reserved keys of the `sessions.metadata` JSON object. | Add. (`01`) | `42-D19`, `42 §4.3`; reconciles `26 §4.4` with `26 §4.6`/`01 §3`. |
| 43-D2 | **The `01` "no schema" rule is amended** to reserve a header-codec namespace; the user bag stays opaque. | Add. (`01`) | `42-D19`; `01:188-190`. |
| 43-D3 | **`SessionOptions` gains `depth`/`agent_preset`** and `createSession` copies them like `kind`/`parentSession`. | Add. (`23`) | `42 §4.2`; `23 §4.3`. |
| 43-D4 | **`presets.max_depth`** (int >= 0, default 3) is the fifth `presets.*` key. | Add. (`21`) | `42-D18`; `26-dsh-alignment.md:1094`. |
| 43-D5 | **`AgentServices` gains `AgentPresetRoster* presets`**; the delegation tool applies the child composition before creation. | Add. (`06`) | `42-I5`; `26-F9`. |

---

## 8. References

**Owning spec this serves**

- `42-agent-presets.md` §2.1.1 (preset schema), §2.5 (config), §3 (interfaces),
  §4.1–§4.3 (header/storage), §5 (events), §9 (PR-1..PR-4).

**Specs this amends (by reference)**

- `01-session.md` `SessionHeader` (`:149-190`), `deriveMessages` (the
  projection), `01 I23` (`:1189`).
- `23-session-lifecycle-errata.md` `SessionOptions` (`:846-856`), the
  creation-path copy (`:858-866`).
- `21-config-jsonc-errata.md` §6 (no TOML; config-load gate).
- `06-agent-loop.md` §7 (subagents), `AgentServices`.

**Shipped-code anchors (working tree at authoring time)**

- `include/ymh/session/session.hpp:44-58` (`struct SessionHeader`).
- `include/ymh/session/session_manager.hpp:29-36` (`struct SessionOptions`),
  `:68` (`sessionPtr`).
- `src/session/session_manager.cpp:52-77` (`createSession`).
- `src/session/session_persistence.cpp:36-51` (`sessions` DDL; `metadata` at
  `:47`), `:284-308` (header read), `:731-747` (header write).
- `include/ymh/core/event.hpp:51-75` (`enum class EventType`), `:83`/`:86`/`:90`
  (`wire_name`/`parse_event_type`/`all_event_types`).
- `src/session/session.cpp:363` (`deriveMessages`), `:376` (the switch, no
  `default:`).
- `include/ymh/agent/agent_loop.hpp:49-75` (`AgentServices`).
- `include/ymh/agent/subagent.hpp:17-31` (`SubagentRunner`).

---

## 9. Revision log

| Rev | Date | Change |
|---|---|---|
| Rev 1 | 2026-09-20 | Initial pin. Authored with `42` Rev 2 to discharge `42 §9` PR-1..PR-4: the `01` header fields + reserved-key codec + `AgentPresetSelected`, the `23` creation-path copy, the `21` `presets.*` keys, and the `06` child-creation seam. |
