#include "litbook/book/events.hpp"

#include "litbook/book/book_config.hpp"
#include "litbook/book/recording_listener.hpp"
#include "litbook/types.hpp"

#include <ostream>
#include <variant>

namespace litbook {

std::ostream& operator<<(std::ostream& os, Side side) {
    return os << (side == Side::Buy ? "Buy" : "Sell");
}

std::ostream& operator<<(std::ostream& os, OrderType type) {
    return os << (type == OrderType::Limit ? "Limit" : "Market");
}

std::ostream& operator<<(std::ostream& os, TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GoodTillCancel:
            return os << "GTC";
        case TimeInForce::ImmediateOrCancel:
            return os << "IOC";
        case TimeInForce::FillOrKill:
            return os << "FOK";
    }
    return os << "TimeInForce(" << static_cast<int>(tif) << ")";
}

std::ostream& operator<<(std::ostream& os, SelfTradePolicy policy) {
    switch (policy) {
        case SelfTradePolicy::CancelNewest:
            return os << "CancelNewest";
        case SelfTradePolicy::CancelOldest:
            return os << "CancelOldest";
        case SelfTradePolicy::CancelBoth:
            return os << "CancelBoth";
        case SelfTradePolicy::Decrement:
            return os << "Decrement";
    }
    return os << "SelfTradePolicy(" << static_cast<int>(policy) << ")";
}

std::ostream& operator<<(std::ostream& os, RejectReason reason) {
    switch (reason) {
        case RejectReason::DuplicateOrderId:
            return os << "DuplicateOrderId";
        case RejectReason::ZeroQuantity:
            return os << "ZeroQuantity";
        case RejectReason::UnknownOrderId:
            return os << "UnknownOrderId";
        case RejectReason::InvalidPrice:
            return os << "InvalidPrice";
        case RejectReason::InvalidLotSize:
            return os << "InvalidLotSize";
        case RejectReason::InvalidOrderType:
            return os << "InvalidOrderType";
        case RejectReason::PostOnlyWouldCross:
            return os << "PostOnlyWouldCross";
        case RejectReason::PriceOutOfRange:
            return os << "PriceOutOfRange";
    }
    return os << "RejectReason(" << static_cast<int>(reason) << ")";
}

std::ostream& operator<<(std::ostream& os, CancelReason reason) {
    switch (reason) {
        case CancelReason::UserRequested:
            return os << "UserRequested";
        case CancelReason::Unfilled:
            return os << "Unfilled";
        case CancelReason::SelfTradePrevention:
            return os << "SelfTradePrevention";
        case CancelReason::BookFull:
            return os << "BookFull";
    }
    return os << "CancelReason(" << static_cast<int>(reason) << ")";
}

std::ostream& operator<<(std::ostream& os, const OrderAccepted& e) {
    return os << "Accepted{id=" << e.order_id << " owner=" << e.owner << ' ' << e.side << ' '
              << e.type << ' ' << e.tif << (e.post_only ? " post-only " : " ") << e.qty << '@'
              << e.price << '}';
}

std::ostream& operator<<(std::ostream& os, const OrderRejected& e) {
    return os << "Rejected{id=" << e.order_id << ' ' << e.reason << '}';
}

std::ostream& operator<<(std::ostream& os, const Trade& e) {
    return os << "Trade{match=" << e.match_id << " aggr=" << e.aggressor_id << '('
              << e.aggressor_side << ") rest=" << e.resting_id << ' ' << e.qty << '@' << e.price
              << " aggr_left=" << e.aggressor_remaining << " rest_left=" << e.resting_remaining
              << '}';
}

std::ostream& operator<<(std::ostream& os, const OrderCanceled& e) {
    return os << "Canceled{id=" << e.order_id << " qty=" << e.canceled_qty
              << " left=" << e.remaining_qty << ' ' << e.reason << '}';
}

std::ostream& operator<<(std::ostream& os, const CancelRejected& e) {
    return os << "CancelRejected{id=" << e.order_id << ' ' << e.reason << '}';
}

std::ostream& operator<<(std::ostream& os, const OrderModified& e) {
    return os << "Modified{id=" << e.order_id << ' ' << e.side << ' ' << e.qty << '@' << e.price
              << (e.kept_priority ? " kept-priority" : " lost-priority") << '}';
}

std::ostream& operator<<(std::ostream& os, const ModifyRejected& e) {
    return os << "ModifyRejected{id=" << e.order_id << ' ' << e.reason << '}';
}

std::ostream& operator<<(std::ostream& os, const LevelUpdate& e) {
    return os << "Level{" << e.side << ' ' << e.price << " total=" << e.total_qty
              << " orders=" << e.order_count << '}';
}

std::ostream& operator<<(std::ostream& os, const BookEvent& event) {
    std::visit([&os](const auto& e) { os << e; }, event);
    return os;
}

}  // namespace litbook
