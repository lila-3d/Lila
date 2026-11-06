// SPDX-License-Identifier: Apache-2.0
#pragma once

// Semantic versions, as the update checks compare them (the app's Help > Check for Updates, Maya's
// groomlab/updates.py): "1.2.0", "v1.3.0-beta.2", "1.2.0+build.5". One implementation for every
// host, so a release feed's version sorts the same everywhere.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace groomlab {

struct SemVer {
    // The dotted numbers, decimal digits without leading zeros, trailing zeros dropped ("1.2" is
    // "1.2.0"; "0" stays): compared numerically however long.
    std::vector<std::string> numbers;
    // The pre-release identifiers after "-" (none: a release, which sorts after its pre-releases).
    std::vector<std::string> prerelease;
};

// The version `text` names (surrounding spaces, a leading "v" and "+build" metadata allowed and
// ignored), or none when it is not one.
std::optional<SemVer> parseSemVer(std::string_view text);
// -1, 0 or 1 as `a` is older than, the same as or newer than `b`: the numbers part by part
// (missing parts 0), then a release after its pre-releases, the pre-release identifiers one by one
// (numeric ones numerically and below alphanumeric ones, those in ASCII order; fewer first when
// one list starts the other: beta < beta.2 < beta.10 < rc.1). Throws std::invalid_argument when
// either is not a version.
int compareVersions(std::string_view a, std::string_view b);

}  // namespace groomlab
