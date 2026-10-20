# 93 - MCP Tool Defaults and Denied-Tool Feedback: server-trust default Allow and an unmissable denial result

```
Status: verified (Rev 2)
Revision: 2
Verification status: adversarial reviewer (independent, Rev 2): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Revision note: Rev 2 applies Gate 93 findings M1-M4 and L1-L6. M1: an explicit
               global `permissions.default:"deny"` now vetoes an implicit MCP
               Allow; an explicit per-server key or a matching rule is the
               override. M2: the `destructiveHint` premise is corrected (it is
               already wired to `request.destructive` and the ReadOnly hard
               deny). M3: the policy-deny test now matches the shipped reason
               tokens (`denied by policy` / `rule`), not a rule id. M4: DF13 is
               scoped to the denial text and provider message; the export's
               `ToolCall.arguments` are a separate pre-existing surface. L1:
               the canonical pass-through is validated. L2: the export no
               longer prints a duplicate `_Error:` line. L3: both
               `no-resolver` and its prose form are mapped. L4: DF-F7 is F2.
               L5: the master-switch table covers per-server `"deny"`. L6:
               the tracker row is a promotion requirement.
Tracker: DESIGN_STATUS.md row 93 is added in the promotion/verification change
         set (AGENTS.md drift tripwire). This working-tree draft intentionally
         does not edit the tracker or any other file.
Component: 93 (errata) -- two workstreams:
           (A) MCP defaults: an enabled MCP server's tools default to Allow
               because an MCP server exists only when the user put it in the
               global config (50-D4) and the permission table is global-only
               (90-D18). A per-server `default_verdict` override and ordinary
               tool rules remain the narrowing mechanisms. An explicit global
               `permissions.default:"deny"` is fail-closed: an absent per-server
               key yields Deny, and only an explicit per-server value or a
               matching rule can produce a different verdict. The spec-89 guard
               order and every hard deny are unchanged; MCP requests carry no
               shell text (`classify_shell_request` returns NotApplicable at
               `src/policy/shell_guard.cpp:2534-2537`). A `destructiveHint:true`
               MCP tool is already in `request.destructive` and is denied under
               `ReadOnly`; this spec retains that wiring.
           (B) Denied feedback: a Denied `ToolResult` carries canonical,
               model-facing denial text that names the outcome, says the tool
               was NOT executed, renders the reason, and forbids reporting
               success. The text is projected into the provider Tool message,
               the export (which suppresses the duplicate error line), and the
               UI (which also renders a `DENIED` badge).
Depends on: 09-permissions.md (verified; `PermissionPolicy`, `PolicyVerdict`,
           `PermissionRequest`, the rule fold), 15-mcp-adapter.md (verified;
           `mcp.<server>.<tool>` naming, `tool_defaults` lowering, M7),
           46-permissions-ui-errata.md (verified; config rules, master switch,
           46-D1.3/46-D1.6, deny veto), 50-skills-mcp-and-session-lifecycle-errata.md
           (verified; 50-D4/50-I22 MCP definitions are global-layer only),
           89-shell-command-safety-guard-errata.md (verified; L1/L2 precedence),
           90-path-scoped-permissions-errata.md (verified; 90-D4 path matching,
           90-D18 global-layer-only permissions), 01-session.md (verified;
           `ToolResult`, I12, `deriveMessages`), 06-agent-loop.md (verified;
           A-F6 deny path), 17-ui-transcript-errata.md (verified; tool fold
           rendering), 25-ui-ux-errata.md (verified; 25-D4 denial output),
           21-config-jsonc-errata.md (verified; MCP key defaults),
           56-dsh-fidelity-gap-closure-errata.md (verified; 56-D6 non-mirror
           justification rule).
Supersedes: the `McpServerConfig::default_verdict{Ask}` initializer
           (`include/ymh/mcp/mcp_types.hpp:193`) and the 15 sec 4.1
           "Never Allow unless configured" comment
           (`docs/design/15-mcp-adapter.md:450-452`);
           15-mcp-adapter.md sec 6.1 "MCP tools default to Ask" and the
           per-server "(default Ask)" parenthetical; 21-config-jsonc-errata.md
           sec 7 key table row `mcp.server.default_verdict = "ask"` and the two
           sample values; 25-ui-ux-errata.md 25-D4's
           `ToolResult{Denied, output=<reason>}` shape (the output becomes the
           canonical denial text containing the reason); 01-session.md
           sec 6.3's `content = payload.output` projection rule for the Denied
           outcome; 17-ui-transcript-errata.md sec 4 item 4 / F-04 / F-10's
           "no outcome marker" rule for the Denied outcome only.
```

## 1. Purpose, user reports, and precedence

### 1.1 Issue 1: every MCP call asks, even under an in-root allow rule

User report (paraphrased): "Even with an in-root allow rule, every MCP tool
call still asks. MCPs are meant to be used."

Verified cause, in the shipped tree:

1. MCP tools are registered as `mcp.<server>.<tool>`
   (`docs/design/15-mcp-adapter.md:347-364`; `src/mcp/mcp_tool.cpp`).
2. `AgentLoop` builds a `PermissionRequest` whose `tool` is that name and whose
   `arguments` are the model's arguments. It sets `request.path` only for tools
   with a path dimension (`src/agent/agent_loop.cpp:871-889`); an MCP request
   has no `path`.
3. A path rule is evaluated by `rule_matches`
   (`src/policy/permission_policy.cpp:164-209`). If `rule.path` is non-empty and
   the tool has no path dimension and is not `shell`/`terminal`, the function
   returns `false` (`:182-184`). Therefore the user's
   `{"tool":"*","path":"**","effect":"allow"}` rule - the in-root allow rule
   shape pinned by `90-path-scoped-permissions-errata.md` sec 4.7 - can never
   match an MCP tool. This is correct path semantics (an MCP tool has no path
   to scope); the defect is that the fallback is Ask.
4. No rule matches, so `evaluate` consults `config_.tool_defaults`
   (`src/policy/permission_policy.cpp:531-540`). For an enabled server the
   defaults table has one entry whose prefix is `mcp.<server>.` and whose
   verdict is `parse_verdict(server.default_verdict, "mcp.default_verdict")`
   unless the master switch is on (`src/cli/wiring.cpp:163-173`).
5. `McpServerSettings::default_verdict` defaults to `"ask"`
   (`include/ymh/config/config.hpp:316-330`), and
   `McpServerConfig::default_verdict` defaults to `PolicyVerdict::Ask`
   (`include/ymh/mcp/mcp_types.hpp:193`). The config loader keeps the struct
   default when the key is absent (`src/config/config.cpp:958-959`,
   `:1122-1123`).
6. The result is `Ask`; `PermissionBroker` emits a `permission.request` on
   every call, for every enabled server, forever.

The user's path rule is not broken. It is a path rule, and MCP has no path
dimension. The fallback default is the defect.

### 1.2 Issue 2: a denied MCP call is reported as success by the model

User report (paraphrased): "A denied MCP call correctly did not run, but the
LLM summary claimed success."

Verified cause, in the shipped tree:

1. On a Deny, the loop sets
   `denied.output = reason.empty() ? "permission denied" : reason`
   (`src/agent/agent_loop.cpp:961-970`).
2. A human Deny returns `PermissionOutcome{Deny, "user"}` from the broker
   (`src/permission/permission_broker.cpp:343`). `"user"` is an audit token, not
   prose.
3. The durable proof is correct: `outcome = ToolOutcome::Denied`, and
   `ToolRegistry::execute` is never called. But the *model-facing* projection
   sets the Tool message text to `payload.output` and drops outcome and error
   (`src/session/session.cpp:491-521`, specifically `:504`; pinned by
   `01-session.md:767-769`).
4. The model therefore receives a `Tool` message whose entire content is
   `"user"`. Nothing in that message says "denied", "not executed", or "no
   result". A model that summarizes the step can honestly claim nothing failed,
   because its evidence says nothing failed.

This is a feedback defect, not an execution defect. The tool did not run.

### 1.3 What this changes in one sentence

Enabled MCP servers default to Allow because their presence in the global
config is the user's consent, with per-server `default_verdict` and ordinary
tool rules as the narrowing surfaces, except that an explicit global
`permissions.default:"deny"` makes an implicit MCP default Deny; and every
Denied tool result carries explicit "DENIED / not executed / reason" text that
reaches the model, the export, and a `DENIED` UI badge.

### 1.4 Precedence and non-negotiables

- **MCP defaults do not touch the spec-89 guard.** `classify_shell_request`
  returns `NotApplicable` for a non-shell/non-terminal tool
  (`src/policy/shell_guard.cpp:2534-2537`), so L1 Forbid/Confirm, the ReadOnly
  hard deny, `force_ask`, protected-path denies, and the 90 target-class rules
  keep their exact order (`90 sec 6`). No MCP request acquires shell text.
- **MCP defaults do not bypass a hard deny.** `ReadOnly` still denies the
  tools named by `tool_is_mutating` (`src/policy/permission_policy.cpp:251-264`,
  `:444-446`); that predicate starts with `request.destructive`, so a
  `destructiveHint:true` MCP tool **is** denied under `ReadOnly` (the hint is
  read at `src/mcp/mcp_client.cpp:231-233`, copied at
  `src/mcp/mcp_manager.cpp:211`, and copied into the request at
  `src/agent/agent_loop.cpp:890-893`). `force_ask` still forces Ask
  (`:453-455`); protected paths still deny (`:457-471`).
- **The global `PermissionConfig::default_verdict` stays Ask, and an explicit
  global `Deny` is fail-closed for implicit MCP.** The per-server fallback is
  Allow by default, but when the user explicitly configures
  `permissions.default:"deny"` an absent per-server key yields Deny. An
  explicit per-server `allow`/`ask` or a matching rule is the deliberate
  override (93-MD11). 09 sec 3.5 is retained for tools without a matching
  `tool_defaults` entry.
- **Denied means not executed.** The loop's `plan.run = false` branch is
  unchanged (`src/agent/agent_loop.cpp:961-971`); no tool `execute` occurs.
- **Outcome integrity is absolute.** No projection may turn a Denied result
  into `Ok`, into empty success text, or into a bare audit token. The durable
  `ToolOutcome` remains the source of truth; the text is derived from it.
- **Denial text is honest, not magical.** It cannot force a model to write a
  truthful summary. It makes a success claim contradicted by the tool message.
  The spec claims exactly that and no more.

### 1.5 Non-goals

- No new permission UI, no new dialog option, and no per-call MCP confirmation
  beyond the existing Ask path when a user explicitly configures it.
- No change to MCP discovery, `allowed_tools`/`denied_tools`, tool naming,
  transport, timeouts, result caps, or the MCP failure taxonomy (specs 15/70).
- No change to `PermissionDecision.reason` tokens. `"user"` remains the durable
  audit token; only the `ToolResult` text is rendered.
- No new `path` semantics. A path rule still never matches a tool with no path
  dimension (90-D4).
- No general outcome-marker revival for Ok/Error/Cancelled results. Only Denied
  gains mandatory text; Error/Cancelled projection is unchanged (OQ-93-2).
- No wire-protocol change. The denial text travels as `ToolResult.output`, which
  already crosses replay, export, and the provider message channel.

## 2. Scope boundaries

**In scope.**

- `McpServerSettings::default_verdict` becomes
  `std::optional<std::string>`; its absence is distinct from an explicit value
  and lets an explicit per-server value override an explicit global Deny
  (93-MD2/MD3/MD11). `McpServerConfig::default_verdict` becomes `Allow` for the
  unread runtime declaration (93-MD1).
- Documentation-level config default and sample updates (21, 15).
- One new session text helper pair (`include/ymh/session/tool_result_text.hpp`,
  `src/session/tool_result_text.cpp`) with three projection callers
  (`session.cpp`, `session_export.cpp`, `ui_model.cpp`).
- The denial branch's `ToolResult.output`/`error` construction
  (`agent_loop.cpp`).
- One UI header badge in `render_tool_entry` (`ui_render.cpp`).
- Tests named in sec 13.

**Out of scope.**

- Sandboxing or confining MCP subprocesses (89-D18 remains a recorded
  non-mirror).
- Changing the shipped `destructiveHint` trust model or its ReadOnly mapping.
  The hint is already wired to `request.destructive` and `tool_is_mutating`
  (15 sec 6.1 calls it a hint); this spec retains that exact wiring and records
  its limitation (93-MD12). Inferring mutation from an MCP tool's name is not
  added.
- A global `mcp.default_verdict` key (OQ-93-1).
- Free-form denial feedback over `permission.decide` (OQ-93-3).
- Changing the `"user"` reason token or any protocol enum.

## 3. Amendment register

| Target | Clause | Disposition |
|---|---|---|
| 15-mcp-adapter.md sec 4.1 | `McpServerConfig::default_verdict{PolicyVerdict::Ask}` and the comment "Never Allow unless configured (M7, sec 6.1)" | **Superseded by 93-MD1/MD3.** The initializer becomes `PolicyVerdict::Allow`; the comment becomes the consent rule of sec 4.2. |
| 15-mcp-adapter.md sec 6.1 | "MCP tools default to `Ask`. There is no implicit `Allow` for a server." | **Superseded by 93-MD1/MD2.** The default becomes Allow because a server is defined only in the global layer by the user. The rest of M7 (no MCP-specific approval path) is retained. |
| 15-mcp-adapter.md sec 6.1 | "`destructiveHint` is a hint only. ... it never changes a verdict." | **Corrected by reference (M2).** The hint is wired into `request.destructive` (`src/mcp/mcp_client.cpp:231-233`, `src/mcp/mcp_manager.cpp:211`, `src/agent/agent_loop.cpp:890-893`), and `tool_is_mutating`/the ReadOnly hard deny act on it (`src/policy/permission_policy.cpp:251-263`, `:444-446`). It remains advisory everywhere else (93-MD10/MD12). |
| 15-mcp-adapter.md sec 6.1 / AM-3 | per-server `default_verdict` "(default `Ask`)" and the lowering into one `ToolDefault` | **Amended.** The absent-key implicit verdict becomes Allow (not a stored string), but the lowering is **presence-aware**: an absent key is Deny when the global `permissions.default` is `"deny"` (93-MD11), and an explicit `allow`/`ask`/`deny` is parsed as written below the master switch (93-MD3). |
| 15-mcp-adapter.md sec 4.1 | "`McpServerConfig::default_verdict` is input to the policy layer" | **Retained.** The effective path is still settings -> `to_permission_config`; the runtime field is kept consistent but is not read by `McpManager`. |
| 21-config-jsonc-errata.md sec 7 | Key table row `mcp.server.default_verdict` default `"ask"`; the two sample values | **Amended: absent lowers to Allow when the global default is not `"deny"`, and to Deny when `permissions.default:"deny"`** (93-MD2/MD3/MD11); it is not stored as a string. An explicit `"ask"`/`"allow"`/`"deny"` is validated exactly as today. |
| 09-permissions.md sec 3.5 | "Built-in default ... `ask` (fail-closed)" | **Retained for `PermissionConfig::default_verdict`.** MCP's per-server fallback is the 15 AM-3 NO-MATCH tier, not the global built-in default (93-MD8). An explicit global Deny additionally vetoes an implicit MCP Allow (93-MD11); the generic rule fold is unchanged. |
| 09-permissions.md sec 3.4 | Rule fold and `tool_defaults`/`default_verdict` order | **Retained, unchanged.** The MCP entry's value changes; its precedence does not (93-MD5). |
| 46-permissions-ui-errata.md 46-D1.3 | Master switch emits every MCP `tool_defaults` entry with Allow | **Retained** (93-MD6). `permissions.default = "allow"` still wins over a per-server `ask` **or `deny`**; a rule deny remains the escape hatch. |
| 46-permissions-ui-errata.md 46-D1.2/46-D1.6 | A rule deny outranks the master switch | **Retained** (93-MD5). A rule is the documented way to deny an MCP tool under the master switch. |
| 90-path-scoped-permissions-errata.md 90-D4/sec 4.5 | A path rule never matches a tool with no path dimension | **Retained** (93-MD4). The user's `{tool:"*",path:"**"}` rule stays a path rule and does not match MCP. |
| 90-path-scoped-permissions-errata.md 90-D18 | The whole `permissions` table is global-layer only | **Retained and used as the consent anchor** (93-MD7). A workspace cannot add MCP servers or permission rules. |
| 50-skills-mcp-and-session-lifecycle-errata.md 50-D4/50-I22 | `mcp`/`mcp_servers` are global-layer only | **Retained and used as the consent anchor** (93-MD7). |
| 01-session.md sec 6.3 | `ToolResult` projects as `Message{Role::Tool, tool_call_id, content = payload.output}` | **Amended for Denied** (93-DF5/DF6): the content is `tool_message_content(result)`, which is `output` for Ok and canonical denial text for Denied, including legacy repair. |
| 01-session.md I12 | Every `tool_use` has a `ToolResult`; a denied call yields `Denied` | **Retained.** The text requirement is added, not the pairing rule. |
| 06-agent-loop.md A-F6/sec 5.5 | Deny -> `ToolResult{Denied}`; turn continues; execute skipped | **Retained.** 93-DF1 adds the output contract. |
| 25-ui-ux-errata.md 25-D4 | Deny -> `ToolResult{Denied, output=<reason>}` | **Superseded in shape:** `output` is the canonical denial text that contains the reason, not the raw token (93-DF1/DF2). The wire still carries no free-form review feedback, so 25-D4's "review feedback" wording is not made true by this errata (OQ-93-3). |
| 17-ui-transcript-errata.md sec 4 item 4, F-04, F-10 | Collapsed tool entry is exactly `tool: <name>`; no outcome marker | **Amended for Denied only** (93-DF7): the header carries the ASCII token `DENIED` before the name. No other outcome/truncation marker is revived. |
| 07-tools-execution.md sec 8.3 | A tool never returns `Denied`; the loop produces it | **Retained, unchanged.** |
| 89-shell-command-safety-guard-errata.md sec 4.1/sec 6 | L1/L2 order | **Retained, unchanged** (93-MD10). |

## 4. Workstream A: MCP defaults

### 4.1 The shipped fold, with the failing branch highlighted

```text
evaluate(request{ tool = "mcp.github.search", path = nullopt }):
  0. temp-root exemption          -> no (not shell/terminal)
  1. L1 Forbid                    -> NotApplicable (no shell text)
  2. ReadOnly + mutating          -> no (sample request.destructive == false)
  3. L1 Confirm                   -> no
  4. force_ask                    -> no
  5. protected path               -> no
  6. target_class                 -> Any (no path dimension; not shell/terminal)
  7. candidates = base_rules + session grants
       {tool:"*", path:"**"}      -> rule_matches false (no path dimension)
       ...                          -> no winner
  8. tool_defaults:
       prefix "mcp.github."       -> verdict = parse(server.default_verdict)
                                     = Ask  <-- the defect
  9. return Ask
```

The path rule is behaving exactly as 90-D4 pins. The correct place to fix "MCPs
should not ask by default" is the server default, not the path matcher.

### 4.2 The consent model (why default Allow is safe here)

An MCP tool call is not like an arbitrary model-authored shell command. The set
of MCP tools is fixed before the model acts:

- A server exists only if the user wrote it into a config file.
- MCP server definitions are **global-layer only** (50-D4/50-I22): a workspace
  `.ymh/config.jsonc` containing `mcp`/`mcp_servers` is a `ConfigError`, so a
  cloned or agent-written workspace cannot introduce a server.
- The permission table is also **global-layer only** (90-D18): a workspace
  cannot add an allow/deny rule or change `permissions.default`.
- The server must be `enabled` (default true) and must pass startup validation;
  a server that fails to start contributes no tools.
- `allowed_tools`/`denied_tools` filter the catalog before any call (15 M3).

Therefore "the user added and enabled this server in their own global config"
is the consent event. Asking again on every call is redundant friction and is
the exact behavior the user reported as broken. This is a deliberate trust
decision, not an implicit grant to unknown code: the user still decides which
servers exist, and can lower the trust per server.

The consent event is only as strong as the global config's integrity: path
tools cannot resolve outside the workspace root (07 X3), and a shell write to
`~/.config/ymh/config.jsonc` is denied by L2 when Landlock is enforced and the
config directory is outside the root. The caveat is a workspace rooted at (or
above) the config directory: launching in `$HOME` puts `~/.config/ymh/` inside
the root, where root-confined tools can write it. That is a pre-existing
property of working in `$HOME`, recorded in sec 14; the normal project-root case
keeps the config outside the root. The 89 L2-unavailable residual is unchanged
and is not created by this spec.

This does not claim confinement. An MCP server is still an arbitrary external
process or remote endpoint; 89-D18's non-mirror stands. Default Allow means "run
the tool without a prompt", not "confine the tool".

### 4.3 The pinned default and the config override

**93-MD1 - `McpServerConfig::default_verdict` defaults to Allow.**
`include/ymh/mcp/mcp_types.hpp:193` changes from
`PolicyVerdict default_verdict{PolicyVerdict::Ask};` to
`PolicyVerdict default_verdict{PolicyVerdict::Allow};`. A comment is added to
the header field stating the consent rule and naming the explicit override; the
spec-15 sketch comment is superseded by sec 4.2. `McpManager` still never reads
this field; the change keeps the declared runtime default from contradicting the
effective config default.

**93-MD2 - `McpServerSettings::default_verdict` is an absent optional that
lowers to Allow.**
`include/ymh/config/config.hpp:330` changes from
`std::string default_verdict = "ask";` to
`std::optional<std::string> default_verdict = std::nullopt;`. The absence is the
new default: `to_permission_config` lowers it to Allow under a non-deny global
default and to Deny under an explicit global Deny (93-MD11). It is not stored as
a `"allow"` string, so presence is unambiguous.

**93-MD3 - The per-server override is the existing key, not a new one.**
`mcp.server[].default_verdict` / `mcp_servers.<id>.default_verdict` remains a
string validated by `parse_verdict` (`src/cli/wiring.cpp:170`,
`src/cli/wiring.cpp:236-237`). Values:

```jsonc
"default_verdict": "allow"  // explicit allow: run without a prompt
"default_verdict": "ask"    // keep the per-call dialog for this server
"default_verdict": "deny"   // never execute this server's tools; no dialog
```

Presence is structural. `McpServerSettings::default_verdict` becomes
`std::optional<std::string>` with `std::nullopt` as the absent value; the
loaders assign it only when the key is present
(`src/config/config.cpp:941-959`, `:1091-1123`), including the localcode import
path that builds an entry for `apply_mcp_servers_entry`
(`src/config/config.cpp:2129`). An absent key is not the same as a key set to
`"allow"`:

- `std::nullopt` + global `permissions.default` not `"deny"` -> Allow (the new
  default).
- `std::nullopt` + global `permissions.default:"deny"` -> Deny (93-MD11,
  fail-closed).
- A present value -> `parse_verdict(*value)` below the master switch (93-MD6).
  An explicit `"allow"` is the only per-server way to auto-run under a global
  Deny; an explicit `"ask"` asks and never auto-runs.

A programmatic `Config` opts into an explicit value by assigning the optional;
leaving it `std::nullopt` is unambiguously the implicit case. The optional is
in-memory only; it is not serialized and has no user-facing key.

**93-MD4 - No new config key.** `mcp.default_verdict`, `trust`, and similar
names are not introduced. The per-server key already exists and is the smallest
surface that satisfies "a config override".

**93-MD5 - The rule fold is unchanged.**
`evaluate` still returns, in order: a matching rule winner (specificity ->
layer -> effect), else the longest matching `tool_defaults` prefix, else
`config_.default_verdict` (`src/policy/permission_policy.cpp:490-540`). An
operator rule for `mcp.github.search` or `mcp.github.*` therefore outranks the
server default, exactly as 15 AM-3 pins.

**93-MD6 - The master switch still wins.**
`permissions.default = "allow"` (or the deployment baseline
`approval == "never"`) sets `master = true`, and `to_permission_config` emits
every MCP `tool_defaults` entry with Allow
(`src/cli/wiring.cpp:146-147`, `:169-170`; 46-D1.3). A per-server `"ask"` **or
`"deny"`** is ignored under the master switch, by design: the user asked for
"never ask". A rule deny still wins (46-D1.2/46-D1.6), and a `permissions.rules`
deny is the documented way to keep one MCP tool denied under the master switch.

**93-MD7 - Consent is structural, not a flag.**
The default is safe because of 50-D4 (MCP definitions global-only) and 90-D18
(`permissions.*` global-only). If either guard is ever relaxed, this spec's
premise fails and 93-MD1/MD2 must be re-reviewed before that relaxation ships.

**93-MD8 - The global default stays Ask, but an explicit global Deny is a
fail-closed veto for implicit MCP.**
`PermissionConfig::default_verdict` remains `Ask` (09 sec 3.5), and the MCP
entry is still a `tool_defaults` NO-MATCH tier for every other value. The one
special case is a global `Deny`: an absent per-server key is lowered to Deny
(93-MD11), so `permissions.default:"deny"` cannot silently become automatic
execution. A tool whose name does not start with an enabled server's
`mcp.<id>.` prefix is unaffected by this special case.

**93-MD9 - Disabled/empty servers contribute nothing.**
`to_permission_config` keeps the guard
`if (!server.enabled || server.id.empty()) continue;`
(`src/cli/wiring.cpp:165-167`). A disabled server therefore does not create an
Allow entry; if its name somehow reaches the policy, the global default applies.

### 4.4 Evaluation after the change

```text
lower_mcp_fallback(server, global_default, master):
  if master:                         return Allow       // 46-D1.3
  if server.default_verdict.has_value()  return parse(*server.default_verdict)
  if global_default == Deny:         return Deny        // 93-MD11
  return Allow                                          // 93-MD2

evaluate(request{ tool = "mcp.github.search", path = nullopt }):
  ... (steps 0-7 unchanged; no rule matches) ...
  8. tool_defaults: "mcp.github." -> lower_mcp_fallback(...)
  9. return that verdict
```

With the ordinary global config (`permissions.default:"ask"`), an absent
per-server key returns Allow and no `permission.request` is emitted. With
`"default_verdict":"ask"` the dialog path runs unchanged. With `"deny"` the loop
records `ToolResult{Denied}` and never executes. With an explicit global
`permissions.default:"deny"` and no server key, step 8 returns Deny; an explicit
server `"allow"` or an allow rule is required to run. A matching
`{"tool":"mcp.github.delete","effect":"deny"}` rule at step 7 still produces a
Deny that outranks the fallback.

### 4.5 Rule interaction table

`master` means `permissions.default == "allow"` or the deployment baseline
`approval == "never"` (46-D1.3). Except for the rule rows, a master verdict of
Allow overrides the listed per-server value.

| Configuration | `mcp.github.search` verdict | Why |
|---|---|---|
| Enabled server, no key, global default `ask` | Allow | implicit default (93-MD1/MD2) |
| Enabled server, no key, global default `deny` | Deny | fail-closed veto (93-MD8/MD11) |
| Enabled server, explicit `"allow"`, global default `deny` | Allow | explicit per-server override (93-MD3/MD11) |
| Enabled server, explicit `"ask"`, global default `deny` | Ask | explicit per-server ask; never auto-runs (93-MD3) |
| Enabled server, explicit `"deny"` | Deny, unless master | explicit override (93-MD3); master forces Allow (93-MD6) |
| Enabled server, explicit `"ask"` | Ask, unless master | explicit override; master forces Allow (93-MD6) |
| Disabled server | not registered; no entry | 93-MD9 |
| `permissions.default:"allow"`, no key | Allow | master switch (93-MD6) |
| `permissions.default:"allow"`, server key `"ask"` or `"deny"` | Allow | master switch overrides both (93-MD6) |
| `permissions.rules:[{tool:"mcp.github.*",effect:"deny"}]` | Deny | rule fold outranks the fallback and the master (93-MD5) |
| `permissions.rules:[{tool:"mcp.github.*",effect:"allow"}]`, global default `deny`, no server key | Allow | rule fold outranks the fallback (93-MD5) |
| `permissions.rules:[{tool:"*",path:"**",effect:"allow"}]` | Default verdict, not the rule | path rule does not match MCP (93-MD4) |
| `permissions.rules:[{tool:"*",path:"**",effect:"deny"}]` | Default verdict, not Deny | the path rule does not match MCP; it does not deny it (93-MD4). Use a tool rule. |

The last two rows are the surprising ones. They are pinned explicitly so a
future "fix" does not make `path:"**"` mean "any tool". The fail-closed
guarantee under a global Deny is about automatic execution: an absent key is
Deny, and an explicit `"ask"` can only ask; only an explicit `"allow"` or a
matching allow rule yields Allow.

### 4.6 Migration and user-visible change

Two behavior changes apply to existing global configs that enable an MCP server
and omit `default_verdict`:

- With the ordinary `permissions.default:"ask"` (or any non-`deny` value),
  those calls stop asking. This is the requested fix.
- With an explicit `permissions.default:"deny"`, those calls now stay denied
  (they previously asked). This is the fail-closed fix (93-MD11). To run an MCP
  tool under a global Deny, write an explicit per-server `"allow"` or a
  matching allow rule; to keep the old dialog, write an explicit per-server
  `"ask"`.

There is no state migration: the value is derived at startup. The optional is
in-memory and has no config key; an existing config with no `default_verdict` is
treated as absent (`nullopt`), not as an explicit allow.

Users who want the old behavior write `"default_verdict": "ask"` on each
server. Users who want to deny one server (without a global deny) write a rule:

```jsonc
{
  "permissions": {
    "rules": [
      { "tool": "mcp.untrusted.*", "effect": "deny", "id": "mcp.untrusted.deny" }
    ]
  }
}
```

Users who want "ask for everything except MCP" keep
`permissions.default:"ask"`; MCP is already Allow because its server entry is
the trust decision. Users who want a global fail-closed baseline write
`permissions.default:"deny"` and opt in per server:

```jsonc
{
  "permissions": { "default": "deny" },
  "mcp": {
    "server": [
      { "id": "github", "command": "npx", "default_verdict": "allow" }
    ]
  }
}
```

Users who want "ask for MCP too" set the server key, and can then allow selected
tools with a rule:

```jsonc
{
  "permissions": {
    "rules": [
      { "tool": "mcp.github.search", "effect": "allow", "id": "mcp.github.search" }
    ]
  }
}
```

The path-scoped rule in the 90 sample does not do this: it is a path rule and
MCP has no path. Both snippets belong in the global `config.jsonc`; a workspace
`.ymh/config.jsonc` containing `permissions` is a `ConfigError` (90-D18).

The live 21 JSONC key table and sample are updated by this spec's
supersession; 21 currently shows `"default_verdict": "ask"` in the key table and
in both examples. The 15 config sketch is a historical TOML illustration; its
prose default is superseded here, but 21 is the live config format.

### 4.7 What default Allow does and does not claim

Claimed:

- No `permission.request` is emitted for an enabled MCP server's tool under the
  default configuration.
- The tool then executes through the normal registry path with the normal
  timeouts/result caps.
- An explicit `ask`, an explicit `deny`, a rule deny, `force_ask`, a hard deny,
  or the master switch all retain their existing precedence.
- An explicit `ask` still uses the generic dialog and grant path. Because an
  MCP tool has no path dimension, an "Allow for session"/"Always" answer is
  tool-wide (90-D9), not path-scoped; there is no per-argument grant for MCP.
- Under `ReadOnly`, a `destructiveHint:true` MCP tool is denied by the existing
  `request.destructive` -> `tool_is_mutating` path (93-MD10), not allowed by the
  new default.

Not claimed:

- MCP confinement (89-D18), filesystem rooting of the server process (15 sec
  6.2), or network egress control. An MCP server can do whatever its own
  process can do; default Allow means the user's configured server is trusted
  to do it without a per-call prompt.
- A complete mutation guarantee. `destructiveHint` is advisory (15 sec 6.1); a
  server can omit it or lie. A non-hint MCP tool can run under `ReadOnly` and
  mutate state. This spec retains the shipped hint mapping and does not infer
  mutation from names (93-MD12).

## 5. Workstream B: denied-tool feedback

### 5.1 The shipped trace, and why the model saw success

```text
user Deny
  -> PermissionBroker::onDecision -> promise.set_value({Deny, "user"})
     (src/permission/permission_broker.cpp:343)
  -> AgentLoop denial branch:
       denied.outcome = Denied
       denied.output  = reason.empty() ? "permission denied" : reason
       denied.error   = denied.output
       plan.run = false
     (src/agent/agent_loop.cpp:961-971)
  -> durable ToolResult{Denied, output="user", error="user"}
  -> deriveMessages ToolResult case:
       Message{Role::Tool, tool_call_id, content = payload.output}
     (src/session/session.cpp:497-505)
  -> provider sees a Tool message whose whole content is "user"
```

The durable record is correct. The projection is the defect: it drops
`outcome`, `error`, and the fact that the tool never ran. The next assistant
message is generated from that evidence.

### 5.2 Canonical denial text

**93-DF1 - The Denied `ToolResult.output` is canonical denial text.**
The loop builds it with `denial_text(call.name, reason)` instead of copying the
token. The exact ASCII form (one line, no newline):

```text
DENIED: tool '<name>' was NOT executed; permission was not granted (reason: <detail>). The tool did not run and produced no tool output; do not report this call as successful.
```

Properties, all pinned:

- Starts with the literal `DENIED:` marker.
- Contains the literal `NOT executed` and `was not granted`.
- Contains the tool name, bounded and sanitized (93-DF12).
- Contains a rendered reason (93-DF2).
- Contains the no-success sentence.
- Is at most `kMaxDenialTextBytes` (1024) bytes, ASCII, no control characters.
- Contains no model arguments, no prompt text, no file bodies, no environment
  values (93-DF13).

**93-DF2 - The reason is rendered, not echoed as a bare token.**
`denial_detail(reason)` maps the known audit tokens:

| `reason` from the decision | rendered detail |
|---|---|
| empty | `unspecified` |
| `user` | `the user denied it` |
| `timeout` | `permission request timed out` |
| `cancelled` | `request cancelled` |
| `shutdown` | `daemon shut down` |
| `transport` | `transport failure` |
| `no-subscribers` | `no attached client` |
| `internal` | `internal error` |
| `no-resolver` | `no permission resolver attached` |
| `rule` (the broker's fallback when a policy deny has no reason) | `denied by a permission rule` |
| `permission:protected-path` | `protected path` |
| anything else (a custom resolver token, `shell-guard:...`, `denied by policy`, ...) | the token itself, sanitized and bounded |

`no-resolver` is the headless/daemon resolver token
(`src/cli/headless.cpp:190`, `src/host/workspace_host.cpp:896`); the no-gate loop
uses the prose `"no permission resolver attached"`
(`src/agent/agent_loop.cpp:939-941`). Both render as the same detail; neither is
a dead row (L3).

The mapping is a display projection. The durable
`PermissionDecision.reason` remains exactly the original token
(`src/permission/permission_broker.cpp:343`; 09 sec 4.2). The audit trail does
not change, and `permission_broker_test.cpp`'s `"user"` assertion remains valid.

**Policy rule denies do not carry a rule id.** `RulePermissionPolicy::evaluate`
returns `winner->effect` and never writes the winner's id into `*reason`
(`src/policy/permission_policy.cpp:490-542`; only hard denies set a token).
The no-gate loop therefore records `"denied by policy"`
(`src/agent/agent_loop.cpp:928-930`), and the broker's no-reason fallback is
`"rule"` (`src/permission/permission_broker.cpp:227-233`). This spec renders
those as `denied by policy` and `denied by a permission rule`; it does **not**
change the token to a rule id (93-DF9, sec 1.5). The test plan asserts the
shipped strings, not a rule id.

**93-DF3 - `error` mirrors the canonical text.**
The loop sets `denied.error = denied.output` as today
(`src/agent/agent_loop.cpp:967`). No second prose variant exists.

**93-DF4 - The canonical text is constructed once, at the decision.**
New events store the canonical text durably. Projections never rewrite a
canonical output; they only repair legacy bare-token outputs (93-DF6).

### 5.3 Model-facing projection

**93-DF5 - The provider Tool message must carry the denial.**
`deriveMessages`'s `ToolResult` case changes from `block.text = value.output` to
`block.text = tool_message_content(value)` (`src/session/session.cpp:504`).
`tool_message_content` is total:

```text
tool_message_content(result):
  if result.outcome != Denied: return result.output        // Ok/Error/Cancelled unchanged
  if is_canonical_denial(result.output):
      return result.output
  reason := first non-empty of (result.output, result.error, "")
  return denial_text(result.name, reason)
```

For a new Denied event, the helper returns the durable output unchanged.
For a legacy event whose output is `"user"` (or empty, or a raw policy token),
the helper synthesizes the canonical text at projection time (93-DF6). This is a
pure projection synthesis in the same spirit as I12's cancelled/failed pairing:
it never appends to the log and never mutates the durable event
(`01-session.md:526-535`). Replay is deterministic because the function is pure.

`Error` and `Cancelled` are intentionally unchanged: their existing outputs are
already tool-authored diagnostics, and changing them would break provider
goldens and is outside this defect (OQ-93-2).

### 5.4 UI rendering

**93-DF7 - A Denied tool row renders an explicit `DENIED` badge.**
`render_tool_entry` today adds a red cross for any non-Ok outcome
(`src/ui/ui_render.cpp:349-352`). That is not enough: a red cross is not the
word "DENIED", and the collapsed row does not distinguish denied from error.
The header gains, for `ToolOutcome::Denied` only:

```cpp
if (call != nullptr && call->outcome == payload::ToolOutcome::Denied) {
    header.push_back({"DENIED ", ftxui::Color::Red, true});
}
```

It is inserted after the existing non-Ok glyph and before the tool name, so the
collapsed row contains the token `DENIED` immediately before the tool name. The
ASCII token `DENIED` is the pinned text; existing styling is reused. The rest of
the row (glyphs, name, bounded argument summary) keeps the shipped layout, so
17's older `tool: <name>` string is not rewritten beyond this insertion.
`Error`/`Cancelled` keep the current cross and gain no new label.

**93-DF8 - The UI model and the export use the canonical text.**
`UiModel::apply(ToolFinished)` already copies `e.output`. For
`e.outcome == Denied` it sets both the `ToolCallView.output` and the matching
`ConversationEntry.text` from `tool_message_content`, so a legacy event's bare
`"user"` is not displayed as the denial body. New events are already canonical.
This is the only UI-model change; no new durable state.
`session_export` already labels the outcome `denied`
(`src/ui/session_export.cpp:65-77`, `:227-239`); its body changes from
`code_block(result.output)` to `code_block(tool_message_content(result))`, so a
legacy export also carries the explicit denial. The exporter additionally
suppresses the `_Error: ..._` line when `result.outcome == Denied` and
`result.error == result.output` (the new-event shape), so the canonical sentence
appears once, not twice (`src/ui/session_export.cpp:234-237`).

### 5.5 Outcome integrity: no summary path may imply success

**93-DF9 - Denied is never collapsed to Ok.** The durable `outcome` set by the
loop is final; no projection, export, UI, or summary code may map it to `Ok`,
drop it, or replace the output with an empty string. The permission decision,
the `ToolResult`, and the tool-message content are consistent by construction:
`Denied` -> canonical text; `Ok` -> tool output; `Error`/`Cancelled` ->
unchanged.

**93-DF10 - The model-facing message is the only summary input.** A later
assistant message (including a user-visible summary) is generated from the
projected messages. It therefore sees "DENIED ... NOT executed ... do not
report this call as successful" instead of `"user"`. The spec does not claim
control over the model's prose; it claims that a success claim is contradicted
by the tool message in the same context window. Nothing in ymh may additionally
mark the turn as a success on the strength of a Denied tool: `TurnEnded` still
means the turn ended, not that every tool succeeded, and the UI/export keep the
Denied outcome.

**93-DF11 - The tool really did not run.** This is already true and is retained:
the loop sets `plan.run = false` and records the Denied result without calling
`ToolRegistry::execute` (`src/agent/agent_loop.cpp:961-971`; 07 sec 8.3, 09
Q4). The denial text's "NOT executed" sentence is therefore true, not a
heuristic.

Other summary surfaces are already outcome-aware: `handoff.cpp`'s facts loop
branches on `outcome != Ok` and records the tool under errors
(`src/agent/handoff.cpp:467-479`), so it cannot imply success and needs no
change; new events supply the canonical error text there, legacy events supply
the old token as a short error detail. The transcript, export, and provider
message are the surfaces this spec repairs.

### 5.6 Legacy sessions

A session recorded before this change may contain
`ToolResult{Denied, output="user"}` (or another bare token). Replay must not
reproduce the misleading message. The projection helper repairs it:

- `deriveMessages` emits the canonical text (93-DF5).
- The UI model sets the call/entry text to the canonical text (93-DF8).
- The export emits the canonical text (93-DF8).
- The durable event is untouched; a later tool or a DB reader still sees the
  original fields. The repair is display-only and deterministic.

There is no backfill, no schema change, and no migration. A resumed legacy
session sends the repaired Tool message to the provider on the next turn.

### 5.7 Bounds, ASCII, and redaction

**93-DF12 - The text is bounded and ASCII.**
`bounded_ascii(text, cap)` keeps bytes in `[0x20, 0x7e]`, replaces every other
byte (including newline, tab, ESC, and UTF-8 continuation bytes) with `?`, and
truncates to `cap` bytes, appending `...` when truncated. Tool names are
bounded to `kMaxDenialToolBytes = 128`; reasons to
`kMaxDenialReasonBytes = 256`; the assembled text to
`kMaxDenialTextBytes = 1024`. A truncated reason still leaves a syntactically
complete sentence (the closing `).` is appended by `denial_text`, not copied
from the reason). The component caps (128 + 256 + the fixed literals) keep the
assembled sentence below 1024, so the final truncation branch is defensive and
never fires in the pinned configuration; the no-success sentence therefore
always ends the text. A durable output is passed through unchanged only when
`is_canonical_denial` also confirms the size cap and every byte is printable
single-line ASCII; a control character, newline, or ESC forces a rebuild
(93-DF5/L1).

**93-DF13 - No arguments or secrets in the denial text.** `denial_text` takes
only the tool name and the reason. It never receives
`PermissionRequest.arguments`, the prompt, file bodies, or environment values. A
64 KiB argument body cannot leak into the denial text or the provider Tool
message. This is a claim about the denial text, not about the markdown export:
`session_export` separately prints the full `ToolCall.arguments` for every call
(`src/ui/session_export.cpp:222-224`), including a denied call, and that
pre-existing surface is unchanged, not redacted, and recorded in sec 14.

## 6. Decision register

**93-MD1 - Runtime MCP default is Allow.**
`McpServerConfig::default_verdict` (`include/ymh/mcp/mcp_types.hpp:193`) becomes
`PolicyVerdict::Allow`. The field is a declaration; `McpManager` does not read
it. Keeping it consistent with the config default prevents a future reader from
re-introducing Ask.

**93-MD2 - Config MCP default is absent, lowering to `"allow"`.**
`McpServerSettings::default_verdict` (`include/ymh/config/config.hpp:330`)
becomes `std::optional<std::string>` with `std::nullopt` as the absent default;
`to_permission_config` lowers it to Allow normally and to Deny under a global
Deny (`src/cli/wiring.cpp:163-173`).

**93-MD3 - The override is the existing per-server key, represented as an optional.**
`default_verdict` remains accepted in both the array form and the legacy
`mcp_servers` object form (`src/config/config.cpp:941-959`, `:1091-1123`) and is
parsed by `parse_verdict` (`src/cli/wiring.cpp:170`, `:236-237`). No new user
key. `McpServerSettings::default_verdict` becomes
`std::optional<std::string>` (`std::nullopt` = absent); both loaders assign it
only when the key is present, and `to_permission_config` uses
`has_value()` to distinguish an implicit default from an explicit `"allow"`
(93-MD11).

**93-MD4 - Path rules do not match MCP tools.**
`rule_matches`'s final `else { return false; }` for a path rule on a no-path tool
(`src/policy/permission_policy.cpp:169-185`) is retained and pinned. An MCP tool
has no path dimension; `path:"**"` is not a wildcard for "any tool".

**93-MD5 - Rules outrank the server default.**
The fold is unchanged (`src/policy/permission_policy.cpp:490-540`). A matching
rule winner beats `tool_defaults`. This is the way to deny one MCP tool under
the master switch or to ask for one tool while the server default is allow.

**93-MD6 - The master switch is unchanged.**
`master` still forces every MCP `tool_defaults` entry to Allow
(`src/cli/wiring.cpp:146-147`, `:169-170`; 46-D1.3), overriding an explicit
per-server `"ask"` or `"deny"` as well. A rule deny still beats it
(46-D1.2/46-D1.6). This is pinned so the fix does not silently change the
"never ask" switch.

**93-MD7 - Consent is the global-layer server definition.**
The default is justified by 50-D4/50-I22 (MCP definitions global-layer only)
and 90-D18 (`permissions.*` global-layer only). If either becomes layering-
permissive, this default must be re-reviewed.

**93-MD8 - The global default stays Ask; an explicit global Deny vetoes implicit MCP.**
`PermissionConfig::default_verdict` is still `Ask` for non-MCP tools. The MCP
entry is a `tool_defaults` fallback, not the global default (09 sec 3.5). The
one special case is an explicit global `Deny`: an absent per-server key is
lowered to Deny (93-MD11), so a fail-closed config is not silently opened.

**93-MD9 - Disabled servers have no entry.**
The existing guard (`src/cli/wiring.cpp:165-167`) is retained. The
subsystem-wide `mcp.enabled=false` is not consulted by `to_permission_config`,
so per-server entries may remain in the table; no MCP tools are registered in
that case, so no request can match them. This is pre-existing and has no
behavioral effect.

**93-MD10 - No guard change; ReadOnly still covers destructive-hint MCP tools.**
`classify_shell_request` returns `NotApplicable` for MCP
(`src/policy/shell_guard.cpp:2534-2537`), so 89 L1/L2, 90 protected paths, and
the 89/90 order are untouched. The ReadOnly hard deny is also untouched:
`request.destructive` is set from the tool schema
(`src/agent/agent_loop.cpp:890-893`), which for an MCP tool comes from the
server's `destructiveHint` (`src/mcp/mcp_client.cpp:231-233`,
`src/mcp/mcp_manager.cpp:211`); `tool_is_mutating` returns true for
`request.destructive` and `evaluate` denies it under ReadOnly
(`src/policy/permission_policy.cpp:251-263`, `:444-446`). Only a
non-destructive-hint MCP tool can pass the ReadOnly check by default.

**93-MD11 - An explicit global Deny vetoes an implicit MCP Allow.**
`to_permission_config` lowers an enabled server's fallback as:
`master ? Allow : has_value ? parse_verdict(*key) : (global_default == Deny
? Deny : Allow)` (`src/cli/wiring.cpp:146-147`, `:163-173`). The presence check
is the optional from 93-MD3. This is the only change to the lowering; the fold
above it is untouched, so a matching allow rule still wins over the Deny, and a
matching deny rule still wins over everything. The rule is fail-closed: no
automatic execution under an explicit global Deny unless the user wrote an
explicit per-server `"allow"` or an allow rule. An explicit per-server `"ask"`
is honored and asks; it does not auto-run.

**93-MD12 - The `destructiveHint` trust stance is retained, not strengthened.**
The hint is advisory (15 sec 6.1). The shipped wiring already turns it into
`request.destructive`, which is enough for the ReadOnly hard deny but is not a
mutation guarantee (a server can omit or lie about the hint). This spec keeps
that exact behavior; it does not treat every MCP tool as mutating (which would
make ReadOnly block all MCP tools) and does not infer mutation from tool names.
The limitation is recorded in sec 14.

**93-DF1 - Denied output is canonical denial text.**
`denial_text(name, reason)` produces the exact form in sec 5.2; the loop assigns
it to `denied.output` instead of the raw reason
(`src/agent/agent_loop.cpp:966`).

**93-DF2 - Reasons are rendered through a fixed token map.**
`denial_detail` maps the known audit tokens and passes unknown tokens through
sanitized/bounded (sec 5.2). The durable decision token is unchanged.

**93-DF3 - `error` mirrors the canonical text, but the export does not duplicate it.**
`denied.error = denied.output` is retained (`src/agent/agent_loop.cpp:967`), so
a consumer that reads only `error` still sees the denial. `session_export`
suppresses the `_Error:` line when `outcome == Denied && error == output`, so the
markdown shows the sentence once.

**93-DF4 - The canonical text is constructed once, at the decision.**
New events store the canonical text durably. Projections never rewrite a
canonical output; they only repair legacy bare-token outputs (93-DF6).

**93-DF5 - The provider Tool message is repaired by construction.**
`tool_message_content` is applied in `deriveMessages`
(`src/session/session.cpp:504`), so every Denied result has an explicit denial
in the model's context. A durable output is passed through only when
`is_canonical_denial` confirms the marker, the `was NOT executed` clause, the
size cap, and printable single-line ASCII; otherwise it is rebuilt (93-DF12).

**93-DF6 - Legacy Denied events are repaired at projection.**
The helper rebuilds the canonical sentence when the durable output lacks the
`DENIED:` prefix or the `was NOT executed` clause. This is pure and does not
mutate the log.

**93-DF7 - The UI renders `DENIED`.**
`render_tool_entry` adds the ASCII `DENIED` badge for
`payload::ToolOutcome::Denied` (`src/ui/ui_render.cpp:349-360`).

**93-DF8 - The UI model and export use the same helper for legacy bodies.**
`UiModel::apply(ToolFinished)` and `session_export` call
`tool_message_content`, so all user-facing projections agree with the model. The
export suppresses a duplicate `_Error:` line for the new Denied shape
(93-DF3).

**93-DF9 - No coercion to Ok, and no other summary rewrite.**
The outcome is passed through everywhere. The only new text is the Denied
projection. `TurnEnded` semantics are unchanged.

**93-DF10 - The model-facing message is the only summary input.**
A later assistant summary is generated from the projected messages; a success
claim is therefore contradicted by the Denied tool message in the same context
window. No ymh code may mark the turn or the tool as a success on the strength
of a Denied outcome.

**93-DF11 - Denied means the tool did not run.**
This is retained from the loop: `plan.run = false` and
`ToolRegistry::execute` is skipped (`src/agent/agent_loop.cpp:961-971`).

**93-DF12 - The text is bounded and ASCII.**
`bounded_ascii` keeps printable ASCII, replaces other bytes with `?`, and
truncates with `...`; the caps in sec 5.7 keep the sentence under 1024 bytes.

**93-DF13 - No arguments or secrets in the denial text.**
`denial_text` receives only the tool name and the reason; it never receives
`PermissionRequest.arguments`, the prompt, file bodies, or environment values.
The export's separate `ToolCall.arguments` block is unchanged and outside this
claim (sec 14).

## 7. New symbols and their callers

| Symbol | Kind / header | Body / contract | Caller(s) in the tree it lands in |
|---|---|---|---|
| `McpServerConfig::default_verdict` | field, `include/ymh/mcp/mcp_types.hpp` | `PolicyVerdict{PolicyVerdict::Allow}` | no runtime read; `to_mcp_config` overwrites it from settings (`src/cli/wiring.cpp:236-237`), and `McpManager` does not read it (15 sec 4.1) |
| `McpServerSettings::default_verdict` | `std::optional<std::string>`, `include/ymh/config/config.hpp` | `std::nullopt` = absent; a value is explicit | `to_permission_config` (`src/cli/wiring.cpp:169-170`); both config loaders (`src/config/config.cpp:958-959`, `:1122-1123`) |
| `kDeniedToolMarker` | `inline constexpr std::string_view`, `include/ymh/session/tool_result_text.hpp` | `"DENIED:"` | `tool_message_content` |
| `kDeniedNotExecuted` | `inline constexpr std::string_view` | `"was NOT executed"` | `tool_message_content` |
| `kMaxDenialToolBytes` | `inline constexpr std::size_t` | `128` | `denial_text` |
| `kMaxDenialReasonBytes` | `inline constexpr std::size_t` | `256` | `denial_detail` |
| `kMaxDenialTextBytes` | `inline constexpr std::size_t` | `1024` | `denial_text` |
| `std::string bounded_ascii(std::string_view, std::size_t)` | free function, header above | replaces non-printables with `?`; truncates to the cap with `...`; total and pure | `denial_detail`; `denial_text` |
| `bool is_canonical_denial(std::string_view) noexcept` | free function, header above | true iff marker + `was NOT executed` + size cap + every byte printable ASCII; total and pure | `tool_message_content` |
| `std::string denial_detail(std::string_view reason)` | free function | token map of sec 5.2; unknown -> `bounded_ascii(reason, kMaxDenialReasonBytes)`; empty -> `"unspecified"` | `denial_text` |
| `std::string denial_text(std::string_view tool, std::string_view reason)` | free function | sec 5.2 exact sentence; bounded tool/reason; total, pure, no I/O | `AgentLoop` denial branch (`src/agent/agent_loop.cpp:966`); `tool_message_content` |
| `std::string tool_message_content(const payload::ToolResult&)` | free function | sec 5.3; Ok/Error/Cancelled passthrough; Denied canonical (validated by `is_canonical_denial`) or repaired | `deriveMessages` (`src/session/session.cpp:504`); `UiModel::apply(ToolFinished)` (`src/ui/ui_model.cpp:1777-1800`); `session_export` (`src/ui/session_export.cpp:234`) |

There is no dead symbol: every new function has at least one production caller,
and the constants are used by the functions above. The new source file
`src/session/tool_result_text.cpp` is added to the `ymh_session` target's source
list (`CMakeLists.txt:191-199`); the implementation wave owns that one-line
build change. `agent_loop.cpp` (in the agent target) and the UI targets already
link `ymh::session`.

## 8. C++ interface sketches

### 8.1 Config and MCP type defaults

```cpp
// include/ymh/config/config.hpp (excerpt; 93-MD2)
struct McpServerSettings {
    // ... existing fields ...
    // 93-MD2: a server exists only in the global layer and only because the
    // user put it there (50-D4). Enabling it is the consent to run its tools
    // without a per-call prompt. Narrow per server with "ask"/"deny", or with
    // a permissions.rules entry (global layer, 90-D18).
    // 93-MD2/MD3/MD11: nullopt = key absent (implicit: Allow normally, Deny
    // under an explicit global Deny); a value is the explicit per-server
    // override. Not serialized.
    std::optional<std::string> default_verdict = std::nullopt;
    // ... existing fields ...
};

// include/ymh/mcp/mcp_types.hpp (excerpt; 93-MD1)
struct McpServerConfig {
    // ... existing fields ...
    // 93-MD1: declaration only; McpManager does not read this field. The
    // effective value is McpServerSettings::default_verdict lowered by the
    // daemon into PermissionConfig::tool_defaults (15 AM-3).
    PolicyVerdict default_verdict{PolicyVerdict::Allow};
    // ... existing fields ...
};
```

```cpp
// src/cli/wiring.cpp (excerpt; 93-MD3/MD11)
const PolicyVerdict implicit_mcp =
    default_verdict == PolicyVerdict::Deny ? PolicyVerdict::Deny
                                           : PolicyVerdict::Allow;
fallback.verdict =
    master ? PolicyVerdict::Allow
           : server.default_verdict.has_value()
                 ? parse_verdict(*server.default_verdict, "mcp.default_verdict")
                 : implicit_mcp;
```

The loaders populate the optional only on presence, not on equality with the
default:

```cpp
// src/config/config.cpp (excerpt; both mcp.server and mcp_servers forms)
if (member(table, "default_verdict") != nullptr) {
    server.default_verdict = read_string(table, "default_verdict", "mcp.server",
                                         "allow", source);
}
```

`to_mcp_config` maps `nullopt` to the unread runtime field's `Allow` initializer
(`McpManager` does not read it); the effective permission value is the
`to_permission_config` lowering above.

### 8.2 Denial text helper

```cpp
// include/ymh/session/tool_result_text.hpp (new; 93-DF1..DF13)
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ymh/session/events.hpp"   // payload::ToolResult, payload::ToolOutcome

namespace ymh {

inline constexpr std::string_view kDeniedToolMarker   = "DENIED:";
inline constexpr std::string_view kDeniedNotExecuted   = "was NOT executed";
inline constexpr std::size_t kMaxDenialToolBytes   = 128;
inline constexpr std::size_t kMaxDenialReasonBytes = 256;
inline constexpr std::size_t kMaxDenialTextBytes   = 1024;

// 93-DF12: printable ASCII only; cap bytes; "..." when truncated.
[[nodiscard]] std::string bounded_ascii(std::string_view text, std::size_t cap);

// 93-DF2: display rendering of a permission reason token. Pure and total.
[[nodiscard]] std::string denial_detail(std::string_view reason);

// 93-DF1: the canonical, model-facing denial text (sec 5.2). Pure and total.
[[nodiscard]] std::string denial_text(std::string_view tool, std::string_view reason);

// 93-DF5/DF6: true iff the text is a canonical denial that is safe to pass
// through: marker, "was NOT executed", size cap, printable single-line ASCII.
[[nodiscard]] bool is_canonical_denial(std::string_view text) noexcept;

// 93-DF5/DF6: provider Tool-message content. Ok/Error/Cancelled pass through;
// Denied returns its validated canonical output, or repairs a legacy token.
[[nodiscard]] std::string tool_message_content(const payload::ToolResult& result);

} // namespace ymh
```

```cpp
// src/session/tool_result_text.cpp (new; implementation sketch)
#include "ymh/session/tool_result_text.hpp"

namespace ymh {
namespace {

void append_ascii(std::string& out, std::string_view text, std::size_t cap) {
    for (const char raw : text) {
        if (out.size() >= cap) {
            return;
        }
        const auto byte = static_cast<unsigned char>(raw);
        out.push_back(byte >= 0x20 && byte <= 0x7e ? raw : '?');
    }
}

} // namespace

std::string bounded_ascii(std::string_view text, std::size_t cap) {
    std::string out;
    out.reserve(cap);
    append_ascii(out, text, cap);
    if (out.size() < text.size() && cap >= 3) {
        out.resize(cap - 3);
        out += "...";
    }
    return out;
}

std::string denial_detail(std::string_view reason) {
    if (reason.empty()) return "unspecified";
    if (reason == "user") return "the user denied it";
    if (reason == "timeout") return "permission request timed out";
    if (reason == "cancelled") return "request cancelled";
    if (reason == "shutdown") return "daemon shut down";
    if (reason == "transport") return "transport failure";
    if (reason == "no-subscribers") return "no attached client";
    if (reason == "internal") return "internal error";
    // 93-L3: the headless/daemon resolvers emit the token; the no-gate loop
    // emits this prose. Both render the same way.
    if (reason == "no-resolver" ||
        reason == "no permission resolver attached") {
        return "no permission resolver attached";
    }
    if (reason == "rule") return "denied by a permission rule";
    if (reason == "permission:protected-path") return "protected path";
    return bounded_ascii(reason, kMaxDenialReasonBytes);
}

bool is_canonical_denial(std::string_view text) noexcept {
    if (text.size() > kMaxDenialTextBytes) return false;
    if (!text.starts_with(kDeniedToolMarker)) return false;
    if (text.find(kDeniedNotExecuted) == std::string_view::npos) return false;
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 0x20 || byte > 0x7e) return false;
    }
    return true;
}

std::string denial_text(std::string_view tool, std::string_view reason) {
    std::string out;
    out.reserve(kMaxDenialTextBytes);
    out += "DENIED: tool '";
    out += bounded_ascii(tool, kMaxDenialToolBytes);
    out += "' was NOT executed; permission was not granted (reason: ";
    out += denial_detail(reason);
    out += "). The tool did not run and produced no tool output; "
           "do not report this call as successful.";
    if (out.size() > kMaxDenialTextBytes) {
        out.resize(kMaxDenialTextBytes - 3);
        out += "...";
    }
    return out;   // already ASCII by construction
}

std::string tool_message_content(const payload::ToolResult& result) {
    if (result.outcome != payload::ToolOutcome::Denied) {
        return result.output;
    }
    if (is_canonical_denial(result.output)) {
        return result.output;
    }
    std::string_view reason = result.output;
    if (reason.empty() && result.error.has_value()) {
        reason = *result.error;
    }
    return denial_text(result.name, reason);
}

} // namespace ymh
```

### 8.3 Agent loop denial branch

```cpp
// src/agent/agent_loop.cpp (excerpt; 93-DF1)
plan.decision = decision;
if (decision == payload::PermissionDecisionKind::Deny) {
    payload::ToolResult denied;
    denied.id      = call.id;
    denied.name    = call.name;
    denied.outcome = payload::ToolOutcome::Denied;
    denied.output  = denial_text(call.name, reason);   // was: reason or "permission denied"
    denied.error   = denied.output;
    denied.source.call = denied.id;
    plan.run     = false;
    plan.decided = std::move(denied);
}
return plan;
```

`include "ymh/session/tool_result_text.hpp"` is added to the loop's includes.
No other branch changes. `reason` remains the `PermissionOutcome.reason` token;
the helper owns the prose.

### 8.4 Projection: session, UI, export

```cpp
// src/session/session.cpp (excerpt; 93-DF3)
case EventType::ToolResult: {
    const auto& value = event.payload.get<payload::ToolResult>();
    // ... existing provenance/id checks ...
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = tool_message_content(value);   // was: value.output
    message.content.push_back(std::move(block));
    // ... existing replace/push ...
}
```

```cpp
// src/ui/ui_model.cpp (excerpt; 93-DF6/DF7)
if constexpr (std::is_same_v<T, ToolFinished>) {
    const std::size_t index = state.tools.find(e.id);
    if (index != kNoEntry) {
        ToolCallView& call = state.tools.calls[index];
        call.finished = true;
        call.outcome  = e.outcome;
        call.truncated = e.truncated;
        call.notice = /* unchanged */;
        if (e.outcome == payload::ToolOutcome::Denied) {
            payload::ToolResult result;
            result.name    = e.name;
            result.outcome = e.outcome;
            result.output  = e.output;
            result.error   = e.error;
            call.output = tool_message_content(result);
        } else if (!e.output.empty()) {
            call.output = e.output;   // existing cap logic unchanged
        }
    }
    // the matching ConversationEntry::text gets the same Denied repair.
}
```

```cpp
// src/ui/ui_render.cpp (excerpt; 93-DF5)
StyledLine header;
if (call != nullptr && call->outcome != payload::ToolOutcome::Ok) {
    header.push_back({"\xE2\x9C\x97 ", ftxui::Color::Red, false});  // existing glyph
}
if (call != nullptr && call->outcome == payload::ToolOutcome::Denied) {
    header.push_back({"DENIED ", ftxui::Color::Red, true});
}
header.push_back({"\xE2\x96\xB8 ", ftxui::Color::Yellow, false});
header.push_back({name, theme.tool_name, true});
```

The existing cross glyph is written as its UTF-8 bytes above only to show the
insertion point; the new pinned text is the ASCII token `DENIED `. No other
header change is allowed.

```cpp
// src/ui/session_export.cpp (excerpt; 93-DF3/DF8)
out << code_block(tool_message_content(result));   // was: code_block(result.output)
const bool duplicate_denial_error =
    result.outcome == payload::ToolOutcome::Denied &&
    result.error.has_value() && *result.error == result.output;
if (result.error.has_value() && !result.error->empty() && !duplicate_denial_error) {
    out << "\n_Error: " << *result.error << "_\n";
}
```

### 8.5 Complete required-change list (implementation wave)

1. `include/ymh/config/config.hpp`: `McpServerSettings::default_verdict` becomes `std::optional<std::string>` (nullopt = absent).
2. `include/ymh/mcp/mcp_types.hpp`: `McpServerConfig::default_verdict{Allow}`; `to_mcp_config` (`src/cli/wiring.cpp:236-237`) maps `nullopt` to Allow and parses a present value (the runtime field is not read by `McpManager`).
3. `src/config/config.cpp`: both `default_verdict` reads become presence-guarded and assign the optional only when the key exists (`apply_mcp_server`, `apply_mcp_servers_entry`; the localcode entry passes through the second).
4. `src/cli/wiring.cpp`: the lowering of 93-MD11 (`master ? Allow : has_value ? parse(*value) : implicit`) and the global-deny veto.
5. New `include/ymh/session/tool_result_text.hpp` + `src/session/tool_result_text.cpp` (including `is_canonical_denial`).
6. `src/agent/agent_loop.cpp`: include the helper; `denied.output = denial_text(...)`.
7. `src/session/session.cpp`: `block.text = tool_message_content(value)`.
8. `src/ui/ui_model.cpp`: Denied repair for `call.output`/entry text.
9. `src/ui/ui_render.cpp`: `DENIED` badge for Denied.
10. `src/ui/session_export.cpp`: helper for the body and duplicate `_Error:` suppression.
11. `CMakeLists.txt:191-199`: add `src/session/tool_result_text.cpp` to `ymh_session`.
12. Docs/samples: 21 sec 7 and 15 sec 4.1/6.1 defaults (this spec's supersession).

No other file is part of the change. In particular, do not change
`rule_matches`, the broker's `"user"` token, the protocol, `ToolOutcome`, or the
event schema.

## 9. Invariants

### 9.1 MCP defaults (`MD#`)

| ID | Invariant |
|---|---|
| MD1 | An enabled MCP server with no explicit `default_verdict` yields Allow for its `mcp.<server>.*` tools, except under an explicit global `Deny` (MD8/MD11). |
| MD2 | An explicit per-server `"ask"` yields Ask and the existing dialog; `"deny"` yields Deny with no dialog and no execution; `"allow"` yields Allow even under a global `Deny`. |
| MD3 | The per-server key is the only user-facing override; no new config key is introduced. The loader records presence as `std::optional<std::string>`. |
| MD4 | A path rule never matches an MCP tool, regardless of `tool:"*"` or `path:"**"` (90-D4 retained). |
| MD5 | A matching tool rule outranks the server default; a rule deny outranks the master switch. |
| MD6 | `permissions.default = "allow"` (or the `approval:"never"` baseline) still forces every enabled server's `tool_defaults` entry to Allow. |
| MD7 | The default is justified only while MCP definitions and `permissions.*` are global-layer only (50-D4, 90-D18). |
| MD8 | `PermissionConfig::default_verdict` remains Ask for non-MCP tools; the MCP entry is a `tool_defaults` fallback. An explicit global `Deny` lowers an implicit MCP key to Deny (MD11); it is not silently upgraded to Allow. |
| MD9 | A disabled or id-less server contributes no `tool_defaults` entry. |
| MD10 | The spec-89 guard, the ReadOnly hard deny (including a `destructiveHint:true` MCP tool via `request.destructive`), `force_ask`, protected paths, and the 90 target-class checks keep their exact order for every MCP request. |
| MD11 | An explicit global `Deny` vetoes an implicit MCP Allow; an explicit per-server `"allow"`/`"ask"` or a matching rule is the only override. |
| MD12 | The shipped `destructiveHint` -> `request.destructive` -> `tool_is_mutating` -> ReadOnly deny is retained; only non-hint MCP tools pass ReadOnly by default. |
| MD13 | `McpManager` does not evaluate permissions; the broker/loop still does. |
| MD14 | `allowed_tools`/`denied_tools` catalog filtering is unchanged and still happens before any call. |

### 9.2 Denied feedback (`DF#`)

| ID | Invariant |
|---|---|
| DF1 | Every Denied `ToolResult` created by the loop has `output == denial_text(name, reason)` and `error == output`. |
| DF2 | The denial text starts with `DENIED:`, contains `NOT executed`, contains a rendered reason, and ends with the no-success sentence. |
| DF3 | `denial_text` and `tool_message_content` are pure, total, deterministic, and perform no I/O. |
| DF4 | `tool_message_content` returns `output` unchanged for Ok, Error, and Cancelled. |
| DF5 | For Denied, `tool_message_content` returns canonical output unchanged only when `is_canonical_denial` is true (marker, `was NOT executed`, size cap, printable single-line ASCII); otherwise it synthesizes the canonical text at projection time. |
| DF6 | `deriveMessages` emits `tool_message_content(result)` for every Tool message; a Denied message is never the bare reason token. |
| DF7 | The UI header of a Denied tool row contains the ASCII token `DENIED` in both collapsed and expanded states. |
| DF8 | The UI model and export use `tool_message_content`, so a legacy bare-token Denied event never displays/export as success or as a bare token body; the export prints the denial sentence once (no duplicate `_Error:` for the new shape). |
| DF9 | No projection maps Denied to Ok, to an empty output, or to a success marker. `PermissionDecision.reason` is unchanged. |
| DF10 | The denial text is at most 1024 bytes, printable ASCII, one line, and contains no request arguments, prompt text, or secrets. This claim is about `denial_text`/the provider message, not the export's separate `ToolCall.arguments` block. |
| DF11 | Replaying the same event sequence yields byte-identical projected Tool messages and the same outcome. |
| DF12 | Exactly one `ToolResult` exists per `tool_use` (01 I12 retained); the denial text does not alter pairing. |
| DF13 | A Denied call never reaches `ToolRegistry::execute`; `plan.run == false`. |
| DF14 | A policy Deny (rule/hard deny/guard) and a human Deny both use the same canonical text shape; only the rendered reason differs. |

## 10. Failure modes (`MD-F#` / `DF-F#`, F# tagged per 00 sec 54)

F# is the architecture finding class at `00-architecture.md:4832-4856`. MCP
default failures are F2 (background permission/attention) unless noted; denial
projection failures are F2 (the model cannot see the decision) or F4
(edge-triggered attention for the UI badge).

| ID | F# | Failure | Detection | Required behavior |
|---|---|---|---|---|
| MD-F1 | F2 | Only `McpServerConfig` flips to Allow while `McpServerSettings` stays `"ask"` (or vice versa); calls still Ask | `DefaultMcpServerIsAllow` unit test | Both defaults change; the effective settings path yields Allow |
| MD-F2 | F2 | Wiring hard-codes Allow for every server and ignores an explicit `"ask"`/`"deny"` | `ExplicitAskWins`, `ExplicitDenyWins` | `parse_verdict(server.default_verdict)` remains the only source (except master) |
| MD-F3 | F2 | Master switch stops forcing MCP entries to Allow (46-D1.3 regression), including over an explicit per-server `"deny"` | `MasterSwitchStillAllows` | `master ? Allow : ...` retained |
| MD-F4 | F2 | A path rule starts matching no-path tools, so `{tool:"*",path:"**",effect:"deny"}` denies MCP | `PathRuleDoesNotMatchMcp` | `rule_matches` no-path branch returns false (90-D4) |
| MD-F5 | F2 | A disabled server gets a default entry, allowing a tool that should not exist | `DisabledServerHasNoDefault` | Keep the `enabled`/`id` guard |
| MD-F6 | F2 | MCP default leaks to non-MCP tools (e.g. `mcp` prefix too short) | `NonMcpToolUnaffected` | Prefix is the literal `mcp.<server>.` |
| MD-F7 | F2 | An explicit `permissions.default:"deny"` is silently lowered to Allow for an implicit MCP server (fail-open) | `GlobalDenyDeniesImplicitMcp`, `ExplicitAllowOverridesGlobalDeny`, `RuleAllowOverridesGlobalDenyForMcp` | Global-deny veto of 93-MD11; explicit per-server `"allow"` or an allow rule is the override |
| MD-F8 | F2 | Rule deny is assumed to beat the master switch and does not | `RuleDenyBeatsMaster` | 46-D1.2/46-D1.6 retained |
| MD-F9 | F2 | A loader does not populate the optional for a present key, so an explicit per-server `"allow"` is treated as implicit and denied under a global Deny | `ExplicitPresenceIsTracked` (both `mcp.server` and `mcp_servers` forms) | Loaders assign the optional iff the key is present; absent is nullopt |
| MD-F10 | F2 | A loader populates the optional for an absent key, so a global Deny is ignored | `AbsentKeyIsNullopt` | Presence check (`member(...) != nullptr`), never a string comparison |
| DF-F1 | F2 | The loop keeps `output = "user"` (or drops the marker) | `DeniedToolResultTextIsUnmissable` | `denial_text` at creation |
| DF-F2 | F2 | `deriveMessages` reverts to `value.output` and the provider sees the bare token | `DeniedResultProjectsCanonicalText`, `LegacyBareTokenIsRepaired` | Use `tool_message_content` |
| DF-F3 | F2 | A legacy `"user"` output is projected verbatim | same legacy test | Projection-time canonical repair |
| DF-F4 | F4 | The UI shows only a red cross, no `DENIED`, so denied and error look alike | `DeniedToolRendersBadge` | `DENIED` badge for Denied |
| DF-F5 | F2 | The UI expanded body/export shows the bare token for legacy events | `LegacyDeniedUiBodyIsCanonical`, export test | Helper in UI model/export |
| DF-F6 | F2 | A Denied result is written as `Ok` or with empty output by a later layer | `OutcomeIsNeverCoerced` | No projection/commit coercion; outcome passes through |
| DF-F7 | F2 | A downstream reason/argument string makes the denial text unbounded or injects newlines/ANSI | `DenialTextIsBoundedAscii`, `DenialTextExcludesArguments` | `bounded_ascii` + caps; arguments never passed; `is_canonical_denial` rejects non-ASCII pass-through |
| DF-F8 | F2 | The helper double-prefixes an already-canonical output | `ToolMessageContentKeepsCanonicalDenied` | `is_canonical_denial` passthrough |
| DF-F11 | F2 | The export prints the denial sentence twice (code block + `_Error:` line) | `DeniedExportDoesNotDuplicateError` | Suppress `_Error:` when `Denied && error == output` |
| DF-F9 | F2 | Replay produces different text (e.g. timestamp/order-dependent) | `DenialProjectionIsDeterministic` | Pure function of the event |
| DF-F10 | F2 | `permission.decide`'s reason is changed to prose, breaking audit/assertions | `BrokerReasonTokenUnchanged` | Broker still returns `"user"`; only the result text is rendered |

## 11. dsh (DeepSeek Harness) mapping

Per 56-D6, every non-mirror row carries a reason and a concrete anchor (a
decision ID or a `file:line`); "not mirrored" alone is not a disposition.

| dsh surface | ymh mirror | Justification |
|---|---|---|
| per-call approval for tool execution | `PermissionPolicy::evaluate` + the `PermissionBroker` Ask path | **Mirror, with an MCP-specific trust default.** Evaluation is still per call; the default verdict is derived from the user's server definition. Anchor: `09-permissions.md:443-495`, `src/policy/permission_policy.cpp:490-540`. |
| explicit server/tool trust configuration | `mcp.server[].default_verdict` = `allow`/`ask`/`deny`, presence-aware | **Mirror.** The per-server key is the trust input; it is a NO-MATCH fallback below rules (15 AM-3). An absent key is Allow normally and Deny under an explicit global Deny (93-MD11). Anchor: `docs/design/15-mcp-adapter.md:1307-1332`, `src/cli/wiring.cpp:163-173`. |
| operator rules override trust defaults | `permissions.rules` `{tool:"mcp.<server>.*",effect:...}` | **Mirror.** Rules are specificity-ranked and outrank `tool_defaults`. Anchor: `46-permissions-ui-errata.md:424-475`, `src/policy/permission_policy.cpp:490-526`. |
| "never ask" / auto-approve mode | `permissions.default = "allow"` master switch | **Mirror.** It still upgrades every MCP default to Allow; a rule deny still vetoes it. Anchor: 46-D1.3/46-D1.6 (`docs/design/46-permissions-ui-errata.md:470-475`). |
| per-call prompt for a configured MCP server | **NON-MIRROR (deliberate):** no prompt by default; prompt only on explicit `"ask"` | Non-mirror reason: a server exists only in the user's global config (50-D4) and the permission table is global-only (90-D18), so enabling it is the consent event; asking again is the reported defect. The fail-closed exception is an explicit global `permissions.default:"deny"`, which denies an absent per-server key (93-MD11). Anchor: `docs/design/50-skills-mcp-and-session-lifecycle-errata.md:828-871` (50-D4), `docs/design/90-path-scoped-permissions-errata.md:1685-1690` (90-D18). |
| path-scoped trust for tools with a filesystem target | path rules match only path tools and shell/terminal; MCP has no path dimension | **Mirror of 90-D4.** `path:"**"` is not an "any tool" wildcard. Anchor: `90 sec 4.5` (`docs/design/90-path-scoped-permissions-errata.md:1002-1040`), `src/policy/permission_policy.cpp:169-185`. |
| tool-result feedback to the model, including failures | explicit Denied text in the provider Tool message | **Mirror, strengthened.** The provider Tool message is text-only; the outcome must therefore be encoded in the text. Anchor: `01-session.md:522-535` (the denied call still produces a ToolResult so the model sees a result), `06-agent-loop.md:794-798` (Deny -> Denied, step continues). |
| machine-readable tool outcome | durable `ToolOutcome` stays structured; the text is a projection | **Mirror with a text envelope.** `ToolResult.outcome` is the source of truth; the message carries `DENIED:` for the model. Anchor: `include/ymh/session/events.hpp:145-158`, `src/session/session.cpp:497-505`. |
| user-visible denial affordance | `DENIED` badge plus canonical body in the transcript/export | **Mirror.** The UI must not render a denied call like a successful one. Anchor: `17-ui-transcript-errata.md` sec 4 item 4 (amended for Denied), `src/ui/ui_render.cpp:349-360`. |
| confinement of MCP subprocesses | **NON-MIRROR:** 89-D18 remains; default Allow does not confine | Non-mirror reason: an MCP server is an operator-configured external process/endpoint, and extending Landlock to it needs a per-tool capability declaration that does not exist; default Allow is a trust decision, not a sandbox. Anchor: `89-D18` (`docs/design/89-shell-command-safety-guard-errata.md:671`) and the mapping row at `:923`; `src/mcp/mcp_transport.cpp:176-205`. |

## 12. State-lifetime table

| State | Type / where | Created | Destroyed / cleared | Owner | Survives restart? |
|---|---|---|---|---|---|
| `McpServerSettings::default_verdict` | `std::optional<std::string>` in `Config` | global `config.jsonc` parse (`src/config/config.cpp:958-959`, `:1122-1123`); nullopt when absent | with `Config` / next load | CLI/daemon startup | no (re-read; never serialized) |
| `McpServerConfig::default_verdict` | enum in `McpConfig` | `to_mcp_config` (`src/cli/wiring.cpp:236-237`) | with `McpManager`/daemon | `McpConfig` owner | no (re-derived) |
| `PermissionConfig::tool_defaults` MCP entry | `ToolDefault` vector | `to_permission_config` (`src/cli/wiring.cpp:163-173`) | with the policy/daemon | `RulePermissionPolicy` | no (rebuilt) |
| `PermissionConfig::default_verdict` | enum | config parse + wiring (`src/cli/wiring.cpp:138-147`) | with the policy/daemon | `RulePermissionPolicy` | no (rebuilt) |
| Denial text | `std::string` in the durable `ToolResult.output` | Deny branch (`src/agent/agent_loop.cpp:966`) | with the session log (durable) | session store / event log | yes (durable event) |
| `ToolResult.error` | optional string | same branch (`:967`) | same | session store | yes |
| Projected Tool-message text | local `ContentBlock::text` in `deriveMessages` | each projection call | end of projection | call frame | no (recomputed, deterministic) |
| Repaired legacy text | local string in `tool_message_content` | each projection/UI apply | end of call | call frame | no (derived) |
| UI `ToolCallView.output` | `std::string` in per-session UI model | `ToolFinished` (`src/ui/ui_model.cpp:1777-1800`) | session switch/daemon death/UI teardown | UI model | no (rebuilt on replay) |
| UI `ToolCallView.outcome` | enum | `ToolFinished` | same | UI model | no (rebuilt) |
| `DENIED` badge | rendered string | `render_tool_entry` per frame | end of frame | renderer | no |
| Export body | local string | `session_export` per event | end of export | export frame | no |
| `kDeniedToolMarker` / `kDeniedNotExecuted` / caps | compile-time constants | static | program lifetime | none | n/a |
| `PermissionDecision.reason` | `std::string` in the durable event | broker/gate outcome -> loop append (`src/agent/agent_loop.cpp:907` gate hook, `:950` no-gate branch) | with the session log | session store | yes (unchanged token) |

No new cache, no new registry row, no new durable file, and no new session
field. The optional is config-lifetime only and is not serialized. The only
new durable content is the denial text in the existing `ToolResult.output`,
which is already an event field.

## 13. Test plan (hermetic, real tests)

All tests are hermetic: no network, no real MCP server unless an existing fake
fixture is used. The existing live suite (`YMH_LIVE_LLM=1`) is unaffected.

### 13.1 New unit tests: MCP defaults

File: `tests/unit/mcp_permission_defaults_test.cpp` (new).

| Test | Setup | Assertion |
|---|---|---|
| `DefaultMcpServerIsAllow` | `Config` with the ordinary global default `ask`, one enabled server, no key | `to_permission_config` has prefix `mcp.github.` with `PolicyVerdict::Allow`; `RulePermissionPolicy` returns Allow for `mcp.github.search` |
| `DisabledServerHasNoDefault` | same server, `enabled=false` | no `tool_defaults` entry; the policy returns the global `Ask` |
| `EmptyIdServerHasNoDefault` | `id=""`, `enabled=true` | no entry (guard retained) |
| `GlobalDenyDeniesImplicitMcp` | `permissions.default="deny"`, server no key | policy returns Deny (93-MD11) |
| `ExplicitAllowOverridesGlobalDeny` | global `deny`, server `default_verdict="allow"` (present) | policy returns Allow |
| `ExplicitAskOverridesGlobalDeny` | global `deny`, server `"ask"` (present) | policy returns Ask; no auto-run |
| `ExplicitDenyWins` | global `ask`, server `"deny"` (present) | policy returns Deny |
| `ExplicitAskWins` | global `ask`, server `"ask"` (present) | policy returns Ask |
| `MasterSwitchStillAllows` | `permissions.default="allow"` + server `"ask"`/`"deny"` (present) | policy returns Allow (46-D1.3 retained) |
| `RuleDenyBeatsMaster` | master allow + rule `{tool:"mcp.github.*",effect:"deny"}` | policy returns Deny (46-D1.2/46-D1.6 retained) |
| `RuleAllowOverridesGlobalDenyForMcp` | global `deny`, no server key, rule `{tool:"mcp.github.*",effect:"allow"}` | policy returns Allow (rule fold outranks the fallback) |
| `ToolRuleAskBeatsDefault` | default allow + rule `{tool:"mcp.github.search",effect:"ask"}` | policy returns Ask |
| `PathRuleDoesNotMatchMcp` | server `"ask"` explicit + rule `{tool:"*",path:"**",effect:"allow"}` | policy returns Ask (the path rule did not match) |
| `PathDenyDoesNotDenyMcp` | default allow + rule `{tool:"*",path:"**",effect:"deny"}` | policy returns Allow (the path rule did not match) |
| `ExplicitPresenceIsTracked` | parse `mcp.server[]` and `mcp_servers` JSON with `default_verdict:"ask"` | `default_verdict.has_value() == true`; wiring yields Ask |
| `AbsentKeyIsNullopt` | parse a server without the key | `default_verdict.has_value() == false`; implicit default is Allow under global `ask`, Deny under global `deny` |
| `NonMcpToolUnaffected` | default server + request `unknown_tool` with `permissions.default:"ask"` | returns Ask; the MCP prefix does not match a non-MCP name |
| `DestructiveHintRequestDeniedInReadOnly` | `PermissionRequest{tool:"mcp.fake.delete", destructive:true, sandbox:ReadOnly}` | `evaluate` returns Deny; the same request with `destructive=false` is not denied by ReadOnly |

### 13.2 New unit tests: denial text helper

File: `tests/unit/denial_text_test.cpp` (new).

| Test | Assertion |
|---|---|
| `CanonicalDenialTextNamesOutcomeToolAndReason` | `denial_text("mcp.github.search", "user")` starts with `DENIED:`, contains `mcp.github.search`, `NOT executed`, `the user denied it`, and the no-success sentence |
| `EmptyReasonIsUnspecified` | `denial_text("x", "")` contains `reason: unspecified` |
| `KnownReasonsAreRendered` | `user`, `timeout`, `cancelled`, `shutdown`, `transport`, `no-subscribers`, `internal`, `no-resolver`, `no permission resolver attached`, `rule`, `permission:protected-path` map as sec 5.2 |
| `UnknownReasonPassesThroughSanitized` | `"permission:protected-path\n\x1b[31m"` is printable ASCII, no newline/ESC, bounded |
| `DenialTextIsBoundedAscii` | a 4 KiB control-char reason yields <= 1024 bytes, all bytes in `[0x20,0x7e]`, ends with a complete sentence |
| `ToolMessageContentPassesThroughOk` | an Ok result returns `.output` byte-for-byte |
| `ToolMessageContentKeepsCanonicalDenied` | a Denied result whose output is the full canonical sentence is returned unchanged (idempotence) |
| `BareDeniedPrefixIsNotMistakenForCanonical` | a Denied result with output `"DENIED: custom"` (no `was NOT executed`) is rebuilt into the full sentence |
| `OversizedCanonicalIsRebounded` | a Denied result with a 2 KiB output that starts `DENIED:` and contains `was NOT executed` is rebuilt and is <= 1024 bytes |
| `ToolMessageContentRepairsLegacyBareToken` | a Denied result with output `"user"` returns canonical text containing `the user denied it` |
| `ToolMessageContentRepairsEmptyDenied` | a Denied result with empty output/error returns `reason: unspecified` |
| `ToolMessageContentLeavesErrorAndCancelled` | Error/Cancelled outputs are unchanged |
| `CanonicalWithControlByteIsRebuilt` | a Denied output that otherwise looks canonical but contains `\n` or ESC fails `is_canonical_denial` and is rebuilt into printable single-line text |
| `IsCanonicalDenialRejectsOversize` | a 2 KiB canonical-looking output fails `is_canonical_denial` |

### 13.3 Agent-loop tests

File: `tests/unit/agent_loop_test.cpp` (extend).

| Test | Setup | Assertion |
|---|---|---|
| `DeniedToolResultTextIsUnmissable` | `PermissionConfig::default_verdict = Deny`; one `read_file` call | the durable `ToolResult` has `outcome=Denied`, `output` starts with `DENIED:`, contains `NOT executed` and the no-success sentence; `error == output` |
| `UserDenialRendersUserDenied` | `AgentEnv` with a resolver returning `{Deny,"user"}` (or a `PermissionGate` fake) | the durable output contains `the user denied it` |
| `DeniedToolStillDoesNotExecute` | existing `DeniedToolYieldsDeniedResultAndContinues` | keep the existing outcome/turn assertions; add the text assertions |
| `PolicyDenyNoGateRendersDeniedByPolicy` | a deny rule, no gate (`services_.gate == nullptr`) | output starts with `DENIED:` and contains `reason: denied by policy`; it does not contain a rule id |
| `PolicyDenyViaBrokerRendersRule` | a resolver returning `{Deny,"rule"}` (the broker's policy-deny fallback; the broker test pins that fallback separately) | output contains `reason: denied by a permission rule`; the durable `PermissionDecision.reason` is `"rule"` |
| `DeniedArgumentsNeverAppearInText` | a tool call with a sensitive argument value | the denial text does not contain that value (the export's separate argument block is out of scope) |

### 13.4 Session projection tests

File: `tests/unit/session_test.cpp` (extend).

| Test | Assertion |
|---|---|
| `DeniedToolResultProjectsCanonicalText` | a session with `ToolResult{Denied, output=denial_text(...)}`; the projected `Role::Tool` message contains `DENIED:` and `NOT executed` |
| `LegacyBareTokenIsRepaired` | a hand-built legacy event with `output="user"`; the projected Tool message contains `DENIED:` and `the user denied it`, not just `user` |
| `DenialProjectionIsDeterministic` | two `deriveMessages` calls return byte-identical Tool content |
| `DenialProjectionDoesNotMutateEvents` | the stored event's `output` is still `"user"` after projection |
| `ErrorProjectionUnchanged` | an Error result's Tool message equals its `output` |

### 13.5 UI tests

File: `tests/unit/ui_render_golden_test.cpp` (extend) and/or
`tests/unit/ui_model_test.cpp`.

| Test | Assertion |
|---|---|
| `DeniedToolRendersBadge` | apply `ToolFinished{Denied, output="DENIED: ..."}` then render; the stripped screen contains `DENIED` and the tool name in the collapsed row |
| `DeniedBadgeExpanded` | with `expanded=true`, the body contains the no-success sentence |
| `ErrorToolDoesNotRenderDenied` | an Error outcome renders the existing cross, not `DENIED` |
| `LegacyDeniedUiBodyIsCanonical` | `ToolFinished{Denied, output="user"}` yields `call.output` containing `DENIED:` and `the user denied it` |

### 13.6 Export test

File: `tests/unit/session_export_test.cpp` (extend).

| Test | Assertion |
|---|---|
| `DeniedResultExportsDeniedAndText` | the markdown contains `Output (denied` and the canonical denial text |
| `LegacyDeniedExportIsCanonical` | a legacy `"user"` event exports the canonical text |
| `DeniedExportDoesNotDuplicateError` | a new Denied event (`error == output`) exports the denial sentence exactly once; the `_Error:` line is suppressed |
| `DeniedExportStillPrintsArguments` | the export still contains the `ToolCall.arguments` JSON block for a denied call (pinned pre-existing surface; DF13 does not claim redaction) |

### 13.7 Broker and integration tests

File: `tests/unit/permission_broker_test.cpp` (retain, do not change the
assertion) and a new `tests/integration/errata93_mcp_permission_test.cpp`.

| Test | Assertion |
|---|---|
| `BrokerReasonTokenUnchanged` | the existing `outcome.reason == "user"` assertion still passes |
| `DefaultMcpCallDoesNotAsk` (integration, `AgentEnv` + fake MCP fixture) | with `permissions.default:"ask"`, an enabled fake server and no `default_verdict`, the agent calls `mcp.fake.alpha`; a recording `PermissionResolver` is never invoked; the fake server receives the call; the `ToolResult` is Ok |
| `ExplicitAskStillAsks` | the same fixture with `default_verdict:"ask"`; the recording resolver is invoked exactly once and returns `{Deny,"user"}`; the fake server receives no call; the `ToolResult` is Denied with canonical text |
| `DeniedMcpCallIsNotReportedOk` | after the Deny, `deriveMessages`'s tool message contains `DENIED:`; the fake LLM's next prompt (captured by `FakeLLM`) contains it |

Use the existing `tests/support/agent_test_env.hpp` (`AgentEnv`, `FakeLLM`) and
the MCP fixture used by `tests/integration_mcp_test.cpp`; any glue between the
two stays inside the test file, with no production seam change. New test
sources are registered in `tests/CMakeLists.txt` under the existing
`add_executable(ymh_tests ...)` list (no glob).

### 13.8 Acceptance trace (must be executable end to end)

```text
A. Config: global only; permissions.default="ask"; the 90 sec 4.7 rule
   {tool:"*",path:"**",effect:"allow"}; one enabled fake MCP server with no
   default_verdict.
   Call mcp.fake.alpha:
     -> evaluate == Allow
     -> no permission.request
     -> registry call succeeds
     -> ToolResult{Ok}

B. Same config, server default_verdict="ask".
   Call mcp.fake.alpha:
     -> evaluate == Ask
     -> exactly one permission.request
     -> fake supervisor replies Deny/Once
     -> ToolResult{Denied, output=canonical text}
     -> the MCP server received no call
     -> deriveMessages Tool message contains "DENIED:" and "NOT executed"
     -> UI render contains "DENIED"
     -> export contains "denied" and the canonical text

C. Same as B, human replies AllowAlways.
   Second identical call -> no permission.request; the tool executes (existing
   grant semantics unchanged; 90-D7 applies only to shell/terminal).

D. Config permissions.default="allow" + server default_verdict="ask" or "deny".
   Call -> Allow, no request (46-D1.3 retained for both).

E. Config default allow + rule {tool:"mcp.fake.*",effect:"deny"}.
   Call -> Deny, no request, canonical denial text.

F. Config permissions.default="deny"; fake server with no default_verdict.
   Call -> Deny, no request, canonical denial text (93-MD11).
   Same config, server default_verdict="allow" (explicit):
   Call -> Allow, no request (explicit per-server override).
   Same config, rule {tool:"mcp.fake.*",effect:"allow"}:
   Call -> Allow, no request (rule override).

G. Legacy episode: ToolResult{Denied, output="user"} replayed.
   -> deriveMessages content starts with "DENIED:"
   -> UI shows DENIED and canonical body
   -> export shows canonical text
   -> the durable event still has output="user".
```

### 13.9 Existing tests to update

| Test | Why |
|---|---|
| Any test that asserts an MCP call defaults to Ask | New default is Allow; pin the new expected verdict or set `default_verdict:"ask"` explicitly |
| `tests/unit/agent_loop_test.cpp:DeniedToolYieldsDeniedResultAndContinues` | Add the text assertions; the outcome/turn assertions remain |
| `tests/unit/ui_render_golden_test.cpp` tool-entry goldens | The Denied row gains `DENIED`; add/refresh the Denied fixture only; Ok/Error goldens must not change |
| `tests/unit/session_export_test.cpp` denied fixture | The body is now canonical text |
| `tests/unit/mcp_manager_test.cpp:ToolDefaultsLoweredFromConfig` | Keep it; it sets the optional to `"allow"`, which is explicit. Add the implicit/default and global-deny cases in the new file |

## 14. Recorded risks and limitations

1. **Default Allow is a real behavior change.** A global config with an enabled
   server, no `default_verdict`, and a non-`deny` `permissions.default` stops
   asking after upgrade. This is the requested behavior; the per-server `"ask"`
   key restores the old behavior. A global `permissions.default:"deny"` is the
   exception: an absent server key stays Deny (93-MD11). Users who never revisit
   a non-deny config will run their MCP tools without prompts.
2. **MCP is not confined.** Default Allow does not add a sandbox, path rooting,
   or network control. 89-D18 stands: an MCP server can do what its process can
   do. The consent model is config authorship, not capability restriction.
3. **A global Deny is fail-closed, with explicit opt-ins.** With
   `permissions.default:"deny"` and no per-server key, MCP tools are denied
   (93-MD11). An explicit per-server `"allow"` or a matching allow rule is the
   deliberate override; an explicit `"ask"` asks. The per-server `"deny"` and
   the `tool_defaults` tier are otherwise unchanged. This is stronger than the
   pre-spec behavior (which asked) and is tested.
4. **The master switch overrides per-server `"ask"` and `"deny"`.**
   `permissions.default:"allow"` (or the `approval:"never"` baseline) is an
   explicit "never ask"; 46-D1.3 is retained. A rule deny is the escape hatch
   (46-D1.6).
5. **Path rules are not MCP policy.** `{tool:"*",path:"**",effect:"deny"}` does
   not deny MCP. Users must write a tool rule. This is correct 90-D4 semantics
   but is a likely source of confusion; the amendment note and sec 4.5 exist to
   make it explicit.
6. **`destructiveHint` is advisory, but it is already wired.** A
   `destructiveHint:true` MCP tool sets `request.destructive` and is denied
   under `ReadOnly` (`src/mcp/mcp_client.cpp:231-233`,
   `src/mcp/mcp_manager.cpp:211`, `src/agent/agent_loop.cpp:890-893`,
   `src/policy/permission_policy.cpp:251-263`, `:444-446`). A server can omit or
   lie about the hint, so a non-hint MCP tool can still run under ReadOnly and
   mutate state. This spec retains the shipped hint mapping (93-MD12) and does
   not claim ReadOnly is a complete MCP side-effect boundary.
7. **The denial text cannot force truthful prose.** A model can ignore it. The
   spec guarantees the evidence in the context window, not the model's summary.
   This is the same honesty boundary as 89-D1.
8. **Legacy repair is display-only.** The durable event still contains the old
   token; a raw DB reader or a non-ymh client sees it. All ymh projections use
   the helper. No backfill is performed.
9. **Error/Cancelled are not repaired.** They keep their existing outputs; a
   Cancelled tool could still be summarized optimistically. Out of scope
   (OQ-93-2); the 01 I12 pairing is unchanged.
10. **`McpServerConfig::default_verdict` is not read at runtime.** Changing it
    is a consistency measure; the effective path is the settings field. A
    future refactor that starts reading the runtime field will agree with the
    config default.
11. **Imported servers use the same implicit default.** The `mcp_servers`
    import path uses the same settings default and leaves the optional nullopt
    when the key is absent. Importing a server is a user action; an
    import from a third-party file yields allow-by-default under a non-deny
    global default, and Deny under an explicit global Deny. The import UI should
    continue to be treated as a consent surface.
12. **No protocol change means no machine-readable denial flag for external
    clients.** A headless consumer reads `ToolResult.outcome`; a provider reads
    the text. Both are correct, but they are two representations. The text is a
    projection of the outcome, not a second source of truth.
13. **The consent argument assumes the global config is not workspace-writable.**
    For a workspace rooted at `$HOME` (or an ancestor of `~/.config`), the
    global config is inside the root and the normal path tools can write it;
    the 90 protected-path set covers `.ymh`/`.git`, not `~/.config`. This does
    not create a new escalation (such a workspace is already trusted by the
    user), but it means "the user wrote the server entry" is weaker there. Do
    not run a project rooted at `$HOME` if the MCP trust default matters to
    you; use a project root.
14. **The export still prints the tool arguments.** `session_export` emits
    `code_block(call.arguments.dump(2), "json")` for every `ToolCall`
    (`src/ui/session_export.cpp:222-224`), including a denied call. The denial
    text itself never contains arguments (93-DF13), but the export is not a
    redacted artifact. This is pre-existing and recorded so DF13 is not
    overclaimed; redacting it would need a separate export-privacy decision.
15. **The canonical pass-through is validated, not assumed.** A durable Denied
    output is reused only if `is_canonical_denial` confirms printable
    single-line ASCII and the size cap; otherwise it is rebuilt. A crafted or
    corrupt legacy event therefore cannot smuggle a newline/ESC into the model
    message (93-DF12).

## 15. Open questions

| ID | Question | Current disposition |
|---|---|---|
| OQ-93-1 | Should there be a global `mcp.default_verdict` key (one switch for all servers)? | No in this revision. The per-server key is the smallest surface and already exists; a global key would need its own layering/precedence analysis. |
| OQ-93-2 | Should Error/Cancelled also carry mandatory outcome markers in the model-facing message? | Out of scope. They are tool-authored diagnostics today; a follow-up can apply the same envelope without changing the durable schema. |
| OQ-93-3 | Should `permission.decide` carry a free-form denial reason for plan review (25-D4 says the model receives review feedback)? | Out of scope. The wire has no such field today; this spec renders the existing token. Recorded as a pre-existing gap, not fixed here. |
| OQ-93-4 | Should the UI badge apply to Error/Cancelled too? | No. Only Denied is pinned; other labels are a separate UI decision. |
| OQ-93-5 | Is the exact denial sentence too prescriptive for the model? | It is deliberate: the defect is a model that claimed success. The sentence is bounded and factual; it does not ask the model to reveal hidden reasoning. If review finds it noisy, the "do not report" clause is the only optional part; the `DENIED:`/`NOT executed`/reason parts are mandatory. |

Resolved in Rev 2 and therefore not open: the global-deny question (M1) is
pinned by 93-MD11 and tested; the `destructiveHint` question (M2) is pinned by
93-MD12 as a retained shipped behavior with its limitation recorded.

None of these blocks the two fixes.

## 16. Revision log

| Revision | Date | Change |
|---|---|---|
| 1 (draft) | 2026-10-20 | Initial draft by `perm-author`: two verified defects and their fixes. MCP defaults: enabled servers default Allow (consent via global-only server definitions and global-only permissions), per-server override retained, master switch and rule precedence retained, path rules stay non-matching. Denied feedback: canonical denial text at the loop, projection repair for legacy events, `DENIED` UI badge, export consistency, bounded ASCII/no arguments. Invariants MD1-MD12 / DF1-DF14; failure modes MD-F1-MD-F8 / DF-F1-DF-F10; state-lifetime table; hermetic test plan with file names and acceptance traces. Not verified; no code written. |
| 2 (draft) | 2026-10-20 | Applies Gate 93 (0 HIGH / 4 MEDIUM / 6 LOW): M1 global-deny veto + optional presence (93-MD3/MD11, MD8, tests); M2 corrected `destructiveHint` wiring (93-MD10/MD12, sec 1.4/sec 2/risk 6); M3 policy-deny test uses shipped `denied by policy`/`rule` tokens, not a rule id; M4 DF13 scoped to the denial text/provider message, export arguments recorded as a separate pre-existing surface; L1 `is_canonical_denial` pass-through validation; L2 export duplicate `_Error:` suppression; L3 both `no-resolver` forms mapped; L4 DF-F7 is F2; L5 master table covers per-server `deny`; L6 tracker row is a promotion requirement. Not re-gated. |

**Promotion checklist.** Before this spec is committed to `dev`, add the
`DESIGN_STATUS.md` row 93 matching this header (status, revision, verification
token) so the `design_status_drift` ctest stays green (`AGENTS.md`; the
dev-side checker flags every `docs/design/[0-9][0-9]-*.md` without a row). The
working-tree draft intentionally does not edit the tracker, per the task
constraint.
