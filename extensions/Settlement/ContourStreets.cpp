// Settlement: hillside streets. Streets along the ground's contours with cross streets, switchbacks and stairs between
// them (terrainContourStreets on a Terrain ground, resolved by settlementCreate), then growth on the flatter ground.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>

namespace settlement
{
namespace
{
StreetClass contourClass(const std::string& kind)
{
    if (kind == "contour")
        return StreetClass::Collector;
    if (kind == "stairs")
        return StreetClass::Path;
    return StreetClass::Local;
}
} // namespace

void layContour(Layout& l, const Ground& g, const Options& o, std::span<const ContourStreet> streets, double limit)
{
    LILA_ZONE();

    if (!l.gen)
        l.gen = std::make_shared<GenLayout>();
    GenLayout& gen = *l.gen;
    if (!streets.empty())
    {
        for (const ContourStreet& s : streets)
        {
            std::vector<Vec2> pts;
            for (const glm::dvec3& p : s.pts)
                pts.push_back({p.x, p.z});
            const StreetClass cls = contourClass(s.kind);
            // Graded by the terrain already: contours hold their level, switchbacks their grade, stairs are steep by design.
            for (const std::vector<Vec2>& run : gen::clip(pts, l.outline, l.center, 0.0, limit, l.exclusions))
                for (const std::vector<Vec2>& piece : gen::dry(run, l.water))
                    l.graph.addPolyline(piece, cls, 0);
        }
    }
    else if (!g.isPlane())
    {
        gen.warnings.push_back("contour: no terrain contour streets (terrainContourStreets needs a Terrain ground); streets traced "
                               "along the contours of the settlement's ground grid instead");
        TensorSpec t;
        TensorBasis height;
        height.kind = "height";
        t.fields.push_back(height);
        TensorBasis grid;
        grid.angle = o.profile.gridAngle;
        grid.weight = 0.05;
        t.fields.push_back(grid);
        t.minorSpacing = std::max(o.profile.spacing, 1500.0);
        t.majorSpacing = 2.0 * t.minorSpacing;
        layTensor(l, g, o, t, limit);
    }
    else
        gen.warnings.push_back("contour: flat ground has no contours; the streets were grown organically instead");
    // Growth fills the flatter ground and links the hillside streets to the centre.
    Options grow = o;
    grow.growth = std::clamp(o.growth * (streets.empty() && g.isPlane() ? 1.0 : 0.5), 0.0, 1.0);
    growOrganic(l, g, grow, limit);
}
} // namespace settlement
