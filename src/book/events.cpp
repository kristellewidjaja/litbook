#include "litbook/book/events.hpp"

#include "litbook/book/recording_listener.hpp"
#include "litbook/types.hpp"

#include <ostream>
#include <variant>

namespace litbook {

std::ostream& operator<<(std::ostream& os, Side side) {
    return os << (side == Side::Buy ? "Buy" : "Sell");
}

std::ostream& operator<<(std::ostream& os, RejectReason reason) {
    switch (reason) {
        case RejectReason::DuplicateOrderId:
            return os << "DuplicateOrderId";
        case RejectReason::ZeroQuantity:
            return os << "ZeroQuantity";
        case RejectReason::UnknownOrderId:
            return os << "UnknownOrderId";
    }
    return os << "RejectReason(" << static_cast<int>(reason) << ")";
}

std::ostream& operator<<(std::ostream& os, const OrderAccepted& e) {
    return os << "Accepted{id=" << e.order_id << ' ' << e.side << ' ' << e.qty << '@' << e.price
              << '}';
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
    return os << "Canceled{id=" << e.order_id << " qty=" << e.canceled_qty << '}';
}

std::ostream& operator<<(std::ostream& os, const CancelRejected& e) {
    return os << "CancelRejected{id=" << e.order_id << ' ' << e.reason << '}';
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
