// SPDX-License-Identifier: Apache-2.0
#include "groomlab/render_overrides.h"

#include <algorithm>
#include <cmath>

#include "groomlab/parallel.h"

namespace groomlab {

float RenderOverrides::densityTimes(float density0) const {
    const float own = std::isnan(density) ? 1.0f : std::clamp(density, 0.0f, 1.0f);
    return own * density0;
}

StrandsPtr applyRenderOverrides(const StrandsPtr& strands, const RenderOverrides& overrides) {
    if (!strands || strands->strandCount() == 0 || overrides.keepsShape()) return strands;
    const float width = std::isfinite(overrides.widthScale) ? std::max(0.0f, overrides.widthScale) : 1.0f;
    const float length = std::isfinite(overrides.lengthScale) ? std::max(0.0f, overrides.lengthScale) : 1.0f;
    auto out = std::make_shared<StrandSet>(*strands);
    if (width != 1.0f) {
        std::vector<float>& widths = out->mutableWidths();
        parallelFor(0, widths.size(), [&](std::size_t i) { widths[i] *= width; }, 65536);
    }
    if (length != 1.0f) {
        out->mutablePositions();  // cloned once: the strands are written side by side
        parallelFor(0, out->strandCount(), [&](std::size_t s) {
            const std::size_t n = out->strandPointCount(s);
            if (n < 2) return;
            Vec3f* pts = out->strandPoints(s);
            const Vec3f root = pts[0];
            for (std::size_t k = 1; k < n; ++k) pts[k] = root + (pts[k] - root) * length;
        });
    }
    return out;
}

}  // namespace groomlab
