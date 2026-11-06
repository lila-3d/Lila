// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
#include "groomlab/sim_cache.h"

#include <memory>
#include <stdexcept>

#include "groomlab/alembic_io.h"
#include "groomlab/frame_time.h"

namespace groomlab {

BakedSimulationPtr readSimulationCache(const std::string& path, const std::string& object, std::uint64_t stamp) {
    auto reader = std::make_shared<const AlembicCurvesReader>(path, object);
    auto baked = std::make_shared<BakedSimulation>();
    baked->file = path;
    baked->stamp = stamp;
    if (!reader->timeRange(&baked->firstSeconds, &baked->lastSeconds)) {
        baked->firstSeconds = -1e9;  // one sample: every time reads it
        baked->lastSeconds = 1e9;
    }
    baked->read = [reader](double seconds) {
        const StrandSet curves = reader->at(seconds);
        const std::vector<float>* wetness = curves.pointChannel(kWetnessChannel);
        auto guides = std::make_shared<GuideSet>();
        for (std::size_t s = 0; s < curves.strandCount(); ++s) {
            const Vec3f* p = curves.strandPoints(s);
            const std::size_t n = curves.strandPointCount(s);
            Guide& g = guides->add(curves.ids()[s], std::vector<Vec3f>(p, p + n));
            if (!wetness) continue;
            const auto at = wetness->begin() + static_cast<std::ptrdiff_t>(curves.strandBegin(s));
            g.pointAttributes[kWetnessChannel].assign(at, at + static_cast<std::ptrdiff_t>(n));
        }
        return GuidesPtr(guides);
    };
    return baked;
}

}  // namespace groomlab
