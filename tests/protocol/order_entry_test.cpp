#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "book/book_test_support.hpp"
#include "book/test_limits.hpp"
#include "support/unwrap.hpp"

namespace litbook::protocol {
namespace {

using litbook::testing::BookTypeNames;
using litbook::testing::BookTypes;

// One outbound message and the account it was sent to.
struct Sent {
    OwnerId owner;
    OutboundMessage message;
    bool operator==(const Sent&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Sent& s) {
    return os << "to " << s.owner << " type " << s.message.index();
}

// Decodes every frame it receives, so each test also checks that the order entry layer
// only ever sends frames that decode.
class RecordingSink final : public OutboundSink {
public:
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        const auto decoded = decode_outbound(frame);
        ASSERT_EQ(decoded.status, DecodeStatus::Ok);
        sent_.push_back(Sent{.owner = owner, .message = litbook::testing::unwrap(decoded.message)});
    }
    std::vector<Sent> take() { return std::exchange(sent_, {}); }

private:
    std::vector<Sent> sent_;
};

constexpr Price kTick = 100;  // $0.01 with 4 implied decimals.
constexpr Timestamp kNow = 1'000;

Ticker sym(const char* name) {
    return litbook::testing::unwrap(make_ticker(name));
}

EnterOrder enter(UserRefNum ref, Side side, Qty qty, Price price, const char* symbol = "AAPL") {
    return EnterOrder{.user_ref = ref,
                      .side = side,
                      .qty = qty,
                      .symbol = sym(symbol),
                      .price = price,
                      .tif = TimeInForce::GoodTillCancel,
                      .type = OrderType::Limit,
                      .post_only = false};
}

Sent accepted(OwnerId owner, const EnterOrder& e, OrderRef ref, Timestamp t = kNow) {
    return {.owner = owner,
            .message = AcceptedMessage{.timestamp = t,
                                       .user_ref = e.user_ref,
                                       .side = e.side,
                                       .qty = e.qty,
                                       .symbol = e.symbol,
                                       .price = e.type == OrderType::Market ? 0 : e.price,
                                       .tif = e.tif,
                                       .type = e.type,
                                       .post_only = e.post_only,
                                       .order_ref = ref}};
}

Sent rejected(OwnerId owner, UserRefNum ref, WireRejectReason reason) {
    return {.owner = owner,
            .message = RejectedMessage{.timestamp = kNow, .user_ref = ref, .reason = reason}};
}

Sent executed(OwnerId owner, UserRefNum ref, Qty qty, Price price, Liquidity liq, MatchId match) {
    return {.owner = owner,
            .message = ExecutedMessage{.timestamp = kNow,
                                       .user_ref = ref,
                                       .qty = qty,
                                       .price = price,
                                       .liquidity = liq,
                                       .match = match}};
}

Sent canceled(OwnerId owner, UserRefNum ref, Qty qty, WireCancelReason reason) {
    return {.owner = owner,
            .message =
                CanceledMessage{.timestamp = kNow, .user_ref = ref, .qty = qty, .reason = reason}};
}

Sent replaced(OwnerId owner, UserRefNum orig, UserRefNum ref, Side side, Qty qty, Price price,
              OrderRef order_ref, bool kept, const char* symbol = "AAPL") {
    return {.owner = owner,
            .message = ReplacedMessage{.timestamp = kNow,
                                       .orig_user_ref = orig,
                                       .user_ref = ref,
                                       .side = side,
                                       .qty = qty,
                                       .symbol = sym(symbol),
                                       .price = price,
                                       .order_ref = order_ref,
                                       .kept_priority = kept}};
}

BookConfig stock_config(Qty lot = 1) {
    return litbook::testing::with_test_limits(BookConfig{.tick_size = kTick, .lot_size = lot});
}

template <class Book>
class OrderEntryTest : public ::testing::Test {
protected:
    explicit OrderEntryTest(OrderEntryConfig config = {.max_owners = 4,
                                                       .max_live_orders = 4096,
                                                       .max_symbols = 4})
        : entry_(sink_, config) {
        entry_.add_symbol(sym("AAPL"), stock_config());
        entry_.add_symbol(sym("MSFT"), stock_config(100));
    }

    void in(OwnerId owner, const InboundMessage& m, Timestamp t = kNow) {
        entry_.handle(owner, m, t);
    }

    // Every live order has exactly one record, so the books and the order entry layer
    // agree on how many orders rest.
    void expect_consistent(bool full_check = true) {
        std::size_t resting = 0;
        for (std::size_t s = 0; s < entry_.engine().symbol_count(); ++s) {
            resting += entry_.engine().book(static_cast<SymbolId>(s))->order_count();
        }
        EXPECT_EQ(entry_.live_orders(), resting);
        if (full_check) {
            EXPECT_TRUE(entry_.engine().check_invariants());
        }
    }

    RecordingSink sink_;
    OrderEntry<Book> entry_;
};

TYPED_TEST_SUITE(OrderEntryTest, BookTypes, BookTypeNames);

TYPED_TEST(OrderEntryTest, EnterOrderIsAccepted) {
    const EnterOrder e = enter(1, Side::Buy, 300, 500'000);
    this->in(1, e, 77);
    EXPECT_EQ(this->sink_.take(), (std::vector<Sent>{accepted(1, e, 1, 77)}));
    EXPECT_EQ(this->entry_.live_orders(), 1U);
    EXPECT_EQ(this->entry_.engine().book(0)->best_bid(), Price{500'000});
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, OrderRefsAreUniqueAcrossAccountsAndStocks) {
    const EnterOrder a = enter(1, Side::Buy, 10, 500'000, "AAPL");
    const EnterOrder m = enter(1, Side::Buy, 100, 500'000, "MSFT");
    const EnterOrder a2 = enter(2, Side::Sell, 10, 600'000, "AAPL");
    this->in(1, a);
    this->in(2, m);
    this->in(1, a2);
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{accepted(1, a, 1), accepted(2, m, 2), accepted(1, a2, 3)}));
}

TYPED_TEST(OrderEntryTest, UnknownSymbolIsRejectedAndUsesUpTheUserRefNum) {
    this->in(1, enter(5, Side::Buy, 10, 500'000, "IBM"));
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{rejected(1, 5, WireRejectReason::InvalidSymbol)}));
    this->in(1, enter(5, Side::Buy, 10, 500'000, "AAPL"));
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().stale_user_ref, 1U);
    EXPECT_EQ(this->entry_.live_orders(), 0U);
}

TYPED_TEST(OrderEntryTest, UserRefNumMustIncreasePerAccount) {
    this->in(1, enter(10, Side::Buy, 10, 500'000));
    this->sink_.take();
    for (const UserRefNum stale : {10U, 9U, 0U}) {
        this->in(1, enter(stale, Side::Buy, 10, 500'000));
    }
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().stale_user_ref, 3U);
    EXPECT_EQ(this->entry_.last_user_ref(1), 10U);
    // Another account has its own sequence and may start at 1.
    const EnterOrder other = enter(1, Side::Buy, 10, 500'000);
    this->in(2, other);
    EXPECT_EQ(this->sink_.take(), (std::vector<Sent>{accepted(2, other, 2)}));
    // Gaps are fine.
    this->in(1, enter(1000, Side::Buy, 10, 500'000));
    EXPECT_EQ(this->sink_.take().size(), 1U);
}

TYPED_TEST(OrderEntryTest, ZeroUserRefNumIsIgnored) {
    this->in(1, enter(0, Side::Buy, 10, 500'000));
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().stale_user_ref, 1U);
}

TYPED_TEST(OrderEntryTest, BookRuleViolationsAreRejectedWithWireCodes) {
    this->in(1, enter(1, Side::Buy, 0, 500'000));
    this->in(1, enter(2, Side::Buy, 10, 500'050));           // Off the $0.01 tick.
    this->in(1, enter(3, Side::Buy, 150, 500'000, "MSFT"));  // Odd lot.
    EnterOrder priced_market = enter(4, Side::Buy, 10, 500'000);
    priced_market.type = OrderType::Market;
    this->in(1, priced_market);
    EnterOrder post_only_market = enter(5, Side::Buy, 10, 0);
    post_only_market.type = OrderType::Market;
    post_only_market.post_only = true;
    this->in(1, post_only_market);
    this->in(1, enter(6, Side::Buy, 10, 1'999'999'900));  // Above the test band.
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{rejected(1, 1, WireRejectReason::InvalidQuantity),
                                 rejected(1, 2, WireRejectReason::InvalidPrice),
                                 rejected(1, 3, WireRejectReason::InvalidQuantity),
                                 rejected(1, 4, WireRejectReason::InvalidPrice),
                                 rejected(1, 5, WireRejectReason::InvalidOrderType),
                                 rejected(1, 6, WireRejectReason::InvalidPrice)}));
    EXPECT_EQ(this->entry_.live_orders(), 0U);
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, PostOnlyThatWouldCrossIsRejected) {
    this->in(1, enter(1, Side::Sell, 10, 500'000));
    EnterOrder post = enter(1, Side::Buy, 10, 500'000);
    post.post_only = true;
    this->in(2, post);
    const auto sent = this->sink_.take();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[1], rejected(2, 1, WireRejectReason::PostOnlyWouldCross));
}

TYPED_TEST(OrderEntryTest, TradeReportsBothSidesWithOneMatchNumber) {
    const EnterOrder sell = enter(1, Side::Sell, 100, 500'000);
    const EnterOrder buy = enter(7, Side::Buy, 100, 510'000);
    this->in(1, sell);
    this->in(2, buy);
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{accepted(1, sell, 1), accepted(2, buy, 2),
                                 executed(2, 7, 100, 500'000, Liquidity::Removed, 1),
                                 executed(1, 1, 100, 500'000, Liquidity::Added, 1)}));
    EXPECT_EQ(this->entry_.live_orders(), 0U);
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, ImmediateOrCancelRemainderIsCanceled) {
    this->in(1, enter(1, Side::Sell, 30, 500'000));
    EnterOrder ioc = enter(1, Side::Buy, 100, 500'000);
    ioc.tif = TimeInForce::ImmediateOrCancel;
    this->in(2, ioc);
    EXPECT_EQ(
        this->sink_.take(),
        (std::vector<Sent>{accepted(1, enter(1, Side::Sell, 30, 500'000), 1), accepted(2, ioc, 2),
                           executed(2, 1, 30, 500'000, Liquidity::Removed, 1),
                           executed(1, 1, 30, 500'000, Liquidity::Added, 1),
                           canceled(2, 1, 70, WireCancelReason::ImmediateOrCancel)}));
    EXPECT_EQ(this->entry_.live_orders(), 0U);
}

TYPED_TEST(OrderEntryTest, MarketOrderTrades) {
    this->in(1, enter(1, Side::Sell, 30, 500'000));
    EnterOrder market = enter(1, Side::Buy, 30, 0);
    market.type = OrderType::Market;
    this->in(2, market);
    const auto sent = this->sink_.take();
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(sent[1], accepted(2, market, 2));
    EXPECT_EQ(sent[2], executed(2, 1, 30, 500'000, Liquidity::Removed, 1));
}

TYPED_TEST(OrderEntryTest, CancelToZeroCancelsEverythingOpen) {
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->sink_.take();
    this->in(1, CancelOrder{.user_ref = 1, .qty = 0});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{canceled(1, 1, 100, WireCancelReason::UserRequested)}));
    EXPECT_EQ(this->entry_.live_orders(), 0U);
    // The order is gone, so a second cancel is silently ignored.
    this->in(1, CancelOrder{.user_ref = 1, .qty = 0});
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().unknown_order, 1U);
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, CancelToAnIntendedSizeCountsExecutedShares) {
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->in(2, enter(1, Side::Sell, 30, 500'000));  // 30 execute, 70 stay open.
    this->in(1, enter(2, Side::Buy, 10, 500'000));   // Queued behind order 1.
    this->sink_.take();

    // At most 50 in total may execute, 30 already did, so 20 stay open and 50 come off.
    this->in(1, CancelOrder{.user_ref = 1, .qty = 50});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{canceled(1, 1, 50, WireCancelReason::UserRequested)}));
    EXPECT_EQ(this->entry_.engine().book(0)->volume_at(Side::Buy, 500'000), 30U);

    // Shrinking kept the order's place, so the next sell trades with it first.
    this->in(3, enter(1, Side::Sell, 5, 500'000));
    const auto sent = this->sink_.take();
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[2], executed(1, 1, 5, 500'000, Liquidity::Added, 2));

    // An intended size of 80 would not reduce the order (35 executed, 15 open).
    this->in(1, CancelOrder{.user_ref = 1, .qty = 80});
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().no_effect_cancel, 1U);

    // An intended size at or below what already executed cancels the rest.
    this->in(1, CancelOrder{.user_ref = 1, .qty = 35});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{canceled(1, 1, 15, WireCancelReason::UserRequested)}));
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, CancelToAnOddLotSizeIsIgnored) {
    this->in(1, enter(1, Side::Buy, 300, 500'000, "MSFT"));  // Lot size 100.
    this->sink_.take();
    this->in(1, CancelOrder{.user_ref = 1, .qty = 150});
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().no_effect_cancel, 1U);
    EXPECT_EQ(this->entry_.engine().book(1)->volume_at(Side::Buy, 500'000), 300U);
}

TYPED_TEST(OrderEntryTest, AccountsCannotTouchEachOthersOrders) {
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->sink_.take();
    this->in(2, CancelOrder{.user_ref = 1, .qty = 0});
    this->in(2, ReplaceOrder{.orig_user_ref = 1, .user_ref = 2, .qty = 10, .price = 500'000});
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().unknown_order, 2U);
    EXPECT_EQ(this->entry_.live_orders(), 1U);
}

TYPED_TEST(OrderEntryTest, ReplaceThatShrinksKeepsPriority) {
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->sink_.take();
    this->in(1, ReplaceOrder{.orig_user_ref = 1, .user_ref = 2, .qty = 60, .price = 500'000});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{replaced(1, 1, 2, Side::Buy, 60, 500'000, 1, true)}));
    // The order now answers only to its new UserRefNum.
    this->in(1, CancelOrder{.user_ref = 1, .qty = 0});
    EXPECT_EQ(this->entry_.stats().unknown_order, 1U);
    this->in(1, CancelOrder{.user_ref = 2, .qty = 0});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{canceled(1, 2, 60, WireCancelReason::UserRequested)}));
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, ReplaceThatCrossesTradesUnderTheNewUserRefNum) {
    this->in(2, enter(1, Side::Sell, 40, 510'000));
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->sink_.take();
    this->in(1, ReplaceOrder{.orig_user_ref = 1, .user_ref = 2, .qty = 100, .price = 510'000});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{replaced(1, 1, 2, Side::Buy, 100, 510'000, 2, false),
                                 executed(1, 2, 40, 510'000, Liquidity::Removed, 1),
                                 executed(2, 1, 40, 510'000, Liquidity::Added, 1)}));
    EXPECT_EQ(this->entry_.engine().book(0)->volume_at(Side::Buy, 510'000), 60U);
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, ReplaceQuantityCountsExecutedShares) {
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->in(2, enter(1, Side::Sell, 30, 500'000));
    this->sink_.take();
    // 30 already executed, so a total of 50 leaves 20 open.
    this->in(1, ReplaceOrder{.orig_user_ref = 1, .user_ref = 2, .qty = 50, .price = 500'000});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{replaced(1, 1, 2, Side::Buy, 20, 500'000, 1, true)}));
}

TYPED_TEST(OrderEntryTest, InvalidReplaceIsRejectedAndCancelsTheOriginal) {
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->sink_.take();
    this->in(1, ReplaceOrder{.orig_user_ref = 1, .user_ref = 2, .qty = 100, .price = 500'050});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{rejected(1, 2, WireRejectReason::InvalidPrice),
                                 canceled(1, 1, 100, WireCancelReason::UserRequested)}));
    EXPECT_EQ(this->entry_.live_orders(), 0U);
    // The replacement UserRefNum was used up.
    EXPECT_EQ(this->entry_.last_user_ref(1), 2U);
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, ReplaceBelowExecutedSharesFails) {
    this->in(1, enter(1, Side::Buy, 100, 500'000));
    this->in(2, enter(1, Side::Sell, 30, 500'000));
    this->sink_.take();
    this->in(1, ReplaceOrder{.orig_user_ref = 1, .user_ref = 2, .qty = 30, .price = 500'000});
    EXPECT_EQ(this->sink_.take(),
              (std::vector<Sent>{rejected(1, 2, WireRejectReason::InvalidQuantity),
                                 canceled(1, 1, 70, WireCancelReason::UserRequested)}));
}

TYPED_TEST(OrderEntryTest, ReplaceWithStaleUserRefNumIsIgnored) {
    this->in(1, enter(5, Side::Buy, 100, 500'000));
    this->sink_.take();
    this->in(1, ReplaceOrder{.orig_user_ref = 5, .user_ref = 5, .qty = 10, .price = 500'000});
    this->in(1, ReplaceOrder{.orig_user_ref = 9, .user_ref = 10, .qty = 10, .price = 500'000});
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().stale_user_ref, 1U);
    EXPECT_EQ(this->entry_.stats().unknown_order, 1U);
    EXPECT_EQ(this->entry_.engine().book(0)->volume_at(Side::Buy, 500'000), 100U);
    // An ignored replace for an unknown order does not use up its UserRefNum.
    EXPECT_EQ(this->entry_.last_user_ref(1), 5U);
}

TYPED_TEST(OrderEntryTest, SelfTradePreventionIsReported) {
    this->in(1, enter(1, Side::Sell, 10, 500'000));
    this->in(1, enter(2, Side::Buy, 10, 500'000));
    const auto sent = this->sink_.take();
    ASSERT_EQ(sent.size(), 3U);
    // The default policy cancels the newest order.
    EXPECT_EQ(sent[2], canceled(1, 2, 10, WireCancelReason::SelfMatchPrevention));
    EXPECT_EQ(this->entry_.live_orders(), 1U);
    this->expect_consistent();
}

TYPED_TEST(OrderEntryTest, BadOwnerIsCounted) {
    this->in(kNoOwner, enter(1, Side::Buy, 10, 500'000));
    this->in(5, enter(1, Side::Buy, 10, 500'000));
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().bad_owner, 2U);
}

TYPED_TEST(OrderEntryTest, MalformedFrameIsCountedAndChangesNothing) {
    const std::array<std::byte, 3> garbage{std::byte{'O'}, std::byte{0}, std::byte{1}};
    EXPECT_EQ(this->entry_.on_frame(1, garbage, kNow), DecodeStatus::TooShort);
    EXPECT_EQ(this->entry_.on_frame(1, {}, kNow), DecodeStatus::Empty);
    EXPECT_TRUE(this->sink_.take().empty());
    EXPECT_EQ(this->entry_.stats().malformed, 2U);
    EXPECT_EQ(this->entry_.last_user_ref(1), 0U);
}

TYPED_TEST(OrderEntryTest, FramesAreDecodedAndHandled) {
    std::array<std::byte, kMaxMessageLength> buffer{};
    const EnterOrder e = enter(1, Side::Sell, 10, 500'000);
    const std::size_t n = encode(e, buffer);
    EXPECT_EQ(this->entry_.on_frame(3, std::span(buffer.data(), n), 42), DecodeStatus::Ok);
    EXPECT_EQ(this->sink_.take(), (std::vector<Sent>{accepted(3, e, 1, 42)}));
}

TYPED_TEST(OrderEntryTest, BookFullCancelIsASystemCancel) {
    RecordingSink sink;
    OrderEntry<TypeParam> entry(sink, {.max_owners = 2, .max_live_orders = 8, .max_symbols = 1});
    BookConfig tiny = stock_config();
    tiny.max_orders = 1;
    entry.add_symbol(sym("AAPL"), tiny);
    entry.handle(1, enter(1, Side::Buy, 10, 500'000), kNow);
    entry.handle(1, enter(2, Side::Buy, 10, 490'000), kNow);
    const auto sent = sink.take();
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[2], canceled(1, 2, 10, WireCancelReason::System));
    EXPECT_EQ(entry.live_orders(), 1U);
}

TYPED_TEST(OrderEntryTest, NoRoomForAnotherLiveOrderIsRejected) {
    RecordingSink sink;
    OrderEntry<TypeParam> entry(sink, {.max_owners = 2, .max_live_orders = 1, .max_symbols = 1});
    entry.add_symbol(sym("AAPL"), stock_config());
    entry.handle(1, enter(1, Side::Buy, 10, 500'000), kNow);
    entry.handle(1, enter(2, Side::Buy, 10, 490'000), kNow);
    const auto sent = sink.take();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[1], rejected(1, 2, WireRejectReason::Other));
    // Once the first order is gone there is room again.
    entry.handle(1, CancelOrder{.user_ref = 1, .qty = 0}, kNow);
    entry.handle(1, enter(3, Side::Buy, 10, 490'000), kNow);
    EXPECT_EQ(sink.take().size(), 2U);
}

TYPED_TEST(OrderEntryTest, ConstructorRejectsBadConfig) {
    RecordingSink sink;
    using Entry = OrderEntry<TypeParam>;
    EXPECT_THROW(Entry(sink, {.max_owners = 0}), std::invalid_argument);
    EXPECT_THROW(Entry(sink, {.max_live_orders = 0}), std::invalid_argument);
    EXPECT_THROW(Entry(sink, {.max_symbols = 0}), std::invalid_argument);
}

// Makes random but reproducible inbound messages from four accounts across two stocks.
// Each choice has its own small function so the mix is easy to read and to change.
class RandomClients {
public:
    explicit RandomClients(std::uint64_t seed) : rng_(seed) {}

    // Returns the next account and the message it sends.
    std::pair<OwnerId, InboundMessage> next() {
        const auto owner = static_cast<OwnerId>(1 + pick(4));
        const std::uint32_t kind = pick(10);
        if (kind < 5) {
            return {owner, new_order(owner)};
        }
        if (kind < 8) {
            return {owner, cancel(owner)};
        }
        return {owner, replace(owner)};
    }

private:
    // A random number from 0 to n - 1.
    std::uint32_t pick(std::uint32_t n) { return static_cast<std::uint32_t>(rng_() % n); }

    bool one_in(std::uint32_t n) { return pick(n) == 0; }

    // A fresh UserRefNum for this account.
    UserRefNum fresh_ref(OwnerId owner) { return ++last_ref_.at(owner); }

    // One of the account's last few UserRefNums (or the newest), so cancels and replaces
    // often name an order that is still live, and sometimes one that is gone.
    UserRefNum recent_ref(OwnerId owner) {
        const UserRefNum last = last_ref_.at(owner);
        return last - pick(std::min<UserRefNum>(last, 8) + 1);
    }

    // Buys sit below $50.00 and sells above it, so a book builds up. One order in ten is
    // priced on the other side of $50.00 so that it crosses and trades, and one price in
    // fifty is off the $0.01 tick so the book rejects it.
    Price price_for(Side side) {
        const bool cross = one_in(10);
        const auto distance = static_cast<Price>(pick(20)) + 1;
        const bool below = (side == Side::Buy) != cross;
        const Price ticks = below ? 5'000 - distance : 5'000 + distance;
        const Price off_tick = one_in(50) ? 7 : 0;
        return ticks * kTick + off_tick;
    }

    // MSFT trades in round lots of 100. AAPL has lot size 1, and the quantity is mostly
    // a multiple of 10 with a small remainder now and then. Zero happens too.
    Qty quantity_for(const char* stock) {
        const Qty lots = pick(8);
        if (std::string_view(stock) == "MSFT") {
            return lots * 100;
        }
        return lots * 10 + pick(3);
    }

    const char* stock() { return one_in(2) ? "AAPL" : "MSFT"; }

    InboundMessage new_order(OwnerId owner) {
        const Side side = one_in(2) ? Side::Buy : Side::Sell;
        const char* name = stock();
        // One order in thirty reuses an old UserRefNum and must be ignored, and one in
        // forty names a stock that is not listed.
        const UserRefNum ref = one_in(30) ? recent_ref(owner) : fresh_ref(owner);
        EnterOrder e =
            enter(ref, side, quantity_for(name), price_for(side), one_in(40) ? "IBM" : name);
        if (one_in(5)) {
            e.tif = TimeInForce::ImmediateOrCancel;
        }
        return e;
    }

    InboundMessage cancel(OwnerId owner) {
        // A third of cancels remove everything, the rest ask for a random intended size.
        const Qty intended = one_in(3) ? 0 : quantity_for(stock());
        return CancelOrder{.user_ref = recent_ref(owner), .qty = intended};
    }

    InboundMessage replace(OwnerId owner) {
        const UserRefNum orig = recent_ref(owner);
        // One replace in twenty reuses the old number and must be ignored.
        const UserRefNum ref = one_in(20) ? orig : fresh_ref(owner);
        const Side side = one_in(2) ? Side::Buy : Side::Sell;
        return ReplaceOrder{.orig_user_ref = orig,
                            .user_ref = ref,
                            .qty = quantity_for(stock()),
                            .price = price_for(side)};
    }

    std::mt19937_64 rng_;
    std::array<UserRefNum, 5> last_ref_{};  // Indexed by owner. Slot 0 is unused.
};

// Sends a long random stream of valid, invalid and stale messages. After every message
// the order entry layer and the books must agree, and every outbound frame must decode
// (RecordingSink checks that). The counts at the end prove the stream really exercised
// trades, replaces and the silent-ignore rules.
TYPED_TEST(OrderEntryTest, RandomStreamKeepsRecordsAndBooksInStep) {
    RandomClients clients(20261119);
    std::size_t executions = 0;
    std::size_t replaces = 0;
    for (int step = 0; step < 20'000; ++step) {
        const auto [owner, message] = clients.next();
        this->in(owner, message);
        for (const Sent& s : this->sink_.take()) {
            if (std::holds_alternative<ExecutedMessage>(s.message)) {
                ++executions;
            }
            if (std::holds_alternative<ReplacedMessage>(s.message)) {
                ++replaces;
            }
        }
        // The book invariant check scans the whole ladder, so it runs every 100 steps.
        this->expect_consistent(step % 100 == 0);
        ASSERT_FALSE(::testing::Test::HasFailure()) << "step " << step;
    }
    EXPECT_GT(executions, 100U);
    EXPECT_GT(replaces, 100U);
    EXPECT_GT(this->entry_.stats().stale_user_ref, 0U);
    EXPECT_GT(this->entry_.stats().unknown_order, 0U);
}

}  // namespace
}  // namespace litbook::protocol
