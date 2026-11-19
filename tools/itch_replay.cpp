// Replays a full Nasdaq TotalView-ITCH 5.0 day through one book per stock and measures
// how long the book takes per message. This is the project's headline benchmark.
//
// Usage:
//   gunzip -c data/12302019.NASDAQ_ITCH50.gz | litbook_itch_replay sizes.csv MODE [--ladder-stocks]
//   [--untimed] [--sample-every N]
//
// sizes.csv comes from litbook_itch_stats and gives each stock's peak number of resting
// orders and its price range, so every book is sized before the day starts.
//
// MODE is one of
//   map      MapOrderBook for every stock
//   sorted   SortedOrderBook for every stock
//   fast     FastOrderBook, only for the stocks whose whole price range fits a small
//            ladder (most stocks have stub quotes from $0.0001 to $199,999.99, which no
//            ladder can hold)
//   null     a book that does nothing, run through the same pipeline. Its CPU time is
//            the cost of reading, decoding and tracking orders, so subtracting it
//            from an untimed run of a real book leaves that book's own cost.
//   verify   MapOrderBook and SortedOrderBook side by side. After every message their
//            events must be identical. Nothing is timed.
// --ladder-stocks limits map and sorted to the same stocks fast can hold, so all three
// can be compared on identical input.
// --untimed skips the timer reads. Comparing the CPU time of the message loop with and
// without them shows how much the measurement itself costs.
// --sample-every N times one message in every N (default 64). N = 1 times every message,
// which is how Phase 4b measured.
//
// How ITCH maps onto the book. ITCH reports what already happened on Nasdaq, after
// matching. So an add becomes a resting limit order, an execution or a partial cancel
// shrinks an order (a modify to the smaller size, which keeps its place in line, or a
// cancel when nothing is left), a delete becomes a cancel, and a replace becomes a cancel
// followed by a new order. Because Nasdaq already matched everything, a faithful replay
// produces no trades and no rejects at all. The tool counts both, and any nonzero count
// means the replay drifted from Nasdaq's real book.
//
// The translation itself is ItchBookBuilder (include/litbook/itch/book_builder.hpp),
// which has its own unit tests.
//
// What is timed. Only the calls into the book for one ITCH message in every N (a replace is
// two calls, timed together). Reading, decompressing and decoding the file, and the
// builder's own table that turns "execute 100 shares of order X" into a new size, are
// not timed. Each book's listener only counts events.
//
// Why only one message in N. Reading the timer is not free on this machine. The isb
// instruction in TickClock::now() makes the CPU finish everything in flight before it
// reads the counter, and timing every message added roughly 40 to 100 ns of CPU time per
// message, about as much as the sorted book itself costs. Timing one message in 64 spreads
// that cost so thinly that the book runs at nearly its natural speed, and 64 still leaves
// about four million samples for the percentiles. Each sample still contains one timer
// read, so the tool also measures the timer floor, the reading for a call that does
// nothing, and prints it next to the results.

#include "litbook/book/book_config.hpp"
#include "litbook/book/events.hpp"
#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/recording_listener.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/itch/binary_file_reader.hpp"
#include "litbook/itch/book_builder.hpp"
#include "litbook/itch/itch_messages.hpp"
#include "litbook/itch/itch_parser.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace litbook;
using metrics::LatencyHistogram;
using metrics::TickClock;

// A ladder this many levels long per side costs 2 * 16,384 * 24 bytes, about 0.8 MB.
// Only stocks whose whole price range fits get a FastOrderBook.
constexpr std::uint64_t kLadderLimit = std::uint64_t{1} << 14U;

struct StockInfo {
    std::string symbol;
    std::uint32_t peak = 0;
    std::uint32_t min_price = 0;
    std::uint32_t max_price = 0;
    bool whole_cents = false;

    [[nodiscard]] Price tick() const { return whole_cents ? 100 : 1; }
    [[nodiscard]] std::uint64_t ladder_levels() const {
        return (max_price - min_price) / static_cast<std::uint32_t>(tick()) + 1;
    }
    [[nodiscard]] bool fits_ladder() const { return ladder_levels() <= kLadderLimit; }
};

std::vector<std::optional<StockInfo>> load_sizes(const char* path) {
    std::vector<std::optional<StockInfo>> stocks(65'536);
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path, "r"), &std::fclose);
    if (!file) {
        return {};
    }
    std::FILE* in = file.get();
    std::array<char, 256> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), in) != nullptr) {
        unsigned locate = 0;
        std::array<char, 16> symbol{};
        unsigned peak = 0;
        unsigned min_price = 0;
        unsigned max_price = 0;
        int whole = 0;
        // NOLINTNEXTLINE(cert-err34-c): a malformed line simply does not match all six fields
        if (std::sscanf(line.data(), "%u,%15[^,],%u,%u,%u,%d", &locate, symbol.data(), &peak,
                        &min_price, &max_price, &whole) == 6 &&
            locate < stocks.size()) {
            stocks[locate] = StockInfo{.symbol = symbol.data(),
                                       .peak = peak,
                                       .min_price = min_price,
                                       .max_price = max_price,
                                       .whole_cents = whole != 0};
        }
    }
    return stocks;
}

// Counts the events that show whether the replay is still faithful to Nasdaq's book.
class HealthListener final : public OrderBookListener {
public:
    void on_accepted(const OrderAccepted& /*e*/) override { ++events; }
    void on_rejected(const OrderRejected& /*e*/) override { ++events, ++rejected; }
    void on_trade(const Trade& /*e*/) override { ++events, ++trades; }
    void on_canceled(const OrderCanceled& e) override {
        ++events;
        book_full += e.reason == CancelReason::BookFull ? 1 : 0;
    }
    void on_cancel_rejected(const CancelRejected& /*e*/) override { ++events, ++rejected; }
    void on_modified(const OrderModified& /*e*/) override { ++events; }
    void on_modify_rejected(const ModifyRejected& /*e*/) override { ++events, ++rejected; }
    void on_level_update(const LevelUpdate& /*e*/) override { ++events; }

    std::uint64_t events = 0;
    std::uint64_t trades = 0;    // must stay 0, Nasdaq already matched everything
    std::uint64_t rejected = 0;  // must stay 0
    std::uint64_t book_full = 0;
};

// Runs MapOrderBook and SortedOrderBook in lockstep for verify mode. After every call
// their recorded events must be identical. Then the events go on to the outer listener.
class VerifyBook {
public:
    VerifyBook(OrderBookListener& outer, BookConfig config)
        : outer_(&outer), map_(map_rec_, config), sorted_(sorted_rec_, config) {}

    void submit(const OrderRequest& r) {
        both([&](auto& b) { b.submit(r); });
    }
    void cancel(OrderId id) {
        both([&](auto& b) { b.cancel(id); });
    }
    void modify(OrderId id, Price p, Qty q) {
        both([&](auto& b) { b.modify(id, p, q); });
    }

    [[nodiscard]] std::size_t order_count() const { return map_.order_count(); }
    [[nodiscard]] bool check_invariants() const {
        return map_.check_invariants() && sorted_.check_invariants() &&
               map_.order_count() == sorted_.order_count();
    }
    [[nodiscard]] std::uint64_t mismatches() const { return mismatches_; }

private:
    template <class F>
    void both(F&& call) {
        call(map_);
        call(sorted_);
        if (map_rec_.events() != sorted_rec_.events()) {
            ++mismatches_;
        }
        for (const BookEvent& e : map_rec_.events()) {
            std::visit([&](const auto& ev) { forward(ev); }, e);
        }
        map_rec_.clear();
        sorted_rec_.clear();
    }
    void forward(const OrderAccepted& e) { outer_->on_accepted(e); }
    void forward(const OrderRejected& e) { outer_->on_rejected(e); }
    void forward(const Trade& e) { outer_->on_trade(e); }
    void forward(const OrderCanceled& e) { outer_->on_canceled(e); }
    void forward(const CancelRejected& e) { outer_->on_cancel_rejected(e); }
    void forward(const OrderModified& e) { outer_->on_modified(e); }
    void forward(const ModifyRejected& e) { outer_->on_modify_rejected(e); }
    void forward(const LevelUpdate& e) { outer_->on_level_update(e); }

    OrderBookListener* outer_;
    RecordingListener map_rec_;
    RecordingListener sorted_rec_;
    MapOrderBook map_;
    SortedOrderBook sorted_;
    std::uint64_t mismatches_ = 0;
};

// Times one group of book calls in every `every`, counting down in *countdown. When
// enabled is false nothing is timed.
struct Timer {
    LatencyHistogram* ticks;
    std::uint64_t* total_ticks;
    std::uint64_t* countdown;
    std::uint64_t every;
    bool enabled;

    template <class F>
    void operator()(F&& call) const {
        if (!enabled || --*countdown != 0) {
            call();
            return;
        }
        *countdown = every;
        const std::uint64_t start = TickClock::now();
        call();
        const std::uint64_t elapsed = TickClock::now() - start;
        ticks->record(elapsed);
        *total_ticks += elapsed;
    }
};

// Builds one book per selected stock, sized from the stats file, inside an
// ItchBookBuilder that does the ITCH-to-book translation.
// Accepts every call and does nothing, for the null mode described at the top.
class NullBook {
public:
    NullBook(OrderBookListener& /*listener*/, BookConfig /*config*/) {}
    void submit(const OrderRequest& r) { checksum_ += r.id; }
    void cancel(OrderId id) { checksum_ += id; }
    void modify(OrderId id, Price /*p*/, Qty q) { checksum_ += id + q; }
    [[nodiscard]] std::size_t order_count() const { return 0; }
    [[nodiscard]] bool check_invariants() const { return true; }
    [[nodiscard]] std::uint64_t checksum() const { return checksum_; }

private:
    // A running sum of the arguments, so the compiler cannot drop the calls entirely.
    std::uint64_t checksum_ = 0;
};

template <class Book>
class Replayer {
public:
    // More than the 1.9 million orders that rest at the busiest moment of the sample day.
    static constexpr std::uint32_t kMaxLiveOrders = std::uint32_t{1} << 22U;

    Replayer(const std::vector<std::optional<StockInfo>>& stocks, bool ladder_only, bool timed,
             std::uint64_t sample_every)
        : countdown_(sample_every),
          builder_(kMaxLiveOrders, Timer{.ticks = &ticks_,
                                         .total_ticks = &total_ticks_,
                                         .countdown = &countdown_,
                                         .every = sample_every,
                                         .enabled = timed}) {
        for (std::size_t locate = 0; locate < stocks.size(); ++locate) {
            const std::optional<StockInfo>& s = stocks[locate];
            if (!s || (ladder_only && !s->fits_ladder())) {
                continue;
            }
            builder_.add_book(static_cast<std::uint16_t>(locate), health_, config_for(*s));
            ++book_count_;
        }
    }

    [[nodiscard]] itch::ItchBookBuilder<Book, Timer>& builder() { return builder_; }
    [[nodiscard]] const HealthListener& health() const { return health_; }
    [[nodiscard]] const LatencyHistogram& histogram() const { return ticks_; }
    [[nodiscard]] std::uint64_t book_ticks() const { return total_ticks_; }
    [[nodiscard]] std::uint64_t book_messages() const { return builder_.book_messages(); }
    [[nodiscard]] std::size_t book_count() const { return book_count_; }
    [[nodiscard]] std::size_t live_left() const { return builder_.live_orders(); }
    [[nodiscard]] std::uint64_t dropped() const { return builder_.dropped(); }
    [[nodiscard]] const std::vector<std::unique_ptr<Book>>& books() const {
        return builder_.books();
    }

    // At the end of the day every book should be consistent, and every book that was
    // replayed all day should be empty. A suspended book keeps what it held at its halt.
    [[nodiscard]] std::pair<std::size_t, std::size_t> end_of_day_check() const {
        std::size_t non_empty = 0;
        std::size_t broken = 0;
        const auto& books = builder_.books();
        for (std::size_t locate = 0; locate < books.size(); ++locate) {
            if (!books[locate]) {
                continue;
            }
            const bool all_day = builder_.replaying(static_cast<std::uint16_t>(locate));
            non_empty += all_day && books[locate]->order_count() != 0 ? 1U : 0U;
            broken += books[locate]->check_invariants() ? 0U : 1U;
        }
        return {non_empty, broken};
    }
    [[nodiscard]] std::size_t suspended() const { return builder_.suspended_stocks(); }

private:
    static BookConfig config_for(const StockInfo& s) {
        // Some slack above the peak, so that BookFull can only happen if the replay
        // drifted from Nasdaq's book, and then the health check reports it.
        const std::uint32_t capacity = s.peak + s.peak / 2 + 16;
        if constexpr (std::is_same_v<Book, FastOrderBook>) {
            const Price tick = s.tick();
            return {.tick_size = tick,
                    .min_price = static_cast<Price>(s.min_price) / tick * tick,
                    .max_price = (static_cast<Price>(s.max_price) + tick - 1) / tick * tick,
                    .max_orders = capacity};
        } else {
            return {.max_orders = capacity};
        }
    }

    HealthListener health_;
    LatencyHistogram ticks_;
    std::uint64_t total_ticks_ = 0;
    std::uint64_t countdown_;
    std::size_t book_count_ = 0;
    itch::ItchBookBuilder<Book, Timer> builder_;  // last, so the pointers above outlive it
};

// CPU time used by this process so far, user plus system, in seconds. The gunzip that
// feeds the pipe is a separate process and is not included.
double process_cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& t) {
        return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * 1e-6;
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

struct Options {
    bool ladder_only = false;
    bool untimed = false;
    std::uint64_t sample_every = 64;
};

// Times a call that does nothing many times, the same way the replay times the book. The
// result is the smallest reading the timer can give, which is part of every sample.
LatencyHistogram timer_floor() {
    LatencyHistogram floor;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint64_t start = TickClock::now();
        const std::uint64_t elapsed = TickClock::now() - start;
        floor.record(elapsed);
    }
    return floor;
}

template <class Book>
int run(const std::vector<std::optional<StockInfo>>& stocks, const char* mode, Options options) {
    const bool verify = std::is_same_v<Book, VerifyBook>;
    const bool ladder_only = options.ladder_only;
    const bool timed = !verify && !options.untimed;
    const auto wall_start = std::chrono::steady_clock::now();
    Replayer<Book> replay(stocks, ladder_only, timed, options.sample_every);
    const double setup_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();

    itch::BinaryFileReader reader(stdin, std::size_t{1} << 24U);
    std::span<const std::byte> payload;
    const double cpu_start = process_cpu_seconds();
    std::uint64_t messages = 0;
    itch::BinaryFileReader::Status status{};
    while ((status = reader.next(payload)) == itch::BinaryFileReader::Status::Message) {
        ++messages;
        (void)itch::parse_message(payload, replay.builder());
    }
    const double loop_cpu_s = process_cpu_seconds() - cpu_start;
    const double wall_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();

    const HealthListener& h = replay.health();
    const auto [non_empty, broken] = replay.end_of_day_check();
    std::printf(
        "mode %s%s: %zu books, built in %.1f s; %zu stocks stopped at a halt, pause "
        "or quotation period\n",
        mode, ladder_only ? " (ladder stocks only)" : "", replay.book_count(), setup_s,
        replay.suspended());
    std::printf("ITCH messages read: %llu, messages sent to a book: %llu, wall time %.1f s\n",
                static_cast<unsigned long long>(messages),
                static_cast<unsigned long long>(replay.book_messages()), wall_s);
    std::printf(
        "CPU time of this process for the message loop (reading, decoding, replaying): "
        "%.1f s\n",
        loop_cpu_s);
    std::printf(
        "health: trades %llu, rejects %llu, book-full cancels %llu (all must be 0); "
        "events %llu\n",
        static_cast<unsigned long long>(h.trades), static_cast<unsigned long long>(h.rejected),
        static_cast<unsigned long long>(h.book_full), static_cast<unsigned long long>(h.events));
    std::printf(
        "end of day: books not empty %zu, books failing invariants %zu, orders still "
        "tracked %zu, orders dropped for lack of room %llu\n",
        non_empty, broken, replay.live_left(), static_cast<unsigned long long>(replay.dropped()));
    const bool healthy = h.trades == 0 && h.rejected == 0 && h.book_full == 0 && non_empty == 0 &&
                         broken == 0 && status != itch::BinaryFileReader::Status::Truncated;
    const std::uint64_t mismatches = [&]() -> std::uint64_t {
        if constexpr (std::is_same_v<Book, VerifyBook>) {
            std::uint64_t sum = 0;
            for (const auto& b : replay.books()) {
                sum += b ? b->mismatches() : 0;
            }
            return sum;
        } else {
            return 0;
        }
    }();

    if constexpr (std::is_same_v<Book, VerifyBook>) {
        std::printf("map vs sorted: messages with different events: %llu\n",
                    static_cast<unsigned long long>(mismatches));
    } else if (!timed) {
        std::printf("untimed: no per-message timer reads\n");
    } else {
        const double tick = TickClock::tick_ns();
        const LatencyHistogram& hist = replay.histogram();
        const auto samples = static_cast<double>(hist.count());
        const double sampled_ns = static_cast<double>(replay.book_ticks()) * tick / samples;
        std::printf(
            "timed one message in %llu: %.0f samples, %.1f ns per timed message on average\n",
            static_cast<unsigned long long>(options.sample_every), samples, sampled_ns);
        std::printf("per timed message (ns, 1 tick = %.2f ns):", tick);
        for (const double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
            std::printf("  p%g %.0f", p, static_cast<double>(hist.percentile(p)) * tick);
        }
        std::printf("  max %.0f\n", static_cast<double>(hist.max()) * tick);
        const LatencyHistogram floor = timer_floor();
        std::printf("timer floor (ns, an empty call timed the same way): p50 %.0f  p99 %.0f\n",
                    static_cast<double>(floor.percentile(50.0)) * tick,
                    static_cast<double>(floor.percentile(99.0)) * tick);
    }
    const bool ok = healthy && mismatches == 0;
    std::printf("%s\n", ok ? "RESULT: OK" : "RESULT: PROBLEM");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) try {
    if (argc < 3) {
        std::fprintf(
            stderr,
            "usage: litbook_itch_replay sizes.csv map|sorted|fast|null|verify [--ladder-stocks] "
            "[--untimed] [--sample-every N]\n");
        return 2;
    }
    const auto stocks = load_sizes(argv[1]);
    if (stocks.empty()) {
        std::fprintf(stderr, "could not read %s\n", argv[1]);
        return 2;
    }
    const std::string_view mode = argv[2];
    Options options;
    for (int i = 3; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (flag == "--ladder-stocks") {
            options.ladder_only = true;
        } else if (flag == "--untimed") {
            options.untimed = true;
        } else if (flag == "--sample-every" && i + 1 < argc) {
            options.sample_every = std::strtoull(argv[++i], nullptr, 10);
            if (options.sample_every == 0) {
                std::fprintf(stderr, "--sample-every needs a number of at least 1\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (mode == "map") {
        return run<MapOrderBook>(stocks, argv[2], options);
    }
    if (mode == "sorted") {
        return run<SortedOrderBook>(stocks, argv[2], options);
    }
    if (mode == "fast") {
        options.ladder_only = true;
        return run<FastOrderBook>(stocks, argv[2], options);
    }
    if (mode == "null") {
        return run<NullBook>(stocks, argv[2], options);
    }
    if (mode == "verify") {
        return run<VerifyBook>(stocks, argv[2], options);
    }
    std::fprintf(stderr, "unknown mode %s\n", argv[2]);
    return 2;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
}
