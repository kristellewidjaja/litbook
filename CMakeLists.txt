cmake_minimum_required(VERSION 3.25)

project(litbook
    VERSION 0.0.1
    DESCRIPTION "Low-latency exchange simulator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # use -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # used by clangd and clang-tidy

option(LITBOOK_BUILD_TESTS "Build unit tests" ON)
option(LITBOOK_BUILD_BENCH "Build benchmarks" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)    # must come before any target so everything is instrumented
include(Fuzzing)
include(Warnings)

# The Python module (python/) loads into an uninstrumented python3 process, which cannot
# host ThreadSanitizer at all and needs AddressSanitizer preloaded (DECISIONS.md D-071).
# So it is built only without sanitizers. The C++ classes it wraps are in the library and
# their GoogleTest tests run in every preset.
if(LITBOOK_SANITIZER OR LITBOOK_FUZZ)
    set(litbook_python_default OFF)
else()
    set(litbook_python_default ON)
endif()
option(LITBOOK_BUILD_PYTHON "Build the pybind11 Python module" ${litbook_python_default})
if(LITBOOK_BUILD_PYTHON AND (LITBOOK_SANITIZER OR LITBOOK_FUZZ))
    message(FATAL_ERROR "LITBOOK_BUILD_PYTHON cannot be combined with sanitizers or fuzzing")
endif()

include(Dependencies)

# Core library. The source list is kept in a variable because the Python module needs a
# second copy of the library built as position independent code (see below).
set(LITBOOK_SOURCES
    src/version.cpp
    src/book/book_config.cpp
    src/book/events.cpp
    src/book/fast_order_book.cpp
    src/book/map_order_book.cpp
    src/book/order_book.cpp
    src/book/sorted_order_book.cpp
    src/client/feed_book.cpp
    src/client/order_client.cpp
    src/client/order_tracker.cpp
    src/concurrency/thread_affinity.cpp
    src/engine/engine.cpp
    src/feed/feed_codec.cpp
    src/feed/feed_handler.cpp
    src/feed/feed_publisher.cpp
    src/feed/feed_receiver.cpp
    src/feed/mold.cpp
    src/feed/multicast.cpp
    src/gateway/session.cpp
    src/gateway/socket.cpp
    src/gateway/soup.cpp
    src/itch/binary_file_reader.cpp
    src/journal/crc32c.cpp
    src/journal/directory_replay.cpp
    src/journal/journal_directory.cpp
    src/journal/journal_format.cpp
    src/journal/journal_reader.cpp
    src/journal/journal_writer.cpp
    src/journal/replay.cpp
    src/journal/setup.cpp
    src/journal/snapshot_format.cpp
    src/journal/snapshot_store.cpp
    src/loadgen/client_session.cpp
    src/loadgen/load_report.cpp
    src/loadgen/load_run.cpp
    src/loadgen/order_flow.cpp
    src/pipeline/doorbell.cpp
    src/protocol/codec.cpp)
add_library(litbook STATIC ${LITBOOK_SOURCES})
add_library(litbook::litbook ALIAS litbook)
target_include_directories(litbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The pipeline (Phase 7) starts threads, so everything that uses the library links the
# system thread library.
find_package(Threads REQUIRED)
target_link_libraries(litbook PUBLIC Threads::Threads PRIVATE litbook::warnings)

# A shared library such as the Python module can only link code compiled with -fPIC. The
# main library keeps its usual code generation, so every recorded benchmark still measures
# the same code, and the module gets this copy instead.
if(LITBOOK_BUILD_PYTHON)
    add_library(litbook_pic STATIC ${LITBOOK_SOURCES})
    set_target_properties(litbook_pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(litbook_pic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(litbook_pic PUBLIC Threads::Threads PRIVATE litbook::warnings)
endif()

# Tests and benchmarks
if(LITBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LITBOOK_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(LITBOOK_BUILD_TESTS OR LITBOOK_FUZZ)
    add_subdirectory(fuzz)
endif()

add_subdirectory(tools)

if(LITBOOK_BUILD_PYTHON)
    add_subdirectory(python)
endif()

# These are optional local practice exercises, not part of the repository. Their tests
# carry the label "exercise" and are excluded from the normal test presets.
if(LITBOOK_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/exercises/CMakeLists.txt)
    add_subdirectory(exercises)
endif()
