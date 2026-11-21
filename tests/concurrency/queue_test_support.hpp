#pragma once

#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace litbook::concurrency::testing {

// A 64 byte message, the size the Phase 7b pipeline plans to send. Every field is
// derived from the sequence number, so a consumer can tell if it ever read a message
// that was only half written (a torn read), not just whether it arrived in order.
struct TestMessage {
    std::uint64_t sequence = 0;
    std::uint64_t check = 0;
    std::array<std::uint64_t, 6> fill{};
};
static_assert(sizeof(TestMessage) == 64);

inline TestMessage make_message(std::uint64_t sequence) {
    TestMessage m;
    m.sequence = sequence;
    m.check = sequence * 0x9E3779B97F4A7C15ULL;
    for (std::size_t i = 0; i < m.fill.size(); ++i) {
        m.fill[i] = sequence + i;
    }
    return m;
}

// True if every field matches what make_message would build for m.sequence.
inline bool is_intact(const TestMessage& m) {
    if (m.check != m.sequence * 0x9E3779B97F4A7C15ULL) {
        return false;
    }
    for (std::size_t i = 0; i < m.fill.size(); ++i) {
        if (m.fill[i] != m.sequence + i) {
            return false;
        }
    }
    return true;
}

}  // namespace litbook::concurrency::testing
