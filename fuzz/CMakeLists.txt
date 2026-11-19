# The differential fuzz target, shared by two executables:
#   book_fuzzer          libFuzzer build (`fuzz` preset, Clang only)
#   litbook_fuzz_replay  replays the saved inputs in seeds/ and regressions/ as a normal
#                        test, in every other preset
add_library(litbook_fuzz_target STATIC book_fuzzer.cpp)
target_include_directories(litbook_fuzz_target
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/tests)  # the naive oracle
target_link_libraries(litbook_fuzz_target PUBLIC litbook::litbook PRIVATE litbook::warnings)

if(LITBOOK_FUZZ)
    add_executable(book_fuzzer libfuzzer_main.cpp)
    target_link_libraries(book_fuzzer PRIVATE litbook_fuzz_target litbook::warnings)
    target_link_options(book_fuzzer PRIVATE -fsanitize=fuzzer)
endif()

if(LITBOOK_BUILD_TESTS)
    add_executable(litbook_fuzz_replay replay_main.cpp)
    target_link_libraries(litbook_fuzz_replay PRIVATE litbook_fuzz_target litbook::warnings)
    add_test(NAME FuzzCorpusReplay
        COMMAND litbook_fuzz_replay
            ${CMAKE_CURRENT_SOURCE_DIR}/seeds
            ${CMAKE_CURRENT_SOURCE_DIR}/regressions)
    set_tests_properties(FuzzCorpusReplay PROPERTIES TIMEOUT 300)
endif()
