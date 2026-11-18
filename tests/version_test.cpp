#include "litbook/version.hpp"

#include <gtest/gtest.h>

namespace litbook {
namespace {

TEST(VersionTest, IsNotEmpty) {
    EXPECT_FALSE(version().empty());
}

TEST(VersionTest, MatchesProjectVersion) {
    EXPECT_EQ(version(), "0.0.1");
}

}  // namespace
}  // namespace litbook
