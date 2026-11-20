#include "litbook/engine/engine.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <variant>
#include <vector>

#include "book/book_test_support.hpp"
#include "book/test_limits.hpp"
#include "book/workload.hpp"
#include "engine/recording_engine_listener.hpp"
#include "support/unwrap.hpp"

namespace litbook::testing {
namespace {

Ticker tick(const char* name) {
    return unwrap(make_ticker(name));
}

// A small banded config that every book type supports.
BookConfig small_config(Price tick_size = 1) {
    return with_test_limits(BookConfig{.tick_size = tick_size});
}

OrderRequest limit(OrderId id, Side side, Price price, Qty qty) {
    return OrderRequest{.id = id, .side = side, .price = price, .qty = qty};
}

// Runs the same calls into a standalone book, to get the stream the engine should forward.
template <class Book>
class Standalone {
public:
    explicit Standalone(const BookConfig& config) : book_(listener_, config) {}
    Book& book() { return book_; }
    [[nodiscard]] const std::vector<BookEvent>& events() const { return listener_.events(); }

private:
    RecordingListener listener_;
    Book book_;
};

// Returns the events for one stock with their match numbers zeroed, so a stream from the
// engine can be compared with a stream from a standalone book, which numbers trades
// itself.
std::vector<BookEvent> events_for(const std::vector<EngineEvent>& all, SymbolId symbol) {
    std::vector<BookEvent> out;
    for (const EngineEvent& e : all) {
        if (e.symbol == symbol) {
            out.push_back(e.event);
        }
    }
    return out;
}

std::vector<BookEvent> without_match_ids(std::vector<BookEvent> events) {
    for (BookEvent& e : events) {
        if (auto* t = std::get_if<Trade>(&e)) {
            t->match_id = 0;
        }
    }
    return events;
}

std::vector<MatchId> match_ids(const std::vector<EngineEvent>& all) {
    std::vector<MatchId> out;
    for (const EngineEvent& e : all) {
        if (const auto* t = std::get_if<Trade>(&e.event)) {
            out.push_back(t->match_id);
        }
    }
    return out;
}

template <class Book>
class EngineTest : public ::testing::Test {
protected:
    RecordingEngineListener listener_;
};

TYPED_TEST_SUITE(EngineTest, BookTypes, BookTypeNames);

TYPED_TEST(EngineTest, AddSymbolHandsOutDenseIds) {
    Engine<TypeParam> engine(this->listener_, 4);
    EXPECT_EQ(engine.add_symbol(tick("AAPL"), small_config()), 0);
    EXPECT_EQ(engine.add_symbol(tick("MSFT"), small_config()), 1);
    EXPECT_EQ(engine.add_symbol(tick("AMZN"), small_config()), 2);
    EXPECT_EQ(engine.symbol_count(), 3U);
    EXPECT_EQ(engine.max_symbols(), 4U);
    EXPECT_EQ(engine.ticker(1), tick("MSFT"));
    EXPECT_TRUE(engine.check_invariants());
}

TYPED_TEST(EngineTest, FindLooksUpByTicker) {
    Engine<TypeParam> engine(this->listener_, 4);
    engine.add_symbol(tick("AAPL"), small_config());
    engine.add_symbol(tick("MSFT"), small_config());
    EXPECT_EQ(engine.find(tick("AAPL")), SymbolId{0});
    EXPECT_EQ(engine.find(tick("MSFT")), SymbolId{1});
    EXPECT_EQ(engine.find(tick("IBM")), std::nullopt);
    // A prefix of a known ticker is a different ticker.
    EXPECT_EQ(engine.find(tick("AAP")), std::nullopt);
}

TYPED_TEST(EngineTest, ConstructorRejectsBadCapacity) {
    EXPECT_THROW(Engine<TypeParam>(this->listener_, 0), std::invalid_argument);
    EXPECT_THROW(Engine<TypeParam>(this->listener_, 65'536), std::invalid_argument);
    EXPECT_NO_THROW(Engine<TypeParam>(this->listener_, 1));
}

TYPED_TEST(EngineTest, DuplicateTickerThrowsAndChangesNothing) {
    Engine<TypeParam> engine(this->listener_, 4);
    engine.add_symbol(tick("AAPL"), small_config());
    EXPECT_THROW(engine.add_symbol(tick("AAPL"), small_config()), std::invalid_argument);
    EXPECT_EQ(engine.symbol_count(), 1U);
}

TYPED_TEST(EngineTest, FullEngineThrows) {
    Engine<TypeParam> engine(this->listener_, 2);
    engine.add_symbol(tick("A"), small_config());
    engine.add_symbol(tick("B"), small_config());
    EXPECT_THROW(engine.add_symbol(tick("C"), small_config()), std::invalid_argument);
    EXPECT_EQ(engine.symbol_count(), 2U);
    EXPECT_EQ(engine.find(tick("C")), std::nullopt);
}

TYPED_TEST(EngineTest, InvalidConfigThrowsAndAddsNothing) {
    Engine<TypeParam> engine(this->listener_, 4);
    BookConfig bad = small_config();
    bad.tick_size = 0;
    EXPECT_THROW(engine.add_symbol(tick("AAPL"), bad), std::invalid_argument);
    EXPECT_EQ(engine.symbol_count(), 0U);
    EXPECT_EQ(engine.find(tick("AAPL")), std::nullopt);
    // The ticker is still free after the failed attempt.
    EXPECT_EQ(engine.add_symbol(tick("AAPL"), small_config()), 0);
}

TYPED_TEST(EngineTest, UnknownSymbolIdDoesNothing) {
    Engine<TypeParam> engine(this->listener_, 4);
    engine.add_symbol(tick("AAPL"), small_config());
    EXPECT_EQ(engine.submit(1, limit(1, Side::Buy, 100, 10)), EngineStatus::UnknownSymbol);
    EXPECT_EQ(engine.cancel(1, 1), EngineStatus::UnknownSymbol);
    EXPECT_EQ(engine.modify(7, 1, 100, 5), EngineStatus::UnknownSymbol);
    EXPECT_TRUE(this->listener_.events().empty());
    EXPECT_EQ(engine.book(1), nullptr);
    EXPECT_NE(engine.book(0), nullptr);
}

TYPED_TEST(EngineTest, StocksNeverTradeWithEachOther) {
    Engine<TypeParam> engine(this->listener_, 4);
    const SymbolId a = engine.add_symbol(tick("AAPL"), small_config());
    const SymbolId m = engine.add_symbol(tick("MSFT"), small_config());
    EXPECT_EQ(engine.submit(a, limit(1, Side::Buy, 100, 10)), EngineStatus::Ok);
    EXPECT_EQ(engine.submit(m, limit(2, Side::Sell, 100, 10)), EngineStatus::Ok);
    EXPECT_TRUE(match_ids(this->listener_.events()).empty());
    EXPECT_EQ(engine.book(a)->best_bid(), Price{100});
    EXPECT_EQ(engine.book(a)->best_ask(), std::nullopt);
    EXPECT_EQ(engine.book(m)->best_ask(), Price{100});
    EXPECT_EQ(engine.book(m)->best_bid(), std::nullopt);
}

TYPED_TEST(EngineTest, SameOrderIdMayRestInTwoStocks) {
    Engine<TypeParam> engine(this->listener_, 4);
    const SymbolId a = engine.add_symbol(tick("AAPL"), small_config());
    const SymbolId m = engine.add_symbol(tick("MSFT"), small_config());
    engine.submit(a, limit(7, Side::Buy, 100, 10));
    engine.submit(m, limit(7, Side::Buy, 100, 10));
    EXPECT_TRUE(engine.book(a)->contains(7));
    EXPECT_TRUE(engine.book(m)->contains(7));
    // Canceling in one stock leaves the other alone.
    engine.cancel(a, 7);
    EXPECT_FALSE(engine.book(a)->contains(7));
    EXPECT_TRUE(engine.book(m)->contains(7));
}

TYPED_TEST(EngineTest, ForwardsEachBookStreamTaggedWithItsSymbol) {
    Engine<TypeParam> engine(this->listener_, 4);
    const SymbolId a = engine.add_symbol(tick("AAPL"), small_config());
    const SymbolId m = engine.add_symbol(tick("MSFT"), small_config());
    Standalone<TypeParam> ref_a(small_config());
    Standalone<TypeParam> ref_m(small_config());

    // Each call goes to the engine and to the matching standalone book.
    engine.submit(a, limit(1, Side::Sell, 101, 30));
    ref_a.book().submit(limit(1, Side::Sell, 101, 30));
    engine.submit(m, limit(1, Side::Buy, 99, 5));
    ref_m.book().submit(limit(1, Side::Buy, 99, 5));
    engine.submit(a, limit(2, Side::Buy, 101, 10));  // This one trades.
    ref_a.book().submit(limit(2, Side::Buy, 101, 10));
    engine.modify(a, 1, 102, 15);
    ref_a.book().modify(1, 102, 15);
    engine.cancel(m, 9);  // Unknown order, so the book sends CancelRejected.
    ref_m.book().cancel(9);
    engine.cancel(a, 1);
    ref_a.book().cancel(1);

    const auto& all = this->listener_.events();
    EXPECT_EQ(without_match_ids(events_for(all, a)), without_match_ids(ref_a.events()));
    EXPECT_EQ(without_match_ids(events_for(all, m)), without_match_ids(ref_m.events()));
    EXPECT_EQ(all.size(), ref_a.events().size() + ref_m.events().size());
}

TYPED_TEST(EngineTest, MatchIdsAreUniqueAcrossStocks) {
    Engine<TypeParam> engine(this->listener_, 4);
    const SymbolId a = engine.add_symbol(tick("AAPL"), small_config());
    const SymbolId m = engine.add_symbol(tick("MSFT"), small_config());
    engine.submit(a, limit(1, Side::Sell, 100, 10));
    engine.submit(m, limit(1, Side::Sell, 100, 10));
    engine.submit(a, limit(2, Side::Buy, 100, 4));   // Trade 1 in AAPL.
    engine.submit(m, limit(2, Side::Buy, 100, 4));   // Without the engine this is trade 1 too.
    engine.submit(a, limit(3, Side::Buy, 100, 10));  // Trade 3 in AAPL.
    EXPECT_EQ(match_ids(this->listener_.events()), (std::vector<MatchId>{1, 2, 3}));
    EXPECT_EQ(engine.last_match_id(), 3U);
}

TYPED_TEST(EngineTest, EachStockKeepsItsOwnConfig) {
    Engine<TypeParam> engine(this->listener_, 4);
    const SymbolId fine = engine.add_symbol(tick("FINE"), small_config(1));
    const SymbolId coarse = engine.add_symbol(tick("COARSE"), small_config(5));
    engine.submit(fine, limit(1, Side::Buy, 101, 10));
    engine.submit(coarse, limit(1, Side::Buy, 101, 10));
    EXPECT_TRUE(engine.book(fine)->contains(1));
    EXPECT_FALSE(engine.book(coarse)->contains(1));
    const auto coarse_events = events_for(this->listener_.events(), coarse);
    ASSERT_EQ(coarse_events.size(), 1U);
    EXPECT_EQ(coarse_events[0], rejected(1, RejectReason::InvalidPrice));
}

// Runs every standard workload at once, each on its own stock, with their operations
// interleaved one by one. Each stock's forwarded stream must equal what a standalone book
// produces for that workload alone, and the match numbers must count 1, 2, 3, ... across
// all stocks in the order trades happened.
TYPED_TEST(EngineTest, InterleavedWorkloadsMatchStandaloneBooks) {
    const std::vector<WorkloadParams> workloads = standard_workloads();
    Engine<TypeParam> engine(this->listener_, workloads.size());
    std::vector<std::vector<Operation>> streams;
    std::vector<std::unique_ptr<Standalone<TypeParam>>> refs;
    for (const WorkloadParams& w : workloads) {
        std::ostringstream name;
        name << 'W' << streams.size();
        engine.add_symbol(unwrap(make_ticker(name.str())), book_config(w));
        streams.push_back(generate(w));
        refs.push_back(std::make_unique<Standalone<TypeParam>>(book_config(w)));
    }

    // A small adapter so the shared apply() helper can drive one stock of the engine.
    struct EngineStock {
        Engine<TypeParam>* engine;
        SymbolId symbol;
        void submit(const OrderRequest& r) { engine->submit(symbol, r); }
        void cancel(OrderId id) { engine->cancel(symbol, id); }
        void modify(OrderId id, Price p, Qty q) { engine->modify(symbol, id, p, q); }
    };

    std::size_t longest = 0;
    for (const auto& s : streams) {
        longest = std::max(longest, s.size());
    }
    for (std::size_t i = 0; i < longest; ++i) {
        for (std::size_t s = 0; s < streams.size(); ++s) {
            if (i < streams[s].size()) {
                EngineStock stock{&engine, static_cast<SymbolId>(s)};
                apply(stock, streams[s][i]);
                apply(refs[s]->book(), streams[s][i]);
            }
        }
    }

    const auto& all = this->listener_.events();
    std::size_t total = 0;
    for (std::size_t s = 0; s < streams.size(); ++s) {
        SCOPED_TRACE(workloads[s].name);
        EXPECT_EQ(without_match_ids(events_for(all, static_cast<SymbolId>(s))),
                  without_match_ids(refs[s]->events()));
        total += refs[s]->events().size();
    }
    EXPECT_EQ(all.size(), total);

    const std::vector<MatchId> ids = match_ids(all);
    ASSERT_FALSE(ids.empty());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ASSERT_EQ(ids[i], i + 1);
    }
    EXPECT_EQ(engine.last_match_id(), ids.size());
    EXPECT_TRUE(engine.check_invariants());
}

TEST(EngineStatusTest, Prints) {
    std::ostringstream os;
    os << EngineStatus::Ok << ' ' << EngineStatus::UnknownSymbol;
    EXPECT_EQ(os.str(), "Ok UnknownSymbol");
}

}  // namespace
}  // namespace litbook::testing
