#pragma once

#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace litbook::protocol {

// The configuration of the order entry layer (order_entry.hpp), in a header of its own so
// that code which only stores or checks it, like the journal, does not need the whole
// order entry layer and every message type.

// Pre-trade risk limits for one account. An Enter Order or Replace Order that breaks one
// is refused before it reaches the book. The defaults are so large that they never apply.
struct RiskLimits {
    // The largest Quantity field an Enter Order or Replace Order may carry. For a replace
    // this is the order's new total size, counting shares that already executed.
    Qty max_order_qty = std::numeric_limits<Qty>::max();
    // The most orders the account may have live at the same time.
    std::uint32_t max_open_orders = std::numeric_limits<std::uint32_t>::max();
    bool operator==(const RiskLimits&) const = default;
};

struct OrderEntryConfig {
    // Accounts are numbered 1 to max_owners. Owner 0 is kNoOwner, which the books exempt
    // from self-trade prevention, so it is never a real account.
    OwnerId max_owners = 16;
    // The most orders that can be live at once across all accounts and stocks.
    std::uint32_t max_live_orders = 1U << 20U;
    // The most stocks the engine may list.
    std::size_t max_symbols = 1024;
};

}  // namespace litbook::protocol
