#include "litbook/book/order_book.hpp"

#include <ostream>

namespace litbook {

std::ostream& operator<<(std::ostream& os, RestoreStatus status) {
    switch (status) {
        case RestoreStatus::Ok:
            return os << "Ok";
        case RestoreStatus::BadPrice:
            return os << "BadPrice";
        case RestoreStatus::BadQuantity:
            return os << "BadQuantity";
        case RestoreStatus::DuplicateId:
            return os << "DuplicateId";
        case RestoreStatus::WouldCross:
            return os << "WouldCross";
        case RestoreStatus::BookFull:
            return os << "BookFull";
    }
    return os << "RestoreStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook
