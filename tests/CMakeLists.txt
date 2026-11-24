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
    book/sorted_order_book_test.cpp
    book/differential_test.cpp
    book/book_restore_test.cpp
    client/feed_book_test.cpp
    client/order_client_test.cpp
    client/order_tracker_test.cpp
    concurrency/mutex_queue_test.cpp
    concurrency/queue_contract_test.cpp
    concurrency/spsc_queue_test.cpp
    concurrency/thread_affinity_test.cpp
    engine/engine_test.cpp
    engine/ticker_test.cpp
    feed/feed_codec_test.cpp
    feed/feed_handler_test.cpp
    feed/feed_publisher_test.cpp
    feed/feed_receiver_test.cpp
    feed/level_book_test.cpp
    feed/lossy_feed_test.cpp
    feed/mold_test.cpp
    feed/multicast_test.cpp
    gateway/byte_buffer_test.cpp
    gateway/epoll_gateway_test.cpp
    gateway/session_manager_async_test.cpp
    gateway/session_manager_test.cpp
    gateway/session_test.cpp
    gateway/soup_test.cpp
    itch/binary_file_reader_test.cpp
    loadgen/client_session_test.cpp
    loadgen/in_flight_queue_test.cpp
    loadgen/load_report_test.cpp
    loadgen/load_run_test.cpp
    loadgen/order_flow_test.cpp
    loadgen/pacer_test.cpp
    itch/book_builder_test.cpp
    itch/itch_parser_test.cpp
    journal/crc32c_test.cpp
    journal/directory_replay_test.cpp
    journal/journal_directory_test.cpp
    journal/journal_format_test.cpp
    journal/journal_reader_test.cpp
    journal/journal_writer_test.cpp
    journal/replay_test.cpp
    journal/setup_test.cpp
    journal/snapshot_format_test.cpp
    journal/snapshot_replay_test.cpp
    journal/snapshot_store_test.cpp
    journal/steady_stream_test.cpp
    metrics/latency_histogram_test.cpp
    pipeline/doorbell_test.cpp
    pipeline/feed_worker_test.cpp
    pipeline/journal_worker_test.cpp
    pipeline/snapshot_worker_test.cpp
    pipeline/manual_pipeline_test.cpp
    pipeline/matching_worker_test.cpp
    pipeline/messages_test.cpp
    pipeline/queue_backend_test.cpp
    pipeline/reply_pump_test.cpp
    pipeline/threaded_feed_test.cpp
    pipeline/threaded_gateway_test.cpp
    pipeline/threaded_journal_test.cpp
    protocol/byte_order_test.cpp
    protocol/codec_test.cpp
    protocol/order_entry_test.cpp
    protocol/order_entry_restore_test.cpp
    protocol/client_map_key_test.cpp
    metrics/tick_clock_test.cpp)

target_include_directories(litbook_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The snapshot format test reads the fuzz seeds that an independent encoder wrote.
target_compile_definitions(litbook_tests
    PRIVATE LITBOOK_SNAPSHOT_SEEDS="${PROJECT_SOURCE_DIR}/fuzz/snapshot_seeds")

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
