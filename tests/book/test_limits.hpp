#pragma once

// These test limits are shared by tests, fuzz targets and benchmarks. This file has no
// GoogleTest dependency.

#include "litbook/book/book_config.hpp"
#include "litbook/types.hpp"

#include <cstdint>
#include <limits>

namespace litbook::testing {

// FastOrderBook needs a finite price band and capacity. Shared tests leave them at
// "unbounded" and get this band of +-10,000 ticks and 1,024 orders instead, applied to
// every implementation so all of them run under the same rules.
inline constexpr Price kTestBandTicks = 10'000;
inline constexpr std::uint32_t kTestMaxOrders = 1024;

inline BookConfig with_test_limits(BookConfig config) {
    if (config.min_price == std::numeric_limits<Price>::min()) {
        config.min_price = -kTestBandTicks * config.tick_size;
    }
    if (config.max_price == std::numeric_limits<Price>::max()) {
        config.max_price = kTestBandTicks * config.tick_size;
    }
    if (config.max_orders == std::numeric_limits<std::uint32_t>::max()) {
        config.max_orders = kTestMaxOrders;
    }
    return config;
}

}  // namespace litbook::testing
