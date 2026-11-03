#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Topology.h"

#include <algorithm>
#include <numeric>
#include <queue>

namespace lila::gn::nodes
{
namespace topology
{
/** A field read off the mesh of the context it is evaluated in; anything but a mesh gets the type's zero. */
class MeshField final : public Field
{
public:
    using Fn = std::function<Column(const FieldContext&, const geo::Mesh&)>;
    MeshField(SocketType t, Fn fn) : Field(t), mFn(std::move(fn)) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Mesh || !ctx.mesh)
            return convert(Column::of(0.0), type());
        return convert(mFn(ctx, *ctx.mesh), type());
    }

private:
    Fn mFn;
};

FieldPtr meshField(SocketType t, MeshField::Fn fn) { return std::make_shared<MeshField>(t, std::move(fn)); }

/** A field computed on one domain of the mesh and moved to the domain it is read on. */
FieldPtr onDomain(SocketType t, Domain natural, std::function<Column(const geo::Mesh&)> fn)
{
    return meshField(t, [natural, fn = std::move(fn)](const FieldContext& ctx, const geo::Mesh& m) {
        return adaptDomain(m, fn(m), natural, ctx.domain);
    });
}

std::vector<int64_t> ints(const FieldPtr& f, const FieldContext& ctx) { return evaluateAll<int64_t>(f, ctx); }
std::vector<double> floats(const FieldPtr& f, const FieldContext& ctx) { return evaluateAll<double>(f, ctx); }
std::vector<uint8_t> bools(const FieldPtr& f, const FieldContext& ctx) { return evaluateAll<uint8_t>(f, ctx); }

bool inRange(int64_t i, Index n) { return i >= 0 && i < int64_t(n); }

/** Two faces' bend across a manifold edge: 0 when flat, positive when convex. */
double signedEdgeAngle(const geo::Mesh& m, Index e)
{
    if (m.edgeFaceCount(e) != 2)
        return 0.0;
    Index c = m.edgeCorner(e);
    Index f1 = m.cornerFace(c), f2 = m.cornerFace(m.cornerRadial(c));
    D3 n1 = m.faceNormal(f1), n2 = m.faceNormal(f2);
    double a = std::acos(std::clamp(glm::dot(n1, n2), -1.0, 1.0));
    return glm::dot(n1, m.faceCentroid(f2) - m.faceCentroid(f1)) > 0.0 ? -a : a;
}

/** Ids of a choice list sorted by weight, ties kept in id order, and the one at sortIndex (wrapping). */
int64_t pickSorted(std::vector<Index>& ids, const std::vector<double>& weights, int64_t sortIndex)
{
    if (ids.empty())
        return 0;
    std::sort(ids.begin(), ids.end());
    std::stable_sort(ids.begin(), ids.end(), [&](Index a, Index b) { return weights[a] < weights[b]; });
    int64_t n = int64_t(ids.size());
    return ids[size_t(((sortIndex % n) + n) % n)];
}

/** Sockets shared by Corners of Face, Corners of Vertex and Edges of Vertex. */
Sockets choiceSockets(const char* indexId, const char* indexDoc, const char* outId)
{
    return {{fin(indexId, T::Int, nullptr, Implicit::Index, indexDoc),
             fin("weights", T::Float, 0.0, Implicit::None, "Values the choices are sorted by, lowest first."),
             fin("sortIndex", T::Int, 0, Implicit::None, "Which of the sorted choices to output; wraps around.")},
            {fout(outId, T::Int), fout("total", T::Int, "How many choices there are.")}};
}

void measureNodes()
{
    registerDecl({
        .type = "EdgeAngle",
        .label = "Edge Angle",
        .category = "Mesh Topology",
        .doc = "Angle between the faces on each edge, radians. Signed is positive on convex edges and negative on concave ones. Edges "
               "without exactly two faces read 0.",
        .sockets = {.outputs = {fout("unsignedAngle", T::Float), fout("signedAngle", T::Float)}},
        .exec =
            [](ExecContext& c) {
                auto angles = [](bool sign) {
                    return onDomain(T::Float, Domain::Edge, [sign](const geo::Mesh& m) {
                        std::vector<double> a(m.edgeCapacity(), 0.0);
                        for (Index e = 0; e < m.edgeCapacity(); ++e)
                            if (m.edgeValid(e))
                                a[e] = sign ? signedEdgeAngle(m, e) : std::abs(signedEdgeAngle(m, e));
                        return Column::of(std::move(a));
                    });
                };
                c.out("unsignedAngle", angles(false));
                c.out("signedAngle", angles(true));
            },
    });
    registerDecl({
        .type = "FaceArea",
        .label = "Face Area",
        .category = "Mesh Topology",
        .doc = "Surface area of each face.",
        .sockets = {.outputs = {fout("area", T::Float)}},
        .exec =
            [](ExecContext& c) {
                c.out("area", onDomain(T::Float, Domain::Face, [](const geo::Mesh& m) {
                          std::vector<double> a(m.faceCapacity(), 0.0);
                          for (Index f = 0; f < m.faceCapacity(); ++f)
                              if (m.faceValid(f))
                                  a[f] = m.faceArea(f);
                          return Column::of(std::move(a));
                      }));
            },
    });
    registerDecl({
        .type = "IsFacePlanar",
        .label = "Is Face Planar",
        .category = "Mesh Topology",
        .doc = "True for faces whose points all lie within the threshold of the face's plane.",
        .sockets = {.inputs = {bounded(fin("threshold", T::Float, 0.01, Implicit::None, "Distance from the plane allowed, read per face."), 0.0)},
                    .outputs = {fout("planar", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr threshold = c.field("threshold");
                c.out("planar", meshField(T::Bool, [threshold](const FieldContext& ctx, const geo::Mesh& m) {
                          std::vector<double> th = floats(threshold, FieldContext::forMesh(m, Domain::Face));
                          std::vector<uint8_t> planar(m.faceCapacity(), 1);
                          for (Index f = 0; f < m.faceCapacity(); ++f)
                          {
                              if (!m.faceValid(f))
                                  continue;
                              D3 n = m.faceNormal(f);
                              double lo = std::numeric_limits<double>::max(), hi = -lo;
                              m.forEachFaceCorner(f, [&](Index k) {
                                  double d = glm::dot(m.position(m.cornerPoint(k)), n);
                                  lo = std::min(lo, d);
                                  hi = std::max(hi, d);
                              });
                              planar[f] = hi - lo <= th[f] ? 1 : 0;
                          }
                          return adaptDomain(m, Column::of(std::move(planar)), Domain::Face, ctx.domain);
                      }));
            },
    });
}

void neighborNodes()
{
    registerDecl({
        .type = "VertexNeighbors",
        .label = "Vertex Neighbors",
        .category = "Mesh Topology",
        .doc = "Per point: how many points it shares an edge with, and how many faces use it.",
        .sockets = {.outputs = {fout("vertexCount", T::Int), fout("faceCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("vertexCount", onDomain(T::Int, Domain::Point, [](const geo::Mesh& m) {
                          std::vector<int64_t> n(m.pointCapacity(), 0);
                          for (Index p = 0; p < m.pointCapacity(); ++p)
                              if (m.pointValid(p))
                                  n[p] = m.pointValence(p);
                          return Column::of(std::move(n));
                      }));
                c.out("faceCount", onDomain(T::Int, Domain::Point, [](const geo::Mesh& m) {
                          std::vector<int64_t> n(m.pointCapacity(), 0);
                          for (Index p = 0; p < m.pointCapacity(); ++p)
                              if (m.pointValid(p))
                                  m.forEachPointCorner(p, [&](Index) { ++n[p]; });
                          return Column::of(std::move(n));
                      }));
            },
    });
    registerDecl({
        .type = "FaceNeighbors",
        .label = "Face Neighbors",
        .category = "Mesh Topology",
        .doc = "Per face: its number of corners, and how many other faces share one of its edges (counted per edge).",
        .sockets = {.outputs = {fout("vertexCount", T::Int), fout("faceCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("vertexCount", onDomain(T::Int, Domain::Face, [](const geo::Mesh& m) {
                          std::vector<int64_t> n(m.faceCapacity(), 0);
                          for (Index f = 0; f < m.faceCapacity(); ++f)
                              if (m.faceValid(f))
                                  n[f] = m.faceSize(f);
                          return Column::of(std::move(n));
                      }));
                c.out("faceCount", onDomain(T::Int, Domain::Face, [](const geo::Mesh& m) {
                          std::vector<int64_t> n(m.faceCapacity(), 0);
                          for (Index f = 0; f < m.faceCapacity(); ++f)
                              if (m.faceValid(f))
                                  m.forEachFaceCorner(f, [&](Index k) { n[f] += int64_t(m.edgeFaceCount(m.cornerEdge(k))) - 1; });
                          return Column::of(std::move(n));
                      }));
            },
    });
    registerDecl({
        .type = "EdgeNeighbors",
        .label = "Edge Neighbors",
        .category = "Mesh Topology",
        .doc = "Per edge: how many faces use it.",
        .sockets = {.outputs = {fout("faceCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("faceCount", onDomain(T::Int, Domain::Edge, [](const geo::Mesh& m) {
                          std::vector<int64_t> n(m.edgeCapacity(), 0);
                          for (Index e = 0; e < m.edgeCapacity(); ++e)
                              if (m.edgeValid(e))
                                  n[e] = m.edgeFaceCount(e);
                          return Column::of(std::move(n));
                      }));
            },
    });
}

/** Island per point through edges, numbered in order of each island's lowest point. */
std::vector<int64_t> pointIslands(const geo::Mesh& m, int64_t& count)
{
    std::vector<Index> parent(m.pointCapacity());
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index p) {
        while (parent[p] != p)
            p = parent[p] = parent[parent[p]];
        return p;
    };
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            Index a = find(m.edgePoint(e, 0)), b = find(m.edgePoint(e, 1));
            if (a != b)
                parent[std::max(a, b)] = std::min(a, b);
        }
    std::vector<int64_t> island(m.pointCapacity(), 0), number(m.pointCapacity(), -1);
    count = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        Index r = find(p);
        if (number[r] < 0)
            number[r] = count++;
        island[p] = number[r];
    }
    return island;
}

void islandNode()
{
    registerDecl({
        .type = "MeshIsland",
        .label = "Mesh Island",
        .category = "Mesh Topology",
        .doc = "Which connected piece each element belongs to, numbered from 0 in point order, and how many pieces there are.",
        .sockets = {.outputs = {fout("islandIndex", T::Int), fout("islandCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("islandIndex", meshField(T::Int, [](const FieldContext& ctx, const geo::Mesh& m) {
                          int64_t count = 0;
                          std::vector<int64_t> island = pointIslands(m, count);
                          if (ctx.domain == Domain::Point)
                              return Column::of(std::move(island));
                          // Every element of a piece shares its island, so take any point instead of averaging.
                          std::vector<int64_t> out(ctx.size, 0);
                          for (size_t k = 0; k < ctx.size; ++k)
                          {
                              Index id = Index(k);
                              if (ctx.domain == Domain::Edge && m.edgeValid(id))
                                  out[k] = island[m.edgePoint(id, 0)];
                              else if (ctx.domain == Domain::Face && m.faceValid(id))
                                  out[k] = island[m.cornerPoint(m.faceCorner(id))];
                              else if (ctx.domain == Domain::Corner && m.cornerValid(id))
                                  out[k] = island[m.cornerPoint(id)];
                          }
                          return Column::of(std::move(out));
                      }));
                c.out("islandCount", meshField(T::Int, [](const FieldContext&, const geo::Mesh& m) {
                          int64_t count = 0;
                          pointIslands(m, count);
                          return Column::of(count);
                      }));
            },
    });
}

void cornerNodes()
{
    registerDecl({
        .type = "CornersOfFace",
        .label = "Corners of Face",
        .category = "Mesh Topology",
        .doc = "A corner of a face, chosen by sorting the face's corners by weight. With equal weights they come in winding order.",
        .sockets = choiceSockets("faceIndex", "The face to look in; defaults to the index.", "cornerIndex"),
        .exec =
            [](ExecContext& c) {
                FieldPtr index = c.field("faceIndex"), weights = c.field("weights"), sortIndex = c.field("sortIndex");
                auto make = [=](bool total) {
                    return meshField(T::Int, [=](const FieldContext& ctx, const geo::Mesh& m) {
                        std::vector<int64_t> fi = ints(index, ctx), si = ints(sortIndex, ctx);
                        std::vector<double> w = floats(weights, FieldContext::forMesh(m, Domain::Corner));
                        std::vector<int64_t> out(ctx.size, 0);
                        std::vector<Index> ids;
                        for (size_t k = 0; k < ctx.size; ++k)
                        {
                            if (!inRange(fi[k], m.faceCapacity()) || !m.faceValid(Index(fi[k])))
                                continue;
                            Index f = Index(fi[k]);
                            if (total)
                            {
                                out[k] = m.faceSize(f);
                                continue;
                            }
                            ids.clear();
                            m.forEachFaceCorner(f, [&](Index x) { ids.push_back(x); });
                            std::stable_sort(ids.begin(), ids.end(), [&](Index a, Index b) { return w[a] < w[b]; });
                            int64_t n = int64_t(ids.size());
                            out[k] = ids[size_t(((si[k] % n) + n) % n)];
                        }
                        return Column::of(std::move(out));
                    });
                };
                c.out("cornerIndex", make(false));
                c.out("total", make(true));
            },
    });

    auto ofVertex = [](const char* type, const char* label, const char* outId, bool edges) {
        registerDecl({
            .type = type,
            .label = label,
            .category = "Mesh Topology",
            .doc = std::string("One of the ") + (edges ? "edges" : "corners") +
                   " around a point, chosen by sorting them by weight. With equal weights they come in index order.",
            .sockets = choiceSockets("vertexIndex", "The point to look around; defaults to the index.", outId),
            .exec =
                [outId, edges](ExecContext& c) {
                    FieldPtr index = c.field("vertexIndex"), weights = c.field("weights"), sortIndex = c.field("sortIndex");
                    auto make = [=](bool total) {
                        return meshField(T::Int, [=](const FieldContext& ctx, const geo::Mesh& m) {
                            std::vector<int64_t> vi = ints(index, ctx), si = ints(sortIndex, ctx);
                            std::vector<double> w = floats(weights, FieldContext::forMesh(m, edges ? Domain::Edge : Domain::Corner));
                            std::vector<int64_t> out(ctx.size, 0);
                            std::vector<Index> ids;
                            for (size_t k = 0; k < ctx.size; ++k)
                            {
                                if (!inRange(vi[k], m.pointCapacity()) || !m.pointValid(Index(vi[k])))
                                    continue;
                                ids.clear();
                                if (edges)
                                    m.forEachPointEdge(Index(vi[k]), [&](Index e) { ids.push_back(e); });
                                else
                                    m.forEachPointCorner(Index(vi[k]), [&](Index x) { ids.push_back(x); });
                                out[k] = total ? int64_t(ids.size()) : pickSorted(ids, w, si[k]);
                            }
                            return Column::of(std::move(out));
                        });
                    };
                    c.out(outId, make(false));
                    c.out("total", make(true));
                },
        });
    };
    ofVertex("CornersOfVertex", "Corners of Vertex", "cornerIndex", false);
    ofVertex("EdgesOfVertex", "Edges of Vertex", "edgeIndex", true);

    /** A per-corner lookup driven by a corner index field. */
    auto cornerLookup = [](FieldPtr index, std::function<int64_t(const geo::Mesh&, Index)> fn) {
        return meshField(T::Int, [index, fn = std::move(fn)](const FieldContext& ctx, const geo::Mesh& m) {
            std::vector<int64_t> ci = ints(index, ctx);
            std::vector<int64_t> out(ctx.size, 0);
            for (size_t k = 0; k < ctx.size; ++k)
                if (inRange(ci[k], m.cornerCapacity()) && m.cornerValid(Index(ci[k])))
                    out[k] = fn(m, Index(ci[k]));
            return Column::of(std::move(out));
        });
    };
    auto cornerIn = [] { return fin("cornerIndex", T::Int, nullptr, Implicit::Index, "The corner; defaults to the index."); };

    registerDecl({
        .type = "EdgesOfCorner",
        .label = "Edges of Corner",
        .category = "Mesh Topology",
        .doc = "The edges on either side of a corner in its face: the next one runs from the corner's point onward along the winding.",
        .sockets = {.inputs = {cornerIn()}, .outputs = {fout("nextEdgeIndex", T::Int), fout("previousEdgeIndex", T::Int)}},
        .exec =
            [cornerLookup](ExecContext& c) {
                FieldPtr index = c.field("cornerIndex");
                c.out("nextEdgeIndex", cornerLookup(index, [](const geo::Mesh& m, Index k) { return int64_t(m.cornerEdge(k)); }));
                c.out("previousEdgeIndex", cornerLookup(index, [](const geo::Mesh& m, Index k) { return int64_t(m.cornerEdge(m.cornerPrev(k))); }));
            },
    });
    registerDecl({
        .type = "FaceOfCorner",
        .label = "Face of Corner",
        .category = "Mesh Topology",
        .doc = "The face a corner belongs to, and the corner's position in that face's winding.",
        .sockets = {.inputs = {cornerIn()}, .outputs = {fout("faceIndex", T::Int), fout("indexInFace", T::Int)}},
        .exec =
            [cornerLookup](ExecContext& c) {
                FieldPtr index = c.field("cornerIndex");
                c.out("faceIndex", cornerLookup(index, [](const geo::Mesh& m, Index k) { return int64_t(m.cornerFace(k)); }));
                c.out("indexInFace", cornerLookup(index, [](const geo::Mesh& m, Index k) {
                          Index first = m.faceCorner(m.cornerFace(k));
                          int64_t n = 0;
                          for (Index x = first; x != k; x = m.cornerNext(x))
                              ++n;
                          return n;
                      }));
            },
    });
    registerDecl({
        .type = "VertexOfCorner",
        .label = "Vertex of Corner",
        .category = "Mesh Topology",
        .doc = "The point a corner is attached to.",
        .sockets = {.inputs = {cornerIn()}, .outputs = {fout("vertexIndex", T::Int)}},
        .exec =
            [cornerLookup](ExecContext& c) {
                c.out("vertexIndex", cornerLookup(c.field("cornerIndex"), [](const geo::Mesh& m, Index k) { return int64_t(m.cornerPoint(k)); }));
            },
    });
    registerDecl({
        .type = "OffsetCornerInFace",
        .label = "Offset Corner in Face",
        .category = "Mesh Topology",
        .doc = "The corner a number of steps along the face's winding from another, wrapping around the face.",
        .sockets = {.inputs = {cornerIn(), fin("offset", T::Int, 0)}, .outputs = {fout("cornerIndex", T::Int)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr index = c.field("cornerIndex"), offset = c.field("offset");
                c.out("cornerIndex", meshField(T::Int, [index, offset](const FieldContext& ctx, const geo::Mesh& m) {
                          std::vector<int64_t> ci = ints(index, ctx), off = ints(offset, ctx);
                          std::vector<int64_t> out(ctx.size, 0);
                          for (size_t k = 0; k < ctx.size; ++k)
                          {
                              if (!inRange(ci[k], m.cornerCapacity()) || !m.cornerValid(Index(ci[k])))
                                  continue;
                              Index x = Index(ci[k]);
                              int64_t n = m.faceSize(m.cornerFace(x));
                              for (int64_t s = ((off[k] % n) + n) % n; s > 0; --s)
                                  x = m.cornerNext(x);
                              out[k] = x;
                          }
                          return Column::of(std::move(out));
                      }));
            },
    });
    registerDecl({
        .type = "EdgeVertices",
        .label = "Edge Vertices",
        .category = "Mesh Topology",
        .doc = "The two points of each edge and their positions.",
        .sockets = {.outputs = {fout("vertexIndex1", T::Int), fout("vertexIndex2", T::Int), fout("position1", T::Vector), fout("position2", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                for (int side = 0; side < 2; ++side)
                {
                    std::string s = std::to_string(side + 1);
                    c.out("vertexIndex" + s, onDomain(T::Int, Domain::Edge, [side](const geo::Mesh& m) {
                              std::vector<int64_t> v(m.edgeCapacity(), 0);
                              for (Index e = 0; e < m.edgeCapacity(); ++e)
                                  if (m.edgeValid(e))
                                      v[e] = m.edgePoint(e, side);
                              return Column::of(std::move(v));
                          }));
                    c.out("position" + s, onDomain(T::Vector, Domain::Edge, [side](const geo::Mesh& m) {
                              std::vector<D3> v(m.edgeCapacity(), D3(0.0));
                              for (Index e = 0; e < m.edgeCapacity(); ++e)
                                  if (m.edgeValid(e))
                                      v[e] = m.position(m.edgePoint(e, side));
                              return Column::of(std::move(v));
                          }));
                }
            },
    });
}

struct PathResult
{
    std::vector<int64_t> next;
    std::vector<double> cost;
};

/** Dijkstra from every end point at once over edges: each point's next step toward its nearest end. */
PathResult shortestPaths(const geo::Mesh& m, const std::vector<uint8_t>& ends, const std::vector<double>& edgeCost)
{
    LILA_ZONE();

    const Index n = m.pointCapacity();
    PathResult r;
    r.next.resize(n);
    std::iota(r.next.begin(), r.next.end(), int64_t(0));
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    for (Index p = 0; p < n; ++p)
        if (m.pointValid(p) && ends[p])
        {
            dist[p] = 0.0;
            queue.push({0.0, p});
        }
    while (!queue.empty())
    {
        auto [d, p] = queue.top();
        queue.pop();
        if (d > dist[p])
            continue;
        m.forEachPointEdge(p, [&](Index e) {
            Index q = m.edgeOtherPoint(e, p);
            double nd = d + std::max(0.0, edgeCost[e]);
            if (nd < dist[q])
            {
                dist[q] = nd;
                r.next[q] = p;
                queue.push({nd, q});
            }
        });
    }
    r.cost.resize(n);
    for (Index p = 0; p < n; ++p)
        r.cost[p] = std::isfinite(dist[p]) ? dist[p] : 0.0;
    return r;
}

/** Walks next indices from a start until the walk stops moving, leaves the mesh or comes back on itself. */
template <class F> void walkPath(Index start, const std::vector<int64_t>& next, std::vector<uint32_t>& stamp, uint32_t mark, F&& step)
{
    Index p = start;
    stamp[p] = mark;
    while (true)
    {
        int64_t q = next[p];
        if (!inRange(q, Index(next.size())) || Index(q) == p || stamp[size_t(q)] == mark)
            return;
        step(p, Index(q));
        p = Index(q);
        stamp[p] = mark;
    }
}

void pathNodes()
{
    registerDecl({
        .type = "ShortestEdgePaths",
        .label = "Shortest Edge Paths",
        .category = "Mesh Topology",
        .doc = "For every point, the neighbouring point on the cheapest path along edges to the nearest end vertex, and that path's cost. "
               "End vertices and points with no path point to themselves with a cost of 0.",
        .sockets = {.inputs = {fin("endVertex", T::Bool, false, Implicit::None, "Where paths end, read per point."),
                               fin("edgeCost", T::Float, 1.0, Implicit::None, "Cost of crossing each edge; negative counts as 0.")},
                    .outputs = {fout("nextVertexIndex", T::Int), fout("totalCost", T::Float)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr ends = c.field("endVertex"), cost = c.field("edgeCost");
                auto solve = [ends, cost](const geo::Mesh& m) {
                    return shortestPaths(m, bools(ends, FieldContext::forMesh(m, Domain::Point)), floats(cost, FieldContext::forMesh(m, Domain::Edge)));
                };
                c.out("nextVertexIndex", onDomain(T::Int, Domain::Point, [solve](const geo::Mesh& m) { return Column::of(solve(m).next); }));
                c.out("totalCost", onDomain(T::Float, Domain::Point, [solve](const geo::Mesh& m) { return Column::of(solve(m).cost); }));
            },
    });

    auto pathInputs = [] {
        return std::vector<SocketDecl>{fin("startVertices", T::Bool, true, Implicit::None, "Points to start a path from."),
                                       fin("nextVertexIndex", T::Int, -1, Implicit::None, "Each point's next point along its path, per point.")};
    };
    registerDecl({
        .type = "EdgePathsToCurves",
        .label = "Edge Paths to Curves",
        .category = "Mesh Topology",
        .doc = "A poly curve from each start vertex following the next vertex indices until a path stops, leaves the mesh or loops back. "
               "Paths of a single point make no curve.",
        .sockets = {.inputs = [&] {
                        std::vector<SocketDecl> s = {geoIn("mesh")};
                        for (SocketDecl& d : pathInputs())
                            s.push_back(std::move(d));
                        return s;
                    }(),
                    .outputs = {geoOut("curves")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("mesh");
                CurveSet cs;
                if (g->mesh)
                {
                    const geo::Mesh& m = *g->mesh;
                    FieldContext ctx = FieldContext::forMesh(m, Domain::Point);
                    std::vector<uint8_t> start = bools(c.field("startVertices"), ctx);
                    std::vector<int64_t> next = ints(c.field("nextVertexIndex"), ctx);
                    std::vector<uint32_t> stamp(m.pointCapacity(), 0);
                    uint32_t mark = 0;
                    std::vector<D3> pts;
                    for (Index p = 0; p < m.pointCapacity(); ++p)
                    {
                        if (!m.pointValid(p) || !start[p])
                            continue;
                        pts.assign(1, m.position(p));
                        walkPath(p, next, stamp, ++mark, [&](Index, Index q) { pts.push_back(m.position(q)); });
                        if (pts.size() > 1)
                            cs.addCurve(pts, false);
                    }
                }
                c.out("curves", curveGeometry(std::move(cs)));
            },
    });
    registerDecl({
        .type = "EdgePathsToSelection",
        .label = "Edge Paths to Selection",
        .category = "Mesh Topology",
        .doc = "Selects the edges walked from each start vertex following the next vertex indices.",
        .sockets = {.inputs = pathInputs(), .outputs = {fout("selection", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr start = c.field("startVertices"), nextField = c.field("nextVertexIndex");
                c.out("selection", onDomain(T::Bool, Domain::Edge, [start, nextField](const geo::Mesh& m) {
                          FieldContext ctx = FieldContext::forMesh(m, Domain::Point);
                          std::vector<uint8_t> s = bools(start, ctx);
                          std::vector<int64_t> next = ints(nextField, ctx);
                          std::vector<uint8_t> sel(m.edgeCapacity(), 0);
                          std::vector<uint32_t> stamp(m.pointCapacity(), 0);
                          uint32_t mark = 0;
                          for (Index p = 0; p < m.pointCapacity(); ++p)
                              if (m.pointValid(p) && s[p])
                                  walkPath(p, next, stamp, ++mark, [&](Index a, Index b) {
                                      Index e = m.findEdge(a, b);
                                      if (e != geo::kInvalid)
                                          sel[e] = 1;
                                  });
                          return Column::of(std::move(sel));
                      }));
            },
    });
}

/** A copy of m with points, edges and faces in the given orders (each new-to-old), attributes carried along. */
geo::Mesh reorderedMesh(const geo::Mesh& m, const std::vector<Index>& points, const std::vector<Index>& edges, const std::vector<Index>& faces)
{
    geo::Mesh out;
    for (int d = 0; d < 4; ++d)
        out.attributes(geo::Domain(d)).mergeLayout(m.attributes(geo::Domain(d)));
    out.materialNames() = m.materialNames();
    out.colorSetInfo() = m.colorSetInfo();
    out.reserve(m.numPoints(), m.numEdges(), m.numFaces(), m.numCorners());
    std::vector<Index> newPoint(m.pointCapacity(), geo::kInvalid);
    for (Index old : points)
    {
        Index p = out.addPoint(m.position(old));
        out.pointAttrs().copyFrom(m.pointAttrs(), p, old);
        newPoint[old] = p;
    }
    // Edges before faces so faces reuse them and edge ids follow the requested order.
    for (Index old : edges)
    {
        Index e = out.addEdge(newPoint[m.edgePoint(old, 0)], newPoint[m.edgePoint(old, 1)]);
        out.edgeAttrs().copyFrom(m.edgeAttrs(), e, old);
    }
    std::vector<Index> pts;
    for (Index old : faces)
    {
        pts.clear();
        m.forEachFaceCorner(old, [&](Index k) { pts.push_back(newPoint[m.cornerPoint(k)]); });
        Index f = out.addFace(pts);
        if (f == geo::kInvalid)
            continue;
        out.faceAttrs().copyFrom(m.faceAttrs(), f, old);
        m.forEachFaceCorner(old, [&](Index k) {
            Index nk = out.findCorner(f, newPoint[m.cornerPoint(k)]);
            if (nk != geo::kInvalid)
                out.cornerAttrs().copyFrom(m.cornerAttrs(), nk, k);
        });
    }
    return out;
}

/** New-to-old order: selected elements sorted by group then weight fill the slots the selected elements held. */
std::vector<Index> sortOrder(size_t n, const std::vector<uint8_t>& sel, const std::vector<int64_t>& group, const std::vector<double>& weight)
{
    std::vector<Index> order(n);
    std::iota(order.begin(), order.end(), Index(0));
    std::vector<Index> picked;
    for (Index k = 0; k < n; ++k)
        if (sel[k])
            picked.push_back(k);
    std::vector<Index> slots = picked;
    std::stable_sort(picked.begin(), picked.end(), [&](Index a, Index b) {
        if (group[a] != group[b])
            return group[a] < group[b];
        return weight[a] < weight[b];
    });
    for (size_t k = 0; k < slots.size(); ++k)
        order[slots[k]] = picked[k];
    return order;
}

std::vector<Index> identity(size_t n)
{
    std::vector<Index> v(n);
    std::iota(v.begin(), v.end(), Index(0));
    return v;
}

void sortNode()
{
    registerDecl({
        .type = "SortElements",
        .label = "Sort Elements",
        .category = "Mesh Topology",
        .doc = "Reorders elements by group ID, then by weight within a group. Selected elements are sorted among the places they held; the "
               "rest stay where they are. Mesh points, edges and faces, point clouds and instances.",
        .props = {prop("domain", {"point", "edge", "face", "instance"}, "point")},
        .sockets = {.inputs = {geoIn(), selectionIn(), fin("groupId", T::Int, 0), fin("sortWeight", T::Float, 0.0)}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("geometry");
                Domain d = domainOf(c.prop("domain"));
                FieldPtr sel = c.field("selection"), group = c.field("groupId"), weight = c.field("sortWeight");
                auto orderIn = [&](const FieldContext& ctx) {
                    return sortOrder(ctx.size, bools(sel, ctx), ints(group, ctx), floats(weight, ctx));
                };
                Geometry out = *g;
                if (g->mesh && d <= Domain::Face)
                {
                    const geo::Mesh& m = *g->mesh;
                    std::vector<Index> order = orderIn(FieldContext::forMesh(m, d));
                    out.mesh = std::make_shared<const geo::Mesh>(reorderedMesh(m, d == Domain::Point ? order : identity(m.pointCapacity()),
                                                                               d == Domain::Edge ? order : identity(m.edgeCapacity()),
                                                                               d == Domain::Face ? order : identity(m.faceCapacity())));
                }
                if (g->points && d == Domain::Point)
                {
                    geo::Mesh p = *g->points;
                    std::vector<Index> order = orderIn(FieldContext::forPoints(p));
                    std::vector<D3> pos(p.positions().begin(), p.positions().end());
                    for (Index k = 0; k < order.size(); ++k)
                        p.setPosition(k, pos[order[k]]);
                    p.pointAttrs().reorder(order);
                    out.points = std::make_shared<const geo::Mesh>(std::move(p));
                }
                if (g->instances && d == Domain::Instance)
                {
                    Instances inst = *g->instances;
                    std::vector<Index> order = orderIn(FieldContext::forInstances(inst));
                    Instances src = inst;
                    for (size_t k = 0; k < order.size(); ++k)
                    {
                        inst.transforms[k] = src.transforms[order[k]];
                        inst.reference[k] = src.reference[order[k]];
                    }
                    inst.attrs.reorder(order);
                    out.instances = std::make_shared<const Instances>(std::move(inst));
                }
                c.out("geometry", makeGeometry(std::move(out)));
            },
    });
}

void faceGroupNodes()
{
    registerDecl({
        .type = "EdgesToFaceGroups",
        .label = "Edges to Face Groups",
        .category = "Mesh Topology",
        .doc = "Groups faces connected without crossing a boundary edge. Group IDs run from 0.",
        .sockets = {.inputs = {fin("boundaryEdges", T::Bool, true, Implicit::None, "Edges that separate groups.")}, .outputs = {fout("faceGroupId", T::Int)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr boundary = c.field("boundaryEdges");
                c.out("faceGroupId", onDomain(T::Int, Domain::Face, [boundary](const geo::Mesh& m) {
                          std::vector<uint8_t> block = bools(boundary, FieldContext::forMesh(m, Domain::Edge));
                          std::vector<Index> comp;
                          geo::faceComponents(m, comp, block);
                          std::vector<int64_t> out(m.faceCapacity(), 0);
                          for (Index f = 0; f < m.faceCapacity(); ++f)
                              if (comp[f] != geo::kInvalid)
                                  out[f] = comp[f];
                          return Column::of(std::move(out));
                      }));
            },
    });
    registerDecl({
        .type = "FaceGroupBoundaries",
        .label = "Face Group Boundaries",
        .category = "Mesh Topology",
        .doc = "Selects edges between faces of different groups. Edges with a single face are not boundaries.",
        .sockets = {.inputs = {fin("faceGroupId", T::Int, 0, Implicit::None, "Group of each face.")}, .outputs = {fout("boundaryEdges", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr group = c.field("faceGroupId");
                c.out("boundaryEdges", onDomain(T::Bool, Domain::Edge, [group](const geo::Mesh& m) {
                          std::vector<int64_t> id = ints(group, FieldContext::forMesh(m, Domain::Face));
                          std::vector<uint8_t> out(m.edgeCapacity(), 0);
                          for (Index e = 0; e < m.edgeCapacity(); ++e)
                          {
                              if (!m.edgeValid(e) || m.edgeCorner(e) == geo::kInvalid)
                                  continue;
                              int64_t first = id[m.cornerFace(m.edgeCorner(e))];
                              m.forEachEdgeCorner(e, [&](Index k) {
                                  if (id[m.cornerFace(k)] != first)
                                      out[e] = 1;
                              });
                          }
                          return Column::of(std::move(out));
                      }));
            },
    });
}
} // namespace topology

void registerTopologyNodes()
{
    topology::measureNodes();
    topology::neighborNodes();
    topology::islandNode();
    topology::cornerNodes();
    topology::pathNodes();
    topology::sortNode();
    topology::faceGroupNodes();
}
} // namespace lila::gn::nodes
