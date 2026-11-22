add_executable(litbook_bench
    hello_bench.cpp
    book_bench.cpp
    protocol_bench.cpp
    queue_bench.cpp
    feed_bench.cpp)

# The book benchmarks replay streams from the randomized-test workload generator.
target_include_directories(litbook_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests)

target_link_libraries(litbook_bench PRIVATE
    litbook::litbook
    litbook::warnings
    benchmark::benchmark_main)

# The latency tool times every single operation and prints percentiles. It is a plain
# program rather than a Google Benchmark, because Google Benchmark reports averages.
add_executable(litbook_latency latency_main.cpp)
target_include_directories(litbook_latency PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(litbook_latency PRIVATE litbook::litbook litbook::warnings)

# A short run as a test, so every preset (including the sanitizers) checks that the tool
# works. The numbers from this run mean nothing.
if(LITBOOK_BUILD_TESTS)
    add_test(NAME LatencyToolSmokeTest COMMAND litbook_latency 2000)
    set_tests_properties(LatencyToolSmokeTest PROPERTIES TIMEOUT 300)
endif()

# Round trips through the TCP gateway on loopback (see the comment at the top of the file).
add_executable(litbook_gateway_latency gateway_latency.cpp)
target_link_libraries(litbook_gateway_latency PRIVATE litbook::litbook litbook::warnings)

if(LITBOOK_BUILD_TESTS)
    add_test(NAME GatewayLatencySmokeTest COMMAND litbook_gateway_latency 200 blocking)
    add_test(NAME GatewayLatencySpinSmokeTest COMMAND litbook_gateway_latency 200 spin)
    # The Phase 7 pipeline, both queue kinds, the throughput mode and pinning.
    add_test(NAME GatewayLatencySpscSpinSmokeTest
             COMMAND litbook_gateway_latency 200 spin --pipeline spsc)
    add_test(NAME GatewayLatencyMutexBlockingSmokeTest
             COMMAND litbook_gateway_latency 200 blocking --pipeline mutex --sample-every 1)
    add_test(NAME GatewayLatencyWindowSmokeTest
             COMMAND litbook_gateway_latency 200 blocking --pipeline spsc --window 64)
    add_test(NAME GatewayLatencyInlineWindowSmokeTest
             COMMAND litbook_gateway_latency 200 spin --window 8 --pin 0 0 0 0)
    add_test(NAME GatewayLatencyBadPipelineTest
             COMMAND litbook_gateway_latency 200 --pipeline nope)
    add_test(NAME GatewayLatencyBadWindowTest COMMAND litbook_gateway_latency 200 --window 513)
    add_test(NAME GatewayLatencyShortPinTest COMMAND litbook_gateway_latency 200 --pin 0 1)
    set_tests_properties(GatewayLatencySmokeTest GatewayLatencySpinSmokeTest
                         GatewayLatencySpscSpinSmokeTest GatewayLatencyMutexBlockingSmokeTest
                         GatewayLatencyWindowSmokeTest GatewayLatencyInlineWindowSmokeTest
                         PROPERTIES TIMEOUT 300)
    # A run that left orders behind or dropped a reply is a failure even if it finished.
    set_tests_properties(GatewayLatencySpscSpinSmokeTest GatewayLatencyMutexBlockingSmokeTest
                         GatewayLatencyWindowSmokeTest GatewayLatencyInlineWindowSmokeTest
                         PROPERTIES PASS_REGULAR_EXPRESSION
                         "health: live orders 0, replies [0-9]+, dropped replies 0")
    set_tests_properties(GatewayLatencyBadPipelineTest GatewayLatencyBadWindowTest
                         GatewayLatencyShortPinTest PROPERTIES WILL_FAIL TRUE)
endif()

# Passing messages between two threads with SpscQueue and MutexQueue (see the comment at
# the top of the file).
add_executable(litbook_queue_latency queue_latency.cpp)
target_link_libraries(litbook_queue_latency PRIVATE litbook::litbook litbook::warnings)

if(LITBOOK_BUILD_TESTS)
    add_test(NAME QueueLatencySmokeTest COMMAND litbook_queue_latency 2000)
    add_test(NAME QueueLatencyPinnedSmokeTest COMMAND litbook_queue_latency 2000 --pin 0 1)
    add_test(NAME QueueLatencyBadArgumentsTest COMMAND litbook_queue_latency 2000 --pin 0)
    # The options used to explain the mutex wait result. One CPU for both threads is only
    # sensible for mutex wait, which sleeps instead of spinning.
    add_test(NAME QueueLatencyDetailSmokeTest
             COMMAND litbook_queue_latency 2000 --only mutex-wait --detail --pin 0 0)
    add_test(NAME QueueLatencyIdleSpinnerSmokeTest
             COMMAND litbook_queue_latency 2000 --only spsc --idle-spinner 0)
    add_test(NAME QueueLatencyUnknownSetupTest COMMAND litbook_queue_latency 2000 --only nope)
    add_test(NAME QueueLatencyUnknownFlagTest COMMAND litbook_queue_latency 2000 --fast)
    set_tests_properties(QueueLatencySmokeTest QueueLatencyPinnedSmokeTest
                         QueueLatencyDetailSmokeTest QueueLatencyIdleSpinnerSmokeTest
                         PROPERTIES TIMEOUT 300)
    set_tests_properties(QueueLatencyBadArgumentsTest QueueLatencyUnknownSetupTest
                         QueueLatencyUnknownFlagTest PROPERTIES WILL_FAIL TRUE)
    # The detail lines must really be printed, one per pass.
    set_tests_properties(QueueLatencyDetailSmokeTest PROPERTIES
                         PASS_REGULAR_EXPRESSION "pass 5: p50 [0-9]+ ns.*helper CPU 0 to 0")
endif()

# From an order on TCP to its price level on the multicast feed (see the top of the file).
add_executable(litbook_feed_latency feed_latency.cpp)
target_link_libraries(litbook_feed_latency PRIVATE litbook::litbook litbook::warnings)

if(LITBOOK_BUILD_TESTS)
    add_test(NAME FeedLatencySpinSmokeTest COMMAND litbook_feed_latency 200 spin)
    add_test(NAME FeedLatencyBlockingSmokeTest COMMAND litbook_feed_latency 200 blocking)
    # A run that lost a packet or left an order behind is not a clean measurement.
    set_tests_properties(FeedLatencySpinSmokeTest FeedLatencyBlockingSmokeTest PROPERTIES
        TIMEOUT 300
        PASS_REGULAR_EXPRESSION "health: live orders 0, level updates applied [0-9]+, gaps 0, recoveries 0, dropped by sender 0, ended yes")
endif()

# How long a late joiner needs to be in step, against the number of stocks.
add_executable(litbook_feed_recovery feed_recovery.cpp)
target_link_libraries(litbook_feed_recovery PRIVATE litbook::litbook litbook::warnings)

if(LITBOOK_BUILD_TESTS)
    add_test(NAME FeedRecoverySmokeTest COMMAND litbook_feed_recovery 8 --runs 3)
    add_test(NAME FeedRecoverySpinSmokeTest COMMAND litbook_feed_recovery 4 --runs 2 --spin
                                                    --spacing-us 200)
    set_tests_properties(FeedRecoverySmokeTest FeedRecoverySpinSmokeTest PROPERTIES
        TIMEOUT 300
        PASS_REGULAR_EXPRESSION "time to in step \\(ms\\): median [0-9.]+")
    add_test(NAME FeedRecoveryRejectsBadArguments COMMAND litbook_feed_recovery 0)
    set_tests_properties(FeedRecoveryRejectsBadArguments PROPERTIES WILL_FAIL TRUE)
endif()
