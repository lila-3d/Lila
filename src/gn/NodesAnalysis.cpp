#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Bvh.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <numeric>
#include <queue>

namespace lila::gn::nodes
{
namespace analysis
{
using D3 = glm::dvec3;

/** A field computed from the mesh of the context it is evaluated in; anything but a mesh gets the type's zero. */
class SurfaceField final : public Field
{
public:
    using Fn = std::function<Column(const FieldContext&, const geo::Mesh&)>;
    SurfaceField(SocketType t, Fn fn) : Field(t), mFn(std::move(fn)) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Mesh || !ctx.mesh)
            return convert(Column::of(0.0), type());
        return convert(mFn(ctx, *ctx.mesh), type());
    }

private:
    Fn mFn;
};

FieldPtr surfaceField(SocketType t, SurfaceField::Fn fn) { return std::make_shared<SurfaceField>(t, std::move(fn)); }

/** Computed per point and moved to the domain it is read on. */
FieldPtr perPoint(SocketType t, std::function<Column(const geo::Mesh&)> fn)
{
    return surfaceField(t, [fn = std::move(fn)](const FieldContext& ctx, const geo::Mesh& m) {
        return adaptDomain(m, fn(m), Domain::Point, ctx.domain);
    });
}

std::vector<D3> smoothPointNormals(const geo::Mesh& m) { return evaluateAll<D3>(normalField(), FieldContext::forMesh(m, Domain::Point)); }

/** Per point mean and Gaussian curvature, positive where convex. */
struct Curvatures
{
    std::vector<double> mean, gaussian;
};

Curvatures curvatures(const geo::Mesh& m)
{
    LILA_ZONE();

    const Index n = m.pointCapacity();
    std::vector<D3> laplace(n, D3(0.0));
    std::vector<double> area(n, 0.0), angle(n, 0.0);
    auto cotan = [](const D3& a, const D3& b) {
        double s = glm::length(glm::cross(a, b));
        return s > 1e-15 ? glm::dot(a, b) / s : 0.0;
    };
    std::vector<Index> pts;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        pts.clear();
        m.forEachFaceCorner(f, [&](Index k) { pts.push_back(m.cornerPoint(k)); });
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const D3& p = m.position(pts[k]);
            D3 a = m.position(pts[(k + pts.size() - 1) % pts.size()]) - p, b = m.position(pts[(k + 1) % pts.size()]) - p;
            double la = glm::length(a), lb = glm::length(b);
            if (la > 1e-15 && lb > 1e-15)
                angle[pts[k]] += std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0, 1.0));
        }
        // Fan triangles: each corner angle weights the edge across from it.
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            Index t[3] = {pts[0], pts[k], pts[k + 1]};
            double a3 = glm::length(glm::cross(m.position(t[1]) - m.position(t[0]), m.position(t[2]) - m.position(t[0]))) / 6.0;
            for (int i = 0; i < 3; ++i)
            {
                Index o = t[i], u = t[(i + 1) % 3], v = t[(i + 2) % 3];
                double w = cotan(m.position(u) - m.position(o), m.position(v) - m.position(o));
                laplace[u] += w * (m.position(v) - m.position(u));
                laplace[v] += w * (m.position(u) - m.position(v));
                area[t[i]] += a3;
            }
        }
    }
    std::vector<D3> normals = smoothPointNormals(m);
    Curvatures c;
    c.mean.assign(n, 0.0);
    c.gaussian.assign(n, 0.0);
    for (Index p = 0; p < n; ++p)
    {
        if (!m.pointValid(p) || area[p] <= 1e-15)
            continue;
        double full = m.isBoundaryPoint(p) ? 3.14159265358979323846 : 2.0 * 3.14159265358979323846;
        c.gaussian[p] = (full - angle[p]) / area[p];
        c.mean[p] = -glm::dot(laplace[p] / (2.0 * area[p]), normals[p]) / 2.0;
    }
    return c;
}

void curvatureNode()
{
    registerDecl({
        .type = "Curvature",
        .label = "Curvature",
        .category = "Mesh Analysis",
        .doc = "Surface curvature per point, in 1 / scene units: mean (positive on convex bumps, negative in hollows), Gaussian (positive on "
               "domes, negative on saddles) and the principal curvatures.",
        .sockets = {.outputs = {fout("mean", T::Float), fout("gaussian", T::Float), fout("minimum", T::Float), fout("maximum", T::Float)}},
        .exec =
            [](ExecContext& c) {
                auto make = [](int which) {
                    return perPoint(T::Float, [which](const geo::Mesh& m) {
                        Curvatures k = curvatures(m);
                        if (which == 0)
                            return Column::of(std::move(k.mean));
                        if (which == 1)
                            return Column::of(std::move(k.gaussian));
                        std::vector<double> out(k.mean.size());
                        for (size_t p = 0; p < out.size(); ++p)
                        {
                            double d = std::sqrt(std::max(0.0, k.mean[p] * k.mean[p] - k.gaussian[p]));
                            out[p] = which == 2 ? k.mean[p] - d : k.mean[p] + d;
                        }
                        return Column::of(std::move(out));
                    });
                };
                c.out("mean", make(0));
                c.out("gaussian", make(1));
                c.out("minimum", make(2));
                c.out("maximum", make(3));
            },
    });
}

/** Points reached along edges from p without leaving a sphere of the radius around it, p excluded, capped at limit. */
void neighbourhood(const geo::Mesh& m, Index p, double radius, size_t limit, std::vector<Index>& out, std::vector<Index>& queue,
                   std::vector<uint32_t>& stamp, uint32_t mark)
{
    out.clear();
    queue.assign(1, p);
    stamp[p] = mark;
    const D3 centre = m.position(p);
    for (size_t head = 0; head < queue.size() && out.size() < limit; ++head)
        m.forEachPointEdge(queue[head], [&](Index e) {
            Index q = m.edgeOtherPoint(e, queue[head]);
            if (stamp[q] == mark)
                return;
            stamp[q] = mark;
            // The first ring always counts, so a radius under the edge length still sees something.
            if (head == 0 || glm::length(m.position(q) - centre) <= radius)
            {
                out.push_back(q);
                queue.push_back(q);
            }
        });
}

void cavityNode()
{
    registerDecl({
        .type = "Cavity",
        .label = "Cavity",
        .category = "Mesh Analysis",
        .doc = "How far each point sits below (cavity) or above (convexity) the average of the surface around it, 0 to 1. Larger radii pick up "
               "broader shapes. Good for dirt and edge-wear masks.",
        .sockets = {.inputs = {bounded(in("radius", T::Float, 10.0, "Distance around each point that counts as its surroundings."), 0.0),
                               bounded(in("strength", T::Float, 1.0, "Contrast of the result."), 0.0)},
                    .outputs = {fout("cavity", T::Float), fout("convexity", T::Float)}},
        .exec =
            [](ExecContext& c) {
                double radius = std::max(c.f("radius"), 1e-9), strength = c.f("strength");
                auto make = [radius, strength](bool convex) {
                    return perPoint(T::Float, [radius, strength, convex](const geo::Mesh& m) {
                        LILA_ZONE();
                        std::vector<D3> normals = smoothPointNormals(m);
                        std::vector<double> out(m.pointCapacity(), 0.0);
                        tbb::parallel_for(tbb::blocked_range<Index>(0, m.pointCapacity(), 256), [&](const tbb::blocked_range<Index>& r) {
                            std::vector<Index> ring, queue;
                            std::vector<uint32_t> stamp(m.pointCapacity(), 0);
                            uint32_t mark = 0;
                            for (Index p = r.begin(); p != r.end(); ++p)
                            {
                                if (!m.pointValid(p))
                                    continue;
                                neighbourhood(m, p, radius, 512, ring, queue, stamp, ++mark);
                                if (ring.empty())
                                    continue;
                                D3 mean(0.0);
                                for (Index q : ring)
                                    mean += m.position(q);
                                mean /= double(ring.size());
                                double v = 2.0 * strength * glm::dot(normals[p], mean - m.position(p)) / radius;
                                out[p] = std::clamp(convex ? -v : v, 0.0, 1.0);
                            }
                        });
                        return Column::of(std::move(out));
                    });
                };
                c.out("cavity", make(false));
                c.out("convexity", make(true));
            },
    });
}

/** Positions and normals of the context's elements, for nodes that cast rays from each of them. */
void elementFrames(const FieldContext& ctx, std::vector<D3>& pos, std::vector<D3>& nrm)
{
    pos = evaluateAll<D3>(positionField(), ctx);
    nrm = evaluateAll<D3>(normalField(), ctx);
}

double offsetFor(const geo::Mesh& m) { return std::max(1e-6, glm::length(m.bounds().size()) * 1e-5); }

void rayNodes()
{
    registerDecl({
        .type = "Thickness",
        .label = "Thickness",
        .category = "Mesh Analysis",
        .doc = "Distance through the mesh from each element straight back against its normal to the far side. Open areas with nothing "
               "behind them read the max distance.",
        .sockets = {.inputs = {bounded(in("maxDistance", T::Float, 1000.0), 0.0)}, .outputs = {fout("thickness", T::Float)}},
        .exec =
            [](ExecContext& c) {
                double maxDist = c.f("maxDistance");
                c.out("thickness", surfaceField(T::Float, [maxDist](const FieldContext& ctx, const geo::Mesh& m) {
                          LILA_ZONE();
                          geo::MeshBvh bvh(m);
                          std::vector<D3> pos, nrm;
                          elementFrames(ctx, pos, nrm);
                          const double eps = offsetFor(m);
                          std::vector<double> out(ctx.size, maxDist);
                          tbb::parallel_for(size_t(0), ctx.size, [&](size_t k) {
                              if (glm::length(nrm[k]) < 1e-12)
                                  return;
                              D3 dir = -glm::normalize(nrm[k]);
                              geo::BvhHit hit;
                              if (bvh.raycast(pos[k] + dir * eps, dir, hit, maxDist))
                                  out[k] = std::min(maxDist, hit.t + eps);
                          });
                          return Column::of(std::move(out));
                      }));
            },
    });
    registerDecl({
        .type = "AmbientOcclusion",
        .label = "Ambient Occlusion",
        .category = "Mesh Analysis",
        .doc = "How open each element is to its surroundings: 1 when nothing of the mesh lies within the distance above it, 0 when it is "
               "fully enclosed. Rays are spread over the hemisphere around the normal.",
        .sockets = {.inputs = {bounded(in("samples", T::Int, 16), 1, 1024), bounded(in("distance", T::Float, 100.0), 0.0)},
                    .outputs = {fout("ao", T::Float), fout("occlusion", T::Float)}},
        .exec =
            [](ExecContext& c) {
                int samples = int(std::clamp<int64_t>(c.i("samples"), 1, 1024));
                double dist = c.f("distance");
                auto make = [samples, dist](bool invert) {
                    return surfaceField(T::Float, [samples, dist, invert](const FieldContext& ctx, const geo::Mesh& m) {
                        LILA_ZONE();
                        geo::MeshBvh bvh(m);
                        std::vector<D3> pos, nrm;
                        elementFrames(ctx, pos, nrm);
                        const double eps = offsetFor(m);
                        std::vector<double> out(ctx.size, invert ? 0.0 : 1.0);
                        tbb::parallel_for(size_t(0), ctx.size, [&](size_t k) {
                            if (glm::length(nrm[k]) < 1e-12)
                                return;
                            D3 n = glm::normalize(nrm[k]);
                            D3 t = glm::normalize(glm::cross(n, std::abs(n.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0)));
                            D3 b = glm::cross(n, t);
                            int hits = 0;
                            for (int s = 0; s < samples; ++s)
                            {
                                // Cosine-weighted, stratified in the first coordinate.
                                double u = (s + hashToUnit(hash32(uint32_t(k), uint32_t(s), 1))) / samples;
                                double v = hashToUnit(hash32(uint32_t(k), uint32_t(s), 2));
                                double r = std::sqrt(u), phi = 2.0 * 3.14159265358979323846 * v;
                                D3 dir = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1.0 - u));
                                if (bvh.occluded(pos[k] + n * eps, dir, dist))
                                    ++hits;
                            }
                            double open = 1.0 - double(hits) / samples;
                            out[k] = invert ? 1.0 - open : open;
                        });
                        return Column::of(std::move(out));
                    });
                };
                c.out("ao", make(false));
                c.out("occlusion", make(true));
            },
    });
}

struct Geodesic
{
    std::vector<double> distance;
    std::vector<int64_t> source;
};

/** Dijkstra from every source point over edges and across faces, so paths may cut straight through polygons. */
Geodesic geodesic(const geo::Mesh& m, const std::vector<uint8_t>& sources)
{
    LILA_ZONE();

    const Index n = m.pointCapacity();
    Geodesic g;
    g.distance.assign(n, std::numeric_limits<double>::infinity());
    g.source.assign(n, -1);
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    for (Index p = 0; p < n; ++p)
        if (m.pointValid(p) && sources[p])
        {
            g.distance[p] = 0.0;
            g.source[p] = p;
            queue.push({0.0, p});
        }
    auto relax = [&](Index p, Index q, double d) {
        double nd = d + glm::length(m.position(q) - m.position(p));
        if (nd < g.distance[q])
        {
            g.distance[q] = nd;
            g.source[q] = g.source[p];
            queue.push({nd, q});
        }
    };
    while (!queue.empty())
    {
        auto [d, p] = queue.top();
        queue.pop();
        if (d > g.distance[p])
            continue;
        m.forEachPointEdge(p, [&](Index e) { relax(p, m.edgeOtherPoint(e, p), d); });
        m.forEachPointCorner(p, [&](Index k) {
            Index f = m.cornerFace(k);
            if (m.faceSize(f) > 3)
                m.forEachFaceCorner(f, [&](Index x) {
                    if (m.cornerPoint(x) != p)
                        relax(p, m.cornerPoint(x), d);
                });
        });
    }
    for (Index p = 0; p < n; ++p)
        if (!std::isfinite(g.distance[p]))
            g.distance[p] = 0.0;
    return g;
}

void geodesicNode()
{
    registerDecl({
        .type = "GeodesicDistance",
        .label = "Geodesic Distance",
        .category = "Mesh Analysis",
        .doc = "Distance over the surface from each point to the nearest source point, and which source that is. Points no source reaches "
               "read 0 and -1.",
        .sockets = {.inputs = {fin("source", T::Bool, false, Implicit::None, "Points the distance is measured from.")},
                    .outputs = {fout("distance", T::Float), fout("sourceIndex", T::Int)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr source = c.field("source");
                auto solve = [source](const geo::Mesh& m) { return geodesic(m, evaluateAll<uint8_t>(source, FieldContext::forMesh(m, Domain::Point))); };
                c.out("distance", perPoint(T::Float, [solve](const geo::Mesh& m) { return Column::of(solve(m).distance); }));
                c.out("sourceIndex", perPoint(T::Int, [solve](const geo::Mesh& m) { return Column::of(solve(m).source); }));
            },
    });
}

/** Ring number each point is reached in, growing from the seeds along edges; -1 where never reached. */
std::vector<int64_t> floodRings(const geo::Mesh& m, const std::vector<uint8_t>& seed, int64_t steps, double maxAngle)
{
    std::vector<int64_t> ring(m.pointCapacity(), -1);
    std::vector<D3> normals = smoothPointNormals(m);
    const double minDot = std::cos(maxAngle);
    std::vector<Index> front;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && seed[p])
        {
            ring[p] = 0;
            front.push_back(p);
        }
    std::vector<Index> next;
    for (int64_t step = 1; !front.empty() && (steps <= 0 || step <= steps); ++step)
    {
        next.clear();
        for (Index p : front)
            m.forEachPointEdge(p, [&](Index e) {
                Index q = m.edgeOtherPoint(e, p);
                if (ring[q] >= 0 || glm::dot(normals[p], normals[q]) < minDot - 1e-9)
                    return;
                ring[q] = step;
                next.push_back(q);
            });
        front.swap(next);
    }
    return ring;
}

void floodFillNode()
{
    registerDecl({
        .type = "FloodFill",
        .label = "Flood Fill",
        .category = "Mesh Analysis",
        .doc = "Grows a point selection along edges, a ring per step, without crossing to a point whose normal turns more than the angle "
               "limit. Steps of 0 grow until nothing more can be reached.",
        .sockets = {.inputs = {fin("seed", T::Bool, false, Implicit::None, "Points to grow from."), bounded(in("steps", T::Int, 1), 0),
                               bounded(in("angleLimit", T::Float, 180.0, "Degrees between neighbouring normals that still grows."), 0.0, 180.0)},
                    .outputs = {fout("selection", T::Bool), fout("step", T::Int, "Step each point was reached in, 0 for seeds, -1 if never.")}},
        .exec =
            [](ExecContext& c) {
                FieldPtr seed = c.field("seed");
                int64_t steps = c.i("steps");
                double angle = glm::radians(c.f("angleLimit"));
                auto rings = [seed, steps, angle](const geo::Mesh& m) {
                    return floodRings(m, evaluateAll<uint8_t>(seed, FieldContext::forMesh(m, Domain::Point)), steps, angle);
                };
                c.out("selection", perPoint(T::Bool, [rings](const geo::Mesh& m) {
                          std::vector<int64_t> r = rings(m);
                          std::vector<uint8_t> sel(r.size());
                          for (size_t k = 0; k < r.size(); ++k)
                              sel[k] = r[k] >= 0 ? 1 : 0;
                          return Column::of(std::move(sel));
                      }));
                c.out("step", perPoint(T::Int, [rings](const geo::Mesh& m) { return Column::of(rings(m)); }));
            },
    });
}

void featureEdgesNode()
{
    registerDecl({
        .type = "FeatureEdges",
        .label = "Feature Edges",
        .category = "Mesh Analysis",
        .doc = "Classifies edges: sharp where two faces meet at more than the angle, boundary with one face, non-manifold with none or more "
               "than two. Feature is any of them.",
        .sockets = {.inputs = {bounded(in("angle", T::Float, 30.0, "Degrees of bend between faces that counts as sharp."), 0.0, 180.0)},
                    .outputs = {fout("feature", T::Bool), fout("sharp", T::Bool), fout("boundary", T::Bool), fout("nonManifold", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                double minDot = std::cos(glm::radians(c.f("angle")));
                auto make = [minDot](int kind) {
                    return surfaceField(T::Bool, [minDot, kind](const FieldContext& ctx, const geo::Mesh& m) {
                        std::vector<uint8_t> out(m.edgeCapacity(), 0);
                        for (Index e = 0; e < m.edgeCapacity(); ++e)
                        {
                            if (!m.edgeValid(e))
                                continue;
                            Index faces = m.edgeFaceCount(e);
                            bool boundary = faces == 1, nonManifold = faces == 0 || faces > 2, sharp = false;
                            if (faces == 2)
                            {
                                Index k = m.edgeCorner(e);
                                sharp = glm::dot(m.faceNormal(m.cornerFace(k)), m.faceNormal(m.cornerFace(m.cornerRadial(k)))) < minDot - 1e-9;
                            }
                            bool v = kind == 0 ? (sharp || boundary || nonManifold) : kind == 1 ? sharp : kind == 2 ? boundary : nonManifold;
                            out[e] = v ? 1 : 0;
                        }
                        return adaptDomain(m, Column::of(std::move(out)), Domain::Edge, ctx.domain);
                    });
                };
                c.out("feature", make(0));
                c.out("sharp", make(1));
                c.out("boundary", make(2));
                c.out("nonManifold", make(3));
            },
    });
}
} // namespace analysis

void registerAnalysisNodes()
{
    analysis::curvatureNode();
    analysis::cavityNode();
    analysis::rayNodes();
    analysis::geodesicNode();
    analysis::floodFillNode();
    analysis::featureEdgesNode();
}
} // namespace lila::gn::nodes
