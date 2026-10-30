#include "gn/NodesCommon.h"

#include "geo/BoundingShapes.h"
#include "geo/Bvh.h"
#include "geo/MultiBox.h"
#include "geo/Primitives.h"
#include "geo/Reduce.h"
#include "retopo/Retopo.h"
#include "uv/Uv.h"

#include <numeric>
#include <sstream>
#include <unordered_map>

namespace lila::gn::nodes::game
{
using D3 = glm::dvec3;
using D4 = glm::dvec4;

/** Faces grouped by connectivity through shared points. */
std::vector<std::vector<Index>> faceIslands(const geo::Mesh& m)
{
    std::vector<Index> parent(m.pointCapacity());
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index p) {
        while (parent[p] != p)
            p = parent[p] = parent[parent[p]];
        return p;
    };
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        Index first = find(m.cornerPoint(m.faceCorner(f)));
        m.forEachFaceCorner(f, [&](Index c) {
            Index r = find(m.cornerPoint(c));
            if (r != first)
                parent[r] = first;
        });
    }
    std::unordered_map<Index, size_t> islandOf;
    std::vector<std::vector<Index>> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        Index r = find(m.cornerPoint(m.faceCorner(f)));
        auto [it, added] = islandOf.try_emplace(r, out.size());
        if (added)
            out.emplace_back();
        out[it->second].push_back(f);
    }
    return out;
}

/** Faces grouped by an int face attribute, or by island when the mesh doesn't have it. */
std::vector<std::vector<Index>> faceGroups(const geo::Mesh& m, std::string_view attr)
{
    auto ids = m.faceAttrs().get<int32_t>(attr);
    if (ids.empty())
        return faceIslands(m);
    std::map<int32_t, std::vector<Index>> byId;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            byId[ids[f]].push_back(f);
    std::vector<std::vector<Index>> out;
    for (auto& [id, faces] : byId)
        out.push_back(std::move(faces));
    return out;
}

std::vector<Index> groupPoints(const geo::Mesh& m, std::span<const Index> faces)
{
    std::vector<Index> pts;
    std::vector<uint8_t> seen(m.pointCapacity(), 0);
    for (Index f : faces)
        m.forEachFaceCorner(f, [&](Index c) {
            Index p = m.cornerPoint(c);
            if (!seen[p])
            {
                seen[p] = 1;
                pts.push_back(p);
            }
        });
    return pts;
}

std::vector<D3> positionsOf(const geo::Mesh& m, std::span<const Index> pts)
{
    std::vector<D3> out;
    out.reserve(pts.size());
    for (Index p : pts)
        out.push_back(m.position(p));
    return out;
}

/** The faces as a mesh of their own, positions only. */
geo::Mesh subMesh(const geo::Mesh& m, std::span<const Index> faces)
{
    geo::Mesh out;
    std::unordered_map<Index, Index> map;
    std::vector<Index> pts;
    for (Index f : faces)
    {
        pts.clear();
        m.forEachFaceCorner(f, [&](Index c) {
            Index p = m.cornerPoint(c);
            auto [it, added] = map.try_emplace(p, Index(0));
            if (added)
                it->second = out.addPoint(m.position(p));
            pts.push_back(it->second);
        });
        out.addFace(std::span<const Index>(pts));
    }
    return out;
}

size_t triangleCount(const geo::Mesh& m)
{
    size_t n = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceSize(f) >= 3)
            n += m.faceSize(f) - 2;
    return n;
}

glm::dmat4 frameMatrix(const glm::dmat3& axes, const D3& center)
{
    glm::dmat4 xf(1.0);
    for (int i = 0; i < 3; ++i)
        xf[i] = glm::dvec4(axes[i], 0.0);
    xf[3] = glm::dvec4(center, 1.0);
    return xf;
}

/** Point normals as area-weighted face normals. */
std::vector<D3> smoothPointNormals(const geo::Mesh& m)
{
    std::vector<D3> n(m.pointCapacity(), D3(0.0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            D3 a = m.faceAreaVector(f);
            m.forEachFaceCorner(f, [&](Index c) { n[m.cornerPoint(c)] += a; });
        }
    for (D3& v : n)
        v = glm::length(v) > 1e-20 ? glm::normalize(v) : D3(0.0, 1.0, 0.0);
    return n;
}

GeometryPtr requireMesh(ExecContext& c, const char* id = "mesh")
{
    GeometryPtr g = c.geometry(id);
    if (!g->mesh || g->mesh->numFaces() == 0)
        return nullptr;
    return g;
}

/** Copy-on-write edit of just the mesh, everything else passed through. */
GeometryPtr withMesh(const GeometryPtr& g, const std::function<void(geo::Mesh&)>& fn)
{
    return editComponents(g, {.mesh = fn});
}

std::vector<double> parseNumbers(const std::string& s)
{
    std::vector<double> out;
    std::string text = s;
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream in(text);
    double v;
    while (in >> v)
        out.push_back(v);
    return out;
}

// LOD chain

void lodChain()
{
    registerDecl({
        .type = "LodChain",
        .label = "LOD Chain",
        .category = "Game",
        .doc = "Levels of detail: LOD0 is the mesh, each further level keeps ratio^level of its triangles. Gives one instance per "
               "level (identity transforms, or spaced along X for a preview) with \"lod\" and \"screenSize\" instance attributes, "
               "and the mesh of one level.",
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("levels", T::Int, 3, "Reduced levels after LOD0 (Unreal takes 8 in all)."), 0, 7),
                               bounded(in("ratio", T::Float, 0.5, "Triangles kept per level, relative to the one before."), 0.01, 1.0),
                               in("screenSizes", T::String, "1 0.5 0.25 0.125", "Unreal screen size per level; missing levels halve the last."),
                               in("keepBorders", T::Bool, true, "Open borders stay in place."),
                               bounded(in("spacing", T::Float, 0.0, "Offset between levels along X, for viewing them side by side."), 0.0),
                               bounded(in("level", T::Int, 0, "The level given on the mesh output."), 0, 7)},
                    .outputs = {geoOut("lods"), geoOut("mesh"), fout("triangles", T::Int, "Triangles in the level on the mesh output.")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                if (!g)
                {
                    c.out("lods", makeGeometry({}));
                    c.out("mesh", makeGeometry({}));
                    return;
                }
                const geo::Mesh& base = *g->mesh;
                const int levels = int(std::clamp<int64_t>(c.i("levels"), 0, 7));
                const double ratio = std::clamp(c.f("ratio"), 0.01, 1.0);
                std::vector<double> sizes = parseNumbers(c.s("screenSizes"));
                auto inst = std::make_shared<Instances>();
                std::vector<double> screen;
                std::vector<int64_t> lod;
                for (int k = 0; k <= levels; ++k)
                {
                    geo::Mesh level;
                    if (k == 0)
                        level = base;
                    else
                    {
                        geo::ReduceOptions o;
                        o.ratio = std::pow(ratio, double(k));
                        o.keepBorders = c.b("keepBorders");
                        level = geo::reduceMesh(base, o).mesh;
                        level.compact();
                    }
                    inst->references.push_back(meshGeometry(std::move(level)));
                    glm::dmat4 xf(1.0);
                    xf[3] = glm::dvec4(c.f("spacing") * k, 0.0, 0.0, 1.0);
                    inst->add(xf, k);
                    double s = k < int(sizes.size()) ? sizes[k] : (sizes.empty() ? std::pow(0.5, k) : sizes.back() * std::pow(0.5, k - int(sizes.size()) + 1));
                    screen.push_back(s);
                    lod.push_back(k);
                }
                writeAttribute(*inst, "lod", Column::of(lod), T::Int);
                writeAttribute(*inst, "screenSize", Column::of(screen), T::Float);
                int pick = int(std::clamp<int64_t>(c.i("level"), 0, levels));
                GeometryPtr picked = inst->references[size_t(pick)];
                c.out("mesh", picked);
                c.out("triangles", constantInt(int64_t(triangleCount(*picked->mesh))));
                Geometry out;
                out.instances = std::move(inst);
                c.out("lods", makeGeometry(std::move(out)));
            },
    });
}

// Collision

enum Shape : int32_t
{
    Hull = 0,  // UCX_
    Box = 1,   // UBX_
    Sphere = 2, // USP_
    Capsule = 3, // UCP_
};

struct Piece
{
    geo::Mesh mesh;
    Shape shape = Hull;
};

void collision()
{
    registerDecl({
        .type = "Collision",
        .label = "Collision",
        .category = "Game",
        .doc = "Simple collision for Unreal, per island or for the whole mesh: a convex hull, an approximate convex decomposition, "
               "or a fitted box, sphere or capsule. Gives the pieces as one mesh with face attributes \"collision\" (piece) and "
               "\"collisionShape\" (0 UCX_ hull, 1 UBX_ box, 2 USP_ sphere, 3 UCP_ capsule), the prefix each piece exports with.",
        .props = {prop("shape", {"hull", "decompose", "box", "sphere", "capsule"}, "hull"), prop("per", {"island", "whole"}, "island")},
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("maxHulls", T::Int, 8, "Decompose: most pieces per island."), 1, 64),
                               bounded(in("minFill", T::Float, 0.85, "Decompose: how full a piece must be before it stops splitting; higher follows concave shapes closer."), 0.05, 1.0),
                               bounded(in("resolution", T::Int, 48, "Decompose: voxels along the longest side."), 8, 256),
                               bounded(in("maxHullPoints", T::Int, 32, "Points per hull; Unreal keeps hulls light."), 4, 255),
                               in("offset", T::Float, 0.0, "Pushed out on every side.")},
                    .outputs = {geoOut("collision"), fout("count", T::Int, "Pieces made.")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                if (!g)
                {
                    c.out("collision", makeGeometry({}));
                    c.out("count", constantInt(0));
                    return;
                }
                const geo::Mesh& m = *g->mesh;
                const std::string shape = c.prop("shape");
                const double offset = c.f("offset");
                const int hullPoints = int(std::clamp<int64_t>(c.i("maxHullPoints"), 4, 255));
                std::vector<std::vector<Index>> groups;
                if (c.prop("per") == "whole")
                {
                    groups.emplace_back();
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            groups.back().push_back(f);
                }
                else
                    groups = faceIslands(m);

                std::vector<Piece> pieces;
                for (const std::vector<Index>& faces : groups)
                {
                    std::vector<D3> pts = positionsOf(m, groupPoints(m, faces));
                    if (pts.size() < 2)
                        continue;
                    if (shape == "decompose")
                    {
                        geo::MultiBoxOptions o;
                        o.maxBoxes = int(std::clamp<int64_t>(c.i("maxHulls"), 1, 64));
                        o.minFill = std::clamp(c.f("minFill"), 0.05, 1.0);
                        o.resolution = int(std::clamp<int64_t>(c.i("resolution"), 8, 256));
                        o.shape = "hull";
                        o.maxHullPoints = hullPoints;
                        o.padding = offset;
                        geo::MultiBoxResult r = geo::multiBoxDecompose(subMesh(m, faces), o);
                        for (geo::BoxChunk& chunk : r.chunks)
                        {
                            if (chunk.hull && chunk.hullMesh.numFaces() > 0)
                            {
                                pieces.push_back({std::move(chunk.hullMesh), Hull});
                                continue;
                            }
                            D3 s = glm::max(chunk.box.size, D3(1e-4));
                            geo::Mesh box = geo::makeCube({s.x, s.y, s.z});
                            transformMesh(box, frameMatrix(chunk.box.axes, chunk.box.center));
                            pieces.push_back({std::move(box), Box});
                        }
                        continue;
                    }
                    if (shape == "hull")
                    {
                        geo::Mesh hull = geo::convexHullMesh(pts, hullPoints, offset);
                        if (hull.numFaces() > 0)
                            pieces.push_back({std::move(hull), Hull});
                        continue;
                    }
                    if (shape == "sphere")
                    {
                        geo::RoundShape s = geo::minimumSphere(pts);
                        double r = std::max(s.radius + offset, 1e-4);
                        geo::Mesh sphere = geo::makeSphere({r, 16, 8});
                        transformMesh(sphere, frameMatrix(glm::dmat3(1.0), s.center));
                        pieces.push_back({std::move(sphere), Sphere});
                        continue;
                    }
                    std::optional<geo::OrientedBox> fit = geo::minimumVolumeBox(pts);
                    if (!fit)
                        continue;
                    if (shape == "box")
                    {
                        D3 s = glm::max(fit->size + 2.0 * offset, D3(1e-4));
                        geo::Mesh box = geo::makeCube({s.x, s.y, s.z});
                        transformMesh(box, frameMatrix(fit->axes, fit->center));
                        pieces.push_back({std::move(box), Box});
                        continue;
                    }
                    geo::RoundShape s = geo::fitCapsule(pts, fit->axes);
                    double r = std::max(s.radius + offset, 1e-4);
                    double h = std::max(s.height + 2.0 * offset, 2.0 * r);
                    geo::Mesh cap = geo::makeCapsule({r, h, 16, 1, 4});
                    transformMesh(cap, frameMatrix(s.axes, s.center));
                    pieces.push_back({std::move(cap), Capsule});
                }

                geo::Mesh out;
                std::vector<std::pair<Index, Index>> ranges;
                for (Piece& p : pieces)
                {
                    p.mesh.compact();
                    Index before = out.faceCapacity();
                    out.append(p.mesh);
                    ranges.push_back({before, out.faceCapacity()});
                }
                auto piece = out.faceAttrs().getOrAdd<int32_t>("collision");
                for (size_t k = 0; k < ranges.size(); ++k)
                    for (Index f = ranges[k].first; f < ranges[k].second; ++f)
                        piece[f] = int32_t(k);
                auto kind = out.faceAttrs().getOrAdd<int32_t>("collisionShape");
                for (size_t k = 0; k < ranges.size(); ++k)
                    for (Index f = ranges[k].first; f < ranges[k].second; ++f)
                        kind[f] = pieces[k].shape;
                c.out("collision", meshGeometry(std::move(out)));
                c.out("count", constantInt(int64_t(pieces.size())));
            },
    });
}

// Sockets

void socketPoints()
{
    registerDecl({
        .type = "SocketPoints",
        .label = "Socket Points",
        .category = "Game",
        .doc = "Attach points for Unreal sockets: a point per selected element with \"socket\" (its number) and \"rotation\" point "
               "attributes. Align to Normal turns each socket's Y axis to the surface normal, then applies the rotation.",
        .props = {prop("domain", {"point", "face"}, "face")},
        .sockets = {.inputs = {geoIn("geometry"), selectionIn(), fin("position", T::Vector, nullptr, Implicit::Position),
                               fin("rotation", T::Rotation, vec(0, 0, 0)), in("alignToNormal", T::Bool, true)},
                    .outputs = {geoOut("sockets"), fout("count", T::Int)}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("geometry");
                const geo::Mesh* src = g->mesh ? g->mesh.get() : g->points.get();
                geo::Mesh pts;
                if (!src)
                {
                    c.out("sockets", pointGeometry(std::move(pts)));
                    c.out("count", constantInt(0));
                    return;
                }
                Domain d = g->mesh && c.prop("domain") == "face" ? Domain::Face : Domain::Point;
                FieldContext ctx = g->mesh ? FieldContext::forMesh(*src, d) : FieldContext::forPoints(*src);
                std::vector<Index> sel = selected(c.field("selection"), ctx);
                std::vector<D3> pos = evaluateAll<D3>(c.field("position"), ctx);
                std::vector<D3> rot = evaluateAll<D3>(c.field("rotation"), ctx);
                std::vector<D3> nrm = evaluateAll<D3>(normalField(), ctx);
                const bool align = c.b("alignToNormal");
                std::vector<D3> outRot;
                std::vector<int64_t> id;
                for (Index k : sel)
                {
                    pts.addPoint(pos[k]);
                    glm::dmat3 r = eulerToMatrix(rot[k]);
                    if (align && glm::length(nrm[k]) > 1e-12)
                        r = rotationBetween(D3(0, 1, 0), glm::normalize(nrm[k])) * r;
                    outRot.push_back(matrixToEuler(r));
                    id.push_back(int64_t(id.size()));
                }
                writeAttribute(pts, "socket", Domain::Point, Column::of(id), T::Int);
                writeAttribute(pts, "rotation", Domain::Point, Column::of(outRot), T::Rotation);
                c.out("count", constantInt(int64_t(id.size())));
                c.out("sockets", pointGeometry(std::move(pts)));
            },
    });
}

// Pivot Painter

/** Lila's Y-up position as Unreal's Z-up local space, which also flips handedness on import. */
D3 toUnreal(const D3& p) { return {p.x, p.z, p.y}; }

void pivotPainter()
{
    registerDecl({
        .type = "PivotPainter",
        .label = "Pivot Painter",
        .category = "Game",
        .doc = "Bakes each element's pivot and axis into the mesh for foliage and per-piece animation in Unreal materials. Element "
               "pivot xy goes into uv set N and z with the element's 0..1 index into uv set N+1; the vertex colour set holds "
               "the axis from the pivot to the element's farthest point (as 0..1) and, in alpha, its length relative to the longest.",
        .props = {prop("element", {"island", "attribute"}, "island"), prop("pivot", {"boundsBottom", "boundsCenter", "centroid"}, "boundsBottom"),
                  prop("space", {"unreal", "lila"}, "unreal")},
        .sockets = {.inputs = {geoIn("mesh"), in("attribute", T::String, "piece", "Element: a face int attribute naming each element."),
                               bounded(in("uvSet", T::Int, 2, "First of the two uv sets written."), 1, 6),
                               in("colorSet", T::String, "pivotAxis", "Vertex colour set for the axis.")},
                    .outputs = {geoOut("mesh"), fout("elements", T::Int)}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                if (!g)
                {
                    c.out("mesh", c.geometry("mesh"));
                    c.out("elements", constantInt(0));
                    return;
                }
                size_t count = 0;
                std::string colorSet = c.s("colorSet").empty() ? std::string("pivotAxis") : c.s("colorSet");
                c.out("mesh", withMesh(g, [&](geo::Mesh& m) {
                          std::vector<std::vector<Index>> groups = c.prop("element") == "attribute" ? faceGroups(m, c.s("attribute")) : faceIslands(m);
                          count = groups.size();
                          const std::string pivotMode = c.prop("pivot");
                          const bool ue = c.prop("space") == "unreal";
                          std::vector<D3> pivots(groups.size()), axes(groups.size());
                          std::vector<double> lengths(groups.size(), 0.0);
                          double longest = 0.0;
                          for (size_t k = 0; k < groups.size(); ++k)
                          {
                              std::vector<D3> pts = positionsOf(m, groupPoints(m, groups[k]));
                              geo::Bounds b;
                              D3 sum(0.0);
                              for (const D3& p : pts)
                              {
                                  b.extend(p);
                                  sum += p;
                              }
                              D3 pivot = pivotMode == "centroid" ? sum / double(std::max<size_t>(pts.size(), 1)) : b.center();
                              if (pivotMode == "boundsBottom")
                                  pivot.y = b.min.y;
                              D3 far = pivot;
                              for (const D3& p : pts)
                                  if (glm::length(p - pivot) > glm::length(far - pivot))
                                      far = p;
                              D3 axis = far - pivot;
                              lengths[k] = glm::length(axis);
                              longest = std::max(longest, lengths[k]);
                              pivots[k] = ue ? toUnreal(pivot) : pivot;
                              axes[k] = lengths[k] > 1e-12 ? axis / lengths[k] : D3(0, 1, 0);
                              if (ue)
                                  axes[k] = toUnreal(axes[k]);
                          }
                          const int set = int(std::clamp<int64_t>(c.i("uvSet"), 1, 6));
                          auto uvA = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(set));
                          auto uvB = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(set + 1));
                          uvA = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(set));
                          std::vector<D4> faceColor(m.faceCapacity(), D4(0.5, 1.0, 0.5, 1.0));
                          for (size_t k = 0; k < groups.size(); ++k)
                          {
                              float index = groups.size() > 1 ? float(double(k) / double(groups.size() - 1)) : 0.0f;
                              for (Index f : groups[k])
                              {
                                  m.forEachFaceCorner(f, [&](Index cc) {
                                      uvA[cc] = glm::vec2(float(pivots[k].x), float(pivots[k].y));
                                      uvB[cc] = glm::vec2(float(pivots[k].z), index);
                                  });
                                  faceColor[f] = D4(axes[k] * 0.5 + 0.5, longest > 0.0 ? lengths[k] / longest : 0.0);
                              }
                          }
                          writeAttribute(m, colorSet, Domain::Face, Column::of(faceColor), T::Color);
                      }));
                c.out("elements", constantInt(int64_t(count)));
            },
    });
}

// Walkable surfaces

void walkableSurface()
{
    registerDecl({
        .type = "WalkableSurface",
        .label = "Walkable Surface",
        .category = "Game",
        .doc = "Faces a character can stand on: no steeper than the slope, with clearance above (a ray up from the face centre), "
               "plus step faces no taller than the step height beside them. Selects them, or keeps only them.",
        .props = {prop("mode", {"select", "separate"}, "select")},
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("maxSlope", T::Float, 45.0, "Steepest walkable slope, degrees."), 0.0, 90.0),
                               bounded(in("stepHeight", T::Float, 35.0, "Tallest step a character climbs."), 0.0),
                               bounded(in("clearance", T::Float, 180.0, "Free height needed above; 0 skips the test."), 0.0),
                               in("up", T::Vector, vec(0, 1, 0))},
                    .outputs = {geoOut("mesh"), fout("walkable", T::Bool, "Face selection.")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                std::string name = c.anonymous("walkable");
                if (!g)
                {
                    c.out("mesh", c.geometry("mesh"));
                    c.out("walkable", constantBool(false));
                    return;
                }
                D3 up = c.v("up");
                up = glm::length(up) > 1e-12 ? glm::normalize(up) : D3(0, 1, 0);
                const double minCos = std::cos(std::clamp(c.f("maxSlope"), 0.0, 90.0) * kPi / 180.0);
                const double step = std::max(0.0, c.f("stepHeight"));
                const double clearance = std::max(0.0, c.f("clearance"));
                const geo::Mesh& src = *g->mesh;
                std::vector<uint8_t> flat(src.faceCapacity(), 0), walk(src.faceCapacity(), 0);
                geo::MeshBvh bvh;
                if (clearance > 0.0)
                    bvh.build(src);
                const double eps = std::max(1e-4, glm::length(src.bounds().size()) * 1e-6);
                for (Index f = 0; f < src.faceCapacity(); ++f)
                {
                    if (!src.faceValid(f) || glm::dot(src.faceNormal(f), up) < minCos)
                        continue;
                    D3 o = src.faceCentroid(f) + up * eps;
                    flat[f] = clearance <= 0.0 || !bvh.occluded(o, up, clearance);
                }
                walk = flat;
                for (Index f = 0; f < src.faceCapacity(); ++f)
                {
                    if (!src.faceValid(f) || flat[f] || step <= 0.0 || glm::dot(src.faceNormal(f), up) < -0.1)
                        continue;
                    double lo = std::numeric_limits<double>::max(), hi = -lo;
                    bool besideFlat = false;
                    src.forEachFaceCorner(f, [&](Index cc) {
                        double h = glm::dot(src.position(src.cornerPoint(cc)), up);
                        lo = std::min(lo, h);
                        hi = std::max(hi, h);
                        Index other = src.oppositeFace(src.cornerEdge(cc), f);
                        besideFlat = besideFlat || (other != geo::kInvalid && flat[other]);
                    });
                    walk[f] = besideFlat && hi - lo <= step;
                }
                const bool separate = c.prop("mode") == "separate";
                c.out("mesh", withMesh(g, [&](geo::Mesh& m) {
                          if (separate)
                          {
                              for (Index f = 0; f < m.faceCapacity(); ++f)
                                  if (m.faceValid(f) && !walk[f])
                                      m.removeFace(f);
                              m.removeAllLoose();
                              return;
                          }
                          auto dst = m.faceAttrs().getOrAdd<bool>(name);
                          for (Index f = 0; f < m.faceCapacity() && f < walk.size(); ++f)
                              dst[f] = walk[f] != 0;
                      }));
                c.out("walkable", separate ? constantBool(true) : attributeField(name, T::Bool));
            },
    });
}

// Occluder

void occluderMesh()
{
    registerDecl({
        .type = "OccluderMesh",
        .label = "Occluder Mesh",
        .category = "Game",
        .doc = "A closed low-poly proxy that sits inside the mesh, for occlusion culling or a far fallback: voxel remeshed, pulled "
               "in by the inset, then reduced to the triangle budget.",
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("resolution", T::Int, 32, "Voxels along the longest side."), 8, 256),
                               bounded(in("triangles", T::Int, 64, "Triangle budget."), 4),
                               in("inset", T::Float, -1.0, "How far inside the surface; negative is one voxel.")},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                if (!g)
                {
                    c.out("mesh", makeGeometry({}));
                    return;
                }
                retopo::VoxelOptions vo;
                vo.resolution = int(std::clamp<int64_t>(c.i("resolution"), 8, 256));
                vo.project = false;
                geo::Mesh m = retopo::voxelRemesh(*g->mesh, vo);
                m.compact();
                double voxel = std::max({g->mesh->bounds().size().x, g->mesh->bounds().size().y, g->mesh->bounds().size().z}) / vo.resolution;
                double inset = c.f("inset") < 0.0 ? voxel : c.f("inset");
                std::vector<D3> n = smoothPointNormals(m);
                for (Index p = 0; p < m.pointCapacity(); ++p)
                    if (m.pointValid(p))
                        m.setPosition(p, m.position(p) - n[p] * inset);
                geo::ReduceOptions ro;
                ro.targetTriangles = Index(std::max<int64_t>(4, c.i("triangles")));
                ro.keepBorders = false;
                ro.keepHardEdges = false;
                geo::Mesh reduced = geo::reduceMesh(m, ro).mesh;
                reduced.compact();
                c.out("mesh", meshGeometry(std::move(reduced)));
            },
    });
}

// UVs

std::vector<Index> selectedFaces(ExecContext& c, const geo::Mesh& m)
{
    if (c.group.linksInto(c.node.name, "selection").empty() && c.b("selection"))
        return {};
    return selected(c.field("selection"), FieldContext::forMesh(m, Domain::Face));
}

void uvNodes()
{
    registerDecl({
        .type = "LightmapUV",
        .label = "Lightmap UV",
        .category = "Game",
        .doc = "Unreal's lightmap channel: the source set's islands made non-overlapping and packed with padding for the lightmap "
               "resolution into the target set.",
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("resolution", T::Int, 64, "The lightmap resolution in Unreal."), 8, 4096),
                               bounded(in("padding", T::Float, 2.0, "Texels between charts."), 0.0), in("source", T::String, "uv0"),
                               bounded(in("target", T::Int, 1, "UV set written."), 0, 7)},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                if (!g)
                {
                    c.out("mesh", c.geometry("mesh"));
                    return;
                }
                std::string source = c.s("source").empty() ? std::string("uv0") : c.s("source");
                std::string target = geo::attr::uvName(int(std::clamp<int64_t>(c.i("target"), 0, 7)));
                c.out("mesh", withMesh(g, [&](geo::Mesh& m) {
                          uv::LightmapOptions o;
                          o.resolution = int(std::clamp<int64_t>(c.i("resolution"), 8, 4096));
                          o.padding = std::max(0.0, c.f("padding"));
                          o.source = source;
                          o.target = target;
                          if (!uv::generateLightmapUvs(m, o).ok)
                              throw ExecError("Lightmap UV: the charts don't fit at this resolution and padding");
                      }));
            },
    });

    registerDecl({
        .type = "UVUnwrap",
        .label = "UV Unwrap",
        .category = "Game",
        .doc = "Unwraps the selected faces' islands. Seams come from the seam input when it is linked, else the mesh's own seams, "
               "or automatic ones; islands are packed into 0..1 when Pack is on.",
        .props = {prop("method", {"slim", "conformal", "bff"}, "slim"), prop("seams", {"existing", "auto"}, "existing")},
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), fin("seam", T::Bool, false, Implicit::None, "Edges to cut along."),
                               in("uvSet", T::String, "uv0"), in("pack", T::Bool, true), bounded(in("padding", T::Float, 4.0, "Texels between islands when packing."), 0.0),
                               bounded(in("resolution", T::Int, 1024, "Texture size padding is measured in."), 16, 16384)},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                if (!g)
                {
                    c.out("mesh", c.geometry("mesh"));
                    return;
                }
                std::string set = c.s("uvSet").empty() ? std::string("uv0") : c.s("uvSet");
                std::string method = c.prop("method");
                bool seamLinked = !c.group.linksInto(c.node.name, "seam").empty();
                c.out("mesh", withMesh(g, [&](geo::Mesh& m) {
                          std::vector<Index> faces = selectedFaces(c, m);
                          uv::UnwrapOptions uo;
                          uo.method = method == "conformal" ? uv::UnwrapMethod::Conformal : method == "bff" ? uv::UnwrapMethod::Bff : uv::UnwrapMethod::Slim;
                          uv::PackOptions po;
                          po.resolution = int(std::clamp<int64_t>(c.i("resolution"), 16, 16384));
                          po.padding = std::max(0.0, c.f("padding"));
                          if (c.prop("seams") == "auto" && !seamLinked)
                          {
                              uv::AutoUvOptions ao;
                              ao.unwrap = uo;
                              ao.pack = po;
                              uv::autoUv(m, faces, set, ao);
                              return;
                          }
                          if (seamLinked)
                          {
                              FieldContext ectx = FieldContext::forMesh(m, Domain::Edge);
                              std::vector<uint8_t> cut = evaluateAll<uint8_t>(c.field("seam"), ectx);
                              auto seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
                              for (Index e = 0; e < m.edgeCapacity() && e < cut.size(); ++e)
                                  seam[e] = cut[e] != 0;
                          }
                          uv::UnwrapFacesOptions fo;
                          fo.unwrap = uo;
                          fo.keepPlacement = false;
                          fo.pack = c.b("pack");
                          fo.packing = po;
                          uv::unwrapFaces(m, faces, set, fo);
                      }));
            },
    });

    registerDecl({
        .type = "PackUVIslands",
        .label = "Pack UV Islands",
        .category = "Game",
        .doc = "Packs the selected faces' uv islands into 0..1 with padding measured in texels of the resolution.",
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), in("uvSet", T::String, "uv0"), bounded(in("padding", T::Float, 4.0), 0.0),
                               bounded(in("resolution", T::Int, 1024), 16, 16384), in("rotate", T::Bool, true, "Try quarter turns."),
                               in("normalizeScale", T::Bool, true, "Even texel density across islands.")},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                if (!g)
                {
                    c.out("mesh", c.geometry("mesh"));
                    return;
                }
                std::string set = c.s("uvSet").empty() ? std::string("uv0") : c.s("uvSet");
                c.out("mesh", withMesh(g, [&](geo::Mesh& m) {
                          if (!m.cornerAttrs().has(set))
                              throw ExecError("Pack UV Islands: the mesh has no uv set " + set);
                          uv::PackOptions po;
                          po.resolution = int(std::clamp<int64_t>(c.i("resolution"), 16, 16384));
                          po.padding = std::max(0.0, c.f("padding"));
                          po.rotateStep = c.b("rotate") ? 90 : 0;
                          po.normalizeScale = c.b("normalizeScale");
                          uv::packIslands(m, selectedFaces(c, m), set, po);
                      }));
            },
    });

    registerDecl({
        .type = "TexelDensity",
        .label = "Texel Density",
        .category = "Game",
        .doc = "Texture pixels per scene unit at a texture size (10.24 at 1024 is 1024 px per metre in centimetres). Apply scales "
               "the selected faces' islands about their centres to the density; the density output measures each face after.",
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), in("uvSet", T::String, "uv0"), bounded(in("resolution", T::Int, 1024), 1, 16384),
                               bounded(in("density", T::Float, 10.24, "Target pixels per scene unit."), 0.0), in("apply", T::Bool, true)},
                    .outputs = {geoOut("mesh"), fout("density", T::Float, "Per face.")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = requireMesh(c);
                std::string name = c.anonymous("density");
                if (!g)
                {
                    c.out("mesh", c.geometry("mesh"));
                    c.out("density", constantFloat(0.0));
                    return;
                }
                std::string set = c.s("uvSet").empty() ? std::string("uv0") : c.s("uvSet");
                const double res = double(std::clamp<int64_t>(c.i("resolution"), 1, 16384));
                c.out("mesh", withMesh(g, [&](geo::Mesh& m) {
                          if (!m.cornerAttrs().has(set))
                              throw ExecError("Texel Density: the mesh has no uv set " + set);
                          if (c.b("apply"))
                          {
                              std::vector<Index> faces = selectedFaces(c, m);
                              if (faces.empty())
                                  for (Index f = 0; f < m.faceCapacity(); ++f)
                                      if (m.faceValid(f))
                                          faces.push_back(f);
                              uv::setTexelDensity(m, set, faces, res, std::max(0.0, c.f("density")));
                          }
                          std::vector<float> measured(m.faceCapacity(), 0.0f);
                          for (Index f = 0; f < m.faceCapacity(); ++f)
                              if (m.faceValid(f))
                              {
                                  Index one[1] = {f};
                                  measured[f] = float(uv::texelDensity(m, set, one, res));
                              }
                          auto dst = m.faceAttrs().getOrAdd<float>(name);
                          std::copy(measured.begin(), measured.end(), dst.begin());
                      }));
                c.out("density", attributeField(name, T::Float));
            },
    });
}

// Snap to grid

D3 snap(const D3& p, const D3& grid, const D3& offset)
{
    D3 out = p;
    for (int i = 0; i < 3; ++i)
        if (grid[i] > 1e-12)
            out[i] = std::round((p[i] - offset[i]) / grid[i]) * grid[i] + offset[i];
    return out;
}

void snapToGrid()
{
    registerDecl({
        .type = "SnapToGrid",
        .label = "Snap to Grid",
        .category = "Game",
        .doc = "Rounds selected points, curve points and instance positions to a grid, for modular kits. A rotation step also "
               "rounds instance rotations, in degrees. Zero on an axis leaves it free.",
        .sockets = {.inputs = {geoIn(), selectionIn(), in("grid", T::Vector, vec(50, 50, 50)), in("offset", T::Vector, vec(0, 0, 0)),
                               bounded(in("rotationStep", T::Float, 0.0, "Degrees; 0 keeps rotations."), 0.0, 360.0)},
                    .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                const D3 grid = c.v("grid"), offset = c.v("offset");
                const double step = c.f("rotationStep") * kPi / 180.0;
                auto points = [&](geo::Mesh& m) {
                    FieldContext ctx = m.numFaces() || m.numEdges() ? FieldContext::forMesh(m, Domain::Point) : FieldContext::forPoints(m);
                    for (Index p : selected(c.field("selection"), ctx))
                        if (m.pointValid(p))
                            m.setPosition(p, snap(m.position(p), grid, offset));
                };
                c.out("geometry", editComponents(c.geometry("geometry"), {
                                                                   .mesh = points,
                                                                   .points = points,
                                                                   .curves =
                                                                       [&](CurveSet& cs) {
                                                                           FieldContext ctx = FieldContext::forCurves(cs, Domain::Point);
                                                                           for (Index p : selected(c.field("selection"), ctx))
                                                                               cs.positions[p] = snap(cs.positions[p], grid, offset);
                                                                       },
                                                                   .instances =
                                                                       [&](Instances& inst) {
                                                                           FieldContext ctx = FieldContext::forInstances(inst);
                                                                           for (Index k : selected(c.field("selection"), ctx))
                                                                           {
                                                                               glm::dmat4& xf = inst.transforms[k];
                                                                               xf[3] = glm::dvec4(snap(D3(xf[3]), grid, offset), 1.0);
                                                                               if (step <= 1e-9)
                                                                                   continue;
                                                                               D3 s(glm::length(D3(xf[0])), glm::length(D3(xf[1])), glm::length(D3(xf[2])));
                                                                               glm::dmat3 r(1.0);
                                                                               for (int i = 0; i < 3; ++i)
                                                                                   r[i] = s[i] > 1e-12 ? D3(xf[i]) / s[i] : D3(0.0);
                                                                               D3 e = matrixToEuler(r);
                                                                               for (int i = 0; i < 3; ++i)
                                                                                   e[i] = std::round(e[i] / step) * step;
                                                                               xf = composeTransform(D3(xf[3]), e, s);
                                                                           }
                                                                       },
                                                               }));
            },
    });
}

// Materials

/** Bool per face: the face's material slot is named the material; adapted to the context's domain. */
class MaterialSelectionField : public Field
{
public:
    explicit MaterialSelectionField(std::string material) : Field(SocketType::Bool), mMaterial(std::move(material)) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Mesh || !ctx.mesh)
            return Column::of(uint8_t(0));
        const geo::Mesh& m = *ctx.mesh;
        const auto& names = m.materialNames();
        auto it = std::find(names.begin(), names.end(), mMaterial);
        if (it == names.end())
            return Column::of(uint8_t(0));
        const int32_t slot = int32_t(it - names.begin());
        auto ids = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        std::vector<uint8_t> v(m.faceCapacity(), 0);
        for (Index f = 0; f < m.faceCapacity(); ++f)
            v[f] = (f < ids.size() ? ids[f] : 0) == slot;
        Column col = Column::of(std::move(v));
        return ctx.domain == Domain::Face ? col : adaptDomain(m, col, Domain::Face, ctx.domain);
    }

private:
    std::string mMaterial;
};

void materials()
{
    registerDecl({
        .type = "SetMaterial",
        .label = "Set Material",
        .category = "Game",
        .doc = "Assigns a material by name to the selected faces, adding its slot when the mesh doesn't have one.",
        .sockets = {.inputs = {geoIn("geometry"), fin("selection", T::Bool, true, Implicit::None, "Faces to assign."), in("material", T::String, "")},
                    .outputs = {geoOut("geometry")}},
        .exec =
            [](ExecContext& c) {
                const std::string material = c.s("material");
                c.out("geometry", withMesh(c.geometry("geometry"), [&](geo::Mesh& m) {
                          FieldContext ctx = FieldContext::forMesh(m, Domain::Face);
                          std::vector<Index> sel = selected(c.field("selection"), ctx);
                          if (sel.empty())
                              return;
                          auto& names = m.materialNames();
                          bool hadSlots = m.faceAttrs().has(geo::attr::kMaterial) || !names.empty();
                          // Unselected faces keep the default slot 0 rather than taking this material.
                          if (!hadSlots && sel.size() < m.numFaces())
                              names.push_back("");
                          auto it = std::find(names.begin(), names.end(), material);
                          int32_t slot = int32_t(it - names.begin());
                          if (it == names.end())
                              names.push_back(material);
                          auto ids = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
                          for (Index f : sel)
                              ids[f] = slot;
                      }));
            },
    });

    registerDecl({
        .type = "SetMaterialIndex",
        .label = "Set Material Index",
        .category = "Game",
        .doc = "Sets the selected faces' material slot by number.",
        .sockets = {.inputs = {geoIn("geometry"), fin("selection", T::Bool, true), bounded(fin("materialIndex", T::Int, 0), 0.0)},
                    .outputs = {geoOut("geometry")}},
        .exec =
            [](ExecContext& c) {
                c.out("geometry", withMesh(c.geometry("geometry"), [&](geo::Mesh& m) {
                          FieldContext ctx = FieldContext::forMesh(m, Domain::Face);
                          std::vector<Index> sel = selected(c.field("selection"), ctx);
                          std::vector<int64_t> index = evaluateAll<int64_t>(c.field("materialIndex"), ctx);
                          auto ids = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
                          for (Index f : sel)
                              ids[f] = int32_t(std::max<int64_t>(0, index[f]));
                      }));
            },
    });

    registerDecl({
        .type = "MaterialSelection",
        .label = "Material Selection",
        .category = "Game",
        .doc = "True on faces using the named material.",
        .sockets = {.inputs = {in("material", T::String, "")}, .outputs = {fout("selection", T::Bool)}},
        .exec = [](ExecContext& c) { c.out("selection", FieldPtr(std::make_shared<MaterialSelectionField>(c.s("material")))); },
    });

    registerDecl({
        .type = "MaterialIndex",
        .label = "Material Index",
        .category = "Game",
        .doc = "Each face's material slot number.",
        .sockets = {.outputs = {fout("materialIndex", T::Int)}},
        .exec = [](ExecContext& c) { c.out("materialIndex", attributeField(std::string(geo::attr::kMaterial), T::Int)); },
    });
}
} // namespace lila::gn::nodes::game

namespace lila::gn::nodes
{
void registerGameNodes()
{
    game::lodChain();
    game::collision();
    game::socketPoints();
    game::pivotPainter();
    game::walkableSurface();
    game::occluderMesh();
    game::uvNodes();
    game::snapToGrid();
    game::materials();
}
} // namespace lila::gn::nodes
