#include "litbook/engine/engine.hpp"

#include <ostream>

namespace litbook {

std::ostream& operator<<(std::ostream& os, EngineStatus status) {
    switch (status) {
        case EngineStatus::Ok:
            return os << "Ok";
        case EngineStatus::UnknownSymbol:
            return os << "UnknownSymbol";
    }
    return os << "EngineStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook
