// SPDX-License-Identifier: Apache-2.0
#include "groomlab/ramp.h"

#include <algorithm>

#include "groomlab/shared/groom_math.h"

namespace groomlab {

void Ramp::sort() {
    std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

namespace {

// The ramp at t, `tangent(k)` key k's tangent (operator(), at).
template <class Tangent>
float rampAt(const Ramp& ramp, float t, const Tangent& tangent) {
    const auto& keys = ramp.keys;
    if (keys.empty()) return 1.0f;
    if (!(t > keys.front().first)) return keys.front().second;  // also NaN
    if (t >= keys.back().first) return keys.back().second;
    const auto hi = std::upper_bound(keys.begin(), keys.end(), t, [](float x, const auto& k) { return x < k.first; });
    const auto lo = hi - 1;
    const float span = hi->first - lo->first;
    if (span <= 0.0f) return hi->second;
    const float f = (t - lo->first) / span;
    if (ramp.straight) return lo->second + (hi->second - lo->second) * f;

    // Monotone cubic Hermite: tangents from the neighboring secants.
    const std::size_t i = static_cast<std::size_t>(lo - keys.begin());
    const float m0 = tangent(i) * span, m1 = tangent(i + 1) * span;
    return shared::rampHermite(f, lo->second, m0, hi->second, m1);
}

}  // namespace

float Ramp::operator()(float t) const {
    return rampAt(*this, t, [&](std::size_t k) { return tangent(k); });
}

std::vector<float> Ramp::tangents() const {
    std::vector<float> out(keys.size());
    for (std::size_t k = 0; k < keys.size(); ++k) out[k] = tangent(k);
    return out;
}

float Ramp::at(float t, const float* tangents) const {
    return rampAt(*this, t, [&](std::size_t k) { return tangents[k]; });
}

float Ramp::tangent(std::size_t k) const {
    auto secant = [&](std::size_t s) {  // slope of span s (keys s to s + 1)
        return shared::rampSecant(keys[s].first, keys[s].second, keys[s + 1].first, keys[s + 1].second);
    };
    if (keys.size() < 2) return 0.0f;
    if (k == 0) return secant(0);
    if (k + 1 == keys.size()) return secant(k - 1);
    const float h0 = keys[k].first - keys[k - 1].first, h1 = keys[k + 1].first - keys[k].first;
    return shared::rampInnerTangent(h0, h1, secant(k - 1), secant(k));
}

}  // namespace groomlab
