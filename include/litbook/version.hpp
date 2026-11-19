#pragma once

#include <string_view>

namespace litbook {

// Returns the library version string, for example "0.0.1". It is never empty and always
// matches the project VERSION in the top-level CMakeLists.txt.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace litbook
