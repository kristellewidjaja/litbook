#include "litbook/book/book_config.hpp"

#include <stdexcept>

namespace litbook {

void validate_config(const BookConfig& config) {
    if (config.tick_size < 1) {
        throw std::invalid_argument("BookConfig::tick_size must be >= 1");
    }
    if (config.lot_size < 1) {
        throw std::invalid_argument("BookConfig::lot_size must be >= 1");
    }
    if (config.min_price > config.max_price) {
        throw std::invalid_argument("BookConfig::min_price must be <= max_price");
    }
    if (config.max_orders < 1) {
        throw std::invalid_argument("BookConfig::max_orders must be >= 1");
    }
}

}  // namespace litbook
