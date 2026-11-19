// Reads a full ITCH 5.0 day and prints what a book builder needs to know before sizing
// its books. That is how many messages of each type there are, how many orders each
// stock has resting at its busiest moment, and how wide each stock's range of prices
// is. The fast book needs a price band and a capacity per stock up front, so these
// numbers decide whether it can hold a real day at all.
//
// Usage: gunzip -c data/12302019.NASDAQ_ITCH50.gz | litbook_itch_stats [sizes.csv]
//
// If a file name is given, the tool also writes one line per stock with orders, in the
// form "locate,symbol,peak_resting,min_price,max_price,whole_cents". The replay tool
// reads it to size each stock's book before the day starts, the way an exchange loads
// its reference data before the open.
//
// This is an offline analysis tool, not hot-path code, so it uses std::unordered_map
// freely.

#include "litbook/itch/binary_file_reader.hpp"
#include "litbook/itch/itch_messages.hpp"
#include "litbook/itch/itch_parser.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace litbook::itch;

struct Live {
    std::uint16_t locate;
    std::uint32_t price;
    std::uint32_t shares;
};

struct Symbol {
    std::string name;
    std::uint64_t messages = 0;
    std::uint64_t live = 0;
    std::uint64_t peak_live = 0;
    std::uint32_t min_price = UINT32_MAX;
    std::uint32_t max_price = 0;
    bool all_whole_cents = true;  // every price a multiple of 100 (Price (4) units)
    // The same, leaving out stub quotes (see is_stub below).
    std::uint64_t stub_orders = 0;
    std::uint32_t core_min = UINT32_MAX;
    std::uint32_t core_max = 0;
    bool core_whole_cents = true;
    std::uint32_t last_trade = 0;  // price of the most recent execution, 0 before the first
};

// Market makers must always show a quote, and when they do not want to trade they post
// "stub quotes" at absurd prices. In this data they sit at the extremes the format
// allows, $0.0001 and about $200,000. Prices at or below 1 cent or at or above $199,000
// are counted as stubs here, so the range of the real trading prices can be measured.
constexpr bool is_stub(std::uint32_t price) {
    return price <= 100 || price >= 1'990'000'000U;
}

class Stats {
public:
    Stats() : symbols_(65'536) { live_.reserve(1U << 22U); }

    void on_system_event(const SystemEvent& m) { system_events_.push_back(m.event_code); }
    void on_stock_directory(const StockDirectory& m) {
        symbols_[m.header.stock_locate].name.assign(m.stock.data(), m.stock.size());
    }
    void on_stock_trading_action(const StockTradingAction& /*m*/) {}
    void on_add_order(const AddOrder& m) {
        add(m.header.stock_locate, m.order_ref, m.price, m.shares);
    }
    void on_order_executed(const OrderExecuted& m) {
        reduce(m.order_ref, m.executed_shares, Trade::AtOrderPrice);
    }
    void on_order_executed_with_price(const OrderExecutedWithPrice& m) {
        reduce(m.order_ref, m.executed_shares, Trade::AtPrice, m.execution_price);
    }
    void on_order_cancel(const OrderCancel& m) {
        reduce(m.order_ref, m.canceled_shares, Trade::No);
    }
    void on_order_delete(const OrderDelete& m) {
        const auto it = live_.find(m.order_ref);
        if (it == live_.end()) {
            ++unknown_refs_;
            return;
        }
        remove(it);
    }
    void on_order_replace(const OrderReplace& m) {
        const auto it = live_.find(m.original_order_ref);
        if (it == live_.end()) {
            ++unknown_refs_;
            return;
        }
        const std::uint16_t locate = it->second.locate;
        remove(it);
        add(locate, m.new_order_ref, m.price, m.shares);
    }

    void count_type(char type) { ++type_counts_[static_cast<unsigned char>(type)]; }
    void count_bad_length(char type) { ++bad_length_[static_cast<unsigned char>(type)]; }
    void count_message_for(std::span<const std::byte> payload) {
        if (payload.size() >= 3) {
            const auto locate =
                static_cast<std::uint16_t>((std::to_integer<unsigned>(payload[1]) << 8U) |
                                           std::to_integer<unsigned>(payload[2]));
            ++symbols_[locate].messages;
        }
    }

    void print(std::uint64_t total, std::uint64_t bytes) const;
    bool write_sizes(const char* path) const;

private:
    using LiveMap = std::unordered_map<std::uint64_t, Live>;
    enum class Trade : std::uint8_t { No, AtOrderPrice, AtPrice };

    void add(std::uint16_t locate, std::uint64_t ref, std::uint32_t price, std::uint32_t shares) {
        if (!live_.emplace(ref, Live{.locate = locate, .price = price, .shares = shares}).second) {
            ++duplicate_refs_;
            return;
        }
        Symbol& s = symbols_[locate];
        if (s.last_trade != 0) {
            // Distance from the last trade in cents, bucketed by powers of two.
            const std::uint32_t far =
                price > s.last_trade ? price - s.last_trade : s.last_trade - price;
            ++distance_[static_cast<std::size_t>(std::bit_width(far / 100U))];
            ++distance_total_;
        }
        s.peak_live = std::max(s.peak_live, ++s.live);
        s.min_price = std::min(s.min_price, price);
        s.max_price = std::max(s.max_price, price);
        s.all_whole_cents = s.all_whole_cents && price % 100 == 0;
        if (is_stub(price)) {
            ++s.stub_orders;
            ++stub_orders_;
        } else {
            s.core_min = std::min(s.core_min, price);
            s.core_max = std::max(s.core_max, price);
            s.core_whole_cents = s.core_whole_cents && price % 100 == 0;
        }
        ++adds_;
        peak_total_live_ = std::max<std::uint64_t>(peak_total_live_, live_.size());
    }
    void reduce(std::uint64_t ref, std::uint32_t shares, Trade trade, std::uint32_t price = 0) {
        const auto it = live_.find(ref);
        if (it == live_.end()) {
            ++unknown_refs_;
            return;
        }
        if (trade != Trade::No) {
            symbols_[it->second.locate].last_trade =
                trade == Trade::AtPrice ? price : it->second.price;
        }
        if (shares > it->second.shares) {
            ++over_reductions_;
        }
        it->second.shares -= std::min(shares, it->second.shares);
        if (it->second.shares == 0) {
            remove(it);
        }
    }
    void remove(LiveMap::iterator it) {
        --symbols_[it->second.locate].live;
        live_.erase(it);
    }

    std::vector<Symbol> symbols_;
    LiveMap live_;
    std::array<std::uint64_t, 256> type_counts_{};
    std::array<std::uint64_t, 256> bad_length_{};
    std::vector<char> system_events_;
    std::uint64_t unknown_refs_ = 0;
    std::uint64_t duplicate_refs_ = 0;
    std::uint64_t over_reductions_ = 0;
    std::uint64_t peak_total_live_ = 0;
    std::uint64_t stub_orders_ = 0;
    std::uint64_t adds_ = 0;
    std::array<std::uint64_t, 40> distance_{};  // index = bit_width(distance in cents)
    std::uint64_t distance_total_ = 0;
};

void Stats::print(std::uint64_t total, std::uint64_t bytes) const {
    std::printf("messages: %llu, bytes: %llu\n", static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(bytes));
    std::printf("\nmessages by type (bad length in brackets):\n");
    for (std::size_t t = 0; t < 256; ++t) {
        if (type_counts_[t] != 0) {
            std::printf("  %c %12llu  [%llu]\n", static_cast<char>(t),
                        static_cast<unsigned long long>(type_counts_[t]),
                        static_cast<unsigned long long>(bad_length_[t]));
        }
    }
    std::printf("\nsystem events in order: %.*s\n", static_cast<int>(system_events_.size()),
                system_events_.data());
    std::printf(
        "references to unknown orders: %llu, duplicate order refs: %llu, "
        "reductions larger than the order: %llu\n",
        static_cast<unsigned long long>(unknown_refs_),
        static_cast<unsigned long long>(duplicate_refs_),
        static_cast<unsigned long long>(over_reductions_));
    std::printf("orders resting at the end: %zu, peak resting across all stocks: %llu\n",
                live_.size(), static_cast<unsigned long long>(peak_total_live_));

    // Price range per stock, measured in ladder levels. A stock whose prices are all
    // whole cents can use a tick of 100 (in Price (4) units), otherwise the tick is 1.
    std::vector<const Symbol*> active;
    std::uint64_t over_limit = 0;
    std::uint64_t sub_cent = 0;
    for (const Symbol& s : symbols_) {
        if (s.max_price == 0) {
            continue;
        }
        active.push_back(&s);
        const std::uint64_t tick = s.all_whole_cents ? 100 : 1;
        sub_cent += s.all_whole_cents ? 0 : 1;
        if ((s.max_price - s.min_price) / tick + 1 > (std::uint64_t{1} << 22U)) {
            ++over_limit;
        }
    }
    std::printf(
        "\nstocks with orders: %zu, with sub-cent prices: %llu, with more than 2^22 "
        "levels between their lowest and highest order price: %llu\n",
        active.size(), static_cast<unsigned long long>(sub_cent),
        static_cast<unsigned long long>(over_limit));

    const auto levels = [](const Symbol* s) {
        return (s->max_price - s->min_price) / (s->all_whole_cents ? 100U : 1U) + 1;
    };
    const auto show = [&](const char* title, auto key) {
        std::vector<const Symbol*> v = active;
        std::ranges::sort(v, [&](const Symbol* a, const Symbol* b) { return key(a) > key(b); });
        std::printf("\n%s\n  %-8s %12s %10s %14s %14s %12s\n", title, "symbol", "messages",
                    "peak live", "min price $", "max price $", "levels");
        for (std::size_t i = 0; i < std::min<std::size_t>(10, v.size()); ++i) {
            const Symbol* s = v[i];
            std::printf("  %-8.8s %12llu %10llu %14.4f %14.4f %12u\n", s->name.c_str(),
                        static_cast<unsigned long long>(s->messages),
                        static_cast<unsigned long long>(s->peak_live), s->min_price / 1e4,
                        s->max_price / 1e4, levels(s));
        }
    };
    show("top 10 by messages:", [](const Symbol* s) { return s->messages; });
    show("top 10 by peak resting orders:", [](const Symbol* s) { return s->peak_live; });
    show("top 10 by price range in levels:", levels);

    // The same measurement without stub quotes.
    std::uint64_t core_over = 0;
    std::uint64_t core_sub_cent = 0;
    std::vector<std::uint32_t> core_levels;
    for (const Symbol* s : active) {
        if (s->core_max == 0) {
            continue;
        }
        const std::uint32_t tick = s->core_whole_cents ? 100 : 1;
        const std::uint32_t n = (s->core_max - s->core_min) / tick + 1;
        core_levels.push_back(n);
        core_over += n > (1U << 22U) ? 1 : 0;
        core_sub_cent += s->core_whole_cents ? 0 : 1;
    }
    std::ranges::sort(core_levels);
    std::printf(
        "\nwithout stub quotes (price <= $0.01 or >= $199,000): stub orders %llu of %llu "
        "adds; stocks with sub-cent prices: %llu; stocks over 2^22 levels: %llu\n",
        static_cast<unsigned long long>(stub_orders_), static_cast<unsigned long long>(adds_),
        static_cast<unsigned long long>(core_sub_cent), static_cast<unsigned long long>(core_over));
    if (!core_levels.empty()) {
        const auto at = [&](double p) {
            return core_levels[static_cast<std::size_t>(
                p * static_cast<double>(core_levels.size() - 1))];
        };
        std::printf("core levels per stock: median %u, p90 %u, p99 %u, max %u\n", at(0.5), at(0.9),
                    at(0.99), core_levels.back());
    }
    std::vector<const Symbol*> wide = active;
    std::ranges::sort(wide, [](const Symbol* a, const Symbol* b) {
        return (a->core_max - std::min(a->core_min, a->core_max)) /
                   (a->core_whole_cents ? 100U : 1U) >
               (b->core_max - std::min(b->core_min, b->core_max)) /
                   (b->core_whole_cents ? 100U : 1U);
    });
    std::printf("widest core ranges:\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(8, wide.size()); ++i) {
        const Symbol* s = wide[i];
        std::printf("  %-8.8s core $%.4f .. $%.4f, sub-cent %s, stubs %llu, peak live %llu\n",
                    s->name.c_str(), s->core_min / 1e4, s->core_max / 1e4,
                    s->core_whole_cents ? "no" : "yes",
                    static_cast<unsigned long long>(s->stub_orders),
                    static_cast<unsigned long long>(s->peak_live));
    }

    std::printf(
        "\nhow far new orders are from the stock's last trade (orders after its first "
        "trade: %llu)\n",
        static_cast<unsigned long long>(distance_total_));
    std::uint64_t cumulative = 0;
    for (std::size_t b = 0; b < distance_.size() && distance_total_ > 0; ++b) {
        cumulative += distance_[b];
        if (distance_[b] != 0) {
            std::printf(
                "  within %10llu cents: %8.4f%%\n",
                static_cast<unsigned long long>((std::uint64_t{1} << b) - 1),
                100.0 * static_cast<double>(cumulative) / static_cast<double>(distance_total_));
        }
    }

    std::vector<std::uint32_t> all_levels;
    all_levels.reserve(active.size());
    for (const Symbol* s : active) {
        all_levels.push_back(levels(s));
    }
    std::ranges::sort(all_levels);
    if (!all_levels.empty()) {
        const auto at = [&](double p) {
            return all_levels[static_cast<std::size_t>(p *
                                                       static_cast<double>(all_levels.size() - 1))];
        };
        std::printf("\nlevels per stock: median %u, p90 %u, p99 %u, max %u\n", at(0.5), at(0.9),
                    at(0.99), all_levels.back());
    }
}

bool Stats::write_sizes(const char* path) const {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path, "w"), &std::fclose);
    if (!file) {
        return false;
    }
    std::FILE* out = file.get();
    for (std::size_t locate = 0; locate < symbols_.size(); ++locate) {
        const Symbol& s = symbols_[locate];
        if (s.max_price == 0) {
            continue;
        }
        std::string name = s.name;
        while (!name.empty() && name.back() == ' ') {
            name.pop_back();
        }
        std::fprintf(out, "%zu,%s,%llu,%u,%u,%d\n", locate, name.c_str(),
                     static_cast<unsigned long long>(s.peak_live), s.min_price, s.max_price,
                     s.all_whole_cents ? 1 : 0);
    }
    return std::fclose(file.release()) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    BinaryFileReader reader(stdin, std::size_t{1} << 24U);
    Stats stats;
    std::span<const std::byte> payload;
    std::uint64_t total = 0;
    BinaryFileReader::Status status{};
    while ((status = reader.next(payload)) == BinaryFileReader::Status::Message) {
        ++total;
        const char type = static_cast<char>(std::to_integer<unsigned char>(payload[0]));
        stats.count_type(type);
        stats.count_message_for(payload);
        if (parse_message(payload, stats) == ParseStatus::BadLength) {
            stats.count_bad_length(type);
        }
    }
    stats.print(total, reader.bytes_consumed());
    if (argc > 1 && !stats.write_sizes(argv[1])) {
        std::fprintf(stderr, "could not write %s\n", argv[1]);
        return 1;
    }
    const char* end = status == BinaryFileReader::Status::EndOfSession ? "end-of-session marker"
                      : status == BinaryFileReader::Status::EndOfFile  ? "end of file, no marker"
                                                                       : "TRUNCATED";
    std::printf("\ninput ended with: %s\n", end);
    return status == BinaryFileReader::Status::Truncated ? 1 : 0;
}
