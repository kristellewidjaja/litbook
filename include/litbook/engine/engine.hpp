#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/container/flat_id_map.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace litbook {

// What happened to a call that names a stock. Anything that reaches a book is reported
// through events instead, as usual.
enum class EngineStatus : std::uint8_t {
    Ok,             // The call reached the stock's book.
    UnknownSymbol,  // No stock has this SymbolId. Nothing happened and no event was sent.
};

std::ostream& operator<<(std::ostream& os, EngineStatus status);

// The matching engine. It owns one order book per stock and routes each call to the
// right book by SymbolId, which is a plain array index, so routing costs one bounds
// check and one load. All books are built at startup by add_symbol. After that, submit,
// cancel, modify and find never allocate (as long as the book type never allocates).
//
// Each book reports into a small forwarder that adds the stock's SymbolId and passes the
// event on to the one EngineListener. Books number their trades independently, so two
// stocks would both produce trade 1. The forwarder therefore replaces Trade::match_id
// with the engine's own counter, which starts at 1 and counts every trade on every
// stock, so a match number is unique for the whole day, as OUCH and ITCH require.
//
// Order IDs stay per book. The same OrderId may rest in two different stocks, and making
// IDs unique across stocks is the job of the order entry layer that assigns them.
//
// These invariants always hold. symbol_count() <= max_symbols(). SymbolIds 0 to
// symbol_count() - 1 each own exactly one book, and no two of them share a ticker.
// last_match_id() equals the number of trades reported so far.
template <OrderBook Book>
class Engine {
public:
    // The most stocks one engine supports. SymbolId is 16 bits wide.
    static constexpr std::size_t kMaxSymbols = 65'535;

    // Throws std::invalid_argument if max_symbols is 0 or more than kMaxSymbols.
    Engine(EngineListener& listener, std::size_t max_symbols)
        : listener_(&listener), by_ticker_(checked(max_symbols)), max_symbols_(max_symbols) {
        slots_.reserve(max_symbols);
    }

    // Moving the engine would leave every book's forwarder pointing at the old engine.
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine() = default;

    // Builds a book for a new stock and returns its SymbolId. This is a startup call that
    // allocates. Throws std::invalid_argument if the ticker is already used, if the
    // engine already has max_symbols() stocks, or if the book rejects the config.
    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        if (by_ticker_.contains(ticker.key())) {
            throw std::invalid_argument("duplicate ticker " + ticker.name());
        }
        if (slots_.size() == max_symbols_) {
            throw std::invalid_argument("engine already holds max_symbols stocks");
        }
        const auto id = static_cast<SymbolId>(slots_.size());
        slots_.push_back(std::make_unique<Slot>(*this, id, ticker, config));
        by_ticker_.insert(ticker.key(), id);
        return id;
    }

    // Looks up a stock by ticker. This is meant for the order entry path, which receives
    // the ticker on every new order, so it reads one flat hash table and never allocates.
    [[nodiscard]] std::optional<SymbolId> find(Ticker ticker) const noexcept {
        const FlatIdMap::Value v = by_ticker_.find(ticker.key());
        if (v == FlatIdMap::kNotFound) {
            return std::nullopt;
        }
        return static_cast<SymbolId>(v);
    }

    EngineStatus submit(SymbolId symbol, const OrderRequest& request) {
        if (symbol >= slots_.size()) {
            return EngineStatus::UnknownSymbol;
        }
        slots_[symbol]->book.submit(request);
        return EngineStatus::Ok;
    }

    EngineStatus cancel(SymbolId symbol, OrderId id) {
        if (symbol >= slots_.size()) {
            return EngineStatus::UnknownSymbol;
        }
        slots_[symbol]->book.cancel(id);
        return EngineStatus::Ok;
    }

    EngineStatus modify(SymbolId symbol, OrderId id, Price price, Qty qty) {
        if (symbol >= slots_.size()) {
            return EngineStatus::UnknownSymbol;
        }
        slots_[symbol]->book.modify(id, price, qty);
        return EngineStatus::Ok;
    }

    // Snapshot calls, see OrderEntry::restore. They rebuild a fresh engine's books and
    // counters without any event. They are startup calls and throw std::out_of_range for
    // an unknown SymbolId.
    RestoreStatus restore_resting(SymbolId symbol, const RestingOrder& order) {
        return slots_.at(symbol)->book.restore_resting(order);
    }
    void restore_book_match_id(SymbolId symbol, MatchId next) {
        slots_.at(symbol)->book.restore_next_match_id(next);
    }
    void restore_last_match_id(MatchId last) noexcept { last_match_id_ = last; }

    // Read-only access to one stock's book, or nullptr for an unknown SymbolId.
    [[nodiscard]] const Book* book(SymbolId symbol) const noexcept {
        return symbol < slots_.size() ? &slots_[symbol]->book : nullptr;
    }

    // The ticker of a stock. `symbol` must be less than symbol_count().
    [[nodiscard]] Ticker ticker(SymbolId symbol) const noexcept { return slots_[symbol]->ticker; }

    [[nodiscard]] std::size_t symbol_count() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t max_symbols() const noexcept { return max_symbols_; }
    [[nodiscard]] MatchId last_match_id() const noexcept { return last_match_id_; }

    // Checks every book's own invariants. This is O(total orders), so it is for tests.
    [[nodiscard]] bool check_invariants() const {
        for (const auto& slot : slots_) {
            if (!slot->book.check_invariants()) {
                return false;
            }
        }
        return true;
    }

private:
    // Adds the SymbolId to each event from one book and hands it to the engine listener.
    class Forwarder final : public OrderBookListener {
    public:
        Forwarder(Engine& engine, SymbolId symbol) : engine_(&engine), symbol_(symbol) {}

        void on_accepted(const OrderAccepted& e) override {
            engine_->listener_->on_accepted(symbol_, e);
        }
        void on_rejected(const OrderRejected& e) override {
            engine_->listener_->on_rejected(symbol_, e);
        }
        void on_trade(const Trade& e) override {
            Trade numbered = e;
            numbered.match_id = ++engine_->last_match_id_;
            engine_->listener_->on_trade(symbol_, numbered);
        }
        void on_canceled(const OrderCanceled& e) override {
            engine_->listener_->on_canceled(symbol_, e);
        }
        void on_cancel_rejected(const CancelRejected& e) override {
            engine_->listener_->on_cancel_rejected(symbol_, e);
        }
        void on_modified(const OrderModified& e) override {
            engine_->listener_->on_modified(symbol_, e);
        }
        void on_modify_rejected(const ModifyRejected& e) override {
            engine_->listener_->on_modify_rejected(symbol_, e);
        }
        void on_level_update(const LevelUpdate& e) override {
            engine_->listener_->on_level_update(symbol_, e);
        }

    private:
        Engine* engine_;
        SymbolId symbol_;
    };

    // One stock. The forwarder is declared before the book because the book keeps a
    // reference to it, so it has to be constructed first.
    struct Slot {
        Slot(Engine& engine, SymbolId symbol, Ticker t, const BookConfig& config)
            : forwarder(engine, symbol), book(forwarder, config), ticker(t) {}

        Forwarder forwarder;
        Book book;
        Ticker ticker;
    };

    static std::size_t checked(std::size_t max_symbols) {
        if (max_symbols == 0 || max_symbols > kMaxSymbols) {
            throw std::invalid_argument("Engine max_symbols must be in [1, 65535]");
        }
        return max_symbols;
    }

    EngineListener* listener_;
    // Each Slot is its own allocation so that its address never changes, since the book
    // holds a reference to the forwarder inside it.
    std::vector<std::unique_ptr<Slot>> slots_;
    FlatIdMap by_ticker_;  // Ticker::key() to SymbolId.
    std::size_t max_symbols_;
    MatchId last_match_id_ = 0;
};

}  // namespace litbook
