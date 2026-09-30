#pragma once

// 72-D1/72-D2: the single compile-time app version. CMake supplies the macro
// from the top-level `project(ymh VERSION ...)`:
//   target_compile_definitions(<target> PRIVATE YMH_VERSION="${PROJECT_VERSION}")
// The fallback keeps non-CMake/editor TU builds compiling; it is never shipped.
#ifndef YMH_VERSION
#define YMH_VERSION "0.0.0"
#endif
