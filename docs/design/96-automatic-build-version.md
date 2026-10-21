# 96 - Automatic build version stamp

```
Status: verified (Rev 6)
Revision: 6
Verification status: adversarial reviewer (independent, Rev 6): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Component: 96 (feature) -- the user-visible version is a hand-edited constant
           (`CMakeLists.txt:5`, `VERSION 0.003`) that reaches the CLI, the TUI,
           and the MCP handshake through `YMH_VERSION`. Bumping it requires a
           deliberate edit. This spec replaces the displayed version with an
           automatic, configure-time stamp: a gitignored `.version_info` at the
           source root holds `DD-MM-YYYY:NN`; each configure increments it and
           the binary reports `DD-Mon-YYYY:#NN` (e.g. `20-Oct-2026:#02`).
           Rev 2 fixes the Rev 1 blockers: CMake strips `#` from compile
           definitions, so the version is carried by a small generated header
           (96-D8); a read-only file in a writable tree must not abort the
           configure, so the stamp uses an atomic write-and-rename and no
           `file(LOCK)` (96-D6/D7); and the seven golden assertions that
           hardcode `v0.003` are re-derived from `YMH_VERSION` (96-D9).
Depends on: CMake >= 3.22, `include/ymh/core/version.hpp`, the three TUs that
            include it, and the dev-side test harness (`tests/CMakeLists.txt`).
Amends: CMakeLists.txt:5 (project version is no longer displayed), :382-384,
        :787-792, :833-835, :845-847 (definitions -> generated header),
        `include/ymh/core/version.hpp` (fallback + generated include),
        `.gitignore`, 72-D1/72-I2 (single source is now the stamp),
        `tests/unit/ui_render_golden_test.cpp:286,307,1002,1032,1035,2231`.
Tracker: a `DESIGN_STATUS.md` row is required before commit.
```

## 1. Problem

The version is manual:

* `CMakeLists.txt:5` sets `project(ymh VERSION 0.003)`.
* Four targets copy it into the C++ macro: `YMH_VERSION="${PROJECT_VERSION}"`
  (`:384` `ymh_mcp`, `:792` `ymh_ui`, `:835` `ymh_cli`, `:847` `ymh`).
* `include/ymh/core/version.hpp:7-8` falls back to `"0.0.0"`.
* Consumers print it: `ymh --version` (`src/cli/cli.cpp:1362`, `:1507`), the
  TUI header `ymh (v...)` (`src/ui/ui_render.cpp:1726`), the supervisor's
  `SupervisorRunOptions::version` (`cli.cpp:580`), and the MCP handshake
  `clientInfo.version` (`src/mcp/mcp_client.cpp:131`).

Two builds of one commit are indistinguishable and every bump is a source edit.
The user asked for a build identifier that advances on its own, driven by a
gitignored `.version_info` shaped `20-10-2026:01`, with the built version
rendered as `20-Oct-2026:#02`.

Only three translation units include `version.hpp`
(`src/mcp/mcp_client.cpp:3`, `src/cli/cli.cpp:46`, `src/ui/ui_render.cpp:22`),
so a changing version rebuilds three TUs, not the world.

## 2. Decisions

### 96-D1 - `.version_info`: location, format, parsing

Path: `${CMAKE_CURRENT_SOURCE_DIR}/.version_info` (the source root).

Content: one line, `<DD>-<MM>-<YYYY>:<NN>` plus `\n`, e.g. `20-10-2026:01`.

Reading is **lenient**: surrounding whitespace is stripped, day/month may be
1-2 digits, the year is 4 digits, the counter is 1+ digits. Writing is
**canonical**: zero-padded day/month and a counter padded to at least two
digits (`01`..`99`, then `100`, `101`, ...).

Pattern (after stripping), written with **explicit repeat groups**: CMake's
`string(REGEX)` engine has no `{m,n}` quantifier (verified: `MATCHES` is false
for `{1,2}`), so the pattern is

```text
^([0-9][0-9]?)-([0-9][0-9]?)-([0-9][0-9][0-9][0-9]):([0-9]+)$
```

Anything else - empty, `garbage`, a truncated write - is **malformed** and is
treated as missing (96-D4).

`.version_info` is added to `.gitignore` (96-I10); its transient write temp
files are covered by `.version_info.tmp-*` (96-D6).

### 96-D2 - Display format

```text
<DD>-<Mon>-<YYYY>:#<NN>
```

`Mon` comes from a **fixed English table** (`Jan Feb Mar Apr May Jun Jul Aug
Sep Oct Nov Dec`), never the locale's `%b`; day and counter are zero-padded to
at least two digits. Examples: `20-Oct-2026:#01`, `20-Oct-2026:#02`,
`20-Oct-2026:#100`.

The string is a free-form build identifier, deliberately not semver (96-D9).

### 96-D3 - Increment at configure time, not at build time

`ymh_stamp_version` runs once while CMake configures, before `project()`;
an ordinary `cmake --build` never touches the file. A build that makes Ninja
re-run CMake (because `CMakeLists.txt` or an included module changed) **is** a
configure and does bump once (96-I11 is scoped accordingly).

Rationale: a build-time bump would either rewrite a header on every build
(recompiling the three consumers each time) or be invisible to the generator.
Configure is the single cheap point where one increment is observable.

### 96-D4 - Sequence

Given today's local date `D`:

| State before configure | Written file | Built version |
|---|---|---|
| absent, empty, or malformed | `D:01` | `#01` |
| file date == `D`, counter `N` | `D:(N+1)` | `#(N+1)` |
| file date != `D` (older or future) | `D:01` | `#01` |

The configure that creates the file reports `#01` (it does not increment a
freshly created value); later configures the same day report `#02`, `#03`, ...
This matches the requested examples.

### 96-D5 - Date source and the test override

The date is the **local** date: `string(TIMESTAMP date "%d-%m-%Y")` without
`UTC` (CMake's default is local time).

`YMH_VERSION_DATE` (`-DYMH_VERSION_DATE=DD-MM-YYYY`) overrides it. It must
match `^[0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9]$` **and** have month `01..12`
and day `01..31`; an unnormalized, out-of-range, or malformed override is
ignored with a warning (never used in `list(GET)` / `math(EXPR)`). It is used by the tests and script-mode runs; it is not read
from the environment and is not part of the CLI. In configure mode a `-D` value
is cached by CMake, so a developer who sets it once keeps it until
`-U YMH_VERSION_DATE`; documented, not defended against.

### 96-D6 - Atomic, non-fatal write; no `file(LOCK)`

Rev 1 locked the target file. That is fatal on a read-only **file** even in a
writable directory (CMake raises `creation failed (check permissions)` despite
`RESULT_VARIABLE`), and a sidecar lock leaves `.version_info.lock` behind.
Rev 2's `file(RENAME)` was still fatal for an immutable target.

Rev 3 performs the write through `execute_process`, which reports a non-zero
exit instead of aborting:

```cmake
set(tmp "${file}.tmp-${suffix}")                       # suffix = string(RANDOM)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${date}:${nn}"
                OUTPUT_FILE "${tmp}" ERROR_QUIET)
set(tmp_body "")
if(EXISTS "${tmp}")                                    # echo returns 0 even when
    execute_process(COMMAND ${CMAKE_COMMAND} -E cat "${tmp}"   # it wrote nothing
                    OUTPUT_VARIABLE tmp_body RESULT_VARIABLE cat_rc ERROR_QUIET)
endif()
string(STRIP "${tmp_body}" tmp_body)
if(NOT cat_rc EQUAL 0 OR NOT tmp_body STREQUAL "${date}:${nn}")
    message(WARNING "ymh: cannot write a complete ${tmp}; version not persisted")
    file(REMOVE "${tmp}")                              # non-fatal
else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E rename "${tmp}" "${file}"
                    RESULT_VARIABLE rename_rc ERROR_QUIET)
    if(NOT rename_rc EQUAL 0)
        message(WARNING "ymh: cannot replace .version_info (rename ${rename_rc})")
        file(REMOVE "${tmp}")                          # non-fatal
    endif()
endif()
```

The read-back check is required because `cmake -E echo` with `OUTPUT_FILE`
exits 0 even when it wrote nothing (verified on a read-only directory). The
temp is read back with `cmake -E cat` and compared with the exact intended
line; a zero-byte, partial, or missing temp fails that comparison, so it is
never renamed over a valid stamp. This also avoids `file(SIZE)` (fatal on a
directory or an unreadable temp path, reproduced).

`cmake -E rename` is `rename(2)` on POSIX: it replaces the destination entry
and needs write permission on the **directory**, not on the target inode
(verified: replacing a `0444` file in a writable directory succeeds and yields
`0644`). A failed rename is a warning, never an abort.

Concurrency: there is no lock, so two simultaneous configures can read the same
`N` and both write `N+1` (a repeated number). Accepted: the stamp is advisory
(96-D16), and the failure mode is a duplicate, never a partial file.

### 96-D7 - A read-only or unwritable tree must not fail the configure

There is no separate probe (Rev 2's `cmake -E touch` probe proved only
directory writability at probe time and left a `.probe` artifact). The write
path itself is non-fatal (96-D6):

* a missing temp file after `cmake -E echo` means "not writable" -> warn, no
  write;
* a failed `cmake -E rename` -> warn, remove the temp, no write;
* either way the configure continues and the variant still yields a value.

The computed value is always well-defined: `N+1` when the readable file is
dated today, otherwise `01`. It is never empty (96-F7).

Consequence: on a read-only checkout every configure reports the same value
(`#01` when the file is absent). Accepted, and why the stamp is advisory.

### 96-D8 - One display source: a generated header, not a compile definition

**CMake strips `#` from compile definitions.** Verified on this host:
`target_compile_definitions(t PRIVATE YMH_VERSION="20-Oct-2026:#02")` produces
`-DYMH_VERSION=""` in `flags.make` and the build fails; the `#` starts a CMake
comment. So `#` cannot travel through `COMPILE_DEFINITIONS`.

The version is carried by a generated header instead:

```cmake
# cmake/build_version.hpp.in
#pragma once
#define YMH_VERSION "@YMH_DISPLAY_VERSION@"

# CMakeLists.txt (after the stamp)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_version.hpp.in"
               "${CMAKE_BINARY_DIR}/generated/ymh/build_version.hpp" @ONLY)
```

`${CMAKE_BINARY_DIR}/generated` is added to the include path of the three
consumers (`ymh_mcp`, `ymh_ui`, `ymh_cli`) and of `ymh_tests`. The four
`YMH_VERSION="${PROJECT_VERSION}"` definitions are **removed**.

`include/ymh/core/version.hpp` becomes:

```cpp
#pragma once
#if defined(__has_include)
#  if __has_include("ymh/build_version.hpp")
#    include "ymh/build_version.hpp"
#  endif
#endif
#ifndef YMH_VERSION
#define YMH_VERSION "unknown"
#endif
```

The fallback changes from `"0.0.0"` to `"unknown"` so a target that misses the
generated include is visibly wrong rather than plausibly semver.

`project(ymh VERSION 0.003 ...)` stays as CMake's compatibility version and is
documented as not displayed. `PROJECT_VERSION` is no longer a version source
(96-D16); 72-D1/72-I2 are superseded (sec. 9).

### 96-D9 - Consumers keep their shapes; goldens stop hardcoding

* `ymh --version` prints exactly `ymh <display>\n` (`cli.cpp:1362`, `:1507`
  unchanged).
* The TUI header prints `ymh (v<display>)` (`ui_render.cpp:1726` unchanged).
* `SupervisorRunOptions::version` is the display string (`cli.cpp:580`
  unchanged).
* MCP `clientInfo.version` is the display string (`mcp_client.cpp:131`
  unchanged); MCP treats it as an opaque string, and a non-semver value is
  accepted here as a deliberate choice.

Six assertions in `tests/unit/ui_render_golden_test.cpp`
(`:286, :307, :1002, :1032, :1035, :2231`) plus the comment at `:1024`
hardcode `ymh (v0.003)`. They are rewritten to build the expected title from
`YMH_VERSION`, and the test TU gains `#include "ymh/core/version.hpp"`; the
test target takes the generated include path, so the goldens stay exact
without freezing a version.

### 96-D10 - Locale independence

Month names come from a fixed list indexed by `MM`; `string(TIMESTAMP ...)` with
`%b`/`%B` is forbidden. `LC_TIME` cannot change the string (96-I6).

### 96-D11 - Counter growth

The counter is a plain integer; at `100` the file holds `D:100` and the display
is `#100`. Padding is a minimum of two digits. The counter is capped at `9999`:
a same-date value `>= 9999` is reused (with a warning) instead of incrementing,
so `math(EXPR)` can never overflow.

### 96-D12 - No `SOURCE_DATE_EPOCH`; not reproducible by design

The stamp is a build identifier. `SOURCE_DATE_EPOCH` is not consulted;
reconfiguring an identical tree intentionally changes the version. Recorded as
a deliberate scope decision.

### 96-D13 - Configure log line

Exactly one status line per configure:

```text
ymh: build version 20-Oct-2026:#02 (from .version_info)
```

### 96-D14 - The script is testable standalone

`cmake/version_stamp.cmake` defines `ymh_stamp_version(<file> <out_var>)`; in
script mode it runs with

```text
cmake -DYMH_VERSION_FILE=<path> -DYMH_VERSION_DATE=DD-MM-YYYY \
      -DYMH_VERSION_OUT=<path> -P cmake/version_stamp.cmake
```

and writes the display string to `YMH_VERSION_OUT` plus the stamp to the file,
so tests never configure a project.

### 96-D15 - No durable state beyond the file

No database schema, event, wire DTO, or session state changes.

### 96-D16 - The version is not a key

The display string is never a cache key, a file name, a protocol gate, or a
security decision. It is advisory and may repeat or go stale (96-D6/D7).

### 96-D17 - Placement relative to `project()`

```cmake
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_stamp.cmake")
ymh_stamp_version("${CMAKE_CURRENT_SOURCE_DIR}/.version_info" YMH_DISPLAY_VERSION)
project(ymh VERSION 0.003 ...)
configure_file(...)                     # 96-D8, after project()
```

`CMAKE_CURRENT_SOURCE_DIR` and `CMAKE_BINARY_DIR` are defined at that point;
the module uses only `file`, `string`, `math`, `list`, `message`, and
`execute_process`.

## 3. Interface

```cmake
# cmake/version_stamp.cmake
function(ymh_stamp_version file out_var)
    # Inputs:  file              absolute path to .version_info
    #          YMH_VERSION_DATE  (optional) DD-MM-YYYY override (96-D5)
    # Output:  ${out_var}        the display version, e.g. 20-Oct-2026:#02
    # Effects: writes <file> canonically unless the directory is not writable
    #          (96-D7); emits one STATUS line in configure mode (96-D13)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    ymh_stamp_version("${YMH_VERSION_FILE}" _ymh_out)
    file(WRITE "${YMH_VERSION_OUT}" "${_ymh_out}\n")
endif()
```

```cpp
// include/ymh/core/version.hpp (complete shape)
#pragma once
#if defined(__has_include)
#  if __has_include("ymh/build_version.hpp")
#    include "ymh/build_version.hpp"
#  endif
#endif
#ifndef YMH_VERSION
#define YMH_VERSION "unknown"
#endif
```

## 4. Algorithm

```text
1. date = YMH_VERSION_DATE ?: local "%d-%m-%Y"                  (96-D5)
2. read the file with `cmake -E cat` (non-fatal):               (96-D7)
       rc != 0 (missing/unreadable/directory) -> body = ""
       else body = output, whitespace-stripped
3. if body matches PATTERN:
       if file_date == date numerically: n = min(N+1, 9999)
       else:                             n = 1
   else:                                 n = 1
4. write "<date>:<pad2(n)>\n" via `cmake -E echo` to
   "<file>.tmp-<random>", then read it back with `cmake -E cat` and require
   the exact intended line; otherwise warn, remove the temp, and return
   display(n) without a write                                     (96-D6)
5. replace the file with `cmake -E rename`; on failure warn, remove the temp
6. display = "<DD>-<Mon>-<YYYY>:#<pad2(n)>"                     (96-D2)
7. STATUS line (configure mode only)                            (96-D13)
```

`file_date == date` compares numeric day/month/year, so `1-2-2026` equals
`01-02-2026`. The written file is canonical.

## 5. Invariants

* **96-I1 (one step per configure).** The function runs once per configure and
  advances the counter by one (or reuses `9999`, 96-D11) when it reads a
  same-date file; a configure that creates, resets, or cannot read the file
  uses `01` (96-D4/D7). It never increments twice in one run. An unwritable
  tree reports the same value without persisting it (96-D7).
* **96-I2 (monotonic where writable).** Sequential configures on one date in a
  writable tree produce `01, 02, 03, ...` whenever each rename succeeds; the
  creating configure is `01`.
  Concurrent configures may repeat a number (no lock, 96-D6) but never corrupt
  the file; a read-only tree reports a constant value (96-D7).
* **96-I3 (built == persisted on success).** When the rename succeeds, the
  display version equals the file's counter for that configure. When it fails,
  the previous file is intact and the computed value still applies to this
  build (96-D7).
* **96-I4 (rollover).** A date change, a missing, empty, or malformed file all
  restart at `01`.
* **96-I5 (never fatal).** No `.version_info` condition fails the configure:
  reads use `cmake -E cat`, writes/read-backs/renames use `execute_process`, and
  every write/rename failure is a warning (96-D6/96-D7).
* **96-I6 (locale-free).** The display is identical under any `LC_TIME`.
* **96-I7 (single source).** All consumer surfaces use `YMH_VERSION`, supplied
  by the generated header; no consumer re-derives a version.
* **96-I8 (opaque).** No code parses, compares, sorts, or gates on the version.
* **96-I9 (no durable state).** No DB, event, or wire change.
* **96-I10 (untracked).** `.version_info` is gitignored and never committed.
* **96-I11 (no ordinary build bump).** `cmake --build` leaves the file
  untouched unless it triggers a CMake regeneration, which is a configure
  (96-D3).
* **96-I12 (shared per checkout).** All build directories configured from one
  source tree share the counter.

## 6. Failure modes

No sec. 54 finding is about build metadata; the closest tags are used: F8 for
bounded resources (counter growth/padding), F3 for a stale value after a failed
write, F9 for an interrupted configure, F1 for per-checkout isolation.

| ID | Tag | Scenario | Required behavior |
|---|---|---|---|
| 96-F1 | F1 | `.version_info` absent | Create `D:01`; build `#01` |
| 96-F2 | F3 | Empty file (killed write, or a stray lock artifact) | Treat as absent; `#01` |
| 96-F3 | F3 | Malformed (`garbage`, `20-10-2026`, `:5`) | Overwrite with `D:01`; `#01` |
| 96-F4 | F9 | Date differs from today | Reset to `D:01`; `#01` |
| 96-F5 | F8 | Counter `99` | `:100`, `#100` (no wrap) |
| 96-F6 | F9 | Directory not writable | Warn; compute in memory; no write; configure succeeds |
| 96-F7 | F9 | Unwritable and unreadable | Warn; `#01`; never empty |
| 96-F8 | F9 | Concurrent configures | May repeat a counter; no corruption; no fatal error |
| 96-F9 | F3 | Locale with non-English month names | Fixed table; unchanged output |
| 96-F10 | F1 | A target misses the generated include | Prints `unknown`, not a fake semver |
| 96-F11 | F9 | `#` in a compile definition | Forbidden; the header carries it (96-D8) |
| 96-F12 | F9 | Interrupted write | Temp file only; the real file is intact; a stale temp is harmless and gitignored |

## 7. State lifetime

| State | Created | Destroyed | Owner | Survives restart? |
|---|---|---|---|---|
| `.version_info` | first configure | never (user may delete; recreated next configure) | the checkout | yes |
| Counter `NN` | first configure of a date | date rollover / file deletion | `.version_info` | yes, until reset |
| `.version_info.tmp-<random>` | one write | the rename (or an explicit remove on failure) | the stamp script | no |
| `YMH_DISPLAY_VERSION` | configure | end of configure | CMake | no |
| `ymh/build_version.hpp` | configure (`configure_file`) | next configure overwrites | the build dir | no |
| `YMH_VERSION` | compile | the binary | the binary | yes (baked in) |

## 8. Tests

1. **T1 creation** - no file, fixed date: writes `20-10-2026:01`, emits `#01`.
2. **T2 increment** - three runs, same fixed date: `#01`, `#02`, `#03`; file
   ends at `:03`.
3. **T3 rollover** - file dated yesterday with `:07`: `#01`, file `20-10-2026:01`.
4. **T4 malformed/empty** - `garbage`, empty, and `20-10-2026` (no counter):
   each resets to `#01`.
5. **T5 padding** - `9` -> `:10`/`#10`; `99` -> `:100`/`#100`.
6. **T6 lenient parse** - `20-10-2026:7` -> `#08`; ` 20-10-2026:07\n` -> `#08`.
7. **T7 script mode** - `cmake -P` writes both files and needs no project.
8. **T8 month table** - all twelve months produce the fixed abbreviations
   (`01` -> `Jan` ... `12` -> `Dec`), independent of the host locale.
9. **T9 read-only** - a `chmod 0555` temp dir: the script returns `#01`, writes
   nothing, emits the warning, and exits 0; skipped when running as root (root
   ignores the mode).
10. **T10 format** - a C++ test asserts `YMH_VERSION` matches
    `^[0-9]{2}-[A-Z][a-z]{2}-[0-9]{4}:#[0-9]{2,}$` (the test target takes the
    generated include path).
11. **T11 single source** - a source check asserts no
    `YMH_VERSION="${PROJECT_VERSION}"` and no `YMH_VERSION=` compile definition
    remains.
12. **T12 gitignore** - `git check-ignore .version_info` succeeds (skipped
    outside a git work tree).
13. **T13 no build bump** - configure once, build twice, assert the file is
    unchanged after the builds (a plain `cmake --build`, no CMake edit).
14. **T14 generated header** - the header exists after configure and contains
    the same string the script reported.

15. **T15 directory at `.version_info`** - replace the file with a directory:
    the read fails non-fatally, the value is `#01`, the configure succeeds.
16. **T16 unreadable file** - `chmod 000` in a writable directory (skipped as
    root): the read fails, the value is `#01`, and the rename still replaces the
    file (POSIX needs directory write permission, not file read), so the file
    becomes `D:01`; no failure.
17. **T17 malformed override** - `YMH_VERSION_DATE=1-2-2026` is ignored with a
    warning and the local date is used.

T1-T9 and T11-T17 run as a Python CTest (`tests/tools/version_stamp_test.py`,
registered like `design_status_drift`, guarded on `Python3`; it invokes
`${CMAKE_COMMAND}`). T10 is a gtest.

## 9. Amendments and supersession

* **`CMakeLists.txt:5`** - `project(VERSION 0.003)` remains CMake's
  compatibility version; it is no longer displayed.
* **`CMakeLists.txt:382-384, 787-792, 833-835, 845-847`** - the four
  `YMH_VERSION` definitions are removed; the comments that name
  `PROJECT_VERSION` are updated to name the generated header.
* **`include/ymh/core/version.hpp:3-8`** - the comment and fallback are
  replaced by the shape in 96-D8.
* **`.gitignore`** - adds `.version_info` and `.version_info.*`.
* **72-D1 / 72-I2 (`72-*.md:51, :138`)** - superseded: the single version
  source is now `.version_info` + the generated header, not `PROJECT_VERSION`.
  72-A2/72-A6's *intent* (one source for CLI, TUI, and MCP) is preserved.
* **72-F1 (`72-*.md:146`)** - superseded: a missing definition now yields
  `"unknown"`, not `"0.0.0"` (96-D8).
* **72-U1 / 72-U3 (`72-*.md:162, :164`)** - amended: the label assertions are
  re-derived from `YMH_VERSION` instead of a frozen string (96-D9).
* **`tests/unit/ui_render_golden_test.cpp:286,307,1002,1024,1032,1035,2231`** -
  the hardcoded `v0.003` becomes `YMH_VERSION`.

## 10. dsh mapping

| dsh behavior | ymh | Anchor / justification |
|---|---|---|
| One version string shared by CLI, TUI, and MCP handshake | Mirrored | 72-A2/72-A6 intent; consumers `cli.cpp:1362,1507`, `ui_render.cpp:1726`, `mcp_client.cpp:131` |
| A single build-system version source | Mirrored | 96-D8: one generated header feeds every consumer; no per-target literals |
| Version bumped by a hand edit | **Not mirrored** | Deliberate: the user asked on 2026-10-20 for an auto-incrementing stamp; the manual constant it replaces is `CMakeLists.txt:5` (56-D6) |
| Date-based build identifier (`20-Oct-2026:#02`) | **Not mirrored** | Same decision; dsh has no configure-time stamp. Recorded so it is not read as an oversight (`CMakeLists.txt:5`, 96-D2) |

## 11. Checklist

* [ ] `cmake/version_stamp.cmake` (function + script mode; 96-D1..96-D7)
* [ ] `cmake/build_version.hpp.in` + `configure_file` (96-D8)
* [ ] Include path on `ymh_mcp`, `ymh_ui`, `ymh_cli`, `ymh_tests` (96-D8)
* [ ] Four compile definitions removed (96-D8)
* [ ] `version.hpp` generated include + `"unknown"` fallback (96-D8)
* [ ] `.gitignore` gains `.version_info` and `.version_info.tmp-*` (96-D1/D6)
* [ ] Seven golden assertions use `YMH_VERSION` (96-D9)
* [ ] STATUS line (96-D13)
* [ ] T1-T14 registered and passing
* [ ] ASCII; anchors resolve; tracker row before commit
