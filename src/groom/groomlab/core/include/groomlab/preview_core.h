// SPDX-License-Identifier: Apache-2.0
#pragma once

// The preview core: what keeps a thinned preview the render's strands, only fewer of them.
//
// A preview at density p shows the strands whose hashed ID falls under p (keepAtDensity), and
// every strand it shows must be exactly the render's strand of that ID. Nodes whose strands depend
// on other strands (a Clump picking its centers, a strand's neighbours, a clump's middle, a
// partner, the longest strand) would see other strands in a preview than in the render, so they
// read only the core: the strands every preview holds, at every density, the render included. A
// source thinned by the preview (Scatter) makes its core the strands whose hashed ID falls under
// its core share (coreShare: kCoreShare, more for a small scatter); a source never thinned
// (ToStrands, a Scatter with previewAll, imported curves) is core all through.
//
// Below its core share a preview still evaluates the whole core, and marks the core strands it
// does not show hidden: every node runs on them as on any strand (so the aggregates read the very
// strands the render's read), and the graph drops them from what it hands its callers
// (Graph::evaluate*, shownStrands). They are never shown, exported or counted.
//
// Per strand the role is kept in the strand channel kPreviewChannel (default kCoreShown, so strands
// joined from a source that has none are core): copies, subsets and joins carry it like any
// channel (a Mirror's copy is hidden when its strand is, a Split's outputs keep theirs). A set
// without the channel is core all through. The graph removes the channel from what it hands on.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "groomlab/portable_math.h"
#include "groomlab/random.h"
#include "groomlab/strand_set.h"

namespace groomlab {
using StrandsPtr = std::shared_ptr<const StrandSet>;
}

namespace groomlab {

// The share of a thinned source's strands that are core. 3%: under the viewport's default stroke
// preview (a preview density of 0.1 times a sculpt density of 0.35), so a default stroke holds no
// hidden strands (and keeps the GPU's stroke preview, which takes none); above it, a clump of 300
// on 500k strands has 50 core strands to pick from and average over.
constexpr float kCoreShare = 0.03f;
// A thinned source makes at least this many core strands (all of them when it makes fewer): a
// small groom's aggregates read enough strands, and a preview of it evaluates a few thousand
// strands more at most.
constexpr double kCoreLeast = 5000.0;

// The strand channel holding each strand's role in a preview (internal: the graph hands it on to no
// host). Its values:
constexpr const char* kPreviewChannel = "_preview";
constexpr float kShownOnly = 0.0f;   // shown, not core
constexpr float kCoreShown = 1.0f;   // core and shown (the default)
constexpr float kCoreHidden = 2.0f;  // core, held for the aggregates only: never shown

// The core share of a thinned source making `strands` strands at full density: a power of two
// (1, 1/2, 1/4...) leaving it kCoreLeast to twice that core strands, down to kCoreShare. In steps,
// so an edit of the scalp that changes the strands' count (its area) keeps the share, and with it
// the core everywhere but where the edit is, unless the count crosses a step (doubles or halves).
inline float coreShare(double strands) {
    if (!(strands > kCoreLeast)) return 1.0f;
    const double share = std::ldexp(1.0, -static_cast<int>(std::floor(pm::log2(strands / kCoreLeast))));
    return share > static_cast<double>(kCoreShare) ? static_cast<float>(share) : kCoreShare;
}

// The role of a strand of a thinned source (by its ID, as keepAtDensity), shown at `keep` and core
// under `core`; false when the preview holds it not at all.
inline bool previewRole(std::uint64_t id, float keep, float core, float* role) {
    const float h = unitFloat(hash64(id));
    const bool shown = h < keep, inCore = h < core;
    if (!shown && !inCore) return false;
    *role = shown ? (inCore ? kCoreShown : kShownOnly) : kCoreHidden;
    return true;
}

// The roles of a set's strands, as nodes read them.
class PreviewCore {
public:
    explicit PreviewCore(const StrandSet& strands) : roles_(strands.strandChannel(kPreviewChannel)) {}
    // Every strand core (no roles: a source never thinned).
    bool all() const { return roles_ == nullptr; }
    bool core(std::size_t s) const { return !roles_ || (*roles_)[s] >= 0.5f; }
    bool hidden(std::size_t s) const { return roles_ && (*roles_)[s] >= 1.5f; }

private:
    const std::vector<float>* roles_;
};

// Whether some strand of the set is hidden (held for the aggregates only).
bool hasHiddenStrands(const StrandSet& strands);
// The core strands' indices, in order (every strand without roles).
std::vector<std::size_t> coreStrands(const StrandSet& strands);
// What a caller of the graph is handed: the strands without the hidden ones and without the
// roles' channel; `strands` itself when it has no roles.
StrandsPtr shownStrands(const StrandsPtr& strands);
// For each strand of `strands`, its index among the shown ones (UINT32_MAX: hidden); empty when
// none is hidden.
std::vector<std::uint32_t> shownIndices(const StrandSet& strands);
// Whether `asked` is `made`, or what the graph handed on of it without hiding any strand (its
// arrays, shared): the same strands, in the same places. What operators' kept state (an
// Interpolate's binding, a Clump's choice: operator_state.h) answers for.
bool sameOrShown(const StrandsPtr& made, const StrandsPtr& asked);

}  // namespace groomlab
