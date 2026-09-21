# 46 — Permissions & UI Errata: Config-Gated Auto-Allow, Durable Always Grants, the Empty-Switcher Notice, Dialog Compositing, Alias Completion, Two-Press Enter, Fresh-Start Sessions, Tool Deadlines, the Turn Spinner, Prompt-History Hydration, and the LocalCode Import Expansion

```
Status: **draft (Rev 6)** — awaiting the final Oracle design re-review. This spec amends the owning specs;
        it introduces no new component. It is the second UI/permission errata
        after 45-ui-interaction-errata.md and, like 45, is written so that every
        "current state" claim is reproducible from the shipped tree.
        Rev 6 responds to the Oracle re-review
        (/tmp/opencode/spec46-oracle-rereview.md: DO NOT APPROVE — 1 HIGH /
        1 MEDIUM / 4 LOW) and to all prior reports — the Oracle design pass
        (/tmp/opencode/spec46-oracle.md: DO NOT APPROVE — 3 HIGH / 7 MEDIUM /
        3 LOW), Gate 1
        (/tmp/opencode/spec46-gate1.md: GATE FAIL 9 HIGH / 32 MEDIUM / 15 LOW),
        Gate 2 (/tmp/opencode/spec46-gate2.md: GATE FAIL 2 HIGH / 19 MEDIUM /
        18 LOW) and Gate 3 (/tmp/opencode/spec46-gate3.md: GATE FAIL 0 HIGH /
        4 MEDIUM / 5 LOW). Every finding from all five reports is resolved or
        rebutted in §23. This is the final convergence round: the Oracle verified
        12 of the 13 prior findings genuinely closed and confirmed the approved
        areas show no regression, so Rev 6 is surgical.
        Rev 6 fixes the one HIGH and one MEDIUM the Oracle re-review raised plus
        its four LOWs:
        (NEW-1) the `resume_in_flight_` refcount is **released only at the two
        genuine resume terminals** via a guarded `release_resume` helper —
        `recover_unknown_session` is removed from the marker path entirely, because
        it has two non-resume callers (`prompt`, `agent.select`) that never
        incremented the count, and the Rev-5 unconditional decrement there would
        wrap a zero entry to `SIZE_MAX` and permanently close the D7 gate;
        (NEW-2) an explicit config **deny** now **vetoes a durable `Always`
        grant** regardless of specificity, so the D1.6/F1 recovery actually
        revokes a grant instead of only deleting the file;
        (NEW-3) I1 now distinguishes a config-rule `path` key (fatal `ConfigError`)
        from a grants-file `path` key (fail-safe skip);
        (NEW-4) the pinned daemon order guards the `findByCanonicalPath`
        `optional` before dereferencing it;
        (NEW-5) the `kRedacted` citation is corrected to
        `src/llm/redaction.cpp:10`; and
        (NEW-6) I36 is restated to the shape-based, enforceable form the redactor
        can actually guarantee.
        Rev 5 fixed the three structural defects the gate rounds could not see:
        D2's durable grants are rescoped to **tool + command only** (the
        `path` dimension is dropped from both durable grants and config rules,
        because the shipped loop never populates `PermissionRequest::path`) and
        are matched **literally**, not as globs (O-H1/O-M1); the `GrantStore`
        construction, ownership and injection seam is pinned together with the
        real daemon startup order (O-H2); and the `resume_in_flight_` marker
        becomes a per-workspace **refcount** backstopped by a
        `SupervisorConnection` disconnect drain that replies to every pending
        request, so no exit can leak it (O-H3/O-M6). It also corrected the
        confinement claim for `shell`/`git`/`mcp.*` (O-M2), pinned the
        `max_tokens` route (O-M3), unified `PolicyConfigError`→`ConfigError`
        (O-M4), moved the `force_ask` guard into `remember()` (O-M5), and pinned
        log-seam redaction (O-M7).
        Rev 3 superseded the rev-2 `pending_resume_` gate with a
        `resume_in_flight_` marker (G2-H1) and re-verified every pinned symbol
        against the shipped headers (G2-H2). Rev 4 closed the four gate-3
        MEDIUMs in the two flagship fixes: it inserted the marker **inside**
        `resume_after_attach` so the explicit already-attached path is covered
        (N1), enumerated every marker-clear exit (N2), pinned the PTY trichotomy
        so an expired deadline can never be a `0` `wait_ms` (N3), and pinned the
        MCP clamp's `caller_ms` so the configured server timeout stays a ceiling
        (N4); the five gate-3 LOWs were swept (N5–N9). Rev 5 superseded Rev 4's
        set-based marker-clear discipline with a refcount and superseded the
        Rev-4 claim that a reply is always delivered on disconnect (it was false
        — see §9.2/O-H3). **Rev 6 supersedes Rev 4/Rev 5's
        `recover_unknown_session` marker release (NEW-1) and Rev 5's
        "independent of the layered config" grant statement (NEW-2).**
Component: 46 (errata) — amends 07-tools-execution.md, 08-llm-provider.md,
            09-permissions.md, 10-supervisor-tui.md, 21-config-jsonc-errata.md,
            22-switcher-sessions-errata.md, 23-session-lifecycle-errata.md,
            25-ui-ux-errata.md, 28-llm-service-boundary-errata.md,
            45-ui-interaction-errata.md
Depends on: 00-architecture.md (verified), 05-transport.md (verified),
            07-tools-execution.md (verified), 08-llm-provider.md (verified),
            09-permissions.md (verified), 10-supervisor-tui.md (verified),
            21-config-jsonc-errata.md (verified),
            22-switcher-sessions-errata.md (verified),
            23-session-lifecycle-errata.md (verified),
            25-ui-ux-errata.md (verified),
            28-llm-service-boundary-errata.md (verified),
            45-ui-interaction-errata.md (verified)
Related:    01-session.md (durable `UserMessage`, `PermissionDecision`),
            02-persistence.md (no grants table), 16-daemon-ownership.md
            (daemon-owned per-workspace artifacts), 19-session-rename-errata.md
Method:     every "current state" claim below is verified against the shipped tree
            with `file:line` and a verbatim quote; spec sections are cited only
            for pinned decisions. No implementation is written here. The spec
            author does not touch git. Rev 3 re-opened every retained citation
            AND every pinned type/enum/function/field name in §16 and the
            D-decisions against the headers; citations that could not be
            re-verified are marked "unverified". No symbol is named that was not
            grepped in the shipped headers. Rev 5 re-opened every citation in
            every section it touched (the shipped loop's `PermissionRequest`
            construction, `path_pattern`, `remember`, `glob_match`,
            `WorkspaceRuntimeOptions`/`workspace_host` startup,
            `SupervisorConnection::process_requests`/`handle_disconnect`,
            `agent_loop.cpp:539`/`to_agent_config`, `SpdlogLogger::log`,
            `redact_secrets`, `apply_permissions`/`validate_rule`,
            `config.hpp:14-17`) and re-grepped every symbol it names. Rev 6
            re-opened every citation in every section it touched
            (`recover_unknown_session` and its callers, `resume_after_attach`/
            `apply_resume_success`, `rule_greater`/`evaluate`/`remember`,
            `Layer`, `findByCanonicalPath`/`ensureWorkspaceRegistered`,
            `kRedacted`/`redact_secrets`, the `D7.2` gate branch) and re-grepped
            every symbol it names.
```

---

## 1. Purpose, scope, and supersession map

### 1.1 The thirteen requirements (verbatim, from `requirements_draft.txt`)

1. "Fix permissions. If enabled in the config - then all commands in the current
   repo's directory and below shall be without additional asking."
2. "If user answered always allowed - is it marked permanently for the
   repo/directory?"
3. "When started supervisor UI and immediately hit `<ctrl+s>` and switched to
   another the only workspace, hitting `<ctrl+s>` again shows (current session
   hidden). The whole pop up window shall be replaced with 'No other workspaces
   available' and offering only 'OK' button to be dismissed with `<Enter>` or
   `<Esc>`."
4. "Bug: when permissions window stretches horizontally to show longer shell
   command it is about to execute the window shows background text right after
   options 'Allow once', 'Allow for session' etc."
5. "Now /quit is exiting as /exit does - good. But when /quit is typed there is
   no text what it does like it is when /exit is typed. Make when /quit is typed
   the same string as when /exit is typed is shown."
6. "When /q and `<Enter>` or `<Tab>` is hit it should autocomplete quit. Same as
   with other commands. Another `<Enter>` is required to execute it."
7. "Now when I open 'ymh' it opens a previous workspace by default. It should be
   a clean new session (or even an empty one until first prompt is issued)."
8. "Add a timeout for tool runs. That should prevent hangs when tool took too
   long or hung."
9. "Add a spinning animation - same as 'Thinking' has, in the lowest most left
   corner, before agent mode. It should be on right after user hit prompt and
   until ymh is waiting for new user input."
10. "When press '/' and `<esc>` the command list disappears but '/' remains. If
    user continue typing command it happens without commands list. So if `<esc>`
    deleted the list but user continues typing the command list shall reappear."
11. "Arrows up/down do not bring back previous prompts."
12. "Initial import of `~/.localcode/config.json` shall import API keys and
    permission rules as well. It should also import model parameters such as max
    content etc."
13. "Add ctrl+e shortcut to start `$EDITOR` to edit prompt. Once user :wq the
    editor should exit and the prompt shall be copied to prompt text box."

### 1.2 What this changes, in one sentence

Permissions gain one config-gated "allow within the workspace" master switch plus
a general config rule list; an `Always` answer becomes durable in a workspace-local
JSONC grants file; the Live switcher is replaced by a single-OK notice when it has
no target; the permission dialog (and its two sibling overlays) stop leaking the
transcript behind them; command matching becomes alias-aware so `/quit` shows the
same help row as `/exit` and `/q` completes to `quit`; **Enter completes a partial
command and a second Enter executes it**; a fresh launch opens a clean session
instead of resuming the earliest stored one; every tool call gets a configurable
deadline enforced at the blocking primitive; the status line grows a bottom-left
turn spinner; the command list already reappears after Esc and only needs a
regression pin; prompt history is hydrated from the durable log; the localcode
first-run import also carries API keys, permission rules and model parameters
(`max_tokens`, `context_window`); and Ctrl+E hands the composer to `$EDITOR`.

### 1.3 Supersession map

#### 1.3.1 Superseded

| ID | Prior text | Change |
|---|---|---|
| 46-S1 | `45-ui-interaction-errata.md` 45-D8.5 (`:969-971`): "Completion is unchanged. `complete` still matches canonical names only … Tab completes `/exit`, never `/exit(quit)`." **And** 45-D8.2 (`:951-952`): "`CommandHint` becomes `{ name, display, description }` (`include/ymh/ui/ui_model.hpp:179-182`)". The pinned tests are `UI45_D8_TabCompletesCanonicalName` (`tests/unit/command_registry_test.cpp:131-139`) and `AliasesAreNotCompleted` (`:39-43`). | **Superseded by 46-D5/46-D6.** `complete()` is retired and replaced by `complete_candidates()`, which matches canonical names *and* aliases and returns the matched spelling. `/q` completes to `/quit`; `/quit` yields the same hint row as `/exit`. `CommandHint` becomes `{name, display, insert, description}` (a fourth field; arity changed). The 45-D8 **display** rule (`name(alias1,alias2)`, `command_display_name`, `45-ui-interaction-errata.md:947-950`, 45-I19 `:1660`) is **retained**. |
| 46-S2 | `25-ui-ux-errata.md` 25-D12 (`:1475-1505`) pins `/help` as `  /exit  quit the supervisor (alias: /quit)`. | Already superseded by 45-D8; **46 does not re-open it.** 46-D5 only changes *matching*, not the rendered string. Recorded so the two alias specs are not conflated. |
| 46-S3 | `09-permissions.md` §2.3 Q13 (`:208-226`), §3.6 (`:517-520`) and decision (e) (`:1236-1239`): `Always` grants are "appended to the workspace-local grants file `<workspace>/.ymh/permissions.local.toml`". | **Superseded by 46-D2.** The store becomes `<workspace>/.ymh/permissions.jsonc` (JSONC), because TOML is retired project-wide (`21-config-jsonc-errata.md` §1.3; `AGENTS.md` "Config is JSONC; TOML is retired"). Workspace scoping, daemon single-writer ownership, and the `{Once, Session, Always}` semantics are retained. |
| 46-S4 | `21-config-jsonc-errata.md:214-224` "Out of scope — `permissions.local.toml` (pinned)": the grants file "is a distinct, still-unimplemented persistence artifact … not a `ConfigPaths` slot. … Whether the grants file should also become JSONC is a spec-09 decision and is deliberately not pre-empted here." | **Superseded by 46-D2.** 46 makes the spec-09 decision: the artifact is now in scope, implemented, and JSONC. It remains **not** a `ConfigPaths` slot (it is not the layered config file). |
| 46-S5 | Shipped startup auto-resume: `refresh_sessions` resumes the earliest stored session, `src/ui/supervisor.cpp:1344-1345` (`resume_after_attach(workspace, stored.front().first)`), with the design comment at `:1286-1294` ("For the active workspace with no open session but stored history, the first stored session is resumed …"). | **Superseded by 46-D7.** A fresh launch creates a clean session; `--resume` and an explicit Ctrl+S/`/sessions` selection remain the only resume paths (22 §5.2, `docs/design/22-switcher-sessions-errata.md:1298-1319`). The zero-stored eager-create rule of 23-D58 (`docs/design/23-session-lifecycle-errata.md:375`, `:1253-1293`) is retained and extended to "always create". |
| 46-S6 | `21-config-jsonc-errata.md:232-233`: "**Secrets never in `Config`** — `llm.api_key_env` names an env var (`include/ymh/config/config.hpp:14-16`, `08-llm-provider.md:706`)." Also the shipped comments `include/ymh/config/config.hpp:14-17` ("Secrets are never stored in `Config`"), `src/config/config.cpp:953` ("Secrets are never stored here") and `include/ymh/config/config.hpp:298-300` ("no provider api_key copy"). | **Superseded by 46-D12.2 (USER DECISION, 2026-09-20).** `LlmSettings::api_key` is a literal secret stored in the ymh config file, exactly as localcode does. Mitigations are pinned in 46-D12.2/10/11: `api_key` is accepted **only in the global layer** (a workspace-layer `api_key` is a `ConfigError`, G2-M13); the scaffolded global `config.jsonc` is written `0600` (G2-M12); the key is never rendered in the UI or logs (redaction, G2-M14); the key is never copied into the session event log. The three shipped comments above are part of the amendment (G2-L11). **O-L3 (Rev 5):** the header comment `include/ymh/config/config.hpp:14-17` is explicitly **rewritten**, not merely superseded — the replacement reads: *"`llm.api_key` (46-D12.2) is the one permitted literal secret; it is accepted only in the global config layer, the file is `0600`, and the value is never logged, rendered, or event-logged. All other credentials come from `llm.api_key_env`."* This is a deliberate, reviewed narrowing of the "secrets never in `Config`" invariant; the env path (`api_key_env`) is retained as the fallback. |
| 46-S7 | `08-llm-provider.md` §14.1(g) `:1322-1323`: "**(g) v1 credentials come from `api_key_env` only.** Literal keys in config files are rejected; a keyring is deferred (`§40`, `§46`)." §14.1(t) `:1359-1360`: "**(t) `api_key_env` only.** A literal key in config and an OS keyring are deferred; v1 rejects literal keys (§6.4)." **And** §6.4 `:855-867`: "v1 never reads a literal key from a config file (decision (g))", "Keys … never persisted outside the session log", "The key is held only for the lifetime of a request; it is never written to the registry, the session DB, or the event log." | **Superseded by 46-D12.2 (USER DECISION).** A literal `api_key` in the ymh config is accepted and takes precedence over `api_key_env`; a keyring remains deferred. **46-D12.3** amends `08 §5.1` (`LLMProviderConfig`) and 28 `to_provider_config` to carry the value. §6.4's three bullets are explicitly superseded: a literal key *is* read from the global config file, is persisted outside the session log (in the `0600` global config), and is held in `LLMProviderConfig` for the provider lifetime (G2-M15). |
| 46-S8 | `25-ui-ux-errata.md` 25-D14/D15: `:1774-1775` "Import MCP servers, model, compaction, and concurrency? (Permission rules and API keys are not imported; ymh keeps its own permission prompts.)"; `:1809` "… Map `base_url` and `model`; map `context_window` to `context_window_tokens`. Do **not** copy `max_tokens`."; `:1810` "`providers[*].api_key` … **Ignored.**"; `:1811` "`permission` (glob rule list) … **Ignored**"; `:1808` "`skip_permissions == true` … **Not imported**". | **Superseded by 46-D12.** The first-run import also carries `providers[*].api_key`, `skip_permissions`, the `permission` rule map, and `profiles[*].max_tokens`. 25-D13 (`mcp_servers`) is retained unchanged. |
| 46-S9 | `09-permissions.md` decision (f) (`:1240-1242`): "Configuration extends `§37` with `[[permissions.rule]]` while keeping the `[permissions]` shorthand." (also referenced by `15-mcp-adapter.md:40,521,1326,2058`). | **Superseded by 46-D1.** The rule list is `permissions.rules` in JSONC (an array of objects), not a TOML `[[permissions.rule]]` table. The `[permissions]` shorthand (`shell`/`write`/`read`) is retained. |
| 46-S10 | `10-supervisor-tui.md:156`: `enum class UiMode : std::uint8_t { Conversation, Switcher, Dialog };`. | **Amended by 46-D3.** `UiMode` gains `ExitConfirm`, `Context`, and `Notice` (the shipped enum already has the first two; Rev 2 names the full set for completeness). |
| 46-S11 | `22-switcher-sessions-errata.md:319-320`: "`SwitcherOverlayModel::open` applies both halves of the display predicate." | **Amended by 46-D3.** The Ctrl+S open path now evaluates `switcher_has_targets()` first and opens the `Notice` instead of the switcher when it is false; `SwitcherOverlayModel::open` is unchanged and still applies the display predicate when the switcher *is* opened. |
| 46-S12 | Shipped one-keypress accept-and-dispatch for a partial command: `src/ui/supervisor.cpp:2355-2372` (`accept_highlight` rewrites the draft to the canonical name and the same Enter dispatches it; `:2134-2148`). Also `25-ui-ux-errata.md` 25-D9 (`:309-311`, sourced by `45-ui-interaction-errata.md:309-310`): "`accept_highlight` (Enter accepts the highlight) is retained", pinned by `UX-U15` (`25-ui-ux-errata.md:2158`) and the shipped test `UX_U15_EnterAcceptsHighlightAndDispatches` (`tests/unit/supervisor_harness_test.cpp:1037`). | **Superseded by 46-D6 (USER DECISION).** A draft that is **not** an exact command name/alias is completed in place on Enter and **not** dispatched; a **second** Enter dispatches. An exact name/alias still dispatches on the first Enter. This changes today's behaviour for every partial draft (e.g. `/e` + Enter no longer exits). `accept_highlight` is retired (46-D6.5), so **25-D9/UX-U15 is superseded** and `UX_U15_EnterAcceptsHighlightAndDispatches` is a required rewrite (G2-M17). |
| 46-S13 | `28-llm-service-boundary-errata.md:303` `to_provider_config` (`src/cli/wiring.cpp:70-74`) and the 08 §5.1 `LLMProviderConfig` field set (`include/ymh/llm/provider_registry.hpp:25-37`). | **Amended by 46-D12.3.** `LLMProviderConfig` gains `std::optional<std::string> api_key`; `to_provider_config` copies it; the OpenAI adapter uses it in preference to `api_key_env`. (`max_tokens` is 46-D12.4 — the rev-2 register had these two IDs swapped; G2-M16.) |
| 46-S14 | `21-config-jsonc-errata.md` §1.3.3 `:226` "Retained (recorded, not changed)"; the live key schema is §3.3 "Every config key read today" (`:372`), §7.5 "`permissions`" (`:1220`), §7.7 "`llm`, `llm.default`, and `llm.default.retry`" (`:1242`), and §7.11 "`skills`" (`:1418`). | **Amended by 46-D1/D2/D8/D12.** New keys are added to the live key schema: `permissions.default`/`permissions.rules` under §3.3/§7.5; `llm.default.api_key`/`llm.default.max_tokens` under §3.3/§7.7; `tools.timeout_ms` under §3.3 **and** a new `21` §7.12 `tools` subsection (there is no `tools` §7.x today — `:1220` is §7.5 `permissions`, not `tools`; G2-M4). §1.3.3 is *not* the key table and is not the amendment target. |

#### 1.3.2 Amended

- 25-A11 (`docs/design/25-ui-ux-errata.md:163`): `force_ask` ordering retained; 46-D1
  never bypasses it.
- 09 §3.3/§3.5 (`docs/design/09-permissions.md:325`, `:497`): the rule model and
  the hard-deny/default order; 46-D1 adds config rules at `Layer::Project`.
- 09 §3.6 and decision (e): the workspace-scoped `Always` semantics; 46-D2 only
  changes the file format and adds the seam.
- 07 §5.5: `ToolConfig`; 46-D8 adds `tool_timeout` and the deadline plumbing.
- 10 §2.3/§5.4/§6/§7.1/§7.2/§9.2/§9.3: the startup focus decision, the status
  segments, the switcher open path, and the command-input handlers.
- 22 §3/§5.2: the switcher display predicate and the explicit resume paths.
- 23 §1.1/§6.5: eager create and the refresh auto-create / stray-session rule.
- 08 §5.1/§6.2/§6.4: `LLMProviderConfig`, provider-error redaction, credential
  sourcing (46-D12.3/11; §6.4's three literal-key bullets are named in 46-S7).
- Shipped "secrets never in `Config`" comments: `include/ymh/config/config.hpp:14-17`,
  `src/config/config.cpp:953`, `include/ymh/config/config.hpp:298-300` (G2-L11).
- 28 §`to_provider_config`: the provider config mapping.
- 45 45-D8.2/45-D8.5: `CommandHint` arity and completion; the display rule is
  retained.

#### 1.3.3 Retained (explicitly not changed)

- The `{Once, Session, Always}` grant-scope semantics and workspace scoping
  (`docs/design/09-permissions.md:208-226`, `:1236-1239`).
- The `RulePermissionPolicy::evaluate` order: `ReadOnly` mutating hard-deny →
  `force_ask` → rules → `tool_defaults` → `default_verdict`
  (`src/policy/permission_policy.cpp:204-210`, `:242-252`).
- The rule precedence *order*: specificity → layer → effect
  (`src/policy/permission_policy.cpp:58-79`); 46-D1 changes only the layer
  assigned to config rules, not the order.
- The Live catalog projection and the switcher display predicate
  (`docs/design/22-switcher-sessions-errata.md:319-320`).
- `--resume <id>` and the explicit `session.resume` for the History source and for
  `--resume` (`docs/design/22-switcher-sessions-errata.md:1298-1319`).
- 16-daemon-ownership: the workspace daemon is the single writer of its
  workspace-local artifacts.
- 25-D13 `mcp_servers` import (`docs/design/25-ui-ux-errata.md:1805`).

### 1.4 Scope boundaries

**In scope.** Two config sections (`permissions.default`/`permissions.rules`,
`tools.timeout_ms`), one durable grants file, one new UI mode (single-OK notice),
one renderer-compositing fix, alias-aware command matching, the two-press Enter
interaction, the startup session decision, a tool deadline plumbed to the blocking
primitives, a status-line spinner, prompt-history hydration, the localcode import
mapping (API keys, permission rules, model parameters), and the Ctrl+E composer
editor.

**Out of scope (recorded, not pinned).**
- A `/permissions` management command (list/reset grants). 46-D2 pins the store
  and its invalidation (deleting the file); a management surface needs its own
  spec.
- Interrupting an in-process tool that ignores its deadline (46-D8.8 documents the
  cooperative limit, including `git`; it is not a defect of this spec).
- A global (cross-workspace) `Always` grant. `Always` stays workspace-scoped
  (09 decision (e)); a global grant is config-only.
- An OS keyring. 46-D12 stores the literal key in the `0600` config file; a
  keyring remains deferred (`08` §40/§46).
- Migrating or mapping localcode's non-openai-compatible provider types
  (`bedrock`, etc.); they are recorded and skipped with a note.

### 1.5 Terminology (pinned)

- **Auto-allow switch** = `permissions.default == "allow"` (46-D1). It sets the
  fallback verdict and upgrades the builtin tool rules and MCP tool defaults to
  `Allow`; it never bypasses a hard deny or `force_ask`.
- **Grant** = a `PolicyRule` with `effect = Allow` and
  `layer ∈ {LocalGrant, SessionGrant}` (`include/ymh/policy/permission_policy.hpp:83-91`).
  **Always grant** = a `LocalGrant`; **Session grant** = a `SessionGrant`.
  A grant carries **`tool` and, for command-bearing tools, `command` only** —
  the `path` dimension is never set (O-H1). A `remember()`-derived grant is
  **literal**: its `command` matches exactly, never as a glob (O-M1).
- **Literal match** = a `PolicyRule` with `literal = true` (46-D2) whose
  non-empty `command` must equal `command_pattern(request)` exactly;
  `glob_match` is used only when `literal == false`. Config rules are
  `literal == false` (operator globs); durable grants are `literal == true`.
- **Workspace root** = the canonical root returned by
  `ExecutionEnvironment::root()`; every tool path is resolved through
  `ExecutionEnvironment::resolve()` (`include/ymh/tools/tool_context.hpp:46-49`).
- **Notice** = the single-OK message popup of 46-D3 (`UiMode::Notice`).
- **Live target** = a node the Live switcher can act on: a live-renderable
  workspace other than the active one, or a non-focused session leaf of the
  active workspace.
- **Turn active** = the active session's `agent_state` is `Thinking` or
  `CallingTool` (`src/ui/ui_model.cpp:324-326`).
- **Deadline** = the absolute `steady_clock::time_point` carried by `ToolContext`
  for one tool call (46-D8). **Disabled deadline** = no deadline (the config value
  is `0`); **expired deadline** = the absolute time is at or before `now`.
- **Exact command** = a draft whose first whitespace-delimited token, without the
  leading `/`, is a canonical command name or an alias (`CommandRegistry::find`
  non-null, `src/ui/command_registry.cpp:71-86`).

---

## 2. Amendment register

| ID | Decision | Amends |
|---|---|---|
| 46-D1 | `permissions.default` + `permissions.rules`; the "allow within the workspace" master switch | 09 §3.3/§3.5, 09 decision (f), 21 §3.3/§7.5 |
| 46-D2 | Durable `Always` grants in `<workspace>/.ymh/permissions.jsonc`, scoped **tool + command only** and matched **literally**; the store seam inside `RulePermissionPolicy::remember`; the daemon-owned `GrantStore` injected via `WorkspaceRuntimeOptions` | 09 §2.3 Q13/§3.6/decision (e), 21 §1.4 (grants file now in scope) |
| 46-D3 | Live switcher with no target → single-OK `Notice` ("No other workspaces available") | 10 §7.1/§7.2 + `10:156` `UiMode`, 22 §3 |
| 46-D4 | `render_dialog` / `render_exit_confirm` / `render_switcher` gain `ftxui::clear_under` | 10 §8 |
| 46-D5 | Alias-aware matching; `/quit` shows the `/exit` row (item 5) | 10 §9.2, 45 45-D8.2/45-D8.5 |
| 46-D6 | Two-press Enter: first Enter completes a partial command, second executes (item 6) | 10 §9.2, 45 §6.1/45-D2, 25-D9/UX-U15 (`accept_highlight` retired) |
| 46-D7 | Fresh session on startup; no resume-earliest (item 7) | 10 §2.3/§5.4, 22 §5.2, 23 §1.1/§6.5 |
| 46-D8 | `tools.timeout_ms` + `ToolContext` deadline enforced at blocking primitives (item 8) | 07 §5.5, 21 §3.3 + new §7.12 `tools` |
| 46-D9 | Bottom-left turn spinner in the status line (item 9) | 10 §6, 25 D1 |
| 46-D10 | Command list reappears after Esc — already satisfied; regression pin (item 10) | none (45-D5 retained) |
| 46-D11 | Prompt history hydrated from replayed `UserMessage` events (item 11) | 10 §4.3/§9.3 |
| 46-D12 | LocalCode import: API key, permission rules, `max_tokens` (item 12) | 08 §5.1/§6.2/§6.4/§14.1(g)(t), 21 §1.3.3 (`:232-233` "secrets never in `Config`") + §3.3/§7.5 (permissions) + §7.7 (llm), 25 D14/D15, 28 |
| 46-D13 | Ctrl+E opens `$EDITOR` and copies the result into the composer (item 13) | 10 §9.2 |

**Supersession rows added in Rev 2:** 46-S7 (08 literal-key rejection), 46-S8
(25-D14/D15 import exclusions), 46-S9 (09 decision (f) TOML rule table), 46-S10
(10 `UiMode`), 46-S11 (22 §3), 46-S12 (shipped one-keypress Enter), 46-S13 (28
`to_provider_config`), 46-S14 (21 key-table target). 46-S6 (21:232-233 "secrets
never in `Config`") is superseded by the user's API-key decision.

**Rev 3 register deltas:** 46-S7 gains `08` §6.4's three literal-key bullets
(G2-M15); 46-S6 gains the three shipped "secrets never in `Config`" comments
(G2-L11); 46-S12 gains `25-D9`/`UX-U15` (G2-M17); 46-S13's amendment ID is
corrected to **46-D12.3** (`max_tokens` is **46-D12.4**; G2-M16); 46-S14's
key-table targets are corrected (`tools.timeout_ms` → new `21` §7.12; G2-M4).
46-D7's gate is `resume_in_flight_` (G2-H1).

**Rev 5 register deltas:** 46-D1's rule shape drops the `path` dimension
(O-H1); 46-D2's grants become tool + command only, matched literally, with the
`GrantStore` seam pinned (O-H1/O-M1/O-H2); 46-D7's gate becomes a per-workspace
refcount with a disconnect drain (O-H3/O-M6); 46-S6 gains the pinned replacement
text for `config.hpp:14-17` (O-L3).

**Rev 6 register deltas:** 46-D2 gains item 13 — an explicit config **deny**
vetoes a durable `Always` grant (NEW-2), amending the "independent of the layered
config" reading of 46-D2.9; 46-D7's marker release moves out of
`recover_unknown_session` into the resume-failure terminal via a guarded
`release_resume` (NEW-1); I1 distinguishes a config-rule `path` key from a
grants-file `path` key (NEW-3); the daemon startup order guards the
`findByCanonicalPath` optional (NEW-4); the `kRedacted` citation is corrected
(NEW-5); I36 is restated to the shape-based form (NEW-6); and the tool-wide
durable path grant is recorded as an explicitly accepted limitation (§21 Q16).

---

## 3. D1 — Config-gated "no asking at or below the workspace root" (item 1)

### 3.1 Current state (verified)

- The only `[permissions]` keys are `shell`/`write`/`read`; an unknown key is a
  `ConfigError`. `src/config/config.cpp:349-357`:

  ```cpp
  void apply_permissions(Config& config, const Json& table, const std::filesystem::path& source) {
      reject_unknown(table, "permissions", {"shell", "write", "read"}, source);
      config.permissions.shell =
          read_string(table, "shell", "permissions", config.permissions.shell, source);
      config.permissions.write =
          read_string(table, "write", "permissions", config.permissions.write, source);
      config.permissions.read =
          read_string(table, "read", "permissions", config.permissions.read, source);
  }
  ```

- `default_verdict` is **hard-coded `Ask`**; the six builtin tools get one rule
  each from the three keys, and each builtin rule is `Layer::Global`.
  `src/cli/wiring.cpp:24-34` (`add_rule` sets `rule.layer = PolicyRule::Layer::Global`)
  and `src/cli/wiring.cpp:78-91`:

  ```cpp
  PermissionConfig to_permission_config(const Config& config) {
      PermissionConfig permissions;
      permissions.default_verdict = PolicyVerdict::Ask;

      const PolicyVerdict read  = parse_verdict(config.permissions.read, "read");
      const PolicyVerdict write = parse_verdict(config.permissions.write, "write");
      const PolicyVerdict shell = parse_verdict(config.permissions.shell, "shell");

      add_rule(permissions, "read_file", read, "default.read_file");
      add_rule(permissions, "grep", read, "default.grep");
      add_rule(permissions, "glob", read, "default.glob");
      add_rule(permissions, "write_file", write, "default.write_file");
      add_rule(permissions, "edit_file", write, "default.edit_file");
      add_rule(permissions, "shell", shell, "default.shell");
  ```

- There is **no** auto-approve / bypass / directory-scoped key. The design's
  `default_verdict{PolicyVerdict::Ask}` (`include/ymh/policy/permission_policy.hpp:105-111`)
  is returned only when no rule and no tool default matches
  (`src/policy/permission_policy.cpp:242-252`).
- Evaluation order is fixed. `src/policy/permission_policy.cpp:204-210`:

  ```cpp
  PolicyVerdict RulePermissionPolicy::evaluate(const PermissionRequest& request) const {
      if (request.sandbox == SandboxMode::ReadOnly && tool_is_mutating(request)) {
          return PolicyVerdict::Deny;
      }
      if (request.force_ask) {
          return PolicyVerdict::Ask;
      }
  ```

- Rule precedence is specificity-first, then layer, then effect.
  `src/policy/permission_policy.cpp:58-79` (`specificity_key` counts non-empty
  dimensions and compares `tool`, then `path`, then `command`; `rule_greater`
  compares the specificity key **before** the layer):

  ```cpp
  std::vector<long long> specificity_key(const PolicyRule& rule) {
      std::vector<long long> key;
      key.push_back((rule.tool.empty() ? 0 : 1) + (rule.path.empty() ? 0 : 1) +
                    (rule.command.empty() ? 0 : 1));
      append_dimension(key, rule.tool);
      append_dimension(key, rule.path);
      append_dimension(key, rule.command);
      return key;
  }
  bool rule_greater(const PolicyRule& left, const PolicyRule& right) {
      const std::vector<long long> left_key = specificity_key(left);
      const std::vector<long long> right_key = specificity_key(right);
      if (left_key != right_key) {
          return std::lexicographical_compare(right_key.begin(), right_key.end(),
                                              left_key.begin(), left_key.end());
      }
      if (left.layer != right.layer) {
          return static_cast<int>(left.layer) > static_cast<int>(right.layer);
      }
      return effect_rank(left.effect) > effect_rank(right.effect);
  }
  ```

- The rule validators (`validate_rule`, `validate_glob`) live in an **anonymous
  namespace** inside `src/policy/permission_policy.cpp` (`:12 namespace {` … `:158
  } // namespace`); they are **not** declared in `permission_policy.hpp` and
  cannot be called from `src/config/config.cpp`. This is the F-10 correction.
- Path containment is structural: every tool path goes through
  `ExecutionEnvironment::resolve()` (`include/ymh/tools/tool_context.hpp:46-49`),
  which throws `ToolError{ToolErrorCode::PathEscape}` for an out-of-root path
  (`src/execution/environment.cpp:60-87`; `include/ymh/execution/errors.hpp:4`,
  `:18`).

### 3.2 Decision (46-D1)

1. **New key `permissions.default`** ∈ `{"ask","allow","deny"}`, default `"ask"`,
   mapped to `PermissionConfig::default_verdict`. It is the single
   "never ask" opt-out the user asked for.
2. **New key `permissions.rules`** — an array of rule objects. Each entry is
   `{"tool"?: string, "command"?: string, "effect": "allow"|"ask"|"deny",
   "id"?: string}`; at least one of `tool`/`command` is required. **The `path`
   dimension is not part of the config rule shape** (O-H1): the shipped loop
   never assigns `PermissionRequest::path`
   (`src/agent/agent_loop.cpp:662-675`), so `path_pattern()` always returns `{}`
   (`src/policy/permission_policy.cpp:117-125`) and any `path` rule would be
   dead code that loads but can never match (`:226-234`). A `path` key in a rule
   object is an unknown key ⇒ `ConfigError` (via `reject_unknown`); the
   `PolicyRule::path` field remains in the shipped struct but is not reachable
   from any config surface or durable grant.
   **A rule that sets `command` MUST also set `tool`; otherwise it is a
   `ConfigError` at load.** Rationale (F-08): `specificity_key` compares the
   non-empty-dimension count first, so a `{command: ...}`-only rule (one dimension,
   no `tool`) ties the one-dimension builtin `{tool:"shell"}` rule and then loses
   the `tool` comparison, silently ignoring the operator's deny. Requiring `tool`
   makes the config rule at least as specific as the builtin it overrides.
   Entries become `PolicyRule`s with `layer = Layer::Project`, `literal = false`,
   and a stable id (`"config.rule.<n>"` when omitted). `Layer::Project` (enum
   value 2) is strictly greater than the builtins' `Layer::Global` (value 1)
   (`include/ymh/policy/permission_policy.hpp:83-91`), so a same-specificity
   config rule wins the layer tiebreak. **F-10 fix (corrected in Rev 3 / G2-M19;
   signature narrowed in Rev 5 / O-H1):** the shipped helper is
   `void validate_rule(const PolicyRule& rule)` in the anonymous namespace
   (`src/policy/permission_policy.cpp:98`, called as `validate_rule(rule)` at
   `:178`). 46-D1 exports `validate_rule(std::string_view tool,
   std::string_view command)` (2-argument — the config surface has no `path`)
   and `validate_glob(const std::string& pattern)` from
   `include/ymh/policy/permission_policy.hpp`; the dimension check (`command`
   without `tool` ⇒ error) lives in the 2-arg form, and the in-policy call site
   `:178` keeps calling the 1-arg `PolicyRule` validator. **O-M4 (Rev 5):** the
   exported validator throws `PolicyConfigError` (the policy layer cannot depend
   on the config layer; the type is at
   `include/ymh/policy/permission_policy.hpp:113-116`). The config parser
   (`apply_permissions`) **catches `PolicyConfigError` and rethrows via
   `fail(source, …)`, which raises `ConfigError`** — so a malformed rule is a
   `ConfigError` at load and maps to exit 2 at the CLI
   (`src/cli/cli.cpp:289`), never a silent default and never an uncaught
   `PolicyConfigError`. `apply_jsonc_file` (the import path) uses the same
   translation.
3. **The master switch.** When `permissions.default == "allow"`,
   `to_permission_config` (`src/cli/wiring.cpp:78-104`) emits the six builtin tool
   rules with `Allow`, emits every MCP `tool_defaults` entry with `Allow`, and
   sets `default_verdict = Allow`. Consequence: **every** tool call runs without
   a prompt — path tools (`read_file`, `grep`, `glob`, `write_file`,
   `edit_file`), `shell`, `git`, and `mcp.*` — **except** where `force_ask`
   applies (46-D1.5, 46-I2 qualified) or a `permissions.rules` deny outranks it
   (46-D1.2).
4. **Scope: path tools are root-confined; command tools are NOT (O-M2, Rev 5;
   corrected from the Rev-4 claim).** The user's wording is "the current repo's
   directory and below". That boundary is enforced **only for path tools**, and
   only because their path arguments go through `ExecutionEnvironment::resolve()`
   (`src/execution/environment.cpp:60-87`; `include/ymh/execution/errors.hpp:4`,
   `:18`), which canonicalizes against `root()` and throws
   `ToolErrorCode::PathEscape` for an out-of-root path. It is **not** an OS
   sandbox — there is no `landlock`/`seccomp`/`chroot`/`unshare`/`setuid` in the
   tree (a repo-wide search finds none) — and:
   - `shell` passes the raw command to `/bin/bash -lc` with
     `cwd = context.root()` (`src/tools/builtin_tools.cpp:381-399`); the child
     process can read or write any path the daemon uid can.
   - `git` runs in-process via libgit2 against the resolved repository.
   - `mcp.*` tools are arbitrary subprocesses launched by `McpManager`.
   Therefore, with `permissions.default = "allow"`, **`shell`/`git`/`mcp.*`
   grant unrestricted command execution as the daemon uid, not repo-confined
   execution.** The key needs no path argument to be directory-scoped *for path
   tools*; it cannot and does not make command tools directory-scoped. This
   residual blast radius is the direct consequence of the user's explicit
   "no asking" choice and is stated in §21 Q15. A future OS-confinement layer
   (landlock/seccomp) is out of scope (§1.4).
5. **Never bypassed.** `SandboxMode::ReadOnly` mutating hard-deny and
   `PermissionRequest::force_ask` are evaluated before rules and are unaffected
   (`src/policy/permission_policy.cpp:205-210`; 25-A11,
   `docs/design/25-ui-ux-errata.md:163`). **Consequence (F-13):** the auto-allow
   switch cannot suppress an `Ask` that `force_ask` produces (e.g. the
   `exit_plan_mode` forced review while plan mode is active).
6. **Precedence with `shell`/`write`/`read`.** When `permissions.default` is
   `"allow"` the three per-tool keys are ignored (all six builtin rules become
   `Allow`). This is intentional — the master switch means "no asking" — and is
   called out in the scaffolded config comment. To keep a targeted prompt, use
   `"default": "ask"` and per-tool keys, or add a specific `permissions.rules`
   deny. **An explicit `permissions.rules` deny also overrides a durable `Always`
   grant (46-D2.13, Rev 6 / NEW-2)** — without that precedence the recommended
   recovery would be inert against a grant, because `Layer::LocalGrant` outranks
   `Layer::Project` in `rule_greater` (`src/policy/permission_policy.cpp:68-79`).
   When `permissions.default` is `"ask"` (the default), the three keys behave
   exactly as today.
7. **`"deny"` default.** `permissions.default == "deny"` sets the fallback to
   `Deny`; the builtin rules keep their `shell`/`write`/`read` verdicts. It is the
   mirror opt-out and is not otherwise special.
8. **No new runtime type.** `PermissionConfig` already carries `rules` and
   `default_verdict`; only the config parser and `to_permission_config` change.
9. **Register completeness (F-11/F-12).** 46-D1 amends `09` decision (f)
   (`docs/design/09-permissions.md:1240-1242`, the TOML `[[permissions.rule]]`
   clause) and adds keys to `21` §3.3/§7.5; `21:226` §1.3.3 is *not* the live key
   table and is not the target.

### 3.3 Failure modes

- A user sets `permissions.default = "allow"` and `permissions.shell = "deny"`
  expecting shell to stay denied: the master switch wins (46-D1.6). Recovery:
  use `permissions.rules` with a `{tool:"shell", effect:"deny"}` rule, or
  `"default": "ask"`. That deny now also revokes a durable `Always` grant
  (46-D2.13, Rev 6 / NEW-2). 46-F1.
- A `permissions.rules` entry with `command` but no `tool` → `ConfigError`
  at load (46-D1.2); never a silently-ignored rule. 46-F21.
- A `permissions.rules` entry with a `path` key → `ConfigError` at load
  (unknown key; the dimension does not exist, O-H1). 46-F21.
- A malformed rule (no match dimension, malformed glob, unknown key) is a
  `ConfigError` at load, never a silent default (46-I1). The exported validator
  throws `PolicyConfigError`; the parser translates it to `ConfigError`
  (O-M4). 46-F33.
- A user enables `permissions.default = "allow"` believing it confines shell to
  the repo: it does not; `shell`/`git`/`mcp.*` run unrestricted as the daemon
  uid (O-M2, 46-D1.4). 46-F34.

**Owning specs amended:** `09` §3.3/§3.5 + decision (f); `21` §3.3/§7.5.
**Tests:** `UI46_D1_DefaultAllowNoPrompts`, `UI46_D1_DefaultAllowShellGitMcp`,
`UI46_D1_DefaultAllowCannotBypassReadOnly`, `UI46_D1_DefaultAllowCannotBypassForceAsk`,
`UI46_D1_DefaultAllowPathToolsNoPrompts`, `UI46_D1_RulesOutrankBuiltins`,
`UI46_D1_CommandOnlyRuleRequiresTool`, `UI46_D1_PathKeyRejected`,
`UI46_D1_CommandDenyBeatsBuiltinAllow`,
`UI46_D1_MalformedRuleIsConfigError`, `UI46_D1_UnknownPermissionKeyIsConfigError`.
**Non-vacuity (gate §E):** the two `ConfigError` tests are non-vacuous only if they
assert the **new** rejection surfaces (`permissions.default`/`rules` keys and the
`tool`-required rule), not the pre-existing unknown-key rejection; the test table
in §20.1 records this. `UI46_D1_CommandDenyBeatsBuiltinAllow` fails on the rev-1
tree (the command-only rule is ignored) and passes only after 46-D1.2.

---

## 4. D2 — Durable `Always` grants, per workspace (item 2)

### 4.1 Current state (verified)

- `AllowAlways` + `Always` is remembered **in memory only**, in the daemon's
  `RulePermissionPolicy`. `src/policy/permission_policy.cpp:257-281`:

  ```cpp
  void RulePermissionPolicy::remember(const PermissionRequest& request,
                                      payload::PermissionDecisionKind kind,
                                      GrantScope scope) {
      if (kind == payload::PermissionDecisionKind::Deny || scope == GrantScope::Once) {
          return;
      }
      PolicyRule grant;
      grant.tool = request.tool;
      grant.path = path_pattern(request);
      grant.command = command_pattern(request);
      grant.effect = PolicyVerdict::Allow;
      grant.id = "grant:" + request.tool;
      std::lock_guard<std::mutex> lock(mutex_);
      if (scope == GrantScope::Always) {
          grant.layer = PolicyRule::Layer::LocalGrant;
          local_grants_.push_back(grant);
      } else {
          grant.layer = PolicyRule::Layer::SessionGrant;
          session_grants_[request.session.value].push_back(grant);
      }
      ++generation_.value;
      reorder();
  }
  ```

  `remember()` returns **`void`** and builds the `PolicyRule` internally. It is
  called from **three** production sites:
  1. the broker — `src/permission/permission_broker.cpp:333`
     (`state->policy.remember(entry->core, kind, grant_scope(params.scope));`);
  2. `PermissionGate::decide` — `src/policy/permission_policy.cpp:413`
     (`policy_.remember(request, decision, scope);`);
  3. the agent loop — `src/agent/agent_loop.cpp:731-736`
     (`services_.policy->remember(request, decision, scope);`).
  Because rev 1 hooked persistence only at the broker, `Always` decisions taken
  through sites 2 and 3 would be remembered in memory but never written (F-05).
- The storage is two in-memory containers, no I/O:
  `include/ymh/policy/permission_policy.hpp:148-159` (`session_grants_`,
  `local_grants_`). The header contract says the policy does no I/O:
  `include/ymh/policy/permission_policy.hpp:3-7` ("The policy is pure: it
  classifies a request and reads its own rule/grant state, performs no I/O …").
- **No persistence exists.** `registry.db` has no grants table
  (`src/registry/registry.cpp:43-103`); `sessions.db` has no grants table
  (`src/session/session_persistence.cpp:35-84`); a repo-wide `grep
  permissions.local` finds only design docs. The durable `permission/decision`
  event is a record, not a grant store (`src/agent/agent_loop.cpp:725-729`;
  `src/core/event.cpp:33`), and no code replays it into the policy.
- The design intended a workspace-local TOML file
  (`docs/design/09-permissions.md:208-226`, `:517-520`, `:1236-1239`), but
  `21-config-jsonc-errata.md:214-224` declares it unimplemented and out of scope,
  and TOML is retired (`AGENTS.md`).
- `force_ask` never records a grant: `src/agent/agent_loop.cpp:731` guards the
  in-process `remember` with `!request.force_ask`; the broker path is likewise
  guarded (`src/permission/permission_broker.cpp:332`); test
  `ForcedReviewNeverRecordsAGrant`,
  `tests/unit/permission_broker_test.cpp:512-533`.
- The broker maps `Always` → `AllowAlways` (`src/permission/permission_broker.cpp:138-156`)
  and calls `remember` guarded by `!force_ask` (`:332-334`); the UI maps its four
  options to `{Once, Session, Always, Deny}` (`src/ui/supervisor.cpp:2194-2240`;
  `tests/unit/supervisor_harness_test.cpp:536-559`).

### 4.2 Decision (46-D2)

**Final answer to item 2 ("If user answered always allowed — is it marked
permanently for the repo/directory?").** **Yes, but the scope is tool + command,
not path** (O-H1). An `Always` answer is written to
`<workspace>/.ymh/permissions.jsonc`, owned by that workspace's daemon, and
reloaded the next time that daemon starts — so it is permanent **for the
workspace (the repo/directory) and only for it** (another workspace re-asks).
Within the workspace:
- for a **command-bearing** tool (`shell`, `terminal`) the grant records the
  **exact approved command** and matches it **literally** (a `"git push*"`-style
  glob is never stored), so it cannot widen to a different command (O-M1);
- for a **path** tool (`write_file`, `edit_file`, `read_file`, `grep`, `glob`)
  the grant is **tool-wide within the workspace** — there is no path dimension,
  because the shipped loop never populates `PermissionRequest::path`
  (`src/agent/agent_loop.cpp:662-675`) and a path-scoped grant would silently
  degrade to tool-wide anyway (`src/policy/permission_policy.cpp:257-280`).
The repo/directory boundary for the **master switch** is provided by the D1
config gate plus `ExecutionEnvironment::resolve()` confinement of path tools —
**not** by grants (O-M2; command tools are not confined). Finer per-path durable
grants would require populating `PermissionRequest::path` in the loop; that is
out of scope and recorded in §21 Q15.

1. **Store.** `<workspace>/.ymh/permissions.jsonc`, a JSONC document with a
   provenance marker, per-entry `effect`, and an explicit match mode:

   ```jsonc
   {
     "schema": "ymh.permissions/1",
     "workspace_id": "3f2a1c04-9e7b-4c11-8a2d-6b1f0c2e7d55",
     "grants": [
       { "tool": "shell", "command": "git push origin main", "match": "literal",
         "effect": "allow", "id": "grant:shell:a1b2c3d4" }
     ]
   }
   ```

   Each entry carries **`tool` and (for command-bearing tools) `command` only**
   — the same dimensions a `remember()`-derived `PolicyRule` actually sets
   (`include/ymh/policy/permission_policy.hpp:76-94`) — **plus an explicit
   `effect`**
   (F-14: `PolicyRule::effect` defaults to `PolicyVerdict::Ask`,
   `include/ymh/policy/permission_policy.hpp:81`; the loader **forces**
   `effect = Allow` on every loaded grant regardless of the stored value, so a
   hand-edited file cannot smuggle a non-Allow effect). **No `path` key is
   accepted**: the loader rejects it (unknown key), mirroring the config rule
   shape (O-H1). Each entry is loaded with `layer = PolicyRule::Layer::LocalGrant`.
   **`match` (O-M1, Rev 5)** ∈ `{"literal","glob"}` defaults to `"literal"`:
   `remember()` always writes `"literal"` and the loader sets
   `PolicyRule::literal = true`, so a grant matches `command_pattern(request)`
   **exactly** and a routine `Always` on `rm -rf build/*` can never match
   `rm -rf build/../../etc`. A hand-authored entry may set `"match": "glob"` to
   opt into the `glob_match` semantics; that is the only way a durable grant is
   a glob. The `.toml` path in `09-permissions.md` is superseded (46-S3); the
   file is **not** a `ConfigPaths` slot (46-S4).
2. **Scope key = the workspace identity, by file location + recorded id (F-16,
   corrected in Rev 3 / G2-M1).** One file per workspace, owned and written only
   by that workspace's daemon (16-daemon-ownership; 09 §3.6 "one writer per
   workspace"). The document records `workspace_id` = the workspace's
   `registry.db` identity (`WorkspaceRecord::id`, a UUIDv4 that is **never the
   path**: `src/registry/registry.cpp:44-46` `CREATE TABLE workspaces (id TEXT
   PRIMARY KEY, canonical_path TEXT NOT NULL UNIQUE, …)`;
   `include/ymh/registry/registry.hpp:65` `WorkspaceId id; // UUIDv4; never the
   path`). At load the daemon resolves its own id with
   `WorkspaceRegistry::findByCanonicalPath(root)`
   (`include/ymh/registry/registry.hpp`, read-only open) and compares; if the
   recorded id does not equal the resolved id — or no registry row exists — the
   file is **ignored** (re-ask). The expected id is a parameter of the grants
   layer (`open_file_grant_store(path, std::optional<std::string>
   expected_workspace_id)`): production passes `record.id.value` (the UUID
   string), M1/tests inject one. A path alone is **not** an identity: a different
   repository later created at the same path has the same canonical path, so the
   rev-2 path check detected relocation only. The id check fires on the repo-swap
   case. This is a residual-risk mitigation, not a cryptographic binding; the risk
   is documented in §21 Q10.
3. **Construction, ownership and load (O-H2, Rev 5 — the seam and the real
   order).** The grants layer is **constructed by the daemon and owned by it**,
   then injected into the runtime as a **non-owning pointer** (exactly like the
   existing `WorkspaceRuntimeOptions::executor`). The pinned seam:
   - `WorkspaceRuntimeOptions` gains `GrantStore* grant_store = nullptr;`
     (`include/ymh/agent/workspace_runtime.hpp:75-104`; the caller owns it and
     must keep it alive for the runtime's lifetime). The daemon holds it in a
     member declared **before** `runtime_`
     (`src/host/workspace_host.cpp:353-354`) so it outlives the runtime.
   - The daemon opens the registry and resolves the id **before** creating the
     runtime. The shipped order is wrong for this: `WorkspaceRuntime::create` is
     called at `src/host/workspace_host.cpp:489-504` while `registry_ =
     WorkspaceRegistry::open(...)` only runs at `:511`, so
     `findByCanonicalPath(root)` does not exist yet. 46-D2 pins the reorder —
     every moved step is independent of the runtime
     (`ensureWorkspaceRegistered` uses only `registry_`, `canonical_root_`,
     `config_.workspace`, `src/host/workspace_host.cpp:420-440`):
     1. `registry_ = WorkspaceRegistry::open(config_.registry)` +
        `resolvePendingMutations()` (moved up);
     2. `ensureWorkspaceRegistered()` (moved up; registers the row);
      3. `const auto record = registry_->findByCanonicalPath(canonical_root_)`
         (`include/ymh/registry/registry.hpp:218`; returns
         `std::optional<WorkspaceRecord>`). **Guard the `optional` (Rev 6 /
         NEW-4):** `ensureWorkspaceRegistered` can return `true` via its first
         branch (`registry_->findById(config_.workspace).has_value()`,
         `src/host/workspace_host.cpp:421-423`) **without** a row at
         `canonical_root_`, so `record` may be `nullopt`; a bare
         `record->id.value` would be undefined behavior. Pin
         `if (!record) { return HostExitCode::RegistryFailed; }` **before** the
         store is opened. (Reachable only via a malformed/hand-run
         `ymh --host` that names an id registered for a different canonical
         path; production supervisors pass a consistent `(id, path)` pair.)
      4. `grant_store_ = open_file_grant_store(canonical_root_ / ".ymh" /
         "permissions.jsonc", record->id.value)`;
     5. build `runtime_options` with `grant_store = grant_store_.get()` and call
        `WorkspaceRuntime::create` (the runtime build, moved down).
   - `WorkspaceRuntime::Impl` (`src/agent/workspace_runtime.cpp:121-122`)
     constructs `policy_(permission_config_, options.grant_store)`. When
     `store != nullptr`, the `RulePermissionPolicy` constructor calls
     `local_grants_ = store->load();` and then `reorder()`
     (`src/policy/permission_policy.cpp:188-202`), so grants are folded before
     any `evaluate()`. The headless `ymh run` path passes
     `grant_store = nullptr` (no human `Always`; persistence disabled), as may
     tests.
   A missing file is empty, not an error. A malformed file, a missing/wrong
   `schema` marker, or a mismatched `workspace_id` is logged and skipped
   (fail-safe: re-ask), never fatal. **Trust rule (F-19):** the
   `"schema": "ymh.permissions/1"` marker is required; a file without it (e.g. a
   repository-shipped `.ymh/permissions.jsonc` committed before ymh ran) is
   ignored. This **raises the bar but is not a trust boundary**: a repo that
   ships the marker is still loaded, because the file is ordinary user-writable
   content (G2-L3). The `.ymh/.gitignore` (`*`, written by ymh) remains a second
   line of defence.
4. **Write.** Only after a **human** decision with `kind != Deny` and
   `scope == Always` (the `AllowAlways`/`Always` pair). **F-05 fix — the seam is
   inside `remember()`:** `RulePermissionPolicy` gains an injected
   `GrantStore*` (nullable; null disables persistence for tests). On the `Always`
   branch, `remember()` builds the grant (`tool`, `command`, `effect = Allow`,
   `layer = LocalGrant`, `literal = true`), **pushes it to `local_grants_` under
   `mutex_`, releases the lock, then calls `store_->append(grant)`** (O-L1: the
   blocking temp+fsync+rename+flock append must not run under `mutex_`, which
   `evaluate()` also takes at `src/policy/permission_policy.cpp:213-218`), and
   returns the created `PolicyRule` (signature changes from `void` to
   `PolicyRule`; **not** `[[nodiscard]]`, so the three existing call sites may
   ignore the return without tripping `-Werror`). Because all three call sites
   already call `remember()`, all three persist — the broker, the gate, and the
   loop. Concurrent `remember()` appends are serialized by the store's own
   `flock(LOCK_EX)` read-modify-write (D2.8), not by `mutex_`. `remember()` stays
   failure-isolated: a failed append never throws and never fails the turn. The
   header contract at `include/ymh/policy/permission_policy.hpp:3-7` is
   **narrowed**: `evaluate()` is still total and performs no I/O; `remember()` is
   the single mutation seam that may perform one best-effort durable append.
5. **Interaction with the other scopes (retained, Q13).** `Once` persists
   nothing (already `return`s at `permission_policy.cpp:260`). `Session` stays
   in-memory for the daemon's lifetime and does not survive a restart (re-ask,
   fail-safe). `Always` is the only scope that touches the file.
6. **`force_ask` never persists (F-05/G2-M5, corrected in Rev 3; seam moved into
   `remember()` in Rev 5 / O-M5).** The guard lives **inside `remember()`
   itself** — the single mutation seam — not only at the call sites: `remember()`
   returns immediately when `request.force_ask` is true, before building or
   appending a grant. This is the only placement that a future fourth call site
   cannot re-open. The existing call-site guards are retained as redundant
   defense: the agent loop (`src/agent/agent_loop.cpp:732`
   `if (services_.policy != nullptr && !request.force_ask)`) and the broker
   (`src/permission/permission_broker.cpp:332` `if (!entry->core.force_ask)`).
   The third, `PermissionGate::decide` (`src/policy/permission_policy.cpp:413`),
   calls `policy_.remember(request, decision, scope)` **unconditionally** today;
   46-D2 pins that it also gains the `!request.force_ask` guard. The rev-2 claim
   that "all three call sites already guard" was false, and the rev-3/4 placement
   at three call sites was fragile by construction (O-M5). (The gate is inert in
   the shipped daemon — `attach_permission_gate=false`,
   `src/host/workspace_host.cpp:493` — but it is live for M1 and tests.)
7. **Write failure is non-fatal.** Log, keep the in-memory grant, and degrade it
   to `Session` (09 §3.6 retained). A failed append never fails the turn.
   **Atomicity (F-15, corrected in Rev 3 / G2-M3):** the write is temp file +
   `fsync(temp fd)` + `rename` + `fsync(parent dir)` into place. This is a **new**
   requirement: the config importer does temp + `rename` but **no** `fsync`
   (`grep -rn fsync src include` = 0; `src/cli/cli.cpp:851-853` opens the temp
   `O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600`, `:894` renames), so it is **not**
   cited as a precedent. `rename` is atomic for visibility but not for durability
   across power loss; the two `fsync`s make the grant durable. A `SIGKILL`
   mid-write cannot leave a truncated file that the loader would read as `{}` and
   thereby discard all prior grants.
8. **Locking (F-17).** The daemon is the single writer, but a supervisor
   spawn/teardown window can momentarily produce two writers (16:753
   acknowledges a concurrent `ymh run`). The grants file is therefore `flock`ed
   (`LOCK_EX`) around read-modify-write; a lock timeout degrades the append to
   `Session` (46-D2.7). This is a lost-update backstop, not a replacement for
   single-writer ownership.
9. **Invalidation.** Deleting `<workspace>/.ymh/permissions.jsonc` drops the
   grants. There is no TTL and no config-change invalidation of *other* kinds;
   grants are additive and independent of the layered config for **allow**
   purposes, **except** that an explicit config **deny** overrides a durable
   grant (46-D2.13, Rev 6 / NEW-2). A management/reset surface is otherwise out of
   scope (§1.4).
10. **No migration.** The `.toml` file was never implemented, so there is nothing
    to migrate. A pre-existing `.toml` is invisible (ymh has no TOML awareness,
    `AGENTS.md`).
11. **JSONC parser seam (F-18, corrected in Rev 3 / G2-M2).** The shipped JSONC
    parser is `Json parse_jsonc(const std::filesystem::path&, std::string_view)`
    (`src/config/config.cpp:101-117`), returning a parsed `Json`; the
    comments-only short-circuit `blank_or_comments_only` (`:52`) is also in the
    anonymous namespace. 46-D2 pins a new shared helper in
    `include/ymh/config/jsonc.hpp` (implementation moved out of the anonymous
    namespace), returning the **parsed document** — not a `std::string`:
    `[[nodiscard]] std::optional<nlohmann::json> parse_jsonc_document(
    std::string_view text, std::string& error)` plus
    `[[nodiscard]] bool blank_or_comments_only(std::string_view text)`. Both the
    config loader and the `GrantStore` call the exported helpers; a comments-only
    document yields `nullopt` with an empty `error` (matching today's no-op
    semantics). The rev-2 sketch pinned a `std::string` return with undefined
    semantics, which cannot deliver "the exact parser" to a store that needs the
    object.
12. **Grant id uniqueness (F-56; updated in Rev 5 / O-H1).** `grant.id =
    "grant:" + request.tool` (`src/policy/permission_policy.cpp:269`) collides
    across multiple grants for one tool; if the store dedups by id, only the last
    survives. 46-D2 pins a content-derived id over the **stored dimensions**:
    `"grant:" + tool + ":" + fnv1a64(command)` (stable across restarts, distinct
    per literal command; `fnv1a64` is a new internal helper). The `path` term is
    dropped because grants have no path dimension (O-H1). Two grants for the same
    tool with the same command are the same grant and dedup correctly.
13. **Revocation: an explicit config `deny` overrides a durable `Always` grant
    (Rev 6 / NEW-2).** The shipped `rule_greater`
    (`src/policy/permission_policy.cpp:68-79`) ranks **specificity → layer →
    effect**, and `Layer::LocalGrant` (5) outranks `Layer::Project` (2)
    (`include/ymh/policy/permission_policy.hpp:83-91`). A durable grant is loaded
    as `Layer::LocalGrant` (46-D2.1) and a config rule as `Layer::Project`
    (46-D1.2), so today a `permissions.rules` deny of equal-or-lower specificity
    can never revoke a grant — only deleting the grants file can. 46-D2 pins the
    precedence: **a matching rule with `effect == Deny` whose layer is a
    non-grant layer (`Builtin`, `Global`, `Project`, `Profile`, `CommandLine`)
    overrides a matching durable grant (`Layer::LocalGrant`, `effect == Allow`),
    regardless of specificity.** The veto is applied in `evaluate()` as an
    explicit post-pass, not by changing `rule_greater` (a non-transitive
    asymmetric comparator would corrupt the ordering): factor the per-rule match
    predicate into `bool rule_matches(const PolicyRule&, const PermissionRequest&)`,
    and after the existing winner loop, if the winner is an `Allow`
    `LocalGrant` and any candidate satisfies
    `rule_matches(c, request) && c.effect == Deny && c.layer != Layer::LocalGrant
    && c.layer != Layer::SessionGrant`, return `Deny`. This makes the D1.6/F1
    recovery ("add a specific `permissions.rules` deny") actually work against a
    durable grant, and is consistent with localcode's `decision: "deny"`, which
    imports as a `Layer::Project` rule (46-D12.6/7 — the mapping itself is
    unchanged). **Config-vs-config precedence (specificity → layer → effect) is
    unchanged**; the veto only ever elevates a matching non-grant deny over an
    `Allow` grant. **Session grants are intentionally out of scope of the veto:**
    a `Session` grant is the human's explicit in-session decision, is not durable,
    and is gone on restart; deleting the grants file or restarting the daemon
    clears it. Deleting the file (46-D2.9) remains the blunt reset.
    **Accepted limitation (see §21 Q16):** a durable path-tool grant is
    tool-wide, so one `Always` on `write_file src/a.txt` auto-allows every
    `write_file` in the workspace, including `.ymh/` and git-hook paths inside the
    root. That is the broadest user-visible consequence of dropping the path
    dimension and is accepted, not incidental.

### 4.3 Failure modes

- Grants file unreadable / malformed / missing marker / mismatched workspace id at
  startup → log, load `{}`, re-ask (fail-safe). 46-F2.
- Append fails (read-only workspace, disk full, lock timeout) → keep the
  in-memory grant, degrade to `Session`, surface nothing into the turn. 46-F3.
- A `SIGKILL` mid-append → the temp file is orphaned, the original is intact
  (atomic rename). 46-F22.
- An `Always` on a **path** tool (`write_file`) is tool-wide within the
  workspace, not path-scoped — by design, because no path dimension exists in a
  durable grant (O-H1; §4.2 item-2 answer). 46-F35.
- A command grant is **literal**, so an `Always` on `rm -rf build/*` does not
  match `rm -rf build/../../etc` (O-M1). 46-F36.
- A grant is persisted for a command that no longer exists → it simply never
  matches; harmless. No automatic pruning (documented).
- A repository ships a pre-seeded `.ymh/permissions.jsonc` without the schema
  marker → ignored (46-D2.3). 46-F23.
- Two writers in a spawn/teardown window → `flock` serializes them; the loser
  degrades to `Session`. 46-F24.
- The daemon cannot resolve its workspace id (registry open/registration fails)
  → startup fails with `HostExitCode::RegistryFailed` before the runtime is
  built; no grants are loaded (O-H2). 46-F37.

**Owning specs amended:** `09` §2.3 Q13, §3.6, decision (e); `21` §1.4 (the
grants file moves from "out of scope" to "in scope").
**Tests:** `UI46_D2_AlwaysPersistsAcrossRestart`,
`UI46_D2_AlwaysPersistsThroughLoop`, `UI46_D2_AlwaysPersistsThroughGate`,
`UI46_D2_OnceNotPersisted`, `UI46_D2_SessionNotPersisted`,
`UI46_D2_ForceAskNotPersisted`, `UI46_D2_ForceAskNotPersistedAtGate`,
`UI46_D2_MalformedGrantsFileReasks`,
`UI46_D2_MissingMarkerIgnored`, `UI46_D2_ForeignWorkspaceIgnored`,
`UI46_D2_AppendFailureDegradesToSession`, `UI46_D2_AtomicReplaceSurvivesKill`,
`UI46_D2_LockContentionDegradesToSession`,
`UI46_D2_DeleteFileDropsGrants`, `UI46_D2_GrantIsWorkspaceScoped`,
`UI46_D2_GrantIdIsContentDerived`, `UI46_D2_GrantCommandIsLiteral` (O-M1),
`UI46_D2_PathToolGrantIsToolWide` (O-H1),
`UI46_D2_StoreInjectedBeforeRuntime` (O-H2),
`UI46_D2_LoadsViaConstructor` (O-H2), `UI46_D2_AppendOutsideMutex` (O-L1),
`UI46_D2_ForceAskGuardInsideRemember` (O-M5),
`UI46_D2_ConfigDenyOverridesDurableGrant` (NEW-2).
**Non-vacuity (gate §E):** `AlwaysPersistsThroughLoop` / `AlwaysPersistsThroughGate`
must drive the **loop** and **gate** paths (not the broker) and assert the file
was written and re-loaded; these fail on the rev-1 tree (no persistence at all).
`GrantCommandIsLiteral` fails on the rev-4 tree (the stored command was a glob);
`StoreInjectedBeforeRuntime` fails on the rev-4 tree (the daemon built the
runtime before the registry).

---

## 5. D3 — The Live switcher with no target becomes a single-OK notice (item 3)

### 5.1 Current state (verified)

- Ctrl+S / Ctrl+P always open the switcher. `src/ui/supervisor.cpp:2628-2634`:

  ```cpp
  if (event == ftxui::Event::CtrlS || event == ftxui::Event::CtrlP) {
      model_.openSwitcher();
      catalog_visible_.store(false);
      if (catalog_ != nullptr) {
          catalog_->refreshNow();
      }
      return true;
  }
  ```

- `openSwitcher()` unconditionally enters `UiMode::Switcher`.
  `src/ui/ui_model.cpp:955-960`:

  ```cpp
  void UiModel::openSwitcher() {
      switcher.source = SwitcherSource::Live;
      switcher.open(*this);
      mode = UiMode::Switcher;
      dirty.markAggregate();
  }
  ```

- `SwitcherOverlayModel::open` includes **every** live-renderable workspace,
  with no exclusion of the active workspace. `src/ui/ui_model.cpp:1010-1013`:

  ```cpp
  for (const auto& [workspace_id, workspace] : model.workspaces) {
      if (!live_switcher_renderable(workspace)) {
          continue;
      }
      WorkspaceNode node;
  ```

  `live_switcher_renderable` is `workspace.live && (daemonStatus == Attached ||
  daemonStatus == Stopping)` (`src/ui/ui_model.cpp:350-353`).
- The focused session is removed from the node's leaves and, when that empties
  the node, `sessions_hidden_by_focus` is set (`src/ui/ui_model.cpp:1027-1051`).
- The renderer then emits the literal `(current session hidden)`.
  `src/ui/ui_render.cpp:788-801`:

  ```cpp
  if (workspace.sessions.empty()) {
      std::string leaf;
      if (!history && workspace.catalog_pending) {
          leaf = "(loading live sessions…)";
      } else if (workspace.note.has_value()) {
          leaf = history_note_leaf(*workspace.note);
      } else if (workspace.sessions_hidden_by_focus) {
          leaf = "(current session hidden)";
      } else if (history) {
          leaf = "(no stored sessions)";
      } else {
          leaf = "(no live sessions)";
      }
      rows.push_back(ftxui::text("    " + leaf) | ftxui::dim);
      continue;
  }
  ```

- The switcher overlay has no message/OK branch; its keys are Esc/CtrlC close,
  j/k and arrows move, Tab expands, Enter focuses/closes
  (`src/ui/supervisor.cpp:2285-2326`). `UiMode` has no message mode
  (`include/ymh/ui/ui_event.hpp:45-51`). **Scope correction (F-20):** a repo-wide
  `grep "No other workspaces"` over **shipped code** (`src/`, `include/`) matches
  nothing; the string appears in `requirements_draft.txt:3` and in this spec. The
  rev-1 claim "matches only `requirements_draft.txt:3`" was self-refuting because
  the spec itself contains the string.
- `modal_owns_input()` enumerates the input-blocking modals
  (`src/ui/supervisor.cpp:1511-1515`):

  ```cpp
  [[nodiscard]] bool modal_owns_input() const {
      return model_.exitConfirm.open || model_.dialog.open ||
             (model_.mode == UiMode::Context && model_.context.open) ||
             model_.mode == UiMode::Switcher;
  }
  ```

### 5.2 Decision (46-D3)

1. **New mode + model.** Add `UiMode::Notice` and:

   ```cpp
   struct MessageDialogModel {
       bool        open = false;
       std::string text;
   };
   ```

   with `UiModel::message` as the single instance. `UiMode` gains `Notice`
   (amending `10-supervisor-tui.md:156`, 46-S10); the shipped enum already has
   `ExitConfirm` and `Context` beyond the pinned three.
2. **Target predicate.** Define `switcher_has_targets()` as: there exists a
   live-renderable workspace whose id differs from `activeWorkspaceId`, **or**
   the active workspace's node has at least one visible session leaf after the
   focused-session exclusion. A workspace that is live-renderable but not the
   active one is a target even when it has no sessions (its node renders its
   placeholder).
3. **Open path.** `openSwitcher()` first evaluates the predicate. If false, it
   sets `message.text = "No other workspaces available"`, `message.open = true`,
   `mode = UiMode::Notice`, and returns — it does **not** build or open the
   switcher (`switcher.open` is not called; `switcher.source`/cursor are
   untouched). Otherwise it behaves exactly as today (and
   `SwitcherOverlayModel::open` still applies the display predicate, 22 §3,
   46-S11).
4. **Whole-popup replacement.** The notice is a centered window that contains the
   message and exactly one option row `[ OK ]` (inverted). No workspace rows, no
   session rows, no placeholder leaf, no j/k footer.
5. **Dismissal.** `Enter` and `Escape` (and `Ctrl+C`) close the notice and return
   to `UiMode::Conversation`; any other key is swallowed and the notice stays up.
6. **Input blocking.** `modal_owns_input()` gains `|| model_.message.open`. No
   keystroke reaches the composer while the notice is up.
7. **Catalog refresh.** The Ctrl+S handler keeps requesting a catalog refresh
   (it is what makes the predicate truthful on the next open). The notice itself
   does not read the catalog.
8. **Exact text.** The message is exactly `No other workspaces available`
   (the user's literal string). The window title is `workspaces` (consistent with
   the switcher window title, `src/ui/ui_render.cpp:845-847`); the title is not
   part of the required text.

### 5.3 Failure modes

- The active workspace is the only live one and has one focused session → the
  notice renders; the switcher never renders the `(current session hidden)` leaf
  alone. 46-F4.
- Another live workspace appears between the predicate and the render (a reply
  lands) → the notice stays up until dismissed; the next Ctrl+S rebuilds the
  switcher. Accepted (the notice is a point-in-time decision). 46-F5.
- The active workspace has a second session → the predicate is true (a session
  leaf is a target) and the switcher renders, so a valid switch is never hidden.

**Owning specs amended:** `10` §7.1/§7.2 + `10:156` (`UiMode`), `22` §3.
**Tests:** `UI46_D3_NoOtherWorkspaceShowsNotice`,
`UI46_D3_NoticeEnterDismisses`, `UI46_D3_NoticeEscapeDismisses`,
`UI46_D3_NoticeSwallowsOtherKeys`, `UI46_D3_OtherLiveWorkspaceShowsSwitcher`,
`UI46_D3_OtherSessionShowsSwitcher`, `UI46_D3_NoticeBlocksComposer`; golden
`UI46_G1_NoticePopup`.

---

## 6. D4 — The permission dialog leaks the transcript; composite it opaquely (item 4)

### 6.1 Current state (verified)

- The permission dialog is a `window` placed over the conversation with `dbox`.
  `src/ui/ui_render.cpp:1204-1217`:

  ```cpp
  Element main = ftxui::vbox(std::move(rows)) | ftxui::border;
  if (model.exitConfirm.open) {
      return ftxui::dbox({main, render_exit_confirm(model, theme)});
  }
  if (model.dialog.open) {
      return ftxui::dbox({main, render_dialog(model, theme)});
  }
  if (model.mode == UiMode::Context && model.context.open) {
      return ftxui::dbox({main, render_context_overlay(model, size, theme)});
  }
  if (model.mode == UiMode::Switcher) {
      return ftxui::dbox({main, render_switcher(model, theme)});
  }
  return main;
  ```

- `render_dialog` returns a bare window with no opaque background.
  `src/ui/ui_render.cpp:620-664` (tail):

  ```cpp
  rows.push_back(ftxui::text("↑/↓ select · Enter confirm · Esc cancel") | ftxui::dim);
  (void)theme;
  return ftxui::window(ftxui::text("permission"), ftxui::vbox(std::move(rows))) | ftxui::center;
  ```

  When the summary is long, `ftxui::paragraph(dialog.summary)`
  (`src/ui/ui_render.cpp:627-635`) widens the window; the option rows
  (`"Allow once"`, `"Allow for session"`, …) and the footer are narrower, so the
  cells to their right are not painted and the `dbox` underneath shows through.
- The **correct** pattern already exists on the context overlay.
  `src/ui/ui_render.cpp:1124-1125`:

  ```cpp
  return ftxui::window(ftxui::text(""), ftxui::vbox(std::move(content))) |
         ftxui::clear_under;
  ```

  `clear_under` is the FTXUI decorator declared at
  `build/_deps/ftxui-src/include/ftxui/dom/elements.hpp:182`.
- The same omission affects the exit confirmation
  (`src/ui/ui_render.cpp:702`: `return ftxui::window(ftxui::text("Exiting"), …) | ftxui::center;`)
  and the switcher (`src/ui/ui_render.cpp:845-847`).

### 6.2 Decision (46-D4)

1. Append `| ftxui::clear_under` to the returned element of `render_dialog`
   (`src/ui/ui_render.cpp:663`), `render_exit_confirm` (`:702`) and
   `render_switcher` (`:845-847`). The `| ftxui::center` placement is retained
   (order: `window(...) | clear_under | center`, matching the context overlay's
   use of `clear_under` before its placement).
2. No layout change: the summary paragraph, the four options, and the footer are
   unchanged. The fix is compositing only.
3. The theme is not consulted for a background colour; `clear_under` uses the
   element's own cells, so the fix is theme-independent. **F-22 correction:**
   `render_dialog` and `render_exit_confirm` already `(void)theme`
   (`src/ui/ui_render.cpp:662`, `:701`); `render_switcher` does **not** — it
   consumes `theme` at `src/ui/ui_render.cpp:782` and `:824`. The claim is scoped
   accordingly: the `clear_under` fix does not require a theme colour in any of
   the three.

### 6.3 Failure modes

- A long summary still widens the window, but the interior is now opaque; the
  transcript no longer bleeds through. 46-F6.
- A very narrow terminal: `clear_under` does not change sizing; existing width
  behaviour is retained.

**Owning specs amended:** `10` §8 (renderers).
**Tests:** golden `UI46_G2_PermissionDialogOpaque` (asserts no conversation cell
appears to the right of each option row on a wide summary),
`UI46_G3_ExitConfirmOpaque`, `UI46_G4_SwitcherOpaque`.
---

## 7. D5 — Alias-aware matching: `/quit` shows the `/exit` help row (item 5)

### 7.1 Current state (verified)

- `complete()` matches canonical names only. `src/ui/command_registry.cpp:88-97`:

  ```cpp
  std::vector<const Command*> CommandRegistry::complete(const std::string& prefix) const {
      std::vector<const Command*> matches;
      for (const Command& command : commands_) {
          if (command.name.size() >= prefix.size() &&
              command.name.compare(0, prefix.size(), prefix) == 0) {
              matches.push_back(&command);
          }
      }
      return matches;
  }
  ```

- `find()` matches canonical names **and** aliases. `src/ui/command_registry.cpp:71-86`.
- `exit` is registered with alias `"quit"` and description `quit the supervisor`.
  `src/ui/command_registry.cpp:272-279`:

  ```cpp
  registry.add(Command{
      "exit", "quit the supervisor",
      [](CommandContext& context, const std::string&) {
          if (context.request_exit) {
              context.request_exit();
          }
      },
      {"quit"}});
  ```

- The hint list is built from `complete()`. `src/ui/supervisor.cpp:2063-2075`:

  ```cpp
  const std::string& draft = state.input.draft;
  if (draft.empty() || draft.front() != '/') {
      return;
  }
  const std::string prefix = draft.substr(1);
  if (prefix.find_first_of(" \t") != std::string::npos) {
      return;
  }
  for (const Command* command : registry_.complete(prefix)) {
      state.command_hints.push_back(CommandHint{command->name,
                                                command_display_name(*command),
                                                command->description});
  }
  ```

- The row text is `"/" + hint.display + " - " + hint.description`
  (`src/ui/ui_render.cpp:373-380`), so `/exit` renders
  `> /exit(quit) - quit the supervisor`. Typing `/quit` produces **no row**
  because `complete("quit")` is empty; `/quit` still executes via `find()`
  (`src/ui/command_registry.cpp:79-85`, `:132-138`). This is exactly item 5.
- 45-D8.2 pins `CommandHint = { name, display, description }`
  (`docs/design/45-ui-interaction-errata.md:951-952`) and 45-D8.5 pins the
  canonical-only rule and the test `UI45_D8_TabCompletesCanonicalName`
  (`docs/design/45-ui-interaction-errata.md:969-971`;
  `tests/unit/command_registry_test.cpp:131-139`).

### 7.2 Decision (46-D5)

1. **New completion API.** Retire `CommandRegistry::complete` (superseding
   45-D8.5) and add:

   ```cpp
   struct CompletionCandidate {
       const Command* command = nullptr;
       std::string    spelling;   // canonical name, or the matched alias
   };

   // 46-D5: matches canonical names first, then aliases; one candidate per
   // command; registration order preserved.
   [[nodiscard]] std::vector<CompletionCandidate>
   complete_candidates(std::string_view prefix) const;
   ```

   Matching is exact-prefix, case-sensitive (unchanged semantics), applied to
   `command.name` first and then to each alias; a command matched by both yields
   one candidate whose `spelling` is the canonical name.
2. **`CommandHint` gains `insert`.** `CommandHint` becomes
   `{name, display, insert, description}` where `name` is the canonical name
   (unchanged, kept for identity), `display = command_display_name(*command)`
   (45-D8 retained), and `insert = candidate.spelling`. **This changes the
   45-D8.2 arity and is declared in 46-S1 (F-09).**
3. **Both construction sites migrate.** `refresh_hints`
   (`src/ui/supervisor.cpp:2057-2076`) and `set_command_hints`
   (`src/ui/supervisor.cpp:2121-2132`) iterate `complete_candidates` and populate
   `insert`. The two-site pin of 45-D2.5 is retained (both sites keep constructing
   `CommandHint`).
4. **Item 5 outcome.** Typing `/quit` now yields the candidate
   `{exit, "quit"}` and therefore the row
   `> /exit(quit) - quit the supervisor` — the same string `/exit` shows.
   `/help` already renders the same row via `command_display_name`
   (`src/ui/command_registry.cpp:280-291`), so no change is needed there.
5. **`/quit` is still not a separate command.** It remains an alias; there is no
   second row. Only matching changes.

### 7.3 Failure modes

- A prefix matches an alias and a different canonical name (e.g. a future alias
  colliding with a command): both candidates are returned in registration order;
  the highlight starts at the first. 46-F7.
- `complete()` is removed; any out-of-tree caller fails to compile (intended — the
  two shipped call sites migrate, 46-D5.3).

**Owning specs amended:** `10` §9.2, `45` 45-D8.2/45-D8.5 (display retained).
**Tests:** `UI46_D5_QuitShowsExitRow`, `UI46_D5_CompleteMatchesAliases`,
`UI46_D5_CompleteDedupesByCommand`, `UI46_D5_CompleteOrderRegistration`,
`UI46_D5_CompleteCanonicalPreferred`; golden `UI46_G5_QuitRowLiteral`.
**Rewrites (F-23):** `UI45_D8_TabCompletesCanonicalName`
(`tests/unit/command_registry_test.cpp:131-139`) and `AliasesAreNotCompleted`
(`:39-43`) assert the **old** canonical-only contract; both are rewritten under
46-D5 to the `complete_candidates` contract.

---

## 8. D6 — Two-press Enter: the first Enter completes, the second executes (item 6)

### 8.1 Current state (verified)

- `complete_selected_command` writes the **canonical** name.
  `src/ui/supervisor.cpp:2096-2118`:

  ```cpp
  bool complete_selected_command(SessionUiState& state) {
      if (state.command_hints.empty()) {
          const std::string& draft = state.input.draft;
          if (draft.empty() || draft.front() != '/' ||
              draft.find_first_of(" \t") != std::string::npos) {
              return false;
          }
          const std::vector<const Command*> matches = registry_.complete(draft.substr(1));
          if (matches.empty()) {
              return false;
          }
          state.input.draft  = "/" + matches.front()->name + " ";
          state.input.cursor = state.input.draft.size();
          state.command_hint_selected = 0;
          return true;
      }
      const std::size_t count = state.command_hints.size();
      const std::size_t index = std::min(state.command_hint_selected, count - 1);
      state.input.draft  = "/" + state.command_hints[index].name + " ";
      state.input.cursor = state.input.draft.size();
      state.command_hints.clear();
      state.command_hint_selected = 0;
      return true;
  }
  ```

- Tab routing: with a command list or a bare `/prefix`, Tab calls
  `complete_selected_command`; on failure it returns false.
  `src/ui/supervisor.cpp:2373-2386`.
- `accept_highlight` (Enter) writes the **canonical** name and only when hints
  exist. `src/ui/supervisor.cpp:2134-2148`:

  ```cpp
  bool accept_highlight(const SessionUiState& state, std::string& text) const {
      if (text.size() < 2 || text.front() != '/' || text == "/") {
          return false;
      }
      if (text.find_first_of(" \t") != std::string::npos) {
          return false;
      }
      if (state.command_hints.empty()) {
          return false;
      }
      const std::size_t selected =
          std::min(state.command_hint_selected, state.command_hints.size() - 1);
      text = "/" + state.command_hints[selected].name;
      return true;
  }
  ```

- The shipped Enter flow accepts-and-dispatches in **one** keypress:
  `src/ui/supervisor.cpp:2355-2372`:

  ```cpp
  if (event == ftxui::Event::Return) {
      std::string text = input.draft;
      if (accept_highlight(*state, text)) {
          input.draft = text;
      }
      if (dispatch_command(text)) {
          input.push_history(text);
          input.draft.clear();
          ...
          return true;
      }
      submit(text);
      return true;
  }
  ```

  So `/e` + Enter rewrites to `/exit` and **immediately exits**. For `/q`,
  `complete("q")` is empty → `accept_highlight` returns false → `dispatch_command`
  runs and appends `unknown command: /q (try /help)`
  (`src/ui/command_registry.cpp:132-136`; `dispatch` is
  `src/ui/command_registry.cpp:118-139`). This is item 6.

### 8.2 Decision (46-D6) — USER DECISION (2026-09-20)

**The user decided: the first Enter completes a partial command, and a second
Enter executes it.** This is pinned as follows.

1. **Exact vs partial.** On `Return`, classify the draft:
   - **Exact command** = the draft's first whitespace-delimited token, without the
     leading `/`, is a canonical name or alias (`CommandRegistry::find` non-null,
     `src/ui/command_registry.cpp:71-86`). An exact command **dispatches** on this
     Enter (unchanged from today), including `/exit`, `/quit`, `/help`, and
     `/plan on` (token `plan` is exact).
   - **Partial command** = the draft is command-shaped (`bare_slash_prefix`:
     starts with `/`, no whitespace, length > 1, `src/ui/supervisor.cpp:331-334`)
     and is **not** exact, and `complete_candidates(draft.substr(1))` is
     non-empty. On this Enter the draft is **rewritten in place** to
     `"/" + spelling + " "` — the same completion Tab performs. To resolve a
     **multi-match prefix** (`complete_candidates("s")` matches `skills`,
     `sessions`, `status`), the Enter path delegates to the same candidate the Tab
     path uses: `complete_selected_command` with `command_hint_selected` (index
     `0` when none has been highlighted), **not** the first candidate
     unconditionally (G2-M18). This preserves the retained 45-D2 contract that
     Enter accepts the highlighted candidate (ArrowDown then Enter inserts the
     highlighted spelling). The cursor moves to the end, hints are refreshed, and
     **nothing is dispatched**.
   - **Anything else** (a plain prompt, or a command-shaped draft with no
     candidate, or a bare `/`) follows today's path: `dispatch_command(text)` if
     it resolves (including the existing `unknown command` / `commands:` notice),
     else `submit(text)`.
2. **The second Enter executes.** After the in-place completion the draft is an
   exact command (e.g. `/quit `), so the next Enter dispatches it. `/q` + Enter →
   `/quit ` (no exit); `/q` + Enter + Enter → exits.
3. **Non-destructive.** Typing `/q` and pressing Enter **must never quit**: the
   completion path does not call `dispatch_command`. This is the user's explicit
   requirement ("the completion must be non-destructive").
4. **Tab is unchanged.** Tab still completes in place via
   `complete_selected_command` (now writing the matched `insert` spelling,
   46-D6.5) and never dispatches. The Enter completion reuses the same function
   (46-D6.1), so a multi-match prefix resolves to the arrow-highlighted candidate
   for both keys.
5. **Insert the matched spelling.** `complete_selected_command` writes
   `"/" + hint.insert + " "` in both branches (empty-list recompute and
   selected-hint) using `complete_candidates` (46-D5); `accept_highlight` is
   retired (its role is subsumed by the Enter classifier). `/q` + Tab → `/quit `.
6. **Interface.**

   ```cpp
   // src/ui/supervisor.cpp (private)
   enum class EnterAction : std::uint8_t { Dispatch, Complete, Submit };
   // 46-D6: exact -> Dispatch; partial-with-candidates -> Complete (rewrite only);
   // otherwise -> Submit.
   [[nodiscard]] EnterAction classify_enter(const SessionUiState& state,
                                            const std::string& draft) const;
   ```

   The `Return` handler calls `classify_enter`; `Dispatch` runs the existing
   `dispatch_command` (or `submit` if it returns false), `Complete` runs the
   in-place completion and returns, `Submit` runs `submit`.
7. **Supersession.** This changes today's one-keypress accept-and-dispatch for
   **partial** drafts (46-S12): `/e` + Enter no longer exits; `/q` + Enter no
   longer prints `unknown command`. Exact commands are unchanged.

### 8.3 Failure modes

- `/q` + Enter → `/quit ` (no dispatch); the user presses Enter again to exit.
  46-F8.
- `/q` + Tab inserts `/quit `; if the user then edits to an unknown command,
  Enter appends the existing `unknown command` notice (unchanged).
- A partial draft with no candidates (e.g. `/zz`) → falls through to
  `dispatch_command`, which appends `unknown command: /zz (try /help)`
  (unchanged). 46-F25.
- A prefix that matches only via alias completes to the alias spelling; `find()`
  resolves it (46-I11).
- **Documented asymmetry (G2-L18):** an **exact** command/alias (`/quit`,
  `/exit`) dispatches on the **first** Enter, while a partial (`/q`) needs two.
  This is intended (46-D6.1) and is called out here so the UX is not read as a
  bug.

**Owning specs amended:** `10` §9.2, `45` §6.1/45-D2 (one-keypress
accept-and-dispatch for partial drafts superseded), `25-D9`/UX-U15
(`accept_highlight` retired; G2-M17).
**Tests:** `UI46_D6_QCompletesToQuit`, `UI46_D6_QTabInsertsSpelling`,
`UI46_D6_QEnterCompletesOnly`, `UI46_D6_QTwoEnterExecutes`,
`UI46_D6_ExactCommandDispatchesOnFirstEnter`,
`UI46_D6_MultiCandidateEnterUsesHighlight` (G2-M18),
`UI46_D6_QuitExactOneEnter`,
`UI46_D6_CanonicalPrefixInsertsCanonical`, `UI46_D6_CompleteRetired`
(compile-level: `complete` is gone); **rewrite**
`UX_U15_EnterAcceptsHighlightAndDispatches`
(`tests/unit/supervisor_harness_test.cpp:1037`; G2-M17). **Non-vacuity:**
`QEnterCompletesOnly` fails on the rev-1 tree (rev 1 dispatched on the first
Enter); it passes only after 46-D6. The shipped
`UI45_D8_TabCompletesCanonicalName` / `AliasesAreNotCompleted` are rewritten
(46-D5).

---

## 9. D7 — A fresh launch opens a clean session (item 7)

### 9.1 Current state (verified)

- On `Attached`, `on_link_state` calls `refresh_sessions`
  (`src/ui/supervisor.cpp:1253-1263`), which issues `session.list`
  (`:1295-1301`). The reply handler builds `stored`/`live` (`:1302-1317`) and then
  chooses focus. `src/ui/supervisor.cpp:1337-1351`:

  ```cpp
  if (it->second.activeSessionId().value.empty() &&
      model_.activeWorkspaceId == workspace) {
      if (!live.empty()) {
          // A live session can be focused directly.
          activate_session(workspace, live.front().first);
          // 25 review M3: plain attach must refresh the context.
          refresh_status_context(workspace, live.front().first);
      } else if (!stored.empty()) {
          resume_after_attach(workspace, stored.front().first);
      } else {
          // Attach (existing daemon) and spawn paths both
          // converge here, so auto-create the first session.
          create_session(workspace, std::string{});
      }
  }
  ```

- `stored.front()` is the **earliest-created** stored session: the daemon orders
  sessions by `ordinal ASC` in `Registry::listSessionsOrdered` →
  `orderSessions` (`src/host/host_runtime.cpp:531-532`;
  `src/registry/registry.cpp:609-644`), and `ordinal` is `MAX(ordinal)+1` at
  creation (`src/registry/registry.cpp:1221-1228`). This is the "opens a previous
  workspace by default" behaviour.
- `resume_after_attach` submits `session.resume` and focuses on success
  (`src/ui/supervisor.cpp:1042-1067`).
- **The explicit already-attached path (verified, N1).** `resume_from_history`
  (`src/ui/supervisor.cpp:1032-1040`) is the single Ctrl+S / `/sessions` session
  entry point: for a workspace that is already `Attached` it calls
  `resume_after_attach(workspace, session)` directly at `:1036` and returns; only
  the not-yet-attached branch calls `ensure_workspace_running` (`:1039`). Neither
  branch sets any marker today, and `resume_after_attach` itself sets none — so
  the explicit path is currently indistinguishable from the startup
  `pending_resume_` path at the `session.list` reply branch.
- **The `recover_unknown_session` early return (verified, N2; superseded in Rev
  6 / NEW-1).** `recover_unknown_session`
  (`src/ui/supervisor.cpp:1121-1133`) returns early at `:1124-1127` when the
  workspace is no longer modeled. The Rev-4/Rev-5 answer placed the marker erase
  **before** that return; Rev 6 instead removes the marker from this helper
  entirely (it has non-resume callers — `prompt` `:1450`, `agent.select`
  `:1907` — that never incremented the refcount), so the release happens in the
  resume's `!reply.ok` terminal before the `unknown`/`surface_notice` split. The
  early return is therefore no longer on any marker path. §9.2 item 2.
- `--resume` is a separate explicit path: `options.initial_resume` is set only
  from a non-empty `--resume` (`src/cli/cli.cpp:470-485`, `:543-545`); the
  no-args `Tui` command leaves it unset.
- **The race (F-07).** `on_link_state` (`:1263`) calls `refresh_sessions`
  (asynchronous) and then **synchronously** consumes `pending_resume_` and calls
  `resume_after_attach` (`:1271-1276`). The `session.list` reply branch
  (`:1337-1351`) tests **only** `activeSessionId().value.empty()` — it never
  checks `pending_resume_`. If the list reply lands before the resume reply,
  `activeSessionId` is still empty, so the `stored.empty()` fall-through calls
  `create_session(workspace, {})` and a stray empty session is created — the exact
  anti-stray condition pinned by 23 §6.5
  (`docs/design/23-session-lifecycle-errata.md:1253-1293`; 23-D58 at `:375`).
- Spec 23 pins the eager auto-create and the zero-stored gate
  (`docs/design/23-session-lifecycle-errata.md:96-115`, `:375`, `:1253-1293`).

### 9.2 Decision (46-D7)

1. **Remove the resume-earliest branch.** Replace
   `else if (!stored.empty()) { resume_after_attach(workspace, stored.front().first); }`
   with the create branch, so the branch body becomes:

   ```text
   if (activeSessionId empty && active workspace == workspace
       && no pending resume for this workspace) {
       if (!live.empty())   -> focus live.front()              (retained)
       else                 -> create_session(workspace, "")   (was: resume stored.front())
   }
   ```

   A fresh launch therefore opens a clean, empty session.
2. **Close the race (F-07; corrected in Rev 3 / G2-H1; insertion site corrected
   in Rev 4 / N1).** The rev-2 gate on `pending_resume_` cannot work: the shipped
   callback consumes the pending entry **synchronously before** the resume request
   is submitted (`src/ui/supervisor.cpp:1271-1276`: `pending_resume_.erase(pending);
   resume_after_attach(workspace, session);`), while the `session.list` reply
   submitted at `:1263` can only be processed **after** that callback returns. So
   `pending_resume_.find(workspace) == end()` is always true in the reply branch,
   and the rev-2 guard never fires. The fix is a marker that outlives the erase:

   - Add a private `std::map<WorkspaceId, std::size_t> resume_in_flight_;` to the
     supervisor (next to `pending_resume_`, `src/ui/supervisor.cpp:2773`). A
     **refcount per workspace** (not a set) is required because
     `resume_after_attach` is re-enterable: `resume_from_history`
     (`src/ui/supervisor.cpp:1032-1040`) is not de-duplicated, so a second
     Ctrl+S/`/sessions` selection can start a second resume while the first is
     outstanding (O-M6). A set cannot represent two; the first terminal reply
     would clear the gate while the second resume is still in flight, and a
     `session.list` reply could then take the focus/create decision — the exact
     stray session D7 exists to prevent.
   - **Increment at the top of `resume_after_attach` (`:1042`)**
     (`++resume_in_flight_[workspace];`). This is the **single** insertion site:
     because *every* resume path funnels through `resume_after_attach` — the
     `pending_resume_` consume site (`:1275`), the explicit already-attached
     `resume_from_history` branch (`:1036`), and the `--resume` startup path
     (which reaches the consume site via `pending_resume_`) — the marker is set
     before any `session.resume` is submitted on all of them. No call site
     touches the marker itself, so no future resume path can omit it. This
     supersedes the Rev-3 set pin and the Rev-4 idempotent-insert pin.
   - **Release at the two genuine resume terminals only, through a guarded
     private helper (Rev 6 / NEW-1).** The decrement lives **only** where a
     matching increment did. A private `release_resume(workspace)` decrements the
     entry **iff it exists and is `> 0`**, erasing it at zero; a call with no
     entry (or a zero entry) is a no-op, so no `std::size_t` can wrap. It is
     called from exactly two places:
     1. the success terminal `apply_resume_success` (`:1090`) — on entry, before
        the "workspace no longer open" early return at `:1091-1095`; a single
        entry decrement covers both exits.
     2. the `!reply.ok` failure terminal inside `resume_after_attach`
        (`:1053-1059`) — on entry to the `enqueue` lambda, before the
        `unknown`/`surface_notice` split, so both sub-exits (`recover_unknown_session`
        and `surface_notice`) are covered.
     **`recover_unknown_session` (`:1121`) does NOT touch the marker** — this
     supersedes the Rev-4/Rev-5 pin. The helper has **two non-resume production
     callers**: `prompt()`'s `agent.prompt` `UnknownSession` branch
     (`src/ui/supervisor.cpp:1450`) and the `agent.select` `UnknownSession`
     branch (`:1907`). Neither incremented the refcount. A decrement inside the
     helper would drive a zero entry to `SIZE_MAX` (or default-construct a zero
     and then wrap), leaving `resume_in_flight_.count(workspace) == 0` false
     **forever** for that workspace and permanently disabling the D7.2
     focus/create branch (`:1337-1350`) — a regression of the exact bug D7 exists
     to fix. Moving the decrement into the `!reply.ok` terminal (co-located with
     the increment at the top of `resume_after_attach`) plus the presence guard in
     `release_resume` makes the non-resume callers inert **by construction**:
     they neither increment nor decrement.
     The gate tests `resume_in_flight_.count(workspace) == 0` (no outstanding
     resume). No other function touches the marker. The helper contract is
     `release_resume`: "if the entry exists and is `> 0`, decrement; erase at 0;
     otherwise no-op."
   - **The reply guarantee is now structural, not assumed (O-H3, Rev 5).** The
     Rev-4 claim that `SupervisorConnection::submit` "always delivers a reply on
     disconnect" was **false**. `process_requests`
     (`src/ui/supervisor_connection.cpp:376-406`) swaps `requests_` into a local
     `batch` and, after detecting a dead connection following a reply, calls
     `handle_disconnect(...)` and `return`s — abandoning every remaining
     `PendingRequest` in `batch` **with no reply**. `handle_disconnect`
     (`:431-440`) closes the socket and clears subscriptions but does **not**
     drain `requests_` or the batch; the drain at `:505-511` runs only on
     `pump()` stop. A `session.resume` queued behind a failing `session.list`
     therefore never got a reply, and the marker leaked permanently. 46-D7 pins
     the fix in the connection: **reply `{ok=false, error="connection lost"}`
     to every remaining request in the local `batch` before `process_requests`
     returns, and drain `requests_` in `handle_disconnect` with the same reply.**
     With that, "a reply is always delivered" is true by construction for
     timeout, stop, overflow and disconnect, so the refcount decrement runs on
     every path. The connection-drain test is
     `UI46_D7_DisconnectDrainsPendingResume`.
   - **Gate** the `session.list` reply branch (`:1337`) on
     `resume_in_flight_.count(workspace) == 0` **in addition to** the existing
     `activeSessionId().value.empty() && model_.activeWorkspaceId == workspace`.
     While any resume is in flight the handler does **not** take the
     focus/create decision; `apply_resume_success` owns the focus. The `stored`
     vector no longer drives startup focus.

   The test must force the list reply to land **between** the resume submit and
   its reply (not merely "an early list reply"), which the rev-2 test name
   implied but could not exercise with an erased map. The injection point is
   pinned in §16/§20.1 (`UI46_D7_ResumeInFlightBeatsListReply`); no timing or
   `sleep` is used. The overlapping-resume case is
   `UI46_D7_OverlappingResumesKeepGateClosed`; the disconnect case is
   `UI46_D7_DisconnectDrainsPendingResume`.
3. **`stored` is removed from the reply handler (F-24).** After 46-D7 the reply
   handler's `stored` is dead: the cell loop iterates `live` only
   (`src/ui/supervisor.cpp:1329-1335`) and `/sessions` reads directly from disk
   (22 §4). The `stored` accumulator is deleted; `session.list` still enumerates
   every stored session (its pinned contract) but the supervisor only tracks the
   `live` subset. `activate_session`/`refresh_status_context` for `live.front()`
   are retained.
4. **Explicit resume paths are unchanged, and are now covered by the marker.**
   `--resume <id>` (`src/cli/cli.cpp:543-545`), a Ctrl+S/History selection
   (`resume_from_history`, `src/ui/supervisor.cpp:1032-1040`), and `/sessions`
   selection still call `resume_after_attach`/`session.resume` (22 §5.2,
   `docs/design/22-switcher-sessions-errata.md:1298-1319`). Because 46-D7.2
   increments `resume_in_flight_` **inside** `resume_after_attach`, the explicit
   already-attached branch (`:1036`) increments the refcount too, so I14's
   coverage claim is now true (N1), and a second overlapping selection
   increments it again so the gate stays closed until **both** resolve (O-M6).
5. **Eager create is retained.** The chosen reading of "clean new session (or
   even an empty one until first prompt)" is the eager empty session, consistent
   with 23-D58 (`docs/design/23-session-lifecycle-errata.md:96-109`, `:375`). The
   "no session until first prompt" alternative is rejected to avoid re-opening
   23's eager-create contract; recorded as Q4 (§21).
6. **The design comment is updated.** The comment at
   `src/ui/supervisor.cpp:1286-1294` ("the first stored session is resumed … so
   the focus is always a usable, live session") is rewritten to say a fresh
   session is created. The "focus is always usable" property is preserved by
   `create_session`.

### 9.3 Failure modes

- `session.create` fails → the existing create-reply failure path surfaces a
  notice; focus stays empty and the first keystroke's first-keystroke path
  (`docs/design/23-session-lifecycle-errata.md:112-115`) retries. 46-F9.
- `--resume` with the `session.list` reply arriving first → the reply branch is
  skipped (46-D7.2); the resume reply focuses the resumed session; no stray
  session. 46-F26.
- A resume fails with a **non-transport, non-`UnknownSession`** error (e.g. the
  daemon returns `Internal`): the failure branch surfaces `"resume failed: …"`,
  decrements the refcount (erasing at zero), and leaves the workspace with no
  focused session (N9). No later `session.list` is guaranteed on this path, so
  the recovery is the same first-keystroke create path as the create-failure case
  above (`docs/design/23-session-lifecycle-errata.md:112-115`); the D7.2
  rationale is corrected accordingly (it previously assumed a later list reply).
  46-F32.
- An `UnknownSession` reply for a workspace evicted between submit and reply:
  the resume's `!reply.ok` terminal calls `release_resume` **before** the
  `unknown`/`surface_notice` split, so the refcount is released even when
  `recover_unknown_session` then takes its workspace-gone early return (N2).
  `recover_unknown_session` itself never touches the marker, so a
  `prompt`/`agent.select` `UnknownSession` (no resume in flight) leaves the
  count at zero and the D7.2 focus/create branch still runs (Rev 6 / NEW-1).
  46-F32.
- **The connection drops with a resume queued behind another request (O-H3):**
  `SupervisorConnection::process_requests`/`handle_disconnect` now reply
  `{ok=false, error="connection lost"}` to every pending request, so the resume's
  `!reply.ok` branch runs and decrements the refcount; the workspace is left with
  no focus and the first keystroke's create path recovers. Before Rev 5 this
  dropped the request and leaked the marker permanently. 46-F38.
- **Two overlapping resumes (O-M6):** the refcount stays ≥ 1 until both terminal
  replies run, so a `session.list` reply cannot take the focus/create decision
  in between; no stray session. 46-F39.
- A user who expected their last conversation restored must use `--resume` or
  Ctrl+S/`/sessions`; the startup change is intentional (item 7).

**Owning specs amended:** `10` §2.3/§5.4, `22` §5.2, `23` §1.1/§6.5.
**Tests:** `UI46_D7_FreshStartCreatesEmptySession`,
`UI46_D7_NoResumeOfStored`, `UI46_D7_ResumeFlagStillResumes`,
`UI46_D7_ResumeInFlightBeatsListReply`, `UI46_D7_LiveSessionStillFocused`,
`UI46_D7_CreateFailureNotice`, `UI46_D7_ExplicitResumeSetsMarker` (N1),
`UI46_D7_UnknownSessionEvictedClearsMarker` (N2),
`UI46_D7_NonTransportResumeFailureFirstKeystrokeRecovers` (N9),
`UI46_D7_OverlappingResumesKeepGateClosed` (O-M6),
`UI46_D7_DisconnectDrainsPendingResume` (O-H3),
`UI46_D7_NonResumeUnknownSessionDoesNotDecrement` (Rev 6 / NEW-1).
**Non-vacuity:** `ResumeInFlightBeatsListReply` fails on the rev-1 tree (the early
list reply creates a stray session) and passes only after 46-D7.2; on the rev-3
tree it is the explicit-path variant that fails (N1).
`OverlappingResumesKeepGateClosed` fails on the rev-4 tree (a set clears on the
first terminal reply); `DisconnectDrainsPendingResume` fails on the rev-4 tree
(the batch is abandoned without a reply).

---

## 10. D8 — A deadline for every tool run (item 8)

### 10.1 Current state (verified)

- The tool call is a **synchronous** call whose result is already computed.
  `src/agent/agent_loop.cpp:767-772`:

  ```cpp
  StaticPermissionHandle handle(plan.decision);
  ToolContext context(*services_.execution, session_, *services_.logger, plan.token,
                      *services_.governor, *services_.output, handle, plan.call.id,
                      plan.call.turn, plan.call.step);
  try {
      result = services_.tools->execute(plan.call, context).get();
  ```

- `execute` returns `Task<ToolResult>`
  (`include/ymh/tools/tool_registry.hpp:92-93`), and `Task<T>` is **eager**: the
  value is present when the call returns, so `.get()` never waits
  (`include/ymh/core/task.hpp:28-49`; `:42-43` "An eager task is ready as soon as
  it is returned"). The blocking happens **inside** `execute()`.
- Exclusive tools run on the loop thread; parallel-safe tools run via
  `std::async(std::launch::async, …)` and are joined by `drain_all`
  (`src/agent/agent_loop.cpp:1207-1212` defines `drain_all`; `:1254` is the
  `std::async` call).
- `ToolConfig` has no deadline and is **not** parsed from JSON; it is
  default-constructed (`include/ymh/execution/config.hpp:14-38`;
  `src/agent/workspace_runtime.cpp:107-118`, `:232`). The `[tools]` section only
  carries presentation (`include/ymh/config/config.hpp:172-176`;
  `src/config/config.cpp:807-818`).
- **`ToolConfig` is invisible to the agent loop (F-03).** `AgentServices` exposes
  `ExecutionEnvironment* execution` but no `ToolConfig` and no timeout accessor
  (`include/ymh/agent/agent_loop.hpp:50-84`); `ToolConfig config = {}` is a
  **constructor parameter only** and no `config()`/`toolConfig()` accessor exists
  (`include/ymh/execution/environment.hpp:51-74`). `grep ToolConfig
  src/agent/agent_loop.cpp` returns zero hits. The rev-1 sketch could not compile.
- The resource governor exposes caps only, no per-tool deadlines
  (`include/ymh/execution/resource_governor.hpp:20-29`).
- Existing per-primitive timeouts:
  - `ProcessRequest.timeout` where **`0` means "no deadline"**
    (`include/ymh/execution/process.hpp:26-42`, `:31`
    `std::chrono::milliseconds timeout{0}; // 0 => no deadline`), used by `shell`
    from a model-supplied `timeout_ms`
    (`src/tools/builtin_tools.cpp:365-409`, `:391-394`).
  - PTY: `read` waits only when `wait.count() > 0`
    (`src/execution/pty.cpp:282`), and `wait(timeout)` blocks indefinitely when
    `timeout.count() == 0` (`src/execution/pty.cpp:311`). So PTY `0` = block.
  - MCP: `options.timeout.count() > 0 ? options.timeout : config_.call_timeout`
    (`src/mcp/mcp_client.cpp:206-207`); MCP `0` = fall back to the 60 s default
    (`config_.call_timeout`).
  - `git` is **in-process via libgit2**, with no clampable primitive:
    `context.execution().git().status(query).get()`
    (`src/tools/git_tools.cpp:65`) and `.diff(query).get()`
    (`src/tools/git_tools.cpp:98`). A grep of `src/tools` for
    `ProcessRequest|process().run` matches only `builtin_tools.cpp:397`.
- `ToolErrorCode::Timeout` already exists
  (`include/ymh/execution/errors.hpp:15-24`, `:21`).
- `ToolContext` carries the per-call state but no deadline and no `<chrono>`
  include (`include/ymh/tools/tool_context.hpp:1-66`, `:7-15`).

### 10.2 Decision (46-D8)

#### 10.2.1 Config

1. **Config key `tools.timeout_ms`** — integer milliseconds, default `300000`
   (5 min), `0` disables. Added to `ToolsSettings` and the `apply_tools`
   allow-list (`src/config/config.cpp:807-818`), and mapped into
   `ToolConfig::tool_timeout` in the daemon wiring
   (`src/agent/workspace_runtime.cpp:107-118`). A negative value is a
   `ConfigError`; `0` disables (never an immediate timeout).
2. **Register (corrected in Rev 3 / G2-M4).** 46-D8 amends `07` §5.5 and records
   the key in `21` §3.3 (the live key table, `21:372`). There is **no** `tools`
   §7.x subsection in `21` today — `21:1220` is §7.5 `permissions`, not `tools`
   (`grep -n "tools" 21-config-jsonc-errata.md` finds only MCP
   `allowed_tools`/`denied_tools`; the code's `tools` allow-list is
   `{"presentation","tool_order"}`, `src/config/config.cpp:807-818`). 46-D8
   therefore **adds** a new `21` §7.12 `tools` subsection (after §7.11 `skills`)
   as an explicit amendment. `21:226` §1.3.3 is **not** the live key table and is
   not the target (F-28). The rev-1 `09 §4.5 (timeout policy)` amendment is
   dropped: `09:718-733` is the permission-`Ask` deadline (`expires_at_ms`),
   unrelated to tool runtime (F-27).

#### 10.2.2 The deadline representation (F-02)

3. **`ToolContext` carries an optional absolute deadline.** Add to
   `include/ymh/tools/tool_context.hpp` (and `#include <chrono>`,
   `#include <optional>`):

   ```cpp
   // 46-D8: absolute deadline for this call; nullopt == no deadline (disabled).
   // A default-constructed time_point is NOT a valid "disabled" sentinel
   // (steady_clock's epoch is in the past); the optional is the sentinel.
   std::optional<std::chrono::steady_clock::time_point> deadline_;   // private

   [[nodiscard]] bool has_deadline() const noexcept;                 // deadline_.has_value()
   [[nodiscard]] std::chrono::steady_clock::time_point deadline() const;  // precondition: has_deadline()
   [[nodiscard]] std::chrono::milliseconds remaining() const noexcept;    // total (see below)
   [[nodiscard]] bool expired() const noexcept;                      // has_deadline() && now >= *deadline_
   ```

   `remaining()` is **total** and unambiguous:
   - `!has_deadline()` → `std::chrono::milliseconds::max()` (disabled ⇒ never expires);
   - else → `std::max(std::chrono::milliseconds::zero(),
     std::chrono::duration_cast<std::chrono::milliseconds>(*deadline_ - now()))`
     (the `duration_cast` is required: `time_point - time_point` is
     `steady_clock::duration`, not `milliseconds`, so a bare `std::max` against
     `milliseconds::zero()` fails template deduction — G2-L9). Clamped at 0; never
     negative.

   This resolves the rev-1 contradiction where a default-constructed
   `time_point{}` (the clock epoch, in the past) made "disabled" behave as
   "always expired" (F-02).
4. **The loop computes the deadline.** `tool_timeout == 0` ⇒ `std::nullopt`
   (disabled); else `now + tool_timeout`. It obtains the value through the new
   accessor (46-D8.5).

#### 10.2.3 The clamp rule (F-01)

5. **`ToolConfig` becomes visible to the loop (F-03).** Add to
   `ExecutionEnvironment` (`include/ymh/execution/environment.hpp:28-44`):

   ```cpp
   [[nodiscard]] virtual const ToolConfig& toolConfig() const noexcept = 0;
   ```

   `LocalEnvironment` stores `ToolConfig config_` (it already receives one as a
   constructor parameter, `include/ymh/execution/environment.hpp:51-54`) and
   overrides the accessor. The loop computes the deadline from
   `services_.execution->toolConfig().tool_timeout` at the `ToolContext`
   construction site (`src/agent/agent_loop.cpp:768-770`). `agent_loop.hpp` and
   `tool_context.hpp` already transitively include `environment.hpp`, which
   includes `execution/config.hpp` (`include/ymh/execution/environment.hpp:14`).
6. **One total clamp helper.** Pin a free function (new header
   `include/ymh/execution/deadline.hpp`):

   ```cpp
   // 46-D8: the effective primitive timeout for a call.
   //   nullopt            -> no deadline (the primitive's own semantics apply)
   //   milliseconds{0}    -> the deadline is already expired: time out IMMEDIATELY
   //   milliseconds{>0}   -> the budget to wait
   // Rules: expired => 0 (never nullopt/0-as-disabled); caller == 0 => remaining;
   //        else min(caller, remaining).
   [[nodiscard]] std::optional<std::chrono::milliseconds>
   clamp_timeout(const std::optional<std::chrono::steady_clock::time_point>& deadline,
                 std::chrono::milliseconds caller_ms,
                 std::chrono::steady_clock::time_point now);
   ```

   The rule is a function of an **expired** deadline, not a raw `min`:
   - `!deadline` → `nullopt`;
   - `remaining = max(0, *deadline - now)`; if `remaining == 0` → `0ms`
     (immediate timeout);
   - else if `caller_ms == 0` → `remaining`;
   - else → `min(caller_ms, remaining)`.

   The rev-1 `min(x, remaining)` was inverted by the shipped `0 == no deadline`
   convention: `min(x, 0) == 0`, which `shell` reads as "no deadline", PTY as
   "block forever", and MCP as "the 60 s default". The pre-dispatch guard only
   narrows the race; it cannot close it, so the primitive itself must receive an
   explicit "expired" signal (F-01).
7. **Apply the clamp explicitly to each primitive.** Every primitive receives the
   result of `clamp_timeout(...)`; no primitive re-derives the budget.
   - **process (`shell`)**: add an additive
     `std::optional<std::chrono::milliseconds> deadline` to `ProcessRequest`
     (`include/ymh/execution/process.hpp:26-42`; `timeout` is retained). The
     effective wait is exactly `clamp_timeout(deadline, request.timeout, now)` —
     the process is **not** given a separate `min(timeout>0 ? timeout : ∞,
     deadline)` formula (the rev-2 `∞` was a non-existent `milliseconds` value
     and duplicated the helper; G2-L16). `nullopt` → today's behaviour
     (`timeout`); `0` → the process is **not started** and a
     `ProcessResult{timed_out = true}` (or `ToolError{Timeout}`) is returned
     immediately; `>0` → wait at most the clamped budget and then terminate
     (grace → SIGKILL, `include/ymh/execution/config.hpp:32-33`).
   - **PTY (corrected in Rev 4 / N3).** `PtySession::read` and `PtySession::wait`
     (`include/ymh/execution/pty.hpp:351-356` — the deadline methods are on
     `PtySession`, **not** `PtyService`, which exposes only
     `open`/`find`/`list`/`closeSession`/`available`/`closeAll`, `:376-387`) gain
     an additive `std::optional<std::chrono::milliseconds> deadline` parameter
     carrying the `clamp_timeout` result. The existing `wait`/`timeout` parameter
     is **unchanged and passed as before**; the deadline is a separate parameter,
     never folded into `wait_ms`. The result is **representable**: 46-D8 adds
     `bool timed_out = false;` to `PtyRead` (`pty.hpp:95-100`) and to `PtyExit`
     (`:53-57`), since neither has one today (G2-M8). The trichotomy is pinned
     exactly as `clamp_timeout` defines it, so an **expired** deadline can never
     be expressed as a `0` `wait_ms` (which today means "poll", `read`, and
     "block forever", `wait`):
     - `nullopt` (disabled) → **today's semantics exactly**: `wait_ms`/`timeout`
       is the only bound (`read`: `0` = non-blocking poll; `wait`: `0` = block
       until exit), and `timed_out` stays `false`. This is the "disabled maps to"
       answer for PTY: disabled is the absent optional, never a `0` value.
     - `0ms` (expired) → return **immediately** with `timed_out = true` and empty
       `data`; the `wait_ms`/`timeout` argument is not consulted, so the
       `count() == 0` branches (`src/execution/pty.cpp:282` for `read`,
       `:311` for `wait`) are bypassed for the expired case.
     - `>0ms` (active budget) → wait at most `*deadline`, combined with the
       existing bound: `read` uses `min(wait_ms == 0 ? 0 : wait_ms, *deadline)`
       (a `wait_ms == 0` poll stays a poll — the deadline never extends a poll),
       and `wait` uses `min(timeout == 0 ? *deadline : timeout, *deadline)` (an
       unbounded `timeout == 0` becomes the budget). `timed_out` is set **iff the
       deadline budget was the binding bound and no data/eof/cancel occurred**;
       it is derived from the new `deadline` parameter only, never inferred from
       `wait_ms == 0`.
     `terminal_tool.cpp` computes the clamp with the context's optional deadline
     and `caller_ms = wait` (`clamp_timeout(context.has_deadline() ?
     std::optional{context.deadline()} : std::nullopt, wait, now)`) at the existing
     `wait_ms` parse site (`src/tools/terminal_tool.cpp:202-210`) and passes the
     result to the new deadline parameter, leaving `wait_ms` untouched (`:221`);
     it maps `read.timed_out` → `throw
     ToolError{ToolErrorCode::Timeout, "pty read timed out"}` (the only route by
     which the error reaches the loop).
   - **MCP (corrected in Rev 4 / N4).** `McpCallOptions`
     (`include/ymh/mcp/mcp_types.hpp:133-137` — the declaration site;
     `src/mcp/mcp_tool.cpp:40-43` merely populates a local) gains
     `std::optional<std::chrono::milliseconds> deadline`. `McpClient::callTool`
     computes the clamp **before** the existing fallback and with the existing
     effective timeout as the caller value, so the configured server timeout stays
     a ceiling:
     `caller_ms = (options.timeout.count() > 0 ? options.timeout : config_.call_timeout)`
     (`options.timeout` is the server's `call_timeout` set at
     `src/mcp/mcp_tool.cpp:42`; `config_.call_timeout` is
     `McpServerConfig::call_timeout`, `include/ymh/mcp/mcp_types.hpp:172`, whose
     config default is `60'000`, `include/ymh/config/config.hpp:134`). The clamp
     is therefore `min(existing_effective, remaining)`, and an active deadline
     with `remaining > 60 s` can **never raise** the call above the configured
     server timeout. Then: `0` → throw
     `McpError{McpErrorCode::CallTimeout, "deadline expired"}` immediately (the
     enum member is `CallTimeout`, **not** `Timeout`; `mcp_types.hpp:87`,
     `to_string(CallTimeout) == "CallTimeout"`, `src/mcp/mcp_types.cpp:116`;
     G2-H2); `>0` → pass the clamped budget as the call timeout, replacing the
     `options.timeout.count() > 0 ? … : config_.call_timeout` expression at
     `src/mcp/mcp_client.cpp:206-207`. `mcp_tool.cpp`'s `catch (const McpError&)`
     (`:57-62`) maps `CallTimeout` → `throw ToolError{ToolErrorCode::Timeout, …}`
     so the canonical `"Timeout"` reaches `ToolRegistry` (see 46-D8.11, G2-M6).
8. **`git` is cooperative, not preemptible (F-04).** `git_status`/`git_diff` run
   in-process via libgit2 (`src/tools/git_tools.cpp:65`, `:98`) with no
   cancellation primitive. They are moved into the 46-D8.8 cooperative bucket:
   the tool checks `context.expired()` **before** the libgit2 call and returns a
   `ToolError{Timeout}` result if so, and checks again after it returns; a
   libgit2 call that hangs mid-operation is **not** preemptible and is documented
   as such. The rev-1 universal claim ("all shipped blocking tools route through
   process()/PTY/MCP") is **deleted** (it was false).
9. **Interaction with a model-supplied `timeout_ms`.** The effective tool deadline
   is the minimum of the model's request, the primitive's configured value, and
   the remaining tool deadline — the deadline is a ceiling, never an extension.
   For MCP the "primitive's configured value" is made concrete in 46-D8.7: the
   clamp is called with `caller_ms = (options.timeout.count() > 0 ? options.timeout
   : config_.call_timeout)`, so the server's configured `call_timeout` (default
   `60'000`, `include/ymh/config/config.hpp:134`) is a hard ceiling and an active
   tool deadline can only lower it, never raise it (N4).
   When the **tool** deadline is disabled (`tools.timeout_ms == 0`), the
   model/primitive value applies unchanged — but this does **not** mean "no tool
   call times out": a model-supplied `timeout_ms` still bounds `shell`
   (`src/tools/builtin_tools.cpp:391-394`) and an MCP call still falls back to the
   server's `call_timeout_ms` default of `60'000`
   (`include/ymh/config/config.hpp:134`; `src/mcp/mcp_client.cpp:207`). Disabling
   `tools.timeout_ms` disables only the **tool deadline** layer (G2-M7; I30
   qualified).

#### 10.2.4 Guards, errors, and turn continuation

10. **Pre-dispatch guard (narrow race-reducer; G2-L17).** If `context.expired()`
    before the tool body runs, the loop fails the call immediately with a Timeout
    result instead of starting it. Because the deadline is computed at
    `ToolContext` construction (`src/agent/agent_loop.cpp:768`), this guard is
    almost always false in production and is **not** the enforcement point — the
    primitive clamp (46-D8.7) is. The `UI46_D8_DeadlineExpiredPreDispatch` test
    must therefore construct a `ToolContext` with an **already-past** deadline
    directly (or advance an injected clock past it), so it is non-vacuous without
    the full D8 plumbing.
11. **Error path (F-26; corrected in Rev 3 / G2-M6, G2-L2).** No new event family.
    For `process`/PTY the tool surfaces `ToolError{ToolErrorCode::Timeout, …}`;
    **`ToolRegistry` catches it** (`src/tools/tool_registry.cpp:358-362`) and
    returns an error `ToolResult` whose **`error` field** is `to_string(code)` =
    `"Timeout"` (`src/tools/tool_registry.cpp:25` sets `result.error`; the
    `output` field is materialized separately; `src/execution/errors.cpp:14`).
    For **MCP** the tool swallows `McpError` itself (`src/mcp/mcp_tool.cpp:57-62`
    sets `result.error = to_string(error.code())`), so `ToolRegistry`'s catch is
    not reached; 46-D8.7 pins that `mcp_tool.cpp` maps `CallTimeout` →
    `ToolError{ToolErrorCode::Timeout}` so the canonical `"Timeout"` is produced
    for MCP too (the raw `to_string(CallTimeout)` is `"CallTimeout"`). The loop's
    catch (`src/agent/agent_loop.cpp:773-779`) is a last-resort `std::exception`
    handler and does **not** see a `ToolError`; the durable
    `payload::ToolResult` event records the error
    (`include/ymh/session/events.hpp:140-161`).
12. **How the turn continues.** An `Error` tool result is a normal tool result:
    the loop commits it (`commitToolResult`, `src/agent/agent_loop.cpp:783`) and
    proceeds to the next step, feeding the timeout error back to the model. The
    turn is not cancelled.
13. **Fate of abandoned work — no abandoned work, no detached thread.** Because
    `execute()` is synchronous and the deadline is enforced by killing the
    underlying primitive, the `execute()` call returns once the primitive is torn
    down. On the exclusive path the loop thread unblocks; on the parallel path the
    `std::async` worker returns and `drain_all` joins it
    (`src/agent/agent_loop.cpp:1207-1212`, `:1254`). **Explicitly rejected:**
    racing `future.wait_for(deadline)` and detaching on timeout. `std::async`
    futures block on destruction, and detaching leaks a thread that still holds
    the session's governor/output state.
14. **Cooperative limit (documented).** A tool that blocks purely in-process and
    never touches a clampable primitive cannot be preempted; it must poll
    `context.remaining()`. This bucket contains `git` (46-D8.8) and any future
    in-process tool.

### 10.3 Failure modes

- A `shell` command that ignores SIGTERM: the existing terminate-grace → SIGKILL
  path bounds it (`include/ymh/execution/config.hpp:32-33`). 46-F10.
- A hung MCP server: the MCP call timeout is clamped to the deadline; the
  transport is closed. 46-F11.
- An **expired** deadline reaching a primitive: the primitive returns an
  immediate timeout, never an unbounded wait (46-D8.7). 46-F27.
- A **disabled** tool deadline (`tools.timeout_ms = 0`): `remaining()` is
  `milliseconds::max()` and the tool-deadline layer is off. The model's
  per-call `timeout_ms` and the MCP server `call_timeout_ms` default (60 s) still
  apply, so "no call times out" is **not** claimed (G2-M7). 46-F28.
- A tool that ignores the deadline (pure in-process loop, including `git`):
  not preemptible; documented limit (46-D8.8/14). 46-F12.
- `tools.timeout_ms` negative → `ConfigError`; `0` → disabled, never an immediate
  timeout.

**Owning specs amended:** `07` §5.5, `21` §3.3/§7.5.
**Tests:** `UI46_D8_ToolDeadlineConfig`, `UI46_D8_ShellDeadlineKillsProcess`,
`UI46_D8_PtyExpiredDeadlineReturnsImmediately` (asserts `PtyRead.timed_out`),
`UI46_D8_McpExpiredDeadlineTimesOut`, `UI46_D8_McpDeadlineClamped`,
`UI46_D8_GitPrecheckTimesOut`, `UI46_D8_GitPostcheckTimesOut` (G2-L6),
`UI46_D8_ProcessExpiredDeadlineNotStarted` (G2-L7),
`UI46_D8_DeadlineExpiredPreDispatch` (constructs a past deadline directly; G2-L17),
`UI46_D8_DisabledDeadlineNeverTimesOut`,
`UI46_D8_TimeoutResultIsErrorAndTurnContinues`,
`UI46_D8_ModelTimeoutNeverExceedsDeadline`, `UI46_D8_ZeroDisables`,
`UI46_D8_ParallelWorkerJoinedAfterKill`.
**Non-vacuity (gate §E):** `ShellDeadlineKillsProcess` and
`TimeoutResultIsErrorAndTurnContinues` pass on the pre-existing model-`timeout_ms`
path (`builtin_tools.cpp:391-394`, `:407-408`) and must be re-pointed at
`tools.timeout_ms` + the `ToolContext` deadline to be non-vacuous;
`PtyExpiredDeadlineReturnsImmediately`, `McpExpiredDeadlineTimesOut`,
`GitPrecheckTimesOut`, and `DisabledDeadlineNeverTimesOut` are new and fail on
the rev-1 tree.

---

## 11. D9 — A bottom-left turn spinner in the status line (item 9)

### 11.1 Current state (verified)

- The reasoning spinner frames and sign already exist.
  `src/ui/ui_render.cpp:214-226`:

  ```cpp
  constexpr std::array<const char*, 8> kReasoningSpinnerFrames = {
      "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧",
  };

  const char* reasoning_sign(const ConversationEntry& entry, std::size_t frame) {
      if (!entry.streaming) {
          return "•";
      }
      return kReasoningSpinnerFrames[frame % kReasoningSpinnerFrames.size()];
  }
  ```

  and the "Thinking" header is `std::string(reasoning_sign(...)) + " Thinking"`
  (`src/ui/ui_render.cpp:228-232`).
- The turn-active predicate is `Thinking || CallingTool`.
  `src/ui/ui_model.cpp:324-326`:

  ```cpp
  bool is_active_state(AgentState state) noexcept {
      return state == AgentState::Thinking || state == AgentState::CallingTool;
  }
  ```

- `advance_reasoning_spinner` exists but is **never called in production**; it
  advances only while a reasoning block streams.
  `src/ui/ui_model.cpp:1271-1285`:

  ```cpp
  bool UiModel::advance_reasoning_spinner(std::chrono::milliseconds delta) {
      constexpr std::chrono::milliseconds kFrameStep{120};
      if (!has_streaming_reasoning()) {
          spinner.elapsed = std::chrono::milliseconds::zero();
          return false;
      }
      spinner.elapsed += delta;
      bool changed = false;
      while (spinner.elapsed >= kFrameStep) {
          spinner.elapsed -= kFrameStep;
          ++spinner.frame;
          changed = true;
      }
      return changed;
  }
  ```

- **`has_streaming_reasoning()` is global, not active-session-scoped (F-31).**
  `src/ui/ui_model.cpp:1259-1268` iterates **every** session:

  ```cpp
  bool UiModel::has_streaming_reasoning() const {
      for (const auto& [id, state] : sessions) {
          (void)id;
          for (const ConversationEntry& entry : state.conversation.entries) {
              if (entry.role == ConversationRole::Reasoning && entry.streaming) {
                  return true;
              }
          }
      }
      return false;
  }
  ```

- `UiEventAdapter::onTick` ticks only the flash.
  `src/ui/ui_event_adapter.cpp:407-413`:

  ```cpp
  void UiEventAdapter::onTick(std::chrono::milliseconds delta) {
      const FlashPhase before = model_.aggregate.flash.phase;
      model_.aggregate.flash.tick(delta);
      if (model_.aggregate.flash.phase != before) {
          model_.dirty.markAggregate();
      }
  }
  ```

- The repaint gate is `animation_active_`, driven by flash or streaming
  reasoning. `src/ui/supervisor.cpp:1493-1496`:

  ```cpp
  adapter_.onTick(delta);
  tick_presence();
  animation_active_.store(model_.aggregate.flash.isFlashing() ||
                          model_.has_streaming_reasoning());
  ```

  The timer posts `Event::Custom` every 50 ms while `animation_active_`
  (`src/ui/supervisor.cpp:49-50`).
- The bottom-left group starts at `mode` ("plan"/"build") and is the first cell
  of `left_cells`. `src/ui/ui_render.cpp:514`, `:577-585`:

  ```cpp
  std::string mode = status.plan_active ? "plan" : "build";
  ...
  Elements left_cells;
  const auto append_segment = [&](Element element) {
      if (!left_cells.empty()) {
          left_cells.push_back(ftxui::text(" · "));
      }
      left_cells.push_back(std::move(element));
  };
  append_segment(status.plan_active ? paint(ftxui::text(mode), ftxui::Color::Yellow, theme)
                                    : ftxui::text(mode));
  ```

  `render_status` is the last row of the UI (`src/ui/ui_render.cpp:1197-1202`).
- The spinner state is `ReasoningSpinnerState { frame, elapsed }`
  (`include/ymh/ui/ui_model.hpp:471-477`), held as `UiModel::spinner` (`:489`).
- `AgentState::WaitingForInput` is never emitted; the adapter's projection maps
  `TurnEnded`/`TurnCancelled`/`SessionEnded` → `Idle`
  (`src/ui/ui_event_adapter.cpp:54-80`).

### 11.2 Decision (46-D9)

1. **New model helpers.**

   ```cpp
   // 46-D9: the ACTIVE session's turn is running (Thinking || CallingTool).
   [[nodiscard]] bool UiModel::has_active_turn() const;
   // 46-D9: the ACTIVE session has a streaming reasoning block.
   [[nodiscard]] bool UiModel::active_has_streaming_reasoning() const;

   // 46-D9: advances the shared frame clock while a turn is active OR the active
   // session streams reasoning; returns true when the frame changed.
   [[nodiscard]] bool UiModel::advance_spinner(std::chrono::milliseconds delta);
   ```

   `advance_spinner` reuses `spinner.frame`/`spinner.elapsed` and the same 120 ms
   step; `advance_reasoning_spinner` is retained (RB-17) and `advance_spinner`
   generalizes it. **F-31 fix:** `advance_spinner` is scoped to the active session
   (`has_active_turn() || active_has_streaming_reasoning()`), not the global
   `has_streaming_reasoning()`.
2. **Advance hook.** `UiEventAdapter::onTick` (`src/ui/ui_event_adapter.cpp:407-413`)
   calls `model_.advance_spinner(delta)` and marks `UiDirtyFlag::Input` (or the
   aggregate) when the frame changed.
3. **Repaint gate.** Extend `animation_active_` (`src/ui/supervisor.cpp:1495-1496`)
   to `flash || has_streaming_reasoning() || has_active_turn()`. The global
   `has_streaming_reasoning()` is retained in the **repaint** gate (RB-17), while
   the spinner's **advance** and **visibility** are active-session-scoped. The
   50 ms timer already repaints while true.
4. **Render — exact construction (F-30).** In `render_status`, when
   `model.has_active_turn()` is true, push a single element
   `ftxui::text(std::string(kReasoningSpinnerFrames[model.spinner.frame %
   kReasoningSpinnerFrames.size()]) + " ")` as the first element of `left_cells`
   **before** the `mode` segment, using the existing `append_segment` for `mode`.
   The exact rendered bottom-left prefix is therefore `<glyph>  · <mode> · …` —
   the glyph plus one trailing space (`"<glyph> "`), then the existing `" · "`
   separator inserted by `append_segment` because `left_cells` is non-empty
   (`src/ui/ui_render.cpp:578-585`), yielding two spaces between the glyph and the
   middle dot. (Rev 1 claimed `<frame>  build` with no separator; that string does
   not result from the shipped separator logic. This is the F-30 correction.) The
   `kReasoningSpinnerFrames` table is
   promoted to the shared renderer scope so both the reasoning header and the
   status line use the identical glyph set ("same as Thinking has"). The frame
   index is `model.spinner.frame % kReasoningSpinnerFrames.size()`
   (`src/ui/ui_render.cpp:225`), not a literal `8` (F-48).
5. **On/off window.** The spinner is on while the active session's turn is active
   (`Thinking`/`CallingTool`), i.e. from the first `UserMessage`/`TurnStarted`
   until `TurnEnded`/`TurnCancelled`/`SessionEnded` (→ `Idle`) or a waiting state
   (`WaitingForPermission`/`Error`). Since `WaitingForInput` is never emitted
   (`src/ui/ui_event_adapter.cpp:54-80`), `Idle` is the "waiting for new input"
   terminal. This satisfies "on right after user hit prompt and until ymh is
   waiting for new user input".
6. **Only the active session.** The spinner's **visibility and advance** reflect
   the focused session; a background session's turn does not drive it (the status
   line is per-session). A background session that streams reasoning still drives
   the global **repaint** gate (RB-17 retained), but it does not advance or render
   the status spinner. This reconciles D9.6 with I19 (F-31).
7. **No layout change to the segment table.** 25-D1's ordered segments
   (`docs/design/25-ui-ux-errata.md:329-339`) are unchanged; the spinner is a
   prefix element, not a new segment.

### 11.3 Failure modes

- A turn ends but the last frame is shown until the next repaint: the frame is
  static once `has_active_turn()` is false; `animation_active_` drops and the
  timer stops. No flicker. 46-F13.
- A background session streams reasoning while the active session is idle: the
  repaint gate stays active (RB-17), but the status spinner does not advance or
  render (46-D9.6). No spinner.
- No active session → no spinner.

**Owning specs amended:** `10` §6, `25` D1.
**Tests:** `UI46_D9_SpinnerOnWhileTurnActive`, `UI46_D9_SpinnerOffOnIdle`,
`UI46_D9_SpinnerOffOnWaitingForPermission`, `UI46_D9_SpinnerAdvancesOnTick`,
`UI46_D9_SpinnerBeforeModeSegment`, `UI46_D9_SpinnerSharesFrameTable`,
`UI46_D9_SpinnerOnlyActiveSession`; golden `UI46_G6_StatusSpinner` (asserts the
exact `<glyph>  · build` prefix).

---

## 12. D10 — The command list reappears after Esc (item 10) — **already satisfied**

### 12.1 Current state (verified)

- Esc hides a visible list without touching the draft, and sets the dismissal
  flag. `src/ui/supervisor.cpp:2344-2354`:

  ```cpp
  if (event == ftxui::Event::Escape) {
      if (!state->command_hints.empty()) {
          state->hints_dismissed = true;
          state->command_hints.clear();
          state->command_hint_selected = 0;
          model_.dirty.mark(state->id, UiDirtyFlag::Input);
          return true;
      }
      return false;
  }
  ```

- Any command-prefix edit clears the flag and rebuilds. Character insert:
  `src/ui/supervisor.cpp:2467-2474`:

  ```cpp
  if (event.is_character()) {
      input.draft.insert(input.cursor, event.character());
      input.cursor += event.character().size();
      state->hints_dismissed = false;
      refresh_hints(*state);
      model_.dirty.mark(state->id, UiDirtyFlag::Input);
      return true;
  }
  ```

  Backspace (`:2397-2406`), Delete (`:2407-2414`), Ctrl+U (`:2452-2458`) and
  Ctrl+W (`:2459-2466`) clear it identically.
- `refresh_hints` early-returns only while dismissed.
  `src/ui/supervisor.cpp:2060-2062`:

  ```cpp
  if (state.hints_dismissed) {
      return;
  }
  ```

- The flag lives on `SessionUiState` (`include/ymh/ui/ui_model.hpp:240-247`).
- Scenario `/` → Esc → type `h`: the `/` insert builds the list; Esc sets
  `hints_dismissed` and leaves `draft == "/"`; typing `h` takes the
  `event.is_character()` branch, clears `hints_dismissed`, and `refresh_hints`
  repopulates the list for prefix `"h"`. This is exactly the required behaviour.
- **Exception (F-32).** `handle_input` checks `modal_tail_suppression_active()`
  **before** the character-insert branch: `src/ui/supervisor.cpp:2341-2343`
  returns `true` (swallowing the character) while the RB-12 modal-tail window
  (`kModalTailWindow`, 25 ms after a modal closes, `src/ui/supervisor.cpp:63`)
  is active. During that window a typed character does not reach the insert
  branch, so it does not clear `hints_dismissed`. The core `/`→Esc→type path is
  correct outside that window.
- **Test-coverage gap:** no shipped test types a character after Esc while the
  draft is a bare `/prefix` (`tests/unit/supervisor_harness_test.cpp:791-857`
  covers Esc-hides, Esc-keeps-draft, Esc-survives-history-recall,
  delete-slash-hides, slash-reopens).

### 12.2 Decision (46-D10)

1. **Already satisfied — no change.** 45-D5 / 45-D5.4 / 45-I9
   (`docs/design/45-ui-interaction-errata.md:533-583`, `:1650`) already pin the
   behaviour and the code implements it (`src/ui/supervisor.cpp:2344-2354`,
   `:2467-2474`, `:2060-2062`).
2. **Close the coverage gap** with the regression test
   `UI46_D10_SlashEscThenTypeRebuildsList`: dispatch `/`, Esc, then `h`, and
   assert `command_hints` is non-empty and contains `help`.
3. **No behaviour change**, so no supersession and no interface change. The
   `hints_dismissed` invariant (46-I20) is restated for the new test, **qualified
   by the RB-12 modal-tail exception (F-32)**: I20 holds for any command-prefix
   edit that reaches the insert/delete branches; a character swallowed by the
   25 ms modal-tail window is not a command-prefix edit.
4. **Reference the shipped test (F-33).** The rev-1
   `UI46_D10_HistoryRecallStillDoesNotRebuild` duplicates the shipped
   `UI45_D5_EscSurvivesHistoryRecall`
   (`tests/unit/supervisor_harness_test.cpp:817-830`); 46-D10 references the
   shipped test instead of adding a duplicate.
5. **Register reconciliation (F-49).** §2's D10 row reads "none (45-D5 retained)",
   consistent with §12.2's "amended: none". 45 remains a *dependency*, not an
   amended owning spec.

### 12.3 Failure modes

- If a future edit stops clearing `hints_dismissed` on character insert, the
  regression test fails. 46-F14.
- A character typed inside the 25 ms modal-tail window is swallowed before the
  insert branch; the list reappears on the next non-suppressed edit (documented
  exception). 46-F29.

**Owning specs amended:** none (45-D5 retained).
**Tests:** `UI46_D10_SlashEscThenTypeRebuildsList` (new); reference
`UI45_D5_EscSurvivesHistoryRecall`
(`tests/unit/supervisor_harness_test.cpp:817-830`).

---

## 13. D11 — Prompt history is hydrated from the durable log (item 11)

### 13.1 Current state (verified)

- `push_history` **is** called on prompt submit and on command submit.
  `src/ui/supervisor.cpp:431-448` (the `submit` override):

  ```cpp
  void submit(const std::string& text) override {
      if (text.empty()) {
          return;
      }
      WorkspaceModel* workspace = model_.activeWorkspace();
      if (workspace == nullptr) {
          return;
      }
      SessionUiState* state = model_.session(workspace->activeSessionId());
      ...
      if (state != nullptr) {
          state->input.push_history(text);
          state->input.draft.clear();
          ...
  ```

  and the command branch at `src/ui/supervisor.cpp:2355-2372` (quoted in §8.1).
- `push_history` de-duplicates only against the immediately previous entry;
  `history_up`/`history_down` walk the vector. `src/ui/ui_model.cpp:192-226`:

  ```cpp
  void InputModel::push_history(std::string line) {
      if (line.empty()) {
          return;
      }
      saved_draft.clear();
      if (!history.empty() && history.back() == line) {
          history_pos = history.size();
          return;
      }
      history.push_back(std::move(line));
      history_pos = history.size();
      ...
  ```

- In-session recall works: `UI45_D1_HistoryHoldsPromptsAndCommands`
  (`tests/unit/supervisor_harness_test.cpp:663-680`) passes against the shipped
  build.
- **The gap:** `InputModel::history` is never hydrated from the durable log. The
  `UserMessage` handler appends to the conversation only.
  `src/ui/ui_model.cpp:659-665`:

  ```cpp
  if constexpr (std::is_same_v<T, UserMessage>) {
      ConversationEntry entry;
      entry.role   = ConversationRole::User;
      entry.text   = e.text;
      entry.source = e.source;
      state.conversation.entries.push_back(std::move(entry));
      dirty.mark(e.session, UiDirtyFlag::Conversation);
  }
  ```

  There is no other writer of `input.history` outside `push_history` and tests.
  Consequence: after a resume (an explicit `--resume`, or a Ctrl+S/`/sessions`
  selection), the transcript shows previous prompts but ArrowUp recalls nothing.
  This is the reproducible form of item 11.
- The first prompt of a brand-new session is also missed by `submit`: when
  `activeSessionId` is empty, `model_.session(...)` is null, so the push is
  skipped and `create_session(workspace_id, text)` runs
  (`src/ui/supervisor.cpp:439-453`).
- **`UserMessage` is not user-only (F-34).** `UserMessage.source` is a full
  `MessageSource` with `Kind ∈ {User, Plugin, Model, Tool, Goal}`
  (`include/ymh/agent/provenance.hpp:150-157`). Producers other than the user
  append `UserMessage` events: `job_wakeup.cpp:31` sets `Plugin`,
  `goal_service.cpp:128-131` handles `Goal`, and the loop appends injected
  messages (`src/agent/agent_loop.cpp:886-890`). Hydrating
  `push_history(e.text)` without a filter would make job/plugin/goal text
  ArrowUp-recallable as if the user had typed it.

### 13.2 Decision (46-D11)

1. **Hydrate on replay/live events, user-sourced only.** In the `UserMessage`
   branch (`src/ui/ui_model.cpp:659-665`), after appending the conversation
   entry, call `state.input.push_history(e.text)` **iff
   `e.source.kind == MessageSource::Kind::User`** (F-34). This runs for both
   replayed (resume) and live events because both flow through the same `apply`
   path (`src/ui/ui_model.cpp:655-658`).
2. **De-duplication makes it safe.** For a live prompt, `submit` pushes first
   (`src/ui/supervisor.cpp:441`) and the event push immediately follows;
   `push_history` drops a line equal to the immediately previous entry
   (`src/ui/ui_model.cpp:197-200`), so no duplicate is added. (Rev 1 cited
   `:49-52`, which is the `ReasoningDelta` path; the real dedup is `:197-200`,
   F-35.)
3. **First-prompt gap closes.** When `submit` has no state, the daemon's
   user-sourced `UserMessage` event creates the state via `ensureSession` and
   hydrates history (46-D11.1), so the first prompt is recallable.
4. **Scope.** Only user-sourced prompts are hydrated. Slash commands are
   client-side and are not persisted as `UserMessage`, so after a resume they are
   not in history; in-session they still are (command branch,
   `src/ui/supervisor.cpp:2361-2368`). Plugin/goal/tool/model-sourced
   `UserMessage`s are **not** hydrated. Documented.
5. **Ordering.** Replay order is the log order, so history is in submission order.
6. **No new field.** `InputModel` is unchanged (`include/ymh/ui/ui_model.hpp:157-170`).

### 13.3 Failure modes

- A replayed user `UserMessage` that is also in the live history duplicates only
  if it is not the immediately previous entry; replay happens on subscribe, before
  new prompts, so the tail is the last replayed prompt. Accepted and covered by a
  test. 46-F15.
- A plugin/goal-sourced `UserMessage` is not hydrated, so it is never recalled as
  a user prompt. 46-F30.
- A resumed session with hundreds of prompts: `push_history` is O(1) amortized
  (dedup only checks the tail). 46-F16.

**Owning specs amended:** `10` §4.3/§9.3.
**Tests:** `UI46_D11_HistoryHydratedOnReplay`,
`UI46_D11_FirstPromptOfNewSessionRecallable`,
`UI46_D11_NoDuplicateForLivePrompt`, `UI46_D11_HistoryOrderMatchesLog`,
`UI46_D11_CommandsNotHydrated`, `UI46_D11_PluginMessageNotHydrated`.
---

## 14. D12 — The localcode import also carries API keys, permission rules and model parameters (item 12)

### 14.1 Current state (verified)

- `build_localcode_import` copies MCP servers, compaction, concurrency, and (for
  an `openai-compatible` provider) `base_url`/`model`/`context_window`.
  `src/config/config.cpp:1418-1523`; the copied-field table is exercised by
  `tests/unit/config_test.cpp:884-926`. The model import is exactly
  `src/config/config.cpp:1497-1520` (`context_window`, `base_url`, `model`);
  rev 1 cited `:912-934`, which is the `goals` section, not the model import
  (F-37).
- API keys, `skip_permissions`, `permission` rules and `max_tokens` are **not**
  imported, and the CLI announces this. `src/cli/cli.cpp:730-742`:

  ```cpp
  void print_localcode_notes(const nlohmann::json& localcode, std::ostream& err) {
      if (const auto skip = localcode.find("skip_permissions");
          skip != localcode.end() && skip->is_boolean() && skip->get<bool>()) {
          err << "ymh: note: localcode 'skip_permissions' is not imported; permission prompts stay "
                 "enabled\n";
      }
      if (const auto rules = localcode.find("permission");
          rules != localcode.end() && rules->is_array() && !rules->empty()) {
          err << "ymh: note: localcode permission rules are not imported (ymh uses coarse "
                 "permission modes)\n";
      }
      ...
  }
  ```

  and the prompt text at `src/cli/cli.cpp:961-965`. The test asserts the absence:
  `tests/unit/config_test.cpp:915-924` (`EXPECT_FALSE(doc.contains("providers"))`,
  `… contains("permission")`, `… contains("skip_permissions")`,
  `… contains("api_key")`).
- `build_localcode_import`'s contract explicitly says "no provider api_key copy"
  (`include/ymh/config/config.hpp:298-308`).
- ymh secrets are env references today: `llm.api_key_env` names an env var
  (`include/ymh/config/config.hpp:14-16`, `:108`), and "Secrets never in `Config`"
  (`docs/design/21-config-jsonc-errata.md:232-233`). The LLM request layer already
  supports `max_output_tokens` (`include/ymh/llm/llm_request.hpp:44`) and
  `LlmCallConfig::max_tokens` (`include/ymh/llm/llm_call_config.hpp:28-37`), but
  `LlmSettings` exposes neither. `LLMProviderConfig` has only `api_key_env`, not a
  literal key (`include/ymh/llm/provider_registry.hpp:25-37`, `:29`); the OpenAI
  adapter reads the key from the environment
  (`src/llm/openai_adapter.cpp:904-907`).
- **The real localcode shape (USER-PROVIDED, 2026-09-20).** The user's actual
  `~/.localcode/config.json` has: `permission` = an **object** keyed by tool
  (`{"bash": [{"match": "dir *", "decision": "allow"}, …7 entries]}`);
  `providers.<name>` = `{type, base_url, api_key}` where `type` is
  `"openai-compat"` (not `"openai-compatible"`) or `"bedrock"`;
  `default_profile` names an entry in `profiles.<p>` =
  `{provider, model, max_tokens, context_window}`; plus `skip_permissions`,
  `mcp_servers`, `auto_compact_enabled`, `auto_compact_percent`,
  `max_concurrent_tasks`, and ignored keys (`auto_delegate`, `keep_going`,
  `model_invocable`, `orchestrate`, `smart_agent`). This resolves Q3: `match` is
  a **shell-command glob** under a tool key, not a path glob.
- Import target is the global config, so imported values are the **global
  layer**, overridden by the workspace layer, env, and CLI.
  `src/config/config.cpp:1271-1276`; documented order
  `include/ymh/config/config.hpp:5-16`. The import runs only on first run:
  `src/cli/cli.cpp:929-983` (`maybe_import_localcode_config`), invoked at
  `src/cli/cli.cpp:1167-1169`; the gate is
  `!std::filesystem::exists(global_config.parent_path())`
  (`src/cli/cli.cpp:938-941`). Once the global config directory exists there is no
  re-import and no merge. The write is `0600` temp + `rename`
  (`src/cli/cli.cpp:851-853`, `:888`, `:894`); validation is
  `apply_jsonc_file(validation_config, temp, /*required=*/true)`
  (`src/cli/cli.cpp:888`). Rev 1 cited `:1172-1194` for the trigger and
  `:1245-1248` for the 0600 write and `:1246-1247` for validation; all three were
  wrong (F-37).

### 14.2 Decision (46-D12)

1. **New ymh config fields.**

   ```cpp
   struct LlmSettings {
       …
       std::optional<std::string>   api_key;     // 46-D12.2 (SECRET)
       std::optional<std::uint32_t> max_tokens;  // 46-D12.4
   };
   ```

   `api_key` is a literal secret stored in the ymh config file (USER DECISION,
   46-S6/46-S7); it takes precedence over `api_key_env`. `max_tokens` is the
   canonical sampling budget and is omitted from the request when unset.
   **O-M3 (Rev 5) — the real route.** The only shipped route to
   `LlmCallConfig::max_tokens` is `src/agent/agent_loop.cpp:539`
   (`config.max_tokens = config_.parameters.max_output_tokens`), where
   `config_.parameters` is `AgentConfig::parameters` (`GenerationParameters`,
   `include/ymh/agent/agent.hpp:108-113`), and
   `GenerationParameters::max_output_tokens` maps 1:1 onto
   `LlmCallConfig::max_tokens` at dispatch
   (`include/ymh/llm/llm_call_config.hpp:28-30`; the adapter emits `max_tokens`
   at `src/llm/openai_adapter.cpp:739-740`). `to_agent_config`
   (`src/cli/wiring.cpp:174-191`) does **not** set it today, so the key would be
   dead. 46-D12 pins the wiring: `to_agent_config` sets
   `agent.parameters.max_output_tokens = config.llm.max_tokens` when
   `config.llm.max_tokens.has_value()`. The Rev-4 phrasing "**not**
   `GenerationParameters::max_output_tokens`" was wrong: that field is the one
   and only conduit, not a bypassed alternative.
2. **API keys (USER DECISION).** Import
   `providers[<default_profile.provider>].api_key` into `llm.default.api_key`.
   The value is written only into the `0600` global config (the importer already
   writes `0600` via temp+rename, `src/cli/cli.cpp:851-853`, `:894`), is **never
   logged**, is **never rendered in the UI**, and is **never copied into the
   session event log**. This supersedes `21:232-233` "secrets never in `Config`"
   (46-S6) and `08` §14.1(g)/(t) **and §6.4** literal-key rejection (46-S7).
   This is the security-sensitive part of the spec; the threat model is 46-D12.10
   and the decision is recorded as resolved by the user in §21 (Q1).

   **Enforceable controls (added in Rev 3; G2-M11/M12/M13; plumbing pinned in
   Rev 4 / N8):**
   - **Global-only.** `llm.default.api_key` is accepted **only in the global
     layer** (`$XDG_CONFIG_HOME/ymh/config.jsonc` or `$HOME/.config/ymh/config.jsonc`).
     An `api_key` in the **workspace layer** (`<workspace>/.ymh/config.jsonc`,
     which lives inside a repo) is a `ConfigError` at load, not an override. This
     is a new rule layered on the general precedence in 46-D12.8. **Plumbing
     (N8):** the `llm` section handler gains the layer flag —
     `apply_llm(Config&, const Json&, const std::filesystem::path&, bool global_layer)`
     (`src/config/config.cpp:389`, called from `apply_document` at `:889`) — and
     fails a non-global `api_key` with `fail(source, "'llm.api_key' is
     global-layer only")`, exactly mirroring the existing `session` guard
     (`apply_document(..., bool global_layer)` at `:840-841`; the `session` guard
     at `:898-899`). No new type is introduced.
   - **`0600` where the user edits.** `write_default_config`
     (`src/config/config.cpp:1033-1057`) creates the scaffolded global
     `config.jsonc` `0600` (today it uses a plain `std::ofstream` at `:1044`,
     which yields `0644` under umask 022). Defense-in-depth: the loader rejects a
     literal `api_key` read from a config file whose mode has group/other bits
     (`ConfigError`), so a chmod'd file cannot silently leak the key.
   - **Env fallback.** `api_key_env` is the fallback when `api_key` is absent.
     `ProviderRegistry::create` (`src/llm/provider_registry.cpp:68-70`) currently
     **hard-requires** a valid `api_key_env` and would reject a config with only
     `api_key`; 46-D12 pins that the `is_valid_env_name` check is skipped when
     `config.api_key` is non-empty (`is_valid_env_name("")` is false, `:26-42`).
     This makes `api_key_env` optional when a literal key is present.
3. **Provider config plumbing (F-40; amendment ID corrected to 46-D12.3,
   G2-M16).** `LLMProviderConfig` gains
   `std::optional<std::string> api_key` (`include/ymh/llm/provider_registry.hpp:25-37`);
   `to_provider_config` (`src/cli/wiring.cpp:61-76`) copies it from
   `config.llm.api_key`; the OpenAI adapter prefers a non-empty
   `config_.api_key` over `get_env(config_.api_key_env)`
   (`src/llm/openai_adapter.cpp:904-907`). 46-D12 amends `08 §5.1` and `28`
   (`docs/design/28-llm-service-boundary-errata.md:303`) and adds an end-to-end
   "the imported key reaches the provider and is never logged" test.
4. **`max_tokens` (46-D12.4).** Import `profiles[<default_profile>].max_tokens`
   (when `> 0`) into `llm.default.max_tokens`. The value reaches the wire through
   the pinned route above (`to_agent_config` → `AgentConfig::parameters.
   max_output_tokens` → `agent_loop.cpp:539` → `LlmCallConfig::max_tokens` →
   `max_tokens` in the request body); `context_window` continues to map to
   `agent.compaction.context_window_tokens` (retained).
5. **`skip_permissions`.** Import `true` as `permissions.default = "allow"`
   (46-D1). `false`/absent imports nothing.
6. **`permission` rules (real shape; Q3 resolved).** The canonical localcode shape
   is an **object** `{ "<tool>": [ {"match": <shell-command glob>,
   "decision": <"allow"|"ask"|"deny"|…>}, … ] }`. Each entry is imported as a
   `permissions.rules` entry `{ "tool": map_tool(<tool>), "command": match,
   "effect": decision, "id": "localcode.rule.<tool>.<n>" }`. `map_tool` maps
   `bash` → `shell`; otherwise the key is used verbatim if it names a known ymh
   tool, else the whole tool key is skipped with a note. Invalid `decision` values
   are dropped with a note. A flat array (`[{match, decision}]`) is **not** the
   real localcode shape; it is not mapped (a note is printed). The synthetic
   fixture `tests/unit/config_test.cpp:884-926` is updated to the object shape.
   Because 46-D1.2 requires `tool` on command rules, every imported rule carries
   `tool` — the import and the D1 rule-shape requirement are consistent.
   **Explicit code change (G2-L15):** the shipped provider-type check accepts only
   the exact string `"openai-compatible"` (`src/config/config.cpp:1494-1495`);
   46-D12 pins that it also accepts `"openai-compat"` (the spelling the user's
   real localcode document uses), mapping both to the canonical
   `"openai-compatible"`.
7. **Mapping table (pinned, derived from the real localcode document).**

   | localcode source | ymh target | Rule |
   |---|---|---|
   | `skip_permissions == true` | `permissions.default = "allow"` | 46-D1 master switch (item 1) |
   | `permission.<tool>[].match` / `.decision` | `permissions.rules[] = {tool: map_tool(<tool>), command: match, effect: decision}` | `match` is a shell-command glob; `map_tool` maps `bash`→`shell`; invalid `decision` dropped with a note (Q3 resolved) |
   | `providers.<name>.type` | `llm.default.provider` | `"openai-compat"` and `"openai-compatible"` → `"openai-compatible"`; other types (`bedrock`, …) skipped with a note |
   | `providers.<name>.base_url` | `llm.default.base_url` | retained; http(s) only (a non-http(s) URL is skipped with a note) |
   | `providers.<name>.api_key` | `llm.default.api_key` | literal; 0600; never logged/rendered/event-logged; `api_key` > `api_key_env` |
   | `default_profile` + `profiles.<p>.provider` | (resolves the provider entry) | `default_profile` names a profile; `profiles.<p>.provider` names a `providers.<name>` entry |
   | `profiles.<p>.model` | `llm.default.model` | retained (openai-compatible only) |
   | `profiles.<p>.max_tokens` | `llm.default.max_tokens` | `> 0`; targets `LlmCallConfig::max_tokens`; omitted when unset |
   | `profiles.<p>.context_window` | `agent.compaction.context_window_tokens` | retained |
   | `mcp_servers` | `mcp_servers` | already imported (25-D13); unchanged |
   | `auto_compact_enabled` / `auto_compact_percent` | `agent.compaction.enabled` / `threshold_ratio` | retained |
   | `max_concurrent_tasks` | `llm.default.max_concurrency` | retained (direct if `> 0`) |
   | `auto_delegate`, `keep_going`, `model_invocable`, `orchestrate`, `smart_agent` | — | ignored |

8. **Precedence (corrected in Rev 3 / G2-L12).** Imported values land in the
   global layer; the workspace layer, `YMH_*` env, and CLI override **ordinary**
   keys (`src/config/config.cpp:1271-1276`; `include/ymh/config/config.hpp:5-16`).
   The importer never writes the workspace layer. `llm.default.api_key` is the
   **exception**: it is global-only (46-D12.2) and there is **no** override path
   for it — `apply_env_overrides` has no `YMH_LLM_API_KEY` key
   (`src/config/config.cpp:1199-1244`) and the CLI has no `--api-key` flag
   (`--api-key-env` at `src/cli/cli.cpp:110` sets only the env-var *name*). Once
   set, a literal key is changed only by editing the global config file. A
   configured literal key outranks `api_key_env`; `--api-key-env` does not clear
   it.
9. **Re-import / conflict (pinned).** The trigger is unchanged (25-D14,
   `docs/design/25-ui-ux-errata.md:1738-1750`): TUI-only, no explicit `--config`,
   the conventional global config **directory** absent, localcode present and
   regular, interactive (`src/cli/cli.cpp:932-951`). If the global config
   directory exists, the import is skipped entirely — no merge, no overwrite
   (`src/cli/cli.cpp:938-941`). The import is all-or-nothing: a malformed mapped
   document or a failed validation falls back to the default config
   (`src/cli/cli.cpp:971-982`, `:886-897`). A malformed/unreadable localcode
   document is skipped with a note (`src/cli/cli.cpp:953-959`).
10. **Threat model for the literal key (resolves Q1; hardened in Rev 3 /
    G2-M12/M13/M14).** Assets: the API key. Adversaries: (a) another local
    user/process reading the file; (b) accidental disclosure via logs, UI, or the
    event log; (c) a repo-shipped file. Controls:
    - **(a)** the global `config.jsonc` is `0600`: the importer writes `0600`
      (`src/cli/cli.cpp:851-853`) **and** `write_default_config`
      (`src/config/config.cpp:1033-1057`) is changed to create the scaffold `0600`
      (today `:1044` is a plain `std::ofstream` ⇒ `0644` under umask 022); the
      loader additionally rejects a literal `api_key` from a group/world-readable
      config file (`ConfigError`).
    - **(b)** the key is never written to the session DB or the event log;
      redaction masks any `api_key` value in log/error/UI strings (46-D12.11),
      and the primary control is source discipline: never stringify the key.
    - **(c)** `api_key` is accepted **only in the global layer**; a workspace-layer
      `api_key` (`<workspace>/.ymh/config.jsonc`, inside a repo) is a
      `ConfigError`, and the importer never writes the key into the workspace layer
      or a repo file.
    Residual risk: a process running as the same user can read the file (the same
    risk as the env var); a keyring is deferred. Accepted by the user.
11. **Redaction rule (F-39; corrected in Rev 3 / G2-M14; enforced at the sink in
    Rev 5 / O-M7).** The primary control is **source discipline: never stringify
    the key** (never `dump()` a config that contains it, never interpolate it
    into a log/notice). Redaction is belt-and-suspenders. Extend the existing
    redactor `redact_secrets`
    (`src/llm/redaction.cpp:52-98`; declared `include/ymh/llm/redaction.hpp:14`)
    with a JSON-key rule that matches the form the secret actually takes in a
    config document: `"api_key"\s*:\s*"…"` → `"api_key": "[REDACTED]"`. Today it
    matches only `Bearer`, `sk-`, `api_key=`, `api-key=`, `apikey=` — **not** the
    JSON colon form. **The redaction token is `[REDACTED]`** (the shipped
    constant `kRedacted`, `src/llm/redaction.cpp:10`), not `"***"` (O-L2).
    **O-M7 (Rev 5): the guarantee is pinned at the sink, because the Rev-4 "pin
    the call sites" list was unenforceable.** `SpdlogLogger::log`
    (`src/core/logging.cpp:42-46`) logged the message verbatim; only `log_prompt`
    (`:184-189`) called `redact_log_text` (`:175-181`). 46-D12 pins that
    **`SpdlogLogger::log` applies `redact_secrets` to the message before
    logging**, so every `category_logger(...)`/`Logger::log` line is covered by
    construction, not by a list of call sites. The UI notice ring is covered by
    pinning its config-secret-bearing producers: `print_localcode_notes`'s note
    text and any config-error string surfaced via `surface_notice`
    (`src/ui/supervisor.cpp:944-947`; `push_notice` at `:929`) pass through
    `redact_secrets` before `model_.pushNotice`. The import notes/errors
    (`src/cli/cli.cpp:730-742`, `:953-959`) likewise pass through `redact_secrets`.
    Invariant 46-I36 is restated to this enforceable form.
    **Drop the rev-2 `ymh config` "dump" surface claim:** `run_config_command`
    (`src/cli/cli.cpp:193-203`) prints only the path and never loads config
    (AGENTS.md: `config` never loads it), so there is no dump surface to redact.
    `08 §6.2` is provider-error redaction, not config secrets; rev 1's citation
    was wrong (F-39). Tests: a redaction unit test on the JSON form plus
    `UI46_D12_ApiKeyNeverRendered` (UI), `UI46_D12_ApiKeyNeverLogged`
    (G2-L13), and `UI46_D12_LogSinkRedacts` (O-M7).
12. **Notes/prompt text.** `print_localcode_notes`
    (`src/cli/cli.cpp:730-742`) drops the two "not imported" notes for
    `skip_permissions` and `permission`; the first-run prompt
    (`src/cli/cli.cpp:961-965`) changes to list API keys and permission rules as
    imported. Notes remain for a non-http(s) `base_url`, an unsupported provider
    `type`, a flat-array `permission`, and dropped rules.

### 14.3 Failure modes

- No `default_profile`/`profiles`/`providers` → no `api_key`/`model`/`max_tokens`
  imported (retained behaviour). 46-F17.
- An `api_key` that is not a string → not imported; no error.
- `max_tokens` `<= 0` or non-numeric → not imported.
- The global config is written but validation fails → the temp is unlinked and
  the default config is written (`src/cli/cli.cpp:886-897`). The API key is never
  left in a rejected temp.
- `permissions.rules` with a malformed glob → the whole import fails at
  validation (`apply_jsonc_file`, `src/cli/cli.cpp:888`), falling back to the
  default config; no partial permission state.
- A provider `type` that ymh does not support (`bedrock`) → the provider block is
  skipped with a note; no partial provider state. 46-F31.

**Owning specs amended:** `08` §5.1/§6.2/**§6.4**/§14.1(g)(t), `21` §1.3.3/§3.3/
§7.5 (permissions) + §7.7 (llm) + new §7.12 (`tools`), `25` D14/D15, `28`.
**Tests:** `UI46_D12_ImportApiKey`, `UI46_D12_ApiKeyReachesProvider`,
`UI46_D12_ApiKeyNeverLogged`, `UI46_D12_ApiKeyNeverRendered` (G2-L13),
`UI46_D12_ApiKeyEnvOptional` (literal key with no env name; G2-M11),
`UI46_D12_WorkspaceLayerApiKeyRejected` (G2-M13),
`UI46_D12_ScaffoldIs0600` (G2-M12),
`UI46_D12_RedactsJsonApiKey` (G2-M14),
`UI46_D12_ImportSkipPermissions`,
`UI46_D12_ImportPermissionRulesObject`, `UI46_D12_FlatPermissionArraySkipped`,
`UI46_D12_OpenAiCompatTypeAccepted`, `UI46_D12_BedrockTypeSkipped`,
`UI46_D12_ImportMaxTokens`, `UI46_D12_MaxTokensTargetsCallConfig` (O-M3: the
imported value appears as `max_tokens` in the outgoing request body via
`to_agent_config`),
`UI46_D12_ModelFieldsRetained`, `UI46_D12_NoProfileNoImport` (G2-L1; F17),
`UI46_D12_NoReimportWhenGlobalExists`,
`UI46_D12_InvalidDecisionDropped`, `UI46_D12_GlobalLayerOverridable`,
`UI46_D12_LogSinkRedacts` (O-M7: an ordinary `category_logger(...).info` line
carrying the literal key is redacted by `SpdlogLogger::log`),
`UI46_D12_LocalcodeFixture`; update `tests/unit/config_test.cpp:884-926` (the
fixture moves to the real object shape; the `api_key` absence assertion flips).
**Non-vacuity:** `ApiKeyReachesProvider`, `ApiKeyEnvOptional`,
`WorkspaceLayerApiKeyRejected`, `ScaffoldIs0600`, `RedactsJsonApiKey`,
`MaxTokensTargetsCallConfig`, `LogSinkRedacts` and `LocalcodeFixture` fail on the
rev-1 tree; `NoReimportWhenGlobalExists` / `ModelFieldsRetained` pass before and
after and are retained as regression pins.

---

## 15. D13 — Ctrl+E opens `$EDITOR` and copies the result into the composer (item 13)

### 15.1 Current state (verified)

- An external-editor launcher already exists and is used by `/export`.
  `src/ui/session_export.cpp:294-342` (`editor_from_environment`, `run_editor`):

  ```cpp
  std::string editor_from_environment() {
      const char* visual = std::getenv("VISUAL");
      if (visual != nullptr && *visual != '\0') {
          return visual;
      }
      const char* editor = std::getenv("EDITOR");
      if (editor != nullptr && *editor != '\0') {
          return editor;
      }
      return "vi";
  }

  int run_editor(const std::filesystem::path& file, const std::string& editor) {
      std::vector<std::string> argv = split_editor(editor);
      ...
      const pid_t pid = ::fork();
      ...
      ::execvp(raw.front(), raw.data());
      ...
      _exit(127);
  }
  ```

  **F-50 correction:** on `execvp` failure the child `_exit(127)`
  (`src/ui/session_export.cpp:326-327`) and the parent returns `WEXITSTATUS`
  (`:337`) ⇒ **127**, not `-1`. `-1` is returned only on a `fork`/`waitpid`
  error. Any non-zero status (including 127) is a launch/exit failure.
  Contract: `include/ymh/ui/session_export.hpp:55-61`.
- The supervisor hands the terminal to the editor via FTXUI `WithRestoredIO`.
  `src/ui/supervisor.cpp:1578-1586`:

  ```cpp
  int edit_export_file(const std::filesystem::path& file) {
      if (screen_ == nullptr) {
          return -1;
      }
      int status = -1;
      screen_->WithRestoredIO(
          [&status, &file] { status = run_editor(file, editor_from_environment()); })();
      return status;
  }
  ```

  The only caller today is `/export`
  (`src/ui/supervisor.cpp:1644`, `const int status = edit_export_file(output);`).
  **F-54 rebuttal:** the cited line 1644 *is* the `/export` caller; the gate's
  claim that the caller is at `:1645` is a false positive (line 1645 is
  `if (status != 0) {`).
- **No Ctrl+E handling exists.** The composer handler and its Ctrl-* cases:
  `src/ui/supervisor.cpp:2328-2343` and `:2452-2466` (CtrlU/CtrlW). A repo-wide
  `grep CtrlE` over `src/` returns no matches. FTXUI provides the event constant
  (`build/_deps/ftxui-src/include/ftxui/component/event.hpp`).
- The composer renders `active->input.draft`.
  `src/ui/ui_render.cpp:394-399`:

  ```cpp
  std::string draft = active == nullptr ? std::string{} : active->input.draft;
  return ftxui::hbox({
      paint(ftxui::text("> "), ftxui::Color::Green, theme) | ftxui::bold,
      ftxui::text(draft),
      ftxui::text("_"),
  });
  ```

- The supervisor process holds no `ExecutionEnvironment`; it launches the editor
  directly (`src/ui/supervisor.cpp:33` includes the protocol, and
  `src/ui/session_export.cpp:320-326` forks/execs). No `std::system`/`popen` in
  the UI path.

### 15.2 Decision (46-D13)

1. **New helper (reuse).**

   ```cpp
   // 46-D13: writes `initial` to a 0600 scratch file, hands the terminal to the
   // editor via the caller's WithRestoredIO, reads it back, and unlinks the file.
   // Returns std::nullopt on launch/exit failure (the draft is left unchanged).
   [[nodiscard]] std::optional<std::string>
   edit_text_in_editor(const std::string& initial, std::string& error);
   ```

   It reuses `editor_from_environment` and `run_editor`
   (`src/ui/session_export.cpp:294-342`); the scratch file lives under the
   workspace `.ymh/` directory (0600, unique name) and is unlinked on every path
   (RAII guard).
2. **Supervisor path.** Add `void edit_prompt(SessionUiState& state)`:
   - `if (screen_ == nullptr) { surface_notice("editor unavailable"); return; }`
   - call `screen_->WithRestoredIO([&]{ edited = edit_text_in_editor(state.input.draft, err); })()`
     (the `edit_export_file` pattern, `src/ui/supervisor.cpp:1578-1586`);
   - on success: set `state.input.draft = *edited`, `state.input.cursor =
     draft.size()`, `state.input.hints_dismissed = false`, then `refresh_hints(state)`
     and `model_.dirty.mark(state.id, UiDirtyFlag::Input)`
   - on failure: leave the draft unchanged and `surface_notice(err)`.
3. **Key binding.** Add an `Event::CtrlE` case in `handle_input`
   (`src/ui/supervisor.cpp:2328-2343`), alongside CtrlU/CtrlW (`:2452-2466`). It
   returns true and does not fall through.
4. **Empty draft.** Ctrl+E on an empty composer opens the editor with an empty
   buffer; `:wq` with no text leaves the draft empty.
5. **`:wq` semantics.** The file contents are used iff the editor exits `0`
   (`WEXITSTATUS == 0`) **and** the file is readable; `:wq` exits 0, so the text
   is copied. `:q!` (exit 0 but unchanged) copies the unchanged text — the draft
   is idempotent. **Any non-zero status — including 127 from `execvp` failure
   (`src/ui/session_export.cpp:326-327`, `:337`) and `-1` from a `fork`/`waitpid`
   error — discards the edit and keeps the old draft (F-50).**
6. **No key injection.** The editor is a child process with the real terminal
   (`WithRestoredIO`); on return the composer is repainted from the model. The
   RB-12 modal-tail suppression (25 ms, `src/ui/supervisor.cpp:63`) applies to
   modals, not to this path; no synthetic keystroke is needed.
7. **Hint refresh.** The refreshed draft may be a `/command`; `refresh_hints`
   runs so the list is correct.

### 15.3 Failure modes

- `$EDITOR` unset and `vi` absent → `run_editor` returns `127`; the draft is
  unchanged and a notice is shown. 46-F18.
- The scratch file cannot be created → `std::nullopt`; draft unchanged; notice.
- The editor is killed by a signal → non-zero status; draft unchanged. 46-F19.
- The scratch file survives a crash: it is under `.ymh/` and 0600; a stale file
  is overwritten by a fresh unique name. (A cleanup sweep is out of scope.)

**Owning specs amended:** `10` §9.2.
**Tests:** `UI46_D13_CtrlEOpensEditor`, `UI46_D13_EditorResultCopied`,
`UI46_D13_EditorFailureKeepsDraft`, `UI46_D13_EditorExit127KeepsDraft`,
`UI46_D13_EmptyDraft`, `UI46_D13_ScratchFileUnlinked`,
`UI46_D13_HintsRefreshedAfterEdit`.

---

## 16. C++ interface sketches (pinned)

Only new/changed symbols are shown; unchanged members are elided with `…`. Every
sketch pins the headers it needs (F-51/F-52).

```cpp
// ── include/ymh/config/config.hpp ──────────────────────────────────────────
#include <cstdint>      // uint32_t
#include <optional>
#include <string>
#include <vector>
namespace ymh {

// 46-D1: one config rule; becomes a PolicyRule with Layer::Project.
// Rev 5 / O-H1: no `path` field — the shipped loop never populates
// `PermissionRequest::path`, so a path rule would be dead code.
struct PermissionRuleSettings {
    std::optional<std::string> tool;
    std::optional<std::string> command;
    std::string                effect;      // "allow" | "ask" | "deny"
    std::string                id;          // "config.rule.<n>" when omitted
};

// 46-D1: `default_verdict` is the master switch; `rules` is the general list.
struct PermissionDefaults {
    std::string shell = "ask";
    std::string write = "ask";
    std::string read = "allow";
    std::string default_verdict = "ask";                    // 46-D1
    std::vector<PermissionRuleSettings> rules;              // 46-D1
};

// 46-D12: api_key is a SECRET (0600, never logged); max_tokens reaches
// LlmCallConfig::max_tokens via to_agent_config ->
// AgentConfig::parameters.max_output_tokens -> agent_loop.cpp:539 (O-M3).
struct LlmSettings {
    …
    std::optional<std::string>   api_key;                   // 46-D12.2
    std::optional<std::uint32_t> max_tokens;                // 46-D12.4
};

// 46-D8: the generic tool deadline; 0 disables.
struct ToolsSettings {
    ToolPresentationMode     presentation = ToolPresentationMode::Native;
    std::vector<std::string> tool_order;
    std::int64_t             timeout_ms = 300'000;          // 46-D8
};

} // namespace ymh
```

```cpp
// ── include/ymh/config/jsonc.hpp (new, 46-D2.11) ───────────────────────────
#include <nlohmann/json.hpp>   // the parsed document (G2-M2)
#include <optional>
#include <string>
#include <string_view>
namespace ymh {
// Moved out of the anonymous namespace in src/config/config.cpp so the
// GrantStore can reuse the exact JSONC parser (F-18). Returns the parsed
// document; nullopt with an empty `error` means blank/comments-only (the
// shipped no-op semantics). A parse failure sets `error` and returns nullopt.
[[nodiscard]] std::optional<nlohmann::json>
parse_jsonc_document(std::string_view text, std::string& error);
[[nodiscard]] bool blank_or_comments_only(std::string_view text);
} // namespace ymh
```

```cpp
// ── src/config/config.cpp — llm section handler (46-D12.2/10; N8) ───────────
// Anonymous namespace (the shipped `Json` alias is nlohmann::json, :30). The
// added `bool global_layer` mirrors `apply_document`'s existing parameter
// (`:840-841`) and its `session` guard (`:898-899`); the handler fails a
// non-global `llm.default.api_key` with `fail(source, …)`.
void apply_llm(Config& config, const Json& table,
               const std::filesystem::path& source, bool global_layer);
// call site (inside apply_document): apply_llm(config, *llm, source, global_layer);
```

```cpp
// ── include/ymh/policy/permission_policy.hpp (46-D1/D2) ────────────────────
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace ymh {

// 46-D1: exported so the config parser can validate (F-10). Throws
// PolicyConfigError if the match dimension set is empty, if `command` is set
// without `tool`, or if a glob is malformed. Takes raw strings, so the policy
// layer does not depend on the config layer. Rev 5 / O-H1: the 3-arg form is
// narrowed to 2 args because the config surface has no `path` dimension. The
// config parser catches PolicyConfigError and rethrows via fail(...) as
// ConfigError (O-M4).
void validate_glob(const std::string& pattern);
void validate_rule(std::string_view tool, std::string_view command);

// 46-D2 / O-M1: the shipped PolicyRule gains one field; `literal == true` means
// a non-empty `command` must equal `command_pattern(request)` exactly, never via
// glob_match. `remember()`-derived grants set it; config rules leave it false.
// (All other PolicyRule fields are unchanged, permission_policy.hpp:76-94.)
struct PolicyRule {
    std::string   tool;
    std::string   path;      // retained in the shipped struct, unreachable from
                             // config/grants (O-H1)
    std::string   command;
    PolicyVerdict effect = PolicyVerdict::Ask;
    enum class Layer : std::uint8_t { Builtin, Global, Project, Profile,
                                      CommandLine, LocalGrant, SessionGrant };
    Layer         layer = Layer::Global;
    std::string   id;
    bool          literal = false;   // 46-D2 (O-M1)
};

// 46-D2: the durable grants seam.
class GrantStore {
public:
    virtual ~GrantStore() = default;
    // Loads the grants; called once by the policy constructor (46-D2.3). Returns
    // the LocalGrant rules; a failure degrades to {} (fail-safe re-ask).
    [[nodiscard]] virtual std::vector<PolicyRule> load() = 0;
    // Atomic (temp+fsync+rename), flock(LOCK_EX)-guarded read-modify-write;
    // returns false on failure.
    virtual bool append(const PolicyRule& grant) = 0;
};
[[nodiscard]] std::unique_ptr<GrantStore>
open_file_grant_store(std::filesystem::path grants_path,
                      std::optional<std::string> expected_workspace_id);  // 46-D2.2

class RulePermissionPolicy {
public:
    // 46-D2: nullable; null disables persistence (tests). When non-null, the
    // constructor loads `store->load()` into `local_grants_` before reorder().
    explicit RulePermissionPolicy(PermissionConfig config, GrantStore* store = nullptr);
    // 46-D2: returns the created grant (signature change from void). Returns
    // immediately if `request.force_ask` (O-M5) or the scope/kind excludes a
    // grant. Builds {tool, command, effect=Allow, layer=LocalGrant, literal=true},
    // pushes under mutex_, releases the lock, then store_->append(grant) (O-L1).
    // Not nodiscard (callers may ignore it). The base
    // `PermissionPolicy::remember` (`:126`, `virtual void remember(...)`) changes
    // to `virtual PolicyRule remember(...)` in lockstep; the override at `:142`
    // follows.
    PolicyRule remember(const PermissionRequest& request,
                        payload::PermissionDecisionKind kind,
                        GrantScope scope) override;
};

// 46-D2.13 (Rev 6 / NEW-2): the config-deny veto. In the .cpp, factor the
// per-rule match predicate out of `evaluate()`'s winner loop into a file-local
// helper `bool rule_matches(const PolicyRule&, const PermissionRequest&)` (same
// semantics as the shipped loop: each non-empty dimension — tool, path, command —
// must match; a `literal` command compares `== command_pattern(request)` instead
// of `glob_match`). `evaluate()` runs its existing winner loop unchanged, then:
//   if (winner != nullptr && winner->effect == PolicyVerdict::Allow &&
//       winner->layer == PolicyRule::Layer::LocalGrant) {
//       for (const PolicyRule& c : candidates)
//           if (c.effect == PolicyVerdict::Deny &&
//               c.layer != PolicyRule::Layer::LocalGrant &&
//               c.layer != PolicyRule::Layer::SessionGrant &&
//               rule_matches(c, request))
//               return PolicyVerdict::Deny;
//   }
// `rule_greater` and config-vs-config precedence are NOT changed. This is the
// only rule-selection change in 46-D2.
} // namespace ymh
```

```cpp
// ── include/ymh/agent/workspace_runtime.hpp (46-D2.3; O-H2, Rev 5) ──────────
// The daemon-owned GrantStore is injected as a non-owning pointer, exactly like
// the existing `Executor* executor` field (workspace_runtime.hpp:107).
struct WorkspaceRuntimeOptions {
    Config                config;
    std::filesystem::path root;
    BootId                boot_id;
    bool                  attach_permission_gate = false;
    bool                  attach_permission_resolver = false;
    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> provider_factory;
    std::function<std::unique_ptr<SessionStore>()> store_factory;
    Executor*             executor = nullptr;
    // 46-D2 (O-H2): the caller (the daemon) owns the store and must keep it
    // alive for the runtime's lifetime; null disables durable grants (headless
    // `ymh run`, tests). The Impl passes it to
    // `RulePermissionPolicy(permission_config_, options.grant_store)`.
    GrantStore*           grant_store = nullptr;
    McpClientFactory      mcp_client_factory;
};

// ── src/host/workspace_host.cpp — pinned startup order (46-D2.3; O-H2) ─────
// The shipped order builds the runtime (:489-504) before opening the registry
// (:511), so `findByCanonicalPath(root)` is unavailable at policy construction.
// 46-D2 reorders to (each moved step is runtime-independent):
//   1. registry_ = WorkspaceRegistry::open(config_.registry);
//      registry_->resolvePendingMutations();
//   2. ensureWorkspaceRegistered();                       // :420-440
//   3. const auto record = registry_->findByCanonicalPath(canonical_root_);
//      if (!record) { return HostExitCode::RegistryFailed; }   // Rev 6 / NEW-4:
//      // ensureWorkspaceRegistered may have returned true via its findById
//      // branch, leaving no row at canonical_root_; never deref a nullopt.
//   4. grant_store_ = open_file_grant_store(
//          canonical_root_ / ".ymh" / "permissions.jsonc", record->id.value);
//   5. runtime_options.grant_store = grant_store_.get();
//      runtime_ = *WorkspaceRuntime::create(std::move(runtime_options));
// `grant_store_` is a member declared BEFORE `runtime_` (:353-354) so it
// outlives the runtime's `policy_`.
```

```cpp
// ── src/cli/wiring.cpp — the max_tokens route (46-D12.4; O-M3, Rev 5) ───────
AgentConfig to_agent_config(const Config& config) {
    …
    if (config.llm.max_tokens.has_value()) {                 // NEW (46-D12.4)
        agent.parameters.max_output_tokens = config.llm.max_tokens;
    }
    return agent;
}
// Existing route (unchanged): src/agent/agent_loop.cpp:539
//   config.max_tokens = config_.parameters.max_output_tokens;
// include/ymh/llm/llm_call_config.hpp:28-30 documents the 1:1 mapping at
// dispatch; src/llm/openai_adapter.cpp:739-740 emits `max_tokens`.
```

```cpp
// ── src/core/logging.cpp — sink redaction (46-D12.11; O-M7, Rev 5) ──────────
class SpdlogLogger final : public Logger {
public:
    void log(LogLevel level, std::string_view message) override {
        // NEW (46-D12.11 / O-M7): redact at the sink so every category logger
        // line is covered by construction, not by a call-site list.
        logger_->log(to_spdlog(level), "{}",
                     redact_secrets(std::string{message}));
    }
};
// UI notice ring: `print_localcode_notes` and `surface_notice`
// (src/ui/supervisor.cpp:944-947) pass their text through redact_secrets before
// model_.pushNotice (:929).
```

```cpp
// ── include/ymh/execution/config.hpp (46-D8) ───────────────────────────────
#include <chrono>
namespace ymh {
struct ToolConfig {
    …
    std::chrono::milliseconds tool_timeout{300'000};   // 46-D8, 0 == disabled
};
} // namespace ymh
```

```cpp
// ── include/ymh/execution/environment.hpp (46-D8.5) ────────────────────────
namespace ymh {
class ExecutionEnvironment {
public:
    …
    [[nodiscard]] virtual const ToolConfig& toolConfig() const noexcept = 0;  // 46-D8
};
class LocalEnvironment final : public ExecutionEnvironment {
public:
    …
    [[nodiscard]] const ToolConfig& toolConfig() const noexcept override {
        return config_;
    }
private:
    ToolConfig config_;   // 46-D8: stored (was a constructor param only)
};
} // namespace ymh
```

```cpp
// ── include/ymh/execution/deadline.hpp (new, 46-D8.6) ──────────────────────
#include <chrono>
#include <optional>
namespace ymh {
// nullopt => no deadline; 0ms => expired (immediate timeout); >0 => budget.
[[nodiscard]] std::optional<std::chrono::milliseconds>
clamp_timeout(const std::optional<std::chrono::steady_clock::time_point>& deadline,
              std::chrono::milliseconds caller_ms,
              std::chrono::steady_clock::time_point now);
} // namespace ymh
```

```cpp
// ── include/ymh/execution/process.hpp (46-D8.7) ────────────────────────────
#include <chrono>
#include <optional>
namespace ymh {
struct ProcessRequest {
    …
    std::chrono::milliseconds timeout{0};      // 0 => no deadline (retained)
    std::optional<std::chrono::milliseconds> deadline;   // 46-D8: 0 => expired
};
} // namespace ymh
```

```cpp
// ── include/ymh/mcp/mcp_types.hpp:133-137 — McpCallOptions (46-D8.7) ───────
#include <chrono>
#include <optional>
namespace ymh {
struct McpCallOptions {
    ToolCallId                call_id;
    std::chrono::milliseconds timeout{0};                // retained: 0 => server default
    std::size_t               max_bytes{0};
    std::optional<std::chrono::milliseconds> deadline;   // 46-D8: 0 => expired
};
} // namespace ymh
// clamp caller (N4): caller_ms = (options.timeout.count() > 0 ? options.timeout
//                                        : config_.call_timeout)   // McpServerConfig
```

```cpp
// ── include/ymh/execution/pty.hpp — PTY expiry representation (46-D8.7) ────
// N3: the deadline is a NEW parameter on the PtySession methods; the existing
// wait_ms/timeout parameter is unchanged. The deadline methods are on
// `PtySession` (:351-356), not `PtyService` (:376-387).
namespace ymh {
class PtySession {
public:
    virtual Task<PtyRead> read(std::size_t max_bytes,
                               std::chrono::milliseconds wait,
                               std::optional<std::chrono::milliseconds> deadline,  // 46-D8
                               CancellationToken cancel) = 0;
    virtual Task<PtyExit> wait(std::chrono::milliseconds timeout,
                               std::optional<std::chrono::milliseconds> deadline,  // 46-D8
                               CancellationToken cancel) = 0;
};
struct PtyRead {
    …
    bool timed_out = false;   // 46-D8: deadline expired before/while reading (G2-M8)
};
struct PtyExit {
    …
    bool timed_out = false;   // 46-D8 (G2-M8)
};
} // namespace ymh
```

```cpp
// ── include/ymh/tools/tool_context.hpp (46-D8.3) ───────────────────────────
#include <chrono>
#include <optional>
namespace ymh {
class ToolContext {
public:
    ToolContext(ExecutionEnvironment& execution, Session& session, Logger& logger,
                CancellationToken cancellation, ResourceGovernor& governor,
                OutputSink& output, PermissionHandle& permission,
                ToolCallId call_id, TurnId turn, StepId step,
                std::optional<std::chrono::steady_clock::time_point> deadline);  // 46-D8
    …
    [[nodiscard]] bool has_deadline() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const;
    [[nodiscard]] std::chrono::milliseconds remaining() const noexcept;  // max() when disabled
    [[nodiscard]] bool expired() const noexcept;
private:
    std::optional<std::chrono::steady_clock::time_point> deadline_;      // 46-D8
};
} // namespace ymh
```

```cpp
// ── include/ymh/ui/command_registry.hpp (46-D5/46-D6) ──────────────────────
#include <string>
#include <string_view>
#include <vector>
namespace ymh::ui {
struct CompletionCandidate {
    const Command* command = nullptr;
    std::string    spelling;   // canonical name, or the matched alias
};

class CommandRegistry {
public:
    …
    // 46-D5: canonical names first, then aliases; one candidate per command.
    [[nodiscard]] std::vector<CompletionCandidate>
    complete_candidates(std::string_view prefix) const;
    // 45-D8 retained: command_display_name(const Command&) is an unchanged
    // free function in this namespace.
    // complete() is REMOVED (46-S1).
};
} // namespace ymh::ui
```

```cpp
// ── include/ymh/ui/ui_model.hpp (46-D3/46-D5/46-D6/46-D9/46-D11) ───────────
#include <chrono>
#include <string>
namespace ymh::ui {

struct CommandHint {
    std::string name;         // canonical
    std::string display;      // 45-D8: "exit(quit)"
    std::string insert;       // 46-D6: the matched spelling ("quit" for /q)
    std::string description;
};

// 46-D3: the single-OK notice popup.
struct MessageDialogModel {
    bool        open = false;
    std::string text;
};

struct UiModel {
    …
    MessageDialogModel message;   // 46-D3
    // 46-D9 (active-session-scoped)
    [[nodiscard]] bool has_active_turn() const;
    [[nodiscard]] bool active_has_streaming_reasoning() const;
    [[nodiscard]] bool advance_spinner(std::chrono::milliseconds delta);
};
} // namespace ymh::ui
```

```cpp
// ── include/ymh/ui/ui_event.hpp (46-D3) ────────────────────────────────────
#include <cstdint>
namespace ymh::ui {
enum class UiMode : std::uint8_t {
    Conversation,
    Switcher,
    Dialog,
    ExitConfirm,     // shipped; named here for completeness
    Context,         // shipped; named here for completeness
    Notice,          // 46-D3
};
} // namespace ymh::ui
```

```cpp
// ── src/ui/supervisor.cpp — new/changed private members of `SupervisorApp` ──
// G2-M10: these are member functions and MUST be declared inside the class body
// (`class SupervisorApp final : public UiController`, :356). A file-scope
// anonymous namespace cannot hold a `const` member function.
class SupervisorApp final : public UiController {
    …
    // 46-D6: exact -> Dispatch; partial-with-candidates -> Complete; else Submit.
    // Declared before `classify_enter` (a nested type is not visible to an
    // earlier member declaration).
    enum class EnterAction : std::uint8_t { Dispatch, Complete, Submit };
    // 46-D3: true when the Live switcher has at least one actionable target.
    [[nodiscard]] bool switcher_has_targets() const;
    // 46-D3: opens the notice instead of the switcher when there is no target.
    void openSwitcher();
    // 46-D3: Enter/Esc/Ctrl+C dismiss; every other key is swallowed.
    bool handle_notice(const ftxui::Event& event);
    [[nodiscard]] EnterAction classify_enter(const SessionUiState& state,
                                             const std::string& draft) const;
    // 46-D13: hands the composer to $EDITOR and copies the result back.
    void edit_prompt(SessionUiState& state);
    // 46-D7 (G2-H1; refcounted in Rev 5 / O-M6; guarded in Rev 6 / NEW-1): a
    // per-workspace count of outstanding resumes. Incremented at the top of
    // `resume_after_attach` (:1042) — the single entry point — and released at the
    // two genuine resume terminals only: `apply_resume_success` (:1090) and the
    // `!reply.ok` branch (:1053-1059), both through `release_resume` below.
    // `recover_unknown_session` (:1121) MUST NOT touch this map: it has
    // non-resume callers (prompt :1450, agent.select :1907) that never
    // incremented it, and an unguarded decrement would wrap a zero entry to
    // `SIZE_MAX` and close the D7 gate forever. The gate is
    // `count(workspace) == 0`.
    std::map<WorkspaceId, std::size_t> resume_in_flight_;   // <map> already included (supervisor.cpp:12)
    std::map<WorkspaceId, SessionId> pending_resume_;   // :2773, retained

    // 46-D7 (Rev 6 / NEW-1): the single release seam. Decrements the entry iff it
    // exists and is > 0, erases it at 0, and is a no-op otherwise — so the
    // decrement can never underflow, even if a future caller misuses it.
    void release_resume(const WorkspaceId& workspace);

    // 46-D7 test seam (N6), mirroring `submit_agent` (:1777-1790): when
    // `session_list_replies_installed_`, deliver the canned reply instead of
    // hitting the connection. `refresh_sessions` (:1295) submits through this.
    void submit_session_list(const WorkspaceId& workspace,
                             SupervisorConnection::ReplyFn reply);
    bool           session_list_replies_installed_ = false;
    nlohmann::json session_list_reply_             = nlohmann::json::array();
    int            session_list_error_             = 0;
};
```

```cpp
// ── include/ymh/ui/supervisor_harness.hpp — additive test seams (N6) ────────
// Mirrors the existing `install_agent_replies` (:89) / `seed_ensure_in_flight`
// (:80) / `ensure_in_flight()` (:115) pattern. No production type changes.
class SupervisorHarness {
    …
    virtual void install_session_list_reply(nlohmann::json result, int error) = 0;
    virtual void seed_resume_in_flight(const WorkspaceId& workspace) = 0;
    [[nodiscard]] virtual const std::map<WorkspaceId, std::size_t>&
    resume_in_flight() const = 0;                       // O-M6: refcount view
    // O-H3: force the link Dead while a request batch is pending, so the test
    // asserts every pending reply (including a queued `session.resume`) is
    // delivered and the marker is cleared.
    virtual void drop_connection(const WorkspaceId& workspace) = 0;
};
// 46-D7 / O-H3 also amends `SupervisorConnection` (production, not a test seam):
// `process_requests` (:376-406) replies {ok=false,"connection lost"} to every
// remaining request in its local `batch` before it returns, and
// `handle_disconnect` (:431-440) drains `requests_` with the same reply.
```

```cpp
// ── include/ymh/ui/session_export.hpp (46-D13) ─────────────────────────────
#include <optional>
#include <string>
namespace ymh::ui {
[[nodiscard]] std::optional<std::string>
edit_text_in_editor(const std::string& initial, std::string& error);
} // namespace ymh::ui
```

```cpp
// ── include/ymh/llm/provider_registry.hpp (46-D12.3) ───────────────────────
#include <optional>
#include <string>
namespace ymh {
struct LLMProviderConfig {
    …
    std::string  api_key_env;              // NAME of the env var (retained)
    std::optional<std::string> api_key;    // 46-D12: literal; outranks api_key_env
};
} // namespace ymh
```

```cpp
// ── src/ui/ui_model.cpp (46-D11) — the UserMessage branch gains a line ─────
} else if constexpr (std::is_same_v<T, UserMessage>) {
    ConversationEntry entry;
    …
    state.conversation.entries.push_back(std::move(entry));
    if (e.source.kind == MessageSource::Kind::User) {   // 46-D11 (F-34)
        state.input.push_history(e.text);
    }
    dirty.mark(e.session, UiDirtyFlag::Conversation);
}
```

---

## 17. Invariants

Numbered `46-I#`. Each is testable or explicitly relabelled a
**retained/regression pin** and excluded from the gate (F-55 / gate §D). The
"Gate" column is `Y` (gates this change) or `pin` (retained behaviour).

| ID | Gate | Invariant |
|---|---|---|
| 46-I1 | Y | An unknown `permissions`/`tools` key, a rule with no match dimension, a `command` rule without `tool`, a `path` key **in a config rule** (the dimension does not exist, O-H1), a malformed glob, or an effect outside `{allow, ask, deny}` is a `ConfigError` at load, never a silent default (`src/config/config.cpp:349-357`, `:807-818`; `src/policy/permission_policy.cpp:81-105`). **A `path` key in a grants-file entry is not a `ConfigError`:** the grants file is not the layered config, so the loader rejects the entry and the file is **fail-safe-skipped** — logged, `{}` loaded, re-ask — per 46-D2.1/3 and 46-I7/46-I33 (Rev 6 / NEW-3). The exported validator throws `PolicyConfigError`; the parser translates it to `ConfigError` (O-M4). |
| 46-I2 | Y | When `permissions.default == "allow"`, every builtin tool rule, every MCP tool default and the fallback verdict are `Allow`; **no Ask is emitted for a path tool inside the workspace, and none for any tool except where `force_ask` applies** (46-I3) (46-D1.3/5). **Scope (O-M2):** the "inside the workspace" confinement holds only for path tools (via `resolve()`); `shell`/`git`/`mcp.*` are unrestricted command execution, not repo-confined. |
| 46-I3 | Y | `permissions.default == "allow"` never bypasses `SandboxMode::ReadOnly` mutating hard-deny or `PermissionRequest::force_ask` (`src/policy/permission_policy.cpp:205-210`; 25-A11). |
| 46-I4 | pin | A **path tool's** path is always under `ExecutionEnvironment::root()`; an out-of-root path throws `ToolErrorCode::PathEscape` (`src/execution/environment.cpp:60-87`; `include/ymh/execution/errors.hpp:18`). This is not an OS sandbox and does not constrain `shell`/`git`/`mcp.*` (O-M2). (Retained; excluded from the gate.) |
| 46-I5 | Y | An `Always` grant is written to `<workspace>/.ymh/permissions.jsonc` only after a human decision with `kind != Deny && scope == Always`, and is loaded at policy construction from the injected `GrantStore` as `Layer::LocalGrant`. A grant carries **`tool` + `command` only** (no `path`) and is **literal** (`literal = true`). The write is owned by `RulePermissionPolicy::remember`, so **all three call sites** (broker, gate, loop) persist (46-D2.3/4; O-H1/O-M1/O-H2). |
| 46-I6 | Y (seam clause); pin (Once/Session retained) | `Once` persists nothing; `Session` stays in memory and is lost on daemon restart; `force_ask` never persists — enforced **inside `remember()` itself** (the single mutation seam, O-M5), with the three call-site guards retained as redundant defense (`src/policy/permission_policy.cpp:413`; `src/agent/agent_loop.cpp:732`; `src/permission/permission_broker.cpp:332`; `src/policy/permission_policy.cpp:260`). The **in-`remember()` guard is new behavior** and is gated by `UI46_D2_ForceAskGuardInsideRemember` and `UI46_D2_ForceAskNotPersistedAtGate`; the `Once`/`Session` clauses are retained pins (N7). |
| 46-I7 | Y | A grants-file read/write failure is non-fatal: re-ask on read; degrade to `Session` on write (46-D2.7). |
| 46-I8 | Y | The Ctrl+S Live switcher renders the `Notice` iff `switcher_has_targets()` is false; the notice has exactly one option, `[ OK ]`, and no workspace/session rows (46-D3). |
| 46-I9 | Y | `Enter`, `Escape` and `Ctrl+C` dismiss the notice; every other key is swallowed; `modal_owns_input()` is true while the notice is open (`src/ui/supervisor.cpp:1511-1515`). |
| 46-I10 | Y | `complete_candidates` matches canonical names and aliases, returns one candidate per command in registration order, and reports the matched spelling; `complete()` does not exist (46-D5). |
| 46-I11 | Y | `complete_selected_command` and the Enter completion insert `"/" + hint.insert` (+ a trailing space for Tab/Enter), never a hard-coded canonical name (46-D6.5). |
| 46-I12 | Y | Typing `/quit` yields the same hint row as `/exit`, `> /exit(quit) - quit the supervisor` (`src/ui/ui_render.cpp:373-380`; 45-D8). |
| 46-I13 | Y | On `Attached`, the active workspace with no active session focuses a live session if one exists, otherwise creates a clean session; it never resumes `stored.front()` (46-D7). |
| 46-I14 | Y | `--resume`, an explicit Ctrl+S/`/sessions` selection, **and the already-attached `resume_from_history` branch** all resume, **and the `session.list` reply branch is gated on `resume_in_flight_.count(workspace) == 0`**, so a list reply that lands between a resume submit and its reply never creates a stray session (46-D7.2; 22 §5.2). `resume_in_flight_` is a **per-workspace refcount** incremented at the top of `resume_after_attach` (the single entry point, covering `:1036` and `:1275`) and released **only** at the two genuine resume terminals — `apply_resume_success` and the `!reply.ok` branch — via `release_resume`, which decrements iff the entry exists and is `> 0` and erases at 0 (Rev 6 / NEW-1). `recover_unknown_session` **never touches the marker**, because `prompt()` (`:1450`) and `agent.select` (`:1907`) reach it on `UnknownSession` with no resume in flight; decrementing there would wrap a zero entry to `SIZE_MAX` and permanently close the D7 gate. No exit can leak it because `SupervisorConnection::process_requests`/`handle_disconnect` reply to **every** pending request on disconnect (O-H3), and overlapping resumes keep the count ≥ 1 until both resolve (O-M6). (G2-H1; N1/N2; Rev 5 O-H3/O-M6; Rev 6 NEW-1.) |
| 46-I15 | Y | When `tools.timeout_ms > 0`, every tool call runs with an absolute deadline; each blocking primitive clamps to `clamp_timeout` so an **expired** deadline yields an immediate timeout and a model-supplied timeout is a request, never an extension. The MCP clamp's `caller_ms` is the existing effective timeout, so the configured server `call_timeout` is a ceiling (N4). When `tools.timeout_ms == 0` no deadline is enforced (46-D8). |
| 46-I16 | Y | A deadline expiry produces an error `ToolResult` with `ToolErrorCode::Timeout`; **split by primitive (G2-M6):** for `process`/PTY the `ToolError` is thrown and caught by `ToolRegistry` (`src/tools/tool_registry.cpp:358-362`, setting the `error` field at `:25`); for **MCP** the tool catches `McpError` itself (`src/mcp/mcp_tool.cpp:57-62`) and 46-D8.7 pins it to map `CallTimeout` → `throw ToolError{ToolErrorCode::Timeout}`. The turn continues and the durable `ToolResult` records the error (46-D8.11/12). |
| 46-I17 | pin | No tool deadline is enforced by abandoning a thread: `execute()` is synchronous and the deadline kills the primitive (`include/ymh/core/task.hpp:42-43`; 46-D8.13). (Retained; excluded from the gate.) |
| 46-I18 | Y | The status line shows the turn spinner iff the **active session's** `agent_state` is `Thinking` or `CallingTool`; the glyph set is `kReasoningSpinnerFrames`; the spinner is the first element of `left_cells`, before `mode` (46-D9). |
| 46-I19 | Y | `advance_spinner` advances the shared frame clock while the **active session's** turn is active or the active session streams reasoning; `animation_active_` additionally retains the global `has_streaming_reasoning()` repaint condition (RB-17) (46-D9.1/3/6). |
| 46-I20 | Y | After Esc hides the command list, any command-prefix edit that reaches the insert/delete branches clears `hints_dismissed` and rebuilds the list; history recall does not, and a character swallowed by the RB-12 25 ms modal-tail window is not a command-prefix edit (45-D5/45-I9 retained; `src/ui/supervisor.cpp:2341-2354`, `:2467-2474`, `:2060-2062`). |
| 46-I21 | Y | `InputModel::history` is hydrated from replayed/live `UserMessage` events **whose `source.kind == MessageSource::Kind::User`**, in log order; `push_history` de-duplication prevents a live duplicate (46-D11). |
| 46-I22 | Y | The localcode import maps `providers[*].api_key` → `llm.default.api_key`, `skip_permissions` → `permissions.default = "allow"`, `permission.<tool>[]` → `permissions.rules`, `profiles[*].max_tokens` → `llm.default.max_tokens`, `context_window` → `agent.compaction.context_window_tokens`, and nothing else new (46-D12.7). |
| 46-I23 | pin | The imported document is written only to the global layer (0600), and only on first run; no re-import, no merge (`src/cli/cli.cpp:938-941`, `:851-853`, `:894`). (Retained; excluded from the gate.) |
| 46-I24 | Y | `llm.default.api_key` is accepted **only in the global layer** (a workspace-layer `api_key` is a `ConfigError`), is never logged, never rendered in the UI, never copied into the session event log, and takes precedence over `api_key_env`; it is the only secret literal permitted in `Config` (46-D12.2/10/11; 46-S6). |
| 46-I25 | Y | Ctrl+E hands the terminal to `$EDITOR` (via `$VISUAL` → `$EDITOR` → `vi`), and on exit 0 copies the file contents into `input.draft`, sets `cursor`, clears `hints_dismissed`, and refreshes hints; on any non-zero status (including 127) the draft is unchanged (46-D13). |
| 46-I26 | Y | The Ctrl+E scratch file is 0600 and unlinked on every path (success, failure, signal) (46-D13.1). |
| 46-I27 | Y | `render_dialog`, `render_exit_confirm` and `render_switcher` are composited with `ftxui::clear_under`; no transcript cell is visible inside their windows (46-D4). |
| 46-I28 | Y | The `permissions.default == "allow"` master switch wins over `shell`/`write`/`read`; the scaffold comment and the docs state this (46-D1.6). |
| 46-I29 | Y | An **expired** deadline reaching a primitive produces an immediate timeout — never an unbounded wait and never a `0`-as-disabled wait — for `process` (not started, `timed_out`), PTY (`PtyRead`/`PtyExit.timed_out`, set from the **new** deadline parameter, never inferred from `wait_ms == 0`), and MCP (immediate `McpErrorCode::CallTimeout`) (46-D8.7). A **disabled** deadline reaches PTY as the **absent** optional (`nullopt`), which maps to today's `wait_ms`/`timeout` semantics; `timed_out` stays `false` (N3). |
| 46-I30 | Y | A **disabled** tool deadline (`tools.timeout_ms == 0`) produces `remaining() == milliseconds::max()` and `has_deadline() == false`; the tool-deadline layer is off. A model-supplied `timeout_ms` (`src/tools/builtin_tools.cpp:391-394`) and the MCP server `call_timeout_ms` default (`include/ymh/config/config.hpp:134`) still apply, so "no tool call times out" is **not** asserted (G2-M7). (46-D8.2/3/9). |
| 46-I31 | Y | `git_status`/`git_diff` check `context.expired()` before and after the libgit2 call; a mid-call hang is documented as non-preemptible (46-D8.8). |
| 46-I32 | Y | On Enter: an exact command name/alias dispatches; a partial command-shaped draft with candidates is completed in place and **not** dispatched; a second Enter dispatches. `/q` + Enter never quits (46-D6). |
| 46-I33 | Y | The grants file carries `"schema": "ymh.permissions/1"` and a `workspace_id` equal to the resolved `WorkspaceRecord::id` (`WorkspaceRegistry::findByCanonicalPath`, resolved **before** the runtime is built, O-H2); a missing marker, a mismatched id, or the absence of a registry row means the file is ignored (re-ask). Every loaded grant has `effect = Allow` forced by the loader, **no `path` key**, and `literal = true` unless the entry explicitly sets `"match": "glob"` (46-D2.1/3; O-H1/O-M1). |
| 46-I34 | Y | `ToolConfig` is visible to the agent loop via `ExecutionEnvironment::toolConfig()`; the loop derives the deadline from `tool_timeout` (46-D8.5). |
| 46-I35 | Y | `git` is in the cooperative bucket; no universal claim that every blocking tool is preemptible is made (46-D8.8). |
| 46-I36 | Y | Any **`SpdlogLogger::log` line** or **UI notice-ring string** that **matches the pinned secret shapes** is redacted to `[REDACTED]` (`src/llm/redaction.cpp:10`, the shipped token). The pinned shapes are the shipped `Bearer <token>`, `sk-…`, and `api_key=`/`api-key=`/`apikey=<token>` forms (`src/llm/redaction.cpp:52-98`) plus the JSON form `"api_key"\s*:\s*"…"` that 46-D12.11 adds. The redactor is **shape-based** and has no knowledge of the configured value, so a configured key printed in an **unrecognized shape is not guaranteed redacted** — the primary control is **never stringifying the key** (`include/ymh/llm/redaction.hpp:14`; §21 Q16), and the sink is the backstop for the known shapes. Enforced **at the logger sink** (`src/core/logging.cpp:42-46` now calls `redact_secrets`) and at the pinned notice-ring producers, not by a call-site list (46-D12.11; G2-M14; Rev 5 O-M7/O-L2; Rev 6 NEW-6). |
| 46-I37 | Y | An explicit config **deny** (a matching `PolicyRule` with `effect == Deny` and a non-grant layer — `Builtin`/`Global`/`Project`/`Profile`/`CommandLine`) overrides a matching durable `Always` grant (`Layer::LocalGrant`, `effect == Allow`) **regardless of specificity**; `rule_greater` and config-vs-config precedence are unchanged. This is what makes the D1.6/F1 recovery ("add a `permissions.rules` deny") revoke a durable grant, and it is consistent with localcode's `decision: "deny"`, which imports as a `Layer::Project` rule (the 46-D12.7 mapping itself is unchanged). `Session` grants are deliberately out of scope of the veto (the human's explicit in-session decision). (Rev 6 / NEW-2; 46-D2.13.) |

---

## 18. Failure modes

Prefix `46-F#` (trigger / symptom / recovery). **§54 mapping (F-55):** the `§54`
column maps each 46-F# to the closest architecture finding (`00-architecture.md`
§54 "Findings reference F1–F12", `:4832-4856`); `—` means errata-local. **Every
row has a §20 test or is an explicit documented no-op (F-55, corrected in Rev 3 /
G2-L1):** the explicit no-op/outcome rows are listed at the end of this table, and
the previously-untested rows 46-F1/F5/F17/F24/F25 now name a test.

| ID | §54 | Trigger | Symptom | Recovery |
|---|---|---|---|---|
| 46-F1 | F6 | `permissions.default = "allow"` with `permissions.shell = "deny"` | The user expects shell to stay denied | The master switch wins; use `permissions.rules` with `{tool:"shell",effect:"deny"}` — which also revokes a durable `Always` grant (46-I28, 46-D1.6, 46-D2.13; Rev 6 / NEW-2) |
| 46-F2 | F10 | Grants file malformed/unreadable/foreign at startup | A stale or partial grant set | Log, load `{}`, re-ask (fail-safe; 46-I7) |
| 46-F3 | F8 | Append fails (read-only, disk full, lock timeout) | The grant is not durable | Keep the in-memory grant, degrade to `Session` (46-I7) |
| 46-F4 | F6 | Only the active workspace is live with one session | `(current session hidden)` alone | The `Notice` replaces the switcher (46-I8) |
| 46-F5 | F4 | A workspace reply lands after the notice opened | The notice is stale | Stays until dismissed; the next Ctrl+S rebuilds (accepted) |
| 46-F6 | — | A long permission summary | Transcript bleeds through | `clear_under` (46-I27) |
| 46-F7 | F6 | A prefix matches an alias and a canonical name | Ambiguous candidate order | Registration order; highlight at the first (46-D5) |
| 46-F8 | F6 | `/q` + Enter | It must not quit | Complete-only; the second Enter executes (46-I32) |
| 46-F9 | F10 | `session.create` fails on a fresh launch | No session | The existing failure path surfaces a notice; the first keystroke retries (23 §1.1) |
| 46-F10 | F8 | A `shell` command ignores SIGTERM | A hung process past the deadline | terminate-grace → SIGKILL (`include/ymh/execution/config.hpp:32-33`) |
| 46-F11 | F8 | A hung MCP server | A hung tool | The MCP call timeout is clamped to the deadline; the transport closes (46-D8.7) |
| 46-F12 | F8 | A pure in-process tool loop ignores the deadline | The deadline cannot preempt | Documented cooperative limit (46-D8.14) |
| 46-F13 | F12 | A turn ends between repaints | A stale spinner frame | `has_active_turn()` false → the spinner disappears on the next repaint (46-I18) |
| 46-F14 | F6 | A future edit stops clearing `hints_dismissed` on insert | The list never reappears after Esc | `UI46_D10_SlashEscThenTypeRebuildsList` fails (46-I20) |
| 46-F15 | F10 | A replayed `UserMessage` duplicates a live one | A duplicated history entry | Only when not the immediately previous entry; replay precedes new prompts (46-I21) |
| 46-F16 | F8 | A resumed session with many prompts | O(n) history build | `push_history` dedups against the tail only (O(1) amortized) (46-I21) |
| 46-F17 | F10 | localcode has no `default_profile`/`providers` | Model/API-key fields absent | No import of those fields; retained (46-D12.3) |
| 46-F18 | — | `$EDITOR` unset and `vi` absent | Ctrl+E does nothing | `run_editor` returns 127; the draft is unchanged and a notice is shown (46-I25) |
| 46-F19 | F9 | The editor is killed by a signal | A partial edit would be copied | Non-zero status → the edit is discarded; the old draft is kept (46-I25) |
| 46-F20 | F10 | The global config directory exists | A second import would overwrite user config | Import is skipped entirely; no merge (46-I23) |
| 46-F21 | F6 | A `permissions.rules` entry with `command` but no `tool`, or with a `path` key | A silently-ignored deny, or a dead path rule | `ConfigError` at load (46-D1.2, 46-I1; O-H1) |
| 46-F22 | F8 | `SIGKILL` mid-append | A truncated grants file | Temp+fsync+rename; the original is intact (46-D2.7) |
| 46-F23 | F2 | A repo ships a pre-seeded `.ymh/permissions.jsonc` | Auto-grant before any prompt | Missing `schema` marker ⇒ ignored (46-D2.3, 46-I33) |
| 46-F24 | F8 | Two writers in a spawn/teardown window | A lost update | `flock`; the loser degrades to `Session` (46-D2.8) |
| 46-F25 | F6 | A partial draft with no candidates (`/zz`) + Enter | No completion | Falls through to the existing `unknown command` notice (46-D6.1) |
| 46-F26 | F10 | `--resume`/Ctrl+S/`/sessions` with a `session.list` reply that lands between the resume submit and its reply | A stray empty session | The reply branch is skipped while `resume_in_flight_.count(workspace) != 0`; the refcount is incremented in `resume_after_attach` so the explicit path is covered too, and stays ≥ 1 across overlapping resumes (46-D7.2, 46-I14; G2-H1, N1; Rev 5 O-M6) |
| 46-F27 | F8 | An expired deadline reaches a primitive | An unbounded wait | The primitive returns an immediate timeout (46-D8.7, 46-I29) |
| 46-F28 | F8 | `tools.timeout_ms = 0` | A disabled deadline misread as expired | `remaining() == max()`, no timeout (46-D8.2, 46-I30) |
| 46-F29 | F6 | A character typed inside the 25 ms modal-tail window | The list does not reappear on that keystroke | The next non-suppressed edit rebuilds it (46-I20) |
| 46-F30 | F7 | A plugin/goal-sourced `UserMessage` | Plugin text recalled as a user prompt | Only `Kind::User` is hydrated (46-I21) |
| 46-F31 | F10 | A provider `type` ymh does not support (`bedrock`) | Partial provider state | The provider block is skipped with a note (46-D12.7) |
| 46-F32 | F10 | A resume fails with a non-transport, non-`UnknownSession` error, or an `UnknownSession` arrives for an evicted workspace | The refcount could leak (stranding the UI) or the workspace is left with no focus | The `!reply.ok` terminal calls `release_resume` before the `unknown`/`surface_notice` split (so both sub-exits are covered); `recover_unknown_session` never touches the marker, so a non-resume `UnknownSession` (`prompt`/`agent.select`) cannot underflow it; focus is restored by the first keystroke's 23 §1.1 create path (46-D7.2, 46-I14; N2/N9; Rev 6 NEW-1) |
| 46-F33 | F6 | The exported `validate_rule`/`validate_glob` throws `PolicyConfigError` out of the config parser | An uncaught policy exception, or a config error not mapped to exit 2 | `apply_permissions`/`apply_jsonc_file` catch `PolicyConfigError` and rethrow via `fail(source, …)` as `ConfigError` (O-M4) |
| 46-F34 | F6 | A user enables `permissions.default = "allow"` believing command tools are repo-confined | Unrestricted shell/git/mcp execution as the daemon uid | Stated in 46-D1.4/46-I2; the switch is "no asking", not an OS sandbox (O-M2) |
| 46-F35 | F6 | An `Always` on a path tool is expected to be path-scoped | It is tool-wide within the workspace, so one `Always` on `write_file src/a.txt` auto-allows every `write_file` including `.ymh/` | By design and **explicitly accepted**: no path dimension exists in a durable grant (O-H1; §4.2 item-2/13; §21 Q16) |
| 46-F36 | F6 | An `Always` on `rm -rf build/*` | The stored grant could match `rm -rf build/../../etc` | The grant is literal (`literal = true`), so it matches only the exact command (O-M1) |
| 46-F37 | F10 | The daemon cannot resolve its workspace id (registry open/registration fails), **or `ensureWorkspaceRegistered` succeeds via its `findById` branch while `findByCanonicalPath(canonical_root_)` has no row** (Rev 6 / NEW-4) | Grants could load under the wrong identity, or a nullopt `record` is dereferenced (undefined behavior) | Startup fails with `HostExitCode::RegistryFailed` before the runtime is built; the pinned order guards the `optional` (`if (!record) return …`) before opening the store (O-H2; Rev 6 NEW-4) |
| 46-F38 | F10 | The connection drops with a `session.resume` queued behind another request | The resume never gets a reply and the marker leaks forever | `process_requests`/`handle_disconnect` reply to every pending request, so the failure branch decrements the refcount (O-H3) |
| 46-F39 | F10 | Two overlapping resumes for one workspace | The first terminal reply clears a set and a `session.list` reply creates a stray session | The per-workspace refcount stays ≥ 1 until both resolve (O-M6) |
| 46-F40 | F6 | A durable `Always` grant is in force and the operator adds a `permissions.rules` deny to revoke it | The deny is silently ignored because `Layer::LocalGrant` outranks `Layer::Project` in `rule_greater` | A matching non-grant `Deny` rule vetoes a matching `Allow` `LocalGrant` in `evaluate()`, regardless of specificity, so the deny revokes the grant; deleting the grants file also works (Rev 6 / NEW-2; 46-D2.13, 46-I37) |

**No-op/outcome rows:** 46-F8 (complete-only; `/q` + Enter never quits) and
46-F12 (cooperative limit) are documented outcomes, not recovery paths; they are
covered by 46-I32/46-I35 tests. Every other row names a §20 test, including the
Rev-3 additions for 46-F1 (`UI46_D1_MasterSwitchBeatsShellDeny`), 46-F5
(`UI46_D3_StaleNoticeStaysUntilDismissed`), 46-F17
(`UI46_D12_NoProfileNoImport`), 46-F24
(`UI46_D2_LockContentionDegradesToSession`), and 46-F25
(`UI46_D6_UnknownPartialFallsThrough`). The Rev-4 additions 46-F32 names
`UI46_D7_UnknownSessionEvictedClearsMarker` (N2) and
`UI46_D7_NonTransportResumeFailureFirstKeystrokeRecovers` (N9). The Rev-5
additions 46-F33–46-F39 name `UI46_D1_MalformedRuleIsConfigError` (F33),
`UI46_D1_DefaultAllowShellGitMcp` (F34), `UI46_D2_PathToolGrantIsToolWide` (F35),
`UI46_D2_GrantCommandIsLiteral` (F36), `UI46_D2_StoreInjectedBeforeRuntime`
(F37), `UI46_D7_DisconnectDrainsPendingResume` (F38), and
`UI46_D7_OverlappingResumesKeepGateClosed` (F39). The Rev-6 additions 46-F40
names `UI46_D2_ConfigDenyOverridesDurableGrant` (F40) and 46-F32 gains
`UI46_D7_NonResumeUnknownSessionDoesNotDecrement` (NEW-1).

---

## 19. dsh mapping

- **Permissions are a policy projection, not UI state.** 46-D1 and 46-D2 add
  inputs (config, grants file) to the same pure `RulePermissionPolicy`; the
  evaluation order is untouched (`src/policy/permission_policy.cpp:204-252`).
  This mirrors dsh's "the policy is a total function of request + rule set".
  Rev 2 narrows the purity contract: `evaluate()` stays total; `remember()` is the
  single best-effort mutation seam (46-D2.4).
- **The grant store is a daemon-owned durable artifact.** Like dsh, an approval
  that outlives the process must be recorded outside the process. 46-D2 puts I/O
  in a daemon-owned `GrantStore` injected into the policy, exactly as the session
  log and registry are daemon-owned. The daemon constructs and owns the store
  (declared before `runtime_`), resolves the workspace id from the registry
  **before** building the runtime, and passes a non-owning `GrantStore*` through
  `WorkspaceRuntimeOptions`; the policy loads the grants in its constructor and
  serializes appends via the store's `flock` (O-H2).
- **The switcher/notice is presentation-only.** 46-D3 adds a UI mode, not a
  protocol surface; no RPC changes. Membership is still derived from the Live
  catalog projection (45-D3).
- **The tool deadline is a core-loop concern.** 46-D8 threads an optional
  deadline through the existing `ToolContext`; the tool registry and transport are
  unchanged. This matches dsh's "cancellation is cooperative and plumbed, never a
  detached killer".
- **Alias completion and Enter semantics are registry/UI concerns.**
  46-D5/46-D6 keep the single source of truth (`Command::aliases`) and derive both
  display and matching from it; no alias table is duplicated.
- **The spinner is a pure-renderer projection of turn state.** 46-D9 derives the
  spinner from the active session's `agent_state` and advances it in `onTick`,
  never in `Render()` (10 D16, §20.1).
- **History hydration reuses the event projection.** 46-D11 makes
  `InputModel::history` a projection of the durable, user-sourced `UserMessage`
  stream, like the conversation model, rather than a second in-memory log.
- **The import is a pure mapping plus one secret.** 46-D12 keeps
  `build_localcode_import` a pure document mapper and confines the secret to the
  `0600` file write and the redacted provider path.

---

## 20. Test plan

### 20.1 Unit

Tests marked **(new)** fail on the rev-1 tree; tests marked **(re-pointed)** pass
today but must be re-pointed at the new field/contract to be non-vacuous; tests
marked **(pin)** are retained-behaviour regression pins (excluded from the gate).

| Test | Covers | Non-vacuity |
|---|---|---|
| `UI46_D1_DefaultAllowNoPrompts` | 46-I2 | new |
| `UI46_D1_DefaultAllowShellGitMcp` | 46-I2 (all tool classes) | new |
| `UI46_D1_DefaultAllowCannotBypassReadOnly` | 46-I3 | new |
| `UI46_D1_DefaultAllowCannotBypassForceAsk` | 46-I2/I3 (the `force_ask` carve-out) | new |
| `UI46_D1_DefaultAllowPathToolsNoPrompts` | 46-I2/I4 (path tools only; renamed from `DefaultAllowPathScoped`, O-M2) | new |
| `UI46_D1_RulesOutrankBuiltins` | 46-I1 (specificity+layer; `src/policy/permission_policy.cpp:58-79`) | new |
| `UI46_D1_CommandOnlyRuleRequiresTool` | 46-I1, 46-F21 | new |
| `UI46_D1_PathKeyRejected` | 46-I1, 46-F21 (a `path` key in a rule is a `ConfigError`) | new (O-H1) |
| `UI46_D1_CommandDenyBeatsBuiltinAllow` | 46-D1.2 | new |
| `UI46_D1_MasterSwitchBeatsShellDeny` | 46-I28, 46-F1 | new (G2-L1) |
| `UI46_D1_MalformedRuleIsConfigError` | 46-I1 | re-pointed (asserts the new keys, not the pre-existing unknown-key rejection) |
| `UI46_D1_UnknownPermissionKeyIsConfigError` | 46-I1 | re-pointed |
| `UI46_D2_AlwaysPersistsAcrossRestart` | 46-I5 | new |
| `UI46_D2_AlwaysPersistsThroughLoop` | 46-I5 (loop path) | new |
| `UI46_D2_AlwaysPersistsThroughGate` | 46-I5 (gate path) | new |
| `UI46_D2_OnceNotPersisted` / `UI46_D2_SessionNotPersisted` | 46-I6 | new |
| `UI46_D2_ForceAskNotPersisted` | 46-I6 | new |
| `UI46_D2_MalformedGrantsFileReasks` | 46-I7, 46-F2 | new |
| `UI46_D2_MissingMarkerIgnored` | 46-I33, 46-F23 | new |
| `UI46_D2_ForeignWorkspaceIgnored` | 46-I33, 46-F16 | new |
| `UI46_D2_AppendFailureDegradesToSession` | 46-I7, 46-F3 | new |
| `UI46_D2_LockContentionDegradesToSession` | 46-D2.8, 46-F24 | new (G2-L1/L4) |
| `UI46_D2_ForceAskNotPersistedAtGate` | 46-I6, 46-D2.6 | new (G2-M5) |
| `UI46_D2_AtomicReplaceSurvivesKill` | 46-D2.7, 46-F22 | new |
| `UI46_D2_DeleteFileDropsGrants` | 46-D2.9 | new |
| `UI46_D2_GrantIsWorkspaceScoped` | 46-D2.2 | new |
| `UI46_D2_GrantIdIsContentDerived` | 46-D2.12, gate F-56 | new |
| `UI46_D2_GrantCommandIsLiteral` | 46-I5, 46-F36 (O-M1) | new (an `Always` on `rm -rf build/*` does not match `rm -rf build/../../etc`) |
| `UI46_D2_PathToolGrantIsToolWide` | 46-I5, 46-F35 (O-H1) | new (an `Always` on `write_file src/a.txt` allows `write_file src/b.txt`, by design) |
| `UI46_D2_StoreInjectedBeforeRuntime` | 46-D2.3, 46-F37 (O-H2) | new (the daemon opens the registry, resolves the id, and opens the store before `WorkspaceRuntime::create`) |
| `UI46_D2_LoadsViaConstructor` | 46-D2.3 (O-H2) | new (the policy constructor calls `store->load()`; grants are folded before `evaluate()`) |
| `UI46_D2_AppendOutsideMutex` | 46-D2.4, O-L1 | new (a slow append does not block `evaluate()`) |
| `UI46_D2_ForceAskGuardInsideRemember` | 46-I6, O-M5 | new (a direct `remember()` with `force_ask` persists nothing even with no call-site guard) |
| `UI46_D2_PathKeyInGrantRejected` | 46-I33, O-H1 | new |
| `UI46_D2_ConfigDenyOverridesDurableGrant` | 46-I37, 46-F40 (Rev 6 / NEW-2) | new (with a durable `Always` `LocalGrant` for `write_file` loaded, a `permissions.rules` `{tool:"write_file",effect:"deny"}` makes `evaluate()` return `Deny` even though the grant is the more specific/higher-layer rule; fails on the shipped `rule_greater`, where `LocalGrant` wins. Also asserts config-vs-config precedence is unchanged and a `Session` grant is not vetoed) |
| `UI46_D3_NoOtherWorkspaceShowsNotice` | 46-I8, 46-F4 | new |
| `UI46_D3_NoticeEnterDismisses` / `_EscapeDismisses` | 46-I9 | new |
| `UI46_D3_NoticeSwallowsOtherKeys` | 46-I9 | new |
| `UI46_D3_OtherLiveWorkspaceShowsSwitcher` / `_OtherSessionShowsSwitcher` | 46-D3.2 | new |
| `UI46_D3_NoticeBlocksComposer` | 46-I9 | new |
| `UI46_D3_StaleNoticeStaysUntilDismissed` | 46-F5 | new (G2-L1) |
| `UI46_D5_QuitShowsExitRow` | 46-I12 | new |
| `UI46_D5_CompleteMatchesAliases` | 46-I10 | new |
| `UI46_D5_CompleteDedupesByCommand` | 46-I10 | new |
| `UI46_D5_CompleteOrderRegistration` | 46-I10 | new |
| `UI46_D5_CompleteCanonicalPreferred` | 46-I10 | new |
| `UI46_D6_QCompletesToQuit` | 46-I32 | new |
| `UI46_D6_QTabInsertsSpelling` | 46-I11 | new |
| `UI46_D6_QEnterCompletesOnly` | 46-I32, 46-F8 | new |
| `UI46_D6_QTwoEnterExecutes` | 46-I32 | new |
| `UI46_D6_ExactCommandDispatchesOnFirstEnter` | 46-I32 | new |
| `UI46_D6_CanonicalPrefixInsertsCanonical` | 46-I11 | new |
| `UI46_D6_CompleteRetired` | 46-I10 (compile-level) | new |
| `UI46_D6_MultiCandidateEnterUsesHighlight` | 46-D6.1 (G2-M18) | new |
| `UI46_D6_QuitExactOneEnter` | 46-D6.1 (G2-L18) | new |
| `UI46_D6_UnknownPartialFallsThrough` | 46-D6.1, 46-F25 | new (G2-L1) |
| `UI46_D7_FreshStartCreatesEmptySession` | 46-I13 | new |
| `UI46_D7_NoResumeOfStored` | 46-I13 | new |
| `UI46_D7_ResumeFlagStillResumes` | 46-I14 | pin (passes before/after; guarded by `ResumeInFlightBeatsListReply`) |
| `UI46_D7_ResumeInFlightBeatsListReply` | 46-I14, 46-F26 | new (G2-H1: forces the list reply to land between the resume submit and its reply; fails on the rev-2 `pending_resume_` gate). **Deterministic injection (N6):** the test calls `harness->install_session_list_reply(<non-empty stored array>, 0)`, `harness->seed_resume_in_flight(workspace)`, then `harness->on_link_state(workspace, Attached, "test")`; the canned `session.list` reply is delivered **synchronously** inside `refresh_sessions` and its decision lambda is enqueued; `harness->drain_actions()` then runs it with the marker already set. The test asserts `harness->resume_in_flight().count(workspace) != 0` and no session created, then calls `harness->apply_resume_success(...)` and re-asserts the marker is erased. It **never sleeps or relies on wall-clock timing**. |
| `UI46_D7_ExplicitResumeSetsMarker` | 46-I14, 46-D7.2 (N1) | new (the marker is incremented by `resume_after_attach` itself, so the explicit already-attached `resume_from_history` path is covered; fails on the rev-3 tree) |
| `UI46_D7_UnknownSessionEvictedClearsMarker` | 46-I14, 46-F32 (N2) | new (drives the resume `UnknownSession` terminal for an unmodeled workspace and asserts `release_resume` erased the refcount; Rev 6 / NEW-1 moved the release out of `recover_unknown_session` into this terminal) |
| `UI46_D7_NonTransportResumeFailureFirstKeystrokeRecovers` | 46-I14, 46-F32 (N9) | new (a non-transport resume failure decrements the marker and leaves no focus; the first keystroke's 23 §1.1 create path recovers) |
| `UI46_D7_OverlappingResumesKeepGateClosed` | 46-I14, 46-F39 (O-M6) | new (two overlapping `resume_after_attach` calls for one workspace; the first terminal reply leaves the count ≥ 1, so a `session.list` reply creates no stray session; fails on the rev-4 set) |
| `UI46_D7_DisconnectDrainsPendingResume` | 46-I14, 46-F38 (O-H3) | new (a `session.resume` queued behind a failing request; `harness->drop_connection(workspace)` forces the disconnect; the test asserts the resume reply is delivered and the refcount is erased; fails on the rev-4 tree) |
| `UI46_D7_NonResumeUnknownSessionDoesNotDecrement` | 46-I14, 46-F32 (Rev 6 / NEW-1) | new (with no resume in flight, an `UnknownSession` reply to `prompt()` **and** to `agent.select` drives `recover_unknown_session`; the test asserts `harness->resume_in_flight().count(workspace) == 0` afterward and that the D7.2 focus/create branch still runs on a later `session.list` reply — fails on a tree that decrements inside `recover_unknown_session`, where the count wraps to `SIZE_MAX`) |
| `UI46_D7_LiveSessionStillFocused` | 46-I13 | pin |
| `UI46_D7_CreateFailureNotice` | 46-F9 | new |
| `UI46_D8_ToolDeadlineConfig` | 46-I15 | new |
| `UI46_D8_ShellDeadlineKillsProcess` | 46-I15, 46-F10 | re-pointed at `tools.timeout_ms` |
| `UI46_D8_PtyExpiredDeadlineReturnsImmediately` | 46-I29 | new (asserts `PtyRead.timed_out` from an expired deadline with `wait_ms == 0`, proving the expiry is not confused with a poll) |
| `UI46_D8_PtyDisabledDeadlineKeepsWaitMs` | 46-I29 (disabled clause), N3 | new (a `nullopt` deadline keeps today's `wait_ms` semantics: `wait_ms == 0` polls and `timed_out` stays `false`) |
| `UI46_D8_McpExpiredDeadlineTimesOut` | 46-I29 | new |
| `UI46_D8_McpDeadlineClamped` | 46-I15, 46-F11 | new |
| `UI46_D8_McpConfiguredTimeoutIsCeiling` | 46-I15, N4 | new (an active deadline with `remaining > call_timeout` still uses `call_timeout`, not the larger remaining) |
| `UI46_D8_GitPrecheckTimesOut` | 46-I31, 46-F12 | new |
| `UI46_D8_DeadlineExpiredPreDispatch` | 46-D8.10 | new (constructs a past deadline directly; G2-L17) |
| `UI46_D8_GitPostcheckTimesOut` | 46-I31 (post-call) | new (G2-L6) |
| `UI46_D8_ProcessExpiredDeadlineNotStarted` | 46-I29 (process clause) | new (G2-L7) |
| `UI46_D8_DisabledDeadlineNeverTimesOut` | 46-I30, 46-F28 | new |
| `UI46_D8_TimeoutResultIsErrorAndTurnContinues` | 46-I16 | re-pointed |
| `UI46_D8_ModelTimeoutNeverExceedsDeadline` | 46-D8.9 | new |
| `UI46_D8_ZeroDisables` | 46-D8.1 | new |
| `UI46_D8_ParallelWorkerJoinedAfterKill` | 46-I17 | pin |
| `UI46_D9_SpinnerOnWhileTurnActive` | 46-I18 | new |
| `UI46_D9_SpinnerOffOnIdle` | 46-I18 | new |
| `UI46_D9_SpinnerOffOnWaitingForPermission` | 46-I18 | new |
| `UI46_D9_SpinnerAdvancesOnTick` | 46-I19 | new |
| `UI46_D9_SpinnerBeforeModeSegment` | 46-I18 | new |
| `UI46_D9_SpinnerSharesFrameTable` | 46-I18 | new |
| `UI46_D9_SpinnerOnlyActiveSession` | 46-I18/I19, 46-F31 | new |
| `UI46_D10_SlashEscThenTypeRebuildsList` | 46-I20, 46-F14 | new |
| `UI46_D11_HistoryHydratedOnReplay` | 46-I21 | new |
| `UI46_D11_FirstPromptOfNewSessionRecallable` | 46-I21 | new |
| `UI46_D11_NoDuplicateForLivePrompt` | 46-I21, 46-F15 | new |
| `UI46_D11_HistoryOrderMatchesLog` | 46-I21 | new |
| `UI46_D11_CommandsNotHydrated` | 46-D11.4 | new |
| `UI46_D11_PluginMessageNotHydrated` | 46-I21, 46-F30 | new |
| `UI46_D12_ImportApiKey` | 46-I22 | new |
| `UI46_D12_ApiKeyReachesProvider` | 46-I22/I24, 46-D12.3 | new |
| `UI46_D12_ApiKeyNeverLogged` | 46-I24/I36 | new |
| `UI46_D12_ImportSkipPermissions` | 46-I22 | new |
| `UI46_D12_ImportPermissionRulesObject` | 46-I22 | new |
| `UI46_D12_FlatPermissionArraySkipped` | 46-D12.6 | new |
| `UI46_D12_OpenAiCompatTypeAccepted` | 46-D12.7 | new |
| `UI46_D12_BedrockTypeSkipped` | 46-D12.7, 46-F31 | new |
| `UI46_D12_ImportMaxTokens` | 46-I22 | new |
| `UI46_D12_MaxTokensTargetsCallConfig` | 46-D12.1/4 (F-38; O-M3: routed via `to_agent_config` → `GenerationParameters::max_output_tokens` → `agent_loop.cpp:539`) | new |
| `UI46_D12_ModelFieldsRetained` | 46-D12.7 | pin |
| `UI46_D12_NoReimportWhenGlobalExists` | 46-I23, 46-F20 | pin |
| `UI46_D12_GlobalLayerOverridable` | 46-D12.8 | new |
| `UI46_D12_NoProfileNoImport` | 46-D12.3, 46-F17 | new (G2-L1) |
| `UI46_D12_ApiKeyNeverRendered` | 46-I24/I36 (UI) | new (G2-L13) |
| `UI46_D12_ApiKeyEnvOptional` | 46-D12.2 (G2-M11) | new |
| `UI46_D12_WorkspaceLayerApiKeyRejected` | 46-D12.2 (G2-M13) | new |
| `UI46_D12_ScaffoldIs0600` | 46-D12.10 (G2-M12) | new |
| `UI46_D12_RedactsJsonApiKey` | 46-I36 (G2-M14; token `[REDACTED]`, O-L2) | new |
| `UI46_D12_LogSinkRedacts` | 46-I36 (O-M7: an ordinary `category_logger(...).info` line is redacted by `SpdlogLogger::log`) | new |
| `UI46_D12_InvalidDecisionDropped` | 46-D12.6 | new |
| `UI46_D12_LocalcodeFixture` | 46-D12.7 (the real shape) | new |
| `UI46_D13_CtrlEOpensEditor` | 46-I25 | new |
| `UI46_D13_EditorResultCopied` | 46-I25 | new |
| `UI46_D13_EditorFailureKeepsDraft` | 46-I25, 46-F18 | new |
| `UI46_D13_EditorExit127KeepsDraft` | 46-I25, 46-F18 (F-50) | new |
| `UI46_D13_EmptyDraft` | 46-D13.4 | new |
| `UI46_D13_ScratchFileUnlinked` | 46-I26 | new |
| `UI46_D13_HintsRefreshedAfterEdit` | 46-D13.7 | new |

### 20.2 Golden TUI render

- `UI46_G1_NoticePopup`: Ctrl+S with a single live workspace renders exactly the
  notice window (message `No other workspaces available` + one `[ OK ]` row, no
  workspace rows, no `(current session hidden)` leaf).
- `UI46_G2_PermissionDialogOpaque`: a permission dialog with a long summary
  renders no transcript characters in the columns to the right of the option
  rows.
- `UI46_G3_ExitConfirmOpaque` / `UI46_G4_SwitcherOpaque`: the same opacity
  assertion for the other two overlays.
- `UI46_G5_QuitRowLiteral`: typing `/quit` renders exactly
  `> /exit(quit) - quit the supervisor`.
- `UI46_G6_StatusSpinner`: a turn-active status line renders the spinner glyph as
  the first cell of the bottom-left group, before `build`, with the exact prefix
  `<glyph>  · build` (46-D9.4).

### 20.3 Integration (FakeLLM / fake daemon / fake store)

- `UI46_I1_DefaultAllowEndToEnd`: a config with `permissions.default = "allow"`
  runs a shell/write/mcp tool with no `permission.request` on the wire.
- `UI46_I2_AlwaysGrantSurvivesDaemonRestart`: a `permission.decide` with
  `Always` writes the grants file; a fresh daemon loads it and does not re-ask.
- `UI46_I3_NoTargetNoticeEndToEnd`: a fake workspace list with only the active
  workspace opens the notice and never the switcher.
- `UI46_I4_ToolDeadlineEndToEnd`: a fake tool that sleeps past the deadline
  yields a Timeout `ToolResult` and the turn continues to the next step.
- `UI46_I5_ImportEndToEnd`: a localcode document with the **real** shape
  (`api_key`, `skip_permissions`, `permission.<tool>[]`, `max_tokens`) produces
  the pinned global config and the runtime starts.
- `UI46_I6_ExpiredDeadlineEndToEnd`: a `shell`/PTY/MCP call with an already-past
  deadline returns a Timeout result promptly (no hang).
- `UI46_I7_ResumeInFlightNoStraySession`: a `--resume` whose `session.list` reply
  is delivered between the resume submit and its reply creates no stray session
  (G2-H1; the `resume_in_flight_` refcount gate).
- `UI46_I8_DaemonGrantStoreInjection`: a daemon start resolves the workspace id
  from the registry, opens the grants file, and the runtime's policy loads it
  before serving; a second daemon start re-loads the same grant (O-H2).
- `UI46_I9_DisconnectDrainsResume`: a fake daemon that drops the connection while
  a `session.list` + `session.resume` batch is in flight replies to both; the
  supervisor's refcount returns to zero and the first keystroke focuses/creates
  (O-H3).

### 20.4 PTY / live (opt-in, `YMH_LIVE_LLM=1`)

- `UI46_P1_QuitHelpRow` (extends `UiSupervisorPty.HelpListAndHistoryRecall`).
- `UI46_P2_EmptySwitcherNotice`: run the real binary in a single-workspace
  checkout, Ctrl+S, assert the notice and that Enter/Esc dismiss it.
- `UI46_P3_CtrlEEditor`: set `EDITOR` to a scripted writer, Ctrl+E, assert the
  composer receives the written text.
- `UI46_P4_SpinnerVisible`: assert a spinner glyph appears in the status line
  while a turn is in flight.
- `UI46_P5_TwoPressEnter`: type `/q`, Enter, assert no exit and draft `/quit `;
  Enter again, assert exit.

---

## 21. Open questions / interpretations

- **Q1 (RESOLVED BY USER, 2026-09-20).** 46-D12.2 imports a literal API key into
  `llm.default.api_key`. The user decided: **remove ymh's "secrets never in
  `Config`" invariant and store `api_key` in the ymh config file, exactly as
  localcode does.** Pinned in 46-D12.2/10/11 and 46-I24/I36; supersedes
  `21:232-233` (46-S6) and `08` §14.1(g)/(t) (46-S7). Mitigations: `0600` global
  config, never logged/rendered/event-logged, redaction. Env (`api_key_env`) is
  retained as the fallback.
- **Q2 (RESOLVED BY USER, 2026-09-20).** Item 6's "another `<Enter>` is required
  to execute it" is now pinned literally: **the first Enter completes a partial
  command, the second executes it** (46-D6). This supersedes the shipped
  one-keypress accept-and-dispatch for partial drafts (46-S12). Exact commands
  still dispatch on the first Enter.
- **Q3 (RESOLVED).** localcode `permission` is an **object** keyed by tool, and
  `match` is a shell-command glob (verified against the user's real
  `~/.localcode/config.json`). The mapping uses
  `permissions.rules[].command` with `tool` from the key (46-D12.6/7). The
  rev-1 flat-array reading (from a synthetic test fixture) is not the real shape.
- **Q4 (recorded).** Item 7's parenthetical "or even an empty one until first
  prompt" is not adopted: 46-D7 keeps 23-D58's eager empty session. Adopting the
  alternative would re-open spec 23's creation-timing contract. Recommendation:
  keep eager create.
- **Q5 (recorded).** Item 3's message is `No other workspaces available` even
  when the only missing target is a non-focused session of the active workspace.
  The predicate (46-D3.2) is stricter than the wording; the wording is the user's
  literal string and is kept.
- **Q6 (recorded).** The spinner reuses `ReasoningSpinnerState::frame`; the
  reasoning header and the status-line spinner therefore advance together. If a
  future change needs independent clocks, a second `ReasoningSpinnerState` is
  required. Recommendation: keep shared.
- **Q7 (factual correction).** The task's hypothesis that `push_history` is never
  called on submit is **false**: it is called at `src/ui/supervisor.cpp:441` and
  `:2361-2368`, and the in-session recall test passes
  (`tests/unit/supervisor_harness_test.cpp:663-680`). Item 11's real gap is the
  missing hydration (46-D11).
- **Q8 (resolved).** Item 10 is already satisfied by 45-D5; the only work is the
  regression test (46-D10). The gate's attack on the "already satisfied" claim
  (F-32) did not land on the core path: only the 25 ms RB-12 modal-tail window
  qualifies it, which 46-I20 now records.
- **Q9 (recorded).** The `permissions.default == "allow"` master switch ignores
  `permissions.shell`/`write`/`read` (46-D1.6). A finer-grained opt-out is
  `permissions.rules`.
- **Q10 (RESOLVED BY AUTHOR, 2026-09-20; corrected in Rev 3 / G2-M1).** The grants
  file records the workspace **`registry.db` id** (`WorkspaceRecord::id`, UUIDv4,
  `src/registry/registry.cpp:44-46`), resolved at load via
  `WorkspaceRegistry::findByCanonicalPath(root)`, and ignores the file on an id
  mismatch or a missing registry row. This is a mitigation, not a cryptographic
  binding; a same-user process can still edit the file. Accepted.
- **Q11 (RESOLVED BY AUTHOR, 2026-09-20).** Whether `permissions.rules` should
  support a `tool`-less rule via a new "operator layer" compared before
  specificity (instead of requiring `tool`). **Decision: require `tool`** (46-D1.2)
  — it is simpler and matches the localcode import, which always has a tool key.
  No open question remains.
- **Q12 (RESOLVED BY AUTHOR, 2026-09-20; from G2-L12).** The literal `api_key`
  has **no** env/CLI override: `apply_env_overrides` has no `YMH_LLM_API_KEY`
  (`src/config/config.cpp:1199-1244`) and the CLI has no `--api-key` flag
  (`src/cli/cli.cpp:110` is `--api-key-env`). **Decision:** do not add one; the
  key is global-only and changed by editing the global config. This keeps a single
  obvious source for the secret and avoids a second precedence chain.
- **Q13 (RESOLVED BY AUTHOR, 2026-09-20; from G2-M12/M13).** `api_key` is
  accepted **only in the global layer**, the scaffold is written `0600`, and the
  loader rejects a literal key from a group/world-readable file. **Decision:** all
  three controls are adopted (belt-and-suspenders); the workspace-layer rejection
  is the primary repo-shipping defence, `0600` is the primary local-read defence.
- **Q14 (RESOLVED BY AUTHOR, 2026-09-20; from Gate 3; the user is offline, so
  these are best-guess decisions with the rationale stated).** Four narrow
  contract choices in the D7/D8 flagship fixes:
  1. **D7 marker insertion (N1).** The marker is inserted at the **top of
     `resume_after_attach`**, not at the `pending_resume_` consume site. Rationale:
     `resume_after_attach` is the single funnel for every resume path (startup
     `pending_resume_`, explicit already-attached `resume_from_history`, and any
     future path), so the invariant is structural rather than a per-call-site
     convention. The increment is a per-workspace map increment (Rev 6 / O-M6),
     so the move is free and structural. This supersedes the Rev-3 consume-site
     pin.
  2. **Marker-release discipline (N2; corrected in Rev 6 / NEW-1).** Release
     through the guarded `release_resume` helper at the **two genuine resume
     terminals only** (`apply_resume_success`, the `!reply.ok` branch) rather than
     at each return, so both sub-exits of the failure branch are covered by one
     call. Rationale: a release-before-any-early-return is the only discipline
     that cannot leak, and the helper's presence/underflow guard makes it safe
     against a future caller. **`recover_unknown_session` is deliberately
     excluded** — it has non-resume callers (`prompt`, `agent.select`) that never
     incremented the refcount, so an unconditional decrement there would wrap a
     zero entry to `SIZE_MAX` and close the D7 gate permanently. The full
     enumeration is recorded in §9.2 for auditability.
  3. **Non-transport resume failure (N9).** The recovery is the existing
     first-keystroke create path (23 §1.1), not a re-issued `refresh_sessions`.
     Rationale: it is the same recovery already used for `session.create` failure
     (46-F9), so it adds no new behavior and no new failure surface; re-issuing a
     list would also be wrong for the transport-failure sub-case (the daemon may be
     gone). The incorrect D7.2 rationale ("a later list reply can still create") is
     deleted.
  4. **PTY/MCP deadline mapping (N3/N4).** PTY's deadline is a **new, separate**
     parameter (disabled = `nullopt`), never folded into `wait_ms`; MCP's clamp
     `caller_ms` is the existing effective timeout (`options.timeout`, else
     `config_.call_timeout`). Rationale: both make the primitive's configured value
     a ceiling and keep the expired signal distinct from the pre-existing `0`
     meanings (`wait_ms == 0` = poll; `options.timeout == 0` = server default). No
     new type is introduced for either.
   No open question remains.
- **Q15 (RESOLVED BY AUTHOR, 2026-09-20; the user is offline, so these are
  best-guess decisions with the rationale stated; from the Oracle pass O-H1…O-L3).**
  Nine Rev-5 contract choices:
  1. **Durable grants are tool + command only; the `path` dimension is dropped
     from D1 rules and D2 grants (O-H1).** Rationale: the shipped loop never
     populates `PermissionRequest::path` (`agent_loop.cpp:662-675`), so a path
     dimension is dead for config rules and silently tool-wide for grants — the
     worst of both. Dropping it makes the stored grant exactly what `remember()`
     can honestly record, and mirrors the real localcode model
     (`permission.bash[].{match,decision}` is a command glob, no paths). The
     item-1 "at or below the repo" boundary is provided by the D1 master switch
     plus `resolve()` confinement of path tools — **not** by grants. The
     alternative (populate `request.path` in the loop) is out of scope: it would
     change spec 07/09's tool/permission seam and is recorded, not adopted.
  2. **Durable grants are literal, not globs (O-M1).** `remember()` sets
     `literal = true`; the grants file records `"match": "literal"` and a
     hand-authored entry may set `"match": "glob"`. Rationale: an `Always` on
     `rm -rf build/*` must not become a durable over-broad grant; literal
     matching is the only semantics that cannot widen, and escaping is not
     available because `glob_match` has no escape character.
  3. **The `GrantStore` is daemon-constructed, daemon-owned, and injected as a
     non-owning pointer; the daemon opens the registry and resolves the id before
     building the runtime (O-H2).** Rationale: this is the only order that
     satisfies the id-check-at-load contract; all moved steps are
     runtime-independent. Concurrency is serialized by the store's own `flock`
     (not `mutex_`, so a slow append cannot block `evaluate()`; O-L1).
  4. **`resume_in_flight_` is a per-workspace refcount released only at the two
     genuine resume terminals, and `SupervisorConnection` replies to every
     pending request on disconnect (O-H3/O-M6; guarded in Rev 6 / NEW-1).**
     Rationale: a set cannot represent overlapping resumes, and the Rev-4 "reply
     always delivered on disconnect" claim was false; the drain makes the
     guarantee structural so the release always runs. The release goes through
     the guarded `release_resume` (no-op when absent or zero), and
     `recover_unknown_session` is excluded because `prompt`/`agent.select` reach
     it without a resume in flight — the Rev-5 pin that decremented there was the
     NEW-1 underflow.
  5. **The master switch is unrestricted command execution, not repo-confined
     (O-M2).** Rationale: no OS confinement exists for `shell`/`git`/`mcp.*`;
     the honest statement is required because a user enabling the switch could
     otherwise be misled. The user's "no asking" choice is honored.
  6. **`llm.default.max_tokens` routes through `to_agent_config` →
     `GenerationParameters::max_output_tokens` (O-M3).** Rationale: that is the
     one shipped conduit to `LlmCallConfig::max_tokens`; the Rev-4 "not
     `max_output_tokens`" phrasing was a category error.
  7. **`PolicyConfigError` is translated to `ConfigError` in the parser
     (O-M4).** Rationale: the policy layer must not depend on the config layer,
     and the CLI only catches `ConfigError` (`cli.cpp:289`).
  8. **The `force_ask` guard is inside `remember()` (O-M5), and log redaction is
     at the logger sink (O-M7), with the `[REDACTED]` token (O-L2).** Rationale:
     invariants must live at the single seam that enforces them, not at a list of
     call sites that a future change can miss.
  9. **The `config.hpp:14-17` comment is rewritten as part of 46-S6 (O-L3).**
     Rationale: a superseded comment left in the header misleads implementers.
   No open question remains.
- **Q16 (RESOLVED BY AUTHOR, 2026-09-21; the user is offline, so these are
  best-guess decisions with the rationale stated; from the Oracle re-review
  NEW-1…NEW-6).** Six Rev-6 contract choices:
  1. **The refcount release moves out of `recover_unknown_session` into the
     resume-failure terminal, and gains a presence guard (NEW-1).** Rationale:
     the helper has two non-resume callers (`prompt` `:1450`, `agent.select`
     `:1907`) that never incremented the refcount; an unguarded decrement there
     wraps a zero entry to `SIZE_MAX` and permanently closes the D7 gate. Moving
     it co-locates release with increment (both in `resume_after_attach`'s
     lifecycle) and the `release_resume` guard makes even a future misuse inert.
     Chosen over the alternative of only adding the guard (which would leave the
     decrement semantically misplaced). A dedicated test covers both non-resume
     paths. This supersedes Q14.2's "erase at the top of the three terminal
     functions".
  2. **An explicit config deny vetoes a durable `Always` grant (NEW-2).**
     Rationale: without it, the D1.6/F1 recovery ("add a `permissions.rules`
     deny") cannot revoke a grant, because `Layer::LocalGrant` (5) outranks
     `Layer::Project` (2) in `rule_greater` — only deleting the grants file
     worked. The veto is an explicit post-pass in `evaluate()`, not a comparator
     change (an asymmetric comparator would break the ordering). It is scoped to
     **non-grant** denies vs **`LocalGrant`** allows, so config-vs-config and
     grant-vs-grant precedence are untouched; `Session` grants are excluded
     (the human's explicit in-session decision). Consistent with localcode's
     `decision: "deny"` (imported as a `Layer::Project` rule).
  3. **The daemon order guards the `findByCanonicalPath` optional (NEW-4).**
     Rationale: `ensureWorkspaceRegistered` returns `true` via its `findById`
     branch without a row at `canonical_root_`, so `record` can be `nullopt`;
     the guard returns `HostExitCode::RegistryFailed` before the store opens.
  4. **A grant `path` key is a fail-safe skip, not a `ConfigError` (NEW-3).**
     Rationale: the grants file is not the layered config; every malformed-grants
     path is already non-fatal (46-I7/I33), so I1 must distinguish config rules
     (fatal) from grants (skipped).
  5. **I36 is restated to the shape-based enforceable form (NEW-6).** Rationale:
     the redactor cannot know an arbitrary configured value; the honest invariant
     names the pinned shapes and states that the primary control is never
     stringifying the key.
  6. **The tool-wide durable path grant is an explicitly accepted limitation.**
     A single `Always` on `write_file src/a.txt` auto-allows every `write_file`
     in the workspace, including `.ymh/` and git-hook paths inside the root; the
     same holds for `edit_file`/`read_file`/`grep`/`glob`. This is the broadest
     user-visible consequence of dropping the path dimension (O-H1) and is
     accepted, documented in §4.2 item 13 and §18 F35, and revocable by deleting
     the grants file or (now) by a config deny. A finer per-path grant requires
     populating `PermissionRequest::path` in the loop, which is out of scope
     (§21 Q15.1). No open question remains.

---

## 22. Revision log

- **Rev 1 (initial draft).** Covers all thirteen items. Decisions 46-D1 … 46-D13.
  Current-state claims verified against the shipped tree (HEAD `5a22e43cc`):
  `src/ui/command_registry.cpp:52-97`, `:272-292`;
  `src/ui/supervisor.cpp:140`, `:431-455`, `:467-479`, `:1042-1114`,
  `:1253-1263`, `:1286-1354`, `:1508-1515`, `:1578-1586`, `:2043-2076`,
  `:2096-2148`, `:2194-2240`, `:2285-2326`, `:2328-2474`, `:2600-2718`;
  `src/ui/ui_model.cpp:192-226`, `:324-332`, `:350-353`, `:659-665`, `:955-960`,
  `:1000-1054`, `:1271-1285`; `src/ui/ui_render.cpp:214-243`, `:361-383`,
  `:394-399`, `:577-597`, `:620-663`, `:666-703`, `:788-803`, `:830-848`,
  `:1096-1125`, `:1197-1215`; `src/ui/ui_event_adapter.cpp:54-80`, `:407-413`;
  `src/ui/session_export.cpp:294-342`; `src/policy/permission_policy.cpp:58-105`,
  `:188-281`; `src/cli/wiring.cpp:78-104`; `src/config/config.cpp:349-357`,
  `:807-818`, `:1131-1136`, `:1271-1276`, `:1418-1523`; `src/cli/cli.cpp:470-485`,
  `:543-545`, `:729-751`, `:929-983`, `:1164-1194`, `:1214-1248`;
  `src/agent/agent_loop.cpp:534-538`, `:754-783`, `:1220-1255`;
  `src/registry/registry.cpp:855-870`; `src/execution/environment.cpp:60-87`;
  `include/ymh/ui/ui_model.hpp:157-170`, `:240-247`, `:471-489`;
  `include/ymh/ui/ui_event.hpp:45-51`; `include/ymh/core/task.hpp:28-49`;
  `include/ymh/tools/tool_context.hpp:22-66`; `include/ymh/tools/tool_registry.hpp:92-93`;
  `include/ymh/execution/config.hpp:14-38`; `include/ymh/execution/process.hpp:26-42`;
  `include/ymh/execution/errors.hpp:15-24`; `include/ymh/session/events.hpp:140-161`;
  `include/ymh/llm/llm_request.hpp:44`; `include/ymh/config/config.hpp:81-115`,
  `:170-176`, `:298-308`.
  Not yet reviewed. **Verification status: GATE FAIL (9 HIGH / 32 MEDIUM / 15 LOW)
  — see `/tmp/opencode/spec46-gate1.md`.**
- **Rev 2.** Responds to the Gate-1 report and applies two user
  decisions. Changes:
  1. **D8 deadline semantics rewritten (F-01/F-02/F-03/F-04).** The disabled
     deadline is `std::optional` (not a default-constructed `time_point`);
     `remaining()` is total (`max()` when disabled); the clamp is a function of an
     **expired** deadline (`nullopt` disabled / `0ms` expired / `>0` budget) and
     is applied explicitly to process, PTY, and MCP; `git` moves to the
     cooperative bucket; `ToolConfig` is exposed to the loop via
     `ExecutionEnvironment::toolConfig()`. The error path is re-cited to
     `ToolRegistry` (F-26). Owning specs are `07 §5.5` + `21 §3.3/§7.5` (F-27/28).
  2. **D2 persistence seam moved into `remember()` (F-05).** All three call sites
     persist; `remember()` returns the grant; the store is injected; the file
     gains `effect`, a `schema` marker, the workspace identity, atomic write,
     `flock`, an exported JSONC parser, and a content-derived id (F-14…F-19,
     F-56).
  3. **D1 precedence fixed (F-08).** Config rules are `Layer::Project` and a
     `command` rule must set `tool` (the `path` dimension was dropped in Rev 5 /
     O-H1); the validators are exported (F-10);
     the register names `09` decision (f) and `21 §3.3/§7.5` (F-11/F-12).
  4. **D6 rewritten to the user's two-press Enter (Q2).** 46-S12 added; 46-I32.
  5. **D12 expanded to the real localcode shape (Q3) and the user's API-key
     decision (Q1).** `LlmSettings::api_key`/`max_tokens` and
     `LLMProviderConfig::api_key` are pinned with precedence, redaction, and the
     `08`/`28`/`25` supersessions (46-S6/S7/S8/S13). Five D12 citations fixed
     (F-37); `max_tokens` targets `LlmCallConfig::max_tokens` (F-38).
  6. **D7 race closed (F-07).** The `session.list` reply branch is gated on
     `pending_resume_`; `stored` is removed (F-24).
  7. **Register completeness (F-09/F-11/F-20/F-21/F-41).** 45-D8.2, 09 decision
     (f), 10 `UiMode`, 22 §3, 25-D14/D15, 08 (g)/(t), 21:232-233, 28
     `to_provider_config` are named.
  8. **Invariant audit (gate §D).** I4/I6/I14/I17/I20/I23 are relabelled
     retained/regression pins and excluded from the gate; I2/I15/I18/I19/I20/I21/
     I24 are corrected; I29–I36 added.
  9. **Test plan (gate §E).** Vacuous tests are marked and re-pointed; the missing
     positive tests (git/PTY/MCP expired, disabled, loop/gate persistence, api_key
     reaches provider, localcode fixture, two-press Enter) are added.
  10. **Citations re-verified.** Every retained `file:line` was re-opened in Rev 2;
     the >25 wrong/stale citations in Rev 1 are corrected (F-26/F-35/F-37/F-42/
     F-43/F-44/F-45/F-46/F-47/F-48/F-50/F-53) or rebutted (F-54). `F-51`/`F-52`
     include fixes are applied in §16.
  11. **§23 gate-response table added**, mapping F-01…F-56 to resolved/rebutted
     with the section that carries the fix.
  Not yet re-gated at the time of writing. **Verification status after Rev 2:
  GATE FAIL (2 HIGH / 19 MEDIUM / 18 LOW) — see
  `/tmp/opencode/spec46-gate2.md`.**
- **Rev 3.** Responds to the Gate-2 report and closes the three
  gate-1 findings the gate-2 table marked "not closed" (F-07/F-16/F-55) plus the
  ten residual defects. Changes:
  1. **G2-H1 (F-07) — the rev-2 `pending_resume_` gate was always open.** The
     shipped code erases the pending entry synchronously before the resume is
     submitted, so the `session.list` reply always sees it empty. Replaced with a
      `resume_in_flight_` set inserted before the erase and cleared in
      `apply_resume_success` / the failure branch / `recover_unknown_session`; the
      reply branch is gated on it. §9.2, §17 I14, §18 F26, §20. *(The insertion
      site was superseded by Rev 4 / N1: the marker is now inserted inside
      `resume_after_attach`. The set was superseded by Rev 5 / O-M6: it is now a
      per-workspace refcount, with a disconnect drain in `SupervisorConnection`.)*
  2. **G2-H2 — non-existent `McpErrorCode::Timeout`.** Corrected to
     `McpErrorCode::CallTimeout` (`include/ymh/mcp/mcp_types.hpp:87`). Then
     **swept every pinned type/enum/function/field name** in §16 and the
     D-decisions against the headers; the other defects found are the citations
     in items 3–15 below.
  3. **G2-M11…M15 — the literal-`api_key` mitigations made enforceable.**
     `ProviderRegistry::create` skips the `api_key_env` name check when `api_key`
     is set (`provider_registry.cpp:68-70`); `write_default_config` writes `0600`
     and the loader rejects a group/world-readable key file
     (`config.cpp:1044`); `api_key` is global-only (a workspace-layer `api_key` is
     a `ConfigError`); `redact_secrets` gains the JSON `"api_key": "…"` form and
     the `ymh config` dump claim is dropped (`cli.cpp:193-203`); `08` §6.4 is
     named as superseded.
  4. **G2-M5 — `PermissionGate::decide` gains the `!force_ask` guard**
     (`permission_policy.cpp:413`), so all three `remember()` sites agree; a test
     is added.
  5. **G2-M1 (F-16) — workspace identity is the registry id, not the path.**
     `registry.cpp:44-46`; the same-path/different-repo swap now fires. Q10 and
     the test updated.
  6. **G2-M2/M3/M4/M8/M9/M10/M16/M18/M19 — citation, signature, sketch and
     parser fixes.** `parse_jsonc_document` returns the parsed document; `fsync`
     is a new requirement (the importer has none); `21` §7.5 is not `tools` (new
     §7.12); PTY gains `timed_out`; `McpCallOptions` is cited in the header; the
     supervisor sketch is class members, not an anon namespace; D12.3/D12.4 are
     un-swapped; multi-match Enter uses the highlighted candidate;
     `validate_rule` is a signature change.
  7. **G2-M6/M7 — I16 split by primitive; I30 qualified** (disabling the tool
     deadline does not disable the model/MCP timeouts).
  8. **G2-M17 — `25-D9`/`UX-U15` named as superseded**; the shipped
     `UX_U15_EnterAcceptsHighlightAndDispatches` is a required rewrite.
  9. **G2-L1…L18 — the low-severity sweep:** F1/F5/F17/F24/F25 tests added;
     the error field (not output) named; the marker is not a trust boundary;
     `remaining()` casts to `milliseconds`; the process formula routes through
     `clamp_timeout`; the pre-dispatch test constructs a past deadline; the
     `/quit` vs `/q` asymmetry documented; §23 citations corrected.
  10. **§23 rewritten to cover both reports** (gate-1 IDs F-01…F-56 and gate-2 IDs
      G2-H1/G2-H2/G2-M1…G2-M19/G2-L1…G2-L18).
   **Verification status: GATE FAIL (2 HIGH / 19 MEDIUM / 18 LOW) — see
   `/tmp/opencode/spec46-gate2.md`; superseded by Rev 3.**
- **Rev 4.** Responds to the Gate-3 report
  (`/tmp/opencode/spec46-gate3.md`: GATE FAIL 0 HIGH / 4 MEDIUM / 5 LOW). All 39
  gate-2 findings were independently confirmed closed; Rev 4 fixes the four
  residual MEDIUMs in the two flagship fixes plus the five LOWs. Changes:
  1. **N1 — the marker now covers the explicit already-attached resume path.**
     The `resume_in_flight_` insertion moved from the `pending_resume_` consume
     site to the **top of `resume_after_attach`** (`src/ui/supervisor.cpp:1042`),
     the single funnel for `--resume`, the consume site (`:1275`), and
     `resume_from_history`'s already-attached branch (`:1036`). §9.1/§9.2, I14,
     F26; tests `UI46_D7_ExplicitResumeSetsMarker` /
     `UI46_D7_ResumeInFlightBeatsListReply`.
  2. **N2 — no marker-clear exit can leak.** Every clear exit is enumerated and
     the erase is pinned unconditionally **at the top** of `apply_resume_success`,
     the `!reply.ok` branch, and `recover_unknown_session` (before its
     workspace-gone early return at `:1124-1127`). §9.1/§9.2, I14, F32; test
     `UI46_D7_UnknownSessionEvictedClearsMarker`.
  3. **N3 — the PTY trichotomy is complete and cannot collide with `0`.**
     `PtySession::read`/`wait` (corrected from `PtyService`) gain a **separate**
     `std::optional<milliseconds> deadline`; `wait_ms`/`timeout` is unchanged;
     disabled is `nullopt` (today's semantics, `timed_out == false`); expired is
     `0ms` (immediate `timed_out`, never routed through `wait_ms`); `timed_out` is
     derived only from the deadline parameter. §10.2.3, §16, I29; tests
     `UI46_D8_PtyExpiredDeadlineReturnsImmediately` /
     `UI46_D8_PtyDisabledDeadlineKeepsWaitMs`.
  4. **N4 — the MCP clamp's `caller_ms` is pinned** to the existing effective
     timeout `(options.timeout.count() > 0 ? options.timeout : config_.call_timeout)`,
     so the configured server `call_timeout` (default 60 s) is a ceiling. §10.2.3
     item 9, §16, I15; test `UI46_D8_McpConfiguredTimeoutIsCeiling`.
  5. **N5/N6 — one test name and a deterministic injection.** The interleaving
     test is canonically `UI46_D7_ResumeInFlightBeatsListReply`; §20.1 pins the
     canned `install_session_list_reply` + `seed_resume_in_flight` +
     `on_link_state` + `drain_actions` sequence and forbids sleeping. §16 adds the
     additive `submit_session_list` / `install_session_list_reply` /
     `seed_resume_in_flight` / `resume_in_flight()` seams (mirroring
     `submit_agent`/`install_agent_replies`/`seed_ensure_in_flight`).
  6. **N7 — I6 is gated for its new clause.** The `force_ask` gate-guard clause is
     new behavior and is marked `Y`; the `Once`/`Session` clauses stay pins. §17 I6.
  7. **N8 — the workspace-layer `api_key` rejection is shown in §16.**
     `apply_llm(..., bool global_layer)` is pinned, mirroring `apply_document`'s
     existing parameter and its `session` guard (`config.cpp:840-841`, `:898-899`).
     §14.2 item 2, §16.
  8. **N9 — the non-transport resume-failure recovery is stated.** The recovery is
     the first-keystroke create path (23 §1.1); the D7.2 rationale that assumed a
     later `session.list` is deleted; 46-F32 records both N2 and N9. §9.3, §18.
  9. **§23.3 added** (gate-3 IDs N1–N4 + N5–N9) and the §22/§23 headers updated;
     `DESIGN_STATUS.md` row 46 updated to `rev 4; gate1 …; gate2 …; gate3 FAIL
     0H/4M/5L; awaiting Oracle`.
  10. **Citations re-verified.** Every citation touched by Rev 4 was re-opened
      against the shipped tree and every named symbol re-grepped; the only symbol
      correction is `PtyService` → `PtySession` for the deadline methods (N3).
      The new §16 symbols (`apply_llm` layer parameter, `submit_session_list`,
      `install_session_list_reply`, `seed_resume_in_flight`, `resume_in_flight()`)
      are explicitly marked **new** and mirror existing patterns.
  **Verification status: GATE FAIL (0 HIGH / 4 MEDIUM / 5 LOW) — see
  `/tmp/opencode/spec46-gate3.md`; superseded by Rev 5.**
- **Rev 5 (this revision).** Responds to the independent Oracle design pass
  (`/tmp/opencode/spec46-oracle.md`: DO NOT APPROVE — 3 HIGH / 7 MEDIUM / 3 LOW).
  The Oracle confirmed D8's deadline design sound and D3/D4/D5/D6/D9/D10/D11/D13
  and D12's localcode mapping faithful; Rev 5 leaves those untouched and fixes
  the three structural defects plus the seven MEDIUMs and three LOWs. Changes:
  1. **O-H1 — path grants are unsound; rescoped to tool + command.** The shipped
     loop never populates `PermissionRequest::path` (`agent_loop.cpp:662-675`),
     so `path_pattern()` is always `""` (`permission_policy.cpp:117-125`) and a
     path grant silently degrades to tool-wide while a path rule is dead
     (`:226-234`). 46-D2's durable grants and 46-D1's config rules now carry
     **`tool` + `command` only**; a `path` key is a `ConfigError`. The item-2
     final answer is stated explicitly at the top of §4.2. §3.2.2, §4.2.1/4,
     §16 (`PermissionRuleSettings`, `PolicyRule`), I1/I5/I33, F21/F35.
  2. **O-H2 — the `GrantStore` seam and the daemon order are pinned.**
     `WorkspaceRuntimeOptions` gains `GrantStore* grant_store`; the daemon
     constructs and owns the store (member before `runtime_`), opens the registry
     and resolves `WorkspaceRecord::id` **before** `WorkspaceRuntime::create`,
     and the policy loads via the constructor. §4.2.3, §16, I5/I33, F37, §19.
  3. **O-H3 — the resume marker can no longer leak.** `SupervisorConnection`
     replies to every pending request on disconnect
     (`process_requests`/`handle_disconnect`), and the Rev-4 "always delivers a
     reply on disconnect" claim is corrected. §9.2, I14, F38, §16.
  4. **O-M1 — grants are literal.** `PolicyRule::literal`; `remember()` sets it;
     the grants file records `"match": "literal"`. §4.2.1/4, §16, F36.
  5. **O-M2 — the confinement claim is corrected.** The master switch grants
     unrestricted command execution for `shell`/`git`/`mcp.*`; "inside the
     workspace" is scoped to path tools. §3.2.4, I2/I4, F34.
  6. **O-M3 — the `max_tokens` route is pinned** via `to_agent_config` →
     `GenerationParameters::max_output_tokens` → `agent_loop.cpp:539`. §14.2.1/4,
     §16.
  7. **O-M4 — `PolicyConfigError`→`ConfigError` at the parser.** §3.2.2, I1, F33.
  8. **O-M5 — the `force_ask` guard moves into `remember()`.** §4.2.6, I6.
  9. **O-M6 — the marker becomes a per-workspace refcount.** §9.2, I14, F39.
  10. **O-M7/O-L2 — redaction at the sink with the `[REDACTED]` token.** §14.2.11,
      I36, §16.
  11. **O-L1 — the store append runs outside `mutex_`.** §4.2.4.
  12. **O-L3 — the `config.hpp:14-17` comment is rewritten in 46-S6.** §1.3.1.
  13. **§23.4 added** (Oracle IDs O-H1…O-L3); `DESIGN_STATUS.md` row 46 updated to
      `rev 5; gate1 FAIL 9H/32M/15L; gate2 FAIL 2H/19M/18L; gate3 FAIL 0H/4M/5L;
      Oracle DO NOT APPROVE 3H/7M/3L; awaiting Oracle re-review`.
  14. **Citations re-verified.** Every citation in every touched section was
      re-opened and every named symbol re-grepped against the shipped tree:
      `PermissionRequest` construction, `path_pattern`/`command_pattern`,
      `remember`, `glob_match`/`path_glob_match`, `WorkspaceRuntimeOptions`/
      `workspace_host.cpp:420-511`, `SupervisorConnection::process_requests`/
      `handle_disconnect`, `agent_loop.cpp:539`, `to_agent_config`,
      `LlmCallConfig::max_tokens`, `GenerationParameters::max_output_tokens`,
      `openai_adapter.cpp:739-740`, `SpdlogLogger::log`/`redact_log_text`,
      `kRedacted`, `apply_permissions`/`validate_rule`, `cli.cpp:289`,
      `config.hpp:14-17`. The new §16 symbols (`GrantStore* grant_store`,
      `PolicyRule::literal`, `to_agent_config` route, `SpdlogLogger::log`
      redaction, the `SupervisorConnection` drain, the refcount map) are marked
      **new**.
  **Verification status: awaiting Oracle design re-review.**
- **Rev 6 (this revision).** Responds to the Oracle re-review
  (`/tmp/opencode/spec46-oracle-rereview.md`: DO NOT APPROVE — 1 HIGH / 1 MEDIUM
  / 4 LOW). The Oracle re-derived all 13 O-findings, confirmed 12 genuinely
  closed (the three structural HIGHs O-H1/O-H2/O-H3 correct and coherent, and the
  rescope not over-correcting into a glob-escape hole), and confirmed D3–D6,
  D8–D13 and the localcode mapping regression-free. Rev 6 is therefore surgical:
  it touches only the D7 marker discipline, the D2 grant precedence, and four
  citations/invariants, and leaves the approved areas untouched. Changes:
  1. **NEW-1 (HIGH) — the refcount release is removed from
     `recover_unknown_session` and moved to the resume-failure terminal, through
     a guarded `release_resume`.** `recover_unknown_session` has two non-resume
     production callers (`prompt` `src/ui/supervisor.cpp:1450`, `agent.select`
     `:1907`); the Rev-5 unconditional decrement there would wrap a zero entry to
     `SIZE_MAX` and permanently close the D7.2 gate (`:1337-1350`). The release
     now happens in the `!reply.ok` lambda (`:1053-1059`) before the
     `unknown`/`surface_notice` split, and in `apply_resume_success` (`:1090`),
     both via `release_resume` (decrement iff present and `> 0`, erase at 0,
     no-op otherwise). §9.1/9.2/9.3, §16 (marker + new `release_resume`), I14,
     F32, §20.1, §21 Q14.2/Q15.4/Q16.1.
  2. **NEW-2 (MEDIUM) — an explicit config deny vetoes a durable `Always`
     grant.** `rule_greater` (`src/policy/permission_policy.cpp:68-79`) ranks
     specificity → layer → effect, and `Layer::LocalGrant` (5) outranks
     `Layer::Project` (2) (`include/ymh/policy/permission_policy.hpp:83-91`), so
     the D1.6/F1 recovery was inert against a grant. 46-D2.13 pins a post-pass
     veto in `evaluate()`: a matching non-grant `Deny` rule overrides a matching
     `Allow` `LocalGrant` regardless of specificity; `rule_greater` and
     config-vs-config precedence are unchanged; `Session` grants are excluded.
     §3.2.6/3.3, §4.2.9/13, §16, I37, F1/F40, §20.1, §21 Q16.2.
  3. **NEW-3 (LOW) — I1 no longer calls a grants-file `path` key a
     `ConfigError`.** Config-rule `path` keys stay fatal; grants-file `path` keys
     cause the fail-safe file skip (46-I7/I33). §17 I1.
  4. **NEW-4 (LOW) — the pinned daemon order guards the
     `findByCanonicalPath` `optional`.** `ensureWorkspaceRegistered` can return
     `true` via its `findById` branch without a row at `canonical_root_`; pin
     `if (!record) return HostExitCode::RegistryFailed;` before the store opens.
     §4.2.3, §16, F37.
  5. **NEW-5 (LOW) — the `kRedacted` citation is corrected to
     `src/llm/redaction.cpp:10`** (it was `:9`, a blank line) in D12.11, I36, the
     §23.4 O-L2 row and the §23.4 symbol sweep.
  6. **NEW-6 (LOW) — I36 is restated to the shape-based enforceable form.** The
     redactor matches pinned shapes and does not know the configured value; the
     invariant now says so and keeps "never stringify the key" as the primary
     control. §17 I36, §21 Q16.5.
  7. **Accepted limitation recorded.** The tool-wide durable path grant (one
     `Always` on `write_file src/a.txt` auto-allows every `write_file`, including
     `.ymh/` and git-hook paths in the root) is now an explicit, accepted,
     documented limitation in §4.2.13, F35 and §21 Q16.6.
  8. **§23.5 added** (Oracle re-review IDs NEW-1…NEW-6) and the §23 intro/heading
     extended; `DESIGN_STATUS.md` row 46 updated to
     `rev 6; gate1 FAIL 9H/32M/15L; gate2 FAIL 2H/19M/18L; gate3 FAIL 0H/4M/5L;
     Oracle rev4 DO NOT APPROVE 3H/7M/3L; Oracle rev5 DO NOT APPROVE 1H/1M/4L;
     awaiting final Oracle re-review`.
  9. **Citations re-verified.** Every citation in every touched section was
     re-opened and every named symbol re-grepped against the shipped tree:
     `recover_unknown_session` (`:1121`) and its callers (`:1055`, `:1450`,
     `:1907`), `resume_after_attach`/`apply_resume_success` (`:1042`/`:1090`), the
     D7.2 gate (`:1337-1350`), `rule_greater` (`permission_policy.cpp:68-79`),
     `evaluate`/`remember` (`:204-281`), `Layer`
     (`permission_policy.hpp:83-91`), `findByCanonicalPath`
     (`registry.hpp:218-219`), `ensureWorkspaceRegistered`
     (`workspace_host.cpp:420-440`), `kRedacted`/`redact_secrets`
     (`redaction.cpp:10`/`:52-98`), `SpdlogLogger::log` (`logging.cpp:42-46`).
     The new Rev-6 symbol (`release_resume`) is marked **new** in §16.
  **Verification status: awaiting final Oracle design re-review.**

---

## 23. Gate response (Gate 1 → Rev 2; Gate 2 → Rev 3; Gate 3 → Rev 4; Oracle → Rev 5; Oracle re-review → Rev 6)

Every finding in `/tmp/opencode/spec46-gate1.md` (F-01…F-56),
`/tmp/opencode/spec46-gate2.md` (G2-H1/G2-H2, G2-M1…G2-M19, G2-L1…G2-L18),
`/tmp/opencode/spec46-gate3.md` (N1…N4, N5…N9),
`/tmp/opencode/spec46-oracle.md` (O-H1…O-H3, O-M1…O-M7, O-L1…O-L3) and
`/tmp/opencode/spec46-oracle-rereview.md` (NEW-1…NEW-6) is resolved
or rebutted. "Where" names the current (Rev-6) section. Severity is the
report's. Rows marked *(Rev 3)*/*(Rev 4)*/*(Rev 5)*/*(Rev 6)* were corrected in
that revision.

### 23.1 Gate-1 findings

| Finding | Sev | Disposition | Where |
|---|---|---|---|
| F-01 | HIGH | **Resolved.** Clamp is a function of an expired deadline (`nullopt` disabled / `0ms` expired / `>0` budget); applied explicitly to process, PTY, MCP; pre-dispatch guard retained as a narrow race-reducer; F-tag 46-F27. | §10.2.3 (46-D8.6/7), §17 I29 |
| F-02 | HIGH | **Resolved.** Deadline is `std::optional<steady_clock::time_point>`; `remaining()` is total (`max()` when disabled, `duration_cast`-clamped at 0 otherwise, G2-L9); `has_deadline()`/`expired()` pinned; default-constructed `time_point` is explicitly not the sentinel. | §10.2.2 (46-D8.3), §16, §17 I30 |
| F-03 | HIGH | **Resolved.** `ExecutionEnvironment::toolConfig()` accessor added; `LocalEnvironment` stores `ToolConfig`; the loop reads `tool_timeout` there. | §10.2.3 (46-D8.5), §16, §17 I34 |
| F-04 | HIGH | **Resolved.** `git` moved to the cooperative bucket; pre/post `expired()` checks; the universal "all blocking tools route through process/PTY/MCP" claim deleted. | §10.2.3 (46-D8.8), §17 I31/I35 |
| F-05 | HIGH | **Resolved.** Persistence moved into `RulePermissionPolicy::remember()` (injected `GrantStore*`); `remember()` returns the grant; all three call sites persist; a loop/gate restart test added. | §4.2 (46-D2.4), §20.1 |
| F-06 | HIGH | **Resolved (user decision); hardened in Rev 3.** `LlmSettings::api_key` + `LLMProviderConfig::api_key` + wiring precedence + adapter branch pinned; 08 (g)/(t)/§6.4 and 28 amended; Q1 resolved with a threat model; global-only, `0600` scaffold, `api_key_env` optional, JSON-form redaction (G2-M11…M15). | §14.2 (46-D12.2/3/10/11), §21 Q1/Q12/Q13, 46-S6/S7/S13 |
| F-07 | HIGH | **Resolved in Rev 3 (G2-H1); refcounted in Rev 5 (O-H3/O-M6).** The rev-2 `pending_resume_` gate was always open (the entry is erased synchronously before the resume is submitted). Replaced by a `resume_in_flight_` marker; the reply branch is gated on it and a true interleaving test is added. Rev 5 makes it a per-workspace refcount and adds the `SupervisorConnection` disconnect drain. | §9.2 (46-D7.2), §17 I14, §18 F26/F38/F39, §20.1 |
| F-08 | HIGH | **Resolved; narrowed in Rev 5 (O-H1).** Config rules are `Layer::Project`; a `command` rule must set `tool` (else `ConfigError`); the `path` dimension is removed entirely. A command-only-deny test added. | §3.2 (46-D1.2), §17 I1, §20.1 |
| F-09 | HIGH | **Resolved.** 45-D8.2 (`CommandHint` arity) named in 46-S1 and §2. | §1.3.1 46-S1, §2 |
| F-10 | MED | **Resolved in Rev 3 (G2-M19); narrowed in Rev 5 (O-H1/O-M4).** `validate_rule`/`validate_glob` are exported from `permission_policy.hpp`: the shipped anonymous-namespace `validate_rule(const PolicyRule&)` (`permission_policy.cpp:98`) stays for the in-policy call site, and the exported form is the 2-arg `validate_rule(tool, command)`; it throws `PolicyConfigError`, which the parser translates to `ConfigError`. | §3.2 (46-D1.2), §16 |
| F-11 | MED | **Resolved.** 09 decision (f) `[[permissions.rule]]` named as superseded. | §1.3.1 46-S9, §2, §3.2.9 |
| F-12 | MED | **Resolved.** D1 targets `21 §3.3/§7.5` (live key table); `21:226` §1.3.3 named as *not* the target. | §1.3.1 46-S14, §2, §3.2.9 |
| F-13 | MED | **Resolved.** I2 qualified by the `force_ask` carve-out. | §17 I2, §3.2.5 |
| F-14 | MED | **Resolved.** The grants format carries `effect`; the loader forces `effect = Allow`. | §4.2 (46-D2.1), §17 I33 |
| F-15 | MED | **Resolved in Rev 3 (G2-M3).** Temp + `fsync(temp)` + `rename` + `fsync(parent)` pinned; `fsync` is a **new** requirement (the config importer has none); crash semantics stated; a `SIGKILL` test added. | §4.2 (46-D2.7), §18 F22 |
| F-16 | MED | **Resolved in Rev 3 (G2-M1).** The file records the workspace **registry id** (not the canonical root); a same-path/different-repo swap now fires; Q10 and the test updated. | §4.2 (46-D2.2), §17 I33, §21 Q10 |
| F-17 | MED | **Resolved.** `flock` around read-modify-write; lock timeout degrades to `Session`; a contention test added (G2-L4). | §4.2 (46-D2.8), §18 F24, §20.1 |
| F-18 | MED | **Resolved in Rev 3 (G2-M2).** `parse_jsonc_document` returns the parsed document (`std::optional<nlohmann::json>`), plus `blank_or_comments_only`, from `include/ymh/config/jsonc.hpp`. | §4.2 (46-D2.11), §16 |
| F-19 | MED | **Resolved; claim softened in Rev 3 (G2-L3).** A required `"schema"` marker; a markerless file is ignored; the marker raises the bar but is not a trust boundary. | §4.2 (46-D2.3), §17 I33 |
| F-20 | MED | **Resolved.** `10:156` `UiMode` amendment declared; the grep claim scoped to shipped code. | §1.3.1 46-S10, §5.1, §5.2 item 1 |
| F-21 | MED | **Resolved.** `22:319-320` named as amended. | §1.3.1 46-S11, §2, §5.2 item 3 |
| F-22 | MED | **Resolved.** The `(void)theme` claim scoped to `render_dialog`/`render_exit_confirm`; `render_switcher` consumes theme. | §6.2.3 |
| F-23 | MED | **Resolved.** The two shipped tests are listed as rewrites under D5/D6. | §7.3, §8.3 |
| F-24 | MED | **Resolved.** `stored` removed from the reply handler; the cell loop's `live`-only iteration cited. | §9.2 (46-D7.3), §9.1 |
| F-25 | MED | **Resolved.** 23-D58 cited at `:375` + §6.5 `:1253-1293`. | §1.3.1 46-S5, §9.1 |
| F-26 | MED | **Resolved in Rev 3 (G2-L2/G2-M6).** The error path is cited to `ToolRegistry` (`tool_registry.cpp:358-362`); the **`error` field** (not output) is `to_string(code)` = `"Timeout"` (`errors.cpp:14`); MCP maps `CallTimeout` → `Timeout`. | §10.2.4 (46-D8.11) |
| F-27 | MED | **Resolved.** The `09 §4.5` amendment is dropped; tool timeouts are `07 §5.5`. | §10.2.1 (46-D8.2) |
| F-28 | MED | **Resolved in Rev 3 (G2-M4).** `tools.timeout_ms` targets `21 §3.3` + a new `21 §7.12 tools` (there is no `tools` §7.x today; `:1220` is §7.5 `permissions`). | §10.2.1 (46-D8.2) |
| F-29 | MED | **Resolved.** The "PTY torn down" claim is dropped; `terminal_tool.cpp:202-210`, `:221` and `pty.cpp:282`, `:311` cited. | §10.2.3 (46-D8.7) |
| F-30 | MED | **Resolved.** The exact spinner construction/string (`<glyph>  · <mode>`) is pinned and golden-tested. | §11.2.4, §20.2 G6 |
| F-31 | MED | **Resolved.** The spinner advance/visibility is active-session-scoped; the global repaint gate is retained (RB-17); I19 corrected. | §11.2.1/3/6, §17 I18/I19 |
| F-32 | MED | **Resolved.** I20 qualified by the RB-12 25 ms modal-tail window. | §12.1, §17 I20, §18 F29 |
| F-33 | MED | **Resolved.** The duplicate test is removed; the shipped `UI45_D5_EscSurvivesHistoryRecall` is referenced. | §12.2.4, §12.3 |
| F-34 | MED | **Resolved.** Hydration filters on `source.kind == Kind::User`; a plugin/goal non-hydration test added. | §13.2.1, §17 I21, §18 F30 |
| F-35 | MED | **Resolved.** The dedup citation is `ui_model.cpp:197-200`. | §13.2.2 |
| F-36 | MED | **Resolved.** The real localcode shape is pinned and a fixture test added; Q3 resolved. | §14.1/14.2.6/7, §20.1 |
| F-37 | MED | **Resolved; wording fixed in Rev 3 (G2-L5).** Five D12 citations were corrected and the `PolicyRule.command` `:114` citation was **removed** (not replaced); the model import is `config.cpp:1497-1520`, 0600 `cli.cpp:851-853,894`, trigger `cli.cpp:929-983,1167-1169`, validation `cli.cpp:888`. | §14.1 |
| F-38 | MED | **Resolved.** `max_tokens` targets `LlmCallConfig::max_tokens`. | §14.2.1, §16 |
| F-39 | MED | **Resolved; made enforceable in Rev 3 (G2-M14).** The redactor gains the JSON `"api_key": "…"` form; the vacuous `ymh config` dump claim is dropped; never-stringify is the primary control; tests added. | §14.2.11, §17 I36 |
| F-40 | MED | **Resolved.** 08 §5.1 and 28 `to_provider_config` added to the amendment list; an end-to-end key test added. | §14.2.3, §20.1 |
| F-41 | MED | **Resolved.** 25-D14 `:1774-1775` and D15 `:1809` (and `:1808`/`:1810`/`:1811`) named as superseded. | §1.3.1 46-S8, §2 |
| F-42 | LOW | **Resolved.** `modal_owns_input` cited at `supervisor.cpp:1511-1515`. | §5.1, §17 I9 |
| F-43 | LOW | **Resolved.** The hint row is cited at `ui_render.cpp:373-380`. | §7.1 |
| F-44 | LOW | **Resolved.** End-lines corrected: dbox `1204-1217`, `render_dialog` `620-664`, `render_switcher` `845-847`. | §6.1 |
| F-45 | LOW | **Resolved.** `/help` construction cited at `command_registry.cpp:280-291`. | §7.2.4 |
| F-46 | LOW | **Resolved.** Ordering cited to `host_runtime.cpp:531-532` → `registry.cpp:609-644`. | §9.1 |
| F-47 | LOW | **Resolved.** `drain_all` cited at `agent_loop.cpp:1207-1212`; `std::async` at `:1254`. | §10.1, §10.2.4 |
| F-48 | LOW | **Resolved.** `% kReasoningSpinnerFrames.size()` (`ui_render.cpp:225`). | §11.2.4 |
| F-49 | LOW | **Resolved.** §2's D10 row reconciled to "none (45-D5 retained)". | §2, §12.2.5 |
| F-50 | LOW | **Resolved; citation corrected in Rev 3 (G2-L10).** `run_editor` returns 127 on `execvp` failure (`session_export.cpp:326-327`); the parent returns `WEXITSTATUS(status)` at `:336` (rev 2 cited `:337`, which is the closing `}`); `-1` only on fork/waitpid error. | §15.1/15.2.5 |
| F-51 | LOW | **Resolved.** `tool_context.hpp` sketch pins `<chrono>`/`<optional>`. | §16 |
| F-52 | LOW | **Resolved.** The `GrantStore` sketch pins `<vector>`, `<memory>`, `<filesystem>`, `permission_policy.hpp`. | §16 |
| F-53 | LOW | **Resolved.** `print_localcode_notes` cited at `cli.cpp:730-742`. | §14.1/14.2.12 |
| F-54 | LOW | **REBUTTED.** The spec cites `supervisor.cpp:1644` for the `/export` caller of `edit_export_file`; line 1644 is exactly `const int status = edit_export_file(output);` (verified). The gate's claim that the caller is at `:1645` is a false positive (line 1645 is `if (status != 0) {`). No change to the citation. | §15.1 |
| F-55 | LOW | **Resolved in Rev 3 (G2-L1).** Every 46-F# is mapped to a §54 category (or `—`); the sequence runs F1–F31; the previously-untested rows F1/F5/F17/F24/F25 now name §20 tests, and only F8/F12 are explicit documented outcomes. | §18, §20.1 |
| F-56 | LOW | **Resolved.** Grant id is content-derived (`"grant:" + tool + ":" + fnv1a64(command + path)`). | §4.2 (46-D2.12) |

**Invariant audit response (gate §D; corrected in Rev 3):** the retained/
regression pins are **I4, I6, I17, I23** (they restate pre-existing behaviour);
**I14 and I20 remain gate invariants (`Y`)** — I14 is now genuinely testable via
`resume_in_flight_` (G2-H1) and I20 is exercised by the D10 regression test. I2,
I15, I16, I18, I19, I20, I21, I24, I29, I30, I33, I36 are corrected in Rev 3;
I14 gains the `resume_in_flight_` clause; I29–I36 are the added invariants. No
invariant is decorative: every `Y` row names a §20 test and every `pin` row names
the retained behaviour it protects.

**Test-gap response (gate §E):** vacuous tests are marked and re-pointed in
§20.1; the required positive tests (git deadline pre/post, PTY/MCP expired,
process expired, disabled deadline, loop/gate `Always` restart,
api_key-reaches-provider + never-logged/rendered + env-optional +
workspace-layer-rejected + scaffold-0600 + JSON redaction, localcode fixture,
two-press Enter, multi-candidate Enter, resume-in-flight, lock contention,
master-switch-vs-shell-deny, stale-notice, no-profile, unknown-partial) are
added. The five previously-untested §18 rows (F1/F5/F17/F24/F25) now each name a
test (G2-L1).

**Citation audit response (gate §F):** every retained citation was re-opened in
Rev 2 and again in Rev 3. Corrections are listed per finding above; F-54 is
rebutted. No citation is marked "unverified".

### 23.2 Gate-2 findings (Rev 3)

Status: **resolved** = fixed in Rev 3; **rebutted** = the gate's claim is factually
wrong (evidence given). Every gate-2 finding is dispositioned; none is omitted.

| ID | Sev | Disposition | Where |
|---|---|---|---|
| G2-H1 | HIGH | **Resolved (Rev 3); insertion site corrected in Rev 4 (N1/N2); refcounted in Rev 5 (O-H3/O-M6); release discipline corrected in Rev 6 (NEW-1).** F-07's rev-2 `pending_resume_` gate was always open (the entry is erased at `supervisor.cpp:1274` before `resume_after_attach` submits). Replaced by `resume_in_flight_`, inserted at the top of `resume_after_attach` (`:1042`, the single funnel, covering `:1036`/`:1275`) and released via the guarded `release_resume` at `apply_resume_success` (`:1090`) and the failure branch (`:1053-1059`); `recover_unknown_session` no longer touches it (it has non-resume callers, `prompt` `:1450` and `agent.select` `:1907`); the list reply branch (`:1337`) is gated on it. Rev 5 makes it a per-workspace refcount and adds the disconnect drain. A true interleaving test is added. | §9.2 (46-D7.2), §17 I14, §18 F26/F38/F39, §20.1 |
| G2-H2 | HIGH | **Resolved.** `McpErrorCode::Timeout` does not exist; corrected to `McpErrorCode::CallTimeout` (`include/ymh/mcp/mcp_types.hpp:87`; `to_string` = `"CallTimeout"`, `mcp_types.cpp:116`). The whole spec's pinned symbol set was then re-grepped. | §10.2.3 (46-D8.7), §16 |
| G2-M1 | MED | **Resolved.** F-16 was a false closure (a path is not an identity). The grants document records the workspace **registry id** (`registry.cpp:44-46`), resolved via `WorkspaceRegistry::findByCanonicalPath`; the test is a same-path/different-id case; Q10 updated. | §4.2 (46-D2.2), §17 I33, §21 Q10, §20.1 |
| G2-M2 | MED | **Resolved.** F-18's pin was defective: `parse_jsonc_document` now returns the parsed document (`std::optional<nlohmann::json>` + `error`), with `blank_or_comments_only` exported; the §16 sketch includes `<nlohmann/json.hpp>`. | §4.2 (46-D2.11), §16 |
| G2-M3 | MED | **Resolved.** F-15's citation was false (the importer has no `fsync`). `fsync` is stated as a **new** requirement (temp fd + `fsync` + `rename` + parent-dir `fsync`); the importer is not cited as precedent. | §4.2 (46-D2.7) |
| G2-M4 | MED | **Resolved.** F-28's replacement citation was partly wrong: `21` has no `tools` §7.x (`:1220` = §7.5 `permissions`). `tools.timeout_ms` is recorded in `21` §3.3 + a new `21` §7.12 `tools`. | §10.2.1 (46-D8.2), 46-S14, §2 |
| G2-M5 | MED | **Resolved.** §4.2.6 was false: `PermissionGate::decide` (`permission_policy.cpp:413`) has no `force_ask` guard. Pinned the guard at the gate; corrected the "all three already guard" count; test added. | §4.2 (46-D2.6), §17 I6, §20.1 |
| G2-M6 | MED | **Resolved.** I16 was false for MCP (the tool swallows `McpError` at `mcp_tool.cpp:57-62`). I16 split by primitive; `mcp_tool.cpp` maps `CallTimeout` → `ToolError{Timeout}`. | §17 I16, §10.2.4 (46-D8.7/11) |
| G2-M7 | MED | **Resolved.** I30 overstated "no tool call times out": disabling `tools.timeout_ms` leaves the model `timeout_ms` (`builtin_tools.cpp:391-394`) and MCP `call_timeout_ms` (60 s, `config.hpp:134`) live. I30 and F28 qualified. | §17 I30, §18 F28, §10.2.3 (46-D8.9) |
| G2-M8 | MED | **Resolved.** PTY expiry was unrepresentable. `PtyRead`/`PtyExit` gain `bool timed_out`; `terminal_tool` maps it to `ToolError{Timeout}`. | §10.2.3 (46-D8.7), §16 |
| G2-M9 | MED | **Resolved.** `McpCallOptions` is declared in `include/ymh/mcp/mcp_types.hpp:133-137`, not `mcp_tool.cpp`. Citation corrected. | §10.2.3 (46-D8.7), §16 |
| G2-M10 | MED | **Resolved.** The §16 supervisor sketch put `const` member functions in a file-scope anonymous namespace. Moved into the `SupervisorApp` class body (declared as members). | §16 |
| G2-M11 | MED | **Resolved.** `api_key_env` is hard-required (`provider_registry.cpp:68-70`). Pinned that `create()` skips the `is_valid_env_name` check when `config.api_key` is non-empty; `api_key_env` becomes optional. Test added. | §14.2 (46-D12.2), §20.1 |
| G2-M12 | MED | **Resolved.** The scaffold was `0644` (`config.cpp:1044`). `write_default_config` now creates `0600`; the loader additionally rejects a key from a group/world-readable file. Test added. | §14.2 (46-D12.10), §21 Q13, §20.1 |
| G2-M13 | MED | **Resolved.** The workspace layer could carry the key inside a repo. Pinned: `api_key` is global-only; a workspace-layer `api_key` is a `ConfigError`. Test added. | §14.2 (46-D12.2/10), §17 I24, §21 Q13, §20.1 |
| G2-M14 | MED | **Resolved.** Redaction was unwired and did not match the JSON form; the `ymh config` dump surface does not exist. `redact_secrets` gains `"api_key"\s*:\s*"…"`; the dump claim is dropped; never-stringify is primary; call sites pinned; tests added. | §14.2 (46-D12.11), §17 I36, §20.1 |
| G2-M15 | MED | **Resolved.** `08` §6.4 (`:855-867`) was contradicted but not named. Added to 46-S7 and the 46-D12 register row. | §1.3.1 46-S7, §2 |
| G2-M16 | MED | **Resolved.** Amendment-ID off-by-one: provider plumbing is **D12.3**, `max_tokens` is **D12.4**. 46-S7/S13 → D12.3; the two `max_tokens` annotations → D12.4. | §1.3.1 46-S7/S13, §14.2.1/3/4, §16 |
| G2-M17 | MED | **Resolved.** D6 silently breaks the shipped `UX_U15_EnterAcceptsHighlightAndDispatches` (25-D9). Added 25-D9/UX-U15 to 46-S12 and D6; the test is a required rewrite. | §1.3.1 46-S12, §8.3, §2 |
| G2-M18 | MED | **Resolved.** Enter candidate selection was undefined for multi-match prefixes. Pinned that Enter delegates to `complete_selected_command` (`command_hint_selected`, first when none); a multi-candidate test is added. | §8.2.1/4, §20.1 |
| G2-M19 | MED | **Resolved; narrowed in Rev 5 (O-H1).** F-10's pin was not a move: the shipped `validate_rule(const PolicyRule&)` (`permission_policy.cpp:98`) stays for the in-policy call site, and the exported form is `validate_rule(tool, command)` (2-arg — the config surface has no `path`). §3.2 reworded. | §3.2 (46-D1.2), §16 |
| G2-L1 | LOW | **Resolved.** The §18/§23 blanket test claim was false (F1/F5/F17/F24/F25 untested). Five tests added; only F8/F12 are explicit outcomes. | §18, §20.1 |
| G2-L2 | LOW | **Resolved.** The error result's field is `error`, not output (`tool_registry.cpp:25`). Corrected. | §10.2.4 (46-D8.11) |
| G2-L3 | LOW | **Resolved.** The F-19 overclaim softened: the marker raises the bar, it is not a trust boundary. | §4.2 (46-D2.3) |
| G2-L4 | LOW | **Resolved.** `flock` test gap closed by `UI46_D2_LockContentionDegradesToSession`. | §20.1, §18 F24 |
| G2-L5 | LOW | **Resolved.** §23 now says the `PolicyRule.command` citation was **removed**, not corrected. | §23.1 F-37 |
| G2-L6 | LOW | **Resolved.** I31's post-call check now has `UI46_D8_GitPostcheckTimesOut`. | §20.1 |
| G2-L7 | LOW | **Resolved.** I29's process clause now has `UI46_D8_ProcessExpiredDeadlineNotStarted`. | §20.1 |
| G2-L8 | LOW | **Resolved.** The `tool_context.hpp` sketch tag is `46-D8.3`, not `46-D8.2`. | §16 |
| G2-L9 | LOW | **Resolved.** `remaining()` now uses `std::chrono::duration_cast<std::chrono::milliseconds>(*deadline_ - now())` before `std::max` with zero. | §10.2.2 (46-D8.3) |
| G2-L10 | LOW | **Resolved.** §23 cites `session_export.cpp:336` for `WEXITSTATUS` (not `:337`). | §23.1 F-50 |
| G2-L11 | LOW | **Resolved.** The three shipped "secrets never in `Config`" comments (`config.hpp:14-17`, `config.cpp:953`, `config.hpp:298-300`) are named in the amendment list. | §1.3.1 46-S6, §1.3.2 |
| G2-L12 | LOW | **Resolved.** The override claim was misleading: there is no `YMH_LLM_API_KEY` and no `--api-key` flag. §14.2.8 now states the literal key is global-only with no env/CLI override; Q12 records the decision. | §14.2.8, §21 Q12 |
| G2-L13 | LOW | **Resolved.** I24's UI clause gains `UI46_D12_ApiKeyNeverRendered` plus the redaction unit test. | §20.1 |
| G2-L14 | LOW | **Resolved.** §23's F-20/F-21 section refs now read "§5.2 item 1"/"§5.2 item 3". | §23.1 |
| G2-L15 | LOW | **Resolved.** The localcode `type` change is pinned explicitly: the check at `config.cpp:1494-1495` also accepts `"openai-compat"`. | §14.2.6 |
| G2-L16 | LOW | **Resolved.** The process formula no longer uses a non-existent `∞`; process routes through `clamp_timeout` uniformly. | §10.2.3 (46-D8.7) |
| G2-L17 | LOW | **Resolved.** The pre-dispatch guard is documented as a narrow race-reducer; its test must construct a past deadline directly to be non-vacuous. | §10.2.4 (46-D8.10), §20.1 |
| G2-L18 | LOW | **Resolved.** The `/quit`/`/exit` one-Enter vs `/q` two-Enter asymmetry is documented in §8.3; a test is added. | §8.3, §20.1 |

**Gate-2 closure checklist:** (1) G2-H1 → §9.2/I14; (2) G2-H2 → §10.2.3/§16;
(3) G2-M1 → §4.2.2/I33/Q10; (4) G2-M2/M6/M7/M8 → §4.2.11/§17 I16/I30/§10.2.3;
(5) G2-M3/M4/M9/M10/M16/M19 → §4.2.7/§10.2.1/§10.2.3/§16/§1.3.1/§3.2;
(6) G2-M5 → §4.2.6/I6; (7) G2-M11…M15 → §14.2.2/8/10/11/§1.3.1;
(8) G2-M17/M18 → §1.3.1/§8.2/§8.3; (9) G2-L1 → §18/§20; (10) G2-L2…L18 → swept.

### 23.3 Gate-3 findings (Rev 4)

Status: **resolved** = fixed in Rev 4. The gate-3 report confirmed all 39 gate-2
findings closed (36 cleanly) and raised only rev-3 residuals; every one is
dispositioned below.

| ID | Sev | Disposition | Where |
|---|---|---|---|
| N1 | MED | **Resolved.** The marker insertion moved from the `pending_resume_` consume site to the **top of `resume_after_attach`** (`supervisor.cpp:1042`), the single funnel for `--resume`, the consume site (`:1275`), and the explicit already-attached `resume_from_history` branch (`:1036`). I14's explicit-path coverage claim is now true; a test fails on the rev-3 tree. | §9.1/§9.2 (46-D7.2/4), §17 I14, §18 F26, §20.1 |
| N2 | MED | **Resolved (Rev 4); superseded in Rev 6 / NEW-1.** The marker release is enumerated in §9.2 and runs at the **two genuine resume terminals** — `apply_resume_success` and the `!reply.ok` branch — via the guarded `release_resume`. Rev 4/Rev 5 also erased at the top of `recover_unknown_session`; Rev 6 removed that because the helper has non-resume callers (`prompt` `:1450`, `agent.select` `:1907`) and the unconditional decrement would underflow. No exit can leak, and a non-resume `UnknownSession` cannot wrap the count. | §9.1/§9.2 (46-D7.2), §17 I14, §18 F32, §20.1 |
| N3 | MED | **Resolved.** The PTY bullet is rewritten: the deadline methods are `PtySession::read`/`wait` (corrected from `PtyService`), the deadline is a **separate** optional parameter, `wait_ms`/`timeout` is unchanged, disabled = `nullopt` (today's semantics, `timed_out == false`), expired = `0ms` (immediate `timed_out`, never routed through `wait_ms`), and `timed_out` is derived only from the deadline parameter. | §10.2.3 (46-D8.7), §16, §17 I29, §20.1 |
| N4 | MED | **Resolved.** The MCP clamp's `caller_ms` is pinned to `(options.timeout.count() > 0 ? options.timeout : config_.call_timeout)`, so the configured server timeout (default 60 s) is a ceiling and an active deadline with `remaining > 60 s` cannot raise it. | §10.2.3 (46-D8.7/9), §16, §17 I15, §20.1 |
| N5 | LOW | **Resolved.** One canonical name: `UI46_D7_ResumeInFlightBeatsListReply` (§9.3 previously said `ResumeBeatsEarlyListReply`). | §9.3, §20.1 |
| N6 | LOW | **Resolved.** The interleaving test's determinism is pinned: `install_session_list_reply` + `seed_resume_in_flight` + `on_link_state` + `drain_actions`, with an explicit no-sleep rule; the additive seams are sketched in §16 (mirroring `submit_agent`/`install_agent_replies`/`seed_ensure_in_flight`). | §16, §20.1 |
| N7 | LOW | **Resolved.** I6's gate-guard clause (new behavior from 46-D2.6) is marked `Y` and gated by `UI46_D2_ForceAskNotPersistedAtGate`; the `Once`/`Session` clauses remain pins. | §17 I6 |
| N8 | LOW | **Resolved.** The `global_layer` plumbing is shown in §16: `apply_llm(..., bool global_layer)` mirrors `apply_document`'s existing parameter and its `session` guard (`config.cpp:840-841`, `:898-899`); a workspace-layer `api_key` fails at load. | §14.2 item 2, §16 |
| N9 | LOW | **Resolved.** A non-transport resume failure decrements the marker (erasing at zero) and leaves no focus; the recovery is the first-keystroke create path (23 §1.1). The D7.2 rationale that assumed a later `session.list` is deleted; 46-F32 records N2 and N9. | §9.3, §18 F32, §21 Q14 |

**Gate-3 closure checklist:** (1) N1 → §9.2/I14/§20.1; (2) N2 → §9.2/I14/F32/§20.1;
(3) N3 → §10.2.3/§16/I29/§20.1; (4) N4 → §10.2.3/§16/I15/§20.1; (5) N5/N6 →
§9.3/§16/§20.1; (6) N7/N8/N9 → §17 I6/§14.2+§16/§9.3+F32.

**Symbol sweep (Rev 4).** Every citation and symbol touched by Rev 4 was re-opened
and re-grepped. In addition to the gate-2 sweep below, confirmed: `PtySession::read`
/`PtySession::wait` (`pty.hpp:351`/`:355`; `PtyService` has no `read`/`wait`,
`:376-387`), `PtyRead`/`PtyExit` (`pty.hpp:95-100`/`:53-57`), the `read` `wait.count()
> 0` branch (`pty.cpp:282`) and `wait` `timeout.count() == 0` branch (`:311`),
`terminal_tool.cpp` `wait_ms` parse (`:202-210`) and `session->read(...)` call
(`:221`), `McpCallOptions` (`mcp_types.hpp:133-137`), `McpServerConfig::call_timeout`
(`mcp_types.hpp:172`), `McpClient::callTool` timeout expression
(`mcp_client.cpp:206-207`), `options.timeout = server_.call_timeout`
(`mcp_tool.cpp:42`), `call_timeout_ms` (`config.hpp:134`), `apply_llm`
(`config.cpp:389`) / `apply_document(..., bool global_layer)` (`:840-841`) / the
`session` guard (`:898-899`), `resume_after_attach`/`resume_from_history`/
`apply_resume_success`/`recover_unknown_session`/`submit_to`
(`supervisor.cpp:1042`/`:1032`/`:1090`/`:1121`/`:1455`), the `refresh_sessions`
submit and reply branch (`:1295-1353`), `pending_resume_` (`:2773`),
`SupervisorConnection::submit` reply guarantees (`supervisor_connection.cpp:93`,
`:100`, `:391-399`, `:508`), and the harness precedent
`submit_agent`/`install_agent_replies`/`seed_ensure_in_flight`/`ensure_in_flight()`
(`supervisor.cpp:1777-1790`/`:2898-2905`/`:2885`/`:2968`;
`supervisor_harness.hpp:79-80`/`:89`/`:115`). New Rev-4 symbols are explicitly
marked **new** in §16.


**Symbol sweep (G2-H2 follow-on).** Every type/enum/function/field name pinned in
§16 and the D-decisions was re-grepped against the shipped headers. Confirmed to
exist: `McpErrorCode::CallTimeout` (`mcp_types.hpp:87`), `McpCallOptions`
(`:133`), `McpError` (`mcp_types.hpp`), `PtyRead`/`PtyExit` (`pty.hpp:95`/`:53`),
`ProcessRequest`/`ProcessResult` (`process.hpp:26`), `ToolConfig`
(`execution/config.hpp`), `ExecutionEnvironment`/`LocalEnvironment`
(`environment.hpp:28`/`:46`), `ToolContext` (`tool_context.hpp:22`),
`PermissionPolicy`/`RulePermissionPolicy` (`permission_policy.hpp:120`/`:137`),
`PolicyRule` (`:76`), `GrantScope`, `WorkspaceId`/`WorkspaceRecord`
(`registry.hpp:40`/`:65`), `WorkspaceRegistry::findByCanonicalPath`,
`LlmSettings` (`config.hpp:104`), `LLMProviderConfig`
(`provider_registry.hpp:25-37`), `LlmCallConfig::max_tokens` (`llm_call_config.hpp:37`), `redact_secrets`
(`redaction.hpp:14`),
`CommandRegistry::find`/`complete` (`command_registry.cpp:71`/`:88`),
`Command`/`CommandContext` (`command_registry.hpp`), `UiModel`/`CommandHint`
(`ui_model.hpp`), `UiMode` (`ui_event.hpp:45-51`), `SupervisorApp`
(`supervisor.cpp:356`), `apply_resume_success`/`recover_unknown_session`/
`resume_after_attach` (`supervisor.cpp:1090`/`:1121`/`:1042`), `pending_resume_`
(`:2773`), `switcher_has_targets`/`classify_enter`/`edit_prompt` (new; §16),
`ToolError`/`ToolErrorCode::Timeout` (`errors.hpp:15`), `error_result`
(`tool_registry.cpp:21`), `write_default_config` (`config.cpp:1033`),
`is_valid_env_name`/`ProviderRegistry::create` (`provider_registry.cpp:26`/`:52`),
`parse_jsonc`/`blank_or_comments_only` (`config.cpp:101`/`:52`),
`validate_rule`/`validate_glob` (`permission_policy.cpp:98`/`:81`),
`session_export.cpp` `run_editor` internals (`:326-337`).

### 23.4 Oracle findings (Rev 5)

Status: **resolved** = fixed in Rev 5. The Oracle report
(`/tmp/opencode/spec46-oracle.md`: DO NOT APPROVE — 3 HIGH / 7 MEDIUM / 3 LOW)
confirmed the four gate-3 MEDIUMs and five LOWs closed and D8's trichotomy sound;
it raised three structural HIGHs, seven MEDIUMs and three LOWs. All are
dispositioned below.

| ID | Sev | Disposition | Where |
|---|---|---|---|
| O-H1 | HIGH | **Resolved.** Durable grants and config rules are rescoped to **tool + command only**; the `path` dimension is dropped from both (the loop never populates `PermissionRequest::path`), a `path` key is a `ConfigError`, and the item-2 answer is stated explicitly. | §3.2.2/4, §4.2 (item-2 answer + D2.1/4/12), §16, §17 I1/I5/I33, §18 F21/F35, §21 Q15.1 |
| O-H2 | HIGH | **Resolved.** `WorkspaceRuntimeOptions::grant_store` pinned; the daemon owns the store (member before `runtime_`), opens the registry and resolves the id **before** `WorkspaceRuntime::create`, and the policy loads via its constructor; appends serialized by the store's `flock`. | §4.2.3/4, §16, §17 I5/I33, §18 F37, §19, §20.1/20.3 |
| O-H3 | HIGH | **Resolved.** The false "reply always delivered on disconnect" claim is corrected; `SupervisorConnection::process_requests`/`handle_disconnect` reply to every pending request, so the refcount decrement always runs. | §9.2/9.3, §16, §17 I14, §18 F38, §20.1/20.3 |
| O-M1 | MED | **Resolved.** `PolicyRule::literal`; `remember()` sets it; the grants file records `"match": "literal"` (default) with `"glob"` opt-in; escaping is impossible (`glob_match` has no escape char), so literal matching is the fix. | §4.2.1/4, §16, §17 I5, §18 F36, §20.1 |
| O-M2 | MED | **Resolved.** The confinement claim is corrected: `shell`/`git`/`mcp.*` are unrestricted command execution (no OS sandbox); "inside the workspace" applies to path tools only. | §3.2.4, §17 I2/I4, §18 F34, §21 Q15.5 |
| O-M3 | MED | **Resolved.** The route is pinned: `to_agent_config` sets `AgentConfig::parameters.max_output_tokens` from `llm.max_tokens`; `agent_loop.cpp:539` maps it to `LlmCallConfig::max_tokens`. The "not `max_output_tokens`" phrasing is corrected. | §14.2.1/4, §16, §20.1 |
| O-M4 | MED | **Resolved.** The parser catches `PolicyConfigError` and rethrows via `fail(...)` as `ConfigError`; I1 and §3.2.2 agree. | §3.2.2, §17 I1, §18 F33 |
| O-M5 | MED | **Resolved.** The `force_ask` guard is inside `remember()` (single mutation seam); call-site guards retained as redundant defense. | §4.2.6, §17 I6, §20.1 |
| O-M6 | MED | **Resolved (Rev 5); release discipline corrected in Rev 6 / NEW-1.** `resume_in_flight_` is a per-workspace refcount; the gate is `count == 0`; overlapping resumes keep it ≥ 1 until both resolve. The release runs only at the two genuine resume terminals through the guarded `release_resume` (`recover_unknown_session` no longer touches it). | §9.2, §16, §17 I14, §18 F39, §20.1 |
| O-M7 | MED | **Resolved.** `SpdlogLogger::log` applies `redact_secrets`; the notice-ring producers pass through it; I36 is restated to the enforceable form. | §14.2.11, §17 I36, §16 |
| O-L1 | LOW | **Resolved.** `remember()` pushes under `mutex_`, releases it, then calls `store_->append`; concurrent appends serialized by `flock`. | §4.2.4, §20.1 |
| O-L2 | LOW | **Resolved.** The redaction token is the shipped `[REDACTED]` (`redaction.cpp:10`); I36 and D12.11 align. | §14.2.11, §17 I36 |
| O-L3 | LOW | **Resolved.** The `config.hpp:14-17` comment is rewritten and named in 46-S6. | §1.3.1 46-S6 |

**Oracle closure checklist:** (1) O-H1 → §3.2.2/4 + §4.2 + §16 + I1/I5/I33 +
F21/F35; (2) O-H2 → §4.2.3 + §16 + I5/I33 + F37 + §19; (3) O-H3 → §9.2 + I14 +
F38; (4) O-M1 → §4.2.1 + §16 + F36; (5) O-M2 → §3.2.4 + I2/I4 + F34; (6) O-M3 →
§14.2.1/4 + §16; (7) O-M4 → §3.2.2 + I1 + F33; (8) O-M5 → §4.2.6 + I6; (9) O-M6 →
§9.2 + I14 + F39; (10) O-M7/O-L2 → §14.2.11 + I36; (11) O-L1 → §4.2.4;
(12) O-L3 → §1.3.1.

**Accepted risks (unchanged plus two newly stated):** workspace-local grants lost
on a repo move (fail-safe re-ask, §4.2.2); a same-user process reading the `0600`
global config (same exposure as the env var); git's non-preemptible mid-call hang
(documented, I31/I35); unrestricted `shell`/`git`/`mcp.*` execution under the
master switch (O-M2, 46-D1.4); **a tool-wide durable path grant** — one `Always`
on `write_file src/a.txt` auto-allows every `write_file` in the workspace,
including `.ymh/` and git-hook paths inside the root (the broadest user-visible
consequence of dropping the path dimension; accepted, §4.2.13/§21 Q16.6/F35);
and **a configured `api_key` printed in an unrecognized shape is not guaranteed
redacted**, because the redactor is shape-based (the primary control is never
stringifying the key; accepted, I36/§21 Q16.5).

**Symbol sweep (Rev 5).** Every citation and symbol touched by Rev 5 was
re-opened and re-grepped against the shipped tree: `PermissionRequest::path`
(`include/ymh/policy/permission_policy.hpp:54-67`; unset at
`src/agent/agent_loop.cpp:662-675`), `path_pattern`/`command_pattern`
(`src/policy/permission_policy.cpp:117-137`), `remember` (`:257-281`),
`glob_match`/`path_glob_match` (`include/ymh/execution/glob.hpp`;
`src/execution/glob.cpp:9-33`), `WorkspaceRuntimeOptions`
(`include/ymh/agent/workspace_runtime.hpp:75-113`), the daemon order
(`src/host/workspace_host.cpp:489-518`; members `:353-354`),
`WorkspaceRegistry::findByCanonicalPath`
(`include/ymh/registry/registry.hpp:218`),
`SupervisorConnection::process_requests`/`handle_disconnect`
(`src/ui/supervisor_connection.cpp:376-406`/`:431-440`), `agent_loop.cpp:539`,
`to_agent_config` (`src/cli/wiring.cpp:174-191`), `LlmCallConfig::max_tokens`
(`include/ymh/llm/llm_call_config.hpp:37`),
`GenerationParameters::max_output_tokens` (`include/ymh/llm/llm_request.hpp:44`),
`openai_adapter.cpp:739-740`, `SpdlogLogger::log`/`redact_log_text`
(`src/core/logging.cpp:42-46`/`:175-189`), `kRedacted`
(`src/llm/redaction.cpp:10`), `apply_permissions` (`src/config/config.cpp:349`),
`validate_rule`/`validate_glob` (`src/policy/permission_policy.cpp:98`/`:81`),
`cli.cpp:289`, `config.hpp:14-17`, `push_notice`/`surface_notice`
(`src/ui/supervisor.cpp:929`/`:944-947`). New Rev-5 symbols are explicitly marked
**new** in §16.

### 23.5 Oracle re-review findings (Rev 6)

Status: **resolved** = fixed in Rev 6. The Oracle re-review
(`/tmp/opencode/spec46-oracle-rereview.md`: DO NOT APPROVE — 1 HIGH / 1 MEDIUM /
4 LOW) re-derived all 13 O-findings and confirmed 12 genuinely closed — the three
structural HIGHs (O-H1 path rescope, O-H2 grant-store seam/order, O-H3 disconnect
drain) correct and coherent, the rescope not over-correcting into a glob-escape
hole, and D3–D6, D8–D13 and the localcode mapping regression-free. It raised one
new HIGH (a Rev-5 regression), one new MEDIUM and four new LOWs, all
dispositioned below.

| ID | Sev | Disposition | Where |
|---|---|---|---|
| NEW-1 | HIGH | **Resolved.** The refcount release is removed from `recover_unknown_session` (which has non-resume callers `prompt` `:1450` and `agent.select` `:1907`) and moved to the resume-failure terminal (`!reply.ok` `:1053-1059`, before the `unknown`/`surface_notice` split) plus `apply_resume_success`, both via the guarded `release_resume` (decrement iff present and `> 0`; erase at 0; no-op otherwise). This closes the Rev-5 underflow that would wrap a zero entry to `SIZE_MAX` and permanently close the D7.2 gate. A non-resume test covers both callers. | §9.1/9.2/9.3, §16 (marker + `release_resume`), I14, F32, §20.1, §21 Q14.2/Q15.4/Q16.1 |
| NEW-2 | MED | **Resolved.** An explicit config **deny** (a matching `Deny` rule at a non-grant layer) now vetoes a matching durable `Always` grant (`Layer::LocalGrant`, `Allow`) regardless of specificity, via a post-pass in `evaluate()`; `rule_greater` and config-vs-config precedence are unchanged, and `Session` grants are excluded. The D1.6/F1 recovery now actually revokes a grant and is consistent with localcode `decision: "deny"` (imported as a `Layer::Project` rule; the mapping is unchanged). | §3.2.6/3.3, §4.2.9/13, §16, I37, F1/F40, §20.1, §21 Q16.2 |
| NEW-3 | LOW | **Resolved.** I1 no longer calls a grants-file `path` key a `ConfigError`; config-rule `path` keys stay fatal, grants-file `path` keys cause the fail-safe file skip (46-I7/I33). | §17 I1 |
| NEW-4 | LOW | **Resolved.** The pinned daemon order guards `findByCanonicalPath`'s `optional` (`if (!record) return HostExitCode::RegistryFailed;`) before opening the store, covering the `ensureWorkspaceRegistered` `findById`-branch case. | §4.2.3, §16, F37 |
| NEW-5 | LOW | **Resolved.** The `kRedacted` citation is corrected to `src/llm/redaction.cpp:10` (it was `:9`, a blank line) in D12.11, I36, the O-L2 row and the symbol sweep. | §14.2.11, §17 I36, §23.4 O-L2/sweep |
| NEW-6 | LOW | **Resolved.** I36 is restated to the shape-based enforceable form: the redactor matches the pinned shapes and does not know the configured value, so an unrecognized-shape key is not guaranteed redacted; "never stringify the key" remains the primary control. | §17 I36, §21 Q16.5 |

**Oracle re-review closure checklist:** (1) NEW-1 → §9.2 + §16 + I14 + F32 +
§20.1 + §21 Q14.2; (2) NEW-2 → §3.2.6 + §4.2.13 + §16 + I37 + F40 + §21 Q16.2;
(3) NEW-3 → I1; (4) NEW-4 → §4.2.3 + §16 + F37; (5) NEW-5 → D12.11 + I36 + O-L2;
(6) NEW-6 → I36 + §21 Q16.5.

**Symbol sweep (Rev 6).** Every citation and symbol touched by Rev 6 was
re-opened and re-grepped against the shipped tree:
`recover_unknown_session` (`src/ui/supervisor.cpp:1121-1133`) and its three
callers (`:1055`, `:1450`, `:1907`), `resume_after_attach`/`apply_resume_success`
(`:1042`/`:1090`), the D7.2 `session.list` reply branch (`:1337-1350`),
`prompt` (`:1440-1453`), `rule_greater`/`specificity_key`
(`src/policy/permission_policy.cpp:58-79`), `evaluate`/`remember`
(`:204-281`), `PolicyRule::Layer`
(`include/ymh/policy/permission_policy.hpp:83-91`),
`WorkspaceRegistry::findByCanonicalPath`
(`include/ymh/registry/registry.hpp:218-219`), `ensureWorkspaceRegistered`
(`src/host/workspace_host.cpp:420-440`), `kRedacted`/`redact_secrets`
(`src/llm/redaction.cpp:10`/`:52-98`), `SpdlogLogger::log`
(`src/core/logging.cpp:42-46`). The new Rev-6 symbol (`release_resume`) is marked
**new** in §16.

*End of spec 46 Rev 6.*
