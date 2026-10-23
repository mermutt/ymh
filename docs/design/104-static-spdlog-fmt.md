# 104 - Statically link spdlog and fmt

Status: verified (Rev 1)
Revision: 1
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 1)

## 1. Why

`ymh` currently links the distro's `libspdlog.so` and `libfmt.so`. The soname
is not stable across supported hosts: Ubuntu 22.04 ships `libspdlog.so.1.9`
(`1.9.2`), while a binary built on 24.04 requests `libspdlog.so.1.12` and
fails on 22.04 with `error while loading shared libraries:
libspdlog.so.1.12`. The same class of failure applies to `libfmt` (fmt 8 vs 9
vs 12). spdlog/fmt are small, header/static-friendly libraries with no ABI
surface exposed by ymh; bundling them removes the runtime soname dependency
without changing any other dependency.

## 2. Decisions

### 104-D1 - Bundle spdlog + fmt by default

Add a CMake option:

```cmake
option(YMH_BUNDLED_SPDLOG "Build and statically link spdlog/fmt" ON)
```

When `ON` (default), CMake fetches pinned `gabime/spdlog` **v1.17.0**
(`GIT_SHALLOW TRUE`) and builds it with:

* `SPDLOG_BUILD_SHARED=OFF` (static archive)
* `SPDLOG_BUILD_PIC=ON` (ymh is a PIE)
* `SPDLOG_FMT_EXTERNAL=OFF` (bundled fmt; no separate `libfmt`)
* `SPDLOG_BUILD_EXAMPLE=OFF`, `SPDLOG_BUILD_TESTS=OFF`,
  `SPDLOG_BUILD_BENCH=OFF`, `SPDLOG_INSTALL=OFF`

`fmt` is not found or linked separately in this mode; spdlog's bundled fmt is
compiled into the archive.

### 104-D2 - System mode remains available

When `OFF`, the current behavior is unchanged: `find_package(spdlog REQUIRED)`
and `find_package(fmt REQUIRED)`, linking the distro shared libraries. This is
the escape hatch for distro packages and build hosts that already have the
spdlog/fmt dev packages (the other FetchContent dependencies still need their
sources).

### 104-D3 - No other dependency changes

`libcurl`, `libgit2`, `libcmark-gfm`, `libsqlite3`, and the C++ runtime remain
dynamic. 104 does not claim a fully static binary; a host still needs the
corresponding runtime packages (documented in README).

### 104-D4 - Binary check

The default build's `ymh` must have no `DT_NEEDED` entry for `libspdlog` or
`libfmt`; `ldd build/ymh` must not list either. The static archive is linked
through the existing `ymh_logging` target's `PRIVATE spdlog::spdlog`, which
CMake propagates to the final link because the library is static.

## 3. Interfaces

One new CMake cache option, `YMH_BUNDLED_SPDLOG` (default `ON`). No C++ API
change.

## 4. Implementation

* `CMakeLists.txt`: replace the unconditional
  `find_package(spdlog REQUIRED)` / `find_package(fmt REQUIRED)` with the
  option-gated block above; keep `FetchContent` from line 33.
* `README.md`: spdlog/fmt are fetched and statically linked by default; the
  `libspdlog-dev`/`libfmt-dev` apt entries become optional
  (`-DYMH_BUNDLED_SPDLOG=OFF`).
* `docs/design/DESIGN_STATUS.md`: add row 104.

## 5. Invariants

* **104-I1.** Default configure + build succeeds without system spdlog/fmt
  development packages (network available for FetchContent).
* **104-I2.** `readelf -d build/ymh` has no `libspdlog`/`libfmt` NEEDED entry.
* **104-I3.** `-DYMH_BUNDLED_SPDLOG=OFF` reproduces the current system-link
  build when the dev packages are installed.
* **104-I4.** The full test suite is unchanged (same tests, same pass count).
* **104-I5.** Static objects are PIC, so the PIE link succeeds on x86-64.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 104-F1 | FetchContent unavailable (offline, no source cache) | configure fails with a clear CMake error; system mode is the documented fallback |
| 104-F2 | system mode without `libspdlog-dev`/`libfmt-dev` | configure fails as today |
| 104-F3 | static spdlog compiled non-PIC | link error; `SPDLOG_BUILD_PIC=ON` prevents it |
| 104-F4 | pinned tag unavailable | configure/FetchContent error; the tag is pinned and must exist |

## 7. State lifetime

Build-time only: a cache option, fetched source tree, and a static archive.
Nothing persists at runtime.

## 8. Tests

1. Default configure + build; `readelf -d build/ymh | grep -E 'spdlog|fmt'`
   is empty; `ldd build/ymh` has no spdlog/fmt.
2. `-DYMH_BUNDLED_SPDLOG=OFF` configure + build on a host with system dev
   packages (manual, optional in CI).
3. Full `ctest` on the default build.

## 9. dsh mapping

Not a dsh-mirrored capability; build-only.

## 10. Amendments

* README build prerequisites: spdlog/fmt become optional for the default
  bundled build.
