add_executable(litbook_bench
    hello_bench.cpp)

target_link_libraries(litbook_bench PRIVATE
    litbook::litbook
    litbook::warnings
    benchmark::benchmark_main)
