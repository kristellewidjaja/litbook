# There are two fuzz targets. Each one is a small library that defines
# litbook::fuzz::run_one (see fuzz_target.hpp).
#   book_fuzzer      differential test of all three books against the naive oracle
#   protocol_fuzzer  decoder robustness and canonical re-encoding of the wire codec
# With the `fuzz` preset (Clang only) each target gets a libFuzzer executable. In every
# other preset a replay executable runs the saved inputs in its seeds and regressions
# folders as a normal test.
add_library(litbook_fuzz_target STATIC book_fuzzer.cpp)
target_include_directories(litbook_fuzz_target
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/tests)  # for the naive oracle
target_link_libraries(litbook_fuzz_target PUBLIC litbook::litbook PRIVATE litbook::warnings)

add_library(litbook_protocol_fuzz_target STATIC protocol_fuzzer.cpp)
target_include_directories(litbook_protocol_fuzz_target PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(litbook_protocol_fuzz_target
    PUBLIC litbook::litbook PRIVATE litbook::warnings)

if(LITBOOK_FUZZ)
    add_executable(book_fuzzer libfuzzer_main.cpp)
    target_link_libraries(book_fuzzer PRIVATE litbook_fuzz_target litbook::warnings)
    target_link_options(book_fuzzer PRIVATE -fsanitize=fuzzer)

    add_executable(protocol_fuzzer libfuzzer_main.cpp)
    target_link_libraries(protocol_fuzzer PRIVATE litbook_protocol_fuzz_target litbook::warnings)
    target_link_options(protocol_fuzzer PRIVATE -fsanitize=fuzzer)
endif()

if(LITBOOK_BUILD_TESTS)
    add_executable(litbook_fuzz_replay replay_main.cpp)
    target_link_libraries(litbook_fuzz_replay PRIVATE litbook_fuzz_target litbook::warnings)
    add_test(NAME FuzzCorpusReplay
        COMMAND litbook_fuzz_replay
            ${CMAKE_CURRENT_SOURCE_DIR}/seeds
            ${CMAKE_CURRENT_SOURCE_DIR}/regressions)
    set_tests_properties(FuzzCorpusReplay PROPERTIES TIMEOUT 300)

    add_executable(litbook_protocol_fuzz_replay replay_main.cpp)
    target_link_libraries(litbook_protocol_fuzz_replay
        PRIVATE litbook_protocol_fuzz_target litbook::warnings)
    add_test(NAME ProtocolFuzzCorpusReplay
        COMMAND litbook_protocol_fuzz_replay
            ${CMAKE_CURRENT_SOURCE_DIR}/protocol_seeds
            ${CMAKE_CURRENT_SOURCE_DIR}/protocol_regressions)
    set_tests_properties(ProtocolFuzzCorpusReplay PROPERTIES TIMEOUT 300)
endif()
