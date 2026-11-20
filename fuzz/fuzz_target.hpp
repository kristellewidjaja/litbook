#pragma once

#include <cstddef>
#include <cstdint>

namespace litbook::fuzz {

// Runs one fuzz input and aborts if a check fails. Returns 0, as libFuzzer expects.
// Each fuzz target (book_fuzzer.cpp, protocol_fuzzer.cpp) defines this function in its own
// library, and the shared libFuzzer entry point and corpus replay tool are linked against
// exactly one of them.
int run_one(const std::uint8_t* data, std::size_t size);

}  // namespace litbook::fuzz
