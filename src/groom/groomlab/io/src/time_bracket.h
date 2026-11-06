// SPDX-License-Identifier: Apache-2.0
#pragma once

// The two samples around a time, for blending animated Alembic properties.

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <Alembic/AbcCoreAbstract/TimeSampling.h>

namespace groomlab {

struct TimeBracket {
    Alembic::AbcCoreAbstract::index_t a = 0;
    Alembic::AbcCoreAbstract::index_t b = 0;
    double t = 0.0;  // 0: sample a, 1: sample b
    bool blends() const { return a != b && t > 0.0; }
};

inline TimeBracket timeBracket(const Alembic::AbcCoreAbstract::TimeSamplingPtr& sampling, std::size_t sampleCount,
                               double seconds) {
    TimeBracket r;
    if (sampleCount <= 1) return r;
    const auto n = static_cast<Alembic::AbcCoreAbstract::index_t>(sampleCount);
    const auto floor = sampling->getFloorIndex(seconds, n);
    const auto ceil = sampling->getCeilIndex(seconds, n);
    r.a = floor.first;
    r.b = ceil.first;
    // On a sample (up to rounding), use it as is.
    constexpr double kEpsilon = 1e-7;
    if (std::abs(ceil.second - seconds) < kEpsilon) {
        r.a = r.b;
    } else if (std::abs(floor.second - seconds) < kEpsilon) {
        r.b = r.a;
    }
    if (r.a != r.b && ceil.second > floor.second) {
        r.t = std::clamp((seconds - floor.second) / (ceil.second - floor.second), 0.0, 1.0);
    }
    return r;
}

}  // namespace groomlab
