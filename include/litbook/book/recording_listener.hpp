#pragma once

#include "litbook/book/events.hpp"
#include "litbook/book/listener.hpp"

#include <iosfwd>
#include <utility>
#include <variant>
#include <vector>

namespace litbook {

using BookEvent = std::variant<OrderAccepted, OrderRejected, Trade, OrderCanceled, CancelRejected,
                               OrderModified, ModifyRejected, LevelUpdate>;

std::ostream& operator<<(std::ostream& os, const BookEvent& event);

// Listener that stores every event in arrival order. It is used by tests and by the
// differential test that compares two book implementations. Its invariant is that
// events() holds exactly the events received since construction or since the last
// take() or clear(), in order.
class RecordingListener final : public OrderBookListener {
public:
    void on_accepted(const OrderAccepted& e) override { events_.emplace_back(e); }
    void on_rejected(const OrderRejected& e) override { events_.emplace_back(e); }
    void on_trade(const Trade& e) override { events_.emplace_back(e); }
    void on_canceled(const OrderCanceled& e) override { events_.emplace_back(e); }
    void on_cancel_rejected(const CancelRejected& e) override { events_.emplace_back(e); }
    void on_modified(const OrderModified& e) override { events_.emplace_back(e); }
    void on_modify_rejected(const ModifyRejected& e) override { events_.emplace_back(e); }
    void on_level_update(const LevelUpdate& e) override { events_.emplace_back(e); }

    [[nodiscard]] const std::vector<BookEvent>& events() const noexcept { return events_; }

    // Returns the recorded events and starts a fresh recording.
    [[nodiscard]] std::vector<BookEvent> take() { return std::exchange(events_, {}); }

    void clear() noexcept { events_.clear(); }

private:
    std::vector<BookEvent> events_;
};

}  // namespace litbook
