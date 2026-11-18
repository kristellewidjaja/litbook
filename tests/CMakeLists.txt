include(GoogleTest)

# One test executable for the whole project; test files mirror src/ and include/.
add_executable(litbook_tests
    version_test.cpp
    util/bits_test.cpp)

target_link_libraries(litbook_tests PRIVATE
    litbook::litbook
    litbook::warnings
    GTest::gtest_main)

# PRE_TEST: discover test names when ctest runs, not at build time
# (keeps builds working under sanitizers and cross-compilation).
gtest_discover_tests(litbook_tests DISCOVERY_MODE PRE_TEST)
