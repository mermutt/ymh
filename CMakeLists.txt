cmake_minimum_required(VERSION 3.25)

project(
    ymh
    VERSION 0.1.0
    DESCRIPTION "Terminal coding-agent harness"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Language / build configuration
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FetchContent)
include(CompilerWarnings)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(YMH_BUILD_TESTS "Build the ymh test suite" ON)

# 14 §13.4 / 24 §11 (memory-safety rule): the whole suite must be built and run
# under both AddressSanitizer and ThreadSanitizer in CI. Off by default so the
# normal build is unchanged. The two are mutually exclusive (they cannot share
# a link line).
#
# `-O0` (not `-O1`) is deliberate: at `-O1` GCC 16 emits optimization-dependent
# `-Wmaybe-uninitialized` false positives from libstdc++ `std::regex`
# (`src/execution/filesystem.cpp`) and nlohmann `json::items()`
# (`src/mcp/schema_translation.cpp`). Warnings stay errors; only the
# optimization level that manufactures the false positives is avoided.
#
#   cmake -S . -B build-asan -G Ninja -DYMH_ASAN=ON  -DCMAKE_BUILD_TYPE=Debug
#   cmake -S . -B build-tsan -G Ninja -DYMH_TSAN=ON  -DCMAKE_BUILD_TYPE=Debug
option(YMH_ASAN "Build with AddressSanitizer" OFF)
option(YMH_TSAN "Build with ThreadSanitizer" OFF)
if(YMH_ASAN AND YMH_TSAN)
    message(FATAL_ERROR "YMH_ASAN and YMH_TSAN are mutually exclusive")
endif()
if(YMH_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O0)
    add_link_options(-fsanitize=address)
endif()
if(YMH_TSAN)
    add_compile_options(-fsanitize=thread -g -O0)
    add_link_options(-fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies (FetchContent)
#
# Declared eagerly so the full dependency set is configured in one place, but
# only the pieces required by the current wave are linked. SQLite3 / nlohmann /
# spdlog / fmt / CURL come from the system via find_package.
# ---------------------------------------------------------------------------

# --- FTXUI (TUI; arrives with the UI wave) ---------------------------------
set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS_FUZZER OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_DEV_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v6.1.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

# --- Asio standalone (async; arrives with the daemon/transport wave) --------
# The asio repository ships headers only (no root CMakeLists.txt), so the
# interface target is declared explicitly.
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-38-2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(asio)
if(NOT TARGET asio::asio)
    add_library(asio INTERFACE)
    add_library(asio::asio ALIAS asio)
    target_include_directories(asio INTERFACE "${asio_SOURCE_DIR}/include")
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
endif()

# --- CLI11 (command line) ---------------------------------------------------
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_SINGLE_FILE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.5.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cli11)

# --- GoogleTest (tests only) ------------------------------------------------
if(YMH_BUILD_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# System dependencies (find_package)
# ---------------------------------------------------------------------------

find_package(SQLite3 REQUIRED)
# `SQLite::SQLite3` is deprecated as of CMake 4.3; use the new name and provide
# the alias on older CMake (the fallback recommended by FindSQLite3).
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# --- libgit2 (Git tools, §29) / cmark-gfm (Markdown, §22) -------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(CMARK_GFM REQUIRED IMPORTED_TARGET libcmark-gfm)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

add_library(ymh_core STATIC
    src/core/clock.cpp
    src/core/event.cpp
    src/core/event_bus.cpp
    src/core/retention.cpp
)
add_library(ymh::core ALIAS ymh_core)

target_include_directories(ymh_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_core
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Session & persistence library (component 01 + 02)
# ---------------------------------------------------------------------------

add_library(ymh_session STATIC
    src/session/events.cpp
    src/session/session.cpp
    src/session/session_persistence.cpp
    src/session/session_manager.cpp
    src/session/plan_mode.cpp
)
add_library(ymh::session ALIAS ymh_session)

target_include_directories(ymh_session
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_session
    PUBLIC
        ymh::core
        nlohmann_json::nlohmann_json
    PRIVATE
        SQLite3::SQLite3
        ymh::warnings
)

# ---------------------------------------------------------------------------
# LLM provider library (component 08)
# ---------------------------------------------------------------------------

add_library(ymh_llm STATIC
    src/llm/assistant_stream.cpp
    src/llm/llm_provider.cpp
    src/llm/llm_runtime.cpp
    src/llm/sha256.cpp
    src/llm/provider_registry.cpp
    src/llm/tool_call_assembler.cpp
    src/llm/sse_parser.cpp
    src/llm/redaction.cpp
    src/llm/openai_adapter.cpp
    src/llm/fake_llm.cpp
)
add_library(ymh::llm ALIAS ymh_llm)

target_include_directories(ymh_llm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_llm
    PUBLIC
        ymh::core
        nlohmann_json::nlohmann_json
    PRIVATE
        CURL::libcurl
        Threads::Threads
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Execution environment library (component 07 §6, §18)
# ---------------------------------------------------------------------------

add_library(ymh_execution STATIC
    src/execution/errors.cpp
    src/execution/glob.cpp
    src/execution/output.cpp
    src/execution/filesystem.cpp
    src/execution/process.cpp
    src/execution/environment.cpp
    src/execution/git_service.cpp
    src/execution/resource_governor.cpp
    src/execution/pty.cpp
)
add_library(ymh::execution ALIAS ymh_execution)

target_include_directories(ymh_execution
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_execution
    PUBLIC
        ymh::core
        nlohmann_json::nlohmann_json
    PRIVATE
        asio::asio
        PkgConfig::LIBGIT2
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Permissions & policy library (component 09)
# ---------------------------------------------------------------------------

add_library(ymh_policy STATIC
    src/policy/permission_policy.cpp
)
add_library(ymh::policy ALIAS ymh_policy)

target_include_directories(ymh_policy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_policy
    PUBLIC
        ymh::core
        ymh::session
        ymh::execution
        nlohmann_json::nlohmann_json
    PRIVATE
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Permission broker library (component 09 §4, errata §7)
# ---------------------------------------------------------------------------

add_library(ymh_permission STATIC
    src/permission/permission_broker.cpp
)
add_library(ymh::permission ALIAS ymh_permission)

target_include_directories(ymh_permission
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_permission
    PUBLIC
        ymh::policy
        nlohmann_json::nlohmann_json
    PRIVATE
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Tools library (component 07 §3-§5)
# ---------------------------------------------------------------------------

add_library(ymh_tools STATIC
    src/tools/tool.cpp
    src/tools/tool_context.cpp
    src/tools/tool_registry.cpp
    src/tools/builtin_tools.cpp
    src/tools/git_tools.cpp
    src/tools/plan_tools.cpp
    src/tools/terminal_tool.cpp
)
add_library(ymh::tools ALIAS ymh_tools)

target_include_directories(ymh_tools
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_tools
    PUBLIC
        ymh::core
        ymh::session
        ymh::execution
        ymh::policy
        nlohmann_json::nlohmann_json
    PRIVATE
        ymh::warnings
)

# ---------------------------------------------------------------------------
# MCP adapter library (component 15)
# ---------------------------------------------------------------------------

add_library(ymh_mcp STATIC
    src/mcp/mcp_types.cpp
    src/mcp/schema_translation.cpp
    src/mcp/mcp_transport.cpp
    src/mcp/mcp_client.cpp
    src/mcp/mcp_tool.cpp
    src/mcp/mcp_manager.cpp
)
add_library(ymh::mcp ALIAS ymh_mcp)

target_include_directories(ymh_mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_mcp
    PUBLIC
        ymh::core
        ymh::session
        ymh::execution
        ymh::policy
        ymh::tools
        nlohmann_json::nlohmann_json
    PRIVATE
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Skills library (component 20): discovery, frontmatter, the `skill` tool
# ---------------------------------------------------------------------------

add_library(ymh_skills STATIC
    src/skills/skill_types.cpp
    src/skills/frontmatter.cpp
    src/skills/skill_catalog.cpp
    src/skills/skill_tool.cpp
)
add_library(ymh::skills ALIAS ymh_skills)

target_include_directories(ymh_skills
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_skills
    PUBLIC
        ymh::core
        ymh::execution
        ymh::tools
    PRIVATE
        ymh::config
        ymh::warnings
        nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# Prompt registry library (component 36): ordered sections, persona, rendering
# ---------------------------------------------------------------------------

add_library(ymh_prompt STATIC
    src/prompt/instructions.cpp
    src/prompt/order.cpp
    src/prompt/persona.cpp
    src/prompt/runtime_context.cpp
    src/prompt/system_prompt.cpp
)
add_library(ymh::prompt ALIAS ymh_prompt)

target_include_directories(ymh_prompt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_prompt
    PUBLIC
        ymh::core
        ymh::tools
    PRIVATE
        ymh::config
        ymh::execution
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Agent & loop library (component 06)
# ---------------------------------------------------------------------------

add_library(ymh_agent STATIC
    src/agent/agent.cpp
    src/agent/agent_loop.cpp
    src/agent/agent_registry.cpp
    src/agent/chunk_coalescer.cpp
    src/agent/compactor.cpp
    src/agent/context_assembler.cpp
    src/agent/context_snapshot.cpp
    src/agent/llm_pool.cpp
    src/agent/plan_mode_controller.cpp
    src/agent/preset.cpp
    src/agent/repeat_tool_reminder.cpp
    src/agent/subagent.cpp
    src/agent/tool_result_pruner.cpp
    src/commands/goal_command.cpp
    src/goal/goal_service.cpp
    src/goal/round_driver.cpp
)
add_library(ymh::agent ALIAS ymh_agent)

target_include_directories(ymh_agent
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_agent
    PUBLIC
        ymh::core
        ymh::session
        ymh::llm
        ymh::execution
        ymh::policy
        ymh::prompt
        ymh::tools
        nlohmann_json::nlohmann_json
    PRIVATE
        ymh::config
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Configuration library (component: config, §37)
# ---------------------------------------------------------------------------

add_library(ymh_config STATIC
    src/config/config.cpp
)
add_library(ymh::config ALIAS ymh_config)

target_include_directories(ymh_config
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_config
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        ymh::mcp
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Logging library (§40)
# ---------------------------------------------------------------------------

add_library(ymh_logging STATIC
    src/core/logging.cpp
)
add_library(ymh::logging ALIAS ymh_logging)

target_include_directories(ymh_logging
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_logging
    PUBLIC
        ymh::core
    PRIVATE
        ymh::llm
        spdlog::spdlog
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Wiring library (config -> services helpers, shared by CLI and TUI)
# ---------------------------------------------------------------------------

add_library(ymh_wiring STATIC
    src/cli/wiring.cpp
)
add_library(ymh::wiring ALIAS ymh_wiring)

target_include_directories(ymh_wiring
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_wiring
    PUBLIC
        ymh::agent
        ymh::config
        ymh::llm
        ymh::mcp
        ymh::policy
        ymh::skills
    PRIVATE
        ymh::registry
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Workspace runtime library (00 §9.6, 04 §3.7): one owner per workspace
# ---------------------------------------------------------------------------

add_library(ymh_runtime STATIC
    src/agent/workspace_runtime.cpp
)
add_library(ymh::runtime ALIAS ymh_runtime)

target_include_directories(ymh_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_runtime
    PUBLIC
        ymh::agent
        ymh::config
        ymh::logging
        ymh::mcp
        ymh::wiring
    PRIVATE
        ymh::skills
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Workspace registry (component 03)
# ---------------------------------------------------------------------------

add_library(ymh_registry STATIC
    src/registry/registry.cpp
    src/registry/liveness.cpp
    src/registry/process_scan.cpp
    src/registry/supervisor.cpp
    src/registry/workspace_cli.cpp
)
add_library(ymh::registry ALIAS ymh_registry)

target_include_directories(ymh_registry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_registry
    PUBLIC
        ymh::core
    PRIVATE
        nlohmann_json::nlohmann_json
        SQLite3::SQLite3
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Transport library (component 05): JSON-RPC over a length-prefixed Unix socket
# ---------------------------------------------------------------------------

add_library(ymh_transport STATIC
    src/transport/protocol.cpp
    src/transport/frame_codec.cpp
    src/transport/json_rpc.cpp
    src/transport/protocol_server.cpp
    src/transport/host_connection.cpp
    src/transport/transport_server.cpp
)
add_library(ymh::transport ALIAS ymh_transport)

target_include_directories(ymh_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_transport
    PUBLIC
        ymh::core
        nlohmann_json::nlohmann_json
        asio::asio
    PRIVATE
        Threads::Threads
        ymh::warnings
)

# ---------------------------------------------------------------------------
# HostRuntime bridge (M2 §4): TransportHost over WorkspaceRuntime
# ---------------------------------------------------------------------------

add_library(ymh_host STATIC
    src/host/host_runtime.cpp
    src/host/workspace_host.cpp
)
add_library(ymh::host ALIAS ymh_host)

target_include_directories(ymh_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_host
    PUBLIC
        ymh::runtime
        ymh::registry
        ymh::transport
        ymh::permission
        ymh::agent
    PRIVATE
        ymh::skills
        nlohmann_json::nlohmann_json
        Threads::Threads
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Render library (component 10 §8.2): markdown, syntax, diff (§22-§24)
# ---------------------------------------------------------------------------

add_library(ymh_render STATIC
    src/ui/render/syntax_renderer.cpp
    src/ui/render/markdown_renderer.cpp
    src/ui/render/diff_renderer.cpp
)
add_library(ymh::render ALIAS ymh_render)

target_include_directories(ymh_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_render
    PUBLIC
        ftxui::component
    PRIVATE
        PkgConfig::CMARK_GFM
        ymh::warnings
)

# ---------------------------------------------------------------------------
# Terminal UI library (component 10)
# ---------------------------------------------------------------------------

add_library(ymh_ui STATIC
    src/ui/ui_model.cpp
    src/ui/ui_event_adapter.cpp
    src/ui/ui_render.cpp
    src/ui/command_registry.cpp
    src/ui/session_export.cpp
    src/ui/terminal_layer.cpp
    src/ui/supervisor_connection.cpp
    src/ui/supervisor_presence.cpp
    src/ui/session_catalog.cpp
    src/ui/supervisor.cpp
)
add_library(ymh::ui ALIAS ymh_ui)

target_include_directories(ymh_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_ui
    PUBLIC
        ymh::core
        ymh::session
        ymh::llm
        ymh::execution
        ymh::policy
        ymh::tools
        ymh::agent
        ymh::config
        ymh::logging
        ymh::wiring
        ymh::runtime
        ymh::render
        ymh::registry
        ymh::host
        ymh::transport
        ftxui::component
    PRIVATE
        Threads::Threads
        ymh::warnings
)

# ---------------------------------------------------------------------------
# CLI library (§41, §42)
# ---------------------------------------------------------------------------

add_library(ymh_cli STATIC
    src/cli/provider_factory.cpp
    src/cli/headless.cpp
    src/cli/session_cli.cpp
    src/cli/assistant_stream_printer.cpp
    src/cli/cli.cpp
)
add_library(ymh::cli ALIAS ymh_cli)

target_include_directories(ymh_cli
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ymh_cli
    PUBLIC
        ymh::core
        ymh::session
        ymh::llm
        ymh::execution
        ymh::policy
        ymh::tools
        ymh::agent
        ymh::config
        ymh::logging
        ymh::wiring
        ymh::runtime
        ymh::registry
        ymh::host
        ymh::ui
    PRIVATE
        CLI11::CLI11
        SQLite3::SQLite3
        Threads::Threads
        ymh::warnings
)

target_compile_definitions(ymh_cli PRIVATE YMH_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(ymh src/main.cpp)
target_link_libraries(ymh
    PRIVATE
        ymh::cli
        ymh::warnings
)
target_compile_definitions(ymh PRIVATE YMH_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(YMH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
