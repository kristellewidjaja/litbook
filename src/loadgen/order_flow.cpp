#include "litbook/loadgen/order_flow.hpp"

#include <stdexcept>
#include <variant>

namespace litbook::loadgen {

namespace {

void check_config(const FlowConfig& c, std::size_t max_live) {
    if (max_live == 0) {
        throw std::invalid_argument("OrderFlow needs room for at least one order");
    }
    if (c.cancel_percent + c.replace_percent + c.aggressive_percent > 100) {
        throw std::invalid_argument("OrderFlow percentages add up to more than 100");
    }
    if (c.min_qty == 0 || c.min_qty > c.max_qty) {
        throw std::invalid_argument("OrderFlow quantity range is empty");
    }
    if (c.tick <= 0 || c.band_ticks == 0) {
        throw std::invalid_argument("OrderFlow needs a positive tick and band");
    }
    // The lowest passive buy and the aggressive sell sit band_ticks below the middle, and
    // both must stay above 0.
    if (c.middle <= c.tick * static_cast<Price>(c.band_ticks) || c.middle % c.tick != 0) {
        throw std::invalid_argument(
            "OrderFlow middle price must be a tick multiple above the band");
    }
}

}  // namespace

OrderFlow::OrderFlow(const FlowConfig& config, std::size_t max_live)
    : config_(config),
      max_live_(max_live),
      state_(config.seed),
      next_ref_(config.first_ref),
      // A live order can be in the map under two UserRefNums while a Replace is on its way.
      index_((check_config(config, max_live), 2 * max_live)) {
    orders_.reserve(max_live);
}

// splitmix64, a small generator with a well-known exact definition, so a seed gives the
// same requests on every platform and compiler (the std distributions do not promise that).
std::uint64_t OrderFlow::random() noexcept {
    state_ += 0x9E37'79B9'7F4A'7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return z ^ (z >> 31U);
}

// A number in [0, n). The modulo is very slightly biased, which does not matter here.
std::uint64_t OrderFlow::random_below(std::uint64_t n) noexcept {
    return random() % n;
}

Qty OrderFlow::random_qty() noexcept {
    const auto span = static_cast<std::uint64_t>(config_.max_qty - config_.min_qty) + 1;
    return config_.min_qty + static_cast<Qty>(random_below(span));
}

Side OrderFlow::random_side() noexcept {
    return random_below(2) == 0 ? Side::Buy : Side::Sell;
}

// Between 1 and band_ticks ticks away from the middle, on the passive side.
Price OrderFlow::passive_price(Side side) noexcept {
    const auto ticks = static_cast<Price>(random_below(config_.band_ticks)) + 1;
    const Price distance = ticks * config_.tick;
    return side == Side::Buy ? config_.middle - distance : config_.middle + distance;
}

std::optional<FlowRequest> OrderFlow::next() noexcept {
    const bool full = orders_.size() + pending_enters_ >= max_live_;
    if (full || orders_.size() >= config_.live_target) {
        if (const auto i = pick_idle()) {
            const std::uint32_t both = config_.cancel_percent + config_.replace_percent;
            const bool do_replace = both > 0 && random_below(both) < config_.replace_percent;
            return do_replace ? replace(*i) : cancel(*i);
        }
        if (full) {
            return std::nullopt;
        }
        return enter(false);
    }
    const std::uint64_t roll = random_below(100);
    std::uint64_t limit = config_.cancel_percent;
    if (roll < limit) {
        if (const auto i = pick_idle()) {
            return cancel(*i);
        }
        return enter(false);
    }
    limit += config_.replace_percent;
    if (roll < limit) {
        if (const auto i = pick_idle()) {
            return replace(*i);
        }
        return enter(false);
    }
    limit += config_.aggressive_percent;
    return enter(roll < limit);
}

FlowRequest OrderFlow::fence() noexcept {
    const protocol::UserRefNum ref = next_ref_++;
    fence_ref_ = ref;
    return {.message = protocol::EnterOrder{.user_ref = ref,
                                            .side = Side::Buy,
                                            .qty = config_.min_qty,
                                            .symbol = config_.fence_symbol,
                                            .price = config_.middle,
                                            .tif = TimeInForce::GoodTillCancel,
                                            .type = OrderType::Limit,
                                            .post_only = false},
            .kind = RequestKind::Fence,
            .ref = ref};
}

FlowRequest OrderFlow::enter(bool aggressive) noexcept {
    const protocol::UserRefNum ref = next_ref_++;
    const Side side = random_side();
    Price price = 0;
    if (aggressive) {
        // Priced through the whole band on the other side, so it can reach every passive
        // order there.
        const Price reach = static_cast<Price>(config_.band_ticks) * config_.tick;
        price = side == Side::Buy ? config_.middle + reach : config_.middle - reach;
    } else {
        price = passive_price(side);
    }
    ++pending_enters_;
    return {.message = protocol::EnterOrder{.user_ref = ref,
                                            .side = side,
                                            .qty = random_qty(),
                                            .symbol = config_.symbol,
                                            .price = price,
                                            .tif = aggressive ? TimeInForce::ImmediateOrCancel
                                                              : TimeInForce::GoodTillCancel,
                                            .type = OrderType::Limit,
                                            .post_only = false},
            .kind = RequestKind::Enter,
            .ref = ref};
}

std::optional<std::size_t> OrderFlow::pick_idle() noexcept {
    if (busy_ == orders_.size()) {
        return std::nullopt;
    }
    // Start at a random order and walk forward to the first one that is not busy. Busy
    // orders are few, so this is almost always the first one looked at.
    const std::size_t start = random_below(orders_.size());
    for (std::size_t step = 0; step < orders_.size(); ++step) {
        std::size_t i = start + step;
        if (i >= orders_.size()) {
            i -= orders_.size();
        }
        if (!orders_[i].busy) {
            return i;
        }
    }
    return std::nullopt;  // Not reached, since busy_ < size means one is idle.
}

FlowRequest OrderFlow::cancel(std::size_t index) noexcept {
    LiveOrder& o = orders_[index];
    o.busy = true;
    ++busy_;
    return {.message = protocol::CancelOrder{.user_ref = o.ref, .qty = 0},
            .kind = RequestKind::Cancel,
            .ref = o.ref};
}

FlowRequest OrderFlow::replace(std::size_t index) noexcept {
    LiveOrder& o = orders_[index];
    const protocol::UserRefNum ref = next_ref_++;
    o.busy = true;
    o.replace_ref = ref;
    ++busy_;
    (void)index_.insert(ref, static_cast<FlatIdMap::Value>(index));
    // The Quantity of a Replace counts the shares that already executed, so the new open
    // size is the random part only.
    return {.message = protocol::ReplaceOrder{.orig_user_ref = o.ref,
                                              .user_ref = ref,
                                              .qty = o.executed + random_qty(),
                                              .price = passive_price(o.side)},
            .kind = RequestKind::Replace,
            .ref = ref};
}

// get_if instead of std::visit, because std::visit may throw bad_variant_access and this
// function promises not to throw.
void OrderFlow::on_reply(const protocol::OutboundMessage& reply) noexcept {
    if (const auto* a = std::get_if<protocol::AcceptedMessage>(&reply)) {
        accepted(*a);
    } else if (const auto* r = std::get_if<protocol::ReplacedMessage>(&reply)) {
        replaced(*r);
    } else if (const auto* c = std::get_if<protocol::CanceledMessage>(&reply)) {
        canceled(*c);
    } else if (const auto* e = std::get_if<protocol::ExecutedMessage>(&reply)) {
        executed(*e);
    } else if (const auto* j = std::get_if<protocol::RejectedMessage>(&reply)) {
        rejected(*j);
    }
}

void OrderFlow::accepted(const protocol::AcceptedMessage& m) noexcept {
    if (pending_enters_ == 0 || find(m.user_ref)) {
        ++unknown_replies_;
        return;
    }
    --pending_enters_;
    const auto index = static_cast<FlatIdMap::Value>(orders_.size());
    orders_.push_back(LiveOrder{.ref = m.user_ref,
                                .replace_ref = 0,
                                .side = m.side,
                                .price = m.price,
                                .open = m.qty,
                                .executed = 0,
                                .busy = false});
    (void)index_.insert(m.user_ref, index);
}

void OrderFlow::replaced(const protocol::ReplacedMessage& m) noexcept {
    const auto index = find(m.orig_user_ref);
    if (!index || orders_[*index].replace_ref != m.user_ref) {
        ++unknown_replies_;
        return;
    }
    LiveOrder& o = orders_[*index];
    (void)index_.erase(o.ref);
    o.ref = m.user_ref;
    o.replace_ref = 0;
    o.price = m.price;
    o.busy = false;
    --busy_;
    if (m.qty == 0) {
        remove(*index);
    } else {
        o.open = m.qty;
    }
}

void OrderFlow::canceled(const protocol::CanceledMessage& m) noexcept {
    if (const auto index = find(m.user_ref)) {
        take_off(*index, m.qty);
    } else {
        ++unknown_replies_;
    }
}

void OrderFlow::executed(const protocol::ExecutedMessage& m) noexcept {
    const auto index = find(m.user_ref);
    if (!index) {
        ++unknown_replies_;
        return;
    }
    LiveOrder& o = orders_[*index];
    const auto shares = static_cast<std::int64_t>(m.qty);
    position_ += o.side == Side::Buy ? shares : -shares;
    o.executed += m.qty;
    take_off(*index, m.qty);
}

void OrderFlow::rejected(const protocol::RejectedMessage& m) noexcept {
    if (m.user_ref == fence_ref_) {
        fence_ref_ = 0;
        return;
    }
    if (const auto index = find(m.user_ref)) {
        // A failed Replace. The exchange cancels the original order right after this, and
        // that Order Canceled removes it, so it stays busy until then.
        LiveOrder& o = orders_[*index];
        if (o.replace_ref != m.user_ref) {
            ++unknown_replies_;
            return;
        }
        (void)index_.erase(o.replace_ref);
        o.replace_ref = 0;
        return;
    }
    if (pending_enters_ == 0) {
        ++unknown_replies_;
        return;
    }
    --pending_enters_;  // A rejected Enter.
}

void OrderFlow::take_off(std::size_t index, Qty shares) noexcept {
    LiveOrder& o = orders_[index];
    o.open = shares >= o.open ? 0 : o.open - shares;
    if (o.open == 0) {
        remove(index);
    }
}

void OrderFlow::remove(std::size_t index) noexcept {
    LiveOrder& o = orders_[index];
    (void)index_.erase(o.ref);
    if (o.replace_ref != 0) {
        (void)index_.erase(o.replace_ref);
    }
    if (o.busy) {
        --busy_;
    }
    const std::size_t last = orders_.size() - 1;
    if (index != last) {
        // The last order moves into the gap, so its map entries must point at its new place.
        o = orders_[last];
        const auto moved = static_cast<FlatIdMap::Value>(index);
        (void)index_.erase(o.ref);
        (void)index_.insert(o.ref, moved);
        if (o.replace_ref != 0) {
            (void)index_.erase(o.replace_ref);
            (void)index_.insert(o.replace_ref, moved);
        }
    }
    orders_.pop_back();
}

std::optional<std::size_t> OrderFlow::find(protocol::UserRefNum ref) const noexcept {
    const FlatIdMap::Value v = index_.find(ref);
    if (v == FlatIdMap::kNotFound) {
        return std::nullopt;
    }
    return v;
}

bool OrderFlow::check_invariants() const {
    if (orders_.size() + pending_enters_ > max_live_) {
        return false;
    }
    std::size_t busy = 0;
    std::size_t keys = 0;
    for (std::size_t i = 0; i < orders_.size(); ++i) {
        const LiveOrder& o = orders_[i];
        if (o.open == 0 || index_.find(o.ref) != i) {
            return false;
        }
        ++keys;
        if (o.replace_ref != 0) {
            if (!o.busy || o.replace_ref <= o.ref || index_.find(o.replace_ref) != i) {
                return false;
            }
            ++keys;
        }
        if (o.busy) {
            ++busy;
        }
    }
    return busy == busy_ && keys == index_.size() && index_.check_invariants();
}

}  // namespace litbook::loadgen
