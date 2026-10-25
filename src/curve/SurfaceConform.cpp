#include "curve/SurfaceConform.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

namespace lila::curve
{
using D3 = glm::dvec3;

void conformToSurface(Spline& s, const geo::Mesh& mesh, const geo::MeshBvh& bvh, double offset)
{
    LILA_ZONE();

    if (bvh.empty())
        return;
    const bool bezier = s.kind == Kind::Bezier && s.handleL.size() == s.size() && s.handleR.size() == s.size();
    for (size_t i = 0; i < s.size(); ++i)
    {
        geo::BvhClosest c;
        if (!bvh.closestPoint(s.points[i], c) || c.face == geo::kInvalid || !mesh.faceValid(c.face))
            continue;
        D3 n = mesh.faceNormal(c.face);
        if (!(glm::dot(n, n) > 0.5))
            n = D3(0.0);
        const D3 p = c.point + n * offset;
        if (bezier)
            for (D3* h : {&s.handleL[i], &s.handleR[i]})
            {
                const D3 d = *h - s.points[i];
                const D3 flat = d - n * glm::dot(d, n);
                const double fl = glm::length(flat);
                *h = p + (fl > 1e-12 ? flat * (glm::length(d) / fl) : D3(0.0));
            }
        s.points[i] = p;
    }
}
} // namespace lila::curve
