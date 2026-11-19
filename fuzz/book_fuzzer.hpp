#pragma once

#include <cstddef>
#include <cstdint>

namespace litbook::fuzz {

// Runs one fuzz input through every book and aborts on any disagreement (see
// book_fuzzer.cpp for the input encoding). Returns 0, as libFuzzer expects.
// Shared by the libFuzzer entry point and the corpus replay test.
int run_one(const std::uint8_t* data, std::size_t size);

}  // namespace litbook::fuzz
