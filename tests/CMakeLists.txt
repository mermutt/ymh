# Focused tests for the standalone code checkout. The full historical suite
# lives on `dev`; this target covers the append-only session store, migration
# and the local/remote liveness split introduced with it.

add_executable(ymh_file_store_tests
    session_file_store_test.cpp
    registry_liveness_test.cpp
    runtime_guard_test.cpp
    ymh_access_test.cpp
    skills_test.cpp
    tmp_access_test.cpp
    session_projection_test.cpp
    prompt_images_test.cpp
    prompt_images_explicit_test.cpp
    prompt_url_test.cpp
    vision_capability_test.cpp
    tool_images_test.cpp
    tool_call_assembler_test.cpp
    tool_result_pruner_test.cpp
    hello_identity_test.cpp
    config_fingerprint_test.cpp
    workspace_target_test.cpp
    workspace_roots_test.cpp
    workspace_roots_store_test.cpp
    permission_roots_test.cpp
    permission_broker_roots_test.cpp
)

target_include_directories(ymh_file_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ymh_file_store_tests
    PRIVATE
        GTest::gtest_main
        ymh::session
        ymh::registry
        ymh::prompt
        ymh::policy
        ymh::execution
        ymh::permission
        ymh::skills
        ymh::config
        ymh::agent
        ymh::transport
        ymh::core
        SQLite3::SQLite3
        Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(ymh_file_store_tests)
