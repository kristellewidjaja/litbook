#pragma once

#include <string_view>

namespace litbook {

// Library version string, e.g. "0.0.1".
// Invariant: never empty; matches the project VERSION in the top-level CMakeLists.txt.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace litbook
