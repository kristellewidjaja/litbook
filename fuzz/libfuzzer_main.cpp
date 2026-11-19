// libFuzzer entry point (built only with the `fuzz` preset).

#include <cstddef>
#include <cstdint>

#include "book_fuzzer.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return litbook::fuzz::run_one(data, size);
}
