#include "litbook/container/flat_id_map.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/protocol/order_entry_config.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace litbook::protocol {
namespace {

// Every pair of an account and a UserRefNum must get its own key, including the largest
// values both can take.
TEST(ClientMapKeyTest, EveryAccountAndUserRefNumGetsItsOwnKey) {
    constexpr UserRefNum kTop = std::numeric_limits<UserRefNum>::max();
    for (const OwnerId max_owners : {OwnerId{1}, OwnerId{4}, OwnerId{16}, OwnerId{1000}}) {
        std::set<FlatIdMap::Key> keys;
        std::size_t pairs = 0;
        for (OwnerId owner = 1; owner <= std::min<OwnerId>(max_owners, 20); ++owner) {
            for (const UserRefNum ref :
                 {UserRefNum{0}, UserRefNum{1}, UserRefNum{2}, UserRefNum{1000}, kTop - 1, kTop}) {
                keys.insert(client_map_key(owner, ref, max_owners));
                ++pairs;
            }
        }
        keys.insert(client_map_key(max_owners, kTop, max_owners));
        keys.insert(client_map_key(max_owners, kTop - 1, max_owners));
        EXPECT_GE(keys.size(), pairs) << "max_owners " << max_owners;
    }
}

TEST(ClientMapKeyTest, TheLargestAccountCountDoesNotOverflow) {
    constexpr OwnerId kMaxOwners = std::numeric_limits<OwnerId>::max();
    constexpr UserRefNum kTop = std::numeric_limits<UserRefNum>::max();
    EXPECT_NE(client_map_key(kMaxOwners, kTop, kMaxOwners),
              client_map_key(kMaxOwners - 1, kTop, kMaxOwners));
    EXPECT_NE(client_map_key(1, kTop, kMaxOwners), client_map_key(1, kTop - 1, kMaxOwners));
    EXPECT_NE(client_map_key(kMaxOwners, kTop - 1, kMaxOwners),
              client_map_key(1, kTop, kMaxOwners));
}

// Regression test. Each account's newest UserRefNums are its densest live orders, and the
// map's fold hash keeps nearby keys in nearby slots. With the account in the high 32 bits,
// four accounts' recent orders landed on almost the same slots and formed one run of
// about 50,000 slots at 100,000 live orders, so every insert, lookup and erase walked it
// (30 us per frame instead of about 0.2 us). Here every UserRefNum of four accounts is
// live, the worst case, in a map of the size the default configuration builds.
TEST(ClientMapKeyTest, DenseRecentOrdersOfSeveralAccountsDoNotPileUp) {
    const OrderEntryConfig config{};
    for (const OwnerId max_owners : {OwnerId{4}, config.max_owners}) {
        FlatIdMap map(config.max_live_orders);
        constexpr UserRefNum kPerAccount = 60'000;
        for (UserRefNum ref = 1; ref <= kPerAccount; ++ref) {
            for (OwnerId owner = 1; owner <= 4; ++owner) {
                ASSERT_TRUE(map.insert(client_map_key(owner, ref, max_owners), ref));
            }
        }
        EXPECT_LE(map.longest_probe(), static_cast<std::size_t>(max_owners))
            << "max_owners " << max_owners;
    }
}

// The same with each account's orders starting far apart, as after a long session where
// one account sent many more orders than the others.
TEST(ClientMapKeyTest, AccountsAtDifferentUserRefNumsDoNotPileUp) {
    const OrderEntryConfig config{};
    FlatIdMap map(config.max_live_orders);
    const std::vector<UserRefNum> starts{1, 3'000'000, 70'000'000, 4'000'000'000U};
    for (UserRefNum k = 0; k < 50'000; ++k) {
        for (OwnerId owner = 1; owner <= 4; ++owner) {
            ASSERT_TRUE(
                map.insert(client_map_key(owner, starts[owner - 1] + k, config.max_owners), k));
        }
    }
    EXPECT_LE(map.longest_probe(), std::size_t{64});
}

}  // namespace
}  // namespace litbook::protocol
