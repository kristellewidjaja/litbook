# Command-line tools that work with real market data.

# One pass over a full ITCH day. It prints message counts and, per stock, the peak number
# of resting orders and the price range, which decide how the books are sized.
add_executable(litbook_itch_stats itch_stats.cpp)
target_link_libraries(litbook_itch_stats PRIVATE litbook::litbook litbook::warnings)

# Replays a full ITCH day through one book per stock and times every message. This is
# the headline benchmark (see the usage comment at the top of itch_replay.cpp).
add_executable(litbook_itch_replay itch_replay.cpp)
target_link_libraries(litbook_itch_replay PRIVATE litbook::litbook litbook::warnings)

# End-to-end tests that run the replay tool on a tiny generated ITCH day. They live in their
# own executable so the main unit tests do not depend on the tools.
if(LITBOOK_BUILD_TESTS)
    include(GoogleTest)
    add_executable(litbook_tool_tests ${PROJECT_SOURCE_DIR}/tests/tools/itch_replay_smoke_test.cpp)
    target_link_libraries(litbook_tool_tests PRIVATE litbook::warnings GTest::gtest_main)
    target_compile_definitions(litbook_tool_tests PRIVATE
        LITBOOK_ITCH_REPLAY="$<TARGET_FILE:litbook_itch_replay>")
    add_dependencies(litbook_tool_tests litbook_itch_replay)
    gtest_discover_tests(litbook_tool_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 300)
endif()
