// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace groomlab {

// Curve over [0, 1], used for "along the length" parameters (clump profile,
// noise falloff, ...). Keys are (position, value) sorted by position; values
// outside the first and last key are held constant.
//
// Between keys the curve is a monotone cubic (Fritsch-Butland tangents): it
// passes through every key smoothly, never overshoots, and flattens at peaks
// and valleys, so profiles don't put kinks into the hair. Two keys give a
// straight line. `straight` switches to straight segments.
struct Ramp {
    std::vector<std::pair<float, float>> keys;
    bool straight = false;  // straight segments instead of the smooth curve

    Ramp() = default;
    Ramp(std::initializer_list<std::pair<float, float>> k) : keys(k) { sort(); }

    static Ramp constant(float v) { return Ramp{{0.0f, v}}; }
    static Ramp linear(float root, float tip) { return Ramp{{0.0f, root}, {1.0f, tip}}; }

    void sort();
    float operator()(float t) const;
    // The monotone cubic's tangent (slope) at key k: the GPU preview takes them with the keys (0
    // with fewer than two keys).
    float tangent(std::size_t k) const;
    // Every key's tangent, and the ramp at t from them (`tangents`: as tangents() lists them): the
    // same value as operator(), for a ramp taken at many places without working them out each time.
    std::vector<float> tangents() const;
    float at(float t, const float* tangents) const;

    bool operator==(const Ramp& o) const { return keys == o.keys && straight == o.straight; }
    bool operator!=(const Ramp& o) const { return !(*this == o); }
};

}  // namespace groomlab
