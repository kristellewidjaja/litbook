// This is the libFuzzer entry point. It is built only with the `fuzz` preset.

#include <cstddef>
#include <cstdint>

#include "fuzz_target.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return litbook::fuzz::run_one(data, size);
}
