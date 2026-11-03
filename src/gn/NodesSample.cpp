#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Bvh.h"

#include <algorithm>
#include <mutex>
#include <numeric>

namespace lila::gn::nodes
{
namespace
{
/** Static kd-tree for nearest-point queries. */
class KdTree
{
public:
    explicit KdTree(std::vector<D3> pts) : mPts(std::move(pts)), mOrder(mPts.size())
    {
        std::iota(mOrder.begin(), mOrder.end(), Index(0));
        build(0, mOrder.size(), 0);
    }

    /** Index of the nearest point, kInvalid when empty. */
    Index nearest(const D3& q) const
    {
        Index best = geo::kInvalid;
        double bestD = std::numeric_limits<double>::max();
        search(0, mOrder.size(), 0, q, best, bestD);
        return best;
    }
    const D3& point(Index i) const { return mPts[i]; }
    bool empty() const { return mPts.empty(); }

private:
    void build(size_t lo, size_t hi, int axis)
    {
        if (hi - lo <= 1)
            return;
        size_t mid = (lo + hi) / 2;
        std::nth_element(mOrder.begin() + lo, mOrder.begin() + mid, mOrder.begin() + hi,
                         [&](Index a, Index b) { return mPts[a][axis] < mPts[b][axis]; });
        build(lo, mid, (axis + 1) % 3);
        build(mid + 1, hi, (axis + 1) % 3);
    }

    void search(size_t lo, size_t hi, int axis, const D3& q, Index& best, double& bestD) const
    {
        if (lo >= hi)
            return;
        size_t mid = (lo + hi) / 2;
        Index i = mOrder[mid];
        double d = glm::dot(mPts[i] - q, mPts[i] - q);
        if (d < bestD || (d == bestD && i < best))
        {
            bestD = d;
            best = i;
        }
        double diff = q[axis] - mPts[i][axis];
        int next = (axis + 1) % 3;
        if (diff < 0.0)
        {
            search(lo, mid, next, q, best, bestD);
            if (diff * diff <= bestD)
                search(mid + 1, hi, next, q, best, bestD);
        }
        else
        {
            search(mid + 1, hi, next, q, best, bestD);
            if (diff * diff <= bestD)
                search(lo, mid, next, q, best, bestD);
        }
    }

    std::vector<D3> mPts;
    std::vector<Index> mOrder;
};

/** Target mesh for sampling: the mesh component, realized. Built lazily and shared by the outputs. */
struct SurfaceTarget
{
    std::shared_ptr<const geo::Mesh> mesh;
    std::once_flag once;
    geo::MeshBvh bvh;

    const geo::MeshBvh& tree()
    {
        std::call_once(once, [&] {
            if (mesh)
                bvh.build(*mesh);
        });
        return bvh;
    }
};

std::shared_ptr<SurfaceTarget> surfaceOf(const GeometryPtr& g)
{
    auto t = std::make_shared<SurfaceTarget>();
    if (g->instances && g->instances->size())
    {
        Geometry r = realize(*g);
        t->mesh = r.mesh;
    }
    else
        t->mesh = g->mesh;
    return t;
}

/** A field computed per element from one or more context fields, using shared lazily built state. */
class LambdaField final : public Field
{
public:
    using Fn = std::function<Column(const FieldContext&)>;
    LambdaField(SocketType t, Fn fn) : Field(t), mFn(std::move(fn)) {}
    Column evaluate(const FieldContext& ctx) const override { return mFn(ctx); }

private:
    Fn mFn;
};

void raycast()
{
    registerDecl({
        .type = "Raycast",
        .label = "Raycast",
        .category = "Geometry",
        .doc = "Casts a ray from each element against the target mesh. The attribute is read on the target's points and interpolated at the hit.",
        .props = {prop("dataType", {"float", "vector", "int", "bool"}, "float"), prop("mapping", {"interpolated", "nearest"}, "interpolated")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("Raycast")->propValue(n, "dataType"));
                json def = t == T::Vector ? vec(0, 0, 0) : json(0);
                return Sockets{{geoIn("targetGeometry"), fin("attribute", t, def), fin("sourcePosition", T::Vector, nullptr, Implicit::Position),
                                fin("rayDirection", T::Vector, vec(0, -1, 0)), bounded(fin("rayLength", T::Float, 100000.0), 0.0)},
                               {fout("isHit", T::Bool), fout("hitPosition", T::Vector), fout("hitNormal", T::Vector), fout("hitDistance", T::Float),
                                fout("attribute", t)}};
            },
        .exec =
            [](ExecContext& c) {
                auto target = surfaceOf(c.geometry("targetGeometry"));
                FieldPtr src = c.field("sourcePosition"), dir = c.field("rayDirection"), len = c.field("rayLength"), attr = c.field("attribute");
                SocketType t = dataTypeOf(c.prop("dataType"));
                bool nearest = c.prop("mapping") == "nearest";
                // Each output re-casts; casts are cheap next to building the tree, which happens once.
                auto cast = [=](const FieldContext& ctx) {
                    std::vector<D3> o = evaluateAll<D3>(src, ctx), d = evaluateAll<D3>(dir, ctx);
                    std::vector<double> l = evaluateAll<double>(len, ctx);
                    std::vector<geo::BvhHit> hits(ctx.size);
                    std::vector<uint8_t> ok(ctx.size, 0);
                    if (target->mesh)
                    {
                        const geo::MeshBvh& bvh = target->tree();
                        tbb::parallel_for(size_t(0), ctx.size, [&](size_t k) {
                            double dl = glm::length(d[k]);
                            if (dl < 1e-12)
                                return;
                            ok[k] = bvh.raycast(o[k], d[k] / dl, hits[k], l[k]) ? 1 : 0;
                        });
                    }
                    return std::make_tuple(std::move(hits), std::move(ok));
                };
                c.out("isHit", std::make_shared<LambdaField>(T::Bool, [cast](const FieldContext& ctx) { return Column::of(std::get<1>(cast(ctx))); }));
                c.out("hitPosition", std::make_shared<LambdaField>(T::Vector, [cast](const FieldContext& ctx) {
                          auto [hits, ok] = cast(ctx);
                          std::vector<D3> out(ctx.size, D3(0.0));
                          for (size_t k = 0; k < ctx.size; ++k)
                              if (ok[k])
                                  out[k] = hits[k].point;
                          return Column::of(std::move(out));
                      }));
                c.out("hitNormal", std::make_shared<LambdaField>(T::Vector, [cast, target](const FieldContext& ctx) {
                          auto [hits, ok] = cast(ctx);
                          std::vector<D3> out(ctx.size, D3(0.0));
                          for (size_t k = 0; k < ctx.size; ++k)
                              if (ok[k])
                                  out[k] = target->mesh->faceNormal(hits[k].face);
                          return Column::of(std::move(out));
                      }));
                c.out("hitDistance", std::make_shared<LambdaField>(T::Float, [cast](const FieldContext& ctx) {
                          auto [hits, ok] = cast(ctx);
                          std::vector<double> out(ctx.size, 0.0);
                          for (size_t k = 0; k < ctx.size; ++k)
                              if (ok[k])
                                  out[k] = hits[k].t;
                          return Column::of(std::move(out));
                      }));
                c.out("attribute", std::make_shared<LambdaField>(t, [cast, target, attr, nearest](const FieldContext& ctx) {
                          auto [hits, ok] = cast(ctx);
                          std::vector<D3> out(ctx.size, D3(0.0));
                          if (!target->mesh)
                              return Column::of(std::move(out));
                          const geo::Mesh& m = *target->mesh;
                          std::vector<D3> values = evaluateAll<D3>(convertField(attr, T::Vector), FieldContext::forMesh(m, Domain::Point));
                          const geo::Triangulation& tris = target->tree().triangles();
                          for (size_t k = 0; k < ctx.size; ++k)
                          {
                              if (!ok[k])
                                  continue;
                              const Index* cc = &tris.corners[size_t(hits[k].triangle) * 3];
                              D3 b = hits[k].bary;
                              if (nearest)
                              {
                                  int i = b.x >= b.y && b.x >= b.z ? 0 : b.y >= b.z ? 1 : 2;
                                  out[k] = values[m.cornerPoint(cc[i])];
                              }
                              else
                                  out[k] = values[m.cornerPoint(cc[0])] * b.x + values[m.cornerPoint(cc[1])] * b.y + values[m.cornerPoint(cc[2])] * b.z;
                          }
                          return Column::of(std::move(out));
                      }));
            },
    });
}

void proximity()
{
    registerDecl({
        .type = "GeometryProximity",
        .label = "Geometry Proximity",
        .category = "Geometry",
        .doc = "Closest point on the target's faces, edges or points.",
        .props = {prop("targetElement", {"faces", "edges", "points"}, "faces")},
        .sockets = {.inputs = {geoIn("targetGeometry"), fin("sourcePosition", T::Vector, nullptr, Implicit::Position)},
                    .outputs = {fout("position", T::Vector), fout("distance", T::Float)}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("targetGeometry");
                auto target = surfaceOf(g);
                std::string mode = c.prop("targetElement");
                std::shared_ptr<KdTree> kd;
                std::vector<std::pair<D3, D3>> segs;
                if (mode == "points")
                {
                    std::vector<D3> pts;
                    Geometry r = realize(*g);
                    for (const geo::Mesh* m : {r.mesh.get(), r.points.get()})
                        if (m)
                            pts.insert(pts.end(), m->positions().begin(), m->positions().end());
                    if (r.curves)
                        pts.insert(pts.end(), r.curves->positions.begin(), r.curves->positions.end());
                    kd = std::make_shared<KdTree>(std::move(pts));
                }
                else if (mode == "edges" && target->mesh)
                    for (Index e = 0; e < target->mesh->edgeCapacity(); ++e)
                        if (target->mesh->edgeValid(e))
                            segs.push_back({target->mesh->position(target->mesh->edgePoint(e, 0)), target->mesh->position(target->mesh->edgePoint(e, 1))});
                FieldPtr src = c.field("sourcePosition");
                auto closest = [=](const FieldContext& ctx) {
                    std::vector<D3> p = evaluateAll<D3>(src, ctx), out(ctx.size, D3(0.0));
                    tbb::parallel_for(size_t(0), ctx.size, [&](size_t k) {
                        if (kd)
                        {
                            Index i = kd->nearest(p[k]);
                            out[k] = i == geo::kInvalid ? p[k] : kd->point(i);
                        }
                        else if (!segs.empty())
                        {
                            double best = std::numeric_limits<double>::max();
                            for (auto& [a, b] : segs)
                            {
                                D3 ab = b - a;
                                double t = glm::dot(ab, ab) > 0.0 ? std::clamp(glm::dot(p[k] - a, ab) / glm::dot(ab, ab), 0.0, 1.0) : 0.0;
                                D3 q = a + ab * t;
                                double d = glm::distance(q, p[k]);
                                if (d < best)
                                {
                                    best = d;
                                    out[k] = q;
                                }
                            }
                        }
                        else if (target->mesh)
                        {
                            geo::BvhClosest cl;
                            out[k] = target->tree().closestPoint(p[k], cl) ? cl.point : p[k];
                        }
                        else
                            out[k] = p[k];
                    });
                    return std::make_pair(std::move(p), std::move(out));
                };
                c.out("position", std::make_shared<LambdaField>(T::Vector, [closest](const FieldContext& ctx) { return Column::of(closest(ctx).second); }));
                c.out("distance", std::make_shared<LambdaField>(T::Float, [closest](const FieldContext& ctx) {
                          auto [p, q] = closest(ctx);
                          std::vector<double> d(ctx.size);
                          for (size_t k = 0; k < ctx.size; ++k)
                              d[k] = glm::distance(p[k], q[k]);
                          return Column::of(std::move(d));
                      }));
            },
    });
}

/** Context for a domain on the first component of g that has it. */
std::optional<FieldContext> sampleContext(const Geometry& g, Domain d)
{
    if (g.mesh && d <= Domain::Corner)
        return FieldContext::forMesh(*g.mesh, d);
    if (g.points && d == Domain::Point)
        return FieldContext::forPoints(*g.points);
    if (g.curves && (d == Domain::Point || d == Domain::Curve))
        return FieldContext::forCurves(*g.curves, d);
    if (g.instances && d == Domain::Instance)
        return FieldContext::forInstances(*g.instances);
    return std::nullopt;
}

void sampleIndex()
{
    registerDecl({
        .type = "SampleIndex",
        .label = "Sample Index",
        .category = "Geometry",
        .doc = "Reads a field from another geometry at an index.",
        .props = {prop("dataType", dataTypes(), "float"), prop("domain", meshDomains(), "point"), prop("clamp", {"false", "true"}, "false")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("SampleIndex")->propValue(n, "dataType"));
                json def = t == T::Vector || t == T::Rotation ? vec(0, 0, 0) : json(0);
                return Sockets{{geoIn(), fin("value", t, def), fin("index", T::Int, nullptr, Implicit::Index)}, {fout("value", t)}};
            },
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("geometry");
                SocketType t = dataTypeOf(c.prop("dataType"));
                std::optional<FieldContext> sctx = sampleContext(*g, domainOf(c.prop("domain")));
                if (!sctx)
                {
                    c.out("value", defaultValue(t).field);
                    return;
                }
                auto values = std::make_shared<Column>(evaluate(c.field("value"), *sctx, t));
                size_t n = sctx->size;
                bool clamp = c.prop("clamp") == "true";
                FieldPtr index = c.field("index");
                // Keeps the sampled geometry alive with the field.
                c.out("value", std::make_shared<LambdaField>(t, [g, values, n, clamp, index](const FieldContext& ctx) {
                          std::vector<int64_t> idx = evaluateAll<int64_t>(index, ctx);
                          return std::visit(
                              [&](const auto& v) -> Column {
                                  using V = typename std::decay_t<decltype(v)>::value_type;
                                  std::vector<V> out(ctx.size);
                                  for (size_t k = 0; k < ctx.size; ++k)
                                  {
                                      int64_t i = idx[k];
                                      if (clamp)
                                          i = std::clamp<int64_t>(i, 0, int64_t(n) - 1);
                                      out[k] = i >= 0 && size_t(i) < n ? values->template at<V>(size_t(i)) : V{};
                                  }
                                  return Column::of(std::move(out));
                              },
                              values->data);
                      }));
            },
    });
}

void sampleNearest()
{
    registerDecl({
        .type = "SampleNearest",
        .label = "Sample Nearest",
        .category = "Geometry",
        .doc = "Index of the element of another geometry closest to each position. Pair with Sample Index.",
        .props = {prop("domain", {"point", "edge", "face", "corner"}, "point")},
        .sockets = {.inputs = {geoIn(), fin("samplePosition", T::Vector, nullptr, Implicit::Position)}, .outputs = {fout("index", T::Int)}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("geometry");
                Domain d = domainOf(c.prop("domain"));
                std::optional<FieldContext> sctx = sampleContext(*g, d);
                if (!sctx)
                {
                    c.out("index", constantInt(0));
                    return;
                }
                auto kd = std::make_shared<KdTree>(evaluateAll<D3>(positionField(), *sctx));
                FieldPtr pos = c.field("samplePosition");
                std::shared_ptr<SurfaceTarget> surface = d == Domain::Face ? surfaceOf(g) : nullptr;
                c.out("index", std::make_shared<LambdaField>(T::Int, [kd, pos, surface](const FieldContext& ctx) {
                          std::vector<D3> p = evaluateAll<D3>(pos, ctx);
                          std::vector<int64_t> out(ctx.size, 0);
                          tbb::parallel_for(size_t(0), ctx.size, [&](size_t k) {
                              if (surface && surface->mesh)
                              {
                                  geo::BvhClosest cl;
                                  if (surface->tree().closestPoint(p[k], cl))
                                      out[k] = int64_t(cl.face);
                                  return;
                              }
                              Index i = kd->nearest(p[k]);
                              out[k] = i == geo::kInvalid ? 0 : int64_t(i);
                          });
                          return Column::of(std::move(out));
                      }));
            },
    });
}
} // namespace

void registerSampleNodes()
{
    raycast();
    proximity();
    sampleIndex();
    sampleNearest();
}
} // namespace lila::gn::nodes
