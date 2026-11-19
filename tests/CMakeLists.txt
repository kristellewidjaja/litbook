include(GoogleTest)

# One test executable for the whole project; test files mirror src/ and include/.
add_executable(litbook_tests
    version_test.cpp
    book/map_order_book_test.cpp
    book/map_order_book_model_test.cpp)

target_include_directories(litbook_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(litbook_tests PRIVATE
    litbook::litbook
    litbook::warnings
    GTest::gtest_main)

# PRE_TEST: discover test names when ctest runs, not at build time
# (keeps builds working under sanitizers and cross-compilation).
gtest_discover_tests(litbook_tests
    DISCOVERY_MODE PRE_TEST
    NO_PRETTY_VALUES)  # name parameterized tests by their suffix, not a dump of the parameter
