# Sanitizer support. Applied globally (including GoogleTest and Google Benchmark),
# because ThreadSanitizer in particular reports false positives and misses races
# when only part of the program is instrumented.
#
#   -DLITBOOK_SANITIZER=address,undefined   ASan + UBSan
#   -DLITBOOK_SANITIZER=thread              TSan (cannot be combined with ASan)

set(LITBOOK_SANITIZER "" CACHE STRING "Sanitizers to enable: '', 'address,undefined', or 'thread'")

if(LITBOOK_SANITIZER)
    if(LITBOOK_SANITIZER MATCHES "thread" AND LITBOOK_SANITIZER MATCHES "address")
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
    endif()

    message(STATUS "litbook: sanitizers enabled: ${LITBOOK_SANITIZER}")
    add_compile_options(
        -fsanitize=${LITBOOK_SANITIZER}
        -fno-omit-frame-pointer        # readable stack traces
        -fno-sanitize-recover=all)     # any UBSan finding fails the test instead of just printing
    add_link_options(-fsanitize=${LITBOOK_SANITIZER})
endif()
