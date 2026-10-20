# 94 - Shell Scope and Permission-Dialog Hotkeys (errata)

```
Status: verified (Rev 2)
Revision: 2
Verification status: adversarial reviewer (independent, Rev 2): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Component: 94 (errata) -- two live defects plus one UX clarification.
            (a) The spec-90 shell target-scope walk escalates a command to
            `ShellTargetScope::Unknown` for a non-path token that merely
            contains an unexpanded `$`, so a fully in-root command with a
            `"exit: $?"` trailer cannot match a `{tool:"*", path:"**"}` rule and
            asks. (b) An unknown, non-destructive verb whose only path-like
            tokens are in-root (the CMake forms, including short-option
            `-DVAR=value` tokens) must stay `InRoot`; today some forms escalate
            to `Unknown`. (c) The permission dialog accepts only arrows, Enter,
            and Esc; this errata adds `A`/`O`/`S`/`D` accelerators, pins their
            rendered hints, and makes the `Always allow` label honest and
            tool-class-aware: `Always` keeps the verified 90-D9 shape (exact
            command for shell/terminal, all in-root paths for a path tool, the
            whole tool for a non-path/MCP tool), and the dialog label says which
            class it grants. No grant schema change is introduced.
            (d) Rev 2 also makes any unexpanded named/command/backtick
            expansion `Unknown` regardless of position; only shell special
            parameters are neutral (H1/H2/H3).
Depends on: 09-permissions.md (verified; `PermissionPolicy`,
           `PolicyVerdict`, `PermissionRequest`, the specificity fold);
           25-ui-ux-errata.md (verified; `force_ask`, UX46);
           45-ui-interaction-errata.md (verified; modal key routing);
           46-permissions-ui-errata.md (verified; grants, 46-I5/46-I6, the
           dialog option set); 56-dsh-fidelity-gap-closure-errata.md
           (verified; 56-D6 non-mirror justification rule);
           89-shell-command-safety-guard-errata.md (verified; L1/SG1-SG17);
           90-path-scoped-permissions-errata.md (verified; D5/D7/D8/D9,
           OQ-90-1).
Supersedes: nothing normative in 89/90. The per-tool `Always` class split of 90-D9,
           46-I5, 90-A15, 90-PS12, and the deferral in OQ-90-1 are **retained
           unchanged**; this errata records the rejected class-scoped
           alternative in 94-D9 (security reason: 90-C10 shows a durable
           in-root prefix for a control-plane verb such as `git` would allow
           `git clean -fdx`). It narrows the user-approved RB-12 decision
           (`REQUIREMENTS_BACKLOG.md:366-394`): the printable-key swallow is
           retained **except** for the exact eight ASCII characters
           `A/a`, `O/o`, `S/s`, `D/d`, which are now dialog accelerators.
           This **re-opens the RB-12 class of accident** for those four keys
           (a `/rename` burst answers `a`); 94-D14 accepts that risk explicitly
           and adds a PTY pin. It does **not** claim to preserve RB-12's
           anti-accident property. The exit-confirm and switcher keys are
           untouched.
Amends: 89 SG5 interpretation only by reference (the L1 risk value and every
           SG invariant are unchanged; this errata changes only the additive
           matching scope); 90 sec 4.5 (scope inputs); 46 sec 6.2 and
           25 sec 7.6/UX46 (dialog labels/footer only; the force_ask option
           set is unchanged).
```

Tracker: `DESIGN_STATUS.md` has no row for 94 yet. Per the repository rule, the
row is added only when this spec reaches `verified`; a draft row and a
`draft` header are equivalent by convention. This errata touches no other file.

---

## 1. Purpose

Three user-visible problems share one seam: the permission resolver decides
whether to prompt from `(request, effective rule set)`, and the TUI renders
and resolves that prompt.

1. **False `Unknown` from `$`.** The exact command the user reported is:

   ```
   cd /home/yury.meshkov/prjs/bb_tcat/louvre && grep -n "T7: deallocate" test/cct/command_completion_tracker_test.cpp; echo "exit: $?"
   ```

   Every filesystem target is the workspace root or below it. The command still
   prompts. The short token `$?` is an exit-status parameter, not a path, but
   the scope walk returns `Unknown` for any token containing an unexpanded `$`.
   `Unknown` maps to `TargetClass::Unknown`, and a shell path rule only matches
   `TargetClass::InRoot` with `rule.path == "**"` (`src/policy/permission_policy.cpp:169-181`).
   The rule cannot match, so the verdict falls through to `Ask`.

2. **False `Unknown` from CMake option values.** An unknown verb with only
   in-root operands must be `InRoot`. The literal forms `cmake -S . -B build`,
   `cmake --build build`, and `cmake -DKEY=value ...` already are, but a
   short-option token that embeds a `/`, `..`, `~`, or `$` in its **value**
   (`-DKEY=a/b`, `-DCMAKE_INSTALL_PREFIX=./stage`, `-DCMAKE_PREFIX_PATH=$HOME/x`)
   is escalated even when the value is not a filesystem target or resolves
   in-root. The short-option branch does not parse `=`, so it cannot distinguish
   `-DKEY=value` from `-I../include`.

3. **The dialog is arrow-only, and `Always allow` reads as a class promise.**
   Every printable key is swallowed (`src/ui/supervisor.cpp:3592-3597`), so the
   user must arrow to an option and press Enter. When the user does choose
   `Always allow`, the grant class depends on the tool: an exact command for
   `shell`/`terminal`, every in-root path for a path tool, and the whole tool
   for a non-path/MCP tool (90-D9). A single hard-coded "exact command" label is
   therefore false for two of the three classes. This errata keeps the 90-D9
   shapes (the class-scoped durable shell alternative is rejected in 94-D9) and
   makes the label tool-class-aware, with a footer that points to `S` for the
   in-memory class-scoped session grant.

The fixes are deliberately narrow:

- Recognized non-path shell special parameters (`$?`, `$$`, `$#`, `$@`, `$*`,
  `$!`, `$-`, `$1`..`$9`, including `${?}`/`${@}` forms) are neutral
  **wherever they occur**; the `?` in `$?` is not a glob. `$0`/`${0}` is
  `Unknown` because `bash -lc` sets it to `/bin/bash`. Any unexpanded named,
  command substitution, or backtick expansion is `Unknown` regardless of
  position (H1/H2/H3).
- Short options parse their `=value` half with the same classifier as long
  options.
- The dialog gains four exact ASCII accelerators and a footer that names them;
  the `Always` row is labeled per tool class. The accelerators re-open the RB-12
  burst risk for `a/o/s/d`; 94-D14 accepts it explicitly.
- `Always` keeps the verified 90-D9 shapes; only its label/footer changes.
  `Session` remains the class-scoped option (90-D7/46-I6).

Nothing in this errata changes L1's risk value, the destructive-verb guard, the
L2 requirement, the wire protocol, or the `force_ask` option set.

---

## 2. Verified current behavior

### 2.1 The scope walk starts `InRoot` and escalates monotonically

`ScopeResult` defaults to `InRoot` (`src/policy/shell_guard.cpp:1096-1099`).
`classify_segment_scope` (`:1691`) walks redirects, a `cd` branch, interpreter
and `find -exec` branches, the `git` branch, and then the generic loop
(`:1800-1820`): every non-option word is passed to `classify_scope_path` with
`force_path = false`. `note_scope` (`:1406-1412`) promotes `Unknown` over
`InRoot` and `Outside` over both; there is no de-escalation.

`classify_shell_command_with_scope` (`:2435-2471`) then overrides the scope from
the L1 risk: `Forbid -> Outside`, `Confirm -> Unknown`, otherwise the computed
scope. `RulePermissionPolicy::evaluate` maps `InRoot -> TargetClass::InRoot` and
`Unknown`/`NotApplicable -> TargetClass::Unknown`
(`src/policy/permission_policy.cpp:480-489`). A shell rule with a `path`
dimension matches only `TargetClass::InRoot` and only `path == "**"`
(`:169-181`). Therefore one bad token defeats an otherwise in-root command.

### 2.2 The `$` gate is not path-aware

The relevant lines are `src/policy/shell_guard.cpp:1371-1375`:

```cpp
if (is_home_ref(s, token.single_quoted)) {
    return ShellTargetScope::Outside;
}
if (token.had_unexpanded && s.find('$') != std::string::npos) {
    return ShellTargetScope::Unknown;
}
```

The gate runs before `path_punctuation` is computed and has no special-parameter
exemption. Two text-level details matter for the fix:

- `has_glob` returns true for `?` as well as `*`
  (`src/policy/shell_guard.cpp:1106-1108`). A naive "is the token path-like?"
  check therefore treats the `?` of `$?` as a glob and keeps `"exit: $?"`
  `Unknown`. The fix must strip recognized special parameters **before** the
  glob test, not merely special-case a two-character `$?` token.
- The `@` branch treats any non-empty token ending in `@` as a malformed curl
  form and returns `Unknown` (`:1317-1320`) before the `$` gate. `$@` is caught
  there unless the special-parameter scan runs first.

Measured on the current tree (probe built from
`classify_shell_command_with_scope`, kernel confinement on):

| Command | Risk | Scope today |
|---|---|---|
| `echo $?` | None | Unknown |
| `echo "exit: $?"` | None | Unknown |
| `echo $@` | None | Unknown (malformed `name@` branch) |
| `echo "exit: 0"` | None | InRoot |
| `cd <root> && grep -n "T7: deallocate" test/x.cpp; echo "exit: $?"` | None | Unknown |
| `echo $HOME` | None | Outside (via `is_home_ref`) |
| `echo $FOO` | None | Unknown |
| `cat foo$BAR` | None | Unknown (conservative; Rev 2 keeps it Unknown) |

The `$FOO`/`$HOME` results are intended: an unexpanded variable can name an
arbitrary path, and `$HOME` is outside. `$?`, `$$`, `$#`, `$@`, `$*`, `$!`,
`$-`, and `$1`..`$9` are scalar/empty shell expansions and cannot select a
path. `$0` is the exception: `/bin/bash -lc` sets it to `/bin/bash`, so it is
treated as `Unknown` (Ask), not neutral. The fix leaves named/command/backtick
expansions `Unknown` wherever they occur (H3), so `cat foo$BAR` remains
conservative rather than becoming `InRoot`.

### 2.3 The short-option branch cannot see an embedded value

`classify_scope_path` handles `--name=value` by recursing on the value
(`:1352-1359`), but a single-dash option is handled by the branch at
`:1348-1368` before any `=` split. Its unknown-option heuristic is textual:
`/`, `~`, `$`, or `..` anywhere in the token -> `Unknown`. Measured:

| Command | Scope today | Why |
|---|---|---|
| `cmake -S . -B build` | InRoot | `.` is in-root; other words neutral |
| `cmake --build build` | InRoot | no path punctuation |
| `cmake -DKEY=value -S . -B build` | InRoot | no punctuation in the token |
| `cmake -DKEY=a/b -S . -B build` | Unknown | `/` in `-DKEY=a/b` |
| `cmake -DCMAKE_INSTALL_PREFIX=./stage -S . -B build` | Unknown | `/` in the token |
| `cmake -DCMAKE_PREFIX_PATH=$HOME/x .` | Unknown | `/` and `$` in the token |
| `cmake -DCMAKE_CXX_FLAGS=-I../include .` | Unknown | `..` in the token |

`-DKEY=value` is neutral today; no existing test pins it (L4), and the new
`ShellScope.CmakeFormsInRoot` test will. The failing forms
are the ones where a CMake define or flag value is path-shaped. A value that is
provably under the root must not poison the command, and a value that is
outside must be `Outside` (Ask/Deny), not `Unknown` (Ask). The distinction
matters for user diagnosis and for rules that distinguish `Outside`.

### 2.4 `Always` is per-tool and the dialog is arrow-only

- `remember()` (`src/policy/permission_policy.cpp:545-662`) sets the `Always`
  class by tool: `shell`/`terminal` -> raw command text, `literal=true`,
  `target_class=Any` (`:603-611`, `:626-628`); a path tool ->
  `target_class=InRoot`, command cleared (`:581-591`); a non-path/MCP tool ->
  `target_class=Any`, command empty (tool-wide). The durable file stores
  `{tool, command, match:"literal", effect:"allow", id}` only
  (`src/config/grant_store.cpp:159-180`). "Always" therefore means an exact
  command only for command-bearing tools; the dialog label must say which class
  it is (M1). This is the documented 90-D9 behavior, not a bug in the fold.
- `handle_dialog` (`src/ui/supervisor.cpp:3552-3598`) resolves on `Escape`/
  `Ctrl-C` (Deny/Once), arrows (move), and `Return` (selected option). The final
  `return true` swallows every other key. The RB-12 decision removed the old
  `y`/`n`/digit shortcuts for a good reason (a slash command leaked `n` and
  silently denied). This errata narrows that swallow list by exactly eight
  characters and **re-opens the same accident class** for them: a `/rename ...`
  burst can answer `a`, and `/sessions` can answer `s`. 94-D14 accepts that
  risk explicitly and adds a PTY pin; no anti-accident claim is made (M2).
- The rendered option set (`src/ui/ui_render.cpp:1150-1174`) is
  `Allow once`, `Allow for session`, `Always allow`, `Deny`, index `0..3`, and
  the footer is the Unicode string at `src/ui/ui_render.cpp:1178`
  (`Up/Down select . Enter confirm . Esc cancel` is an ASCII description, not a
  byte pin). `force_ask` renders `Allow once`, `Deny` only.

---

## 3. Decisions

| ID | Decision |
|---|---|
| **94-D1** | **Only recognized special parameters are neutral; every other unexpanded expansion is `Unknown`.** After `is_home_ref`, a token is scanned left to right. A recognized non-path special parameter (`$?`, `$$`, `$#`, `$@`, `$*`, `$!`, `$-`, `$1`..`$9`, and their `${...}` forms) is stripped and is not path evidence. `$0`/`${0}` is **not** neutral: bash sets it to `/bin/bash`, so it is `Unknown`. Any other unexpanded `$` (named variable, `$(...)`, arithmetic substitution, `${...}` named form) or a backtick makes the whole token `Unknown`, **regardless of position or path punctuation**. This closes H3: `cat foo$BAR`/`foo${BAR}`/`foo$(...)`/backtick must not become `InRoot`. After the scan, the existing `force_path`/path-punctuation rules apply to the sanitized text; `$HOME`/`${HOME}` remain `Outside` through `is_home_ref`. |
| **94-D2** | **Special parameters are matched as substrings, before globs and the `@` branch.** The scan strips every recognized `$?`/`$@`/... occurrence, including inside `"exit: $?"` and `x$@`, and including `${?}`/`${@}`. `path_like` and `has_glob` are then computed on the stripped text, so the `?` of `$?` is not a glob (H1), and the `@` of `$@` is not a malformed curl form (H2). In the tool's non-interactive shell `$@`/`$*` are empty and `$1`..`$9` are scalar/empty. `$0` is `/bin/bash` (a path), so `$0`/`${0}` are handled as `Unknown`, not stripped. Named expansions are **not** special (94-D1). |
| **94-D3** | **Short options parse an embedded `=value`.** For a token with an option prefix (`-` or `--`) containing `=`, the substring after the first `=` is classified with `force_path=false`, i.e. recursively, before the expansion scan. This makes `-DKEY=a/b` resolve in-root, `-DKEY=../x` outside, `-DKEY=$HOME` outside via `is_home_ref`, and `-DKEY=$FOO` unknown, while `-DKEY=value` stays neutral. Single-dash tokens without `=` keep the existing attached-value/unknown-option behavior. |
| **94-D4** | **Scope is matching input only.** The L1 risk (`ShellCommandRisk`) and every SG invariant are untouched (89-SG1/SG5). A change that makes more commands `InRoot` can only let a rule that already matches `path:"**"` apply; it cannot suppress `Forbid`/`Confirm`, protected-path denies, `ReadOnly`, or `force_ask`. |
| **94-D5** | **Hotkey map.** In a four-option dialog: `A`/`a` = `AllowAlways` + `GrantScope::Always`; `O`/`o` = `Allow` + `Once`; `S`/`s` = `Allow` + `Session`; `D`/`d` = `Deny` + `Once`. The keys resolve directly; they do not move `dialog.selected`. |
| **94-D6** | **Case-insensitive, exact, ASCII-only allowlist.** Only the eight single-byte characters `A a O o S s D d` are accelerators. Every other printable character, including digits and non-ASCII input, is swallowed; the four bound keys are the accepted-risk exception (94-D14), not a retention of RB-12. |
| **94-D7** | **`force_ask` offers no persistent shortcut.** In a `force_ask` dialog only `O`/`o` (Allow/Once) and `D`/`d` (Deny/Once) resolve; `A`/`S` are swallowed and create no decision. This preserves 25-UX46 / 46-I6: a forced review never records a grant. |
| **94-D8** | **Rendered hints and the `Always` label are pinned and tool-class-aware.** The four-option footer adds exactly `[A] always \| [O] once \| [S] session \| [D] deny`; the `force_ask` footer adds exactly `[O] allow once \| [D] deny`. Row index 2 is labeled by tool class: `shell`/`terminal` -> `Always allow (exact command)`; a path tool (`tool_has_path_dimension`) -> `Always allow (all in-root paths)`; any other tool (MCP/non-path) -> `Always allow (this tool)`. The other three labels, the label order, and the source navigation footer are unchanged. |
| **94-D9** | **`Always` keeps the verified 90-D9 grant shapes.** For `shell`/`terminal` it writes the literal raw text (`literal=true`, `target_class=Any`, layer `LocalGrant`, id `grant:<tool>:<hash>`); for a path tool it writes `target_class=InRoot`, no command (all in-root paths for that tool); for a non-path/MCP tool it writes `target_class=Any`, no command (the whole tool). The class-scoped durable **shell prefix** alternative is **rejected**: it would let `Always` on `git status` also allow `git clean -fdx`, a control-plane command L1 does not classify (90-C10); the same widening for `rm`/`mv`/`make` is not justified by one approval. OQ-90-1 remains deferred. The UX problem is solved by the class-aware label (94-D8) and the footer's pointer to `S`. |
| **94-D10** | **`Session` is the class-scoped shell option.** A simple in-root shell command answered with `S` becomes the existing in-memory prefix/InRoot grant (90-D7) and is lost on daemon restart (46-I6). Compound, outside, unknown, and `terminal write` commands keep the existing exact Session behavior. `Once` persists nothing; `Deny` persists nothing. |
| **94-D11** | **No grants-schema change.** The durable file stays `ymh.permissions/1` with the existing `{tool, command, match, effect, id}` entry shape; the `Always` class is exactly the 90-D9/90-A15 split (exact for command-bearing tools, in-root-wide for path tools, tool-wide for non-path tools). |
| **94-D12** | **No wire or protocol change.** The UI still sends `{decision, scope}`; the daemon computes the Session class in `remember()` from the core request. 90-D12 is retained. |
| **94-D13** | **No new persistent state.** Scope classification is per-call; hotkey resolution is UI model state; Session remains daemon-memory; `Always` uses the existing grants file. |
| **94-D14** | **The RB-12 burst risk is accepted explicitly for `a/o/s/d`.** The accelerators are single printable keys, so a slash-command burst typed while a permission dialog is open can now resolve it (`/rename ...` reaches `a` -> `AllowAlways`; `/sessions` reaches `s` -> `Allow/Session`). This is a deliberate, user-requested trade-off, not an anti-accident guarantee. Mitigations retained: only these four keys are bound, `A`/`S` are inert on `force_ask`, all other printables are swallowed, and the composer still receives no bytes. The PTY test `UiSupervisorPty.PermissionDialogSlashBurstAcceptedRisk` pins the behavior; the `S` grant widens to the in-memory prefix, which is the 90-D7 risk the class-scoped durable option is refused in 94-D9. |


---

## 4. Fix design

### 4.1 Scope algorithm (ASCII)

```
classify_scope_path(text, token, root, cwd, force_path, git_context):
  if text empty: return neutral
  if is_home_ref(text, token.single_quoted): return Outside
  if text has an option prefix and "=":
      return classify_scope_path(value_after_first_equals, false)   # 94-D3
  if token.had_unexpanded:
      scan := scan_expansions(text)
      if scan.unknown: return Unknown      # $VAR/$()/backtick/$0 (H3)
      text := scan.sanitized               # strip $?/$@/$1..$9 (H1/H2)
      if text empty: return neutral
  URI / scp / @ / env-assignment / option-without-= handling unchanged
  path_like := text == "." || text == ".." ||
               text starts_with ".." || text contains "/" ||
               text has glob || text starts with "~" ||
               text starts with "$"
  ... existing path punctuation / glob / resolve_lexical ...
```

`scan_expansions` (substring-aware, replaces the Rev 1 exact two-character
helper):

```cpp
struct ExpansionScan {
    bool        unknown = false;   // named/command/backtick/$0: cannot prove a path
    std::string sanitized;         // recognized non-path specials removed
};

bool is_neutral_special_parameter_char(char c) {
    return c == '?' || c == '$' || c == '#' || c == '@' || c == '*' ||
           c == '!' || c == '-' || (c >= '1' && c <= '9');
}

ExpansionScan scan_expansions(std::string_view s) {
    ExpansionScan out;
    out.sanitized.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') {                       // command substitution
            out.unknown = true;
            out.sanitized.push_back(s[i]);
            continue;
        }
        if (s[i] != '$' || i + 1 >= s.size()) {
            out.sanitized.push_back(s[i]);
            continue;
        }
        const char c = s[i + 1];
        if (c == '0') {                          // bash -lc sets $0=/bin/bash (a path)
            out.unknown = true;
            continue;
        }
        if (is_neutral_special_parameter_char(c)) {  // $? $$ $# $@ $* $! $- $1..$9
            ++i;                                     // strip the two characters
            continue;
        }
        if (c == '{') {                          // ${?}, ${@}, ${#}, ${1}..${9}
            const std::size_t close = s.find('}', i + 2);
            if (close != std::string_view::npos) {
                const std::string_view inner = s.substr(i + 2, close - i - 2);
                if (inner.size() == 1 && inner[0] == '0') {
                    out.unknown = true;          // ${0} is the shell path
                    i = close;
                    continue;
                }
                if (inner.size() == 1 &&
                    is_neutral_special_parameter_char(inner[0])) {
                    i = close;                   // strip the whole ${...}
                    continue;
                }
            }
        }
        out.unknown = true;                      // named, $(...), $((...)), ...
        out.sanitized.push_back(s[i]);
    }
    return out;
}
```

`classify_scope_path` sketch (only the changed region):

```cpp
std::optional<std::string_view> option_equals_value(std::string_view s) {
    if (s.size() < 2 || s.front() != '-') {
        return std::nullopt;
    }
    const std::size_t eq = s.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    return s.substr(eq + 1);
}

std::optional<ShellTargetScope> classify_scope_path(
    std::string_view text, const Token& token, const std::filesystem::path& root,
    const std::filesystem::path& cwd, bool force_path, bool git_context) {
    std::string s{text};
    if (s.empty()) {
        return std::nullopt;
    }
    if (is_home_ref(s, token.single_quoted)) {
        return ShellTargetScope::Outside;
    }
    if (const auto value = option_equals_value(s); value.has_value()) {
        return classify_scope_path(*value, token, root, cwd, false, git_context);
    }
    if (token.had_unexpanded) {
        ExpansionScan scan = scan_expansions(s);
        if (scan.unknown) {                          // 94-D1 (H3)
            return ShellTargetScope::Unknown;
        }
        s = std::move(scan.sanitized);               // 94-D2 (H1/H2)
        if (s.empty()) {
            return std::nullopt;
        }
    }
    // ... existing URI/scp/@/env-assignment block ...
    // ... existing single-dash branch without '=' (the '=' split moved above) ...
    // ... existing path punctuation / glob / resolve_lexical ...
}
```

Ordering notes:

- The option-`=` split runs before the expansion scan so that
  `-DKEY=$HOME` recurses on `$HOME` and remains `Outside`, while
  `-DKEY=$FOO` recurses and becomes `Unknown`.
- `is_home_ref` runs before the scan so `$HOME`/`${HOME}` remain `Outside`.
- The scan runs before the `@`/scp/URI and glob branches, so `$@` is not a
  malformed curl form (H2) and the `?` of `$?` is not a glob (H1).
- Any named/command/backtick expansion, and `$0`/`${0}`, is `Unknown`
  regardless of position (H3). `echo foo$bar` is therefore a conservative Ask,
  not `InRoot`; the earlier Rev 1 "non-path `$` neutral" promise applied only to
  recognized non-path special parameters. `$0` is path-capable: `/bin/bash
  -lc` sets it to `/bin/bash`, so `cat $0` must not become `InRoot`.
- `FOO=$BAR` (no option prefix) reaches the scan, is named, and becomes
  `Unknown`; `FOO=$?` is stripped to `FOO=` and is neutral.

### 4.2 Truth table

All rows use a canonical workspace root and `kernel_confinement = true`.

| Input | Scope after fix | Matching class |
|---|---|---|
| `cmake -S . -B build` | InRoot | InRoot |
| `cmake --build build` | InRoot | InRoot |
| `cmake --build build -j` | InRoot | InRoot |
| `cmake -DKEY=value -S . -B build` | InRoot | InRoot |
| `cmake -DCMAKE_BUILD_TYPE=Debug -B build .` | InRoot | InRoot |
| `cmake -DKEY=a/b -S . -B build` | InRoot | InRoot |
| `cmake -DCMAKE_INSTALL_PREFIX=./stage -B build .` | InRoot | InRoot |
| `cmake -DCMAKE_PREFIX_PATH=$HOME/x .` | Outside | Outside |
| `cmake -DKEY=$HOME .` | Outside | Outside |
| `cmake -DKEY=$FOO .` | Unknown | Unknown |
| `cmake -DKEY=$? .` | InRoot | InRoot |
| `cmake -DKEY=$@ .` | InRoot | InRoot |
| `cmake -DCMAKE_CXX_FLAGS=-I../include .` | Outside or Unknown (Ask) | Ask |
| `echo $?` | InRoot | InRoot |
| `echo "$?"` | InRoot | InRoot |
| `echo $@` | InRoot | InRoot |
| `echo "$@"` | InRoot | InRoot |
| `echo x$@` | InRoot | InRoot |
| `echo ${?} ${@} $# $* $! $- $1 $9` | InRoot | InRoot |
| `echo $0` / `echo ${0}` | Unknown (Ask) | Unknown |
| `echo "exit: $?"` | InRoot | InRoot |
| `echo "status: $? (done)"` | InRoot | InRoot |
| `echo foo$bar` | Unknown (Ask) | Unknown |
| `cat foo$BAR` | Unknown (Ask) | Unknown |
| `cat foo${BAR}` | Unknown (Ask) | Unknown |
| `cat foo$(echo eA== \| base64 -d)` | Unknown (Ask) | Unknown |
| `cat foo<backtick>echo etc<backtick>` | Unknown (Ask) | Unknown |
| `echo $FOO` | Unknown | Unknown |
| `echo $HOME` | Outside | Outside |
| `cd <root> && grep -n "T7: deallocate" test/x.cpp; echo "exit: $?"` | InRoot | InRoot |
| `cat /etc/passwd` | Outside | Outside |
| `cat ../x` | Outside | Outside |
| `cat ~/.ssh/id_rsa` | Outside | Outside |
| `rm -rf build; echo "exit: $?"` | InRoot | InRoot (L1 risk None, L2 on) |
| `rm -rf ../x; echo "exit: $?"` | Outside, risk Forbid | Deny |

The last two rows are the safety check. The fix does not change L1: `rm -rf
build` is already a bounded in-root operation that `path:"**"` may allow (specs
89/90); `rm -rf ../x` is still `Forbid`. A compound in-root destructive command
followed by `echo $?` now matches the in-root rule, which is the intended
consequence of the user's own in-root policy. SG5 is unaffected because the
destructive classification comes from `classify_impl`, not from the scope walk.

### 4.3 Dialog key handling

Decision order inside `handle_dialog` (first match wins):

```
Escape / Ctrl-C       -> Deny, Once
ArrowUp / ArrowDown   -> move selected (unchanged, wraps within 2 or 4)
Return                -> resolve selected (unchanged)
O / o                 -> Allow, Once
D / d                 -> Deny, Once
A / a                 -> if !force_ask: AllowAlways, Always
S / s                 -> if !force_ask: Allow, Session
any other key         -> swallow, return true
```

`A` and `S` in `force_ask` are swallowed rather than mapped to `Deny`; mapping
them to `Deny` would make an accidental keypress destructive and would differ
from the existing "unbound keys do nothing" rule. They are also not mapped to
`Allow` because the only non-persistent allow is `O`.

Sketch:

```cpp
bool handle_dialog(const ftxui::Event& event) {
    if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
        resolve_dialog(payload::PermissionDecisionKind::Deny, GrantScope::Once);
        return true;
    }
    const int option_count = model_.dialog.force_ask ? 2 : 4;
    if (event == ftxui::Event::ArrowUp) {
        model_.dialog.selected = (model_.dialog.selected + option_count - 1) % option_count;
        return true;
    }
    if (event == ftxui::Event::ArrowDown) {
        model_.dialog.selected = (model_.dialog.selected + 1) % option_count;
        return true;
    }
    if (event == ftxui::Event::Return) {
        // unchanged: force_ask 0 => Allow/Once, 1 => Deny/Once;
        // four-option 0 => Allow/Once, 1 => Allow/Session,
        // 2 => AllowAlways/Always, 3 => Deny/Once.
        return resolve_selected();
    }
    if (event.is_character()) {
        const std::string value = event.character();
        if (value.size() == 1) {
            const char key =
                static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
            switch (key) {
                case 'o':
                    resolve_dialog(payload::PermissionDecisionKind::Allow,
                                   GrantScope::Once);
                    return true;
                case 'd':
                    resolve_dialog(payload::PermissionDecisionKind::Deny,
                                   GrantScope::Once);
                    return true;
                case 'a':
                    if (!model_.dialog.force_ask) {
                        resolve_dialog(payload::PermissionDecisionKind::AllowAlways,
                                       GrantScope::Always);
                    }
                    return true;
                case 's':
                    if (!model_.dialog.force_ask) {
                        resolve_dialog(payload::PermissionDecisionKind::Allow,
                                       GrantScope::Session);
                    }
                    return true;
                default:
                    return true;   // RB-12: every other printable is swallowed
            }
        }
    }
    return true;
}
```

`resolve_dialog` already records `last_dialog_resolution_`, closes the dialog,
and forwards to `resolvePermission`; no new state is needed. Because the
accelerators call it directly, they behave identically to selecting the row and
pressing Return (including `exit_plan_mode` handling and the broker's
`remember` call).

### 4.4 Rendering

Keep the option rows, indexes, and source navigation footer; make the `Always`
label tool-class-aware and add one dim hint row per dialog. `ui_render.cpp`
already links `ymh::policy`, so it can use
`tool_has_path_dimension` (`include/ymh/policy/path_target.hpp:21`).

```cpp
#include "ymh/policy/path_target.hpp"

std::string always_label(std::string_view tool) {
    if (tool == "shell" || tool == "terminal") {
        return "Always allow (exact command)";        // 90-D9 exact
    }
    if (tool_has_path_dimension(tool)) {
        return "Always allow (all in-root paths)";    // 90-D9 InRoot-wide
    }
    return "Always allow (this tool)";                // MCP/non-path tool-wide
}

// four-option dialog
const std::string always = always_label(dialog.tool);
const std::string options[] = {
    "Allow once",
    "Allow for session",
    always,
    "Deny",
};
// after the existing navigation footer
rows.push_back(ftxui::text(
    "[A] always | [O] once | [S] session | [D] deny") | ftxui::dim);

// force_ask dialog: options stay {"Allow once", "Deny"}
rows.push_back(ftxui::text(
    "[O] allow once | [D] deny") | ftxui::dim);
```

The source navigation footer at `src/ui/ui_render.cpp:1178` is unchanged. The
ASCII string `Up/Down select . Enter confirm . Esc cancel` in this spec is a
description of that Unicode footer, not a byte pin (L1). The other three option
labels and the option order are unchanged. The hints are ASCII. The longest
label (32 columns for `all in-root paths`) and the hint line (46 columns) both
fit the existing centered window at 80 columns; the golden test pins that width
(94-F20).

### 4.5 `Always` durability: the 90-D9 class, per tool

**Chosen semantics: the 90-D9 class is retained, and the label names it.**

"Always" is not one class. `remember()` (`src/policy/permission_policy.cpp:545-662`)
writes:

| Tool class | Durable `Always` shape | Label |
|---|---|---|
| `shell` / `terminal` | literal raw `shell_guard_text`, `target_class=Any`, id `grant:<tool>:<hash>` | `Always allow (exact command)` |
| path tool (`read_file`, `write_file`, `edit_file`, `grep`, `glob`, ...) | `target_class=InRoot`, no command (all in-root paths for that tool), `literal=true` | `Always allow (all in-root paths)` |
| non-path/MCP tool | `target_class=Any`, no command (the whole tool) | `Always allow (this tool)` |

The grant is loaded from `<workspace>/.ymh/permissions.jsonc` at policy
construction and written by `remember()`. For `shell`/`terminal`, a second
command with any different byte asks again: the user approved **that** command,
not a family. For a path tool the approval is the in-root class; for an MCP tool
it is the tool. A single hard-coded "exact command" label was false for the
latter two classes (M1).

This errata does **not** widen the shell class to the Session prefix class.
Reasons:

1. **Control-plane verbs.** A durable in-root prefix grant for `git` would make
   `Always` on `git status` also authorize `git clean -fdx`, which L1 does not
   classify as destructive (90-C10: no `git` branch in the catalogue). The same
   holds for `make`, `cmake`, and script runners, which execute arbitrary
   recipes. One approval is not consent for the family across daemon restarts.
2. **Verified contract.** 90-D9, 46-I5, 90-A15, and PS12 pin the class split;
   OQ-90-1 deliberately deferred the schema bump. A UX label does not justify
   reopening a verified security decision.
3. **A class-scoped shell option already exists.** `S` (`Allow for session`)
   produces the in-memory prefix/InRoot grant for simple in-root commands
   (90-D7). It is the right choice when the user wants the family for the
   current daemon; it disappears on restart. The footer makes that discoverable,
   and 94-D14 records the RB-12 burst risk that `S` re-opens.

What changes is only the presentation: the row names the actual class, and the
footer shows the four keys. A user who wants the shell family presses `S`; a
user who wants durability for one shell command presses `A` and is no longer
surprised when a different command asks. A `write_file` user sees that `A`
covers all in-root paths, and an MCP user sees that `A` covers the tool.

State and failure behavior are unchanged from 90-D9/46-D2:

- `remember()` still refuses `force_ask`, protected targets, temp-exempt
  requests, and `Deny`/`Once`.
- A failed append keeps the in-memory grant but degrades it to a session grant
  (46-I7); it is never presented as durable.
- The file is written under `flock(LOCK_EX | LOCK_NB)` with `write -> fsync ->
  rename -> directory fsync`, mode `0600` (`src/config/grant_store.cpp:217-309`).
  A non-blocking lock contention returns `false`, which is the append-failure
  path (degrade to Session), not a retry (L6).

### 4.6 Grants file shape (unchanged)

`FileGrantStore` is constructed by the daemon at
`src/host/workspace_host.cpp:855-856` and injected into the runtime. The schema
and entry shape are exactly the verified 90-A15 shape; this errata adds no
field.

```jsonc
{
  "schema": "ymh.permissions/1",
  "workspace_id": "ws_01J...",
  "grants": [
    {
      "tool": "shell",
      "command": "grep -n \"T7: deallocate\" test/cct/command_completion_tracker_test.cpp",
      "match": "literal",
      "effect": "allow",
      "id": "grant:shell:1a2b3c4d5e6f7081"
    },
    {
      "tool": "write_file",
      "match": "literal",
      "effect": "allow",
      "id": "grant:write_file:0011223344556677"
    }
  ]
}
```

Field rules (retained):

| Field | Values | Meaning |
|---|---|---|
| `schema` | `"ymh.permissions/1"` | Frozen; any other value skips the whole file (fail-safe, re-ask). |
| `workspace_id` | string | Must equal the daemon's registry id; a mismatch skips the whole file. |
| `tool` | non-wildcard string | Wildcard tools remain rejected (90-A15/H1). |
| `command` | string | Required for `shell`/`terminal`; empty/absent for path tools. |
| `match` | `"literal"` or `"glob"` | `Always` from `remember` is literal; hand-authored `glob` remains bounded and rejected if it has no literal character. |
| `effect` | `"allow"` | Grants only allow; deny lives in config rules. |
| `id` | string | Stable id; append replaces an entry with the same id. |

`Session` is not in this file. It lives in
`RulePermissionPolicy::session_grants_`, keyed by `SessionId`, and is lost on
daemon restart (46-I6). `Once` and `Deny` write nothing.

### 4.7 Protected paths, temp exemption, and force_ask

The existing guards in `remember()` are retained in this order:

1. `force_ask` -> `{}` (no grant; `A`/`S` are unreachable in the forced dialog
   by 94-D7 anyway).
2. `Deny` or `Once` -> `{}`.
3. request path protected -> `{}`.
4. shell classification protected target -> `{}`.
5. temp-exempt -> `{}`.

An `Always` grant is constructed only after those checks. The temp-root exemption
still short-circuits `evaluate` before the rule fold, so `Always` on a temp
write never becomes a durable workspace grant.

---

## 5. Invariants

| ID | Invariant |
|---|---|
| **94-I1** | `ScopeResult` still starts `InRoot`; only explicit evidence escalates. |
| **94-I2** | A token with no path punctuation and no unexpanded named/command expansion is neutral; it cannot change the command's scope. |
| **94-I3** | Recognized non-path special parameters (`$?`, `$$`, `$#`, `$@`, `$*`, `$!`, `$-`, `$1`..`$9`, and `${?}`/`${@}`/... forms) are matched as substrings and are neutral in every position (word, option value, env-assignment value), including `"exit: $?"` and `x$@`. Their `?`/`@` characters are stripped before glob and curl-form checks. `$0`/`${0}` is not neutral: it is `Unknown` because `/bin/bash -lc` sets it to `/bin/bash`. |
| **94-I4** | Any named, command-substitution, or backtick expansion (`$FOO`, `${FOO}`, `$FOO/x`, `foo$BAR`, `foo${BAR}`, `$(...)`, backtick) is `Unknown` regardless of position or path punctuation; `$HOME`/`${HOME}` remain `Outside`. |
| **94-I5** | `..`, an absolute outside path, and a leading `~` never become `InRoot`; they are `Outside` (or Ask/Deny), never Allow through a path rule. |
| **94-I6** | An option-prefixed token containing `=` classifies its value before the expansion scan; `-DKEY=a/b` is `InRoot`, `-DKEY=../x` is `Outside`, `-DKEY=$HOME` is `Outside`, `-DKEY=$FOO` is `Unknown`, `-DKEY=$?`/`-DKEY=$@` are neutral. |
| **94-I7** | L1 risk is unchanged: the scope walk cannot return `None` for a destructive verb whose operands are unresolved/ambiguous (89-SG5), and cannot turn `Forbid`/`Confirm` into Allow. |
| **94-I8** | A shell `path` rule matches only `TargetClass::InRoot` and only `path == "**"`; `"./**"` remains a path-tool pattern, not a shell one (documented limitation). |
| **94-I9** | The scope classifier remains pure, total, I/O-free, and does not stat/realpath (89-SG1). |
| **94-I10** | In a four-option dialog, `A/a`, `O/o`, `S/s`, `D/d` produce exactly `AllowAlways/Always`, `Allow/Once`, `Allow/Session`, `Deny/Once`. |
| **94-I11** | The mapping is case-insensitive for those eight ASCII characters and does not apply to any other character. |
| **94-I12** | Every unbound printable key is swallowed (`return true`) and does not resolve, move the selection, or reach the composer. The four bound keys are the accepted-risk exception (94-D14); no broader printable binding exists. |
| **94-I13** | Arrows, Enter, Esc, and Ctrl-C keep their existing behavior and precedence. |
| **94-I14** | In a `force_ask` dialog, `A`/`S` are swallowed and never create a grant; `O` = Allow/Once and `D` = Deny/Once. |
| **94-I15** | The accelerators resolve directly from any selected row; `dialog.selected` is not required to move. |
| **94-I16** | `Always` keeps the three 90-D9 classes: `shell`/`terminal` -> `literal=true`, `target_class=Any`, identical `shell_guard_text` only; path tool -> `target_class=InRoot`, no command (all in-root paths); non-path/MCP tool -> `target_class=Any`, no command (whole tool). No shell prefix grant is created. |
| **94-I17** | `Session` is the only class-scoped option: a simple in-root, non-`terminal write` command becomes the existing in-memory prefix/InRoot grant; it is lost on daemon restart (90-D7/46-I6). |
| **94-I18** | The durable grant file is written under `flock(LOCK_EX \| LOCK_NB)`, atomically, mode `0600`, with a matching `workspace_id`; a mismatch or parse failure loads `{}` and re-asks; lock contention is an append failure, not a retry. |
| **94-I19** | A failed durable append keeps the grant only as a `SessionGrant` (46-I7); it never claims durability. |
| **94-I20** | `Once` and `Deny` persist nothing; `force_ask` never persists (46-I6). |
| **94-I21** | No wire/protocol field changes; the UI still sends only `{decision, scope}` (90-D12). |
| **94-I22** | An exact `Always` grant cannot match a different command, and the path-tool `InRoot` class cannot match an outside path. |
| **94-I23** | The grants schema stays `ymh.permissions/1` with the existing keys; no `scope`/`prefix` field is written or read. A foreign schema skips the whole file (fail-safe). |
| **94-I24** | The four-option dialog renders a tool-class-aware row index 2: `Always allow (exact command)` for `shell`/`terminal`, `Always allow (all in-root paths)` for a path tool, `Always allow (this tool)` otherwise; the other labels/order are unchanged and the ASCII hint lines are exactly those in 94-D8. |
| **94-I25** | The constructor's existing grant normalization is unchanged: no prefix grant exists to preserve, and a non-shell wildcard/empty-command grant is still rejected (90-A15). |
| **94-I26** | The four accelerators knowingly re-open the RB-12 burst accident; this is accepted in 94-D14, is limited to `a/o/s/d`, and is pinned by `UiSupervisorPty.PermissionDialogSlashBurstAcceptedRisk`. No spec text claims RB-12 is preserved. |

---

## 6. Failure modes

| ID | Failure | Cause | Mitigation / test |
|---|---|---|---|
| **94-F1** | A fully in-root command asks because of `"exit: $?"` | `$` token escalated to `Unknown`; the `?` also matched the glob test | Strip recognized special parameters before the glob test; 94-D1/D2/94-I3; `ShellScope.SpecialParameterTrailer` |
| **94-F2** | `echo $?` or `echo $@` asks | Special parameter treated as a variable / malformed curl form | Substring scan before `@` and glob; 94-D2/94-I3; `ShellScope.SpecialParametersNeutral` |
| **94-F3** | A named expansion is neutralized and an outside variable path is auto-allowed | Gate removed rather than narrowed, or only the leading-`$` form guarded | Any named/command/backtick expansion is `Unknown` regardless of position; 94-D1/94-I4; `ShellScope.NamedVariableRemainsUnknown` and `ShellScope.EmbeddedNamedExpansionUnknown` |
| **94-F4** | `$HOME` becomes `Unknown` instead of `Outside` | Special check placed before `is_home_ref` | 94-I4; `ShellScope.HomeRemainsOutside` |
| **94-F5** | `-DKEY=a/b` still asks | Short-option `=` split missing | 94-D3/94-I6; `ShellScope.CmakePathLikeDefine` |
| **94-F6** | `-DKEY=../x` becomes `InRoot` | `=` value not resolved relative to root | 94-I5/94-I6; `ShellScope.CmakeOutsideDefine` |
| **94-F7** | A destructive compound with `$?` is allowed through a path rule | Scope-level fix + user's in-root policy | L1 remains authoritative; `rm -rf ../x; echo $?` is Forbid; `rm -rf build` is the already-intended bounded in-root allow. Test `ShellScope.DestructiveTrailerKeepsGuard` |
| **94-F8** | `A` on a forced review creates a durable grant | Hotkey bypasses `force_ask` check | 94-D7/94-I14; `PermHotkeys.ForceAskOnlyOnceAndDeny` |
| **94-F9** | A slash command typed in the dialog silently resolves | Broad printable mapping, or the four accelerators | Exact eight-character allowlist, all else swallowed; the four-key burst risk is **accepted** (94-D14) and pinned by the PTY test; no anti-accident claim. The existing `RB12_PermissionDialogResolvesOnlyOnEnter` corpus is retained unchanged (L5). |
| **94-F10** | `A` and `O` are swapped | Mapping typo | 94-I10; `PermHotkeys.FourOptionMapping` |
| **94-F11** | Lowercase does not work | Case-sensitive compare | 94-I11; `PermHotkeys.CaseInsensitive` |
| **94-F12** | Hotkey moves the highlight and then Enter double-resolves | Resolve does not close/clear state | `resolve_dialog` closes; `PermHotkeys.NoSelectionMove` |
| **94-F13** | A user picks `Always` twice on two different shell commands and is asked twice | Exact-command semantics for `shell`/`terminal` (90-D9) | Intended; the row says `Always allow (exact command)` and the footer points to `S`; test `AlwaysGrant.ExactIsIntended` |
| **94-F14** | A user wants a shell family but presses `A` | `A` is exact for shell, `S` is the class | Footer labels both; docs pin the distinction (94-D9/D10); `PermHotkeys.HintsRendered` |
| **94-F15** | `Always` on `terminal write` auto-allows future input chunks | Prefix grant mistakenly applied | No prefix is ever created; 94-I16; `AlwaysGrant.TerminalWriteStaysExact` |
| **94-F16** | A hand-authored `/2` file is partially applied by a binary that only knows `/1` | Schema tolerance | Whole-file schema check -> `{}`; 94-I23; `AlwaysGrant.ForeignSchemaRejected` |
| **94-F17** | A durable append fails but the UI shows it as durable | In-memory LocalGrant retained | Degrade to Session in memory; 94-I19; `AlwaysGrant.AppendFailureDegrades` |
| **94-F18** | Two identical `Always` grants duplicate or collide | Id built from tool+command only | Append replaces the entry with the same id; the rule is idempotent; `AlwaysGrant.RepeatIsIdempotent` |
| **94-F19** | A grant from another workspace applies | Workspace id not checked | `parse_grants` compares `workspace_id`; 94-I18; `AlwaysGrant.WorkspaceIdMismatch` |
| **94-F20** | The longer `Always` label or hint wraps and hides options at 80 columns | Long text | The longest label is 32 columns and the hint 46; both fit the 80-column dialog; golden/render test (`PermHotkeys.HintsRendered`) |
| **94-F21** | A rule with `path:"**"` matches a shell command whose only path is `.ymh` | Protected check independent of scope | Protected deny precedes the fold (90-D13); `ShellScope.ProtectedStillDenied` |
| **94-F22** | A non-ASCII key (e.g. a pasted letter) resolves | Byte compare | `value.size() == 1` ASCII gate; 94-I11; `PermHotkeys.OtherPrintableSwallowed` |
| **94-F23** | An exact `Always` grant is misdocumented as a class grant | Copy drift between UI, spec, and store | 94-I16/I24 and the golden/state tests; the doc explicitly rejects the class option |
| **94-F24** | An embedded named expansion (`cat foo$BAR`, `foo${BAR}`, `$(...)`, backtick) becomes `InRoot` and is auto-allowed | Only leading-`$` tokens were guarded | Scan every unexpanded named/command/backtick occurrence and return `Unknown` regardless of position; 94-D1/94-I4; `ShellScope.EmbeddedNamedExpansionUnknown` |
| **94-F25** | The `Always` label lies for a path or MCP tool | One hard-coded `(exact command)` label | Tool-class-aware label via `tool_has_path_dimension`; 94-D8/94-I24; `PermHotkeys.HintsRendered` and `AlwaysGrant.LabelIsToolClassAware` |
| **94-F26** | A `/rename` burst answers the permission dialog via `a` | Single-key accelerators re-open RB-12 | Accepted explicitly in 94-D14; limited to four keys; pinned by `UiSupervisorPty.PermissionDialogSlashBurstAcceptedRisk`; no anti-accident claim |

---

## 7. State lifetime

| State | Created | Destroyed / evicted | Owner | Survives restart | Survives reconnect | Survives session switch | Crash path |
|---|---|---|---|---|---|---|---|
| Per-call `ShellClassification` (risk/scope/protected/tmp) | Each `evaluate` | End of call | Stack | No | No | No | Recomputed; no state |
| `dialog.open`, `dialog.selected`, `dialog.force_ask` | `PermissionRequested` -> `UiModel::apply` | `resolve_dialog` / cancel / daemon loss | Supervisor UI model | No | No (new request reopens) | Yes while open (single dialog) | Dialog closes with the UI; broker denies/expires pending |
| `last_dialog_resolution_` | `resolve_dialog` | Next `open_permission_dialog` resets it | Supervisor harness seam | No | No | No | Test-only; no durable effect |
| `session_grants_[session]` (Session) | `remember(..., Session)` | Policy destruction / daemon exit | `RulePermissionPolicy` (daemon) | **No** (46-I6) | No (daemon owns it) | Yes within the same daemon; not shared across sessions | Lost; next call asks |
| `local_grants_` (Always) | Policy constructor load + `remember(..., Always)` | Policy destruction / file reload | `RulePermissionPolicy` (daemon) | **Yes**, via the file | Yes (in-memory; reloaded by a new daemon) | Yes (workspace-wide) | In-memory copy lost; the file is the durable source |
| `<ws>/.ymh/permissions.jsonc` | First successful Always append | Never automatically; user may delete | Workspace daemon / `FileGrantStore` | **Yes** | Yes | Yes | Atomic rename; a partial temp file is unlinked; parse failure -> `{}` |
| Pending permission request | `PermissionBroker::resolve` | Decision / timeout / cancel / shutdown | Broker (daemon) | No | No | Per session | Timeout/deny (fail-safe) |

The classifier adds no state. This errata adds no durable field: the grants
file keeps the 90-A15 shape. The only new state is the UI label/hint, and the
existing Session map is unchanged.

---

## 8. dsh mapping (56-D6)

Every non-mirror row carries a concrete reason and anchor. A cell that merely
says "not in scope" is not acceptable.

| Behaviour | dsh | ymh disposition | Reason and anchor |
|---|---|---|---|
| L1 destructive-command classifier | dsh does not ship a shell-text parser; it delegates to the provider/sandbox layer | **Non-mirror (ymh addition)** | The incident (`rm -fr build ~`) requires a fail-safe pre-permission clamp; L2 alone is the boundary, L1 is advisory. Anchors: `docs/design/89-shell-command-safety-guard-errata.md:107` (89-D1), `:857-870` (SG1-SG17). |
| Scope walk: special parameters neutral, named/command expansions `Unknown` | No dsh analogue (no target scope) | **Non-mirror (refinement)** | Non-path special parameters (`$?`/`$@`/`$1..$9`, ...) cannot select a path and are stripped before the glob/`@` checks; named/command/backtick expansions and `$0` (which is `/bin/bash`) can, so they stay `Unknown` even when embedded. The risk value is unchanged, so SG5 still forbids `None` for an unresolved **destructive** operand. Anchors: `docs/design/89-shell-command-safety-guard-errata.md:861` (SG5), `src/policy/shell_guard.cpp:1371-1375`, `:1106-1108` (the `?` glob). |
| Short-option `=value` parsing | No dsh analogue | **Non-mirror (refinement)** | Same reason: scope is matching input only; `Outside`/`Unknown` still Ask. Anchor: `docs/design/90-path-scoped-permissions-errata.md:1502` (90-D5), `src/policy/shell_guard.cpp:1348-1368`. |
| Single-key dialog accelerators `A/O/S/D` | dsh presents a selectable approval prompt; it does not define these accelerators | **Non-mirror (presentation, accepted risk)** | Terminal-first UX and the user's explicit request; no protocol change. The four keys **re-open** the RB-12 burst accident (`/rename` -> `a`), which 94-D14 accepts explicitly and a PTY test pins; the spec makes no anti-accident claim. Anchors: `docs/design/90-path-scoped-permissions-errata.md:1609` (90-D12 wire unchanged), `docs/design/REQUIREMENTS_BACKLOG.md:366-394` (RB-12), `tests/unit/ui_supervisor_pty_test.cpp:751-794` (the burst). |
| `force_ask` offers no persistent accelerator | dsh's generic provider does not invent a persistent option either | **Mirror** | 25-UX46/46-I6 already forbid a grant on a forced review; `A`/`S` are swallowed. Anchors: `docs/design/25-ui-ux-errata.md:2052` (UX46), `docs/design/46-permissions-ui-errata.md:3467` (46-I6). |
| `Always` keeps the three 90-D9 classes, with a tool-class-aware label | dsh persists an approval outside the process; it does not define an exact-vs-class distinction | **Non-mirror (retained ymh shape)** | The classes are exact command (shell/terminal), all in-root paths (path tool), and whole tool (MCP/non-path); the label names the actual class (M1). Keeping 90-D9 is deliberate: a durable in-root shell prefix for `git` would also authorize `git clean -fdx`, which L1 does not classify (90-C10); OQ-90-1 stays deferred. Anchors: `docs/design/90-path-scoped-permissions-errata.md:1579-1591` (90-D9), `:122` (90-C10), `:3380-3385` (OQ-90-1), `docs/design/46-permissions-ui-errata.md:3466` (46-I5), `src/policy/permission_policy.cpp:581-591`, `:626-628`. |
| Durable grants live in a daemon-owned file | dsh records approvals outside the process | **Mirror** | 46-D2 already injects a daemon-owned `GrantStore` with `flock` + atomic replace. Anchor: `docs/design/46-permissions-ui-errata.md:3582-3590`, `src/host/workspace_host.cpp:855-868`. |
| `path:"**"` is the only shell path pattern | No dsh analogue | **Non-mirror (limitation, documented)** | A shell command has no single resolved target; the narrow pattern keeps the guarantee honest and avoids implying per-target matching. Anchor: `docs/design/90-path-scoped-permissions-errata.md:1502` (90-D5), `:1615` (90-D13), `src/policy/permission_policy.cpp:169-181`. |
| Session is in-memory and lost on restart | dsh keeps process-local pending state similarly | **Mirror** | 46-I6; Session is a daemon-lifetime grant. Anchor: `docs/design/46-permissions-ui-errata.md:3467`. |

The presentation and classifier rows are the only divergences introduced here;
neither changes the wire, the policy order, or the L1/L2 boundary.

---

## 9. Supersession and amendment map

| Target | Kind | What changes |
|---|---|---|
| `89-shell-command-safety-guard-errata.md` SG1-SG17 | by reference | No change to risk, Forbid/Confirm, L2, or the fail-safe rule. The scope refinement is matching-only (94-D4). |
| `90-path-scoped-permissions-errata.md` 90-D5/90-D7 | amend | The `$` gate and the short-option value are part of the scope proof; `InRoot` now covers the cmake and `$?` cases. |
| `docs/design/90-path-scoped-permissions-errata.md` 90-D9 | retain | `Always` keeps the three classes (exact command for shell/terminal, `InRoot`-wide for a path tool, tool-wide for a non-path tool); only the label becomes class-aware, not the grant. |
| `docs/design/90-path-scoped-permissions-errata.md` 90-A15 | retain | The grants file keeps `{tool, command, match, effect, id}`; no `scope`/`prefix` field. |
| `docs/design/90-path-scoped-permissions-errata.md` PS12 | retain | `Always` is still exact for command-bearing tools. |
| `docs/design/90-path-scoped-permissions-errata.md` OQ-90-1 | retain (unresolved) | Durable class grants remain deferred; 94-D9 records why this errata does not take that option. |
| `docs/design/46-permissions-ui-errata.md` 46-I5 | retain | An `Always` grant still carries only `tool` plus an optional `command` (empty for path/non-path tools) and is literal; no schema change. |
| `docs/design/46-permissions-ui-errata.md` 46-I6 | retain | `Once` nothing, `Session` in-memory, lost on restart; `Always` durable. |
| `docs/design/46-permissions-ui-errata.md` sec 6.2 / 25 UX46 | amend | Add the tool-class-aware `Always` label and the accelerator hint lines; the `force_ask` option set is unchanged and `A`/`S` are inert there. |
| `REQUIREMENTS_BACKLOG.md` RB-12 | narrow + accepted risk | The printable-key swallow is retained except for the eight accelerator characters; 94-D14 accepts that `a/o/s/d` re-open the burst accident and adds a PTY pin. |
| `25-ui-ux-errata.md` | amend hint | Tool-class-aware `Always` label and footer hints only. |

---

## 10. Test plan

All tests are hermetic and use the existing `FakeLLM`, temp workspace, and
supervisor harness. No live LLM is required.

### 10.1 Scope classifier

File: `tests/unit/errata90_shell_scope_test.cpp` (extend) or a new
`tests/unit/errata94_shell_scope_test.cpp` with the same helper.

| Test | Assertion | Non-vacuity |
|---|---|---|
| `ShellScope.SpecialParametersNeutral` | `echo $?`, `echo "$?"`, `echo $$`, `echo $#`, `echo $@`, `echo "$@"`, `echo x$@`, `echo ${?} ${@}`, `echo $9` -> InRoot; `echo $0`, `echo ${0}` -> Unknown | Fails on Rev 1 tree (all Unknown, `$@` via the `@` branch) |
| `ShellScope.SpecialParameterTrailer` | `cd <root> && grep -n "T7: deallocate" test/x.cpp; echo "exit: $?"`, `echo "exit: $?"`, `echo "status: $? (done)"` -> InRoot | The reported command; fails today (the `?` is seen as a glob) |
| `ShellScope.NamedVariableRemainsUnknown` | `echo $FOO`, `echo ${FOO}/x`, `cmake -DKEY=$FOO .` -> Unknown | Guards the over-fix |
| `ShellScope.HomeRemainsOutside` | `echo $HOME`, `cmake -DKEY=$HOME .`, `cmake -DCMAKE_PREFIX_PATH=$HOME/x .` -> Outside | Guards the ordering |
| `ShellScope.CmakeFormsInRoot` | `cmake -S . -B build`, `cmake --build build`, `cmake --build build -j`, `cmake -DKEY=value -S . -B build`, `cmake -DCMAKE_BUILD_TYPE=Debug -B build .` -> InRoot | The user's build forms |
| `ShellScope.CmakePathLikeDefine` | `cmake -DKEY=a/b -S . -B build`, `cmake -DCMAKE_INSTALL_PREFIX=./stage .` -> InRoot | Fails today (Unknown) |
| `ShellScope.CmakeOutsideDefine` | `cmake -DKEY=../x .` -> Outside; `cmake -DKEY=$? .` and `cmake -DKEY=$@ .` -> InRoot; `cmake -DKEY=$FOO .` -> Unknown | Fails safe and pins H2 |
| `ShellScope.ExitStatusInCompound` | `cmake --build build && echo "exit: $?"` -> InRoot | Common wrappers |
| `ShellScope.OutsideStillAsks` | `cat /etc/passwd`, `cat ../x`, `cat ~/.ssh/id_rsa` -> Outside | Regression pin |
| `ShellScope.DestructiveTrailerKeepsGuard` | `rm -rf ../x; echo "exit: $?"` -> risk Forbid and scope Outside; `rm -rf build; echo "exit: $?"` -> risk None and scope InRoot with kernel on | Proves 89 is untouched |
| `ShellScope.ProtectedStillDenied` | `cat .ymh/permissions.jsonc` with an in-root rule -> Deny from `evaluate` | Proves the protected check precedes the fold |
| `ShellScope.EmbeddedNamedExpansionUnknown` | `cat foo$BAR`, `cat foo${BAR}`, `cat foo$(echo eA== \| base64 -d)`, `cat foo<backtick>echo etc<backtick>`, `echo foo$bar` -> Unknown | H3: embedded named/command expansions must not become InRoot |
| `ShellScope.QuotedDollar` | `echo '$?'` (single quoted, unexpanded) -> InRoot | Quote handling |
| `ShellScope.PathRuleMatchesAfterFix` | `RulePermissionPolicy` with `{tool:"*", path:"**", effect:"allow"}`; the reported command -> Allow | End-to-end proof of the user's rule |

### 10.2 Dialog hotkeys (UI harness)

File: `tests/unit/supervisor_harness_test.cpp` (extend the existing RB-12 test
and add a new block).

| Test | Assertion |
|---|---|
| `PermHotkeys.FourOptionMapping` | For each of `A/a`, `O/o`, `S/s`, `D/d`: dialog opens, one key resolves, `last_dialog_resolution()` is `(AllowAlways, Always)`, `(Allow, Once)`, `(Allow, Session)`, `(Deny, Once)` respectively, dialog closes. |
| `PermHotkeys.NoSelectionMove` | Open with `selected == 0`; press `A`; `selected` stays 0 and the resolution is Always. |
| `PermHotkeys.ArrowsAndEnterUnchanged` | Down Down Enter -> AllowAlways/Always; Up from 0 wraps; Enter on each index yields the existing mapping. |
| `PermHotkeys.CaseInsensitive` | Lowercase keys produce the identical resolutions. |
| `PermHotkeys.ForceAskOnlyOnceAndDeny` | With `force_ask`, `O` -> Allow/Once, `D` -> Deny/Once, `A`/`S` are swallowed (dialog remains open, no resolution), then `O` resolves. |
| `PermHotkeys.OtherPrintableSwallowed` | A fresh corpus `y`, `n`, `1`, `2`, `3`, `0`, `x`, `/` does nothing. The existing RB-12 corpus never contained `a/o/s/d`; do not edit it to "remove" them (L5) -- add this fresh inert corpus and leave the RB-12 test's older keys alone. |
| `PermHotkeys.EscapeAndCtrlC` | Still Deny/Once (retained). |
| `PermHotkeys.HintsRendered` | The four-option shell render contains `Always allow (exact command)` and `[A] always \| [O] once \| [S] session \| [D] deny`; a `write_file` render contains `Always allow (all in-root paths)`; an `mcp.foo.bar` render contains `Always allow (this tool)`; the forced dialog contains `[O] allow once \| [D] deny`, no `[A]`/`[S]`, and no Always row. |

Also keep the existing
`SupervisorHarnessTest.RB12_PermissionDialogResolvesOnlyOnEnter` unchanged for
its existing keys (`n,N,y,Y,1,2,3,0,x,/`; it never contained `a/o/s/d`). Add a
comment that RB-12 is narrowed by 94-D5/D6 and that `a/o/s/d` are the accepted
risk in 94-D14; the new `PermHotkeys.*` tests cover the eight bound keys.

### 10.3 `Always` grants (90-D9 classes)

File: `tests/unit/errata94_always_grant_test.cpp`.

| Test | Assertion |
|---|---|
| `AlwaysGrant.ExactIsIntended` | `remember(grep -n x f, AllowAlways, Always)` -> `{tool:"shell", command:"grep -n x f", literal:true, target_class:Any, layer:LocalGrant}`; `evaluate(grep -n y g)` -> Ask (different command); `evaluate(grep -n x f)` -> Allow. |
| `AlwaysGrant.ExactDoesNotMatchOutsideVariant` | The same grant; a different command that targets `/etc/passwd` is not allowed by it (and the L1/protected path still decides). |
| `AlwaysGrant.CompoundStaysExact` | `remember(cd build && cmake .., ...)` -> literal raw text; a different compound asks. |
| `AlwaysGrant.TerminalWriteStaysExact` | `terminal write` Always -> literal raw input; no prefix/class fields. |
| `AlwaysGrant.ForceAskCreatesNoGrant` | `force_ask` + Always -> `{}` (46-I6). |
| `AlwaysGrant.ProtectedCreatesNoGrant` | `.ymh/config.jsonc` target -> `{}` (90-D9). |
| `AlwaysGrant.AppendFailureDegrades` | Fake store returns false; rule layer becomes `SessionGrant`, file unchanged (46-I7). |
| `AlwaysGrant.PersistsAcrossRestart` | Append to a temp `permissions.jsonc`; construct a new `RulePermissionPolicy` with a `FileGrantStore`; the identical command -> Allow; a different command -> Ask. |
| `AlwaysGrant.FileShapeV1` | Serialize/parse a literal shell grant -> `schema:"ymh.permissions/1"`, keys `{tool, command, match:"literal", effect:"allow", id}`; no `scope`/`prefix` keys. |
| `AlwaysGrant.ForeignSchemaRejected` | A file with `schema:"ymh.permissions/2"` -> `{}` (fail-safe; re-ask). |
| `AlwaysGrant.RepeatIsIdempotent` | Re-remembering the same exact grant rewrites one file entry with the same `id`; a fresh policy loading that file exposes one copy. (The in-memory duplicate is pre-existing 90-D9 behavior and is not changed here.) |
| `AlwaysGrant.WorkspaceIdMismatch` | A file with a different `workspace_id` loads `{}`. |
| `AlwaysGrant.PathToolIsInRootWide` | `remember(write_file, src/x, AllowAlways, Always)` -> `target_class=InRoot`, command empty; `evaluate(write_file, src/y)` -> Allow; an outside path does not match. |
| `AlwaysGrant.NonPathToolIsToolWide` | `remember(mcp.foo.bar, ...)` -> `target_class=Any`, command empty; a second call to the same tool matches. |
| `AlwaysGrant.LabelIsToolClassAware` | The four-option render shows `Always allow (exact command)` for `shell`, `Always allow (all in-root paths)` for `write_file`, and `Always allow (this tool)` for `mcp.foo.bar`; the hint line is unchanged. |

### 10.4 Integration / PTY

| Test | Assertion |
|---|---|
| `UiSupervisorPty.PermissionDialogHotkeys` | Under a PTY, type `A`/`O`/`S`/`D` and drive the real dialog; the broker receives the matching decision/scope and the composer receives none of the bytes. |
| `UiSupervisorPty.PermissionDialogSlashBurstAcceptedRisk` | While a four-option permission dialog is open, type the RB-12 burst `/rename repo overview`. The `a` resolves `AllowAlways/Always` (the accepted risk, 94-D14) and the composer receives nothing; a burst with no bound key (`/help`) leaves the dialog open. This pins the behavior instead of claiming the accident is absent. |
| `PermFlow.InRootRuleNoPrompt` | Headless fake-LLM run with a `{tool:"*", path:"**"}` rule and the reported command; no `PermissionRequested` is emitted, the shell tool runs. |
| `PermFlow.OutsideRulePrompts` | Same setup with `cat /etc/passwd`; a `PermissionRequested` is emitted. |

Build/run:

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The suite uses `-Wall -Wextra -Wpedantic -Werror`; no test may need a
suppression.

---

## 11. Non-goals and retained limitations

- No new shell parser for interpreters. `python -c`, `node -e`, `awk`, `sed`,
  `find -exec`, and `xargs` remain `Unknown` and ask (spec 89's stated residual,
  SG-F22).
- `path:"**"` remains the only shell path pattern. `"./**"` and other glob
  patterns match path tools, not shell commands. This is documented rather than
  widened because a shell command has no single resolved target.
- `terminal write` remains `Unknown` and exact; the input chunk is the unit of
  consent (90-D7/90-D12).
- No durable shell prefix/class grant. `Always` keeps the three 90-D9 classes
  (exact for shell/terminal, in-root-wide for a path tool, tool-wide for a
  non-path tool); OQ-90-1 (durable path-class grants) remains deferred, and
  `Session` remains the class-scoped shell option.
- No change to the grants schema, the `PermissionPolicy` order, or the wire.
- Arithmetic substitution (`$((...))`) and ANSI-C quoting (`$'...'`) are treated
  as named expansions (`Unknown`/Ask) even though they are not paths. This is a
  conservative residual, not a claim that they are path-like.
- Remote/SSH transport remains out of scope (project decision 2026-09-17).

---

## 12. Open questions

None blocking. One follow-up is recorded for a future errata:

- `OQ-94-1` -- whether to revisit OQ-90-1 (a durable class grant) after a
  dedicated threat-model review of control-plane verbs (90-C10, `git clean`,
  script runners). This errata deliberately does not take that option; the label
  fix removes the surprising behavior without widening the durable surface.

---

## 13. Revision history

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-10-20 | Initial draft. Pins the `$`/scope root cause with the user's exact command, the CMake option-value gap, the `A/O/S/D` hotkeys with hints and `force_ask` behavior, and the decision to retain the verified exact-command `Always` with an honest label (the class-scoped alternative is rejected on 90-C10/90-D9 grounds). Includes state lifetimes, invariants, failure modes, dsh mapping, and tests. |
| 2 | 2026-10-20 | Applies gate-94 findings. H1: strip special parameters before the `?` glob test. H2: scan before the `@` curl branch (`$@`). H3: named/command/backtick expansions are `Unknown` regardless of position. M1: tool-class-aware `Always` label (exact/all-in-root/this tool). M2: explicit RB-12 accepted risk + PTY pin. LOWs: column counts, `Always` class qualification, algorithm-order consistency, no false "existing test" claim, correct RB-12 corpus note, `LOCK_NB` contention semantics. No tracker row, not re-gated. |
