#include "ui/TopologyTool.h"

#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "retopo/Patch.h"
#include "retopo/Retopo.h"
#include "ui/ViewportDisplay.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lila::ui
{
namespace
{
using geo::Index;
using geo::kInvalid;
using retopo::ConstructKind;

constexpr const char* kPrefs = "general";
constexpr const char* kStripRowsPref = "/topology/stripRows";
constexpr const char* kCylinderPointsPref = "/topology/cylinderPoints";
constexpr const char* kHandleFalloffPref = "/topology/handleFalloff";

const glm::vec4 kHot(1.0f, 1.0f, 1.0f, 1.0f);
const glm::vec4 kKnotRim(0.08f, 0.08f, 0.08f, 1.0f);
const glm::vec4 kKnotFill(0.9f, 0.9f, 0.9f, 1.0f);
const glm::vec4 kStroke(0.85f, 0.85f, 0.85f, 1.0f);

double distPx(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

double segmentDistPx(QPointF p, QPointF a, QPointF b)
{
    QPointF ab = b - a;
    double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double u = len2 > 0.0 ? std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0.0, 1.0) : 0.0;
    return distPx(p, a + ab * u);
}

json vec(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

json vecs(const std::vector<glm::dvec3>& ps)
{
    json out = json::array();
    for (const glm::dvec3& p : ps)
        out.push_back(vec(p));
    return out;
}

std::vector<glm::dvec3> toVecs(const json& j)
{
    std::vector<glm::dvec3> out;
    if (j.is_array())
        for (const json& v : j)
            if (v.is_array() && v.size() == 3)
                out.emplace_back(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    return out;
}

bool monotone() { return PrefStore::get().value<bool>(kPrefs, viewportDisplay::kMonotone, false); }

double distanceToPolyline(const glm::dvec3& p, const std::vector<glm::dvec3>& line)
{
    double best = std::numeric_limits<double>::max();
    for (size_t i = 0; i + 1 < line.size(); ++i)
    {
        glm::dvec3 ab = line[i + 1] - line[i];
        double t = std::clamp(glm::dot(p - line[i], ab) / std::max(glm::dot(ab, ab), 1e-300), 0.0, 1.0);
        best = std::min(best, glm::length(line[i] + ab * t - p));
    }
    if (line.size() == 1)
        best = glm::length(line[0] - p);
    return best;
}

/** Splits a closed loop at the corner indices into sides, each from one corner to the next inclusive. */
json loopSides(const std::vector<glm::dvec3>& loop, const std::vector<int>& corners, bool fixed)
{
    json sides = json::array();
    for (size_t i = 0; i < corners.size(); ++i)
    {
        std::vector<glm::dvec3> span = retopo::polylineSpan(loop, corners[i], corners[(i + 1) % corners.size()], true);
        sides.push_back({{"points", vecs(span)}, {"fixed", fixed}, {"count", fixed ? int(span.size()) - 1 : 0}});
    }
    return sides;
}
} // namespace

// Strokes

void TopologyTool::finishStroke(const ViewInfo& view, bool erase)
{
    LILA_ZONE();

    double travelled = 0.0;
    for (size_t i = 1; i < mStrokePx.size(); ++i)
        travelled += distPx(mStrokePx[i - 1], mStrokePx[i]);
    const bool click = mStroke.size() < 2 || travelled < 4.0;
    if (erase && mMode != TopoMode::Pencil)
        return;
    if (erase)
    {
        // Erase: drop every guide the stroke crosses.
        json guides = json::array();
        bool removed = false;
        for (const retopo::Guide& g : mGuides)
        {
            bool hit = false;
            for (const glm::dvec3& w : g.points)
                if (auto sp = view.project(w); sp && visible(w, view))
                    for (size_t i = 0; i + 1 < mStrokePx.size() && !hit; ++i)
                        hit = segmentDistPx(*sp, mStrokePx[i], mStrokePx[i + 1]) < 8.0;
            removed |= hit;
            if (!hit)
                guides.push_back({{"points", vecs(g.points)}, {"closed", g.closed}, {"color", json::array({g.color.r, g.color.g, g.color.b})}});
        }
        if (removed)
            run("retopoSetGuides", targetCall({{"guides", guides}}));
        return;
    }
    if (click)
    {
        if (mMode == TopoMode::Patch)
        {
            if (Index e = pickBorderEdge(mPressPx, view); e != kInvalid)
                fillLoopAt(e, view);
        }
        else if (mMode == TopoMode::Handles && !mStroke.empty())
            beginHandles({mStroke.front()}, view);
        return;
    }
    const double upp = view.unitsPerPixel(mStroke.front());
    retopo::StrokeOptions o;
    o.spacing = mMode == TopoMode::Pencil ? 4.0 * upp : std::max(3.0 * upp, edgeLengthNear(mStroke.front(), view) / 4.0);
    o.closeDistance = mMode == TopoMode::Stroke || mMode == TopoMode::Handles ? 0.0 : 15.0 * upp;
    o.cornerAngle = mMode == TopoMode::Pencil || mMode == TopoMode::Handles ? -1.0 : 55.0;
    retopo::SurfaceStroke s = retopo::processStroke(mStroke, &mRefBvh, o);
    if (s.empty())
        return;
    Index a = pickBorderPoint(mStrokePx.front(), view), b = pickBorderPoint(mStrokePx.back(), view);
    switch (mMode)
    {
    case TopoMode::Pencil: commitPencil(s); break;
    case TopoMode::Patch: commitPatch(s, a, b, view); break;
    case TopoMode::Strip: commitStrip(s, view); break;
    case TopoMode::Stroke: commitStroke(s, a, b, view); break;
    case TopoMode::Handles: beginHandles(s.points, view); break;
    default: break;
    }
}

void TopologyTool::commitPencil(const retopo::SurfaceStroke& s)
{
    json guides = json::array();
    for (const retopo::Guide& g : mGuides)
        guides.push_back({{"points", vecs(g.points)}, {"closed", g.closed}, {"color", json::array({g.color.r, g.color.g, g.color.b})}});
    glm::vec3 c = retopo::guidePaletteColor(int(mGuides.size()));
    guides.push_back({{"points", vecs(s.points)}, {"closed", s.closed}, {"color", json::array({c.r, c.g, c.b})}});
    run("retopoSetGuides", targetCall({{"guides", guides}}));
    refresh();
}

void TopologyTool::commitPatch(const retopo::SurfaceStroke& s, Index a, Index b, const ViewInfo& view)
{
    const double edge = edgeLengthNear(s.points.front(), view);
    if (a != kInvalid && b != kInvalid && a != b)
    {
        BorderLoop loop = borderThrough(a);
        if (std::find(loop.points.begin(), loop.points.end(), b) != loop.points.end())
        {
            std::vector<glm::dvec3> arc = borderArc(loop, b, a);
            std::vector<glm::dvec3> path = s.points;
            path.front() = pointWorld(a);
            path.back() = pointWorld(b);
            std::vector<int> cuts;
            for (int c : s.corners)
                if (c > 0 && c + 1 < int(path.size()))
                    cuts.push_back(c);
            if (cuts.empty())
                cuts = {int(path.size() - 1) / 3, int(2 * (path.size() - 1)) / 3};
            if (cuts.size() > 4)
                cuts.resize(4);
            cuts.erase(std::remove_if(cuts.begin(), cuts.end(), [&](int c) { return c <= 0 || c >= int(path.size()) - 1; }), cuts.end());
            std::vector<int> breaks = {0};
            breaks.insert(breaks.end(), cuts.begin(), cuts.end());
            breaks.push_back(int(path.size()) - 1);
            breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
            json sides = json::array();
            std::vector<double> lengths;
            for (size_t i = 0; i + 1 < breaks.size(); ++i)
            {
                std::vector<glm::dvec3> span = retopo::polylineSpan(path, breaks[i], breaks[i + 1], false);
                lengths.push_back(retopo::polylineLength(span));
                sides.push_back({{"points", vecs(span)}, {"fixed", false}, {"count", 0}});
            }
            sides.push_back({{"points", vecs(arc)}, {"fixed", true}, {"count", int(arc.size()) - 1}});
            // A U against the border becomes a clean grid: the far side matches the border, the legs match each other.
            if (sides.size() == 4)
            {
                sides[1]["count"] = int(arc.size()) - 1;
                int legs = std::max(1, int(std::lround((lengths[0] + lengths[2]) * 0.5 / edge)));
                sides[0]["count"] = legs;
                sides[2]["count"] = legs;
            }
            createConstruct(ConstructKind::Patch, {{"sides", sides}}, edge);
            return;
        }
    }
    if (!s.closed)
    {
        log::warn("Patch: close the shape, or start and end it on the retopo border");
        return;
    }
    size_t found = s.corners.size();
    int wanted = found >= 3 && found <= 6 ? int(found) : found > 6 ? 6 : 4;
    std::vector<int> corners = retopo::patchCorners(s.points, wanted);
    if (corners.size() < 2)
        return;
    createConstruct(ConstructKind::Patch, {{"sides", loopSides(s.points, corners, false)}}, edge);
}

void TopologyTool::fillLoopAt(Index edge, const ViewInfo&)
{
    if (!mRetopo)
        return;
    retopo::BorderRun run = retopo::walkBorder(*mRetopo, edge, 180.0);
    if (!run.closed)
    {
        log::warn("Patch: that border doesn't close into a loop");
        return;
    }
    std::vector<glm::dvec3> loop;
    for (Index p : run.points)
        loop.push_back(pointWorld(p));
    if (loop.size() > 1 && loop.front() == loop.back())
        loop.pop_back();
    if (loop.size() % 2 != 0)
    {
        log::warn("Patch: the hole has an odd number of edges ({}), so it can't be filled with quads alone", loop.size());
        return;
    }
    std::vector<int> corners = retopo::patchCorners(loop, 4);
    createConstruct(ConstructKind::Patch, {{"sides", loopSides(loop, corners, true)}}, 0.0);
}

void TopologyTool::commitStrip(const retopo::SurfaceStroke& s, const ViewInfo& view)
{
    const double width = 2.0 * brushWorld(s.points.front(), view);
    const int n = int(s.points.size());
    std::vector<int> breaks;
    if (!s.closed)
        breaks.push_back(0);
    for (int c : s.corners)
        if (s.closed || (c > 0 && c < n - 1))
            breaks.push_back(c);
    if (!s.closed)
        breaks.push_back(n - 1);
    if (s.closed && breaks.empty())
        breaks.push_back(0);
    std::vector<glm::dvec3> knots;
    std::vector<int> corners;
    const size_t spans = s.closed ? breaks.size() : breaks.size() - 1;
    for (size_t i = 0; i < spans; ++i)
    {
        std::vector<glm::dvec3> span = retopo::polylineSpan(s.points, breaks[i], breaks[(i + 1) % breaks.size()], s.closed);
        int segs = std::max(1, int(std::lround(retopo::polylineLength(span) / (2.0 * width))));
        std::vector<glm::dvec3> pts = retopo::resamplePolyline(span, segs, false, &mRefBvh);
        bool isCorner = std::find(s.corners.begin(), s.corners.end(), breaks[i]) != s.corners.end();
        for (size_t k = 0; k + 1 < pts.size(); ++k)
        {
            knots.push_back(pts[k]);
            corners.push_back(k == 0 && isCorner ? 1 : 0);
        }
    }
    if (!s.closed)
    {
        knots.push_back(s.points.back());
        corners.push_back(0);
    }
    if (knots.size() < 2)
        return;
    json spec = {{"knots", vecs(knots)}, {"corners", corners}, {"width", width}, {"closed", s.closed},
                 {"rows", std::max(1, PrefStore::get().value<int>(kPrefs, kStripRowsPref, 1))}};
    auto edgeRow = [&](QPointF px, size_t knot) {
        Index e = pickBorderEdge(px, view);
        if (e == kInvalid)
            return json();
        glm::dvec3 p0 = pointWorld(mRetopo->edgePoint(e, 0)), p1 = pointWorld(mRetopo->edgePoint(e, 1));
        knots[knot] = (p0 + p1) * 0.5;
        return json::array({vec(p0), vec(p1)});
    };
    if (!s.closed)
    {
        if (json row = edgeRow(mStrokePx.front(), 0); !row.is_null())
            spec["startRow"] = row;
        if (json row = edgeRow(mStrokePx.back(), knots.size() - 1); !row.is_null())
            spec["endRow"] = row;
        spec["knots"] = vecs(knots);
    }
    createConstruct(ConstructKind::Strip, spec, width);
}

void TopologyTool::commitStroke(const retopo::SurfaceStroke& s, Index a, Index b, const ViewInfo& view)
{
    const double edge = edgeLengthNear(s.points.front(), view);
    if (a != kInvalid && b != kInvalid && a != b)
    {
        BorderLoop loop = borderThrough(a);
        if (std::find(loop.points.begin(), loop.points.end(), b) != loop.points.end())
        {
            commitPatch(s, a, b, view);
            return;
        }
    }
    std::vector<glm::dvec3> path = s.points;
    if (a == kInvalid && b != kInvalid)
    {
        std::reverse(path.begin(), path.end());
        std::swap(a, b);
    }
    if (a != kInvalid)
    {
        BorderLoop loop = borderThrough(a);
        auto it = std::find(loop.points.begin(), loop.points.end(), a);
        if (it == loop.points.end() || loop.world.size() < 2)
            return;
        int anchor = int(it - loop.points.begin());
        const int n = int(loop.world.size());
        path.front() = loop.world[anchor];
        int sign = 1;
        if (!loop.closed && anchor == n - 1)
            sign = -1;
        else if (loop.closed || anchor > 0)
        {
            // The border edge the stroke leaves most squarely is the one carried; on a straight border, the stroke's right.
            glm::dvec3 d = glm::normalize(path[std::min<size_t>(3, path.size() - 1)] - path.front());
            glm::dvec3 forward = glm::normalize(loop.world[(anchor + 1) % n] - loop.world[anchor]);
            glm::dvec3 backward = glm::normalize(loop.world[(anchor + n - 1) % n] - loop.world[anchor]);
            double squareF = std::abs(glm::dot(forward, d)), squareB = std::abs(glm::dot(backward, d));
            if (std::abs(squareF - squareB) > 0.3)
                sign = squareF < squareB ? 1 : -1;
            else
                sign = glm::dot(glm::cross(normalAt(path.front()), d), forward) > 0.0 ? 1 : -1;
        }
        json spec = {{"border", vecs(loop.world)}, {"borderClosed", loop.closed}, {"anchor", anchor}, {"span", 2 * sign}, {"path", vecs(path)}};
        createConstruct(ConstructKind::Sweep, spec, edge);
        return;
    }

    // Drawn along a border: bridge the stroke to the stretch of border beside it.
    if (mRetopo)
    {
        auto screen = projectPoints(view);
        int near = 0;
        for (QPointF px : mStrokePx)
        {
            double best = std::numeric_limits<double>::max();
            for (Index e = 0; e < mRetopo->edgeCapacity(); ++e)
                if (mRetopo->edgeValid(e) && mRetopo->isBoundaryEdge(e))
                {
                    auto pa = screen[mRetopo->edgePoint(e, 0)], pb = screen[mRetopo->edgePoint(e, 1)];
                    if (pa && pb)
                        best = std::min(best, segmentDistPx(px, *pa, *pb));
                }
            near += best < brushRadius();
        }
        if (!mStrokePx.empty() && near >= int(mStrokePx.size() * 8 / 10))
        {
            auto nearestBorder = [&](const glm::dvec3& w) {
                Index best = kInvalid;
                double d = std::numeric_limits<double>::max();
                for (Index p = 0; p < mRetopo->pointCapacity(); ++p)
                    if (mRetopo->pointValid(p) && mRetopo->isBoundaryPoint(p))
                        if (double l = glm::length(pointWorld(p) - w); l < d)
                        {
                            d = l;
                            best = p;
                        }
                return best;
            };
            Index pa = nearestBorder(path.front()), pb = nearestBorder(path.back());
            BorderLoop loop = borderThrough(pa);
            std::vector<glm::dvec3> arc = borderArc(loop, pa, pb);
            if (arc.size() >= 2)
            {
                createConstruct(ConstructKind::Bridge, {{"a", vecs(arc)}, {"aFixed", true}, {"b", vecs(path)}}, edge);
                return;
            }
        }
    }

    // A second free stroke bridges to the last one still waiting.
    const retopo::ConstructRecord* pending = nullptr;
    for (const retopo::ConstructRecord& r : mRecords)
        if (r.kind == ConstructKind::Stroke && r.live)
            pending = &r;
    if (pending)
    {
        json other = pending->spec;
        int pendingId = pending->id;
        UndoChunk chunk("Bridge Strokes");
        run("retopoConstructDelete", {{"mesh", mRetopoName}, {"ids", {pendingId}}});
        refresh();
        createConstruct(ConstructKind::Bridge, {{"a", other["path"]}, {"b", vecs(path)}, {"divisions", other.value("divisions", 0)}}, edge);
        return;
    }
    int divisions = std::max(1, int(std::lround(s.length() / std::max(edge, 1e-9))));
    createConstruct(ConstructKind::Stroke, {{"path", vecs(path)}, {"divisions", divisions}}, edge);
}

// Cylinder

void TopologyTool::commitCylinderLine(QPointF from, QPointF to, const ViewInfo& view)
{
    if (distPx(from, to) < 5.0)
        return;
    QPointF mid = (from + to) * 0.5;
    glm::dvec3 hit;
    bool found = surfaceHit(mid, view, hit);
    for (int k = 1; !found && k < 16; ++k)
        for (int sign : {-1, 1})
            if (!found)
                found = surfaceHit(mid + (to - from) * (sign * k / 34.0), view, hit);
    if (!found)
    {
        log::warn("Cylinder: draw the line across the reference");
        return;
    }
    double depth = glm::length(hit - view.eye);
    glm::dvec3 o0, d0, o1, d1;
    view.ray(from, o0, d0);
    view.ray(to, o1, d1);
    glm::dvec3 across = (o1 + d1 * depth) - (o0 + d0 * depth);
    glm::dvec3 normal = glm::cross(across, view.viewDir(hit));
    if (glm::length(normal) < 1e-12)
        return;
    normal = glm::normalize(normal);
    json ring = {{"origin", vec(hit)}, {"normal", vec(normal)}, {"pick", vec(hit)}};

    auto insertInto = [&](int id) {
        return editConstruct(id, [&](json& spec) {
            json& rings = spec["rings"];
            std::vector<glm::dvec3> origins;
            for (const json& r : rings)
                origins.push_back(toVecs(json::array({r["origin"]}))[0]);
            if (origins.empty())
            {
                rings.push_back(ring);
                return;
            }
            size_t at = rings.size();
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i + 1 < origins.size(); ++i)
            {
                glm::dvec3 ab = origins[i + 1] - origins[i];
                double t = glm::dot(hit - origins[i], ab) / std::max(glm::dot(ab, ab), 1e-300);
                if (t > 0.0 && t < 1.0)
                    if (double d = glm::length(origins[i] + ab * t - hit); d < best)
                    {
                        best = d;
                        at = i + 1;
                    }
            }
            if (at == rings.size() && glm::length(hit - origins.front()) < glm::length(hit - origins.back()))
                at = 0;
            rings.insert(rings.begin() + std::ptrdiff_t(at), ring);
        });
    };
    Hit h = pick(mid, view);
    if (const retopo::ConstructRecord* rec = record(h.construct); rec && rec->live && rec->kind == ConstructKind::Cylinder)
    {
        if (insertInto(h.construct))
            mCylinder = h.construct;
        return;
    }
    if (const retopo::ConstructRecord* rec = record(mCylinder); rec && rec->live)
    {
        insertInto(mCylinder);
        return;
    }
    int points = std::max(3, PrefStore::get().value<int>(kPrefs, kCylinderPointsPref, 8));
    mCylinder = createConstruct(ConstructKind::Cylinder, {{"rings", json::array({ring})}, {"points", points}}, 0.0);
}

// Auto region

void TopologyTool::paintRegion(QPointF px, const ViewInfo& view, bool on)
{
    glm::dvec3 hit;
    if (!surfaceHit(px, view, hit))
        return;
    if (mRegion.size() != mRefWorldMesh.faceCapacity())
        mRegion.assign(mRefWorldMesh.faceCapacity(), 0);
    double r = brushWorld(hit, view);
    geo::Bounds box;
    box.min = hit - glm::dvec3(r);
    box.max = hit + glm::dvec3(r);
    const geo::Triangulation& tris = mRefBvh.triangles();
    for (Index t : mRefBvh.overlapping(box))
    {
        Index f = tris.faces[t];
        if (f < mRegion.size() && glm::length(mRefWorldMesh.faceCentroid(f) - hit) <= r)
            mRegion[f] = on;
    }
}

void TopologyTool::floodRegionFrom(QPointF px, const ViewInfo& view)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    geo::BvhHit hit;
    if (mRefBvh.empty() || !mRefBvh.raycast(o, d, hit) || !mRetopo)
        return;
    const geo::Mesh& ref = mRefWorldMesh;
    std::vector<uint8_t> barrier(ref.faceCapacity(), 0);
    const double step = view.unitsPerPixel(hit.point) * 2.0;
    for (Index e = 0; e < mRetopo->edgeCapacity(); ++e)
    {
        if (!mRetopo->edgeValid(e) || !mRetopo->isBoundaryEdge(e))
            continue;
        glm::dvec3 a = pointWorld(mRetopo->edgePoint(e, 0)), b = pointWorld(mRetopo->edgePoint(e, 1));
        int samples = std::max(2, int(glm::length(b - a) / std::max(step, 1e-9)));
        for (int k = 0; k <= samples; ++k)
            if (geo::BvhClosest c; mRefBvh.closestPoint(glm::mix(a, b, double(k) / samples), c) && c.face < barrier.size())
                barrier[c.face] = 1;
    }
    std::vector<uint8_t> in(ref.faceCapacity(), 0);
    std::vector<Index> stack{hit.face};
    size_t count = 0;
    while (!stack.empty())
    {
        Index f = stack.back();
        stack.pop_back();
        if (f >= in.size() || in[f] || !ref.faceValid(f))
            continue;
        in[f] = 1;
        ++count;
        if (barrier[f])
            continue;
        ref.forEachFaceCorner(f, [&](Index c) {
            Index g = ref.oppositeFace(ref.cornerEdge(c), f);
            if (g != kInvalid && !in[g])
                stack.push_back(g);
        });
    }
    if (count > ref.numFaces() * 6 / 10)
    {
        log::warn("Auto Region: the hole isn't closed off by retopo faces, so the fill ran over most of the reference");
        return;
    }
    if (mRegion.size() != in.size())
        mRegion.assign(in.size(), 0);
    for (size_t f = 0; f < in.size(); ++f)
        mRegion[f] |= in[f];
}

size_t TopologyTool::regionFaceCount() const { return size_t(std::count(mRegion.begin(), mRegion.end(), uint8_t(1))); }

void TopologyTool::clearRegion()
{
    std::fill(mRegion.begin(), mRegion.end(), uint8_t(0));
    viewportTools::requestRedraw();
}

bool TopologyTool::runRegion()
{
    refresh();
    std::vector<Index> faces;
    for (Index f = 0; f < mRegion.size(); ++f)
        if (mRegion[f])
            faces.push_back(f);
    if (faces.empty())
    {
        log::warn("Auto Region: paint the area to retopologise first");
        return false;
    }
    double edge = edgeLength();
    if (edge <= 0.0 && mRetopo && mRetopo->numEdges() > 0)
    {
        double sum = 0.0;
        int n = 0;
        for (Index e = 0; e < mRetopo->edgeCapacity(); ++e)
            if (mRetopo->edgeValid(e))
            {
                sum += glm::length(pointWorld(mRetopo->edgePoint(e, 0)) - pointWorld(mRetopo->edgePoint(e, 1)));
                ++n;
            }
        edge = n ? sum / n : 0.0;
    }
    if (edge <= 0.0)
    {
        geo::Bounds b = mRefWorldMesh.bounds();
        edge = glm::length(b.max - b.min) / 40.0;
    }
    json guides = json::array();
    for (const retopo::Guide& g : mGuides)
        guides.push_back(vecs(g.points));
    // A retopo border loop lying along the region's edge is what the region stitches to.
    json border = json::array();
    if (mRetopo)
    {
        std::vector<uint8_t> seen(mRetopo->pointCapacity(), 0);
        for (Index p = 0; p < mRetopo->pointCapacity() && border.empty(); ++p)
        {
            if (!mRetopo->pointValid(p) || !mRetopo->isBoundaryPoint(p) || seen[p])
                continue;
            BorderLoop loop = borderThrough(p);
            int near = 0;
            for (size_t i = 0; i < loop.points.size(); ++i)
            {
                seen[loop.points[i]] = 1;
                geo::BvhClosest c;
                if (mRefBvh.closestPoint(loop.world[i], c) && c.face < mRegion.size())
                {
                    bool marked = mRegion[c.face];
                    if (!marked)
                        mRefWorldMesh.forEachFaceCorner(c.face, [&](Index corner) {
                            Index g = mRefWorldMesh.oppositeFace(mRefWorldMesh.cornerEdge(corner), c.face);
                            marked |= g != kInvalid && g < mRegion.size() && mRegion[g];
                        });
                    near += marked;
                }
            }
            if (loop.closed && loop.points.size() >= 3 && near >= int(loop.points.size() * 8 / 10))
                border = vecs(loop.world);
        }
    }
    json spec = {{"faces", faces}, {"referenceFaces", mRefWorldMesh.numFaces()}, {"edgeLength", edge}, {"guides", guides}, {"border", border}};
    if (!createConstruct(ConstructKind::Region, spec, edge))
        return false;
    clearRegion();
    return true;
}

// Handles

void TopologyTool::beginHandles(const std::vector<glm::dvec3>& path, const ViewInfo& view)
{
    if (!mRetopo || path.empty())
        return;
    geo::Mesh world = *mRetopo;
    for (Index p = 0; p < world.pointCapacity(); ++p)
        if (world.pointValid(p))
            world.setPosition(p, pointWorld(p));
    double radius = brushWorld(path.front(), view);
    std::vector<Index> region = retopo::regionAlong(world, path, radius);
    if (region.empty())
    {
        log::warn("Handles: drag across retopo faces to choose what they move");
        return;
    }
    std::vector<glm::dvec3> handles = path;
    if (path.size() > 1)
    {
        int count = std::clamp(int(std::lround(retopo::polylineLength(path) / (2.0 * radius))) + 1, 2, 8);
        handles = retopo::resamplePolyline(path, count - 1, false, &mRefBvh);
    }
    double falloff = std::clamp(PrefStore::get().value<double>(kPrefs, kHandleFalloffPref, 0.5), 0.0, 1.0);
    mRig = retopo::HandleRig(world, region, handles, falloff);
    mXforms.clear();
    for (const glm::dvec3& h : handles)
        mXforms.push_back({.pivot = h, .axis = normalAt(h)});
}

void TopologyTool::previewHandles()
{
    mMoved.clear();
    std::vector<glm::dvec3> pos = mRig.deform(mXforms);
    const std::vector<Index>& pts = mRig.points();
    for (size_t i = 0; i < pts.size(); ++i)
        mMoved[pts[i]] = onSurface(pos[i]);
}

void TopologyTool::commitHandles()
{
    if (!mMoved.empty())
    {
        json points = json::array(), positions = json::array();
        for (auto& [p, pos] : mMoved)
        {
            points.push_back(p);
            positions.push_back(vec(pos));
        }
        run("retopoDrawMove", {{"mesh", mRetopoName}, {"points", points}, {"positions", positions}});
    }
    mMoved.clear();
    refresh();
    if (!mRetopo || mXforms.empty())
        return;
    // The rig starts again from where things now are, so the next drag builds on this one.
    std::vector<glm::dvec3> handles;
    for (const retopo::HandleXform& x : mXforms)
        handles.push_back(x.pivot + x.translate);
    geo::Mesh world = *mRetopo;
    for (Index p = 0; p < world.pointCapacity(); ++p)
        if (world.pointValid(p))
            world.setPosition(p, pointWorld(p));
    std::vector<Index> region = mRig.points();
    double falloff = std::clamp(PrefStore::get().value<double>(kPrefs, kHandleFalloffPref, 0.5), 0.0, 1.0);
    mRig = retopo::HandleRig(world, region, handles, falloff);
    std::vector<retopo::HandleXform> next;
    for (size_t i = 0; i < handles.size(); ++i)
        next.push_back({.pivot = handles[i], .axis = normalAt(handles[i]), .scale = 1.0});
    mXforms = std::move(next);
}

void TopologyTool::clearHandles()
{
    mRig = retopo::HandleRig();
    mXforms.clear();
    mMoved.clear();
    mDragHandle = -1;
    viewportTools::requestRedraw();
}

// Picking constructs' own controls

int TopologyTool::pickKnot(QPointF px, const ViewInfo& view, int* construct) const
{
    if (mMode != TopoMode::Strip)
        return -1;
    double best = 9.0;
    int found = -1;
    for (const retopo::ConstructRecord& r : mRecords)
    {
        if (r.kind != ConstructKind::Strip || !r.live)
            continue;
        std::vector<glm::dvec3> knots = toVecs(r.spec.value("knots", json::array()));
        for (size_t i = 0; i < knots.size(); ++i)
            if (auto sp = view.project(knots[i]); sp && visible(knots[i], view) && distPx(*sp, px) < best)
            {
                best = distPx(*sp, px);
                found = int(i);
                if (construct)
                    *construct = r.id;
            }
    }
    return found;
}

int TopologyTool::pickHandle(QPointF px, const ViewInfo& view) const
{
    double best = 11.0;
    int found = -1;
    for (size_t i = 0; i < mXforms.size(); ++i)
    {
        glm::dvec3 w = mXforms[i].pivot + mXforms[i].translate;
        if (auto sp = view.project(w); sp && distPx(*sp, px) < best)
        {
            best = distPx(*sp, px);
            found = int(i);
        }
    }
    return found;
}

int TopologyTool::pickGuide(QPointF px, const ViewInfo& view) const
{
    double best = 8.0;
    int found = -1;
    for (size_t gi = 0; gi < mGuides.size(); ++gi)
    {
        const std::vector<glm::dvec3>& pts = mGuides[gi].points;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            auto a = view.project(pts[i]), b = view.project(pts[i + 1]);
            if (a && b && visible(pts[i], view))
                if (double d = segmentDistPx(px, *a, *b); d < best)
                {
                    best = d;
                    found = int(gi);
                }
        }
    }
    return found;
}

int TopologyTool::pickPendingStroke(QPointF px, const ViewInfo& view) const
{
    double best = 8.0;
    int found = 0;
    for (const retopo::ConstructRecord& r : mRecords)
    {
        if (r.kind != ConstructKind::Stroke || !r.live)
            continue;
        std::vector<glm::dvec3> pts = toVecs(r.spec.value("path", json::array()));
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            auto a = view.project(pts[i]), b = view.project(pts[i + 1]);
            if (a && b)
                if (double d = segmentDistPx(px, *a, *b); d < best)
                {
                    best = d;
                    found = r.id;
                }
        }
    }
    return found;
}

void TopologyTool::previewConstruct(int id, const json& spec)
{
    const retopo::ConstructRecord* rec = record(id);
    if (!rec)
        return;
    retopo::GenResult gen;
    try
    {
        retopo::GenContext ctx;
        ctx.surfaceMesh = &mRefWorldMesh;
        ctx.surface = &mRefBvh;
        ctx.edgeLength = std::max(1e-6, edgeLength());
        gen = retopo::generate(retopo::parseConstructSpec(rec->kind, spec), ctx);
    }
    catch (const std::exception&)
    {
        return;
    }
    if (!gen.ok())
        return;
    mPreviewId = id;
    mPreviewPos = std::move(gen.positions);
    mPreviewFaces = std::move(gen.faces);
    mPreviewSpec = spec;
}

bool TopologyTool::wheelConstruct(int id, int delta, bool second, QPointF px, const ViewInfo& view)
{
    const retopo::ConstructRecord* rec = record(id);
    if (!rec)
        return false;
    auto count = [&](size_t i, int fallback) { return i < rec->counts.size() ? rec->counts[i] : fallback; };
    glm::dvec3 hit(0.0);
    surfaceHit(px, view, hit);
    std::vector<int> counts = rec->counts;
    return editConstruct(id, [&](json& spec) {
        switch (rec->kind)
        {
        case ConstructKind::Patch:
        {
            if (second)
            {
                spec["variant"] = spec.value("variant", 0) + delta;
                break;
            }
            json& sides = spec["sides"];
            int nearest = -1;
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i < sides.size(); ++i)
                if (!sides[i].value("fixed", false))
                    if (double d = distanceToPolyline(hit, toVecs(sides[i]["points"])); d < best)
                    {
                        best = d;
                        nearest = int(i);
                    }
            if (nearest < 0)
                break;
            auto bump = [&](int i, int by) { sides[i]["count"] = std::max(1, sides[i].value("count", 1) + by); };
            int opposite = sides.size() == 4 ? (nearest + 2) % 4 : -1;
            if (opposite >= 0 && !sides[opposite].value("fixed", false))
            {
                bump(nearest, delta);
                bump(opposite, delta);
            }
            else
                bump(nearest, 2 * delta);
            break;
        }
        case ConstructKind::Strip:
            if (second)
                spec["rows"] = std::max(1, spec.value("rows", 1) + delta);
            else
                spec["divisions"] = std::max(1, (spec.value("divisions", 0) > 0 ? spec.value("divisions", 0) : count(0, 4)) + delta);
            break;
        case ConstructKind::Stroke: spec["divisions"] = std::max(1, spec.value("divisions", 8) + delta); break;
        case ConstructKind::Bridge:
            if (second)
                spec["rows"] = std::max(1, (spec.value("rows", 0) > 0 ? spec.value("rows", 0) : count(1, 2)) + delta);
            else
                spec["divisions"] = std::max(1, (spec.value("divisions", 0) > 0 ? spec.value("divisions", 0) : count(0, 4)) + delta);
            break;
        case ConstructKind::Sweep:
            if (second)
                spec["rows"] = std::max(1, (spec.value("rows", 0) > 0 ? spec.value("rows", 0) : count(0, 2)) + delta);
            else
            {
                int span = spec.value("span", 1) + delta;
                spec["span"] = span == 0 ? delta : span;
            }
            break;
        case ConstructKind::Cylinder:
            if (second)
                spec["rowDensity"] = std::clamp(spec.value("rowDensity", 1.0) + 0.1 * delta, 0.1, 10.0);
            else
            {
                bool capped = spec.value("capStart", false) || spec.value("capEnd", false);
                spec["points"] = std::max(capped ? 4 : 3, spec.value("points", 8) + delta * (capped ? 2 : 1));
            }
            break;
        case ConstructKind::Region: spec["edgeLength"] = spec.value("edgeLength", 1.0) * std::pow(0.9, delta); break;
        }
    });
}

// Drawing constructs' own controls

void TopologyTool::drawConstructs(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, bool here) const
{
    const bool mono = monotone();
    for (const retopo::ConstructRecord& r : mRecords)
    {
        if (!r.live)
            continue;
        if (r.kind == ConstructKind::Strip && mMode == TopoMode::Strip)
        {
            std::vector<glm::dvec3> knots = toVecs(r.spec.value("knots", json::array()));
            json corners = r.spec.value("corners", json::array());
            if (mPreviewId == r.id && !mPreviewSpec.is_null())
                knots = toVecs(mPreviewSpec.value("knots", json::array()));
            drawSurfacePolyline(p, view, knots, r.spec.value("closed", false), 1.0f, glm::vec4(0.7f, 0.7f, 0.7f, 0.8f), 4.0f);
            for (size_t i = 0; i < knots.size(); ++i)
                if (auto sp = view.project(knots[i]); sp && visible(knots[i], view))
                {
                    bool corner = i < corners.size() && corners[i].is_number() && corners[i].get<int>() != 0;
                    bool hot = here && distPx(*sp, mCursor) < 9.0;
                    float size = hot ? 7.0f : 5.5f;
                    if (corner)
                    {
                        p.rect(QRectF(sp->x() - size, sp->y() - size, size * 2, size * 2), kKnotRim);
                        p.rect(QRectF(sp->x() - size + 1.5, sp->y() - size + 1.5, size * 2 - 3, size * 2 - 3), hot ? kHot : kKnotFill);
                    }
                    else
                    {
                        p.disc(*sp, size, kKnotRim, 16);
                        p.disc(*sp, size - 1.5f, hot ? kHot : kKnotFill, 16);
                    }
                }
        }
        else if (r.kind == ConstructKind::Stroke)
        {
            std::vector<glm::dvec3> path = toVecs(r.spec.value("path", json::array()));
            drawSurfacePolyline(p, view, path, false, 2.0f, kStroke, 6.0f);
            int divisions = std::max(1, r.spec.value("divisions", 8));
            for (const glm::dvec3& w : retopo::resamplePolyline(path, divisions, false, nullptr))
                if (auto sp = view.project(w); sp && visible(w, view))
                    p.disc(*sp, 3.0f, kStroke, 10);
        }
        else if (r.kind == ConstructKind::Cylinder && (r.id == mCylinder || r.id == mActive))
        {
            json rings = r.spec.value("rings", json::array());
            for (const json& ring : rings)
            {
                glm::dvec3 o = toVecs(json::array({ring["origin"]}))[0];
                if (auto sp = view.project(o); sp && visible(o, view))
                {
                    p.disc(*sp, 5.0f, kKnotRim, 16);
                    p.disc(*sp, 3.5f, kKnotFill, 16);
                }
            }
            if (rings.size() == 1)
            {
                std::string key = r.spec.dump();
                if (key != mRingKey)
                {
                    mRingKey = key;
                    mRing.clear();
                    try
                    {
                        retopo::GenContext ctx;
                        ctx.surfaceMesh = &mRefWorldMesh;
                        ctx.surface = &mRefBvh;
                        retopo::GenResult gen = retopo::generate(retopo::parseConstructSpec(r.kind, r.spec), ctx);
                        if (gen.ok())
                            mRing = gen.positions;
                    }
                    catch (const std::exception&)
                    {
                    }
                }
                drawSurfacePolyline(p, view, mRing, true, 2.0f, kHot);
                for (const glm::dvec3& w : mRing)
                    if (auto sp = view.project(w); sp && visible(w, view))
                        p.disc(*sp, 3.5f, kHot, 10);
            }
        }
        else if (r.kind == ConstructKind::Patch && r.id == mActive && r.spec.contains("pull") && !r.spec["pull"].is_null())
        {
            glm::dvec3 pull = toVecs(json::array({r.spec["pull"]}))[0];
            if (auto sp = view.project(pull); sp && visible(pull, view))
            {
                p.disc(*sp, 6.0f, kKnotRim, 16);
                p.disc(*sp, 2.5f, kHot, 12);
            }
        }
    }

    if (mMode == TopoMode::Region && regionFaceCount() > 0)
    {
        glm::vec4 c = mono ? glm::vec4(0.6f, 0.6f, 0.6f, 0.35f) : glm::vec4(0.9f, 0.45f, 0.05f, 0.35f);
        for (Index f = 0; f < mRegion.size(); ++f)
        {
            if (!mRegion[f] || !mRefWorldMesh.faceValid(f))
                continue;
            std::vector<Index> pts = mRefWorldMesh.facePoints(f);
            for (size_t k = 1; k + 1 < pts.size(); ++k)
                for (Index q : {pts[0], pts[k], pts[k + 1]})
                    s.tris3d.push_back({glm::vec3(mRefWorldMesh.position(q)), c});
        }
    }

    if (mMode == TopoMode::Handles)
        for (size_t i = 0; i < mXforms.size(); ++i)
        {
            glm::dvec3 w = mXforms[i].pivot + mXforms[i].translate;
            if (auto sp = view.project(w))
            {
                bool hot = (here && distPx(*sp, mCursor) < 11.0) || int(i) == mDragHandle;
                float ring = float(9.0 * mXforms[i].scale);
                std::vector<QPointF> circle;
                for (int k = 0; k < 32; ++k)
                {
                    double a = k / 32.0 * 6.283185307179586 + glm::radians(mXforms[i].rotate);
                    circle.push_back(*sp + QPointF(std::cos(a), std::sin(a)) * ring);
                }
                p.polyline(circle, 1.5f, hot ? kHot : kKnotFill, true);
                p.line(*sp, circle.front(), 1.5f, hot ? kHot : kKnotFill);
                p.disc(*sp, 4.0f, kKnotRim, 12);
                p.disc(*sp, 2.5f, hot ? kHot : kKnotFill, 12);
            }
        }
}
} // namespace lila::ui
