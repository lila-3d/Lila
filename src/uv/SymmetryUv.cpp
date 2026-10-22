#include "uv/SymmetryUv.h"

#include "core/Profile.h"
#include "geo/Symmetry.h"
#include "uv/Layout.h"
#include "uv/Metrics.h"
#include "uv/Pack.h"
#include "uv/Seams.h"

#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace lila::uv
{
namespace
{
bool usable(const Chart& ch)
{
    double a = std::abs(ch.areaUv());
    return std::isfinite(a) && a > 1e-14 * std::max(1.0, ch.area3d()) && foldedTriangles(ch) == 0;
}

/** Seams of the faces copied across the plane, so the islands can pair up. Returns how many were added. */
Index mirrorSeams(geo::Mesh& mesh, const geo::SymmetryMap& map, std::span<const Index> faces)
{
    std::span<bool> seam = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    std::vector<Index> edges;
    for (Index f : faces)
        mesh.forEachFaceCorner(f, [&](Index c) {
            if (seam[mesh.cornerEdge(c)])
                edges.push_back(mesh.cornerEdge(c));
        });
    Index added = 0;
    for (Index e : map.mirrorEdges(mesh, edges))
        if (e != kInvalid && !seam[e])
        {
            seam[e] = true;
            ++added;
        }
    return added;
}

/** Mesh corner -> (chart, vertex). */
struct CornerIndex
{
    std::vector<Index> chart, vertex;

    CornerIndex(const geo::Mesh& mesh, std::span<const Chart> charts)
        : chart(mesh.cornerCapacity(), kInvalid), vertex(mesh.cornerCapacity(), kInvalid)
    {
        for (size_t c = 0; c < charts.size(); ++c)
            for (size_t k = 0; k < charts[c].corners.size(); ++k)
            {
                chart[charts[c].corners[k]] = Index(c);
                vertex[charts[c].corners[k]] = charts[c].cornerVertex[k];
            }
    }
};

/** The corner mirroring c: in the mirrored face, at the mirrored point. */
Index mirrorCorner(const geo::Mesh& mesh, const geo::SymmetryMap& map, Index c)
{
    Index f = map.mirrorFace(mesh, mesh.cornerFace(c));
    Index p = map.mirrorPoint(mesh.cornerPoint(c));
    if (f == kInvalid || p == kInvalid)
        return kInvalid;
    return mesh.findCorner(f, p);
}
} // namespace

double symmetrizeChart(Chart& ch, std::span<const Index> partner)
{
    const size_t V = ch.uv.size();
    if (V == 0 || partner.size() != V)
        return 0.0;
    glm::dvec2 mu(0.0);
    for (const glm::dvec2& p : ch.uv)
        mu += p;
    mu /= double(V);
    // Best mirror line through the centroid: maximise sum(y_w . Refl(theta) y_v).
    double A = 0.0, B = 0.0;
    for (size_t v = 0; v < V; ++v)
    {
        glm::dvec2 a = ch.uv[v] - mu, b = ch.uv[partner[v]] - mu;
        A += b.x * a.x - b.y * a.y;
        B += b.x * a.y + b.y * a.x;
    }
    const double theta = 0.5 * std::atan2(B, A);
    const double c2 = std::cos(2.0 * theta), s2 = std::sin(2.0 * theta);
    auto reflect = [&](const glm::dvec2& p) {
        glm::dvec2 y = p - mu;
        return mu + glm::dvec2(c2 * y.x + s2 * y.y, s2 * y.x - c2 * y.y);
    };
    std::vector<glm::dvec2> out(V);
    for (size_t v = 0; v < V; ++v)
        out[v] = 0.5 * (ch.uv[v] + reflect(ch.uv[partner[v]]));
    // Turn the mirror line upright, then make the pairs exact.
    const double turn = glm::half_pi<double>() - theta;
    const double ct = std::cos(turn), st = std::sin(turn);
    for (glm::dvec2& p : out)
    {
        glm::dvec2 y = p - mu;
        p = mu + glm::dvec2(ct * y.x - st * y.y, st * y.x + ct * y.y);
    }
    for (size_t v = 0; v < V; ++v)
    {
        Index w = partner[v];
        if (w == v)
            out[v].x = mu.x;
        else if (v < w)
            out[w] = glm::dvec2(2.0 * mu.x - out[v].x, out[v].y);
    }
    // Correction measured in the chart's own frame: compare against the original turned upright.
    double worst = 0.0;
    for (size_t v = 0; v < V; ++v)
    {
        glm::dvec2 y = ch.uv[v] - mu;
        glm::dvec2 turned = mu + glm::dvec2(ct * y.x - st * y.y, st * y.x + ct * y.y);
        worst = std::max(worst, glm::length(turned - out[v]));
    }
    ch.uv = std::move(out);
    return worst;
}

SymmetryUvResult unwrapSymmetric(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const SymmetryUvOptions& opts)
{
    LILA_ZONE();

    SymmetryUvResult res;
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty())
        return res;
    const int axis = std::clamp(opts.axis, 0, 2);
    const double tol = std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-5);
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, axis, tol);

    if (opts.mirrorSeams)
    {
        // Opening closed parts can cut asymmetrically; mirror and re-open until it settles.
        mirrorSeams(mesh, map, faces);
        for (int round = 0; round < 3; ++round)
        {
            Index cuts = cutToDisks(mesh, faces);
            res.seamsAdded += cuts;
            if (cuts == 0 || mirrorSeams(mesh, map, faces) == 0)
                break;
        }
    }
    else
        res.seamsAdded = cutToDisks(mesh, faces);

    ChartOptions co;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    res.islands = charts.size();
    CornerIndex index(mesh, charts);

    // Partner chart per chart (itself when it straddles the plane), and vertex partners.
    std::vector<Index> partnerChart(charts.size(), kInvalid);
    std::vector<std::vector<Index>> partnerVertex(charts.size());
    for (size_t c = 0; c < charts.size(); ++c)
    {
        const Chart& ch = charts[c];
        Index other = kInvalid;
        std::vector<Index> pv(ch.pos.size(), kInvalid);
        bool ok = true;
        for (size_t k = 0; k < ch.corners.size() && ok; ++k)
        {
            Index mc = mirrorCorner(mesh, map, ch.corners[k]);
            if (mc == kInvalid || index.chart[mc] == kInvalid)
            {
                ok = false;
                break;
            }
            if (other == kInvalid)
                other = index.chart[mc];
            Index& slot = pv[ch.cornerVertex[k]];
            ok = index.chart[mc] == other && (slot == kInvalid || slot == index.vertex[mc]);
            slot = index.vertex[mc];
        }
        if (ok && other != kInvalid && charts[other].pos.size() == ch.pos.size())
        {
            partnerChart[c] = other;
            partnerVertex[c] = std::move(pv);
        }
    }

    // Drivers: one of each pair (the positive side), straddling islands and loners.
    enum class Role : uint8_t
    {
        Alone,
        Self,
        Driver,
        Follower,
    };
    std::vector<Role> role(charts.size(), Role::Alone);
    for (size_t c = 0; c < charts.size(); ++c)
    {
        Index o = partnerChart[c];
        if (o == kInvalid || partnerChart[o] != Index(c))
            continue;
        if (o == Index(c))
        {
            role[c] = Role::Self;
            continue;
        }
        int side = map.majoritySide(charts[c].vertexPoint), otherSide = map.majoritySide(charts[o].vertexPoint);
        bool drive = side != otherSide ? side > 0 : c < size_t(o);
        role[c] = drive ? Role::Driver : Role::Follower;
    }

    std::vector<uint8_t> hadUvs(charts.size(), 0);
    std::vector<glm::dvec2> oldCentroid(charts.size());
    std::vector<double> oldArea(charts.size());
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t c) {
        Chart& ch = charts[c];
        hadUvs[c] = usable(ch) ? 1 : 0;
        oldCentroid[c] = ch.uvCentroid();
        oldArea[c] = std::abs(ch.areaUv());
        if (role[c] == Role::Follower)
            return;
        UnwrapOptions uo = opts.unwrap;
        uo.usePins = false;
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        unwrapChart(ch, uo);
        if (role[c] == Role::Self)
            symmetrizeChart(ch, partnerVertex[c]);
    });

    for (size_t c = 0; c < charts.size(); ++c)
    {
        res.pairs += role[c] == Role::Driver;
        res.selfSymmetric += role[c] == Role::Self;
        res.asymmetric += role[c] == Role::Alone;
        if (role[c] == Role::Follower || opts.pack || !hadUvs[c])
            continue;
        // A driver keeps the pair's old spot: its own centroid, or between the two when flipped.
        glm::dvec2 at = oldCentroid[c];
        double area = oldArea[c];
        if (role[c] == Role::Driver && hadUvs[partnerChart[c]])
            area = 0.5 * (oldArea[c] + oldArea[partnerChart[c]]);
        fitChart(charts[c], at, area);
    }

    auto mirrorFollowers = [&] {
        for (size_t c = 0; c < charts.size(); ++c)
        {
            if (role[c] != Role::Driver)
                continue;
            const Chart& d = charts[c];
            Chart& f = charts[partnerChart[c]];
            const std::vector<Index>& pv = partnerVertex[c];
            const double gap = 0.02 * std::max(d.uvMax().x - d.uvMin().x, d.uvMax().y - d.uvMin().y);
            const double axisU = d.uvMax().x + 0.5 * gap;
            for (size_t v = 0; v < d.uv.size(); ++v)
            {
                glm::dvec2 p = d.uv[v];
                f.uv[pv[v]] = opts.mode == MirrorUvMode::Stack ? p : glm::dvec2(2.0 * axisU - p.x, p.y);
            }
        }
    };
    mirrorFollowers();

    // Flipped partners pack as islands of their own; stacked ones ride on their driver's spot. Without pack,
    // only islands that had no uvs are packed. Packing never turns them, so mirror lines stay vertical.
    std::vector<Chart*> toPack;
    for (size_t c = 0; c < charts.size(); ++c)
    {
        const bool follower = role[c] == Role::Follower;
        if (follower && opts.mode == MirrorUvMode::Stack)
            continue;
        if (opts.pack || !hadUvs[follower ? partnerChart[c] : Index(c)])
            toPack.push_back(&charts[c]);
    }
    if (!toPack.empty())
    {
        std::vector<Chart> packed;
        for (Chart* p : toPack)
            packed.push_back(std::move(*p));
        PackOptions po;
        po.rotateStep = 0;
        po.preOrient = false;
        packCharts(packed, po);
        for (size_t i = 0; i < toPack.size(); ++i)
            *toPack[i] = std::move(packed[i]);
        if (opts.mode == MirrorUvMode::Stack)
            mirrorFollowers();
    }
    writeCharts(mesh, charts, uvSet);
    return res;
}
} // namespace lila::uv
