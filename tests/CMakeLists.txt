# Focused tests for the standalone code checkout. The full historical suite
# lives on `dev`; this target covers the append-only session store, migration
# and the local/remote liveness split introduced with it.

add_executable(ymh_file_store_tests
    session_file_store_test.cpp
    registry_liveness_test.cpp
    runtime_guard_test.cpp
)

target_include_directories(ymh_file_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ymh_file_store_tests
    PRIVATE
        GTest::gtest_main
        ymh::session
        ymh::registry
        ymh::prompt
        ymh::core
        SQLite3::SQLite3
)

include(GoogleTest)
gtest_discover_tests(ymh_file_store_tests)
