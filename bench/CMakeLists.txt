add_executable(litbook_bench
    hello_bench.cpp
    book_bench.cpp)

# The book benchmarks replay streams from the randomized-test workload generator.
target_include_directories(litbook_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests)

target_link_libraries(litbook_bench PRIVATE
    litbook::litbook
    litbook::warnings
    benchmark::benchmark_main)
