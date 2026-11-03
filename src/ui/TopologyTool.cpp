#include "ui/TopologyTool.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Normals.h"
#include "retopo/RetopoCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/NodeLookup.h"
#include "ui/RetopoDrawTool.h"
#include "ui/ToolColors.h"
#include "ui/UiCommands.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportToolMath.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
using geo::Index;
using geo::kInvalid;
using retopo::ConstructKind;

constexpr const char* kBrushPref = "/topology/brush";
constexpr const char* kEdgePref = "/topology/edgeLength";
constexpr const char* kQualityPref = "/topology/qualityOverlay";
constexpr const char* kEvenSizePref = "/topology/relax/evenSize";
constexpr const char* kSquarenessPref = "/topology/relax/squareness";
constexpr const char* kStiffnessPref = "/topology/relax/stiffness";

// Overlay colours are sRGB; the 3D streams take linear.
const glm::vec4 kLiveWireLinear(0.55f, 0.55f, 0.55f, 1.0f);

struct ModeInfo
{
    const char* label;
    const char* action;
};
constexpr ModeInfo kModes[] = {
    {"Pencil", "topo.pencil"}, {"Patch", "topo.patch"},     {"Strip", "topo.strip"},     {"Stroke", "topo.stroke"},
    {"Cylinder", "topo.cylinder"}, {"Auto Region", "topo.region"}, {"Handles", "topo.handles"}, {"Relax", "topo.relax"},
};

/** Esc and Enter reach the tool even when a hotkey would take them; Shift and Ctrl redraw the hints at once. */
class KeyFilter : public QObject
{
public:
    explicit KeyFilter(TopologyTool* tool) : mTool(tool) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control || k->key() == Qt::Key_Alt)
            viewportTools::requestRedraw();
        QWidget* focus = QApplication::focusWidget();
        if (!focus || obj != focus || !focus->inherits("lila::ui::ViewportWidget") || e->type() != QEvent::KeyPress)
            return false;
        if ((k->key() == Qt::Key_Escape || k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && k->modifiers() == Qt::NoModifier)
        {
            ViewInfo none;
            return mTool->keyPress(k, none);
        }
        return false;
    }

private:
    TopologyTool* mTool;
};
} // namespace

const char* topoModeLabel(TopoMode m) { return m < TopoMode::Count ? kModes[size_t(m)].label : ""; }
const char* topoModeAction(TopoMode m) { return m < TopoMode::Count ? kModes[size_t(m)].action : ""; }

TopologyTool::TopologyTool(scene::NodeId reference, std::string retopoName, TopoMode mode)
    : mMode(mode), mReference(reference), mRetopoName(std::move(retopoName))
{
}

TopologyTool::~TopologyTool() = default;

void TopologyTool::activate()
{
    mFilter = std::make_unique<KeyFilter>(this);
    qApp->installEventFilter(mFilter.get());
    mTimer = std::make_unique<QTimer>();
    mTimer->setInterval(16);
    QObject::connect(mTimer.get(), &QTimer::timeout, [this] { tickRelax(); });
    refresh();
    log::info("Topology ({}) on {} into {}", topoModeLabel(mMode),
              mRefMesh ? scene::Graph::current().node(mReference)->path() : std::string("(no reference)"), mRetopoName);
}

void TopologyTool::deactivate()
{
    if (mTimer)
        mTimer->stop();
    if (mFilter)
        qApp->removeEventFilter(mFilter.get());
    mFilter.reset();
}

void TopologyTool::setMode(TopoMode m)
{
    if (m == mMode)
        return;
    mMode = m;
    mDrag = Drag::None;
    mStroke.clear();
    mStrokePx.clear();
    mPreviewId = 0;
    if (m != TopoMode::Handles)
        clearHandles();
    if (m != TopoMode::Cylinder)
        mCylinder = 0;
    viewportTools::requestRedraw();
}

void TopologyTool::setActiveConstruct(int id)
{
    mActive = id;
    viewportTools::requestRedraw();
}

double TopologyTool::brushRadius() { return std::clamp(PrefStore::get().value<double>("general", kBrushPref, 40.0), 4.0, 600.0); }
void TopologyTool::setBrushRadius(double px)
{
    PrefStore::get().set("general", kBrushPref, std::clamp(px, 4.0, 600.0));
    viewportTools::requestRedraw();
}
double TopologyTool::edgeLength() { return std::max(0.0, PrefStore::get().value<double>("general", kEdgePref, 0.0)); }
void TopologyTool::setEdgeLength(double cm) { PrefStore::get().set("general", kEdgePref, std::max(0.0, cm)); }
bool TopologyTool::qualityOverlay() { return PrefStore::get().value<bool>("general", kQualityPref, false); }
void TopologyTool::setQualityOverlay(bool on)
{
    PrefStore::get().set("general", kQualityPref, on);
    viewportTools::requestRedraw();
}

retopo::LiveRelaxOptions TopologyTool::relaxOptions()
{
    PrefStore& p = PrefStore::get();
    retopo::LiveRelaxOptions o;
    o.evenSize = std::clamp(p.value<double>("general", kEvenSizePref, 0.5), 0.0, 1.0);
    o.squareness = std::clamp(p.value<double>("general", kSquarenessPref, 0.7), 0.0, 1.0);
    o.stiffness = std::clamp(p.value<double>("general", kStiffnessPref, 0.5), 0.05, 1.0);
    return o;
}

// Scene cache

void TopologyTool::refresh()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* ref = g.node(mReference);
    scene::MeshPtr refMesh = ref && ref->type().isA("mesh") ? ref->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
    glm::dmat4 refWorld = ref ? ref->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    if (refMesh != mRefMesh || refWorld != mRefWorld)
    {
        mRefMesh = refMesh;
        mRefWorld = refWorld;
        mRefWorldMesh = geo::Mesh();
        if (refMesh)
        {
            mRefWorldMesh = *refMesh;
            mRefWorldMesh.compact();
            for (glm::dvec3& p : mRefWorldMesh.positions())
                p = glm::dvec3(refWorld * glm::dvec4(p, 1.0));
            mRefBvh.build(mRefWorldMesh);
            mRefFaceNormals = geo::faceNormals(mRefWorldMesh);
        }
        else
        {
            mRefBvh = geo::MeshBvh();
            mRefFaceNormals.clear();
        }
        if (mRegion.size() != mRefWorldMesh.faceCapacity())
            mRegion.assign(mRefWorldMesh.faceCapacity(), 0);
    }
    mRetopo = nullptr;
    mSymmetry.reset();
    scene::Node* n = retopoNode();
    if (!n)
    {
        mGuides.clear();
        mRecords.clear();
        mGuidesText.clear();
        mConstructsText.clear();
        return;
    }
    mRetopoId = n->id();
    mRetopoName = n->path();
    mRetopo = scene::modifiers::cage(g, n->id());
    mRetopoWorld = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    if (int64_t axis = n->as<int64_t>(scene::MeshNode::kSymmetry); axis > 0 && axis <= 3)
    {
        glm::dvec3 nl(0.0);
        nl[int(axis - 1)] = 1.0;
        glm::dvec3 o = glm::dvec3(mRetopoWorld * glm::dvec4(nl * scene::symmetryOffset(*n, int(axis - 1)), 1.0));
        glm::dvec3 nw = glm::normalize(glm::transpose(glm::inverse(glm::dmat3(mRetopoWorld))) * nl);
        mSymmetry = geo::SymmetryPlane{nw, glm::dot(nw, o)};
    }
    const std::string& guides = n->as<std::string>(scene::MeshNode::kRetopoGuides);
    if (guides != mGuidesText)
    {
        mGuidesText = guides;
        mGuides = retopo::retopoGuides(*n);
    }
    const std::string& constructs = n->as<std::string>(scene::MeshNode::kRetopoConstructs);
    if (constructs != mConstructsText || mRetopo != mConstructsMesh)
    {
        mConstructsText = constructs;
        mConstructsMesh = mRetopo;
        mRecords = retopo::retopoConstructs(g, *n);
        if (mActive && !record(mActive))
            mActive = 0;
        if (mCylinder && !record(mCylinder))
            mCylinder = 0;
    }
}

scene::Node* TopologyTool::retopoNode() const
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* n = g.node(mRetopoId); n && n->type().isA("mesh"))
        return n;
    scene::Node* n = findNode(mRetopoName);
    return n && n->type().isA("mesh") ? n : nullptr;
}

const retopo::ConstructRecord* TopologyTool::record(int id) const
{
    for (const retopo::ConstructRecord& r : mRecords)
        if (r.id == id)
            return &r;
    return nullptr;
}

bool TopologyTool::surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const
{
    if (mRefBvh.empty())
        return false;
    glm::dvec3 o, d;
    view.ray(px, o, d);
    geo::BvhHit hit;
    if (!mRefBvh.raycast(o, d, hit))
        return false;
    out = hit.point;
    return true;
}

glm::dvec3 TopologyTool::onSurface(const glm::dvec3& p) const
{
    geo::BvhClosest c;
    return !mRefBvh.empty() && mRefBvh.closestPoint(p, c) ? c.point : p;
}

glm::dvec3 TopologyTool::normalAt(const glm::dvec3& p) const
{
    geo::BvhClosest c;
    if (!mRefBvh.empty() && mRefBvh.closestPoint(p, c) && c.face < mRefFaceNormals.size())
        return mRefFaceNormals[c.face];
    return glm::dvec3(0.0, 1.0, 0.0);
}

bool TopologyTool::visible(const glm::dvec3& p, const ViewInfo& view) const
{
    if (mRefBvh.empty())
        return true;
    glm::dvec3 d = p - view.eye;
    double len = glm::length(d);
    double slack = view.unitsPerPixel(p) * 3.0;
    return len <= slack || !mRefBvh.occluded(view.eye, d / len, len - slack);
}

glm::dvec3 TopologyTool::pointWorld(Index p) const { return glm::dvec3(mRetopoWorld * glm::dvec4(mRetopo->position(p), 1.0)); }

std::vector<std::optional<QPointF>> TopologyTool::projectPoints(const ViewInfo& view) const
{
    std::vector<std::optional<QPointF>> out;
    if (!mRetopo)
        return out;
    out.resize(mRetopo->pointCapacity());
    for (Index p = 0; p < mRetopo->pointCapacity(); ++p)
        if (mRetopo->pointValid(p))
        {
            glm::dvec3 w = mMoved.contains(p) ? mMoved.at(p) : pointWorld(p);
            if (visible(w, view))
                out[p] = view.project(w);
        }
    return out;
}

TopologyTool::Hit TopologyTool::pick(QPointF px, const ViewInfo& view) const
{
    Hit h;
    if (!mRetopo)
        return h;
    const geo::Mesh& m = *mRetopo;
    auto screen = projectPoints(view);
    double best = kPickRadiusPx;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (screen[p] && distPx(*screen[p], px) < best)
        {
            best = distPx(*screen[p], px);
            h.point = p;
        }
    double bestEdge = kPickRadiusPx;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        auto a = screen[m.edgePoint(e, 0)], b = screen[m.edgePoint(e, 1)];
        if (a && b)
            if (double d = segmentDistPx(px, *a, *b); d < bestEdge)
            {
                bestEdge = d;
                h.edge = e;
            }
    }
    glm::dvec3 o, d;
    view.ray(px, o, d);
    double bestT = std::numeric_limits<double>::max();
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> pts = m.facePoints(f);
        glm::dvec3 a = pointWorld(pts[0]);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            glm::dvec3 b = pointWorld(pts[k]), c = pointWorld(pts[k + 1]);
            glm::dvec3 e1 = b - a, e2 = c - a, pv = glm::cross(d, e2);
            double det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-18)
                continue;
            glm::dvec3 tv = o - a;
            double u = glm::dot(tv, pv) / det;
            glm::dvec3 qv = glm::cross(tv, e1);
            double v = glm::dot(d, qv) / det, t = glm::dot(e2, qv) / det;
            if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && t > 0.0 && t < bestT)
            {
                bestT = t;
                h.face = f;
            }
        }
    }
    if (h.face != kInvalid)
    {
        glm::dvec3 hit;
        if (surfaceHit(px, view, hit) && glm::length(hit - o) + view.unitsPerPixel(hit) * 4.0 < bestT)
            h.face = kInvalid;
        else
            h.construct = retopo::constructOfFace(m, h.face);
    }
    return h;
}

Index TopologyTool::pickBorderPoint(QPointF px, const ViewInfo& view) const
{
    if (!mRetopo)
        return kInvalid;
    auto screen = projectPoints(view);
    Index bestP = kInvalid;
    double best = kPickRadiusPx * 1.5;
    for (Index p = 0; p < mRetopo->pointCapacity(); ++p)
        if (screen[p] && mRetopo->isBoundaryPoint(p) && distPx(*screen[p], px) < best)
        {
            best = distPx(*screen[p], px);
            bestP = p;
        }
    return bestP;
}

Index TopologyTool::pickBorderEdge(QPointF px, const ViewInfo& view) const
{
    if (!mRetopo)
        return kInvalid;
    auto screen = projectPoints(view);
    Index bestE = kInvalid;
    double best = kPickRadiusPx;
    for (Index e = 0; e < mRetopo->edgeCapacity(); ++e)
    {
        if (!mRetopo->edgeValid(e) || !mRetopo->isBoundaryEdge(e))
            continue;
        auto a = screen[mRetopo->edgePoint(e, 0)], b = screen[mRetopo->edgePoint(e, 1)];
        if (a && b)
            if (double d = segmentDistPx(px, *a, *b); d < best)
            {
                best = d;
                bestE = e;
            }
    }
    return bestE;
}

double TopologyTool::brushWorld(const glm::dvec3& at, const ViewInfo& view) const { return brushRadius() * view.unitsPerPixel(at); }

double TopologyTool::edgeLengthNear(const glm::dvec3& at, const ViewInfo& view) const
{
    if (double e = edgeLength(); e > 0.0)
        return e;
    if (mRetopo && mRetopo->numEdges() > 0)
    {
        double sum = 0.0;
        int n = 0;
        for (Index e = 0; e < mRetopo->edgeCapacity(); ++e)
            if (mRetopo->edgeValid(e))
            {
                sum += glm::length(pointWorld(mRetopo->edgePoint(e, 0)) - pointWorld(mRetopo->edgePoint(e, 1)));
                ++n;
            }
        if (n > 0 && sum > 0.0)
            return sum / n;
    }
    return brushWorld(at, view);
}

// Commands

bool TopologyTool::run(const char* command, const json& call, json* result)
{
    try
    {
        json r = ops::execute(command, call, CallSource::UI);
        if (result)
            *result = std::move(r);
        return true;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
}

json TopologyTool::targetCall(json flags) const
{
    flags["mesh"] = mRetopoName;
    if (scene::Node* ref = scene::Graph::current().node(mReference))
        flags["reference"] = ref->path();
    flags["create"] = true;
    return flags;
}

int TopologyTool::createConstruct(ConstructKind kind, const json& spec, double edgeLen)
{
    json call = targetCall({{"kind", retopo::constructKindName(kind)}, {"spec", spec}});
    if (edgeLen > 0.0)
        call["edgeLength"] = edgeLen;
    json r;
    if (!run("retopoConstruct", call, &r) || !r.is_object())
        return 0;
    if (r.contains("mesh") && r["mesh"].is_string())
        mRetopoName = r["mesh"].get<std::string>();
    refresh();
    mActive = r.value("id", 0);
    return mActive;
}

bool TopologyTool::updateConstruct(int id, ConstructKind kind, const json& spec)
{
    json r;
    if (!run("retopoConstruct", targetCall({{"kind", retopo::constructKindName(kind)}, {"spec", spec}, {"id", id}}), &r))
    {
        refresh();
        return false;
    }
    refresh();
    if (r.value("frozen", false))
    {
        if (mActive == id)
            mActive = 0;
        return false;
    }
    mActive = id;
    return true;
}

bool TopologyTool::editConstruct(int id, const std::function<void(json& spec)>& edit)
{
    refresh();
    const retopo::ConstructRecord* rec = record(id);
    if (!rec)
        return false;
    json spec = rec->spec;
    edit(spec);
    return updateConstruct(id, rec->kind, spec);
}

void TopologyTool::freezeActive()
{
    if (mActive && record(mActive))
        run("retopoConstructFreeze", {{"mesh", mRetopoName}, {"ids", {mActive}}});
    mActive = 0;
    mCylinder = 0;
    refresh();
}

// Borders

TopologyTool::BorderLoop TopologyTool::borderThrough(Index point) const
{
    BorderLoop loop;
    if (!mRetopo || point == kInvalid || !mRetopo->pointValid(point))
        return loop;
    Index edge = kInvalid;
    mRetopo->forEachPointEdge(point, [&](Index e) {
        if (edge == kInvalid && mRetopo->isBoundaryEdge(e))
            edge = e;
    });
    if (edge == kInvalid)
        return loop;
    retopo::BorderRun run = retopo::walkBorder(*mRetopo, edge, 180.0);
    loop.points = run.points;
    loop.closed = run.closed;
    if (loop.closed && !loop.points.empty() && loop.points.front() == loop.points.back())
        loop.points.pop_back();
    for (Index p : loop.points)
        loop.world.push_back(pointWorld(p));
    return loop;
}

std::vector<glm::dvec3> TopologyTool::borderArc(const BorderLoop& loop, Index a, Index b) const
{
    auto ia = std::find(loop.points.begin(), loop.points.end(), a);
    auto ib = std::find(loop.points.begin(), loop.points.end(), b);
    if (ia == loop.points.end() || ib == loop.points.end() || a == b)
        return {};
    int i = int(ia - loop.points.begin()), j = int(ib - loop.points.begin());
    const int n = int(loop.points.size());
    std::vector<glm::dvec3> forward, backward;
    if (!loop.closed)
    {
        for (int k = i;; k += (j > i ? 1 : -1))
        {
            forward.push_back(loop.world[k]);
            if (k == j)
                break;
        }
        return forward;
    }
    for (int k = i;; k = (k + 1) % n)
    {
        forward.push_back(loop.world[k]);
        if (k == j)
            break;
    }
    for (int k = i;; k = (k + n - 1) % n)
    {
        backward.push_back(loop.world[k]);
        if (k == j)
            break;
    }
    return retopo::polylineLength(forward) <= retopo::polylineLength(backward) ? forward : backward;
}

void TopologyTool::deleteAt(QPointF px, const ViewInfo& view)
{
    if (mMode == TopoMode::Pencil)
        if (int gi = pickGuide(px, view); gi >= 0)
        {
            json guides = json::array();
            for (int i = 0; i < int(mGuides.size()); ++i)
                if (i != gi)
                {
                    json pts = json::array();
                    for (const glm::dvec3& p : mGuides[i].points)
                        pts.push_back(vecJson(p));
                    guides.push_back({{"points", pts}, {"closed", mGuides[i].closed},
                                      {"color", json::array({mGuides[i].color.r, mGuides[i].color.g, mGuides[i].color.b})}});
                }
            run("retopoSetGuides", targetCall({{"guides", guides}}));
            refresh();
            return;
        }
    int knotConstruct = 0;
    if (int knot = pickKnot(px, view, &knotConstruct); knot >= 0)
    {
        editConstruct(knotConstruct, [knot](json& spec) {
            if (spec["knots"].size() <= 2)
                return;
            for (const char* key : {"knots", "scales", "twists", "corners"})
                if (spec.contains(key) && spec[key].is_array() && size_t(knot) < spec[key].size())
                    spec[key].erase(spec[key].begin() + knot);
        });
        return;
    }
    if (int stroke = pickPendingStroke(px, view); stroke > 0)
    {
        run("retopoConstructDelete", {{"mesh", mRetopoName}, {"ids", {stroke}}});
        refresh();
        return;
    }
    Hit h = pick(px, view);
    if (h.point != kInvalid)
        run("retopoDrawDelete", {{"mesh", mRetopoName}, {"points", {h.point}}});
    else if (h.face != kInvalid)
        run("retopoDrawDelete", {{"mesh", mRetopoName}, {"faces", {h.face}}});
    refresh();
}

// Live relax

void TopologyTool::beginRelax(const ViewInfo& view)
{
    if (!mRetopo || mRetopo->numFaces() == 0)
        return;
    mRelaxMesh = std::make_unique<geo::Mesh>(*mRetopo);
    for (Index p = 0; p < mRelaxMesh->pointCapacity(); ++p)
        if (mRelaxMesh->pointValid(p))
            mRelaxMesh->setPosition(p, pointWorld(p));
    retopo::LiveRelaxOptions o = relaxOptions();
    o.symmetry = mSymmetry;
    mRelax = std::make_unique<retopo::LiveRelax>(*mRelaxMesh, &mRefBvh, o);
    mRelaxView = view;
    mMoved.clear();
    mTimer->start();
}

void TopologyTool::relaxBrushAt(QPointF px, const ViewInfo& view)
{
    if (!mRelax)
        return;
    mRelaxPx = px;
    auto screen = projectPoints(view);
    const double reach = mRelaxPoint != kInvalid ? brushRadius() * 3.0 : brushRadius();
    QPointF centre = px;
    if (mRelaxPoint != kInvalid && mRelaxPoint < screen.size() && screen[mRelaxPoint])
        centre = *screen[mRelaxPoint];
    std::vector<double> mobility(mRelaxMesh->pointCapacity(), 0.0);
    for (Index p = 0; p < mobility.size(); ++p)
        if (p < screen.size() && screen[p])
        {
            double t = distPx(*screen[p], centre) / reach;
            if (t < 1.0)
                mobility[p] = 1.0 - t * t * (3.0 - 2.0 * t);
        }
    if (mRelaxPoint != kInvalid)
    {
        mobility[mRelaxPoint] = 1.0;
        glm::dvec3 hit;
        if (surfaceHit(px, view, hit))
            mRelax->setTarget(mRelaxPoint, hit, 1.0);
    }
    mRelax->setMobility(std::move(mobility));
    mRelax->wake();
}

void TopologyTool::tickRelax()
{
    if (!mRelax || (mDrag != Drag::RelaxBrush && mDrag != Drag::RelaxPoint))
    {
        mTimer->stop();
        return;
    }
    mRelax->step();
    std::span<const glm::dvec3> x = mRelax->positions();
    for (Index p = 0; p < x.size() && p < mRetopo->pointCapacity(); ++p)
        if (mRetopo->pointValid(p) && glm::length(x[p] - pointWorld(p)) > 1e-9)
            mMoved[p] = x[p];
    viewportTools::requestRedraw();
}

void TopologyTool::commitRelax()
{
    mTimer->stop();
    if (!mMoved.empty())
    {
        json points = json::array(), positions = json::array();
        for (auto& [p, pos] : mMoved)
        {
            points.push_back(p);
            positions.push_back(vecJson(pos));
        }
        run("retopoDrawMove", {{"mesh", mRetopoName}, {"points", points}, {"positions", positions}});
    }
    mRelax.reset();
    mRelaxMesh.reset();
    mRelaxPoint = kInvalid;
    mMoved.clear();
    refresh();
}

// Events

bool TopologyTool::mousePress(QMouseEvent* e, const ViewInfo& view)
{
    refresh();
    mPressPx = e->position();
    mCursor = e->position();
    mCursorView = view.camera;
    mMods = e->modifiers();
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton)
        return false;
    const bool shift = mMods & Qt::ShiftModifier, ctrl = mMods & Qt::ControlModifier;
    mStroke.clear();
    mStrokePx.clear();
    mPreviewId = 0;

    if (e->type() == QEvent::MouseButtonDblClick && e->button() == Qt::LeftButton && !shift && !ctrl)
    {
        int construct = 0;
        if (int knot = pickKnot(e->position(), view, &construct); knot >= 0)
        {
            editConstruct(construct, [knot](json& spec) {
                size_t n = spec["knots"].size();
                if (!spec.contains("corners") || !spec["corners"].is_array() || spec["corners"].size() != n)
                    spec["corners"] = json(std::vector<int>(n, 0));
                spec["corners"][knot] = spec["corners"][knot].get<int>() ? 0 : 1;
            });
            return true;
        }
        if (mMode == TopoMode::Region)
        {
            floodRegionFrom(e->position(), view);
            viewportTools::requestRedraw();
            return true;
        }
        if (mMode == TopoMode::Patch)
            if (const retopo::ConstructRecord* rec = record(pick(e->position(), view).construct); rec && rec->live && rec->kind == ConstructKind::Patch)
            {
                editConstruct(rec->id, [](json& spec) { spec.erase("pull"); });
                return true;
            }
    }
    if (ctrl && (mMods & Qt::AltModifier))
    {
        mDrag = Drag::Delete;
        return true;
    }
    if (shift && mMode != TopoMode::Region)
    {
        mDrag = Drag::RelaxBrush;
        beginRelax(view);
        relaxBrushAt(e->position(), view);
        return true;
    }
    if (mMode == TopoMode::Relax)
    {
        Hit h = pick(e->position(), view);
        mDrag = h.point != kInvalid ? Drag::RelaxPoint : Drag::RelaxBrush;
        mRelaxPoint = h.point;
        beginRelax(view);
        relaxBrushAt(e->position(), view);
        return true;
    }
    if (mMode == TopoMode::Pencil)
    {
        if (e->button() == Qt::MiddleButton)
        {
            mDragGuide = pickGuide(e->position(), view);
            if (mDragGuide < 0 || !surfaceHit(e->position(), view, mGuideFrom))
                return false;
            mDrag = Drag::GuideMove;
            return true;
        }
        mDrag = ctrl ? Drag::Erase : Drag::Stroke;
    }
    else if (e->button() == Qt::MiddleButton)
        return false;
    else if (mMode == TopoMode::Strip)
    {
        int construct = 0;
        int knot = pickKnot(e->position(), view, &construct);
        if (knot >= 0)
        {
            mDrag = Drag::Knot;
            mDragKnot = knot;
            mDragConstruct = construct;
            mActive = construct;
            return true;
        }
        mDrag = Drag::Stroke;
    }
    else if (mMode == TopoMode::Patch)
    {
        Hit h = pick(e->position(), view);
        const retopo::ConstructRecord* rec = record(h.construct);
        if (rec && rec->live && rec->kind == ConstructKind::Patch)
        {
            mDrag = Drag::Pull;
            mDragConstruct = h.construct;
            mActive = h.construct;
            return true;
        }
        mDrag = Drag::Stroke;
    }
    else if (mMode == TopoMode::Cylinder)
        mDrag = Drag::Line;
    else if (mMode == TopoMode::Region)
    {
        mDrag = ctrl ? Drag::Erase : Drag::Paint;
        paintRegion(e->position(), view, !ctrl);
        return true;
    }
    else if (mMode == TopoMode::Handles)
    {
        if (int hi = pickHandle(e->position(), view); hi >= 0)
        {
            mDrag = Drag::Handle;
            mDragHandle = hi;
            return true;
        }
        mDrag = Drag::Stroke;
    }
    else
        mDrag = Drag::Stroke;

    glm::dvec3 hit;
    if (surfaceHit(e->position(), view, hit))
        mStroke.push_back(hit);
    mStrokePx.push_back(e->position());
    return true;
}

bool TopologyTool::mouseMove(QMouseEvent* e, const ViewInfo& view)
{
    mCursor = e->position();
    mCursorView = view.camera;
    mMods = e->modifiers();
    if (mDrag == Drag::None)
    {
        refresh();
        mHover = pick(e->position(), view);
        viewportTools::requestRedraw();
        return true;
    }
    glm::dvec3 hit;
    switch (mDrag)
    {
    case Drag::Stroke:
    case Drag::Erase:
        if (mMode == TopoMode::Region)
        {
            paintRegion(e->position(), view, false);
            break;
        }
        if (distPx(e->position(), mStrokePx.empty() ? mPressPx : mStrokePx.back()) < 2.0)
            break;
        mStrokePx.push_back(e->position());
        if (surfaceHit(e->position(), view, hit))
            mStroke.push_back(hit);
        break;
    case Drag::Line: break;
    case Drag::Paint: paintRegion(e->position(), view, true); break;
    case Drag::RelaxBrush:
    case Drag::RelaxPoint: relaxBrushAt(e->position(), view); break;
    case Drag::Knot:
        if (surfaceHit(e->position(), view, hit))
            if (const retopo::ConstructRecord* rec = record(mDragConstruct))
            {
                json spec = rec->spec;
                if (size_t(mDragKnot) < spec["knots"].size())
                {
                    spec["knots"][mDragKnot] = vecJson(hit);
                    previewConstruct(mDragConstruct, spec);
                }
            }
        break;
    case Drag::Pull:
        if (distPx(e->position(), mPressPx) > 4.0 && surfaceHit(e->position(), view, hit))
            if (const retopo::ConstructRecord* rec = record(mDragConstruct))
            {
                json spec = rec->spec;
                spec["pull"] = vecJson(hit);
                previewConstruct(mDragConstruct, spec);
            }
        break;
    case Drag::Handle:
        if (surfaceHit(e->position(), view, hit) && size_t(mDragHandle) < mXforms.size())
        {
            mXforms[mDragHandle].translate = hit - mXforms[mDragHandle].pivot;
            previewHandles();
        }
        break;
    case Drag::GuideMove: break;
    default: break;
    }
    viewportTools::requestRedraw();
    return true;
}

bool TopologyTool::mouseRelease(QMouseEvent* e, const ViewInfo& view)
{
    mCursor = e->position();
    Drag drag = mDrag;
    mDrag = Drag::None;
    switch (drag)
    {
    case Drag::Delete: deleteAt(e->position(), view); break;
    case Drag::RelaxBrush:
    case Drag::RelaxPoint: commitRelax(); break;
    case Drag::Stroke:
    case Drag::Erase: finishStroke(view, drag == Drag::Erase); break;
    case Drag::Line: commitCylinderLine(mPressPx, e->position(), view); break;
    case Drag::Knot:
    case Drag::Pull:
        if (mPreviewId && !mPreviewSpec.is_null())
            if (const retopo::ConstructRecord* rec = record(mPreviewId))
                updateConstruct(mPreviewId, rec->kind, mPreviewSpec);
        mPreviewId = 0;
        mPreviewSpec = nullptr;
        break;
    case Drag::Handle: commitHandles(); break;
    case Drag::GuideMove:
    {
        glm::dvec3 hit;
        if (mDragGuide >= 0 && mDragGuide < int(mGuides.size()) && surfaceHit(e->position(), view, hit))
        {
            glm::dvec3 delta = hit - mGuideFrom;
            json guides = json::array();
            for (int i = 0; i < int(mGuides.size()); ++i)
            {
                json pts = json::array();
                for (const glm::dvec3& p : mGuides[i].points)
                    pts.push_back(vecJson(i == mDragGuide ? onSurface(p + delta) : p));
                guides.push_back({{"points", pts}, {"closed", mGuides[i].closed},
                                  {"color", json::array({mGuides[i].color.r, mGuides[i].color.g, mGuides[i].color.b})}});
            }
            run("retopoSetGuides", targetCall({{"guides", guides}}));
        }
        mDragGuide = -1;
        break;
    }
    default: break;
    }
    mStroke.clear();
    mStrokePx.clear();
    refresh();
    mHover = pick(e->position(), view);
    viewportTools::requestRedraw();
    return true;
}

bool TopologyTool::wheel(QWheelEvent* e, const ViewInfo& view)
{
    int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    int notches = delta > 0 ? 1 : delta < 0 ? -1 : 0;
    if (notches == 0)
        return false;
    const bool shift = e->modifiers() & Qt::ShiftModifier, ctrl = e->modifiers() & Qt::ControlModifier;
    if (shift && !ctrl)
    {
        setBrushRadius(brushRadius() * std::pow(1.1, delta / 120.0));
        return true;
    }
    refresh();
    QPointF px = e->position();
    if (mMode == TopoMode::Handles)
        if (int hi = pickHandle(px, view); hi >= 0)
        {
            if (ctrl)
                mXforms[hi].rotate += 5.0 * notches;
            else
                mXforms[hi].scale *= std::pow(1.05, notches);
            previewHandles();
            commitHandles();
            return true;
        }
    int knotConstruct = 0;
    if (int knot = pickKnot(px, view, &knotConstruct); knot >= 0)
    {
        editConstruct(knotConstruct, [&](json& spec) {
            size_t n = spec["knots"].size();
            const char* key = ctrl ? "twists" : "scales";
            if (!spec.contains(key) || !spec[key].is_array() || spec[key].size() != n)
                spec[key] = json(std::vector<double>(n, ctrl ? 0.0 : 1.0));
            double v = spec[key][knot].get<double>();
            spec[key][knot] = ctrl ? v + 5.0 * notches : std::clamp(v * std::pow(1.1, notches), 0.05, 20.0);
        });
        return true;
    }
    int id = 0;
    if (int stroke = pickPendingStroke(px, view); stroke > 0)
        id = stroke;
    else
        id = pick(px, view).construct;
    const retopo::ConstructRecord* rec = record(id);
    if (!rec || !rec->live)
        return false;
    return wheelConstruct(id, notches, ctrl, px, view);
}

bool TopologyTool::keyPress(QKeyEvent* e, const ViewInfo&)
{
    if (e->key() == Qt::Key_Escape)
    {
        if (mDrag != Drag::None)
        {
            if (mDrag == Drag::RelaxBrush || mDrag == Drag::RelaxPoint)
            {
                mTimer->stop();
                mRelax.reset();
                mMoved.clear();
            }
            mDrag = Drag::None;
            mStroke.clear();
            mStrokePx.clear();
            mPreviewId = 0;
        }
        else if (mMode == TopoMode::Handles && mRig.handleCount() > 0)
            clearHandles();
        else if (mMode == TopoMode::Cylinder && mCylinder)
            mCylinder = 0;
        else if (mMode == TopoMode::Region && regionFaceCount() > 0)
            clearRegion();
        else
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        viewportTools::requestRedraw();
        return true;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
    {
        if (mMode == TopoMode::Region && regionFaceCount() > 0)
            runRegion();
        else if (mMode == TopoMode::Handles && mRig.handleCount() > 0)
            clearHandles();
        else
            freezeActive();
        viewportTools::requestRedraw();
        return true;
    }
    return false;
}

std::optional<QCursor> TopologyTool::cursor() const { return std::nullopt; }

// Drawing

void TopologyTool::removeOwnDraw(render::FrameState& s) const
{
    if (!mRetopo)
        return;
    scene::Node* n = findNode(mRetopoName);
    if (!n)
        return;
    scene::MeshPtr out = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!out)
        return;
    geo::Bounds b = out->bounds();
    glm::mat4 world(mRetopoWorld);
    float tol = float(std::max(1e-4, glm::length(b.max - b.min) * 1e-4));
    std::erase_if(s.meshes.edit(), [&](const render::MeshDraw& d) {
        if (!d.mesh || d.world != world)
            return false;
        return glm::length(d.mesh->boundsMin - glm::vec3(b.min)) < tol && glm::length(d.mesh->boundsMax - glm::vec3(b.max)) < tol;
    });
}

void TopologyTool::drawRetopo(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, bool here) const
{
    if (!mRetopo)
        return;
    const geo::Mesh& m = *mRetopo;
    auto world = [&](Index i) { return mMoved.contains(i) ? mMoved.at(i) : pointWorld(i); };
    std::span<const int32_t> tags = m.faceAttrs().get<int32_t>(retopo::kConstructAttr);
    auto tagOf = [&](Index f) { return f < tags.size() ? tags[f] : 0; };
    auto live = [&](int id) {
        const retopo::ConstructRecord* r = record(id);
        return r && r->live;
    };
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        int tag = tagOf(f);
        if (mPreviewId && tag == mPreviewId)
            continue;
        std::vector<Index> pts = m.facePoints(f);
        glm::dvec3 a = world(pts[0]), n(0.0);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
            n += glm::cross(world(pts[k]) - a, world(pts[k + 1]) - a);
        double len = glm::length(n);
        double facing = len > 0.0 ? std::abs(glm::dot(n / len, view.viewDir(a))) : 1.0;
        float g = float(0.04 + 0.2 * facing);
        if (tag && live(tag))
            g += tag == mActive ? 0.12f : (here && tag == mHover.construct ? 0.08f : 0.04f);
        glm::vec4 c(g, g, g, 0.45f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
            for (Index q : {pts[0], pts[k], pts[k + 1]})
                s.tris3d.push_back({glm::vec3(world(q)), c});
    }
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        bool liveEdge = false;
        bool skip = false;
        m.forEachEdgeCorner(e, [&](Index c) {
            int tag = tagOf(m.cornerFace(c));
            liveEdge |= tag && live(tag);
            skip |= mPreviewId && tag == mPreviewId && m.edgeFaceCount(e) == 1;
        });
        if (skip)
            continue;
        glm::vec4 c = here && e == mHover.edge && mDrag == Drag::None ? kHotLinear : liveEdge ? kLiveWireLinear : kWireLinear;
        s.linesOnTop.push_back({glm::vec3(world(m.edgePoint(e, 0))), c});
        s.linesOnTop.push_back({glm::vec3(world(m.edgePoint(e, 1))), c});
    }
    auto screen = projectPoints(view);
    for (Index i = 0; i < screen.size(); ++i)
        if (screen[i])
        {
            bool hot = here && i == mHover.point && (mMode == TopoMode::Relax || mMode == TopoMode::Stroke || mMode == TopoMode::Patch);
            p.disc(*screen[i], hot ? 5.5f : 3.5f, hot ? kHotColor : kVertexRim, 12);
            p.disc(*screen[i], hot ? 3.0f : 2.0f, hot ? kHotColor : kVertexFill, 10);
        }
    if (mPreviewId && !mPreviewFaces.empty())
    {
        for (const std::vector<Index>& f : mPreviewFaces)
            for (size_t k = 0; k < f.size(); ++k)
            {
                s.linesOnTop.push_back({glm::vec3(mPreviewPos[f[k]]), kHotLinear});
                s.linesOnTop.push_back({glm::vec3(mPreviewPos[f[(k + 1) % f.size()]]), kHotLinear});
            }
        for (const std::vector<Index>& f : mPreviewFaces)
            for (size_t k = 1; k + 1 < f.size(); ++k)
                for (Index q : {f[0], f[k], f[k + 1]})
                    s.tris3d.push_back({glm::vec3(mPreviewPos[q]), glm::vec4(0.5f, 0.5f, 0.5f, 0.45f)});
    }
}

void TopologyTool::drawQuality(OverlayPainter& p, const ViewInfo& view) const
{
    if (!mRetopo || !qualityOverlay())
        return;
    const geo::Mesh& m = *mRetopo;
    const bool mono = viewportDisplay::monotone();
    auto screen = projectPoints(view);
    for (Index pt = 0; pt < m.pointCapacity(); ++pt)
    {
        if (!screen[pt] || m.isBoundaryPoint(pt))
            continue;
        int valence = 0;
        m.forEachPointEdge(pt, [&](Index) { ++valence; });
        if (valence == 4)
            continue;
        glm::vec4 c = mono ? (valence == 3 ? glm::vec4(0.95f, 0.95f, 0.95f, 1.0f) : valence == 5 ? glm::vec4(0.55f, 0.55f, 0.55f, 1.0f) : glm::vec4(0.1f, 0.1f, 0.1f, 1.0f))
                           : (valence == 3 ? glm::vec4(0.25f, 0.6f, 1.0f, 1.0f) : valence == 5 ? glm::vec4(1.0f, 0.6f, 0.15f, 1.0f) : glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));
        float r = valence >= 6 ? 7.0f : 5.5f;
        p.disc(*screen[pt], r + 1.5f, glm::vec4(0.0f, 0.0f, 0.0f, 0.8f), 16);
        p.disc(*screen[pt], r, c, 16);
    }
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || m.faceSize(f) == 4)
            continue;
        glm::dvec3 centre = glm::dvec3(mRetopoWorld * glm::dvec4(m.faceCentroid(f), 1.0));
        if (!visible(centre, view))
            continue;
        if (auto sp = view.project(centre))
        {
            glm::vec4 c = mono ? glm::vec4(0.9f, 0.9f, 0.9f, 1.0f) : (m.faceSize(f) == 3 ? glm::vec4(1.0f, 0.85f, 0.2f, 1.0f) : glm::vec4(1.0f, 0.3f, 0.8f, 1.0f));
            p.label(*sp, m.faceSize(f) == 3 ? "tri" : QString::number(m.faceSize(f)), c);
        }
    }
}

void TopologyTool::drawSurfacePolyline(OverlayPainter& p, const ViewInfo& view, const std::vector<glm::dvec3>& pts, bool closed, float width,
                                       const glm::vec4& color, float dash) const
{
    std::vector<QPointF> run;
    auto flush = [&]() {
        if (run.size() > 1)
        {
            if (dash > 0.0f)
                p.dashedPolyline(run, width, dash, color);
            else
                p.polyline(run, width, color);
        }
        run.clear();
    };
    const size_t n = pts.size();
    for (size_t i = 0; i < n + (closed && n > 2 ? 1 : 0); ++i)
    {
        const glm::dvec3& w = pts[i % n];
        std::optional<QPointF> sp = visible(w, view) ? view.project(w) : std::nullopt;
        if (sp)
            run.push_back(*sp);
        else
            flush();
    }
    flush();
}

void TopologyTool::drawGuides(OverlayPainter& p, const ViewInfo& view) const
{
    const bool mono = viewportDisplay::monotone();
    for (size_t i = 0; i < mGuides.size(); ++i)
    {
        const retopo::Guide& g = mGuides[i];
        bool hot = mMode == TopoMode::Pencil && mDragGuide == int(i);
        glm::vec4 c = mono ? glm::vec4(0.85f, 0.85f, 0.85f, 1.0f) : glm::vec4(g.color, 1.0f);
        // Monotone tells guides apart by dash length instead of hue.
        float dash = mono ? float(4 + (i % 4) * 4) : 0.0f;
        drawSurfacePolyline(p, view, g.points, g.closed, hot ? 3.5f : 2.5f, c, dash);
    }
}

void TopologyTool::drawBrush(OverlayPainter& p, double radius) const
{
    std::vector<QPointF> circle;
    for (int k = 0; k < 48; ++k)
    {
        double a = k / 48.0 * 6.283185307179586;
        circle.push_back(mCursor + QPointF(std::cos(a), std::sin(a)) * radius);
    }
    p.polyline(circle, 1.5f, kTopoBrush, true);
}

QString TopologyTool::hint() const
{
    switch (mMode)
    {
    case TopoMode::Pencil: return "Pencil: drag to draw a guide, Ctrl-drag erases, MMB-drag moves one";
    case TopoMode::Patch: return "Patch: draw a closed shape, or from border to border; click a border to fill its hole; drag in a patch to pull its poles";
    case TopoMode::Strip: return "Strip: sketch a path (brush = width); drag knots, wheel scales a knot, Ctrl-wheel twists, double-click toggles a corner";
    case TopoMode::Stroke: return "Stroke: draw strokes to bridge them; start on a border to extrude from it, or draw along a border to bridge to it";
    case TopoMode::Cylinder: return "Cylinder: drag a line across a limb for each ring; a line across the cylinder adds a ring; Enter finishes";
    case TopoMode::Region: return "Auto Region: paint the area (Ctrl erases), double-click inside a hole to mark it, Enter to retopologise";
    case TopoMode::Handles: return "Handles: drag across faces to place handles, drag one to move, wheel scales, Ctrl-wheel turns, Esc clears";
    case TopoMode::Relax: return "Relax: drag a vertex and its neighbours follow, drag elsewhere to relax under the brush";
    default: return {};
    }
}

void TopologyTool::draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    refresh();
    removeOwnDraw(s);
    if (mDrag == Drag::None)
        mMods = QGuiApplication::queryKeyboardModifiers();
    const bool here = view.camera == mCursorView;
    drawRetopo(s, p, view, here);
    drawGuides(p, view);
    drawConstructs(s, p, view, here);
    drawQuality(p, view);

    if (here && !mStrokePx.empty() && (mDrag == Drag::Stroke || mDrag == Drag::Erase) && mMode != TopoMode::Region)
        p.polyline(mStrokePx, mDrag == Drag::Erase ? 1.5f : 2.0f, kHotColor);
    if (here && mDrag == Drag::Line)
    {
        p.line(mPressPx, mCursor, 2.0f, kHotColor);
        p.disc(mPressPx, 3.5f, kHotColor, 12);
        p.disc(mCursor, 3.5f, kHotColor, 12);
    }
    const bool shift = mMods & Qt::ShiftModifier, ctrl = mMods & Qt::ControlModifier;
    if (here && (mDrag == Drag::RelaxBrush || (mDrag == Drag::None && shift && !ctrl && mMode != TopoMode::Region) ||
                 (mDrag == Drag::None && (mMode == TopoMode::Relax || mMode == TopoMode::Region || mMode == TopoMode::Strip ||
                                          mMode == TopoMode::Handles)) ||
                 mDrag == Drag::Paint || (mDrag == Drag::Erase && mMode == TopoMode::Region) || (mDrag == Drag::Stroke && mMode == TopoMode::Strip)))
        drawBrush(p, brushRadius());
    if (mRefBvh.empty())
        drawToolMessage(p, view, "Topology: select a reference mesh first");
    else if (here)
        drawToolMessage(p, view, hint());
}

TopologyTool* activeTopologyTool() { return dynamic_cast<TopologyTool*>(viewportTools::active()); }

namespace
{
/** Targets for a topology session: the running Topology or Retopo Draw tool's, else the selection's. */
std::optional<retopo::RetopoDrawTargets> sessionTargets()
{
    if (TopologyTool* t = activeTopologyTool())
        return retopo::RetopoDrawTargets{t->reference(), t->retopoName()};
    if (RetopoDrawTool* t = activeRetopoDraw())
        return retopo::RetopoDrawTargets{t->reference(), t->retopoName()};
    scene::Graph& g = scene::Graph::current();
    return retopo::resolveRetopoDraw(g, g.selection());
}

void startMode(TopoMode mode)
{
    if (TopologyTool* t = activeTopologyTool())
    {
        t->setMode(mode);
        return;
    }
    std::optional<retopo::RetopoDrawTargets> t = sessionTargets();
    if (!t)
    {
        log::warn("Topology: select the reference mesh, or a retopo mesh to resume on");
        return;
    }
    viewportTools::set(std::make_unique<TopologyTool>(t->reference, t->retopo, mode));
}

/** The retopo mesh the session actions work on, or empty. */
std::string sessionRetopoName()
{
    std::optional<retopo::RetopoDrawTargets> t = sessionTargets();
    if (!t || !findNode(t->retopo))
        return {};
    return t->retopo;
}
} // namespace

void registerTopologyActions()
{
    static const char* docs[] = {
        "Draw guides on the reference to plan the edge flow; auto regions follow them.",
        "Fill a closed shape, or the space between a stroke and the retopo border, with a live quad patch; click a border to fill its hole.",
        "Sketch a band of quads along a path, the brush setting its width; its knots stay as handles.",
        "Draw strokes and bridge them into quads; a stroke from a border extrudes it, one along a border bridges to it.",
        "Drag lines across a limb: each makes a ring round the reference, bridged to the last.",
        "Paint part of the reference and auto retopologise it, stitched to the retopo faces around it.",
        "Drag across retopo faces to place handles, then move, scale and turn them to reshape that area.",
        "Physics relax: drag a vertex and its neighbours follow like cloth on the surface; drag elsewhere to relax under the brush.",
    };
    for (int i = 0; i < int(TopoMode::Count); ++i)
    {
        TopoMode mode = TopoMode(i);
        actions::add({
            .id = topoModeAction(mode),
            .label = std::string("Topology ") + topoModeLabel(mode),
            .category = "Topology",
            .run = [mode] { startMode(mode); },
            .doc = docs[i],
            .python = std::string("lila.ui.run_action(\"") + topoModeAction(mode) + "\")",
            .checked = [mode] {
                TopologyTool* t = activeTopologyTool();
                return t && t->mode() == mode;
            },
        });
    }
    actions::add({
        .id = "topo.freezeAll",
        .label = "Freeze All Constructs",
        .category = "Topology",
        .run = [] {
            if (std::string mesh = sessionRetopoName(); !mesh.empty())
                runUi("retopoConstructFreeze", {{"mesh", mesh}});
        },
        .doc = "Leaves every live patch, strip, cylinder and region on the retopo mesh as plain faces.",
        .python = "lila.ops.retopoConstructFreeze(mesh=\"body_retopo\")",
    });
    actions::add({
        .id = "topo.settle",
        .label = "Settle Retopo Mesh",
        .category = "Topology",
        .run = [] {
            if (std::string mesh = sessionRetopoName(); !mesh.empty())
            {
                retopo::LiveRelaxOptions o = TopologyTool::relaxOptions();
                runUi("retopoSettle", {{"mesh", mesh}, {"evenSize", o.evenSize}, {"squareness", o.squareness}});
            }
        },
        .doc = "Live relaxes the whole retopo mesh to rest on its reference: even, square faces, borders held.",
        .python = "lila.ops.retopoSettle(mesh=\"body_retopo\")",
    });
    actions::add({
        .id = "topo.qualityOverlay",
        .label = "Topology Quality Overlay",
        .category = "Topology",
        .run = [] { TopologyTool::setQualityOverlay(!TopologyTool::qualityOverlay()); },
        .doc = "Marks poles by valence, triangles and n-gons on the retopo mesh while a topology tool runs.",
        .python = "lila.prefs.set(\"general\", \"/topology/qualityOverlay\", True)",
        .checked = [] { return TopologyTool::qualityOverlay(); },
    });
    actions::add({
        .id = "topo.runRegion",
        .label = "Retopologise Marked Region",
        .category = "Topology",
        .run = [] {
            if (TopologyTool* t = activeTopologyTool())
                t->runRegion();
            else
                log::warn("Retopologise Marked Region: mark a region with Topology Auto Region first");
        },
        .doc = "Auto retopologises the area marked with Auto Region and stitches it to the retopo faces around it.",
        .python = "lila.ui.run_action(\"topo.runRegion\")",
    });
}
} // namespace lila::ui
