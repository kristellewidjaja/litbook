#pragma once

#include <optional>
#include <stdexcept>

namespace litbook::testing {

// Returns the value inside an optional the test expects to be full. If it is empty, it
// throws, and GoogleTest reports the throw as a failure of the running test. This is
// clearer than writing *opt after an ASSERT, which static analysis cannot follow.
template <class T>
T unwrap(const std::optional<T>& value) {
    if (!value.has_value()) {
        throw std::logic_error("expected a value, but the optional was empty");
    }
    return *value;
}

}  // namespace litbook::testing
