# 20 — Skills

```
Status: written · verified: — · reviewer: —
Revision: 3 — rev 3 fixes the round-2 gate findings H1 (the usability
          predicate, regressed by rev 2's M6 fix), M1–M2, L1–L7; rev 2 fixed the
          round-1 findings H1–H4, M1–M6, L1–L6. §17.1 records the citation
          re-derivation against the current tree.
Component: 20 (new subsystem) — the `/skills` command (RB-05)
Depends on: 00-architecture.md §7 (plugin model), §26 (slash commands), §27 (MCP
            adapter), §31 (context management), §32 (compaction), §37
            (configuration), §40 (logging), §44–§45 (testing), §46 (security),
            §51 (phase 2), §54 (F1–F12); specs 01, 06, 07, 09, 10, 13, 15, 17
Depends on (optional): spec 18 (`/context`) — **only** for the optional
            `/context`-visibility assertions in §8.5 and §14.2. This component is
            implementable and testable **before** 18 lands (20-D12, L4).
Scope: a discoverable, prompt-carrying capability bundle — its on-disk model,
       discovery/precedence, storage/format, `/skills` listing, invocation
       (explicit + model-invoked), context injection, token accounting,
       security/path safety, and its relationship to the tool registry and MCP
Amends:    (additive) `config.hpp` `[skills]`; `05` transport catalog +2 methods;
            `10` `CommandContext` +2 callbacks / `builtin()` +2 commands;
            `wiring.cpp` system-prompt composition; `workspace_runtime` +
            `skills()` accessor + `WorkspaceRuntimeOptions` permission-path flag.
            See §15.3.
```

This document is the design gate for **RB-05 — "Develop a `/skills` command"**
(`REQUIREMENTS_BACKLOG.md:34,171-187`). It is a **new subsystem**: the backlog
records that there is *no* skill concept anywhere in `src/` or `include/`, only a
mention of the *dsh reference design's* plugin taxonomy at
`docs/design/00-architecture.md:4864` ("models, tools, skills, sessions, … are
plugins"), which is not an ymh feature (`REQUIREMENTS_BACKLOG.md:173-179`). The
slash-command registry exists (`include/ymh/ui/command_registry.hpp`,
`src/ui/command_registry.cpp`), so a command can be registered cheaply, but the
skill model, discovery, storage, invocation, and context injection must be
designed here.

It is **design-only**. Nothing here is verified; per `AGENTS.md`'s non-negotiable
rule, no code may be written for this component until this spec is independently
reviewed and marked `verified` with no open HIGH/MEDIUM findings
(`AGENTS.md:7-19`). The interfaces in §5 are **pinned** and must not churn after
verification.

**Naming note.** Specs 01–19 already claim the invariant prefixes `S` (01), `P`
(02), `R` (03), `H`/`D` (04), `T` (05), `A` (06), `X`/`E` (07), `L` (08),
`Q` (09), `U` (10), `M` (11), `C` (13), `E-P` (14), `MCP` (15), `O` (16),
`U-RB*` (17), `CTX` (18), `RN`/`R-F` (19) — see `16-daemon-ownership.md:39-44`
and `19-session-rename-errata.md:121`. This spec uses the fresh, collision-free
prefixes **`SK1`–`SK17`** (invariants), **`SK-F1`–`SK-F13`** (component-local
failure modes), and **`20-D1`–`20-D13`** (decisions). The bare architecture
decision ids `D1`–`D23` (`00-architecture.md:4740-4830`) and the shared review
findings `F1`–`F12` (`00-architecture.md:4832-4856`) keep their existing meaning;
where this spec pins a new architecture-level decision it is written `20-Dn`,
never a bare `Dn`.

**Independence note (L4).** Spec 18 (`/context`) is still **unverified**. This
spec is designed to be implementable and fully testable **without it**:

- the index composition (§5.5), the catalog, the `skill` tool, the `/skills`
  listing, and the `/skill` activation depend on **no** spec-18 type, RPC, or
  segment;
- the **only** spec-18 dependencies are (a) the optional `/context`-visibility
  assertion in §8.5/§14.2 ("the index appears in the `SystemPrompt` segment") and
  (b) the optional additive `SkillIndex` segment (OQ-1/AM-20g). Both are marked
  optional and are skipped, not failed, until 18 lands. §14.2 pins a
  spec-18-independent fallback assertion (`E(assemble(...))` directly).

If 18 is later rejected or re-scoped, nothing in this spec changes except the
optional items above (20-D12).

---

## 1. Purpose and scope

### 1.1 What a skill is, in one sentence

A **skill** is a user-authored, filesystem-discovered, named markdown document
whose body is *instructions for the agent*, and whose only runtime effect is to
place that text into the model's context — either as a small always-present index
entry (name + description) or as a full body loaded on demand.

A skill is **not** code, **not** a tool, **not** a plugin, and **not** a
permission grant. This spec is deliberately conservative about that boundary
(§9, §10).

### 1.2 Position in the component graph

```text
   <config-root>/ymh/skills/<name>/SKILL.md        (user tier — trusted)
   <workspace>/.ymh/skills/<name>/SKILL.md         (workspace tier — untrusted)
                     │  discovery (daemon startup)
                     ▼
              SkillCatalog  ──────────────► index_section() ──┐
                     │                                         │
        ┌────────────┴─────────────┐                           │
        ▼                          ▼                           │
   SkillTool : Tool          HostRuntime                      │
   (model-invoked)           skills.list / skills.show        │
        │                          │                           │
        ▼                          ▼                           ▼
   ToolRegistry ──► AgentLoop    supervisor TUI          effective system prompt
        │              │          (/skills, /skill)       (assembler, pinned)
        ▼              ▼
   ToolCall/ToolResult  ContextInjected ──► deriveMessages ──► LLMRequest
```

The catalog is **daemon-owned** (like `McpManager`, `15-mcp-adapter.md:857`): the
workspace daemon is the only process that resolves paths
(`src/agent/workspace_runtime.cpp:52-58`), and the supervisor/TUI never reads
skill files (SK15). The supervisor obtains the list over an additive read-only
RPC (§5.7) and activates a skill through the existing `agent.inject` method
(`include/ymh/transport/protocol.hpp:500`).

### 1.3 Owned responsibilities (this spec pins)

- the skill on-disk model and frontmatter schema (§2);
- discovery roots, precedence, collision, symlink, and malformed-file rules
  (§3);
- storage/format and the **no-new-dependency** decision (§4);
- the C++ interfaces for the catalog, parser, tool, config, RPC, and UI (§5);
- the `/skills` listing command and its rendering (§6);
- the invocation model — explicit command **and** model-invoked tool — and its
  justification against the agent loop and permission model (§7);
- exactly how skill text enters model context, how it is token-accounted, and how
  it interacts with compaction and `/context` (§8);
- the security and path-safety stance (§9);
- the relationship to the tool registry and MCP (§10);
- invariants, failure modes, a dsh mapping, and a test plan (§11–§14).

### 1.4 Boundaries — deferred to other specs

- **Context assembly and compaction** stay owned by `06`/`13`. This spec does not
  add an assembler method; it *feeds* the existing `systemPrompt_` string
  (`src/agent/context_assembler.cpp:33-34`) and reuses the existing
  `ContextInjected` projection (`src/session/session.cpp:317-320`). It only
  records the interaction rules (§8).
- **Permission semantics** stay owned by `09`. The `skill` tool is an ordinary
  tool; this spec adds no approval path. `09`'s evaluation is name-driven and
  applies unchanged (`09-permissions.md:443-496`, §3.4).
- **Transport framing/profiles** stay owned by `05`; this spec adds two read-only
  methods and two pure-virtual host methods (§5.7).
- **UI rendering primitives** stay owned by `10`/`17`; this spec reuses the
  existing file-local `append_system` logic by exporting it as
  `append_system_entry` (`src/ui/command_registry.cpp:23-33`, §6.1) and the
  transcript `ConversationEntry` (`include/ymh/ui/ui_model.hpp:80-86`).
- **The `/context` snapshot** stays owned by `18`; this spec states how the skill
  index and loaded bodies are accounted for and proposes one optional additive
  segment (OQ-1, §15.3 AM-20g).

### 1.5 Design principles applied

- **Design-first.** This spec precedes any code (`AGENTS.md:7-19`).
- **No new dependency unless unavoidable.** A YAML-subset frontmatter parser is
  hand-rolled; no YAML library is added (§4.2, `AGENTS.md:64-65`).
- **Fail safe, fail loud where authored by the operator, skip-and-warn where
  authored by the world.** Malformed operator config is a hard `ConfigError`
  (`src/config/config.cpp:23-25`); malformed skill files are skipped (§3.5).
- **The permission policy remains between the agent and execution**
  (`00-architecture.md:4331-4357`, D7). A skill cannot move that boundary.

---

## 2. The skill model

### 2.1 Shape

A skill is a **directory** containing a **`SKILL.md`** file:

```text
<root>/<skill-name>/
├── SKILL.md          # required: YAML-subset frontmatter + markdown body
└── …                 # optional supporting files (NEVER auto-loaded; see §9.5)
```

The directory name **is** the skill's identity: `SKILL.md`'s frontmatter `name`
must equal the containing directory's basename. A mismatch is malformed
(SK-F3).

Rationale for a directory (not a bare `.md` file): it reserves room for
supporting assets (templates, reference snippets) without inventing a manifest,
and it matches the ecosystem convention the requirement's author referenced
(Claude-Code-style skills). Supporting files are **not** read by this subsystem
(§9.5); they exist for the human and for tools the model may later invoke
explicitly.

### 2.2 Frontmatter schema (field by field)

The file **must** begin with a `---` fence on line 1 and close it with a `---`
line. Everything between is a **strict YAML subset**: top-level
`key: scalar` mappings and `key: [a, b, c]` flow sequences only. Nested maps,
block scalars, anchors, aliases, tags, and multi-document streams are rejected
(§4.2).

| Field | Type | Required | Default | Meaning / constraint |
|---|---|---|---|---|
| `name` | string | **yes** | — | Skill identity. Grammar `[a-z][a-z0-9]*(-[a-z0-9]+)*`, length 1–64. **Must equal the containing directory basename** (SK-F3). |
| `description` | string | **yes** | — | One-line summary shown by `/skills` and in the model index. Bounded by `max_description_bytes` (default 512); over-long is truncated at a word boundary and a warning is recorded. |
| `version` | integer | no | `1` | Author's contract version. Display only. |
| `allowed-tools` | list<string> | no | `[]` | **Advisory only.** Displayed by `/skills --show`; **never enforced** and never converted into a grant (20-D7, SK13). Values are not validated against `ToolRegistry`. |
| `tags` | list<string> | no | `[]` | Free-form labels for `/skills` grouping. Inert. |
| `license` | string | no | `""` | Display only. |
| `model` | string | no | `""` | Advisory model hint. **Inert** — the model is resolved by config/CLI (`effective_model`, `src/cli/wiring.cpp:157`), never by a skill. |

Unknown top-level keys are **rejected** (malformed) to match the config
loader's strictness philosophy (`include/ymh/config/config.hpp:13-15`) and to
prevent a typo from silently disabling a safety-relevant field. This is stricter
than YAML's permissiveness and is a deliberate choice (20-D3).

The body is the raw markdown after the closing fence (leading blank lines
trimmed, trailing newline normalized). It is stored verbatim; it is **not**
parsed, templated, or expanded (SK1).

### 2.3 Identity

`SkillName` is a distinct namespace from `ToolName` (`include/ymh/tools/tool.hpp:26-31`).
A skill named `read_file` does **not** shadow the `read_file` tool and is not
reachable as a tool: it is reachable only through the single `skill` tool's
`name` argument (§7.3, SK10). The kebab-case grammar (`git-commit`) is chosen
because it is the ecosystem convention and because it cannot be confused with
`ToolName`'s underscore grammar.

### 2.4 Trust tiers

Trust is a function of **where the file was found**, never of its contents
(SK2):

| Tier | Root | Trust | Rationale |
|---|---|---|---|
| **User** | `$XDG_CONFIG_HOME/ymh/skills/` else `$HOME/.config/ymh/skills/` | **Trusted** | Authored by the operator, outside any VCS checkout, not clonable by a third party. |
| **Workspace** | `<workspace>/.ymh/skills/` | **Untrusted** | Arrives with `git clone`; a repository author can write it. |

There are **no built-in skills** in v1 (20-D2). `SkillSource` reserves a
`Builtin` enumerator only so a future spec can add one additively without a
rename; nothing in v1 produces it (OQ-3).

### 2.5 Malformed-skill policy — skip fatal, keep + warn non-fatal (justified)

Two classes of parser outcome are distinguished (pinned, M1):

- **Fatal** (`FrontmatterResult::error` non-empty — a missing `name`/`description`,
  a bad `name`, a missing fence, an unknown key, an over-long fence region, …):
  the skill is **skipped**.
- **Non-fatal** (`FrontmatterResult::warnings` non-empty — currently only the
  `description` truncation of SK-F4): the skill is **kept and loadable**; the
  warnings do not suppress it.

In both cases a `SkillLoadWarning` is recorded and surfaced through two concrete,
existing channels: (a) one `Logger::warn` line at discovery time via the injected
`category_logger(LogCategory::Tool)` (`include/ymh/core/logger.hpp:29`,
`include/ymh/core/logging.hpp:55`), and (b) the `warnings` array in the
`skills.list` reply, rendered in the `/skills` footer. It is never a hard error
and never a silent drop (SK-F1…SK-F9, SK-F13). **No new live event is
introduced** (L3).

**Warning mapping (pinned).** The parser returns strings; the catalog wraps them.
`SkillCatalog::discover()` maps every non-empty `FrontmatterResult::warnings[i]`
to `SkillLoadWarning{skill.file, warnings[i]}`, appends it to `warnings_`, **and
still inserts the skill into `all_`** (§5.2, §3.6). A fatal
`FrontmatterResult::error` maps to exactly one `SkillLoadWarning{file, error}`
and no skill. `SkillLoadWarning` is the only warning type; it flows verbatim to
`skills.list` (§5.7) and to the `/skills` footer (§6.2).

Justification:

1. **Attacker-controlled input must not be able to DoS the daemon.** A hard error
   would let any cloned repository's `.ymh/skills/broken/SKILL.md` prevent the
   workspace daemon from starting — a trivial denial of service. The MCP adapter
   makes the identical choice for incompatible tools: "skip that tool … server
   stays `Ready` while ≥ 1 tool translated" (`15-mcp-adapter.md:1630`, MCP-F4,
   MCP-F8).
2. **Partial value is better than none.** One broken skill should not hide the
   other nine.
3. **Silence is unacceptable.** Every skip is recorded with its file and reason
   and shown in `/skills`, so a typo is discoverable rather than mysterious.

The **counter-rule**: a malformed `[skills]` *config* section is a hard
`ConfigError` (`src/config/config.cpp:23-25`), because config is operator-authored
and a typo must never be silently ignored (`include/ymh/config/config.hpp:13-15`).

---

## 3. Discovery

### 3.1 Search roots

Discovery is performed **once at daemon startup** by `SkillCatalog::discover()`,
before the tool registry is frozen (`src/agent/workspace_runtime.cpp:66-79`) and
before the assembler's system prompt is fixed (`:58`). Roots, in order:

1. **User root** — `default_skills_root()` (§5.2.1), defined as
   `default_global_config_path().parent_path() / "skills"`. It mirrors the config
   loader's own resolution (`src/config/config.cpp:579-587`):
   `$XDG_CONFIG_HOME/ymh/skills` when `XDG_CONFIG_HOME` is set, else
   `$HOME/.config/ymh/skills`, else `.config/ymh/skills`. This is the same root
   the config loader already reads, so the skills subsystem introduces **no new
   root convention**.
2. **Workspace root** — `<environment-root>/.ymh/skills`, where the environment
   root is `ExecutionEnvironment::root()` (`include/ymh/execution/environment.hpp:32`),
   i.e. the canonical workspace path baked in at daemon startup
   (`src/execution/environment.cpp:37-58`).

**The user root MUST be absolute (M2, pinned).** `default_global_config_path()`
is not guaranteed absolute: it returns `$XDG_CONFIG_HOME/...` verbatim, and its
last fallback is the **relative** `.config/ymh/config.toml`
(`src/config/config.cpp:579-587`). The daemon `chdir()`s to the workspace root at
startup (`src/host/workspace_host.cpp:464-467`), so a relative user root would
resolve *inside the workspace* — and every skill found there would then be
classified **Trusted** by provenance (§2.4), i.e. workspace-controlled content
acquiring user trust. Discovery therefore **requires an absolute user root**:
`SkillCatalog::discover()` tests `user_root.is_absolute()` and, when it is false,
**disables the user tier entirely** (empty) and records one `SkillLoadWarning`
(SK-F13). It never canonicalizes a relative user root against the daemon cwd. The
workspace tier is unaffected. An operator who wants a user tier must export an
absolute `XDG_CONFIG_HOME` or `HOME`.

No other roots are searched. In particular there is **no** built-in root and
**no** arbitrary `skills.roots` config list in v1 (OQ-3).

### 3.2 Precedence and collisions

**Rule (pinned): within a tier, first-in-deterministic-order wins; across tiers,
the user (trusted) skill always wins and the workspace skill is rejected.**

- **Within a tier**, two entries cannot share a name because the name *is* the
  directory basename and a directory cannot contain two identical names. The only
  way to collide is a case-insensitive filesystem or a symlinked duplicate; both
  are resolved by deterministic ordering: user tier before workspace tier, then
  byte-wise ascending `SkillName`, then byte-wise ascending canonical path. The
  first wins; every later collision is skipped with a warning (SK-F6).
- **Across tiers**, a workspace skill whose `SkillName` equals a user skill's
  name is **skipped** with a warning; the trusted user skill wins (SK-F5, SK3).

**Why the user tier wins (rejected alternative: workspace overrides user).**
Layering elsewhere is project-over-global (`include/ymh/config/config.hpp:5-11`),
so "workspace overrides user" is superficially consistent. It is rejected here
because skills are *content from a potentially hostile source*, not operator
config. If workspace could shadow user, a repository could ship
`.ymh/skills/git-commit/SKILL.md` and silently replace the operator's trusted
`git-commit` with instructions of the attacker's choosing — a one-clone
prompt-injection. Cross-tier shadowing is therefore forbidden outright. An
operator who wants a per-repo variant must give it a **different name** (or place
it in the user tier), which is a conscious act.

### 3.3 Symlinks and special files

- A skill directory that is a **symlink** is followed only if its canonical
  target is still **within the same root**; otherwise it is skipped with a
  warning (SK-F7). The check is the existing `path_is_within()` component-prefix
  test (`src/execution/environment.cpp:32-35`).
- `SKILL.md` itself must be a **regular file** after canonicalization. A symlink
  whose target leaves the root, a device node, a FIFO, or a directory named
  `SKILL.md` is skipped (SK-F7).
- The **workspace** root's reads are additionally performed through
  `ExecutionEnvironment::resolve()` (`src/execution/environment.cpp:60-91`),
  which realpath-canonicalizes and re-checks containment on every path (X1–X4).
- The **user** root is outside the workspace, so `resolve()` cannot be used for
  it (it would throw `PathEscape`, `environment.cpp:73-76`). It is read with the
  same `weakly_canonical` + `path_is_within` discipline, rooted at the config
  dir — exactly analogous to the config loader reading
  `$XDG_CONFIG_HOME/ymh/config.toml` outside the workspace
  (`src/config/config.cpp:579-587`). This is the **one** sanctioned read outside
  the workspace root, and it is read-only (20-D3, §9.4).
- `chdir()` is **never** called (`00-architecture.md:4810`, D18). The daemon's cwd
  is set once at startup; discovery uses absolute canonical paths only.

### 3.4 Bounds

| Bound | Default | Scope | Enforced at |
|---|---|---|---|
| `max_skills` | 256 | **global** (both tiers merged) | discovery, after merge+sort (excess skipped, SK-F9) |
| `max_skill_bytes` | 64 KiB | per skill | discovery (oversized skipped, SK-F8) |
| `max_description_bytes` | 512 | per skill | frontmatter parse (truncated + warned) |
| `max_index_bytes` | 8 KiB | catalog | index composition (truncated + note, SK-F12) |
| `max_frontmatter_bytes` | 4 KiB | per skill | parse (over-long fence region rejected, SK-F1) |
| directory depth | 1 (`<root>/<name>/SKILL.md`) | per skill | discovery |

`max_skills` is **global** (not per tier) and is applied to the merged,
first-wins, deterministically sorted set — never to the raw per-tier scans — so
the retained set is a pure function of the filesystem (SK4). The per-tier scans
are themselves unbounded in count but bounded per entry; the global cap is what
bounds memory. These are configurable under `[skills]` (§5.6). They bound both
memory (`max_skills × max_skill_bytes` ≈ 16 MiB worst case) and the always-on
system-prompt cost (`max_index_bytes`).

### 3.5 Malformed handling

See §2.5. Discovery never throws for file-level problems; it accumulates
`SkillLoadWarning`s. It **does** throw only for an unreadable/invalid *config*
(`ConfigError`) or an invalid environment root (already thrown by
`LocalEnvironment`, `src/execution/environment.cpp:47-56`).

### 3.6 Discovery algorithm (deterministic)

Order of operations is pinned; `max_skills` is enforced **after** the merge and
sort (M5):

```text
discover():
    warnings := []
    if user_root.is_absolute():               # M2, SK-F13
        user_skills := scan(user_root)        # tier = User
    else:
        warnings += {user_root, "relative user root: user tier disabled"}
        user_skills := []
    workspace_skills := scan(workspace_root)  # tier = Workspace
    by_name := {}                             # first-wins across tiers, user first
    # scan() yields (skill, warn_list); `skill == null` is a FATAL parse or
    # validation error. Non-fatal warnings ride alongside a usable skill (M1).
    for (skill, warn_list) in user_skills ++ workspace_skills:  # stable concat
        warnings += warn_list
        if skill == null:                     # fatal: skip (SK-F1..F3, F7, F8)
            continue
        if skill.name in by_name:
            warnings += {skill.file, "shadowed by " + by_name[skill.name].file}
            continue
        by_name[skill.name] = skill
    ordered := sort(by_name.values(), by=(tier, name, canonical_path))
    # (1) resolve collisions first, (2) sort deterministically, (3) then cap.
    if ordered.size() > config.max_skills:
        for extra in ordered[config.max_skills ..]:
            warnings += {extra.file, "exceeds max_skills"}
        ordered.resize(config.max_skills)
    all_ := ordered
    model_visible_ := [s in all_ if s.trust == Trusted
                              or config.expose_workspace]
    index_section_ := build_index(model_visible_, config.max_index_bytes)
```

Because the cap is applied last, it can never change which skill wins a
collision, and the retained set is deterministic (SK4).

`scan()` lists `<root>/*/SKILL.md`, canonicalizes, applies §3.3, parses (§4.2),
validates (§2.2), and eagerly loads the body (§4.1). A fatal parse/validation
error yields `(null, [SkillLoadWarning{file, reason}])`; a non-fatal
`FrontmatterResult::warnings` entry is copied into the returned `warn_list` while
the skill is still returned, so a skill that carries only a warning stays
loadable and invocable (§2.5, SK-F4, M1). The concatenation order (user then
workspace) plus the stable sort makes `all_` a pure function of the filesystem —
SK4, and the same determinism property MCP pins in M13
(`15-mcp-adapter.md:1576-1578`).

---

## 4. Storage and format

### 4.1 On-disk layout

```text
<config-root>/ymh/skills/            # user tier (trusted)
  git-commit/
    SKILL.md
  release-notes/
    SKILL.md
    template.md                       # supporting file — never auto-loaded
<workspace>/.ymh/skills/             # workspace tier (untrusted)
  repo-conventions/
    SKILL.md
```

`<workspace>/.ymh/` already exists and is gitignored by the daemon at startup
(`src/host/workspace_host.cpp:469-475`), so a workspace skill directory is a
natural addition and does not pollute the repository's tracked tree unless the
author chooses to commit it (which is exactly the untrusted case §2.4
addresses).

### 4.2 Frontmatter format — hand-rolled YAML subset, **no new dependency**

**Decision (20-D3): the frontmatter parser is a strict, hand-rolled subset
implemented inside `ymh`; no YAML library is added.**

The parser accepts exactly:

```text
line 1:  ---
then:    key: scalar            (bare, single-quoted, or double-quoted)
         key: [a, b, c]         (flow sequence of scalars)
close:   ---
```

`scalar` is a trimmed string; `[a, b, c]` splits on commas and trims each item.
Empty values are permitted (`license:` ⇒ `""`). The only typed field is `version`
(integer); the rest are strings or string lists. Comments (`#`) are **not**
supported in v1 (rejected: they complicate quoting rules for no benefit).

**Justification against `AGENTS.md:64-65` ("do not build a giant umbrella
dependency").**

- A general YAML parser (libyaml, yaml-cpp, rapidyaml) is precisely the "giant
  umbrella dependency" the project forbids for a schema this small. The accepted
  dependency set is SQLite3 / nlohmann_json / spdlog / fmt / libcurl / libgit2 /
  cmark-gfm plus FetchContent FTXUI / Asio / toml++ / CLI11 / GoogleTest
  (`AGENTS.md:64-65`); none is a YAML engine.
- `toml++` **is** already available (`CMakeLists.txt:84-92`) and could parse TOML
  frontmatter, but TOML frontmatter uses `+++` fences and is not the convention
  the requirement's author referenced. Adopting TOML would make ymh skill files
  incompatible with the ecosystem while saving at most ~120 lines of parser code.
  Rejected (20-D3).
- The schema is a flat map of scalars/lists. A strict subset parser is small,
  dependency-free, warning-clean under `-Werror`, and testable exhaustively. It
  also lets us reject unknown keys (§2.2), which a permissive YAML parser would
  not do for free.
- **Escape hatch:** if a future spec needs richer frontmatter, the parser is
  isolated behind `parse_skill_file()` (§5.3) and can be replaced without
  touching the catalog, tool, or RPC.

A UTF-8 BOM is stripped; CRLF line endings are normalized; a missing closing
fence, a fence region over `max_frontmatter_bytes`, a duplicate key, an unknown
key, an unsupported construct, or a bad `version` are all malformed (SK-F1).

### 4.3 Body

The body is stored verbatim in memory (eager load, §4.1/§5.2). It is never
templated, never shell-expanded, never resolved. Markdown is the only format;
rendering it in the transcript reuses the existing markdown renderer
(`src/ui/render/markdown_renderer.cpp`) when a body is displayed by
`/skills --show` (optional, §6.4).

---

## 5. C++ interfaces (pinned)

All sketches use existing ymh types. `Role`, `Message`, `ContentBlock` are from
`include/ymh/agent/message.hpp`; `Tool`, `ToolSchema`, `ToolResult` from
`include/ymh/tools/tool.hpp`; `ExecutionEnvironment` from
`include/ymh/execution/environment.hpp`; `Logger` from `include/ymh/core/logger.hpp`.

### 5.1 `SkillName`, sources, trust, metadata

```cpp
// include/ymh/skills/skill_types.hpp  (NEW)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ymh {

// Grammar: [a-z][a-z0-9]*(-[a-z0-9]+)*, length 1..64 (20 §2.3).
struct SkillName {
    std::string value;
    auto operator<=>(const SkillName&) const = default;
};

[[nodiscard]] bool is_valid_skill_name(std::string_view name) noexcept;

enum class SkillSource : std::uint8_t {
    User,       // <config-root>/ymh/skills/<name>/SKILL.md
    Workspace,  // <workspace>/.ymh/skills/<name>/SKILL.md
    Builtin,    // reserved; never produced in v1 (20-D2, OQ-3)
};

enum class SkillTrust : std::uint8_t {
    Trusted,    // User
    Untrusted,  // Workspace (VCS-controllable)
};

struct SkillMetadata {
    SkillName                name;
    std::string              description;
    std::uint32_t            version{1};
    std::vector<std::string> allowed_tools;  // ADVISORY ONLY — never enforced
    std::vector<std::string> tags;
    std::string              license;
    std::string              model;          // advisory hint — never enforced
};

struct Skill {
    SkillMetadata         meta;
    SkillSource           source{SkillSource::User};
    SkillTrust            trust{SkillTrust::Trusted};
    std::string           body;          // eagerly loaded, bounded (20 §4.3)
    std::filesystem::path file;          // canonical SKILL.md path (provenance)
};

[[nodiscard]] std::string_view skill_source_name(SkillSource source) noexcept;
[[nodiscard]] std::string_view skill_trust_name(SkillTrust trust) noexcept;

} // namespace ymh
```

### 5.2 `SkillCatalog` (discovery + immutable store)

```cpp
// include/ymh/skills/skill_catalog.hpp  (NEW)
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/core/logger.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/skills/skill_types.hpp"

namespace ymh {

struct SkillLoadWarning {
    std::filesystem::path file;
    std::string           reason;
};

struct SkillCatalogConfig {
    bool        enabled = true;
    bool        expose_workspace = false;  // untrusted tier visible to the model
    std::size_t max_skills = 256;
    std::size_t max_skill_bytes = 64u * 1024u;
    std::size_t max_description_bytes = 512;
    std::size_t max_index_bytes = 8u * 1024u;
    std::size_t max_frontmatter_bytes = 4u * 1024u;
};

// Immutable after discover(); owned by the workspace daemon (20-D10, SK15).
// `discover()` performs all I/O; the read accessors are const and thread-safe
// because the vectors are never mutated after discovery.
class SkillCatalog {
public:
    SkillCatalog(SkillCatalogConfig       config,
                 const ExecutionEnvironment& environment,
                 std::filesystem::path    user_root,
                 Logger&                  logger);

    // Non-copyable, non-movable (L7): `model_visible_` stores `const Skill*`
    // into `all_`, so a copy would leave the pointers aliasing the source's
    // vector and a move-assignment could reallocate. The catalog is always held
    // by `shared_ptr` and never copied/moved; this mirrors `WorkspaceRuntime`
    // (`workspace_runtime.hpp:110-113`) and `ToolRegistry`
    // (`tool_registry.hpp:76-77`).
    SkillCatalog(const SkillCatalog&) = delete;
    SkillCatalog& operator=(const SkillCatalog&) = delete;
    SkillCatalog(SkillCatalog&&) = delete;
    SkillCatalog& operator=(SkillCatalog&&) = delete;

    void discover();

    [[nodiscard]] const SkillCatalogConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::vector<Skill>& all() const noexcept { return all_; }
    [[nodiscard]] const std::vector<const Skill*>& model_visible() const noexcept {
        return model_visible_;
    }
    // Two distinct lookups (H2). `find()` searches ALL discovered skills and is
    // used by the user-facing surfaces (`/skill`, `skills.show`, `/skills`).
    // `find_model_visible()` searches only `model_visible_` and is the ONLY
    // lookup the model-facing `SkillTool` may use (SK8/SK10).
    [[nodiscard]] const Skill* find(std::string_view name) const noexcept;
    [[nodiscard]] const Skill* find_model_visible(std::string_view name) const noexcept;
    [[nodiscard]] const std::vector<SkillLoadWarning>& warnings() const noexcept {
        return warnings_;
    }
    // The pinned, always-present system-prompt block (20 §8.1). Empty when
    // disabled or when there are no model-visible skills.
    [[nodiscard]] const std::string& index_section() const noexcept {
        return index_section_;
    }

private:
    SkillCatalogConfig          config_;
    const ExecutionEnvironment* environment_;
    std::filesystem::path       user_root_;
    Logger*                     logger_;
    std::vector<Skill>          all_;
    std::vector<const Skill*>   model_visible_;
    std::vector<SkillLoadWarning> warnings_;
    std::string                 index_section_;
};

} // namespace ymh
```

#### 5.2.1 Pinned path/config helpers (M2)

Two free functions are pinned so the previously undeclared symbols have a home:

```cpp
// include/ymh/skills/skill_catalog.hpp  (NEW, same header)
namespace ymh {

// The user skills root: default_global_config_path().parent_path() / "skills".
// Mirrors the config loader's resolution (src/config/config.cpp:579-587):
// $XDG_CONFIG_HOME/ymh/skills, else $HOME/.config/ymh/skills, else
// .config/ymh/skills. Returns an absolute-or-relative path exactly as
// default_global_config_path() does; discovery canonicalizes it.
[[nodiscard]] std::filesystem::path default_skills_root();

} // namespace ymh
```

```cpp
// include/ymh/cli/wiring.hpp  (additive)
namespace ymh {

// Maps Config::skills onto SkillCatalogConfig (20 §5.6). Pure.
[[nodiscard]] SkillCatalogConfig to_skill_catalog_config(const Config& config);

} // namespace ymh
```

`default_skills_root()` is implemented in terms of the already-declared
`default_global_config_path()` (`include/ymh/config/config.hpp:168`); it adds no
new environment-variable logic. `default_global_config_dir()` is not a separate
symbol — it is exactly `default_global_config_path().parent_path()` and is
written inline. **The returned path may be relative** (the loader's last fallback
is `.config/ymh/config.toml`, `src/config/config.cpp:579-587`); discovery MUST
NOT canonicalize a relative user root against the daemon cwd — it disables the
user tier instead (§3.1, SK-F13, M2). `wiring.hpp` gains
`#include "ymh/skills/skill_catalog.hpp"` for `SkillCatalogConfig` (mirroring how
it already includes `ymh/mcp/mcp_types.hpp` for `McpConfig`,
`include/ymh/cli/wiring.hpp:14,27`).

### 5.3 Frontmatter parser

```cpp
// include/ymh/skills/frontmatter.hpp  (NEW)
#pragma once

#include <string>
#include <string_view>

#include "ymh/skills/skill_types.hpp"

namespace ymh {

struct FrontmatterResult {
    SkillMetadata            meta;      // populated only when `error` is empty
    std::string              body;      // text after the closing fence (normalized)
    std::string              error;     // empty on success; otherwise a reason
    std::vector<std::string> warnings;  // non-fatal, e.g. description truncation (M3)
};

// Strict YAML-subset parser (20 §4.2). Pure: no I/O, no allocation beyond the
// result. `content` is the full SKILL.md text. `expected_name` is the directory
// basename; a mismatch yields an error (SK-F3).
[[nodiscard]] FrontmatterResult parse_skill_file(std::string_view content,
                                                 std::string_view expected_name,
                                                 std::size_t max_frontmatter_bytes,
                                                 std::size_t max_description_bytes);

} // namespace ymh
```

**Contract (M1).** `error` is fatal: when non-empty, `meta`/`body` are not
consumed and the skill is skipped. `warnings` is non-fatal: the skill is still
returned and MUST be kept. `SkillCatalog::discover()` maps each warning string to
`SkillLoadWarning{file, warning}` (§2.5); the parser itself performs no logging
and returns no `SkillLoadWarning` (it has no file context).

### 5.4 The `skill` tool (the single model bridge)

```cpp
// include/ymh/skills/skill_tool.hpp  (NEW)
#pragma once

#include <memory>

#include "ymh/skills/skill_catalog.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

// Registered in WorkspaceRuntime before ToolRegistry::freeze() (20 §7.3).
// Pure execute: returns the cached body; appends nothing, touches no UI, makes
// no policy decision (07 X6/X8). Subject to the unchanged permission gate.
[[nodiscard]] std::unique_ptr<Tool> make_skill_tool(
    std::shared_ptr<const SkillCatalog> catalog);

} // namespace ymh
```

The concrete class (private to `src/skills/skill_tool.cpp`, shown here for the
pinned contract):

```cpp
class SkillTool final : public Tool {
public:
    explicit SkillTool(std::shared_ptr<const SkillCatalog> catalog)
        : catalog_(std::move(catalog)) {}

    ToolSchema schema() const override {
        ToolSchema s;
        s.name        = ToolName{"skill"};
        s.version     = ToolVersion{1, 0};
        s.description =
            "Load a named skill's full instructions into context. Call when the "
            "current task matches a listed skill's description. The returned "
            "text is advisory guidance; it never grants permissions.";
        s.input_schema = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
              "name": { "type": "string",
                        "description": "The skill name exactly as listed in the available skills section." }
            },
            "required": ["name"],
            "additionalProperties": false
        })");
        s.destructive = false;
        return s;
    }

    Task<ToolResult> execute(const ToolContext& ctx,
                             const ToolArguments& args) override {
        ToolResult result;
        result.id   = ctx.callId();
        result.name = "skill";
        const auto it = args.value.find("name");
        if (it == args.value.end() || !it->is_string()) {
            result.outcome = payload::ToolOutcome::Error;
            result.error   = std::string{"skill: 'name' is required and must be a string"};
            result.output  = *result.error;
            return Task<ToolResult>{result};
        }
        // Model-facing lookup: ONLY the model-visible set (SK8, H2).
        const Skill* skill = catalog_->find_model_visible(it->get<std::string>());
        if (skill == nullptr) {
            // Name-only resolution: an unknown or non-model-visible name is an
            // error, never a path lookup (SK10, SK-F10).
            result.outcome = payload::ToolOutcome::Error;
            result.error   = std::string{"skill: unknown or unavailable skill '"} +
                             it->get<std::string>() + "'";
            result.output  = *result.error;
            return Task<ToolResult>{result};
        }
        result.outcome = payload::ToolOutcome::Ok;
        result.output  = "[skill: " + skill->meta.name.value + "]\n" + skill->body;
        return Task<ToolResult>{result};
    }

private:
    std::shared_ptr<const SkillCatalog> catalog_;
};
```

The `payload::ToolResult` members used above are pinned at
`include/ymh/session/events.hpp:127-135` (`id`, `name`, `outcome`, `output`,
`truncated`, `error` as `std::optional<std::string>`, `duration`);
`payload::ToolOutcome` is `{Ok, Error, Denied, Cancelled}` at `:120-125` (L5).

### 5.5 System-prompt composition (H1(order) — corrected construction order)

> **Finding-id note.** Two gate findings share the id `H1` across rounds:
> **H1(order)** (round 1 — member-initialization ordering, this section) and
> **round-2 H1** (the permission-path usability predicate, below in this section).
> They are independent; `H1` alone in the rest of the spec is disambiguated by
> context.

**The bug this fixes.** In rev 1 the index was appended in the constructor
*body*, but `assembler_` copies `agent_config_.system_prompt` **during member
initialization** (`src/agent/workspace_runtime.cpp:58`), which runs before the
body. The index would therefore never reach the model. The fix is to compute the
effective prompt **inside a member initializer**, using a member that is
constructed before `agent_config_`.

**Member declaration order (the mechanism).** Members are initialized in
declaration order, not initializer-list order (`workspace_runtime.cpp:105-132`).
The current order is `environment_` (`:113`) … `agent_config_` (`:120`) …
`assembler_` (`:121`). This spec inserts exactly one member,
`std::shared_ptr<SkillCatalog> skill_catalog_;`, **between `environment_` (`:113`)
and `agent_config_` (`:120`)**, and initializes `agent_config_` through a helper
that reads `skill_catalog_->index_section()`:

```cpp
// src/agent/workspace_runtime.cpp — corrected additive wiring (H1).
// New member declaration (insert after environment_ at :113, BEFORE agent_config_):
//     std::shared_ptr<SkillCatalog> skill_catalog_;

namespace {  // file-local helpers, additive

// Build the catalog and run bounded discovery. Called from skill_catalog_'s
// initializer, which runs before agent_config_'s (declaration order).
std::shared_ptr<SkillCatalog> make_skill_catalog(const Config& config,
                                                 const ExecutionEnvironment& env,
                                                 Logger& logger) {
    auto catalog = std::make_shared<SkillCatalog>(
        to_skill_catalog_config(config), env, default_skills_root(), logger);
    if (catalog->config().enabled) {
        catalog->discover();          // all I/O happens here, once
    }
    return catalog;
}

// Is the model-invoked `skill` tool actually usable in this runtime? A tool
// whose verdict is `Ask` with no way to raise a prompt can never run.
//
// ROUND-2 H1 (the rev-2 regression). The predicate MUST NOT key on
// `attach_permission_gate`: that flag is M1-only. The live daemon sets it
// `false` (workspace_host.cpp:488) yet immediately installs a
// `PermissionBroker`-backed resolver (workspace_host.cpp:521-530), so an
// `attach_permission_gate`-only predicate disabled the tool on the interactive
// TUI path where it must work. Usability is a function of the runtime's ACTUAL
// permission path (agent_loop.cpp:380-408):
//   * `services_.gate` (the M1 in-process gate, workspace_runtime.cpp:85), or
//   * `services_.permission_resolver` (the M2 broker resolver the daemon
//     installs, workspace_host.cpp:523-530), or
//   * an explicit policy `Allow`.
// The caller passes whether such a prompt path will exist
// (`attach_permission_gate || attach_permission_resolver`); the resolver is
// attached after construction (`AgentRegistry::set_permission_resolver`,
// agent_registry.cpp:231-232), so it cannot be probed here.
bool skill_tool_usable(const PermissionPolicy& policy, bool prompt_path_available) {
    PermissionRequest probe;          // pure, total; no I/O (permission_policy.cpp:204)
    probe.tool = "skill";
    const PolicyVerdict verdict = policy.evaluate(probe);
    if (verdict == PolicyVerdict::Deny) {
        return false;                 // round-2 L4: an explicit Deny wins even with a prompt path
    }
    if (verdict == PolicyVerdict::Allow) {
        return true;                  // usable with no prompt path
    }
    return prompt_path_available;     // Ask: usable only if a prompt can be raised
}

// Compose the effective AgentConfig: base prompt + pinned skill index. The
// index is advertised ONLY when the tool is usable, so the model is never told
// about a capability it cannot invoke (SK17).
AgentConfig make_agent_config(const Config& config,
                              const SkillCatalog& catalog,
                              const PermissionPolicy& policy,
                              bool prompt_path_available) {
    AgentConfig agent = to_agent_config(config);
    if (skill_tool_usable(policy, prompt_path_available)) {
        const std::string index = catalog.index_section();
        if (!index.empty()) {
            agent.system_prompt += "\n\n";
            agent.system_prompt += index;
        }
    }
    return agent;
}

} // namespace
```

The initializer list then reads (only the two changed entries shown):

```cpp
        : /* … existing members through environment_ (:42-53) … */
          skill_catalog_(make_skill_catalog(config, *environment_,
                                            category_logger(LogCategory::Tool))),
          /* … existing tools_/policy_/gate_ … */
          agent_config_(make_agent_config(config, *skill_catalog_, policy_,
                                          attach_permission_gate ||
                                              attach_permission_resolver)),  // replaces :57
          assembler_(tools_, agent_config_.system_prompt),           // :58 — now correct
```

**Proof that the index is populated before the prompt is captured.**

1. `policy_` is declared at `:118`, `skill_catalog_` (new) at `:113a`,
   `agent_config_` at `:120`, `assembler_` at `:121` — so construction order is
   `environment_` → `skill_catalog_` → `policy_` → `agent_config_` → `assembler_`
   (declaration order governs, `workspace_runtime.cpp:105-132`).
2. `skill_catalog_`'s initializer calls `make_skill_catalog`, which calls
   `discover()` **before** the object is stored, so `index_section()` is final
   by the time any later member initializer runs.
3. `agent_config_`'s initializer calls `make_agent_config`, which reads
   `skill_catalog_->index_section()` and appends it to `system_prompt`.
4. `assembler_`'s initializer copies `agent_config_.system_prompt` — the composed
   value — into `SessionContextAssembler::systemPrompt_`
   (`src/agent/context_assembler.cpp:33-34`), which `assemble()` prepends
   (`:36-50`).

Therefore the index reaches the model. A static assertion or unit test pins the
ordering: `tests/unit/workspace_runtime_test.cpp` asserts that the assembled
system message contains the index for a workspace with one skill (T-H1 in §14.2).

**Tool registration (constructor body).** Because the body runs after all member
initializers, the single bridge tool is registered there, after the built-ins and
the terminal tool and before `McpManager::start()`/`freeze()`
(`src/agent/workspace_runtime.cpp:66-79`):

```cpp
        // additive, before tools_.freeze() at :79
        if (skill_catalog_->config().enabled &&
            skill_tool_usable(policy_, attach_permission_gate ||
                                           attach_permission_resolver)) {
            registrations_.push_back(tools_.add(make_skill_tool(skill_catalog_)));
        }
```

This is the minimal seam: the assembler still prepends exactly one `Role::System`
message built from `systemPrompt_` (`src/agent/context_assembler.cpp:36-50`), and
`/context`'s `SystemPrompt` segment (18 §3.3 item 1) therefore
includes the index (SK11, optional/§8.5). The compaction estimator, which runs on
the assembled messages (`src/agent/agent_loop.cpp:564`), also sees it — matching
18 §3.3's note that the threshold includes the system
prompt.

**Round-2 H1 — the usability predicate (fixed here).** `attach_permission_gate`
is an **M1-only** flag and is `false` on the daemon (`workspace_host.cpp:488`).
The live interactive path is `run_supervisor` (`src/ui/supervisor.cpp`, live per
`19-session-rename-errata.md:26-28`) attached to a workspace daemon; the daemon
does **not** use the owned gate but installs a `PermissionBroker`-backed resolver
immediately after building the runtime
(`runtime_->agents().set_permission_resolver(...)`, `workspace_host.cpp:521-530`;
`agent_registry.cpp:231-232`). `AgentLoop` therefore takes the
`services_.gate == nullptr` branch and, on `Ask`, calls
`services_.permission_resolver` (`src/agent/agent_loop.cpp:388-408`), which the
daemon routes to `broker_->resolve()` and thence to the attached supervisor's
prompt (`permission_broker.hpp:6-11`; the supervisor's `on_permission` sink
forwards it to the UI adapter, `supervisor.cpp:529-533`). **The `skill` tool is
therefore usable on the interactive TUI even though `attach_permission_gate` is
false.**

- **Interactive (live M2):** `prompt_path_available` is true (resolver attached);
  a default-`Ask` `skill` tool is registered and advertised, and the broker
  prompts on first use.
- **Headless `ymh run`:** the headless path sets neither the gate nor a resolver
  (`src/cli/headless.cpp:116-123`), so `agent_loop.cpp:403-405` denies with "no
  permission resolver attached" and an `Ask` fails closed. The tool is not
  registered and the index is not advertised — no dead weight, and the model is
  never told about an unusable capability.
- **Headless with an allow rule:** a synthetic `{tool:"skill"}` probe evaluates
  to `Allow`, so the tool is registered and the model may load skills.
- **Explicit `Deny`:** the predicate returns false even when a prompt path exists
  (round-2 L4), because the gate and the policy short-circuit `Deny`
  (`permission_policy.hpp:202-206`, `agent_loop.cpp:389-395`).
- The explicit `/skill` path is unaffected (it is a client injection, not a tool
  call).

> **Automation caveat.** A daemon serves both profiles; with zero Interactive
> subscribers the broker auto-denies (`permission_broker.hpp:13-21`, D19.3), so an
> automation-only `skill` call fails closed with a recorded `Deny` rather than
> hanging. The usability gate is a construction-time decision (a daemon may gain
> an Interactive subscriber later), so it treats any daemon that installs a
> resolver as prompt-capable.

### 5.6 Configuration

```cpp
// include/ymh/config/config.hpp  (additive)
struct SkillsSettings {
    bool        enabled = true;
    bool        expose_workspace = false;  // DANGER: exposes repo-authored skills
    std::size_t max_skills = 256;
    std::size_t max_skill_bytes = 64u * 1024u;
    std::size_t max_description_bytes = 512;
    std::size_t max_index_bytes = 8u * 1024u;
    std::size_t max_frontmatter_bytes = 4u * 1024u;
};
// Config gains:  SkillsSettings skills;
```

Parsed under `[skills]` with the strict loader (`reject_unknown`,
`src/config/config.cpp:27-45`), layered last-writer-wins
(`include/ymh/config/config.hpp:5-11`), env overrides `YMH_SKILLS_*`. A typo is a
`ConfigError`. `expose_workspace = true` is an explicit, documented opt-in and
should be surfaced by `/skills` with a warning.

### 5.7 Transport: two read-only methods + two host virtuals

```cpp
// include/ymh/transport/protocol.hpp (additive, in namespace method)
inline constexpr std::string_view kSkillsList = "skills.list";
inline constexpr std::string_view kSkillsShow = "skills.show";

// include/ymh/transport/host.hpp (additive pure virtuals on TransportHost)
virtual nlohmann::json listSkills() = 0;
virtual nlohmann::json showSkill(const std::string& name) = 0;
```

Wire contract:

```text
skills.list   params: {}
              result: { skills: [ { name, description, trust, source, version,
                                    tags, allowed_tools } ],
                        truncated, warnings: [ { file, reason } ], note }

skills.show   params: { "name": "<skill-name>" }
              result: { name, description, trust, source, version, tags,
                        allowed_tools, body }
```

Both are read-only, so `is_method_allowed` (`src/transport/protocol.cpp:641-647`)
permits them in **both** profiles without a deny-list entry — the same reasoning
spec 18 pins for `context.show` (18 §3.4). The catalog
grows by two (`src/transport/protocol.cpp:612-624`); see the ordering note in
§15.2 OQ-6 (spec 18 also proposes one).

Explicit activation reuses the **existing** `agent.inject` method
(`include/ymh/transport/protocol.hpp:500`), which already parses
`{role, text, starts_turn}` (`src/host/host_runtime.cpp:118-136`) and enqueues an
`Inject` item (`src/host/host_runtime.cpp:658-675`). No new activation method is
needed (20-D5).

**Daemon accessor (M4).** `HostRuntime` reaches the catalog through a new
`WorkspaceRuntime` accessor, declared next to `tools()`/`policy()`
(`include/ymh/agent/workspace_runtime.hpp:129-136`):

```cpp
// include/ymh/agent/workspace_runtime.hpp  (additive)
[[nodiscard]] SkillCatalog&       skills() noexcept;
[[nodiscard]] const SkillCatalog& skills() const noexcept;
```

`HostRuntime` stores `WorkspaceRuntime& runtime_` (`src/host/host_runtime.cpp:189`)
and already calls `runtime_.agent_config()`, `runtime_.environment()`, etc.; the
two new RPC handlers are therefore thin:

```cpp
// src/host/host_runtime.cpp  (additive sketch)
nlohmann::json HostRuntime::listSkills() {
    return skill_list_json(runtime_.skills());          // pure projection
}
nlohmann::json HostRuntime::showSkill(const std::string& name) {
    const Skill* skill = runtime_.skills().find(name);  // ALL skills (user tier
    if (skill == nullptr) {                             //  is not model-visible)
        throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                               "UnknownSkill"});
    }
    return skill_show_json(*skill);                     // includes `body`
}
```

`skills.show` uses `find()` (all discovered skills), **not**
`find_model_visible()` (H2): the user may inspect an untrusted workspace skill via
`/skills --show`, even though the model cannot load it. The model-facing tool
(§5.4) is the only caller of `find_model_visible()`.

`skill_list_json` and `skill_show_json` are **file-local** (`static`, anonymous
namespace) helpers in `src/host/host_runtime.cpp`; they are not part of the
pinned interface (L3).

**Independence (L4).** Both methods and the accessor depend only on
`SkillCatalog`; none references a spec-18 type, RPC, or `context_segment_token`.
Spec 20 is implementable and testable before 18 (20-D12).

### 5.8 UI surface

```cpp
// include/ymh/ui/command_registry.hpp  (additive to CommandContext)
// /skills [--show NAME] — list discovered skills (read-only, 20 §6).
std::function<void(const std::string& args)> skills;
// /skill NAME — load a skill's instructions into the active session (20 §7.2).
std::function<void(const std::string& name)> skill;
```

```cpp
// src/ui/command_registry.cpp, inside CommandRegistry::builtin() (additive)
registry.add(Command{
    "skills", "list discovered skills (--show NAME for detail)",
    [](CommandContext& context, const std::string& args) {
        if (context.skills) {
            context.skills(args);
        }
    }});
registry.add(Command{
    "skill", "load a skill's instructions into context",
    [](CommandContext& context, const std::string& name) {
        if (context.skill) {
            context.skill(name);
        }
    }});
```

`CommandRegistry::dispatch` (`src/ui/command_registry.cpp:91-111`) and Tab
completion (`:60-69`) are generic, so `/sk<Tab>` completes both new commands for
free — satisfying RB-05's RB-08 dependency (`REQUIREMENTS_BACKLOG.md:186,227-245`).

**Insertion point (L6).** In the current `builtin()` (`:113-196`), the `listed`
vector that `/help` iterates is snapshotted at `:183-186` **before** the `help`
command is registered at `:187-194`. The two new `registry.add(...)` calls must
be placed **before `:183`** (concretely: between the `export` command, which ends
at `:175`, and the `exit` command at `:176-182`). Placing them after `:186` would
register the commands but omit them from `/help`. This is the same constraint
that applies to any additive command on this surface (spec 18/19 note the shared
edit).

---

## 6. Listing — the `/skills` command

### 6.1 Behavior

`/skills` is a **read-only transcript listing**, not an overlay (20-D11).
Rationale: the listing is plain text with no per-cell color; the overlay pattern
was chosen for `/context` specifically because a color grid cannot be a
`ConversationEntry` (18 §4.2), and `UiMode` has no
`Context` value in the shipped code (`include/ymh/ui/ui_event.hpp:47-52`). A
transcript entry uses the existing `append_system` logic, exported as
`append_system_entry` (`src/ui/command_registry.cpp:23-33`, §6.1), and
`/help`'s established shape (`:183-194`), and avoids any collision with spec 18's
in-flight `UiMode::Context` addition.

- `/skills` → list every discovered skill (both tiers).
- `/skills --show <name>` → metadata detail for one skill (trust, source,
  version, tags, `allowed_tools`, and the body).
- `/skill <name>` → activate (load the body into the active session) — §7.2.
- `/skills` with no skills → a helpful empty-state message (§6.3).

Because the daemon owns discovery (SK15), `/skills` must fetch the list over RPC.
**The `/compact` path is NOT a model for this (H3).** `/compact`'s handler calls
`submit_to(..., nullptr)` — it passes no reply callback
(`src/ui/supervisor.cpp:907-915`) — and its outcome reaches the user through the
session event stream (the `ContextCompaction` event projected by the UI adapter),
plus an optimistic local `append_system("compaction requested")`
(`src/ui/command_registry.cpp:162`). `/skills` has no such event: its data is a
one-shot RPC reply, so a reply-append path must be designed explicitly.

**Pinned reply-append mechanism.** Two facts constrain it:

1. A `SupervisorConnection` reply callback runs on the connection's worker
   thread, not the UI thread (`src/ui/supervisor_connection.cpp:362`, where
   `request.reply(...)` is invoked after `connection_->request(...)` returns).
   The supervisor marshals every cross-thread model mutation through
   `enqueue(std::function<void()>)`, which pushes onto `actions_` under
   `action_mutex_` and posts an FTXUI `Custom` event
   (`src/ui/supervisor.cpp:488-496`); `drain()` runs the actions on the UI thread
   (`:788-808`). Every existing sink (`on_envelope`, `on_permission`,
   `on_notice`) uses exactly this wrapper (`:524-544`).
2. `append_system` is a file-local static in `command_registry.cpp`'s anonymous
   namespace (`:23-33`) and is not reachable from `supervisor.cpp`. It is
   therefore promoted to an exported helper:

```cpp
// include/ymh/ui/command_registry.hpp  (additive)
namespace ymh::ui {
// Appends a System conversation entry to `state` and marks it dirty. The
// non-static, callable-from-supervisor form of the existing file-local
// `append_system` (src/ui/command_registry.cpp:23-33).
void append_system_entry(UiModel& model, SessionUiState& state, std::string text);
} // namespace ymh::ui
```

```cpp
// src/ui/supervisor.cpp  (additive; /skills handler)
void request_skills(const std::string& args) {
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace == nullptr || workspace->activeSessionId.value.empty()) {
        return;
    }
    const WorkspaceId id = workspace->id;
    submit_to(id, std::string(protocol::method::kSkillsList), nlohmann::json::object(),
              [this, id](SupervisorReply reply) {
                  // reply runs on the connection worker thread → marshal to UI.
                  enqueue([this, id, reply = std::move(reply)] {
                      append_skills_reply(id, reply);   // UI thread: parses reply,
                  });                                    // builds text, calls
              });                                        // append_system_entry
}
```

`append_skills_reply` is a private supervisor method with the pinned signature
`void append_skills_reply(const WorkspaceId&, const SupervisorReply&);`. It runs
only inside the `enqueue` action (UI thread), formats the listing/warnings from
`reply.result`, resolves the active `SessionUiState`, and calls
`append_system_entry(model_, state, text)`. This is the same threading discipline
spec 17 §6 pins for model writes. The synchronous `export_session` return-a-notice
shape (`src/ui/command_registry.hpp:24-26`) is **not** usable here because the
reply is asynchronous; the `enqueue` wrapper is.

### 6.2 Rendering (pinned text shape)

```text
skills (3):
  git-commit        [user]       Write a conventional commit message from staged changes.
  release-notes     [user]       Draft release notes from git history.
  repo-conventions  [workspace]  This repo's naming and layering conventions.
activate with /skill <name>; detail with /skills --show <name>
```

- Name is left-aligned in a fixed 18-column field; tier tag is
  `[user]` (trusted) or `[workspace]` (untrusted).
- Descriptions are single-line (newlines collapsed to spaces) and bounded by
  `max_description_bytes`.
- Warnings, when present, follow:

```text
2 skills skipped:
  .ymh/skills/broken/SKILL.md: missing closing '---' fence
  .ymh/skills/dup/SKILL.md: shadowed by /home/me/.config/ymh/skills/dup/SKILL.md
```

- If `expose_workspace` is true, a leading `note: workspace skills are exposed to
  the model (skills.expose_workspace = true)` line is shown.

### 6.3 Zero skills

```text
no skills found.
add a skill at <workspace>/.ymh/skills/<name>/SKILL.md
or at ~/.config/ymh/skills/<name>/SKILL.md (trusted).
a skill file starts with '---', then 'name:' and 'description:'.
```

The empty state is **not** an error and appends no event. The `skill` tool is
registered only when it is usable (§5.5, SK17); the system-prompt index is empty,
so the model is never told about a nonexistent capability (SK8).

### 6.4 `/skills --show <name>`

Prints the metadata block plus the body (markdown-rendered by the existing
renderer, `src/ui/render/markdown_renderer.cpp`), bounded by
`max_skill_bytes`. `--show` is read-only and appends only the transcript entry.
`/skill <name>` (activation) is separate so that "look at it" and "load it into
the model's context" are never conflated.

---

## 7. Invocation — the crux

### 7.1 Decision: **both**, trust-asymmetric (20-D5)

| Path | Who triggers | Reachable tiers | Lookup | Mechanism | Durable record |
|---|---|---|---|---|---|
| **Model-invoked** | the model | model-visible (trusted; + workspace iff `expose_workspace`) | `SkillCatalog::find_model_visible()` | `skill` tool → `ToolCall`/`ToolResult` | `ToolCall` + `ToolResult` |
| **Explicit command** | the user (`/skill <name>`) | **any** discovered skill (both tiers) | `SkillCatalog::find()` | `agent.inject` → `ContextInjected` | `ContextInjected` |

Both paths converge on the same effect: skill text enters context. They differ in
who consents, how the text is recorded, and **which lookup is used** (H2): the
model-facing tool may only resolve `model_visible_`; the user-facing surfaces
(`/skill`, `/skills --show`, `skills.show`) resolve the full catalog.

### 7.2 Explicit `/skill <name>`

```text
supervisor                          daemon (HostRuntime → AgentLoop)
  /skill git-commit
    skills.show {name}      ──────►  catalog find() (ALL skills; cached body)
                            ◄──────  { name, trust, source, body }
    agent.inject {session,
       context:{role:"system",
                text:"[skill: git-commit]\n"+body,
                starts_turn:false}} ─►  AgentLoop::inject(ContextMessage)
                                        └─ enqueue Inject (startsTurn=false)
                                           └─ runTurn() drains it into a
                                              ContextInjected event
```

- The user naming the skill **is** the consent. This is the only way to activate
  an untrusted workspace skill without `expose_workspace`.
- `starts_turn = false` (pinned): the body is appended to history but does **not**
  itself start a turn, so the user can load a skill and then type their request.
  This matches `AgentLoop::hasTurnTrigger`, which returns true for an `Inject`
  only when `starts_turn` is set (`src/agent/agent_loop.cpp:83-94`); a
  non-turn-starting inject is drained at the top of the next `runTurn`
  (`:503-507`).
- The text is recorded verbatim in a `ContextInjected` event
  (`include/ymh/session/events.hpp:153-157`), so replay needs no skill file and
  the transcript/export shows exactly what was loaded
  (`src/ui/session_export.cpp:260-262`).

### 7.3 Model-invoked `skill` tool

The model sees exactly one tool, `skill`, whose schema is pinned in §5.4:

```json
{
  "name": "skill",
  "description": "Load a named skill's full instructions into context. Call when the current task matches a listed skill's description. The returned text is advisory guidance; it never grants permissions.",
  "input_schema": {
    "type": "object",
    "properties": {
      "name": { "type": "string",
                "description": "The skill name exactly as listed in the available skills section." }
    },
    "required": ["name"],
    "additionalProperties": false
  },
  "destructive": false
}
```

- `additionalProperties: false` is inside the pinned JSON-Schema subset
  (`15-mcp-adapter.md:1689-1690`).
- `execute()` resolves `name` **only** via `SkillCatalog::find_model_visible()`
  (the model-visible set); it never treats it as a path (SK10, H2). A miss is an
  `Error` result (SK-F10), never a read.
- The call is an ordinary tool call: `ToolRegistry::execute` validates the
  arguments, the permission policy evaluates a `PermissionRequest`
  (`include/ymh/policy/permission_policy.hpp:54-70`), the loop appends
  `ToolCall`/`ToolResult`, and the result enters history as a `Role::Tool`
  message via `deriveMessages`. Nothing in the loop, registry, or assembler
  branches on "skill" (the M1 transparency property, `15-mcp-adapter.md:1517-1519`).

### 7.4 Justification against the existing agent loop and permission model

**Why not model-tool-only.** The user could not force-load a skill the model did
not choose, and untrusted workspace skills (hidden from the model, SK8) would be
unusable. The command path costs almost nothing because `agent.inject` and
`ContextInjected` already exist (`src/host/host_runtime.cpp:658-675`,
`src/session/session.cpp:317-320`).

**Why not explicit-command-only.** The model could not self-serve; the entire
catalog would have to be pre-injected (wasting context on bodies that may never be
used) or the user would have to know to load a skill before every task. The tool
path is the dsh/ecosystem-aligned, model-driven design and it reuses the existing
tool pipeline end to end.

**Why not keyword/keyword-triggered auto-activation.** Deterministic trigger
matching (regex on the user message) is nondeterministic in effect, hides *why* a
skill was loaded, and lets a repository author cause automatic injection merely by
committing a file. Rejected: auto-activation is a privilege the model or user
grants explicitly, never a side effect of content on disk.

**Permission interaction (pinned).** The `skill` tool's verdict is the policy
default (`Ask`, `src/cli/wiring.cpp:71-78`). There is **no implicit allow**, the
same stance MCP pins (`15-mcp-adapter.md:1546-1549`, M7). An operator who wants
frictionless model-invoked skills writes an explicit rule
(`[[permissions.rule]] tool = "skill" effect = "allow"`). The `skill` tool is
`destructive = false`, but that is a hint only and does not change the verdict
(`include/ymh/policy/permission_policy.hpp:69`). `allowed-tools` frontmatter is
**never** consulted by the policy (SK13, 20-D7).

**Headless / interactive consequence (round-2 H1, resolved).** The usability
predicate keys on the runtime's actual permission path, **not** on the M1-only
`attach_permission_gate` flag (§5.5). The daemon sets `attach_permission_gate =
false` (`workspace_host.cpp:488`) but installs a `PermissionBroker` resolver
(`workspace_host.cpp:521-530`), and the headless path sets neither
(`headless.cpp:116-123`). Consequences:

- **Interactive TUI (live M2):** the resolver is attached, so the tool is usable;
  the broker prompts the supervisor on first use.
- **Headless with an allow rule:** the tool is usable; the model may load skills.
- **Headless without an allow rule:** `Ask` fails closed
  (`agent_loop.cpp:403-405`); the tool is not registered and the index is not
  advertised — the model never sees skills. `/skills` listing still works (it is
  an RPC read).
- **Explicit `Deny`:** never registered, even with a resolver (round-2 L4).
- `/skill` (explicit) is a client-side injection and is unaffected by the gate,
  but the headless `ymh run` entry point has no interactive command line, so
  explicit activation is an interactive-only affordance.

---

## 8. Context injection and token accounting

### 8.1 The catalog index (always present, pinned)

For every **model-visible** skill, the effective system prompt gains a bounded
block (built by `SkillCatalog::index_section()`):

```text
Available skills (load a skill's full instructions with the `skill` tool):
- git-commit: Write a conventional commit message from staged changes.
- release-notes: Draft release notes from git history.
(12 more skills not shown; run /skills)
```

- The block is appended to `agent_config_.system_prompt` before the assembler is
  constructed (§5.5), so it is part of the single `Role::System` message the
  assembler prepends (`src/agent/context_assembler.cpp:36-50`).
- It is therefore **outside the session projection** and is **never summarized**:
  spec 13 §3.4/C15 pins that the assembler-added system prompt is "excluded from
  `compacted_context` and from `tokenEstimate`" and "re-added on every assembly"
  (`13-context-compaction.md:413-416`, `:926-935`). The index is thus **pinned**
  (SK11).
- Cost is bounded by `max_index_bytes` (8 KiB). Over-cap is truncated at a
  line boundary with the `(N more skills not shown)` note (SK-F12); the
  truncated skills remain individually invocable via `/skill` and the `skill`
  tool (the model simply is not advertised them).

### 8.2 The body on invocation (compactable)

- **Tool path.** The body is the `ToolResult.output`; the loop appends it
  (`src/agent/agent_loop.cpp:350-461`) and `deriveMessages` projects it as a
  `Role::Tool` message. It is an ordinary projected message with an origin
  `Sequence`, so a later compaction whose boundary is ≥ that sequence **drops
  it** (`src/session/session.cpp:322-337`; `13-context-compaction.md:494-513`).
- **Command path.** The body is a `ContextInjected` event, projected to a
  `Role::System` text message (`src/session/session.cpp:317-320`), also with an
  origin `Sequence`; a later compaction drops it the same way.

**Is a loaded body compacted?** Yes — deliberately. Once the body has been
summarized away, the index entry remains, so the model can re-invoke the tool if
it still needs the detail. Pinning every loaded body would make context growth
unbounded and defeat compaction. The **index** is pinned; the **bodies** are not
(SK11).

### 8.3 Token accounting

| Segment | Where it is counted | Citation |
|---|---|---|
| Skill **index** | inside the `SystemPrompt` segment (18 §3.3 item 1), and inside the loop's threshold estimate `E(messages)` | 18 §3.3; `src/agent/agent_loop.cpp:563-569` |
| Body loaded by the `skill` tool | `Conversation` segment as a `Role::Tool` message (18 §3.3 item 5) | 18 §3.3 |
| Body injected by `/skill` | `Conversation` segment as a `Role::System` message | 18 §3.3; `src/session/session.cpp:317-320` |

Bodies are bounded by `max_skill_bytes` (64 KiB) at discovery and by the tool
result clamp at execution (`clamp_tool_result`, declared at
`include/ymh/tools/tool.hpp:82`; `ToolConfig::tool_result_max_bytes`,
`include/ymh/execution/config.hpp:14-17`). The `skill` tool never produces an
unbounded payload.

The `18 §3.3` rows above are **optional** (L4): they describe how `/context` will
report the tokens once spec 18 lands. Independently of 18, the accounting is
observable directly: the index is inside `systemPrompt_` (`context_assembler.cpp:33-34`,
`:40-48`), so `DefaultTokenEstimator::estimate(assemble(...))`
(`context_assembler.cpp:27-31`) includes it; a loaded body is an ordinary
projected message and is counted by the same estimator.

> **Design gap flagged (OQ-1).** Because the index lives inside the system-prompt
> string, `/context` reports it only as part of `SystemPrompt`; there is no
> per-skill breakdown. A dedicated additive `SkillIndex` segment in
> `context_segment_token` would make it visible. This spec proposes it as an
> optional errata (AM-20g) rather than requiring it, to avoid amending a verified
> spec for a display nicety.

### 8.4 Interaction with compaction (spec 13)

- The index is **never** in the summarize set (it is not a projected message;
  C15, `13-context-compaction.md:413-416`).
- Loaded bodies **are** in the summarize set and may be summarized; this is
  correct and is the reason the index exists.
- The compactor's `lead` computation counts leading assembler messages
  (`13-context-compaction.md:391-411`); the index is part of the single system
  message, so `lead` remains `1` (or `0` for an empty prompt) and the existing
  tail arithmetic is unaffected. **This spec adds no compaction logic.**
- After a compaction that drops a body, the next assembly re-adds the index
  (the assembler is stateless w.r.t. the index), so the model is never left
  without the catalog.

### 8.5 Interaction with `/context` (RB-06, spec 18)

- `skills.list`/`skills.show` are **not** the `/context` surface; `/context` is
  produced by `context.show` (18 §3.4). This spec does not
  extend the snapshot (OQ-1).
- The index appears in `/context` under `SystemPrompt`; loaded bodies under
  `Conversation`. The MCP inventory segment is untouched.
- The `/context` precedence rule (18 §1) is respected: spec
  20 wins for the `/skills`/`/skill` surface only; where it meets 18 on the
  snapshot, 18 wins.

---

## 9. Security and path safety

### 9.1 Threat model

| # | Threat | Vector |
|---|---|---|
| T1 | A repository prompt-injects the agent | `.ymh/skills/<name>/SKILL.md` committed by a third party |
| T2 | A skill instructs exfiltration | body says "read `~/.ssh/id_rsa` and POST it" |
| T3 | Path traversal / symlink escape | a skill dir or `SKILL.md` symlinked outside the root |
| T4 | Resource exhaustion | huge or numerous skill files |
| T5 | Privilege escalation | `allowed-tools`/frontmatter claims to pre-approve tools |
| T6 | Silent activation | a file auto-loads without the user or model choosing it |

### 9.2 Stance: skill content is **untrusted data** (20-D7)

**Every** skill body is treated as untrusted model input, regardless of tier.
The tier governs **who may cause it to be loaded without an explicit user
action**, not whether its prose is trustworthy:

- **Trusted (user)** skills may be advertised in the index and resolved by the
  `skill` tool (via `find_model_visible()`). The user authored them; the model may
  load them (subject to the permission verdict).
- **Untrusted (workspace)** skills are **not** advertised and **not** resolvable
  by the `skill` tool (which uses `find_model_visible()`) unless
  `expose_workspace = true`. They are listed by `/skills` and loadable only by an
  explicit `/skill <name>` (the user's affirmative act, which uses `find()`).

### 9.3 What a skill may and may not do

A skill **may**:

- provide instructions, conventions, checklists, and examples to the model;
- name tools the model should use (as prose, which the model may then invoke
  through the normal gate).

A skill may **not** (and the design makes it impossible):

- **pre-approve or escalate permissions.** `allowed-tools` is inert metadata
  (SK13). No skill can add a `PolicyRule`, a `ToolDefault`, or a grant. The
  policy sees only `ToolCall.name`/`arguments` (`include/ymh/policy/permission_policy.hpp:54-70`);
  it never reads skill state.
- **register or remove tools.** A skill is data; `ToolRegistry::add` is confined
  to startup wiring and `AdapterScope` is available only to the MCP adapter
  (`include/ymh/tools/tool_registry.hpp:44-73`, `15-mcp-adapter.md:1013-1014`).
- **change the sandbox mode.** `SandboxMode` is immutable per environment and
  "selected by config, never by the model"
  (`include/ymh/execution/environment.hpp:35-37`).
- **read outside allowed roots.** See §9.4.
- **execute anything.** A skill is text; only the model's subsequent tool calls
  execute, and those pass through the permission policy and the execution
  environment (`00-architecture.md:4331-4357`, D7).
- **self-activate.** No file on disk causes loading (T6): the index only
  *advertises*, and loading requires either the user's `/skill` or the model's
  `skill` tool call (which is permission-gated).

### 9.4 Path safety (pinned)

- **Workspace tier** reads go through `ExecutionEnvironment::resolve()`
  (`include/ymh/execution/environment.hpp:33`), which is root-relative,
  realpath-canonical, and throws `PathEscape` on escape
  (`src/execution/environment.cpp:60-91`). Discovery also applies
  `path_is_within()` (`:32-35`) to reject symlink escapes before reading.
- **User tier** is outside the workspace, so `resolve()` cannot be used
  (`:73-76`). It is read with `weakly_canonical` + `path_is_within` rooted at the
  XDG config dir, mirroring the config loader's own out-of-workspace read
  (`src/config/config.cpp:579-587`). It is **read-only**; the daemon never writes
  there.
- **User root must be absolute (M2).** A relative user root is *disabled*, never
  canonicalized against the daemon cwd: `default_global_config_path()` can return
  a relative path (`src/config/config.cpp:579-587`) and the daemon chdirs to the
  workspace root (`src/host/workspace_host.cpp:464-467`). Detection:
  `user_root.is_absolute()`; on false the user tier is empty and one
  `SkillLoadWarning` is recorded (§3.1, SK-F13). This closes the hole where a
  workspace could plant files that provenance would classify `Trusted`.
- **No `chdir()`** anywhere in the subsystem (D18, `00-architecture.md:4810`).
- **Name-only resolution at call time.** The `skill` tool argument is a
  `SkillName` looked up in the catalog; it is never concatenated into a path
  (SK10). Even if a lookup succeeded, the body is already in memory, so there is
  no call-time filesystem access at all (SK6) — eliminating TOCTOU between
  discovery and use.
- **Supporting files are never read.** Only `SKILL.md` is loaded. A body may
  *mention* `template.md`, but the model must invoke a normal tool (subject to the
  gate and `resolve()`) to read it. This prevents a skill directory from becoming
  a covert multi-file payload the user never inspected (T1 mitigation).

### 9.5 Logging and redaction

Skill **bodies** are never written to normal logs. Startup logs and warnings
record only names, tiers, byte counts, and file paths (`§40`,
`AGENTS.md:64-65`; the MCP redaction rule, `15-mcp-adapter.md:1572-1574`, M12).
The body is durable only where it must be: in a `ToolResult` or a
`ContextInjected` event in the session log (the authoritative trace, D2).

### 9.6 Bounds as DoS mitigation

`max_skills`, `max_skill_bytes`, `max_frontmatter_bytes`, `max_description_bytes`,
and `max_index_bytes` (§3.4) bound discovery time, memory, and prompt cost. A
repository cannot make the daemon allocate without bound. Discovery is O(number
of files) and does no network or subprocess work.

---

## 10. Interaction with the tool registry and MCP

### 10.1 Skills are not tools

A skill is **not** registered in `ToolRegistry`; only the single `skill` bridge
tool is. Consequently:

- Skill **names** never enter `ToolName` space and cannot collide with a tool
  (SK10). A skill named `read_file` is not the `read_file` tool.
- The model's tool list changes by exactly one entry (`skill`) when skills are
  enabled, keeping the tool set stable across workspaces.
- The frozen-registry rule (`X5`, `include/ymh/tools/tool_registry.hpp:79-82`) is
  untouched: `skill` is registered before `freeze()` in the existing wiring loop
  (`src/agent/workspace_runtime.cpp:66-79`).

### 10.2 A skill cannot bundle tools (rejected for v1)

**Rejected:** letting a skill declare new tools (or an MCP server) in its
frontmatter. A skill is world-authored data; letting data register executable
capabilities would be a privilege escalation, would bypass the operator's config
review, and would break the registry's "registration confined to plugin load /
daemon startup" invariant (`include/ymh/tools/tool_registry.hpp:3-6`, X5).
If a skill needs a tool, the tool is registered by the daemon (built-in or MCP)
and the skill merely names it in prose.

### 10.3 Ordering and conflict rules

- Registration order in `WorkspaceRuntime::Impl` is: built-ins, terminal (if PTY),
  **`skill`**, then `McpManager::start()`, then `freeze()`
  (`src/agent/workspace_runtime.cpp:66-79`). The `skill` tool must be added
  before `freeze()`; the exact position among non-adapter tools is immaterial
  because `ToolRegistry::schemas()` is required to be deterministic (M13,
  `15-mcp-adapter.md:1576-1578`).
- MCP tools are namespaced `mcp.<server>.<tool>` (`15-mcp-adapter.md:344-359`), so
  there is no collision with `skill`. The `AdapterScope` disjointness rule
  (`include/ymh/tools/tool_registry.hpp:98-104`) prevents any adapter from
  shadowing `skill` or `skill.*`.
- If a future adapter opens a `skill.` prefix, the disjointness check rejects it;
  skills deliberately do **not** occupy a `skill.` tool namespace in v1.

### 10.4 Relationship to MCP

Skills and MCP are orthogonal capability sources with the same integration
philosophy (adapt external capability into the shared registry) but opposite
substrate:

| | MCP (`15`) | Skills (`20`) |
|---|---|---|
| Capability | executable remote tools | advisory prompt text |
| Registration | `ToolRegistry::AdapterScope` (post-freeze replace) | one static `skill` tool (pre-freeze) |
| Trust | server output is untrusted | body is untrusted data (§9.2) |
| Durable trace | `ToolCall`/`ToolResult` | `ToolCall`/`ToolResult` **or** `ContextInjected` |
| Discovery | daemon startup, async handshake | daemon startup, synchronous filesystem |
| Failure | skip tool / fail loud (MCP-F4/F8) | skip + warn (SK-F1…F9) |

A skill may describe how to use an MCP tool; it cannot enable, configure, or
bypass one.

---

## 11. Invariants

Numbered, testable, cited. Any code that can violate one is a defect.

**SK1 — Skill is data, not policy.** A skill's only runtime effect is text in
model context. It cannot register a tool, alter permissions or grants, change the
sandbox, read a file, or execute anything. (§9.3, `00 §46`, D4/D7)

**SK2 — Provenance-derived trust.** `SkillTrust` is a function of the search root,
never of frontmatter or content. The user root must be **absolute**; a relative
user root disables the user tier rather than being resolved against the daemon
cwd (SK-F13). (`§2.4`, `§3.1`, `§9.4`)

**SK3 — No cross-tier shadowing.** A workspace skill whose name equals a user
skill's name is rejected; the trusted skill always wins. (`§3.2`, SK-F5)

**SK4 — Deterministic discovery.** For a fixed filesystem and config, the catalog
order and contents are deterministic: user tier before workspace, then byte-wise
`SkillName`, then canonical path. (`§3.6`, M13-style)

**SK5 — Root-confined, symlink-safe reads.** Discovery reads only within the two
roots; workspace reads go through `resolve()`; a symlink may not leave its root;
`chdir()` is never called. (`§3.3`, `§9.4`, X1–X4, D18)

**SK6 — Eager bounded load.** Bodies are read once at discovery and cached;
`execute()` performs no filesystem I/O. Memory is bounded by
`max_skills × max_skill_bytes`. (`§4.1`, `§5.4`, `§9.4`)

**SK7 — Skip-and-warn on malformed files.** A malformed skill file/directory is
skipped with a recorded warning; it never fails daemon startup. Malformed
`[skills]` config is a hard `ConfigError`. (`§2.5`, `§3.5`)

**SK8 — Trust-gated model visibility.** Only trusted skills (plus workspace skills
when `expose_workspace = true`) appear in the system-prompt index and resolve in
the `skill` tool. (`§7.3`, `§9.2`)

**SK9 — One bridge.** The `skill` tool is the only model-reachable skill surface;
it is an ordinary `Tool` registered before `freeze()`, subject to the unchanged
permission policy. (`§7.3`, `§10.1`)

**SK10 — Name-only resolution.** The tool argument is a `SkillName` looked up in
the catalog; it is never a path, so traversal through it is impossible. The
model-facing tool uses only `find_model_visible()`; the user-facing surfaces use
`find()` over all discovered skills. (`§5.2`, `§7.1`, `§7.3`, `§9.4`)

**SK11 — Index pinned, bodies compactable.** The index is part of the assembler's
system prompt (outside the projection) and is never summarized; loaded bodies are
ordinary projected messages and may be compacted. (`§8.1`, `§8.2`, `13` C15)

**SK12 — Durable, replayable activation.** Activation records either
`ToolCall`/`ToolResult` (tool path) or `ContextInjected` (command path); replay
needs no skill files. (`§7.2`, `§7.3`, D2)

**SK13 — No implicit permission.** The `skill` tool's default verdict is the
policy default (`Ask`); `allowed-tools` never grants, and no skill can add a rule
or grant. (`§7.4`, `§9.3`, `09` M7-style)

**SK14 — Bounded prompt and payload cost.** The index is bounded by
`max_index_bytes`; a body is bounded by `max_skill_bytes` and the tool-result
clamp. (`§3.4`, `§8.3`)

**SK15 — Daemon ownership.** Exactly one `SkillCatalog` per workspace daemon; the
supervisor never discovers or reads skill files. (`§1.2`, `§5.7`, D18/16-D1-style)

**SK16 — Redaction.** Skill bodies are never written to normal logs; only names,
tiers, byte counts, and paths. (`§9.5`, `§40`)

**SK17 — No dead capability.** The `skill` tool is registered and advertised iff
the runtime can actually execute it: its `{tool:"skill"}` policy verdict is not
`Deny`, and either the verdict is `Allow` or a prompt path is attached (the M1
gate or the M2 broker resolver). (`§5.5`, `§7.4`)

---

## 12. Failure modes

### 12.1 Shared findings (F1–F12, `§54`)

| F# | Finding | Skill handling |
|---|---|---|
| **F1** | path/process isolation | workspace reads via `resolve()`; absolute user root read-only under the XDG dir (relative ⇒ user tier disabled, SK-F13); no `chdir()`, no subprocess (§9.4, SK5) |
| **F2** | background permission | the `skill` tool is gated like any tool; a background `Ask` is surfaced by the unchanged broker/attention (`09 §5`, SK13) |
| **F3** | late event after close | activation appends only through the loop's normal path; a disposed agent's `inject` is rejected (`AgentDisposed`), never appended (`06` A14) |
| **F4** | edge-triggered attention | skills emit no live status; a permission prompt for `skill` uses the existing attention path |
| **F5** | output ring buffers | bodies are bounded and clamped (§8.3, SK14) |
| **F6** | input/keybinding focus | out of scope; skills never see focus |
| **F7** | per-session dirty flags | out of scope; the catalog is daemon-global, not per-session |
| **F8** | resource caps | `max_skills`/`max_skill_bytes`/`max_frontmatter_bytes` (§3.4, SK14) |
| **F9** | cancellation scoping | the `skill` tool is instant and side-effect-free; it observes no long operation (no cancellation surface) |
| **F10** | resume-suspended | the catalog is daemon-lifetime; resuming a session never re-reads files (SK6) |
| **F11** | subagent ID duality | a subagent shares the daemon's catalog; its `skill` calls carry its own `ToolCallId` |
| **F12** | flash clock in model | out of scope; skills emit no flash state |

**Explicitly out of scope:** F6, F7, F12 (UI projection); F4's flash arithmetic
(spec 10); F2's policy rules (spec 09).

### 12.2 Component-local failure modes (`SK-F1`–`SK-F13`)

Component-local to the skills subsystem; each must be covered by tests (§14.4).

| SK-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **SK-F1** | Malformed frontmatter (missing/duplicate/unknown key, unsupported construct, missing closing fence, over-long fence region, bad `version`) | `parse_skill_file()` returns a non-empty `error` | skip the skill; record `SkillLoadWarning{file, reason}`; daemon starts (SK7) |
| **SK-F2** | Missing required field (`name` or `description`) | parser error | skip + warn |
| **SK-F3** | `name` ≠ directory basename, or invalid `SkillName` | validation after parse | skip + warn |
| **SK-F4** | `description` over `max_description_bytes` | length check | truncate at a word boundary + warn; skill stays usable |
| **SK-F5** | Workspace skill shadows a user skill | catalog name index | skip the workspace skill + warn naming the trusted file (SK3) |
| **SK-F6** | Duplicate name within a tier (case-folded FS / symlink) | catalog name index | first in deterministic order wins; later skipped + warn (SK4) |
| **SK-F7** | Symlink escape / non-regular `SKILL.md` / root not a directory | `weakly_canonical` + `path_is_within`; `is_regular_file` | skip + warn (SK5) |
| **SK-F8** | Oversized body (> `max_skill_bytes`) | size check before load | skip + warn; do not partially load |
| **SK-F9** | Too many skills (> `max_skills`) | count during scan | keep the first `max_skills` in deterministic order; skip the rest + warn (SK14) |
| **SK-F10** | `skill` tool called with unknown / non-model-visible name, or non-string arg | catalog lookup / argument validation | `ToolResult` `Error`; **no** path resolution or file read (SK10) |
| **SK-F11** | Skills root absent or unreadable | directory probe | empty catalog for that tier + one warning; daemon starts (never a hard error) |
| **SK-F12** | Index over `max_index_bytes` | `build_index` length check | truncate at a line boundary; append `(N more skills not shown)`; skills remain individually invocable (SK11/SK14) |
| **SK-F13** | User-skills root is relative (so it would resolve under the daemon cwd and be mis-classified `Trusted`) | `user_root.is_absolute()` before the user-tier scan | disable the user tier (empty) + one warning; never canonicalize against the cwd; workspace tier unaffected (SK2, M2) |

---

## 13. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (`00 §55`), and its design
explicitly lists **skills** among the things modeled as plugins
(`docs/design/00-architecture.md:4864`). Skills map onto it as follows.

| dsh concept | ymh skills | Reference |
|---|---|---|
| Plugin contributes prompt sections | skill index appended to the effective system prompt | `00 §7.2` ("contribute prompt sections"), `§31` |
| Skills as plugins | `SkillCatalog` (discovery + immutable store) + the `skill` tool | `00:4864` |
| Tool registry | `ToolRegistry` (unchanged; one additive `skill` tool) | `07 §4`, §5.4 |
| Tool | `SkillTool : Tool` | `07 §3.1`, §5.4 |
| Tool invocation context | `ToolContext` (unchanged) | `07 §5` |
| Injected context | `ContextInjected` (unchanged) for the `/skill` path | `00 §31`, §7.2 |
| Configuration | `[skills]` in the layered config | `00 §37`, §5.6 |
| Append-only trace | `ToolCall`/`ToolResult` or `ContextInjected`; catalog never appends | D2, §7.2/§7.3 |
| Plugin lifecycle | static discovery at daemon startup; no hot reload, no `dlopen()` | `00 §7.1`/`§7.3`, D11, 20-D4 |
| Capability provider seam | the catalog is a read-only capability source, not a runtime | `00 §7`, `§51` (phase 2) |

**Deliberate omissions (accepted for v1):** no built-in skills (OQ-3), no hot
reload (OQ-2), no skill-bundled tools or MCP servers (§10.2), no automatic
keyword activation (§7.4), no per-repo override of a trusted skill (§3.2), no
separate `/context` segment (§8.5, OQ-1), no remote/SSH skill roots.

---

## 14. Test plan

Strategy is `§44`: unit, integration (`FakeLLM`), golden/replay, and a separate
live opt-in layer. The deterministic layers run offline; the live layer is opt-in
and gated (`00-architecture.md:4197-4290`, `§45`).

### 14.1 Unit tests (hermetic, no LLM, no daemon)

**Frontmatter parser (`parse_skill_file`)**

- valid file with all fields → every field parsed; `body` equals the text after
  the closing fence (leading blank lines trimmed).
- missing opening/closing fence, fence over `max_frontmatter_bytes`, duplicate
  key, unknown key, nested map, block scalar, anchor, tag, bad `version` type,
  CRLF input, UTF-8 BOM → each returns the expected error and does **not** throw
  (SK-F1).
- `[a, b, c]` flow sequence with and without spaces; quoted scalars with `:` and
  `,`; empty value → `""`.
- `description` over `max_description_bytes` → truncated at a word boundary and a
  warning is produced (SK-F4); the skill is still returned by `scan()` and present
  in `all()` (M1 — a non-fatal warning never drops a skill).
- `name` ≠ `expected_name` → error (SK-F3).
- Fuzz corpus: 10k random byte strings never crash and always return a result
  (no exception, bounded output).

**`SkillName`**

- grammar table: valid kebab-case accepted; uppercase, leading/trailing `-`,
  `--`, `_`, `.`, `/`, empty, > 64 chars rejected.

**Catalog discovery**

- two roots with disjoint names → both discovered, user first (SK4).
- cross-tier same name → workspace skipped, warning names the user file (SK-F5,
  SK3).
- same-tier case-folded collision → first in order wins (SK-F6).
- symlinked dir → inside root accepted, escaping root skipped (SK-F7, SK5).
- `SKILL.md` a symlink escaping the root, a FIFO, or a directory → skipped
  (SK-F7).
- body over `max_skill_bytes` → skipped (SK-F8); > `max_skills` → first N kept
  (SK-F9).
- absent/unreadable root → empty tier + one warning, no throw (SK-F11).
- relative user root → user tier disabled + one warning; the workspace tier is
  unaffected and no skill is read from the relative path (SK-F13, M2).
- determinism: two `discover()` runs on the same tree produce byte-identical
  `all()` and `index_section()` (SK4).
- `index_section()` over `max_index_bytes` → truncated at a line boundary with
  the `(N more…)` note (SK-F12).

**`skill` tool**

- unknown name → `Error` result; assert no filesystem call occurred (SK-F10,
  SK10).
- non-string / missing `name` → `Error` (SK-F10).
- workspace skill with `expose_workspace = false` → `Error` (SK8).
- valid trusted name → `Ok` with `[skill: <name>]` header and the exact body
  (SK6).
- **Two-lookup separation (H2).** With a workspace-only skill `x` and
  `expose_workspace = false`: `catalog.find("x") != nullptr` (the user surfaces
  can see it) **and** `catalog.find_model_visible("x") == nullptr`; the `skill`
  tool returns `Error`. With `expose_workspace = true`, both resolve and the tool
  returns `Ok`.
- schema validates under the pinned JSON-Schema subset; `destructive == false`.

### 14.2 Integration tests (FakeLLM, hermetic)

Use `FakeLLM`/`FakeScript` (`include/ymh/llm/fake_llm.hpp:24-63`) and the
`AgentEnv` harness (`tests/support/agent_test_env.hpp:66+`).

- **T-H1 — construction-order regression (H1).** Build a `WorkspaceRuntime` (via
  `make_workspace_runtime`, `include/ymh/agent/workspace_runtime.hpp:160-162`)
  over a workspace containing exactly one trusted skill, with the gate attached.
  Assert that the **first** assembled `LLMRequest.messages[0]` is a `Role::System`
  message whose text contains the skill name and description. This test fails
  against the rev-1 ordering (index appended in the ctor body) and passes against
  the corrected member-initializer order (§5.5). It is the single most important
  regression test in this plan.
- **H1 — usability gating (interactive vs headless).** With the default `Ask`
  verdict:
  1. **Interactive/daemon:** with `attach_permission_resolver = true` (the daemon
     path, `workspace_host.cpp:488,521-530`) — or `attach_permission_gate = true`
     (the M1 path) — assert the tool **is** registered and the index **is**
     present; a `{name:"skill"}` step then reaches the resolver/broker (not a
     fail-closed `Deny`).
  2. **Headless:** with neither attached (`headless.cpp:116-123`), assert
     `runtime.tools().contains(ToolName{"skill"}) == false` and the assembled
     system message does **not** contain the skill index (Ask with no prompt
     path).
  3. **Explicit `Deny`:** with a `{tool:"skill", effect:"deny"}` rule, assert the
     tool is **not** registered even when a resolver is attached (round-2 L4).
  4. **Explicit `Allow`:** with `{tool:"skill", effect:"allow"}`, assert the tool
     is registered with no prompt path.
- **Tool path, permission-gated.** Script a step
  `{name:"skill", arguments:{name:"git-commit"}}` with `FakeLLM`. Assert:
  1. a `PermissionDecision` is recorded **before** the `ToolResult` (SK13);
  2. with an allow rule, `ToolResult.outcome == Ok` and `output` contains the
     body;
  3. the **next** `LLMRequest.messages` contains a `Role::Tool` message whose
     text contains the body (context injection, §8.2).
- **Untrusted not resolvable.** Same script with a workspace-only skill and
  `expose_workspace = false` → `ToolResult.outcome == Error`; assert the body
  never appears in any `LLMRequest`.
- **Command path.** Call `AgentLoop::inject(ContextMessage{Role::System,
  "[skill: git-commit]\n"+body, startsTurn=false})`, then `send` a user prompt.
  Assert: a `ContextInjected` event is durable; the assembled
  `LLMRequest.messages` has the injected `Role::System` text before the user
  message; no turn was started by the injection alone (`hasTurnTrigger` false for
  a non-turn-starting inject, `src/agent/agent_loop.cpp:83-94`).
- **Compaction drops the body, keeps the index.** Load a body, then force a
  compaction whose boundary is after the body's sequence. Assert:
  `deriveMessages()` no longer contains the body text; the assembled system
  message still contains the index section (SK11, §8.4).
- **Replay without skill files.** Record a session that used the `skill` tool,
  delete the skill directory, replay: assert identical `deriveMessages()`
  (SK12).
- **`/skills` RPC.** `HostRuntime::listSkills()` returns the catalog in
  deterministic order with trust/source; `showSkill` returns the body;
  `showSkill` for an unknown name returns a mapped error, never a path read.
- **Token accounting — spec-18-independent (L4).** Assert directly that
  `DefaultTokenEstimator::estimate(assembler.assemble(...))`
  (`src/agent/context_assembler.cpp:27-31`, `:36-50`) grows by the index bytes
  when a skill is present, and grows again after a `skill` tool result / a
  `ContextInjected` body. This runs **without** spec 18.
- **(Optional, gated on spec 18) `/context` accounting.** If `context.show`
  exists, assert its `SystemPrompt` segment token count reflects the index and
  its `Conversation` segment grows after a body load. Mark this test
  `SKIP_IF_NO_SPEC18`; it must **not** block spec 20 verification (20-D12, L4).

### 14.3 Golden / PTY tests

- **Golden `/skills` output** for: 0 skills (§6.3), 3 skills across tiers
  (§6.2), with warnings, with `expose_workspace` note.
- **PTY**: type `/sk<Tab>` → completes to `/skills` and `/skill`
  (`src/ui/command_registry.cpp:60-69`); run `/skills` and assert the rendered
  transcript contains the names and tier tags; run `/skill <name>` then a prompt
  and assert (via the hermetic harness) that the body reached the assembled
  request.
- **PTY zero-state**: `/skills` in an empty workspace renders the §6.3 guidance.

### 14.4 Failure-mode coverage matrix

| SK-F# | Unit | Integration | PTY/Golden |
|---|---|---|---|
| SK-F1 | ✓ | | |
| SK-F2 | ✓ | | |
| SK-F3 | ✓ | | |
| SK-F4 | ✓ | | |
| SK-F5 | ✓ | | |
| SK-F6 | ✓ | | |
| SK-F7 | ✓ | | |
| SK-F8 | ✓ | | |
| SK-F9 | ✓ | | |
| SK-F10 | ✓ | ✓ | |
| SK-F11 | ✓ | | |
| SK-F12 | ✓ | | |
| SK-F13 | ✓ | | |

### 14.5 Invariant coverage

| Invariant | Covered by |
|---|---|
| SK1, SK3, SK13 | §14.1 tool/catalog; §14.2 untrusted-not-resolvable |
| SK2, SK4 | §14.1 determinism + precedence (SK2 also §14.1 relative-user-root) |
| SK5, SK7 | §14.1 symlink/malformed |
| SK6, SK14 | §14.1 oversized/too-many; §14.2 token accounting (spec-18-independent) |
| SK8 | §14.2 untrusted-not-resolvable; §14.1 two-lookup separation |
| SK9, SK10 | §14.1 `skill` tool + two-lookup; §14.2 tool path |
| SK11 | §14.2 T-H1 (index present) + compaction |
| SK12 | §14.2 replay |
| SK15 | §14.2 RPC (daemon-owned) |
| SK16 | log assertion in §14.1/§14.2 |
| SK17 | §14.2 H1 usability gating |
| (H1(order) construction order) | §14.2 T-H1 |
| (H1 usability gating) | §14.2 H1 |

### 14.6 Live end-to-end (opt-in, real LLM, `§44`)

Gated by `YMH_LIVE_LLM=1` + `DEEPSEEK_API_KEY` (`AGENTS.md:66-69`). Under a PTY,
create a workspace skill whose `description` matches a task; prompt the real
model and assert **structurally** (not on prose) that the model invoked the
`skill` tool and that the body entered the request. Tolerant of nondeterminism:
assert events/tool names/final state, never exact text.

---

## 15. Decisions and open questions

### 15.1 Decisions (pinned by this spec)

- **(20-D1) A skill is a directory with a `SKILL.md` carrying YAML-subset
  frontmatter and a markdown body.** The directory name is the identity; the
  frontmatter `name` must match it. (§2)
- **(20-D2) Two roots, no built-ins, no cross-tier shadowing.** User tier is
  trusted; workspace tier is untrusted; the user skill always wins a name
  collision. (§2.4, §3.1–§3.2)
- **(20-D3) Hand-rolled strict YAML-subset frontmatter parser; no new
  dependency.** TOML (already available) and general YAML are rejected. (§4.2)
- **(20-D4) Eager, bounded, immutable catalog; discovery at daemon startup; no
  hot reload.** Bodies are cached; `execute()` does no I/O. (§4.1, §5.2)
- **(20-D5) Both invocation paths, trust-asymmetric.** The model uses the
  permission-gated `skill` tool for model-visible skills; the user uses
  `/skill <name>` (via the existing `agent.inject`) for any skill. (§7)
- **(20-D6) The index is pinned in the system prompt; loaded bodies are
  compactable.** (§8.1–§8.2)
- **(20-D7) Skill content is untrusted data; `allowed-tools` is inert; no skill
  can grant or escalate permissions.** (§9.2–§9.3)
- **(20-D8) The `skill` tool's default verdict is the policy default (`Ask`).**
  No implicit allow. (§7.4)
- **(20-D9) Skills are not tools and cannot bundle tools or MCP servers.** One
  bridge tool. (§10)
- **(20-D10) The catalog is daemon-owned; the supervisor lists via additive
  read-only RPC and activates via `agent.inject`.** (§1.2, §5.7)
- **(20-D11) `/skills` is a transcript listing, not an overlay.** It avoids
  `UiMode` churn; the async reply is marshalled to the UI thread via the existing
  `enqueue` and appended with the new exported `append_system_entry`. (§6.1)
- **(20-D12) Spec 20 is independent of spec 18.** The only spec-18 items are the
  optional `/context`-visibility assertions and the optional `SkillIndex` segment;
  both are skipped, not failed, until 18 lands. (§1 independence note, §8.5,
  §14.2, AM-20g)
- **(20-D13) The `skill` tool is gated on the actual permission path, not on
  `attach_permission_gate`.** It is registered and advertised iff skills are
  enabled **and** the `{tool:"skill"}` policy probe is not `Deny` **and**
  (`Allow`, or a prompt path is attached — the M1 gate **or** the M2
  `PermissionBroker` resolver). The daemon sets `attach_permission_gate = false`
  yet installs a resolver, so an `attach_permission_gate`-only predicate wrongly
  disabled the tool interactively (round-2 H1); a `Deny` verdict now also disables
  it (round-2 L4). (§5.5, §7.4)

### 15.2 Open questions

- **OQ-1 — `/context` breakdown.** Should the skill index get its own
  `context_segment_token` value (`skill_index`) via an errata to 18, or stay
  inside `SystemPrompt`? Proposed: optional additive errata AM-20g; v1 folds it
  into `SystemPrompt`. (§8.3, §8.5)
- **OQ-2 — Reload.** Should `/skills reload` re-run discovery without a daemon
  restart? Deferred; D11/MCP's no-hot-reload stance (`15-mcp-adapter.md:1663`)
  argues against it for v1.
- **OQ-3 — Built-in skills.** Should ymh ship any? Deferred; `SkillSource::Builtin`
  is reserved. (§2.4)
- **OQ-4 — Profile exposure.** Should `skills.list`/`skills.show` be available to
  the Automation profile? Proposed: yes (read-only), matching `context.show`
  (`18 §3.4`).
- **OQ-5 — `starts_turn` for `/skill`.** Should `/skill` optionally start a turn
  (`starts_turn = true`)? Proposed: no in v1; `false` keeps load and request
  separate. (§7.2)
- **OQ-6 — Method-catalog ordering.** Spec 18 adds `context.show` (29→30); this
  spec adds two (29→31, or 30→32 if 18 lands first). Whichever lands second must
  rebase the `std::array` size at `src/transport/protocol.cpp:612-624`. A
  coordination note, not a semantic conflict.
- **OQ-7 — `metadata`.** Dropped from v1 entirely (L2); the strict subset parser
  does not accept nested maps, and no consumer needs it. If a flat key/value map
  is later required, it is a parser-local change behind `parse_skill_file` plus a
  `SkillMetadata` field.

### 15.3 Amendment ledger (required cross-spec / code changes)

- **AM-20a (config).** `include/ymh/config/config.hpp`: add `SkillsSettings` and
  `Config::skills`; `src/config/config.cpp`: parse `[skills]` strictly, add
  `YMH_SKILLS_*` env overrides. Additive; no existing key changes.
- **AM-20b (wiring; H1).** `src/agent/workspace_runtime.cpp`: (1) declare
  `std::shared_ptr<SkillCatalog> skill_catalog_;` **between `environment_`
  (`:113`) and `agent_config_` (`:120`)**; (2) initialize it via
  `make_skill_catalog(...)` (which discovers); (3) initialize `agent_config_` via
  `make_agent_config(...)`, which appends `index_section()` to the base prompt
  **inside the initializer**, before `assembler_` (`:121`) copies it; (4) register
  `make_skill_tool(...)` in the ctor body before `freeze()` (`:79`), gated on
  usability (SK17). Additive; no assembler/tool-registry signature change. The
  ordering is load-bearing: a body-time append is a no-op (H1(order), §5.5).
- **AM-20b2 (accessor; M4).** `include/ymh/agent/workspace_runtime.hpp`: add
  `SkillCatalog& skills() noexcept` and the `const` overload; implement in
  `workspace_runtime.cpp` next to `tools()`.
- **AM-20b3 (permission-path option; round-2 H1).**
  `include/ymh/agent/workspace_runtime.hpp`: add
  `bool attach_permission_resolver = false;` to `WorkspaceRuntimeOptions` beside
  `attach_permission_gate` (`:76`). Thread it into `WorkspaceRuntime::Impl`'s
  constructor (a new `bool` parameter beside `attach_permission_gate`, `:39`) and
  pass `options.attach_permission_resolver` from `create()` (`:204-208`).
  `src/host/workspace_host.cpp`: set it `true` at the daemon wiring site (`:488`),
  because the daemon installs the `PermissionBroker` resolver immediately after
  (`:521-530`). `src/cli/headless.cpp` sets neither flag, so a default-`Ask`
  `skill` probe fails closed and the tool is not registered.
  `src/ui/ui_application.cpp` (M1, dead path) may set either flag. Additive.
- **AM-20c (transport).** `include/ymh/transport/protocol.hpp`: two method
  constants; `src/transport/protocol.cpp`: catalog grows by two; `is_method_allowed`
  unchanged (read-only methods are permitted in both profiles);
  `include/ymh/transport/host.hpp`: two pure virtuals; `src/host/host_runtime.*`
  and `tests/support/fake_transport_host.hpp`: overrides. `HostRuntime::listSkills`
  / `showSkill` read through `runtime_.skills()` (M4). Additive to `05`.
- **AM-20d (UI commands).** `include/ymh/ui/command_registry.hpp`: two
  `CommandContext` callbacks **and** the exported
  `void append_system_entry(UiModel&, SessionUiState&, std::string)`;
  `src/ui/command_registry.cpp`: two `builtin()` commands inserted before the
  `listed` snapshot at `:183` (L6). Additive to `10`; composes with the concurrent
  18/19 edits to the same surface (distinct callbacks/commands).
- **AM-20e (supervisor wiring; H3).** `src/ui/supervisor.cpp`: populate the two new
  callbacks; `/skills` submits `skills.list` with a reply callback that wraps its
  UI mutation in `enqueue(...)` (`:488-496`), then calls the exported
  `append_system_entry`; `/skill` fetches `skills.show` (also via `enqueue`) and
  calls `agent.inject`. The `/compact` handler (`:907-915`) is **not** the model
  (it passes `nullptr`). Additive.
- **AM-20f (system prompt).** Covered by AM-20b; no `06`/`13` interface change.
- **AM-20g (optional, OQ-1, L4).** An additive errata to `18-context-errata.md`
  adding a `skill_index` `context_segment_token` and its accounting rule. Not
  required for v1 and not a dependency (20-D12).

### 15.4 Rejected alternatives (summary)

| Alternative | Why rejected |
|---|---|
| Workspace skills override user skills | lets a cloned repo silently replace a trusted skill (T1); §3.2 |
| Hard error on malformed skill | a repo-controlled file could DoS daemon startup; §2.5 |
| A general YAML library | violates `AGENTS.md`'s no-umbrella-dependency rule; §4.2 |
| TOML frontmatter (`+++`) | incompatible with the ecosystem convention; §4.2 |
| Model-tool-only invocation | user cannot force-load; untrusted skills unusable; §7.4 |
| Command-only invocation | model cannot self-serve; whole catalog must be pre-injected; §7.4 |
| Keyword auto-activation | nondeterministic, hidden, repository-triggerable; §7.4 |
| Enforce `allowed-tools` as a grant | privilege escalation from data; SK13, §9.3 |
| Skill-bundled tools / MCP servers | data registering executable capability; §10.2 |
| `/skills` as an overlay | plain text; `UiMode` has no `Context`; overlay was for the grid; §6.1 |
| Pin every loaded body | unbounded context growth; defeats compaction; §8.2 |
| Eager-load supporting files | turns a skill into an uninspected multi-file payload; §9.4 |
| Auto-reload on file change | no hot reload (D11); watchers add platform/dep cost; OQ-2 |
| Appending the index in the constructor **body** (rev 1) | a no-op: `assembler_` captures the prompt during member init (`:58`); corrected in §5.5 (H1) |
| A single `find()` for both model and user lookups | would expose untrusted skills to the model or hide them from `/skills`; split into `find`/`find_model_visible` (H2, §5.2) |
| Gating usability on `attach_permission_gate` alone (rev 2) | the daemon sets it false but attaches a broker resolver, so the predicate wrongly disabled the tool on the interactive path (round-2 H1); gated on the real permission path (SK17, §5.5) |
| Registering `skill` unconditionally headless | a default-`Ask` tool with no prompt path can never run; not registered (SK17, §5.5) |

---

## 16. References

- `AGENTS.md:7-19` — the design-first gate; `:64-65` — dependency policy; `:66-69`
  — live-test gating.
- `docs/design/00-architecture.md` — §7 (plugin model), §26 (slash commands), §27
  (MCP adapter), §31 (context management), §32 (compaction), §37 (configuration),
  §40 (logging), §44–§45 (testing / FakeLLM), §46 (security), §51 (phase 2), §54
  (D1–D23, F1–F12), §55 (dsh comparison).
- `docs/design/06-agent-loop.md` — agent loop, inbox, `inject`.
- `docs/design/07-tools-execution.md` — `Tool`, `ToolRegistry`, `ToolContext`,
  `resolve()` (X1–X16).
- `docs/design/09-permissions.md` — `PermissionPolicy`, `PermissionConfig`.
- `docs/design/10-supervisor-tui.md` — `CommandRegistry`, `UiModel`, transcript.
- `docs/design/13-context-compaction.md` — §3.4/C15 (system prompt excluded from
  the summarize set), §4.2 (projection rule), §6.2 (`ContextAssembler`), §6.7
  (`/compact` path), §7 (C1–C17).
- `docs/design/15-mcp-adapter.md` — §3.2 (namespacing), §4.8 (`AdapterScope`), §8
  (M1–M16), §9 (failure modes), §10 (dsh mapping), §11 (test plan), §12
  (decisions).
- `docs/design/16-daemon-ownership.md:39-44` — naming-prefix convention; §8 —
  invariant/failure-mode/test-plan format.
- `docs/design/17-ui-transcript-errata.md` — additive-errata style; `U-RB*`
  invariant naming.
- `docs/design/18-context-errata.md` — §1 (precedence), §2.4 (assembly anchors),
  §3.3 (segment token accounting), §3.4 (`context.show` RPC), §4 (`/context`).
- `docs/design/REQUIREMENTS_BACKLOG.md:34,171-187` — RB-05 (this spec's gate).
- `docs/design/DESIGN_STATUS.md` — per-spec tracker. **Note:** rows 18 and 19 are
  not present in the tracker as read; this spec's row 20 must be added by the
  tracker's owner, not by this document (per the task's no-other-file rule).

---

## 17. Citation audit and residual unverified claims

### 17.1 Citation audit (2026-09-17, rev 3)

**Every `file:line` anchor touched by the round-2 fixes was re-derived against
the current working tree.** Corrections made in rev 3 (round-2 gate: H1,
M1–M2, L1–L7):

| Claim | rev 2 | rev 3 (current tree) |
|---|---|---|
| RB-05 "no skill concept" text | `REQUIREMENTS_BACKLOG.md:166-172` | `REQUIREMENTS_BACKLOG.md:173-179` |
| RB-05 gate range | `REQUIREMENTS_BACKLOG.md:34,164-180` | `REQUIREMENTS_BACKLOG.md:34,171-187` |
| `09` permission-ownership anchor | `09-permissions.md:116-129` (identifiers) | `09-permissions.md:443-496` (§3.4 evaluation) |
| `wiring.hpp` include analogy | `mcp_client.hpp` via `workspace_runtime.hpp:30` | `mcp_types.hpp` via `wiring.hpp:14,27` |
| RB-05→RB-08 dependency | `REQUIREMENTS_BACKLOG.md:178` | `REQUIREMENTS_BACKLOG.md:186,227-245` |
| `19` prefix anchor | `19-session-rename-errata.md:70` | `19-session-rename-errata.md:121` |
| method catalog array | `protocol.cpp:611-624` | `protocol.cpp:612-624` |
| H1 usability predicate | keyed on `attach_permission_gate` | keyed on the real permission path: `workspace_host.cpp:488,521-530`, `agent_loop.cpp:380-408`, `workspace_runtime.cpp:85`, `headless.cpp:116-123` |

**Re-verified and unchanged in rev 3:** `config.cpp:23-25,27-45,579-587`;
`config.hpp:5-11,13-15,168`; `workspace_runtime.cpp:58,66-79,85,105-132`;
`workspace_runtime.hpp:73-76,110-113,129-136`; `command_registry.cpp:23-33,60-69,
91-111,113-196`; `supervisor.cpp:488-496,524-544,788-808,901-918`;
`supervisor_connection.cpp:362`; `context_assembler.cpp:27-31,33-34,36-50`;
`agent_loop.cpp:83-94,350-461,503-507,563-569`; `session.cpp:317-320,322-337`;
`events.hpp:120-125,127-135,153-157`; `protocol.cpp:641-647`;
`permission_policy.hpp:54-70,69,202-206`; `permission_policy.cpp:204`;
`agent_registry.cpp:231-232`; `tool_registry.hpp:3-6,44-73,76-77,79-82,98-104`;
`tool.hpp:26-31,82`; `environment.hpp:32,33,35-37`;
`environment.cpp:32-35,37-58,60-91`; `workspace_host.cpp:464-475`;
`logger.hpp:29`; `logging.hpp:55`; `CMakeLists.txt:84-92`; `wiring.cpp:71-78,157`;
`19-session-rename-errata.md:26-28`.

**rev-2 corrections (retained):**

**Every `file:line` anchor in this spec was re-derived against the current
working tree** (not the rev-1 draft), because unrelated `/export` work had shifted
`src/ui/command_registry.cpp` and `src/ui/supervisor.cpp`. Corrections made in
rev 2:

| Claim | rev 1 | rev 2 (current tree) |
|---|---|---|
| `D1`–`D23` decision range | `00-architecture.md:4832-4856` | `00-architecture.md:4740-4830` |
| `default_global_config_path` | `config.cpp:578-589` / `:580-588` | `config.cpp:579-587` |
| `clamp_tool_result` declaration | `tool.hpp:79-80` | `tool.hpp:82` (79-81 is its comment) |
| `/compact` reply behavior | "async reply appended on UI thread" | `supervisor.cpp:907-915` passes `nullptr`; no reply (H3) |
| `dispatch_command` | `:901-918` | `:901-918` (unchanged) |
| `builtin()` `/help` snapshot | `:183-194` | `:183-186` snapshot, `:187-194` help command |
| `payload::ToolResult` | "unverified" | verified `events.hpp:127-135` |
| `payload::ToolOutcome` | "unverified" | verified `events.hpp:120-125` |
| `WorkspaceRuntime` member order | "not fully read" | verified `workspace_runtime.cpp:105-132` |
| `enqueue` UI-thread marshalling | "not traced" | verified `supervisor.cpp:488-496`, `:788-808`; reply thread `supervisor_connection.cpp:362` |
| spec-18 anchors | `18-context-errata.md:NNN` line cites | **converted to section refs** (`18 §3.3`, `§3.4`, `§4.2`, `§1`): spec 18 was rewritten mid-review (60 KB → 76 KB) and its line numbers are not stable |

For rev 2, all other anchors were spot-checked and unchanged. Anchors into `13`,
`15`, `16`, `17` are into design documents whose line numbers were stable across
this review; anchors into `18` use **section references only**, because 18 is
unverified and was being edited concurrently.

### 17.2 Residual unverified claims

1. `ToolRegistry::schemas()`'s determinism/order guarantee — asserted by M13
   (`15-mcp-adapter.md:1576-1578`) but the implementation was not read. §10.3 and
   the test plan assume it; if it is false, registration order of `skill` matters
   and must be pinned.
2. Whether `ToolConfig::read_file_max_bytes` is enforced by
   `LocalFilesystem::read` (the header comment says so at
   `include/ymh/execution/filesystem.hpp:5-6`; the `.cpp` was not read). This
   affects only the alternative of reading bodies via `fs().read`; the chosen
   design (§5.2/§5.4) eager-loads bodies at discovery and does no per-call read,
   so the claim is not load-bearing.
3. Spec 18's status: it is absent from `DESIGN_STATUS.md` as read, and
   `context_segment_token` (18 §3.2/§3.3) is a design-artifact
   name, not a code symbol. This is exactly why spec 20 is designed to be
   independent of 18 (20-D12); no spec-20 requirement depends on it.
4. `src/host/host_runtime.cpp`'s `throw_mapped`/`WireError` helper signatures
   (§5.7 sketch) are used by analogy with `agentInject` (`:658-675`) and were not
   read exhaustively; the RPC handler sketch is illustrative, not pinned.
