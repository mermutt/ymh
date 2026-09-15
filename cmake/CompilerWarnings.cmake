# Compiler warning policy for first-party ymh targets.
#
# No suppression is permitted (see AGENTS.md): no -w, no blanket
# `#pragma GCC diagnostic ignored`, no casts used to silence the compiler.
# Warnings are errors so that a warning can never silently accumulate.
#
# Third-party code pulled in through FetchContent does NOT link this target,
# so a dependency's own warnings cannot fail the ymh build.

add_library(ymh_warnings INTERFACE)
add_library(ymh::warnings ALIAS ymh_warnings)

target_compile_options(
    ymh_warnings
    INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
)
