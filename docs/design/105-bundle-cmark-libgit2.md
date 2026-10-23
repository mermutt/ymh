# 105 - Statically link cmark-gfm (+ extensions) and libgit2

Status: verified (Rev 5)
Revision: 5
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 5)

Extends 104 to the next two runtime dependencies that are commonly absent on a
minimal Ubuntu 24.04 server.

## 1. Why

`libcmark-gfm` and `libgit2` are runtime `DT_NEEDED` entries today
(`libcmark-gfm.so.0.29.0.gfm.13`, `libcmark-gfm-extensions.so…`,
`libgit2.so.1.9`). A minimal server image often has neither package, so a
binary built on a developer host fails the same way `libspdlog` did. Both are
small, C libraries with no ABI exposed by ymh; bundle them as PIC static
archives.

## 2. Decisions

### 105-D1 - Bundle cmark-gfm core + extensions

`YMH_BUNDLED_CMARK_GFM` (default `ON`) fetches
`github/cmark-gfm` **0.29.0.gfm.13** (`GIT_SHALLOW TRUE`) and builds:

* `CMARK_STATIC=ON`, `CMARK_SHARED=OFF`, `CMARK_TESTS=OFF`,
  `CMARK_LIB_FUZZER=OFF`, `CMARK_FUZZ_QUADRATIC=OFF`

All of these are set as **cache entries with `FORCE`**: cmark-gfm's
`cmake_minimum_required(3.0)` leaves CMP0077 old, so a normal variable is
discarded and the project re-enables shared/tests.

libgit2 sets `CMAKE_C_STANDARD=90` in the cache; cmark-gfm needs C99
(`inline` in `config.h`). Both fetches run with `CMAKE_C_STANDARD=99` (cache
FORCE, saved and restored afterwards), so libgit2's unforced 90 does not win
and cmark builds with C99.

`ymh_render` links **both** `libcmark-gfm-extensions_static` and
`libcmark-gfm_static` (the extensions archive does not link the core archive
itself), extensions first. It gets the source/generated include directories
(`${cmark_gfm_SOURCE_DIR}/src`, `${cmark_gfm_BINARY_DIR}/src`,
`${cmark_gfm_SOURCE_DIR}/extensions`). cmark-gfm's top-level
`cmake_minimum_required(3.0)` needs `CMAKE_POLICY_VERSION_MINIMUM=3.5` on
CMake 4; that variable is scoped to the cmark fetch and unset afterwards.

The system path remains (`CMARK_GFM` required, `CMARK_GFM_EXTENSIONS`
optional) when the option is `OFF`.

### 105-D2 - Bundle libgit2

`YMH_BUNDLED_LIBGIT2` (default `ON`) fetches `libgit2/libgit2` **v1.9.7**
(`GIT_SHALLOW TRUE`) and builds a static library with:

* `BUILD_SHARED_LIBS=OFF` as a **cache** entry (FORCE, previous value saved
  and restored after the fetch). A normal variable is not enough: libgit2's
  CMake 3.5.1 policy scope leaves CMP0077 unset, so its `option()` clears a
  normal value and builds the shared library.
* `CMAKE_POSITION_INDEPENDENT_CODE=ON` for the fetch (libgit2 does not set PIC
  itself; the archive must be PIC for the PIE link). It is a normal variable,
  saved and restored around the fetch (it is not an `option()`, so CMP0077
  does not apply).
* `BUILD_TESTS=OFF`, `BUILD_CLI=OFF`, `BUILD_EXAMPLES=OFF`,
  `BUILD_FUZZERS=OFF`
* `USE_SSH=OFF`, `USE_HTTPS=OFF`, `USE_GSSAPI=OFF` (ymh's `GitService` is
  local/read-only; no network git)
* `USE_BUNDLED_ZLIB=ON`, `REGEX_BACKEND=builtin`, `USE_SHA1=CollisionDetection`,
  `USE_SHA256=builtin` (no OpenSSL/pcre2/zlib static dependencies)
* `USE_NSEC=OFF` (libgit2's nanosecond `struct stat` probe fails under the C99
  standard used for cmark; second-granularity mtimes remain, with libgit2's
  normal racy-git fallback)
* `SONAME=OFF`

Every one of these, plus `BUILD_TESTS=OFF`, `BUILD_CLI=OFF`,
`BUILD_EXAMPLES=OFF`, `BUILD_FUZZERS=OFF`, is a **cache entry with `FORCE`**.
libgit2's CMake 3.5.1 policy scope leaves CMP0077 old, so its `option()` (and
the `set(... CACHE ...)` knobs) ignore normal variables; a non-FORCE or normal
value silently re-enables tests, CLI, OpenSSL/PCRE2, and shared linking.

`ymh_execution` links `libgit2package` and gets
`${libgit2_SOURCE_DIR}/include` + `${libgit2_BINARY_DIR}/include` (the target
only exposes an install-interface include directory). The system
`PkgConfig::LIBGIT2` path remains behind the option.

### 105-D3 - No other dependency changes

`libcurl`, `libsqlite3`, the C++ runtime, and glibc stay dynamic. The binary
still needs those runtime packages; 105 only removes cmark-gfm/libgit2 from the
runtime dependency set.

### 105-D4 - Binary check

`readelf -d build/ymh` and `ldd build/ymh` must have no `libcmark-gfm`,
`libcmark-gfm-extensions`, or `libgit2` entries in the default build.

## 3. Interfaces

Two CMake options, both default `ON`:

```cmake
option(YMH_BUNDLED_CMARK_GFM "Build and statically link cmark-gfm + extensions" ON)
option(YMH_BUNDLED_LIBGIT2    "Build and statically link libgit2"                ON)
```

No C++ API change.

## 4. Implementation

* `CMakeLists.txt`
  - Replace the unconditional `pkg_check_modules(LIBGIT2 …)` /
    `pkg_check_modules(CMARK_GFM …)` with option-gated FetchContent blocks.
  - `ymh_render`: static cmark targets + include dirs under the bundled branch;
    current pkg-config links in the system branch.
  - `ymh_execution`: `libgit2package` + include dirs under the bundled branch;
    `PkgConfig::LIBGIT2` in the system branch.
  - Scope `CMAKE_POLICY_VERSION_MINIMUM=3.5` and `BUILD_SHARED_LIBS=OFF` to the
    respective FetchContent calls and restore afterwards.
* `src/execution/git_service.cpp`: replace `GIT_DIFF_OPTIONS_INIT` /
  `GIT_STATUS_OPTIONS_INIT` with value-initialized structs + explicit
  `GIT_*_OPTIONS_VERSION`. The v1.9.7 macros leave trailing fields implicit,
  which `-Wmissing-field-initializers` rejects in C++ (`-Werror`). Preserve
  the macro's non-zero defaults that value-init loses:
  `ignore_submodules = GIT_SUBMODULE_IGNORE_UNSPECIFIED` and
  `context_lines = 3`. Add a regression assertion for the three-line diff
  context.
* C standard: save `CMAKE_C_STANDARD`, set cache `99` before both fetches,
  restore after; `CMAKE_C_EXTENSIONS` stays at the subproject default.
* `README.md`: cmark-gfm/libgit2 become optional system packages; document the
  two new `-DYMH_BUNDLED_*=OFF` opt-outs.
* `docs/design/DESIGN_STATUS.md`: add row 105.

## 5. Invariants

* **105-I1.** Default configure/build succeeds without system
  cmark-gfm/libgit2 dev packages.
* **105-I2.** `readelf -d build/ymh` has no cmark-gfm/extensions/libgit2
  NEEDED entry.
* **105-I3.** `YMH_BUNDLED_CMARK_GFM=OFF` and `YMH_BUNDLED_LIBGIT2=OFF`
  reproduce the current system-link build when the dev packages exist.
* **105-I4.** Markdown rendering and all git tools behave as before; full test
  suite pass count is unchanged.
* **105-I5.** The static archives are PIC and link into the PIE executable;
  libgit2 is built with `CMAKE_POSITION_INDEPENDENT_CODE=ON`.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 105-F1 | offline configure | FetchContent error; system mode is the documented fallback |
| 105-F2 | cmark-gfm CMake 3.0 on CMake 4 | policy floor 3.5 scoped to the fetch; configure succeeds |
| 105-F3 | libgit2 static accidentally pulls OpenSSL/pcre2 | configured off; binary check catches a regression |
| 105-F6 | any libgit2/cmark knob supplied as a normal variable | ignored under CMP0077-old; every knob is cache-FORCE, tests/CLI/shared disabled |
| 105-F7 | libgit2 archive not PIC | PIC ON for the fetch; PIE link succeeds without toolchain defaults |
| 105-F4 | extensions static linked without core | link error; both targets are always linked |
| 105-F5 | system mode without dev packages | configure fails as today |
| 105-F8 | libgit2 cache forces C90, cmark needs C99 | both fetches run at C99; cmark's `inline` compiles |
| 105-F9 | libgit2 nsec probe fails at C99 | `USE_NSEC=OFF`; status uses second granularity + racy-git fallback |

## 7. State lifetime

Build-time only: two cache options, fetched source trees, static archives.
Nothing persists at runtime.

## 8. Tests

1. Default configure + build; binary check for cmark/libgit2 NEEDED.
2. `Errata*`/render/git tests and full `ctest` (same count).
3. Optional manual `-DYMH_BUNDLED_CMARK_GFM=OFF -DYMH_BUNDLED_LIBGIT2=OFF`
   with system dev packages.

## 9. dsh mapping

Not a dsh-mirrored capability; build-only.

## 10. Amendments

* README prerequisites: cmark-gfm/libgit2 become optional under the default
  bundled build; system mode documented.
