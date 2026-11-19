#pragma once

#include "litbook/container/flat_id_map.hpp"
#include "litbook/container/index_pool.hpp"
#include "litbook/itch/itch_messages.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace litbook::itch {

// Rebuilds order books from a Nasdaq ITCH 5.0 feed. It is an ITCH handler (see
// itch_parser.hpp) that turns each message into calls on one book per stock.
//
// ITCH reports what already happened on Nasdaq, after matching, so each message is
// translated as follows.
//   1. An add ('A' or 'F') becomes a resting limit order with the ITCH order reference
//      as its ID.
//   2. An execution ('E' or 'C') or a partial cancel ('X') shrinks the order. That is a
//      modify to the smaller size at the same price, which keeps its place in line, or a
//      cancel if nothing is left.
//   3. A delete ('D') becomes a cancel.
//   4. A replace ('U') becomes a cancel of the old order and a new order with the new
//      reference, on the same side.
// ITCH executions name the order and the shares but not the remaining size, so the
// builder keeps its own small record per resting order (stock, side, price, remaining).
// Messages for stocks that have no book are ignored, which lets a run cover only some
// stocks.
//
// Halts. While a stock is halted, paused or in a quotation-only period, Nasdaq accepts
// orders without matching them, so its book can cross, and a reopening auction clears it
// later. Our books always match and have no auction, so they would trade where Nasdaq
// did not. So when a trading action message ('H') puts a stock in any state other than
// 'T', the builder stops replaying that stock for the rest of the day. Its book keeps
// whatever it held at that moment, and suspended_stocks() counts how many stocks this
// happened to.
//
// Around is a hook that wraps every group of book calls made for one message, for
// example to time it. It is called as around(f) and must call f() exactly once. The
// default just calls f.
//
// The builder never allocates while handling messages. Its order records are a fixed
// pool plus a FlatIdMap, sized by max_live_orders in the constructor. If more orders are
// resting at once than that, the extra ones cannot be tracked. They are counted in
// dropped() and their later messages are ignored, so a nonzero dropped() means the
// capacity was too small.
//
// These invariants hold between messages. Every tracked order has remaining >= 1 and
// belongs to a stock that has a book, and the record count equals the number of entries
// in the ID map.
struct CallDirectly {
    template <class F>
    void operator()(F&& f) const {
        std::forward<F>(f)();
    }
};

template <class Book, class Around = CallDirectly>
class ItchBookBuilder {
public:
    static constexpr std::size_t kStockLocates = 65'536;  // stock locate is 2 bytes

    explicit ItchBookBuilder(std::uint32_t max_live_orders, Around around = {})
        : books_(kStockLocates),
          suspended_(kStockLocates, false),
          refs_(max_live_orders),
          live_(max_live_orders),
          around_(around) {}

    // Creates the book for one stock. Call this before the day starts.
    template <class... Args>
    Book& add_book(std::uint16_t locate, Args&&... args) {
        books_[locate] = std::make_unique<Book>(std::forward<Args>(args)...);
        return *books_[locate];
    }

    [[nodiscard]] Book* book(std::uint16_t locate) noexcept { return books_[locate].get(); }
    [[nodiscard]] const Book* book(std::uint16_t locate) const noexcept {
        return books_[locate].get();
    }

    // ITCH handler interface.
    void on_system_event(const SystemEvent& /*m*/) {}
    void on_stock_directory(const StockDirectory& /*m*/) {}

    void on_stock_trading_action(const StockTradingAction& m) {
        const std::uint16_t locate = m.header.stock_locate;
        if (m.trading_state != 'T' && books_[locate] && !suspended_[locate]) {
            suspended_[locate] = true;
            ++suspended_count_;
        }
    }

    // True while messages for this stock still reach its book.
    [[nodiscard]] bool replaying(std::uint16_t locate) const noexcept {
        return books_[locate] && !suspended_[locate];
    }

    void on_add_order(const AddOrder& m) {
        if (!replaying(m.header.stock_locate)) {
            return;
        }
        Book* b = book(m.header.stock_locate);
        const Side side = m.side == 'B' ? Side::Buy : Side::Sell;
        if (!track(m.order_ref, Live{.locate = m.header.stock_locate,
                                     .side = side,
                                     .price = m.price,
                                     .remaining = m.shares})) {
            return;
        }
        call([&] {
            b->submit({.id = m.order_ref, .side = side, .price = m.price, .qty = m.shares});
        });
    }

    void on_order_executed(const OrderExecuted& m) { shrink(m.order_ref, m.executed_shares); }
    void on_order_executed_with_price(const OrderExecutedWithPrice& m) {
        shrink(m.order_ref, m.executed_shares);
    }
    void on_order_cancel(const OrderCancel& m) { shrink(m.order_ref, m.canceled_shares); }

    void on_order_delete(const OrderDelete& m) {
        const Index i = refs_.find(m.order_ref);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        const std::uint16_t locate = live_[i].locate;
        forget(m.order_ref, i);
        if (!replaying(locate)) {
            return;
        }
        Book* b = books_[locate].get();
        call([&] { b->cancel(m.order_ref); });
    }

    void on_order_replace(const OrderReplace& m) {
        const Index i = refs_.find(m.original_order_ref);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        const Live old = live_[i];
        forget(m.original_order_ref, i);
        if (!replaying(old.locate)) {
            return;
        }
        Book* b = books_[old.locate].get();
        // The old record was just freed, so tracking the new one cannot run out of room.
        (void)track(
            m.new_order_ref,
            Live{.locate = old.locate, .side = old.side, .price = m.price, .remaining = m.shares});
        call([&] {
            b->cancel(m.original_order_ref);
            b->submit({.id = m.new_order_ref, .side = old.side, .price = m.price, .qty = m.shares});
        });
    }

    // Messages that reached a book (a replace counts once).
    [[nodiscard]] std::uint64_t book_messages() const noexcept { return book_messages_; }
    // Orders resting right now, according to the feed.
    [[nodiscard]] std::size_t live_orders() const noexcept { return refs_.size(); }
    // Stocks that stopped being replayed because of a halt, pause or quotation period.
    [[nodiscard]] std::size_t suspended_stocks() const noexcept { return suspended_count_; }
    // Orders that could not be tracked because max_live_orders was too small.
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] const std::vector<std::unique_ptr<Book>>& books() const noexcept {
        return books_;
    }
    [[nodiscard]] Around& around() noexcept { return around_; }

private:
    using Index = IndexPool<int>::Index;

    struct Live {
        std::uint16_t locate;
        Side side;
        std::uint32_t price;  // Price (4), the same unit the books are given
        std::uint32_t remaining;
    };

    // Returns false, and counts a drop, if the order cannot be tracked.
    bool track(std::uint64_t ref, const Live& order) {
        const Index i = live_.acquire();
        if (i == IndexPool<Live>::kNone) {
            ++dropped_;
            return false;
        }
        if (!refs_.insert(ref, i)) {
            live_.release(i);  // a reference that is already resting, which ITCH never sends
            ++dropped_;
            return false;
        }
        live_[i] = order;
        return true;
    }

    void forget(std::uint64_t ref, Index i) {
        refs_.erase(ref);
        live_.release(i);
    }

    // An execution or partial cancel of `shares`.
    void shrink(std::uint64_t ref, std::uint32_t shares) {
        const Index i = refs_.find(ref);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        Live& order = live_[i];
        if (!replaying(order.locate)) {
            forget(ref, i);  // the stock is suspended, so stop tracking its orders
            return;
        }
        Book* b = books_[order.locate].get();
        if (shares >= order.remaining) {
            forget(ref, i);
            call([&] { b->cancel(ref); });
            return;
        }
        order.remaining -= shares;
        const Live now = order;
        call([&] { b->modify(ref, now.price, now.remaining); });
    }

    template <class F>
    void call(F&& f) {
        ++book_messages_;
        around_(std::forward<F>(f));
    }

    std::vector<std::unique_ptr<Book>> books_;
    std::vector<bool> suspended_;
    std::size_t suspended_count_ = 0;
    FlatIdMap refs_;  // ITCH order reference -> record in live_
    IndexPool<Live> live_;
    Around around_;
    std::uint64_t book_messages_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace litbook::itch
