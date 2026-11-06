// SPDX-License-Identifier: Apache-2.0
#include "groomlab/preview_core.h"

#include <algorithm>

namespace groomlab {

bool hasHiddenStrands(const StrandSet& strands) {
    const std::vector<float>* roles = strands.strandChannel(kPreviewChannel);
    return roles && std::any_of(roles->begin(), roles->end(), [](float r) { return r >= 1.5f; });
}

std::vector<std::size_t> coreStrands(const StrandSet& strands) {
    const PreviewCore core(strands);
    std::vector<std::size_t> out;
    out.reserve(strands.strandCount());
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        if (core.core(s)) out.push_back(s);
    }
    return out;
}

std::vector<std::uint32_t> shownIndices(const StrandSet& strands) {
    const std::vector<float>* roles = strands.strandChannel(kPreviewChannel);
    if (!roles || !hasHiddenStrands(strands)) return {};
    std::vector<std::uint32_t> at(roles->size(), UINT32_MAX);
    std::uint32_t next = 0;
    for (std::size_t s = 0; s < roles->size(); ++s) {
        if ((*roles)[s] < 1.5f) at[s] = next++;
    }
    return at;
}

bool sameOrShown(const StrandsPtr& made, const StrandsPtr& asked) {
    if (!made || !asked) return false;
    if (made == asked) return true;
    return made->strandCount() == asked->strandCount() && made->pointCount() == asked->pointCount() &&
           made->ids().data() == asked->ids().data() && made->positions().data() == asked->positions().data() &&
           made->strandChannel(kPreviewChannel) && !asked->strandChannel(kPreviewChannel);
}

StrandsPtr shownStrands(const StrandsPtr& strands) {
    if (!strands || !strands->strandChannel(kPreviewChannel)) return strands;
    std::shared_ptr<StrandSet> out;
    if (hasHiddenStrands(*strands)) {
        const std::vector<float>& roles = *strands->strandChannel(kPreviewChannel);
        std::vector<std::size_t> kept;
        kept.reserve(roles.size());
        for (std::size_t s = 0; s < roles.size(); ++s) {
            if (roles[s] < 1.5f) kept.push_back(s);
        }
        out = std::make_shared<StrandSet>(strands->subset(kept));
    } else {
        out = std::make_shared<StrandSet>(*strands);  // its arrays shared
    }
    out->removeStrandChannel(kPreviewChannel);
    return out;
}

}  // namespace groomlab
