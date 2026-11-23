#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/container/flat_id_map.hpp"
#include "litbook/container/index_pool.hpp"
#include "litbook/engine/engine.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/protocol/order_entry_config.hpp"
#include "litbook/protocol/order_entry_state.hpp"
#include "litbook/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace litbook::protocol {

// The key under which the order entry layer finds a live order by its account and the
// UserRefNum the client gave it. Every pair of an account (1 to max_owners) and a
// UserRefNum gets its own key, and the largest one is exactly 2^64 - 1.
//
// The account is the lowest digit, counted in base max_owners + 1, so the keys of all
// accounts are interleaved into one increasing range, the same pattern as exchange order
// references. FlatIdMap's fold hash gives every key of such a range its own home slot (in
// each block of table-size keys it XORs every key with the same value), and keeps recent
// orders in nearby slots. With the account in the high 32 bits instead, as before, every
// account had a range of its own, and the fold laid the ranges only 2,048 slots apart, so
// the accounts' recent orders competed for the same slots. At 100,000 live orders that
// made one probe run of about 50,000 slots and every frame about 165 times slower (30.4 us
// instead of 0.18 us, DECISIONS.md D-067).
[[nodiscard]] constexpr FlatIdMap::Key client_map_key(OwnerId owner, UserRefNum user_ref,
                                                      OwnerId max_owners) noexcept {
    return (static_cast<std::uint64_t>(user_ref) * (std::uint64_t{max_owners} + 1)) + owner;
}

// Receives every outbound frame, already encoded, together with the account it is for.
// In Phase 6 the gateway writes the frame to that account's connection. The frame is only
// valid during the call.
class OutboundSink {
public:
    virtual ~OutboundSink() = default;
    virtual void send(OwnerId owner, std::span<const std::byte> frame) = 0;

protected:
    OutboundSink() = default;
    OutboundSink(const OutboundSink&) = default;
    OutboundSink(OutboundSink&&) = default;
    OutboundSink& operator=(const OutboundSink&) = default;
    OutboundSink& operator=(OutboundSink&&) = default;
};

// Receives the public side of every book event, trades and changes to price levels, in
// the order the books produce them. In Phase 7 the matching thread passes them on to the
// publisher thread, which becomes the market data feed in Phase 8. The events are only
// valid during the call.
class MarketDataSink {
public:
    virtual ~MarketDataSink() = default;
    virtual void on_trade(SymbolId symbol, const Trade& trade) = 0;
    virtual void on_level_update(SymbolId symbol, const LevelUpdate& update) = 0;

protected:
    MarketDataSink() = default;
    MarketDataSink(const MarketDataSink&) = default;
    MarketDataSink(MarketDataSink&&) = default;
    MarketDataSink& operator=(const MarketDataSink&) = default;
    MarketDataSink& operator=(MarketDataSink&&) = default;
};

// OrderEntryStats and the snapshot state live in order_entry_state.hpp, which uses the
// plain integer types. These checks keep the two in step.
static_assert(std::is_same_v<std::uint64_t, OrderRef>);
static_assert(std::is_same_v<std::uint32_t, UserRefNum>);
static_assert(std::is_same_v<std::uint16_t, SymbolId>);

// The order entry layer. It owns the matching engine, applies the session rules from
// docs/PROTOCOL.md, and turns engine events into outbound messages for each account.
//
// The client knows an order by (account, UserRefNum), and the exchange knows it by its
// Order Reference Number, which is also the order's ID inside its book. Two flat hash maps
// connect them to one record per live order, kept in an IndexPool. Everything is
// allocated in the constructor and in add_symbol, so handling a message never allocates.
//
// Each account also has a small state struct with its last UserRefNum, its risk limits,
// its number of live orders, and the head of a linked list through the records of its
// live orders. The list lets cancel_all() find one account's orders without scanning
// everyone else's.
//
// These invariants hold between calls. Every live order has exactly one record, reachable
// from both maps and from its account's list, and its open quantity equals the quantity
// resting in its book. A record is released as soon as its order has no open quantity
// left. open_orders(owner) is the length of that account's list. last_user_ref(owner) only
// grows.
template <OrderBook Book>
class OrderEntry final : private EngineListener {
public:
    // Throws std::invalid_argument for a zero max_owners or max_live_orders, or a
    // max_symbols the engine does not support.
    OrderEntry(OutboundSink& sink, const OrderEntryConfig& config)
        : sink_(&sink),
          config_(checked(config)),
          engine_(*this, config.max_symbols),
          records_(config.max_live_orders),
          by_client_(config.max_live_orders),
          by_ref_(config.max_live_orders),
          accounts_(static_cast<std::size_t>(config.max_owners) + 1) {}

    OrderEntry(const OrderEntry&) = delete;
    OrderEntry& operator=(const OrderEntry&) = delete;
    OrderEntry(OrderEntry&&) = delete;
    OrderEntry& operator=(OrderEntry&&) = delete;
    ~OrderEntry() override = default;

    // Lists a stock. This is a startup call, see Engine::add_symbol.
    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        return engine_.add_symbol(ticker, config);
    }

    // Sets the risk limits of one account. This is a startup call. Throws
    // std::out_of_range for owner 0 or an owner above max_owners.
    void set_risk_limits(OwnerId owner, const RiskLimits& limits) {
        account_at(owner).limits = limits;
    }

    // Sends every trade and level update to `sink` as well, or to nobody for nullptr.
    // This is a startup call, and the sink must outlive the order entry layer.
    void set_market_data_sink(MarketDataSink* sink) noexcept { market_data_ = sink; }

    // Sends every later reply to `sink` instead. Recovery uses it to count the replies that
    // replaying a journal produces, and then points the replies back at the matching
    // thread. This is a startup call, and the sink must outlive its use.
    void set_outbound_sink(OutboundSink& sink) noexcept { sink_ = &sink; }

    // Decodes one inbound frame from `owner` and handles it. A malformed frame is counted
    // and its DecodeStatus is returned, and nothing else happens. `now` is the timestamp
    // put on every outbound message this frame causes.
    DecodeStatus on_frame(OwnerId owner, std::span<const std::byte> frame, Timestamp now) {
        const auto decoded = decode_inbound(frame);
        if (!decoded.message) {
            ++stats_.malformed;
            return decoded.status;
        }
        handle(owner, *decoded.message, now);
        return DecodeStatus::Ok;
    }

    // Handles one inbound message that is already decoded.
    void handle(OwnerId owner, const InboundMessage& message, Timestamp now) {
        if (owner == kNoOwner || owner > config_.max_owners) {
            ++stats_.bad_owner;
            return;
        }
        now_ = now;
        std::visit([&](const auto& m) { on_message(owner, m); }, message);
    }

    // Cancels every live order of `owner`, for example because its connection dropped and
    // the account asked for cancel on disconnect. Each order gets an Order Canceled with
    // reason System ('Z'), as if the exchange had canceled it. Returns the number of
    // orders canceled. A bad owner is counted and nothing happens.
    std::uint32_t cancel_all(OwnerId owner, Timestamp now) {
        if (owner == kNoOwner || owner > config_.max_owners) {
            ++stats_.bad_owner;
            return 0;
        }
        now_ = now;
        pending_ = PendingCall{.kind = PendingCall::Kind::CancelAll};
        std::uint32_t canceled = 0;
        Index i = accounts_[owner].first_order;
        while (i != kNoRecord) {
            // Canceling the order releases its record and unlinks it from the list, so
            // the next index has to be read first. A cancel never trades, so no other
            // record of this account changes during the call.
            const Index next = records_[i].next_of_owner;
            engine_.cancel(records_[i].symbol, records_[i].ref);
            ++canceled;
            i = next;
        }
        pending_ = PendingCall{};
        return canceled;
    }

    [[nodiscard]] const Engine<Book>& engine() const noexcept { return engine_; }
    [[nodiscard]] const OrderEntryConfig& config() const noexcept { return config_; }
    [[nodiscard]] const OrderEntryStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint32_t live_orders() const noexcept { return records_.in_use(); }
    [[nodiscard]] UserRefNum last_user_ref(OwnerId owner) const {
        return account_at(owner).last_user_ref;
    }
    [[nodiscard]] std::uint32_t open_orders(OwnerId owner) const {
        return account_at(owner).open_orders;
    }
    [[nodiscard]] const RiskLimits& risk_limits(OwnerId owner) const {
        return account_at(owner).limits;
    }

    // Snapshots (see OrderEntryState for what is kept and in which order).

    // Gives every vector of `state` room for the largest state this layer can have, so a
    // later capture() into it never allocates. This is a startup call.
    void reserve_state(OrderEntryState& state) const {
        state.last_user_ref.reserve(static_cast<std::size_t>(config_.max_owners) + 1);
        state.orders.reserve(config_.max_live_orders);
        state.books.reserve(config_.max_symbols);
        state.resting.reserve(config_.max_live_orders);
    }

    // Writes this layer's whole state into `state`, replacing what was there. It never
    // allocates if reserve_state() was called on `state` first, so a thread that keeps a
    // copy of the exchange can take snapshots after startup.
    void capture(OrderEntryState& state) const {
        state.last_order_ref = last_order_ref_;
        state.last_match_id = engine_.last_match_id();
        state.stats = stats_;
        state.last_user_ref.clear();
        for (const Account& account : accounts_) {
            state.last_user_ref.push_back(account.last_user_ref);
        }
        state.orders.clear();
        for (std::size_t owner = 1; owner < accounts_.size(); ++owner) {
            for (Index i = accounts_[owner].first_order; i != kNoRecord;
                 i = records_[i].next_of_owner) {
                const Record& r = records_[i];
                state.orders.push_back(LiveOrderState{.ref = r.ref,
                                                      .owner = r.owner,
                                                      .user_ref = r.user_ref,
                                                      .symbol = r.symbol,
                                                      .side = r.side,
                                                      .price = r.price,
                                                      .open = r.open,
                                                      .executed = r.executed});
            }
        }
        state.books.clear();
        state.resting.clear();
        auto keep = [&state](const RestingOrder& order) { state.resting.push_back(order); };
        for (std::size_t s = 0; s < engine_.symbol_count(); ++s) {
            const Book& book = *engine_.book(static_cast<SymbolId>(s));
            const std::size_t before = state.resting.size();
            book.for_each_resting(Side::Buy, keep);
            book.for_each_resting(Side::Sell, keep);
            state.books.push_back(BookState{
                .next_match_id = book.next_match_id(),
                .order_count = static_cast<std::uint32_t>(state.resting.size() - before)});
        }
    }

    // Rebuilds this layer from a snapshot, with no outbound message and no market event.
    // It must be fresh, with the same stocks and risk limits as the layer the state was
    // captured from and no message handled yet, or it throws std::logic_error. A state
    // that breaks any invariant of OrderEntryState throws std::invalid_argument with a
    // sentence that names the first problem, and then this layer must be thrown away.
    // This is a startup call and allocates.
    void restore(const OrderEntryState& state) {
        if (last_order_ref_ != 0 || records_.in_use() != 0 || stats_ != OrderEntryStats{}) {
            throw std::logic_error("restore() needs an order entry layer that handled nothing");
        }
        check_state_shape(state);
        restore_books(state);
        restore_records(state);
        for (std::size_t owner = 1; owner < accounts_.size(); ++owner) {
            accounts_[owner].last_user_ref = state.last_user_ref[owner];
        }
        last_order_ref_ = state.last_order_ref;
        stats_ = state.stats;
        engine_.restore_last_match_id(state.last_match_id);

        // Capturing again must give back exactly the same state. This catches a state
        // whose orders or accounts are not in the order capture() writes them, which the
        // checks above do not look at.
        OrderEntryState again;
        capture(again);
        if (again != state) {
            throw std::invalid_argument(
                "the snapshot is not in canonical order (orders by account, then each "
                "book best price first)");
        }
    }

private:
    // Everything the order entry layer needs to know about one live order.
    struct Record {
        OrderRef ref;  // Also the order's ID inside its book.
        OwnerId owner;
        UserRefNum user_ref;
        SymbolId symbol;
        Side side;
        Price price;
        Qty open;      // Shares still resting (or about to rest) in the book.
        Qty executed;  // Shares executed so far, over the order's whole life.
        // The neighbors of this record in its account's list of live orders.
        std::uint32_t prev_of_owner;
        std::uint32_t next_of_owner;
    };
    using Index = IndexPool<Record>::Index;
    static constexpr Index kNoRecord = IndexPool<Record>::kNone;

    // Everything the order entry layer keeps per account.
    struct Account {
        UserRefNum last_user_ref = 0;
        RiskLimits limits;
        std::uint32_t open_orders = 0;  // The number of records in the list below.
        Index first_order = kNoRecord;  // The head of the list of live orders.
    };

    // A cancel that shrinks an order and a replace both work by calling the book's
    // modify(). The book answers through the event handlers below, while modify() is
    // still running, and a handler is not allowed to call back into the engine. So
    // before calling modify(), the caller fills in a PendingCall to tell the handlers
    // what the modify is for, and afterwards it reads back whether the book refused it.
    //
    // For example, a Replace from account 1 that renames order 7 to 8 goes like this.
    // 1. on_message(ReplaceOrder) sets kind = Replace and new_user_ref = 8, then calls
    //    modify().
    // 2. If the book accepts the change, on_modified() sees kind == Replace, moves the
    //    record from UserRefNum 7 to 8 and sends Order Replaced. Any trades that follow
    //    are then reported under 8.
    // 3. If the book refuses it, on_modify_rejected() sets refused = true and saves the
    //    reason. After modify() returns, on_message sees refused, sends Rejected for 8
    //    and cancels the original order. That cancel is a new call into the engine,
    //    which is allowed again now that modify() has returned.
    struct PendingCall {
        enum class Kind : std::uint8_t { None, Reduce, Replace, CancelAll };
        Kind kind = Kind::None;
        UserRefNum new_user_ref = 0;  // Only used by a replace.
        bool refused = false;         // Set if the book sent ModifyRejected.
        WireRejectReason reason = WireRejectReason::Other;
    };

    static OrderEntryConfig checked(const OrderEntryConfig& config) {
        if (config.max_owners == 0 || config.max_live_orders == 0) {
            throw std::invalid_argument("OrderEntry needs max_owners and max_live_orders >= 1");
        }
        return config;
    }

    Account& account_at(OwnerId owner) {
        if (owner == kNoOwner || owner > config_.max_owners) {
            throw std::out_of_range("OrderEntry account number out of range");
        }
        return accounts_[owner];
    }
    const Account& account_at(OwnerId owner) const {
        if (owner == kNoOwner || owner > config_.max_owners) {
            throw std::out_of_range("OrderEntry account number out of range");
        }
        return accounts_[owner];
    }

    [[nodiscard]] FlatIdMap::Key client_key(OwnerId owner, UserRefNum user_ref) const noexcept {
        return client_map_key(owner, user_ref, config_.max_owners);
    }

    // Applies the UserRefNum rule. Returns false if the message must be ignored.
    bool take_user_ref(OwnerId owner, UserRefNum user_ref) noexcept {
        UserRefNum& last = accounts_[owner].last_user_ref;
        if (user_ref <= last) {
            ++stats_.stale_user_ref;
            return false;
        }
        last = user_ref;
        return true;
    }

    Index find_client(OwnerId owner, UserRefNum user_ref) const noexcept {
        return by_client_.find(client_key(owner, user_ref));
    }

    // Puts record i at the front of its account's list.
    void link_to_owner(Index i) noexcept {
        Record& r = records_[i];
        Account& account = accounts_[r.owner];
        r.prev_of_owner = kNoRecord;
        r.next_of_owner = account.first_order;
        if (account.first_order != kNoRecord) {
            records_[account.first_order].prev_of_owner = i;
        }
        account.first_order = i;
        ++account.open_orders;
    }

    void unlink_from_owner(Index i) noexcept {
        const Record& r = records_[i];
        Account& account = accounts_[r.owner];
        if (r.prev_of_owner == kNoRecord) {
            account.first_order = r.next_of_owner;
        } else {
            records_[r.prev_of_owner].next_of_owner = r.next_of_owner;
        }
        if (r.next_of_owner != kNoRecord) {
            records_[r.next_of_owner].prev_of_owner = r.prev_of_owner;
        }
        --account.open_orders;
    }

    // The parts of restore().

    // Checks the sizes and counts of a state before anything is changed.
    void check_state_shape(const OrderEntryState& state) const {
        if (state.last_user_ref.size() != accounts_.size() || state.last_user_ref[0] != 0) {
            throw std::invalid_argument("the snapshot has another number of accounts");
        }
        if (state.books.size() != engine_.symbol_count()) {
            throw std::invalid_argument("the snapshot has another number of stocks");
        }
        if (state.orders.size() != state.resting.size()) {
            throw std::invalid_argument(
                "the snapshot has a different number of live orders and resting orders");
        }
        if (state.orders.size() > config_.max_live_orders) {
            throw std::invalid_argument("the snapshot has more live orders than max_live_orders");
        }
        std::size_t total = 0;
        for (const BookState& book : state.books) {
            total += book.order_count;
        }
        if (total != state.resting.size()) {
            throw std::invalid_argument("the snapshot's books do not add up to its resting orders");
        }
    }

    // Puts every resting order back into its book, and every book's match counter.
    void restore_books(const OrderEntryState& state) {
        std::size_t next = 0;
        for (std::size_t s = 0; s < state.books.size(); ++s) {
            const auto symbol = static_cast<SymbolId>(s);
            for (std::uint32_t k = 0; k < state.books[s].order_count; ++k) {
                const RestingOrder& order = state.resting[next];
                ++next;
                const RestoreStatus status = engine_.restore_resting(symbol, order);
                if (status != RestoreStatus::Ok) {
                    throw std::invalid_argument("the book of stock " + std::to_string(s) +
                                                " refused order " + std::to_string(order.id) +
                                                " of the snapshot");
                }
            }
            engine_.restore_book_match_id(symbol, state.books[s].next_match_id);
        }
    }

    // Rebuilds one record per live order, after checking that it agrees with the order
    // resting in its book. link_to_owner() puts a record at the front of its account's
    // list, so walking the orders backwards rebuilds every list in its original order.
    void restore_records(const OrderEntryState& state) {
        std::unordered_map<std::uint64_t, const RestingOrder*> resting_by_ref;
        for (const RestingOrder& order : state.resting) {
            resting_by_ref.emplace(order.id, &order);
        }
        for (std::size_t k = state.orders.size(); k > 0; --k) {
            const LiveOrderState& order = state.orders[k - 1];
            check_live_order(state, order, resting_by_ref);
            const Index i = records_.acquire();
            records_[i] = Record{.ref = order.ref,
                                 .owner = order.owner,
                                 .user_ref = order.user_ref,
                                 .symbol = order.symbol,
                                 .side = order.side,
                                 .price = order.price,
                                 .open = order.open,
                                 .executed = order.executed,
                                 .prev_of_owner = kNoRecord,
                                 .next_of_owner = kNoRecord};
            by_client_.insert(client_key(order.owner, order.user_ref), i);
            by_ref_.insert(order.ref, i);
            link_to_owner(i);
        }
    }

    void check_live_order(
        const OrderEntryState& state, const LiveOrderState& order,
        const std::unordered_map<std::uint64_t, const RestingOrder*>& resting_by_ref) const {
        const std::string which = "live order " + std::to_string(order.ref) + " of the snapshot";
        if (order.owner == kNoOwner || order.owner > config_.max_owners) {
            throw std::invalid_argument(which + " has no valid account");
        }
        if (order.symbol >= engine_.symbol_count()) {
            throw std::invalid_argument(which + " names a stock that is not listed");
        }
        if (order.ref == 0 || order.ref > state.last_order_ref) {
            throw std::invalid_argument(which + " has a reference number never handed out");
        }
        if (order.user_ref == 0 || order.user_ref > state.last_user_ref[order.owner]) {
            throw std::invalid_argument(which + " has a UserRefNum its account never used");
        }
        if (order.open == 0) {
            throw std::invalid_argument(which + " has nothing open");
        }
        if (by_ref_.contains(order.ref) ||
            by_client_.contains(client_key(order.owner, order.user_ref))) {
            throw std::invalid_argument(which + " is listed twice");
        }
        const auto found = resting_by_ref.find(order.ref);
        if (found == resting_by_ref.end() || !engine_.book(order.symbol)->contains(order.ref)) {
            throw std::invalid_argument(which + " does not rest in its book");
        }
        const RestingOrder& resting = *found->second;
        if (resting.owner != order.owner || resting.side != order.side ||
            resting.price != order.price || resting.remaining != order.open) {
            throw std::invalid_argument(which + " does not match the order in its book");
        }
    }

    void release(Index i) noexcept {
        const Record& r = records_[i];
        by_client_.erase(client_key(r.owner, r.user_ref));
        by_ref_.erase(r.ref);
        unlink_from_owner(i);
        records_.release(i);
    }

    template <class Message>
    void send(OwnerId owner, const Message& m) {
        std::array<std::byte, kMaxMessageLength> buffer{};
        const std::size_t n = encode(m, buffer);
        sink_->send(owner, std::span<const std::byte>(buffer.data(), n));
    }

    void reject(OwnerId owner, UserRefNum user_ref, WireRejectReason reason) {
        send(owner, RejectedMessage{.timestamp = now_, .user_ref = user_ref, .reason = reason});
    }

    // Inbound messages.

    void on_message(OwnerId owner, const EnterOrder& m) {
        if (!take_user_ref(owner, m.user_ref)) {
            return;
        }
        const auto symbol = engine_.find(m.symbol);
        if (!symbol) {
            reject(owner, m.user_ref, WireRejectReason::InvalidSymbol);
            return;
        }
        if (m.type == OrderType::Market && m.price != 0) {
            reject(owner, m.user_ref, WireRejectReason::InvalidPrice);
            return;
        }
        // Pre-trade risk checks. They run before the book sees the order, so a refused
        // order never trades.
        const Account& account = accounts_[owner];
        if (m.qty > account.limits.max_order_qty) {
            reject(owner, m.user_ref, WireRejectReason::MaxQuantityExceeded);
            return;
        }
        if (account.open_orders >= account.limits.max_open_orders) {
            reject(owner, m.user_ref, WireRejectReason::MaxOpenOrdersExceeded);
            return;
        }
        const Index i = records_.acquire();
        if (i == kNoRecord) {
            reject(owner, m.user_ref, WireRejectReason::Other);
            return;
        }
        const OrderRef ref = ++last_order_ref_;
        records_[i] = Record{.ref = ref,
                             .owner = owner,
                             .user_ref = m.user_ref,
                             .symbol = *symbol,
                             .side = m.side,
                             .price = m.price,
                             // The book has not accepted the order yet, so nothing is
                             // open. on_accepted() sets the real quantity.
                             .open = 0,
                             .executed = 0,
                             .prev_of_owner = kNoRecord,
                             .next_of_owner = kNoRecord};
        by_client_.insert(client_key(owner, m.user_ref), i);
        by_ref_.insert(ref, i);
        link_to_owner(i);
        engine_.submit(*symbol, OrderRequest{.id = ref,
                                             .owner = owner,
                                             .side = m.side,
                                             .type = m.type,
                                             .tif = m.tif,
                                             .post_only = m.post_only,
                                             .price = m.price,
                                             .qty = m.qty});
    }

    void on_message(OwnerId owner, const CancelOrder& m) {
        const Index i = find_client(owner, m.user_ref);
        if (i == FlatIdMap::kNotFound) {
            ++stats_.unknown_order;
            return;
        }
        const Record& r = records_[i];
        const OrderRef ref = r.ref;
        const Qty target = m.qty > r.executed ? m.qty - r.executed : 0;
        if (target == 0) {
            engine_.cancel(r.symbol, ref);  // on_canceled reports it.
            return;
        }
        if (target >= r.open) {
            ++stats_.no_effect_cancel;
            return;
        }
        pending_ = PendingCall{.kind = PendingCall::Kind::Reduce};
        engine_.modify(r.symbol, ref, r.price, target);  // on_modified reports it.
        const bool refused = pending_.refused;
        pending_ = PendingCall{};
        if (refused) {
            // The smaller size is not a multiple of the lot size, so nothing changed.
            ++stats_.no_effect_cancel;
        }
    }

    void on_message(OwnerId owner, const ReplaceOrder& m) {
        const Index i = find_client(owner, m.orig_user_ref);
        if (i == FlatIdMap::kNotFound) {
            ++stats_.unknown_order;
            return;
        }
        if (!take_user_ref(owner, m.user_ref)) {
            return;
        }
        const Record& r = records_[i];
        const OrderRef ref = r.ref;
        const SymbolId symbol = r.symbol;
        if (m.qty <= r.executed) {
            fail_replace(owner, m.user_ref, symbol, ref, WireRejectReason::InvalidQuantity);
            return;
        }
        // A replace does not add a live order, so only the size limit applies.
        if (m.qty > accounts_[owner].limits.max_order_qty) {
            fail_replace(owner, m.user_ref, symbol, ref, WireRejectReason::MaxQuantityExceeded);
            return;
        }
        pending_ = PendingCall{.kind = PendingCall::Kind::Replace, .new_user_ref = m.user_ref};
        engine_.modify(symbol, ref, m.price, m.qty - r.executed);
        const PendingCall done = pending_;
        pending_ = PendingCall{};
        if (done.refused) {
            fail_replace(owner, m.user_ref, symbol, ref, done.reason);
        }
    }

    // An invalid replace is refused for the new UserRefNum, and then the original order
    // is canceled, as OUCH does.
    void fail_replace(OwnerId owner, UserRefNum user_ref, SymbolId symbol, OrderRef ref,
                      WireRejectReason reason) {
        reject(owner, user_ref, reason);
        engine_.cancel(symbol, ref);
    }

    // Engine events. Each one is turned into the matching outbound message.

    void on_accepted(SymbolId symbol, const OrderAccepted& e) override {
        const Index i = by_ref_.find(e.order_id);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        Record& r = records_[i];
        r.open = e.qty;
        send(r.owner, AcceptedMessage{.timestamp = now_,
                                      .user_ref = r.user_ref,
                                      .side = e.side,
                                      .qty = e.qty,
                                      .symbol = engine_.ticker(symbol),
                                      .price = e.price,
                                      .tif = e.tif,
                                      .type = e.type,
                                      .post_only = e.post_only,
                                      .order_ref = e.order_id});
    }

    void on_rejected(SymbolId /*symbol*/, const OrderRejected& e) override {
        const Index i = by_ref_.find(e.order_id);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        const Record r = records_[i];
        release(i);
        reject(r.owner, r.user_ref, wire_reason(e.reason));
    }

    void on_trade(SymbolId symbol, const Trade& t) override {
        executed(t.aggressor_id, t, t.aggressor_remaining, Liquidity::Removed);
        executed(t.resting_id, t, t.resting_remaining, Liquidity::Added);
        if (market_data_ != nullptr) {
            market_data_->on_trade(symbol, t);
        }
    }

    void on_level_update(SymbolId symbol, const LevelUpdate& update) override {
        if (market_data_ != nullptr) {
            market_data_->on_level_update(symbol, update);
        }
    }

    void executed(OrderRef ref, const Trade& t, Qty remaining, Liquidity liquidity) {
        const Index i = by_ref_.find(ref);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        Record& r = records_[i];
        r.executed += t.qty;
        r.open = remaining;
        const OwnerId owner = r.owner;
        const ExecutedMessage m{.timestamp = now_,
                                .user_ref = r.user_ref,
                                .qty = t.qty,
                                .price = t.price,
                                .liquidity = liquidity,
                                .match = t.match_id};
        if (remaining == 0) {
            release(i);
        }
        send(owner, m);
    }

    void on_canceled(SymbolId /*symbol*/, const OrderCanceled& e) override {
        const Index i = by_ref_.find(e.order_id);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        Record& r = records_[i];
        r.open = e.remaining_qty;
        const OwnerId owner = r.owner;
        // A cancel from cancel_all() reaches the book as an ordinary cancel, so the book
        // calls it user requested. The client did not ask for it, so it is reported as a
        // system cancel instead.
        const WireCancelReason reason = pending_.kind == PendingCall::Kind::CancelAll
                                            ? WireCancelReason::System
                                            : wire_reason(e.reason);
        const CanceledMessage m{
            .timestamp = now_, .user_ref = r.user_ref, .qty = e.canceled_qty, .reason = reason};
        if (e.remaining_qty == 0) {
            release(i);
        }
        send(owner, m);
    }

    void on_modified(SymbolId symbol, const OrderModified& e) override {
        const Index i = by_ref_.find(e.order_id);
        if (i == FlatIdMap::kNotFound) {
            return;
        }
        Record& r = records_[i];
        const Qty old_open = r.open;
        r.open = e.qty;
        r.price = e.price;
        if (pending_.kind == PendingCall::Kind::None ||
            pending_.kind == PendingCall::Kind::CancelAll) {
            return;
        }
        if (pending_.kind == PendingCall::Kind::Reduce) {
            send(r.owner, CanceledMessage{.timestamp = now_,
                                          .user_ref = r.user_ref,
                                          .qty = old_open - e.qty,
                                          .reason = WireCancelReason::UserRequested});
            return;
        }
        // A replace. From here on the order is known by its new UserRefNum, so any
        // executions caused by re-entering it are reported under the new number.
        const UserRefNum orig = r.user_ref;
        by_client_.erase(client_key(r.owner, orig));
        r.user_ref = pending_.new_user_ref;
        by_client_.insert(client_key(r.owner, r.user_ref), i);
        send(r.owner, ReplacedMessage{.timestamp = now_,
                                      .orig_user_ref = orig,
                                      .user_ref = r.user_ref,
                                      .side = e.side,
                                      .qty = e.qty,
                                      .symbol = engine_.ticker(symbol),
                                      .price = e.price,
                                      .order_ref = e.order_id,
                                      .kept_priority = e.kept_priority});
    }

    void on_modify_rejected(SymbolId /*symbol*/, const ModifyRejected& e) override {
        pending_.refused = true;
        pending_.reason = wire_reason(e.reason);
    }

    static WireRejectReason wire_reason(RejectReason reason) noexcept {
        switch (reason) {
            case RejectReason::ZeroQuantity:
            case RejectReason::InvalidLotSize:
                return WireRejectReason::InvalidQuantity;
            case RejectReason::InvalidPrice:
            case RejectReason::PriceOutOfRange:
                return WireRejectReason::InvalidPrice;
            case RejectReason::InvalidOrderType:
                return WireRejectReason::InvalidOrderType;
            case RejectReason::PostOnlyWouldCross:
                return WireRejectReason::PostOnlyWouldCross;
            case RejectReason::DuplicateOrderId:
            case RejectReason::UnknownOrderId:
                return WireRejectReason::Other;
        }
        return WireRejectReason::Other;
    }

    static WireCancelReason wire_reason(CancelReason reason) noexcept {
        switch (reason) {
            case CancelReason::UserRequested:
                return WireCancelReason::UserRequested;
            case CancelReason::Unfilled:
                return WireCancelReason::ImmediateOrCancel;
            case CancelReason::SelfTradePrevention:
                return WireCancelReason::SelfMatchPrevention;
            case CancelReason::BookFull:
                return WireCancelReason::System;
        }
        return WireCancelReason::System;
    }

    OutboundSink* sink_;
    OrderEntryConfig config_;
    Engine<Book> engine_;
    IndexPool<Record> records_;
    FlatIdMap by_client_;            // client_key(owner, UserRefNum) to record index.
    FlatIdMap by_ref_;               // Order Reference Number to record index.
    std::vector<Account> accounts_;  // Indexed by owner. Slot 0 is unused.
    OrderEntryStats stats_;
    OrderRef last_order_ref_ = 0;
    Timestamp now_ = 0;
    PendingCall pending_;
    MarketDataSink* market_data_ = nullptr;
};

}  // namespace litbook::protocol
