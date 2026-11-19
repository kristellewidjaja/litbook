include(GoogleTest)

# This is the main test executable. Its test files mirror src/ and include/.
add_executable(litbook_tests
    version_test.cpp
    container/flat_id_map_test.cpp
    container/index_pool_test.cpp
    container/occupancy_bitmap_test.cpp
    book/order_book_test.cpp
    book/order_book_rules_test.cpp
    book/order_book_model_test.cpp
    book/map_order_book_test.cpp
    book/fast_order_book_test.cpp
    book/differential_test.cpp
    metrics/latency_histogram_test.cpp
    metrics/tick_clock_test.cpp)

target_include_directories(litbook_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(litbook_tests PRIVATE
    litbook::litbook
    litbook::warnings
    GTest::gtest_main)

# PRE_TEST discovers test names when ctest runs, not at build time, which keeps builds
# working under sanitizers and cross-compilation. TIMEOUT makes a bug that loops matching
# forever fail the test after 5 minutes instead of hanging CI.
gtest_discover_tests(litbook_tests
    DISCOVERY_MODE PRE_TEST
    NO_PRETTY_VALUES  # names parameterized tests by suffix, not a parameter dump
    PROPERTIES TIMEOUT 300)

# This test is a separate executable because it replaces the global operator new/delete
# to count allocations, which must not affect the other tests. It is not built under
# ThreadSanitizer, because Clang's TSan runtime defines operator new itself (a link error)
# and this test is single-threaded, so TSan has nothing to check in it. ASan and UBSan
# builds do run it.
if(NOT LITBOOK_SANITIZER MATCHES "thread")
    add_executable(litbook_alloc_tests
        alloc/no_allocation_test.cpp)

    target_include_directories(litbook_alloc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(litbook_alloc_tests PRIVATE
        litbook::litbook
        litbook::warnings
        GTest::gtest_main)

    gtest_discover_tests(litbook_alloc_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 300)
endif()
