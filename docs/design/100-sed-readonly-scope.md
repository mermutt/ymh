# 100 - Shell scope: read-only `sed` stdin pipelines are in-root

Status: verified (Rev 6)
Revision: 6
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 6)

Amends the interpreter-unknown list in 89/90/94 (`kInterpreterUnknown`,
`src/policy/shell_guard.cpp:1820-1827`) for `sed` only, and only for
**stdin-only** invocations.

## 1. Why

Every shell pipeline through `sed` is classified `ShellTargetScope::Unknown`,
because `sed` is treated as an opaque interpreter. A path-scoped allow rule
(e.g. `{tool:"*", path:"**", effect:"allow"}`) only matches `InRoot`
(`permission_policy.cpp:164`, target class at `:473-488`), so a read-only
in-root command asks:

```
cd /w && git show origin/feature:tg/x.cpp | sed -n '80,130p'
```

Verified with `RulePermissionPolicy` under that rule set + `default: ask`:

| Command | scope | verdict |
|---|---|---|
| `cd /w && ... && git show origin/feature:tg/x.cpp` | InRoot | Allow |
| same + `\| sed -n '80,130p'` | Unknown | **Ask** |

`sed -n 'a,bp'` reading stdin and writing stdout cannot touch the filesystem.
Treating it as unknown is a false escalation that makes the allow rule unusable
for ordinary code inspection.

## 2. Decisions

### 100-D1 - Scope of the relaxation

Remove `"sed"` from `kInterpreterUnknown` and add a `sed` branch in
`classify_segment_scope` (`shell_guard.cpp:1764`). The relaxation applies only
when the invocation has **no file operand**: every argument is an option, an
option argument, or the script. Any non-option word that is not the script makes
the result `Unknown` (v1, 100-D3). `-i` is `Unknown` (a write, not stdin-only).

`awk` and every other interpreter stay `Unknown`.

### 100-D2 - Accept only a provably read-only script

The script token must be a literal with **no shell metacharacters**: no `$`,
backtick, `*`, `?`, `[`, `{`, `}`, backslash, or unquoted leading `~`, and
`had_unexpanded == false`. Those characters are the only ways an unquoted (or
partly quoted) token can glob, brace-expand, or continue a line; rejecting all
of them makes the "literal token" claim independent of the scanner's quote
flags. The token must then match:

```text
script  := ws? command (ws? ';' ws? command)* ws?
command := address? ws? '!'? ws? action
address := digits (',' digits)? | '/' regex '/'
action  := 'p' | 'P' | 'd' | 'D' | 'n' | 'N' | '=' | 'l'
         | 'q' digits? | 'Q' digits?
         | 'h' | 'H' | 'g' | 'G' | 'x' | 'z'
         | 'b' ws? label? | 't' ws? label? | 'T' ws? label? | ':' ws? label
         | 's' d body d body d flags
label   := [A-Za-z0-9_]*        (ends at ws, ';', '}', or end of token)
flags   := [gpiImM0-9]*
```

`body` is text between delimiter `d` with backslash escapes; `d` is any
non-alphanumeric character. Anything outside this grammar - `w`, `W`, `r`, `R`,
`e`, `y`, an unparsable expression - is `Unknown` (fail-safe). `$` is rejected
even as the last-line address: the scanner's quote flags cannot distinguish
`'$p'` from a mixed `'$p'x`, and a `$` can be shell-expanded.

### 100-D3 - Options and the no-operand rule

Scan the whole argument list; option recognition stops at `--` (words after it
are never options):

* no argument: `-n`, `--quiet`, `--silent`, `-E`, `-r`, `--regexp-extended`,
  `-s`, `--separate`, `-u`, `--unbuffered`, `-z`, `--null-data`, `--posix`,
  `--debug`, `--sandbox`;
* one script argument: `-e`, `--expression`, `-eSCRIPT`, `--expression=SCRIPT`
  (repeatable; every script is validated);
* `-f`, `--file`, `-fFILE`, `--file=FILE`: **`Unknown`**;
* `-i`, `--in-place`, `-iSUFFIX`, `--in-place=SUFFIX`: **`Unknown`** (a write);
* `--`: stop option recognition;
* any other option: `Unknown`.

GNU `sed` permutes options, so the script is the **first non-option word**
(before or after `--`) when no `-e`/`-f` is present. If a `-e` is present, every
non-option word is a file operand. If a `-f` is present, `Unknown`. Any file
operand (including the post-`--` `-e` in `sed -- 'w /tmp/x' -e 's/a/b/'`) makes
the result `Unknown`, so the user-visible relaxation is exactly "stdin only".

### 100-D4 - Result

If the invocation is stdin-only and every script is accepted: `InRoot`. If no
script exists at all (usage error), also `InRoot` (no file access). Otherwise
`Unknown`.

### 100-D5 - The relaxation is `shell`-only

`terminal` requests are classified from joined argv
(`shell_guard.cpp:2457-2469`), which loses word boundaries; a relaxed parser
there could be fooled by an argv element that joins into a different script.
`classify_shell_request` passes `relaxed_sed = (request.tool == "shell")` down
through `classify_shell_command_with_scope` (new last parameter, default
`false`, so every existing caller and test keeps sed `Unknown`) ->
`scope_impl` -> `classify_segment_scope`, including the terminal-open
`scope_impl` call (`shell_guard.cpp:2664`) and the nested-shell recursion. For
`terminal`, `sed` stays `Unknown`.

## 3. Interfaces

```cpp
// src/policy/shell_guard.cpp (anonymous namespace)
bool sed_script_read_only(const Token& script);   // 100-D2
ScopeResult classify_sed_segment(const std::vector<Token>&, std::size_t b,
                                 std::size_t e, const std::filesystem::path& root,
                                 const std::filesystem::path& cur);  // 100-D3/D4

// include/ymh/policy/shell_guard.hpp (additive, defaulted)
ShellClassification classify_shell_command_with_scope(
    std::string_view text, const std::filesystem::path& root,
    const std::filesystem::path& cwd, bool kernel_confinement,
    const std::filesystem::path& canonical_tmp_root = {},
    bool relaxed_sed = false);                                 // 100-D5
```

Caller: `classify_shell_request` (`shell_guard.cpp`) passes
`request.tool == "shell"`; `classify_segment_scope` dispatches `executable ==
"sed"` to `classify_sed_segment` when `relaxed_sed`, otherwise falls through to
`Unknown`.

## 4. Implementation

* `src/policy/shell_guard.cpp` - remove `"sed"` from `kInterpreterUnknown`;
  add `sed_script_read_only`/`classify_sed_segment`; thread `relaxed_sed`
  through `classify_shell_command_with_scope`, `scope_impl`,
  `classify_segment_scope`, the terminal-open `scope_impl` call and the
  nested-shell recursion. Do **not** add it to `classify_shell_command` (risk
  only; an unused parameter would fail `-Werror`).
* No changes to `permission_policy.cpp`, the wire, or config.
* `tests/unit/errata100_sed_scope_test.cpp` (registered in
  `tests/CMakeLists.txt`) - T1-T12.

## 5. Invariants

* **100-I1.** A stdin-only `sed -n '80,130p'` is `InRoot`.
* **100-I2.** A script containing `w`/`W`/`r`/`R`/`e`/`y`, an `s` flag
  `e`/`w`/`W`, any of `$`, backtick, `*`, `?`, `[`, `{`, `}`, backslash,
  or an unquoted leading `~`, is `Unknown`. A comma is **not** rejected: it is
  the address range separator (`80,130p`).
* **100-I3.** Any file operand, `-f`, or `-i` is `Unknown` (v1).
* **100-I4.** `awk` and every other interpreter stay `Unknown`.
* **100-I5.** `terminal` requests never take the relaxation.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 100-F1 | unparsable script | `Unknown` |
| 100-F2 | unknown option | `Unknown` |
| 100-F3 | `-f`/`--file` script | `Unknown` |
| 100-F4 | any file operand / `-i` | `Unknown` |
| 100-F5 | no script (usage error) | `InRoot` (no file access) |

## 7. State lifetime

None: pure classifier.

## 8. Tests

1. **T1** `git show HEAD:x | sed -n '80,130p'` -> `InRoot`.
2. **T2** `sed -n 's/a/b/'` (stdin) -> `InRoot`; `sed -n 's/a/b/g'` -> `InRoot`;
   `sed -n 's*a*b*'` (glob) -> `Unknown`.
3. **T3** `sed -n '1,5!p'` and `sed -n '/x/p'` -> `InRoot`.
4. **T4** `sed 'w /tmp/x'`, `sed 'W /tmp/x'` -> `Unknown`.
5. **T5** `sed 'r /etc/passwd'`, `sed 'e id'` -> `Unknown`.
6. **T6** `sed 's/a/b/e'`, `sed 's/a/b/w out'` -> `Unknown`.
7. **T7** `sed -f script.sed`, `sed -n p x.cpp`, `sed -i 's/a/b/' x.cpp` ->
   `Unknown` (file operand / `-i`).
8. **T8** `sed -n '$p'` -> `Unknown` ($ rejected); `sed -n 'b; w /tmp/x'` ->
   `Unknown`; `sed -n 'b end; p; :end'` -> `InRoot`.
9. **T9** `sed -n "$p"`, `sed -n "${HOME}"`, `sed -n \`p\`` -> `Unknown`.
10. **T10** `sed -- 'w /tmp/x' -e 's/a/b/'` -> `Unknown`; `sed -- '80,130p'` ->
    `InRoot`.
11. **T11** policy `{tool:"*", path:"**", allow}` + `default: ask` on a
    `shell` request (via `classify_shell_request`): T1 -> Allow; T4 -> Ask.
    The text-level `classify_shell_command_with_scope` default stays `false`
    (sed Unknown), so `errata90_shell_scope_test.cpp:81` is unchanged.
12. **T12** `terminal` with the same sed argv -> `Unknown` (100-D5); `awk
    '{print}'` -> `Unknown`.

## 9. dsh mapping

Not a dsh-mirrored capability; L1 advisory classifier only.

## 10. Amendments

* **89/90/94** - the `"sed"` entry in `kInterpreterUnknown`
  (`shell_guard.cpp:1821`) is withdrawn for `shell` stdin-only invocations;
  every other interpreter entry is unchanged. The temp-pass interpreter list
  (`shell_guard.cpp:2186-2188`) is unchanged: sed stays conservative there
  (only the `/tmp` exemption is affected, not the scope verdict).
