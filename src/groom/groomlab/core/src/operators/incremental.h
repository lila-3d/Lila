// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers for operators that evaluate incrementally (Operator::evaluateIncremental, graph.h):
// the node's last result is copied array by array and only the strands its inputs changed in
// are made again, each exactly as a full evaluation makes it.

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "groomlab/graph.h"
#include "groomlab/parallel.h"

namespace groomlab {

// The strands input `slot` changed in, when an operator can follow them: known and not all of
// them (the same strands, IDs, roots, point counts and channels as the value the last result was
// made from). Null otherwise.
inline const StrandChanges* someStrandsChanged(const IncrementalInputs& in, std::size_t slot) {
    if (slot >= in.inputs.size() || !in.inputs[slot] || in.inputs[slot]->all) return nullptr;
    return in.inputs[slot].get();
}

// True when input `slot` is the value the last result was made from, or one equal to it.
inline bool inputUnchanged(const IncrementalInputs& in, std::size_t slot) {
    return slot < in.inputs.size() && in.inputs[slot] && in.inputs[slot]->none();
}

// True when input `slot` holds the very points the last result was made from (unchanged, or only
// its widths and channels changed: StrandChanges::pointsKept), so an operator whose points depend
// on its input's points alone keeps its last ones.
inline bool inputPointsKept(const IncrementalInputs& in, std::size_t slot) {
    return slot < in.inputs.size() && in.inputs[slot] && (in.inputs[slot]->none() || in.inputs[slot]->pointsKept);
}

// The input's widths and channels on the last result's points: a copy of `input` sharing every
// array of it but the positions, which are `last`'s very array (the two hold the same strands:
// inputPointsKept). For an operator that only moves points, by its input's points alone.
inline StrandsPtr withPointsOf(const StrandSet& input, const StrandSet& last) {
    auto out = std::make_shared<StrandSet>(input);
    out->sharePositionsOf(last);
    return out;
}

// The node's last result as strands, when it holds as many strands and points as `input` (the
// input's changes keep its layout, so the result made from it before had this one's).
inline StrandsPtr previousStrandsFor(const IncrementalInputs& in, const StrandSet& input) {
    StrandsPtr previous = valueAs<StrandsPtr>(in.previous);
    if (!previous || previous->strandCount() != input.strandCount() || previous->pointCount() != input.pointCount()) {
        return nullptr;
    }
    return previous;
}

// The last result's points (or widths) into `out`, a set holding the same strands, for the
// strands an incremental evaluation leaves alone: the last result's very array when the graph let
// go of it (IncrementalInputs::sole) and `last` is its only other holder (nobody else can read it),
// else a copy. The last result's points (widths) are not read after this.
inline std::vector<Vec3f>& positionsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in) {
    if (in.sole && last.use_count() == 2) return out.takePositionsFrom(const_cast<StrandSet&>(*last));
    return out.copyPositionsFrom(*last);
}
inline std::vector<float>& widthsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in) {
    if (in.sole && last.use_count() == 2) return out.takeWidthsFrom(const_cast<StrandSet&>(*last));
    return out.copyWidthsFrom(*last);
}

// The last result as a set of its own, for an operator whose result is laid out as its last one
// but not as its input (a subset, a merge): every array shared with it, except its points and
// widths, which are its very arrays when the graph let go of it (positionsOfLast), else copies.
inline std::shared_ptr<StrandSet> copyOfLast(const StrandsPtr& last, const IncrementalInputs& in) {
    StrandSet arrays;
    positionsOfLast(arrays, last, in);
    widthsOfLast(arrays, last, in);
    auto out = std::make_shared<StrandSet>(*last);
    out->takePositionsFrom(arrays);
    out->takeWidthsFrom(arrays);
    return out;
}

// Strand k of `from` written over strand at[k] of `out` (each the same number of points; `out`
// holding every channel `from` has, with what it lacks left as it is): its points, widths and
// channel values. A channel is taken for writing only where a value differs, so the arrays no
// strand changed in stay shared.
void writeStrands(StrandSet& out, const StrandSet& from, const std::vector<std::uint32_t>& at);

// body(s) for each strand listed, side by side.
template <class Body>
void forEachListed(const std::vector<std::uint32_t>& strands, const Body& body) {
    parallelFor(0, strands.size(), [&](std::size_t k) { body(static_cast<std::size_t>(strands[k])); }, 128);
}

// Strand s's points of `from` into `to` (sets of the same layout): a strand made again starts
// from its incoming points, as in a full evaluation's copy of them.
inline void copyStrandPoints(StrandSet& to, const StrandSet& from, std::size_t s) {
    const Vec3f* src = from.strandPoints(s);
    Vec3f* dst = to.strandPoints(s);
    for (std::size_t i = 0, n = from.strandPointCount(s); i < n; ++i) dst[i] = src[i];
}

}  // namespace groomlab
