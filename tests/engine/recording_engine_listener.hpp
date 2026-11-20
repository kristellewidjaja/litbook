#pragma once

#include "litbook/book/recording_listener.hpp"
#include "litbook/engine/engine_listener.hpp"

#include <vector>

namespace litbook::testing {

// One event from the engine together with the stock it came from.
struct EngineEvent {
    SymbolId symbol;
    BookEvent event;
    bool operator==(const EngineEvent&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const EngineEvent& e) {
    return os << "symbol " << e.symbol << ' ' << e.event;
}

// Stores every engine event in arrival order, for tests.
class RecordingEngineListener final : public EngineListener {
public:
    void on_accepted(SymbolId s, const OrderAccepted& e) override { add(s, e); }
    void on_rejected(SymbolId s, const OrderRejected& e) override { add(s, e); }
    void on_trade(SymbolId s, const Trade& e) override { add(s, e); }
    void on_canceled(SymbolId s, const OrderCanceled& e) override { add(s, e); }
    void on_cancel_rejected(SymbolId s, const CancelRejected& e) override { add(s, e); }
    void on_modified(SymbolId s, const OrderModified& e) override { add(s, e); }
    void on_modify_rejected(SymbolId s, const ModifyRejected& e) override { add(s, e); }
    void on_level_update(SymbolId s, const LevelUpdate& e) override { add(s, e); }

    [[nodiscard]] const std::vector<EngineEvent>& events() const noexcept { return events_; }
    void clear() noexcept { events_.clear(); }

private:
    void add(SymbolId s, const BookEvent& e) {
        events_.push_back(EngineEvent{.symbol = s, .event = e});
    }

    std::vector<EngineEvent> events_;
};

}  // namespace litbook::testing
