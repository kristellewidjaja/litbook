#include "litbook/journal/replay.hpp"

#include <ostream>

namespace litbook::journal {

std::ostream& operator<<(std::ostream& os, ReplayStatus status) {
    switch (status) {
        case ReplayStatus::Ok:
            return os << "Ok";
        case ReplayStatus::MissingSessionStart:
            return os << "MissingSessionStart";
        case ReplayStatus::SecondSessionStart:
            return os << "SecondSessionStart";
        case ReplayStatus::SetupAfterRequests:
            return os << "SetupAfterRequests";
        case ReplayStatus::ConfigRefused:
            return os << "ConfigRefused";
        case ReplayStatus::SymbolRefused:
            return os << "SymbolRefused";
        case ReplayStatus::RiskLimitsRefused:
            return os << "RiskLimitsRefused";
        case ReplayStatus::JournalCorrupt:
            return os << "JournalCorrupt";
        case ReplayStatus::JournalUnreadable:
            return os << "JournalUnreadable";
        case ReplayStatus::SnapshotRefused:
            return os << "SnapshotRefused";
        case ReplayStatus::AlreadyStarted:
            return os << "AlreadyStarted";
    }
    return os << "ReplayStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook::journal
