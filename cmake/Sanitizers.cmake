# This adds sanitizer support. It is applied globally (including GoogleTest and Google
# Benchmark), because ThreadSanitizer in particular reports false positives and misses
# races when only part of the program is instrumented.
#
# Pass -DLITBOOK_SANITIZER=address,undefined for ASan plus UBSan, or
# -DLITBOOK_SANITIZER=thread for TSan, which cannot be combined with ASan.

set(LITBOOK_SANITIZER "" CACHE STRING "Sanitizers to enable: '', 'address,undefined', or 'thread'")

if(LITBOOK_SANITIZER)
    if(LITBOOK_SANITIZER MATCHES "thread" AND LITBOOK_SANITIZER MATCHES "address")
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
    endif()

    message(STATUS "litbook: sanitizers enabled: ${LITBOOK_SANITIZER}")
    add_compile_options(
        -fsanitize=${LITBOOK_SANITIZER}
        -fno-omit-frame-pointer        # gives readable stack traces
        -fno-sanitize-recover=all)     # any UBSan finding fails the test, not just prints
    add_link_options(-fsanitize=${LITBOOK_SANITIZER})
endif()
