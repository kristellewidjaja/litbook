#pragma once

#include "litbook/book/events.hpp"

#include <cstdint>

namespace litbook {

// Small integer that names one stock inside the engine. The engine hands them out as
// 0, 1, 2, ... in the order symbols are added, so a SymbolId is also the index of the
// stock's book. ITCH uses a 16-bit "stock locate code" for the same idea.
using SymbolId = std::uint16_t;

// Receives every event from every book an Engine owns, tagged with the stock it came
// from. Within one call into the engine, events arrive in the book's order (see
// events.hpp). The only change the engine makes is to Trade::match_id, which it
// replaces with a number that is unique across all stocks (see engine.hpp).
//
// Like OrderBookListener, a listener must never call back into the engine while it is
// being notified, because the book that is reporting is in the middle of an operation.
class EngineListener {
public:
    virtual ~EngineListener() = default;

    virtual void on_accepted(SymbolId /*symbol*/, const OrderAccepted& /*event*/) {}
    virtual void on_rejected(SymbolId /*symbol*/, const OrderRejected& /*event*/) {}
    virtual void on_trade(SymbolId /*symbol*/, const Trade& /*event*/) {}
    virtual void on_canceled(SymbolId /*symbol*/, const OrderCanceled& /*event*/) {}
    virtual void on_cancel_rejected(SymbolId /*symbol*/, const CancelRejected& /*event*/) {}
    virtual void on_modified(SymbolId /*symbol*/, const OrderModified& /*event*/) {}
    virtual void on_modify_rejected(SymbolId /*symbol*/, const ModifyRejected& /*event*/) {}
    virtual void on_level_update(SymbolId /*symbol*/, const LevelUpdate& /*event*/) {}

protected:
    EngineListener() = default;
    EngineListener(const EngineListener&) = default;
    EngineListener(EngineListener&&) = default;
    EngineListener& operator=(const EngineListener&) = default;
    EngineListener& operator=(EngineListener&&) = default;
};

}  // namespace litbook
