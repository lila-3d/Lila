// SPDX-License-Identifier: Apache-2.0
// Semantic versions as the update checks compare them (semver.h): the app's and Maya's.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "groomlab/semver.h"

namespace groomlab {
namespace {

TEST(SemVer, VersionsSortAsSemanticVersions) {
    const std::vector<std::string> older = {"0.1.0", "0.1.1", "0.2.0-alpha", "0.2.0-alpha.1", "0.2.0-alpha.beta", "0.2.0-beta.2",
                                            "0.2.0-beta.10", "0.2.0-beta.11", "0.2.0-rc.1", "0.2.0", "0.9.9", "0.10.0", "1.0.0"};
    for (std::size_t i = 0; i + 1 < older.size(); ++i) {
        EXPECT_EQ(compareVersions(older[i], older[i + 1]), -1) << older[i] << " < " << older[i + 1];
        EXPECT_EQ(compareVersions(older[i + 1], older[i]), 1) << older[i + 1] << " > " << older[i];
    }
    // beta.10 after beta.2 (numerically, not as text).
    EXPECT_GT(compareVersions("1.0.0-beta.10", "1.0.0-beta.2"), 0);
    // Missing parts are 0; a "v" prefix and "+build" metadata are ignored.
    EXPECT_EQ(compareVersions("1.2", "1.2.0"), 0);
    EXPECT_EQ(compareVersions("v1.2.0", "1.2.0+build.5"), 0);
    EXPECT_EQ(compareVersions(" v1.3.0-beta.2+exp.sha.5114f85 ", "1.3.0-beta.2"), 0);
    EXPECT_GT(compareVersions("v1.10.0", "1.9.0"), 0);
    EXPECT_EQ(compareVersions("1.0.0-01", "1.0.0-1"), 0);  // numeric identifiers by value
    EXPECT_GT(compareVersions("12345678901234567890.0", "9.0"), 0);  // however long
}

TEST(SemVer, OnlyVersionsParse) {
    for (const char* bad : {"", "latest", "1.", ".1", "1..2", "1.2.x", "1.2.0-", "1.2.0+", "1.2.0-beta..1", "vv1.0", "1.0 beta"}) {
        EXPECT_FALSE(parseSemVer(bad).has_value()) << bad;
    }
    EXPECT_THROW(compareVersions("1.0", ""), std::invalid_argument);
    const auto v = parseSemVer("v01.2.0-rc.1+b7");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->numbers, (std::vector<std::string>{"1", "2"}));
    EXPECT_EQ(v->prerelease, (std::vector<std::string>{"rc", "1"}));
}

}  // namespace
}  // namespace groomlab
