#pragma once

// 96-D8: the single compile-time app version. CMake stamps `.version_info` at
// configure time and `configure_file` emits `ymh/build_version.hpp`, which the
// three consumers include through the generated include path. The `"unknown"`
// fallback keeps non-CMake/editor TU builds compiling and makes a target that
// misses the generated include visibly wrong rather than plausibly semver.
#if defined(__has_include)
#  if __has_include("ymh/build_version.hpp")
#    include "ymh/build_version.hpp"
#  endif
#endif
#ifndef YMH_VERSION
#define YMH_VERSION "unknown"
#endif
