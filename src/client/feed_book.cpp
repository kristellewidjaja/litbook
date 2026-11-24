#include "litbook/client/feed_book.hpp"

#include "litbook/engine/ticker.hpp"

#include <poll.h>

#include <array>
#include <chrono>
#include <stdexcept>

namespace litbook::client {

const char* to_string(SymbolStatus status) noexcept {
    switch (status) {
        case SymbolStatus::Unknown:
            return "unknown";
        case SymbolStatus::Live:
            return "live";
        case SymbolStatus::Stale:
            return "stale";
    }
    return "unknown";
}

namespace {

int checked_wait(int wait_ms) {
    if (wait_ms < 1) {
        throw std::invalid_argument("wait_ms must be at least 1");
    }
    return wait_ms;
}

}  // namespace

FeedBook::FeedBook(const FeedBookConfig& config)
    : receiver_(config.receiver),
      wait_ms_(checked_wait(config.wait_ms)),
      max_symbols_(config.receiver.handler.max_symbols),
      incremental_port_(receiver_.incremental().port()),
      snapshot_port_(receiver_.snapshot().port()) {
    // The thread starts last, once every member it reads is ready.
    reader_ = std::thread([this] { run(); });
}

FeedBook::~FeedBook() {
    stop();
}

void FeedBook::stop() {
    stopping_.store(true, std::memory_order_relaxed);
    if (reader_.joinable()) {
        reader_.join();
    }
}

void FeedBook::run() {
    std::array<pollfd, 2> fds{
        pollfd{.fd = receiver_.incremental().fd(), .events = POLLIN, .revents = 0},
        pollfd{.fd = receiver_.snapshot().fd(), .events = POLLIN, .revents = 0}};
    while (!stopping_.load(std::memory_order_relaxed)) {
        // The wait happens without the mutex, so callers are never held up by it. Only the
        // read below, which never blocks, holds the mutex.
        (void)::poll(fds.data(), fds.size(), wait_ms_);
        std::size_t packets = 0;
        {
            const std::scoped_lock lock(mutex_);
            // poll_once(0) also checks for silence when nothing arrived.
            packets = receiver_.poll_once(0);
            if (packets > 0) {
                ++version_;
            }
        }
        if (packets > 0) {
            changed_.notify_all();
        }
    }
}

std::optional<feed::Locate> FeedBook::find(std::string_view symbol) const {
    const std::optional<Ticker> ticker = make_ticker(symbol);
    if (!ticker) {
        return std::nullopt;
    }
    const feed::FeedHandler& handler = receiver_.handler();
    for (std::size_t i = 0; i < max_symbols_; ++i) {
        const auto locate = static_cast<feed::Locate>(i);
        const auto& directory = handler.directory(locate);
        if (directory && directory->symbol == *ticker) {
            return locate;
        }
    }
    return std::nullopt;
}

SymbolStatus FeedBook::status(std::string_view symbol) const {
    const std::scoped_lock lock(mutex_);
    const std::optional<feed::Locate> locate = find(symbol);
    if (!locate) {
        return SymbolStatus::Unknown;
    }
    return live(*locate) ? SymbolStatus::Live : SymbolStatus::Stale;
}

bool FeedBook::live(feed::Locate locate) const {
    // A receiver that joined late can learn a stock from the snapshot stream first. The
    // handler counts it as live then, but it only notices the updates it missed when the
    // first incremental packet shows the gap, so until then the book is not trusted.
    const feed::FeedHandler& handler = receiver_.handler();
    return handler.live(locate) && handler.stats().incremental_packets > 0;
}

std::optional<Quote> FeedBook::best(std::string_view symbol) const {
    const std::scoped_lock lock(mutex_);
    const std::optional<feed::Locate> locate = find(symbol);
    if (!locate) {
        return std::nullopt;
    }
    const feed::FeedHandler& handler = receiver_.handler();
    const feed::LevelBook* book = handler.book(*locate);
    return Quote{
        .bid = book->best(Side::Buy), .ask = book->best(Side::Sell), .live = live(*locate)};
}

std::vector<feed::PriceLevel> FeedBook::depth(std::string_view symbol, Side side,
                                              std::size_t levels) const {
    std::vector<feed::PriceLevel> out;
    const std::scoped_lock lock(mutex_);
    const std::optional<feed::Locate> locate = find(symbol);
    if (!locate) {
        return out;
    }
    const feed::LevelBook* book = receiver_.handler().book(*locate);
    const std::size_t n = std::min(levels, book->level_count(side));
    out.reserve(n);
    for (std::size_t rank = 0; rank < n; ++rank) {
        out.push_back(book->level(side, rank));
    }
    return out;
}

std::vector<std::string> FeedBook::symbols() const {
    std::vector<std::string> out;
    const std::scoped_lock lock(mutex_);
    const feed::FeedHandler& handler = receiver_.handler();
    for (std::size_t i = 0; i < max_symbols_; ++i) {
        const auto& directory = handler.directory(static_cast<feed::Locate>(i));
        if (directory) {
            out.push_back(directory->symbol.name());
        }
    }
    return out;
}

std::uint64_t FeedBook::version() const {
    const std::scoped_lock lock(mutex_);
    return version_;
}

std::uint64_t FeedBook::wait_update(std::uint64_t seen, int timeout_ms) const {
    std::unique_lock lock(mutex_);
    (void)changed_.wait_for(lock, std::chrono::milliseconds(std::max(timeout_ms, 0)),
                            [&] { return version_ > seen; });
    return version_;
}

FeedBookStats FeedBook::stats() const {
    const std::scoped_lock lock(mutex_);
    const feed::FeedHandler& handler = receiver_.handler();
    return FeedBookStats{.handler = handler.stats(),
                         .receiver = receiver_.stats(),
                         .version = version_,
                         .started = handler.started(),
                         .ended = handler.ended(),
                         .recovering = handler.recovering(),
                         .silent = receiver_.silent()};
}

}  // namespace litbook::client
