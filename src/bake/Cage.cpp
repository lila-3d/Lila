#include "bake/Cage.h"

#include "bake/BakeNodes.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace lila::bake
{
namespace
{
double edge2(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

/** Each point once, at its largest weight. */
void mergeWeights(std::vector<std::pair<geo::Index, float>>& w)
{
    std::ranges::sort(w, [](const auto& a, const auto& b) { return a.first < b.first || (a.first == b.first && a.second > b.second); });
    w.erase(std::unique(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), w.end());
}

glm::dmat4 worldOf(scene::Graph& g, scene::NodeId id)
{
    const scene::PlugValue v = g.value({id, scene::Transform::kWorldMatrix, -1});
    return std::holds_alternative<glm::dmat4>(v) ? std::get<glm::dmat4>(v) : glm::dmat4(1.0);
}

scene::MeshPtr exported(scene::Graph& g, scene::NodeId id)
{
    scene::MeshPtr m = scene::modifiers::exportMesh(g, id);
    if (!m)
        m = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1}));
    return m;
}

scene::Node* meshByName(scene::Graph& g, const std::string& name)
{
    try
    {
        scene::Node* n = g.find(name);
        return n && n->type().isA("mesh") ? n : nullptr;
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** World positions as a mesh at the scene root: the low's topology with the given points. */
geo::Mesh withPoints(const geo::Mesh& low, std::span<const glm::dvec3> points)
{
    geo::Mesh m = low;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, points[p]);
    m.cornerAttrs().remove(geo::attr::kNormal);
    return m;
}
} // namespace

std::optional<PaintAttr> paintAttr(std::string_view name)
{
    if (name == "offset" || name == kOffsetAttr)
        return PaintAttr{kOffsetAttr, 1.0f, 0.0f, 10.0f};
    if (name == "skew" || name == kSkewAttr)
        return PaintAttr{kSkewAttr, 0.0f, -1.0f, 1.0f};
    return std::nullopt;
}

std::vector<float> pointValues(const geo::Mesh& mesh, const PaintAttr& attr)
{
    std::vector<float> out(mesh.pointCapacity(), attr.unpainted);
    std::span<const float> v = mesh.pointAttrs().get<float>(attr.name);
    std::copy_n(v.begin(), std::min(v.size(), out.size()), out.begin());
    return out;
}

bool setPointValues(scene::Graph& g, scene::NodeId mesh, const PaintAttr& attr, std::span<const float> values, const std::string& label)
{
    scene::Node* n = g.node(mesh);
    if (!n || !n->type().isA("mesh"))
        return false;
    UndoChunk chunk(label);
    const scene::PlugRef base = scene::modifiers::editPlug(g, mesh);
    const scene::MeshPtr cur = std::get<scene::MeshPtr>(g.storedValue(base));
    if (!cur || cur->pointCapacity() != values.size())
        return false;
    auto m = std::make_shared<geo::Mesh>(*cur);
    geo::Attribute& a = m->pointAttrs().add(attr.name, geo::AttrType::Float);
    a.setDefault(attr.unpainted);
    std::span<float> v = a.values<float>();
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = std::clamp(values[i], attr.min, attr.max);
    g.setValue(base, scene::MeshPtr(std::move(m)));
    return true;
}

std::optional<PaintOp> paintOpFromName(std::string_view name)
{
    if (name == "add")
        return PaintOp::Add;
    if (name == "subtract")
        return PaintOp::Subtract;
    if (name == "smooth")
        return PaintOp::Smooth;
    if (name == "set" || name == "replace")
        return PaintOp::Set;
    return std::nullopt;
}

std::optional<PaintFalloff> paintFalloffFromName(std::string_view name)
{
    if (name == "smooth")
        return PaintFalloff::Smooth;
    if (name == "linear")
        return PaintFalloff::Linear;
    if (name == "constant")
        return PaintFalloff::Constant;
    return std::nullopt;
}

const char* paintFalloffName(PaintFalloff f)
{
    switch (f)
    {
    case PaintFalloff::Smooth: return "smooth";
    case PaintFalloff::Linear: return "linear";
    case PaintFalloff::Constant: return "constant";
    }
    return "smooth";
}

float paintFalloff(PaintFalloff f, double t)
{
    t = std::max(0.0, t);
    if (t >= 1.0)
        return 0.0f;
    switch (f)
    {
    case PaintFalloff::Smooth: return float((1.0 - t * t) * (1.0 - t * t));
    case PaintFalloff::Linear: return float(1.0 - t);
    case PaintFalloff::Constant: return 1.0f;
    }
    return 0.0f;
}

UvLayout::UvLayout(const geo::Mesh& mesh)
{
    LILA_ZONE();

    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>("uv0");
    if (uv.size() != mesh.cornerCapacity() || uv.empty())
        return;
    mUv.assign(uv.size(), glm::dvec2(0.0));
    glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (geo::Index c = 0; c < mesh.cornerCapacity(); ++c)
        if (mesh.cornerValid(c))
        {
            mUv[c] = glm::dvec2(uv[c]);
            lo = glm::min(lo, mUv[c]);
            hi = glm::max(hi, mUv[c]);
        }
    if (lo.x > hi.x)
    {
        mUv.clear();
        return;
    }
    mTris = geo::triangulate(mesh);
    // About four corners per cell.
    const double cells = std::clamp(double(mesh.numCorners()) / 4.0, 1.0, 1e6);
    const glm::dvec2 size = glm::max(hi - lo, glm::dvec2(1e-9));
    mCols = std::clamp(int(std::sqrt(cells * size.x / size.y)), 1, 4096);
    mRows = std::clamp(int(cells / mCols), 1, 4096);
    mMin = lo;
    mCell = size / glm::dvec2(mCols, mRows);
    mCornerCells.assign(size_t(mCols) * size_t(mRows), {});
    mTriCells.assign(size_t(mCols) * size_t(mRows), {});
    int cx = 0, cy = 0;
    for (geo::Index c = 0; c < mesh.cornerCapacity(); ++c)
        if (mesh.cornerValid(c))
            mCornerCells[size_t(cellOf(mUv[c], cx, cy))].push_back(c);
    for (geo::Index t = 0; t < mTris.triangleCount(); ++t)
    {
        glm::dvec2 a(std::numeric_limits<double>::max()), b(-std::numeric_limits<double>::max());
        for (int k = 0; k < 3; ++k)
        {
            a = glm::min(a, mUv[mTris.corners[size_t(t) * 3 + size_t(k)]]);
            b = glm::max(b, mUv[mTris.corners[size_t(t) * 3 + size_t(k)]]);
        }
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        cellOf(a, x0, y0);
        cellOf(b, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                mTriCells[size_t(y) * size_t(mCols) + size_t(x)].push_back(t);
    }
}

int UvLayout::cellOf(const glm::dvec2& uv, int& cx, int& cy) const
{
    cx = std::clamp(int(std::floor((uv.x - mMin.x) / mCell.x)), 0, mCols - 1);
    cy = std::clamp(int(std::floor((uv.y - mMin.y) / mCell.y)), 0, mRows - 1);
    return cy * mCols + cx;
}

bool UvLayout::triangleAt(const glm::dvec2& p, geo::Index& triangle, glm::dvec3& bary) const
{
    if (mUv.empty())
        return false;
    int cx = 0, cy = 0;
    for (geo::Index t : mTriCells[size_t(cellOf(p, cx, cy))])
    {
        const geo::Index* c = &mTris.corners[size_t(t) * 3];
        const glm::dvec2 a = mUv[c[0]], b = mUv[c[1]], d = mUv[c[2]];
        const double area = edge2(a, b, d);
        if (std::abs(area) < 1e-18)
            continue;
        const glm::dvec3 w(edge2(b, d, p) / area, edge2(d, a, p) / area, edge2(a, b, p) / area);
        if (w.x >= -1e-9 && w.y >= -1e-9 && w.z >= -1e-9)
        {
            triangle = t;
            bary = w;
            return true;
        }
    }
    return false;
}

std::vector<std::pair<geo::Index, float>> UvLayout::corners(const glm::dvec2& center, double radius, PaintFalloff falloff) const
{
    std::vector<std::pair<geo::Index, float>> out;
    if (mUv.empty() || radius <= 0.0)
        return out;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    cellOf(center - glm::dvec2(radius), x0, y0);
    cellOf(center + glm::dvec2(radius), x1, y1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            for (geo::Index c : mCornerCells[size_t(y) * size_t(mCols) + size_t(x)])
                if (const double d = glm::length(mUv[c] - center); d <= radius)
                    out.push_back({c, paintFalloff(falloff, d / radius)});
    geo::Index t = geo::kInvalid;
    glm::dvec3 w;
    if (triangleAt(center, t, w))
    {
        const geo::Index f = mTris.faces[t];
        for (geo::Index tt = mTris.faceFirst[f]; tt < mTris.triangleCount() && mTris.faces[tt] == f; ++tt)
            for (int k = 0; k < 3; ++k)
            {
                const geo::Index c = mTris.corners[size_t(tt) * 3 + size_t(k)];
                out.push_back({c, paintFalloff(falloff, glm::length(mUv[c] - center) / radius)});
            }
    }
    mergeWeights(out);
    return out;
}

const char* paintOpName(PaintOp op)
{
    switch (op)
    {
    case PaintOp::Add: return "add";
    case PaintOp::Subtract: return "subtract";
    case PaintOp::Smooth: return "smooth";
    case PaintOp::Set: return "set";
    }
    return "add";
}

PointPaint::PointPaint(std::shared_ptr<const geo::Mesh> mesh, const PaintAttr& attr) : mMesh(std::move(mesh)), mAttr(attr)
{
    LILA_ZONE();

    mValues = pointValues(*mMesh, mAttr);
    mBvh.build(*mMesh);
}

void PointPaint::setSymmetry(int axis)
{
    if (axis == mAxis)
        return;
    mAxis = axis;
    mSymmetry = {};
    if (axis >= 0 && axis <= 2)
    {
        const double tol = std::max(1e-4, glm::length(mMesh->bounds().size()) * 1e-4);
        mSymmetry = geo::SymmetryMap::build(*mMesh, geo::axisPlane(axis), tol);
    }
}

bool PointPaint::raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const { return !mBvh.empty() && mBvh.raycast(origin, dir, hit); }

std::vector<std::pair<geo::Index, float>> PointPaint::brushPoints(const geo::BvhHit& hit, double radius) const
{
    const geo::Mesh& m = *mMesh;
    const auto& tris = mBvh.triangles();
    std::vector<std::pair<geo::Index, float>> out;
    if (hit.triangle == geo::kInvalid || radius <= 0.0)
        return out;
    std::unordered_map<geo::Index, double> dist;
    using Item = std::pair<double, geo::Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    auto relax = [&](geo::Index p, double d) {
        auto it = dist.find(p);
        if (d > radius || (it != dist.end() && it->second <= d))
            return;
        dist[p] = d;
        queue.push({d, p});
    };
    for (int k = 0; k < 3; ++k)
    {
        const geo::Index p = m.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + size_t(k)]);
        relax(p, glm::length(m.position(p) - hit.point));
    }
    while (!queue.empty())
    {
        auto [d, p] = queue.top();
        queue.pop();
        if (d > dist[p])
            continue;
        m.forEachPointEdge(p, [&](geo::Index e) {
            const geo::Index o = m.edgeOtherPoint(e, p);
            relax(o, d + glm::length(m.position(o) - m.position(p)));
        });
    }
    out.reserve(dist.size());
    for (auto [p, d] : dist)
        out.push_back({p, paintFalloff(mFalloff, d / radius)});
    std::ranges::sort(out);
    return out;
}

std::vector<std::pair<geo::Index, float>> PointPaint::pointsNear(const glm::dvec3& center, double radius) const
{
    std::vector<std::pair<geo::Index, float>> out;
    if (radius <= 0.0)
        return out;
    for (geo::Index p = 0; p < mMesh->pointCapacity(); ++p)
        if (mMesh->pointValid(p))
            if (const double d = glm::length(mMesh->position(p) - center); d <= radius)
                out.push_back({p, paintFalloff(mFalloff, d / radius)});
    return out;
}

const UvLayout& PointPaint::uvLayout() const
{
    if (!mUv)
        mUv = std::make_unique<UvLayout>(*mMesh);
    return *mUv;
}

std::vector<std::pair<geo::Index, float>> PointPaint::uvPoints(const glm::dvec2& center, double radius) const
{
    std::vector<std::pair<geo::Index, float>> out;
    for (auto [c, w] : uvLayout().corners(center, radius, mFalloff))
        if (mMesh->cornerValid(c))
            out.push_back({mMesh->cornerPoint(c), w});
    mergeWeights(out);
    return out;
}

void PointPaint::applyOnce(std::span<const std::pair<geo::Index, float>> points, PaintOp op, float value, float strength)
{
    const geo::Mesh& m = *mMesh;
    if (op == PaintOp::Smooth)
    {
        // From a snapshot, so the result doesn't depend on the order points are visited.
        std::vector<float> after;
        after.reserve(points.size());
        for (auto [p, w] : points)
        {
            double sum = 0.0;
            int n = 0;
            m.forEachPointEdge(p, [&](geo::Index e) {
                sum += mValues[m.edgeOtherPoint(e, p)];
                ++n;
            });
            const float avg = n ? float(sum / n) : mValues[p];
            after.push_back(mValues[p] + (avg - mValues[p]) * std::clamp(w * strength, 0.0f, 1.0f));
        }
        for (size_t i = 0; i < points.size(); ++i)
            mValues[points[i].first] = after[i];
        return;
    }
    for (auto [p, w] : points)
    {
        float& v = mValues[p];
        const float a = std::clamp(w * strength, 0.0f, 1.0f);
        switch (op)
        {
        case PaintOp::Add: v += value * a; break;
        case PaintOp::Subtract: v -= value * a; break;
        case PaintOp::Set: v += (value - v) * a; break;
        case PaintOp::Smooth: break;
        }
        v = std::clamp(v, mAttr.min, mAttr.max);
    }
}

void PointPaint::apply(std::span<const std::pair<geo::Index, float>> points, PaintOp op, float value, float strength)
{
    applyOnce(points, op, value, strength);
    if (mSymmetry.empty())
        return;
    std::vector<std::pair<geo::Index, float>> mirrored;
    for (auto [p, w] : points)
        if (const geo::Index q = mSymmetry.mirrorPoint(p); q != geo::kInvalid && q != p)
            mirrored.push_back({q, w});
    mergeWeights(mirrored);
    // A point the dab already reached on its own side keeps that one application.
    std::vector<uint8_t> reached(mValues.size(), 0);
    for (auto [p, w] : points)
        reached[p] = 1;
    std::erase_if(mirrored, [&](const auto& m) { return reached[m.first] != 0; });
    applyOnce(mirrored, op, value, strength);
}

void PointPaint::flood(PaintOp op, float value, float strength, std::span<const geo::Index> mask)
{
    std::vector<std::pair<geo::Index, float>> pts;
    if (mask.empty())
    {
        for (geo::Index p = 0; p < mMesh->pointCapacity(); ++p)
            if (mMesh->pointValid(p))
                pts.push_back({p, 1.0f});
    }
    else
        for (geo::Index p : mask)
            if (p < mMesh->pointCapacity() && mMesh->pointValid(p))
                pts.push_back({p, 1.0f});
    apply(pts, op, value, strength);
}

std::vector<glm::dvec3> autoCagePoints(const geo::Mesh& mesh, const glm::dmat4& world, double frontal, double offset)
{
    return autoCagePoints(mesh, world, frontal, offset, pointValues(mesh, *paintAttr(kOffsetAttr)));
}

std::vector<glm::dvec3> autoCagePoints(const geo::Mesh& mesh, const glm::dmat4& world, double frontal, double offset, std::span<const float> scale)
{
    LILA_ZONE();

    const std::vector<glm::vec3> smooth = geo::pointNormals(mesh);
    const glm::dmat3 nm = glm::transpose(glm::inverse(glm::dmat3(world)));
    std::vector<glm::dvec3> out(mesh.pointCapacity(), glm::dvec3(0.0));
    tbb::parallel_for(geo::Index(0), mesh.pointCapacity(), [&](geo::Index p) {
        if (!mesh.pointValid(p))
            return;
        glm::dvec3 n = nm * glm::dvec3(smooth[p]);
        const double l = glm::length(n);
        n = l > 0.0 ? n / l : glm::dvec3(0.0, 0.0, 1.0);
        out[p] = glm::dvec3(world * glm::dvec4(mesh.position(p), 1.0)) + n * (frontal * offset * double(p < scale.size() ? scale[p] : 1.0f));
    });
    return out;
}

double groupFrontal(scene::Graph& g, const GroupSettings& s)
{
    if (s.frontal > 0.0)
        return s.frontal;
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const std::string& name : s.lows)
        if (scene::Node* n = meshByName(g, name))
            if (scene::MeshPtr m = exported(g, n->id()))
            {
                const glm::dmat4 w = worldOf(g, n->id());
                for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                    if (m->pointValid(p))
                    {
                        const glm::dvec3 q(w * glm::dvec4(m->position(p), 1.0));
                        lo = glm::min(lo, q);
                        hi = glm::max(hi, q);
                    }
            }
    return lo.x <= hi.x ? 0.03 * glm::length(hi - lo) : 1.0;
}

std::string cageMismatch(const geo::Mesh& low, const geo::Mesh& cage)
{
    if (low.numPoints() != cage.numPoints() || low.pointCapacity() != cage.pointCapacity())
        return "the cage has " + std::to_string(cage.numPoints()) + " points, the low " + std::to_string(low.numPoints());
    if (low.numFaces() != cage.numFaces() || low.faceCapacity() != cage.faceCapacity())
        return "the cage has " + std::to_string(cage.numFaces()) + " faces, the low " + std::to_string(low.numFaces());
    for (geo::Index f = 0; f < low.faceCapacity(); ++f)
    {
        if (low.faceValid(f) != cage.faceValid(f))
            return "face " + std::to_string(f) + " exists in only one of them";
        if (!low.faceValid(f))
            continue;
        const std::vector<geo::Index> a = low.faceCorners(f), b = cage.faceCorners(f);
        if (a.size() != b.size())
            return "face " + std::to_string(f) + " has a different corner count";
        for (size_t k = 0; k < a.size(); ++k)
            if (low.cornerPoint(a[k]) != cage.cornerPoint(b[k]))
                return "face " + std::to_string(f) + " joins different points";
    }
    return {};
}

scene::NodeId cageLow(scene::Graph& g, const scene::Node& group)
{
    const GroupSettings s = groupSettings(group);
    if (s.lows.size() != 1)
        throw std::runtime_error(group.name() + ": a cage needs a group with exactly one low (it has " + std::to_string(s.lows.size()) + ")");
    scene::Node* low = meshByName(g, s.lows.front());
    if (!low)
        throw std::runtime_error(group.name() + ": low " + s.lows.front() + " is not a mesh in the scene");
    return low->id();
}

scene::Node* createCage(scene::Graph& g, scene::NodeId group, const std::string& name)
{
    LILA_ZONE();

    scene::Node* grp = g.node(group);
    if (!grp || !isGroup(*grp))
        throw std::runtime_error("not a bake group");
    g.evaluate();
    const scene::NodeId lowId = cageLow(g, *grp);
    scene::Node* low = g.node(lowId);
    GroupSettings s = groupSettings(*grp);
    scene::MeshPtr m = exported(g, lowId);
    if (!m)
        throw std::runtime_error(low->name() + " has no mesh");
    const std::vector<glm::dvec3> pts = autoCagePoints(*m, worldOf(g, lowId), groupFrontal(g, s), s.cageOffset);

    UndoChunk chunk("Create Bake Cage");
    scene::Node* cage = g.createNode(*scene::types::find("mesh"), name.empty() ? low->name() + "_cage" : name);
    g.setValue({cage->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(withPoints(*m, pts))));
    s.cage = cage->path();
    setGroupSettings(g, group, s);
    return cage;
}

void cageFromPaint(scene::Graph& g, scene::NodeId group)
{
    LILA_ZONE();

    scene::Node* grp = g.node(group);
    if (!grp || !isGroup(*grp))
        throw std::runtime_error("not a bake group");
    g.evaluate();
    const GroupSettings s = groupSettings(*grp);
    scene::Node* cage = s.cage.empty() ? nullptr : meshByName(g, s.cage);
    if (!cage)
        throw std::runtime_error(grp->name() + " has no editable cage (bakeCage create=True makes one)");
    const scene::NodeId lowId = cageLow(g, *grp);
    scene::MeshPtr m = exported(g, lowId);
    const scene::PlugRef base = scene::modifiers::editPlug(g, cage->id());
    const scene::MeshPtr cur = std::get<scene::MeshPtr>(g.storedValue(base));
    if (!m || !cur)
        throw std::runtime_error(grp->name() + ": no mesh to build the cage from");
    if (const std::string why = cageMismatch(*m, *cur); !why.empty())
        throw std::runtime_error(cage->name() + " doesn't match " + g.node(lowId)->name() + ": " + why);
    std::vector<glm::dvec3> pts = autoCagePoints(*m, worldOf(g, lowId), groupFrontal(g, s), s.cageOffset);
    // The cage node may have been moved since it was made; its points are stored in its own object space.
    const glm::dmat4 inv = glm::inverse(worldOf(g, cage->id()));
    for (glm::dvec3& p : pts)
        p = glm::dvec3(inv * glm::dvec4(p, 1.0));
    auto out = std::make_shared<geo::Mesh>(*cur);
    for (geo::Index p = 0; p < out->pointCapacity(); ++p)
        if (out->pointValid(p))
            out->setPosition(p, pts[p]);
    UndoChunk chunk("Bake Paint Into Cage");
    g.setValue(base, scene::MeshPtr(std::move(out)));
}

std::string resetCage(scene::Graph& g, scene::NodeId group, bool deleteNode)
{
    scene::Node* grp = g.node(group);
    if (!grp || !isGroup(*grp))
        throw std::runtime_error("not a bake group");
    GroupSettings s = groupSettings(*grp);
    const std::string was = s.cage;
    if (was.empty())
        return was;
    UndoChunk chunk("Reset Bake Cage");
    s.cage.clear();
    setGroupSettings(g, group, s);
    if (deleteNode)
        if (scene::Node* c = meshByName(g, was))
            g.deleteNode(c->id());
    return was;
}
} // namespace lila::bake
