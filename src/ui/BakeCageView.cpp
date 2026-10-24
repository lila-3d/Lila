#include "ui/BakeCageView.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "bake/Arrange.h"
#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "bake/CagePreview.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Triangulate.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/MaterialDraw.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QPointer>
#include <QTimer>

#include <chrono>
#include <deque>
#include <map>
#include <set>

namespace lila::ui::bakeCage
{
namespace
{
using scene::kNoNode;
using scene::NodeId;
using Clock = std::chrono::steady_clock;

scene::Node* findNode(scene::Graph& g, const std::string& name)
{
    if (name.empty())
        return nullptr;
    try
    {
        return g.find(name);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

scene::MeshPtr exported(scene::Graph& g, NodeId id)
{
    scene::MeshPtr m = scene::modifiers::exportMesh(g, id);
    if (!m)
        if (const scene::PlugValue v = g.value({id, scene::MeshNode::kOutMesh, -1}); std::holds_alternative<scene::MeshPtr>(v))
            m = std::get<scene::MeshPtr>(v);
    return m;
}

glm::dmat4 displayWorld(scene::Graph& g, NodeId id)
{
    return bake::arrange::displayWorld(g, id, g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix));
}

/** The shell of one low (or of an editable cage), cached until its inputs change. */
struct Shell
{
    scene::MeshPtr mesh;
    glm::dmat4 world{0.0};
    double frontal = -1.0;
    double offset = -1.0;
    uint64_t workingVersion = 0;
    std::vector<render::LineVertex> tris;
    std::vector<render::LineVertex> wire;
};

/** Preview textures of one group as the renderer takes them. */
struct PreviewTextures
{
    uint64_t generation = 0;
    std::map<std::string, std::pair<render::TexturePtr, render::TexturePtr>> byMesh;
    bool greenUp = false;
};

struct State
{
    std::set<NodeId> shown;
    NodeId painting = kNoNode;
    bool live = false;
    std::map<NodeId, std::vector<float>> working;
    uint64_t workingVersion = 1;
    std::map<NodeId, Shell> shells;
    std::map<NodeId, PreviewTextures> textures;
    /** Lows of the displayed groups, to their group. */
    std::map<NodeId, NodeId> lowGroup;
    /** Nodes whose change dirties a displayed group: groups, lows, scene highs, cages. */
    std::map<NodeId, NodeId> watched;
    /** Scene highs of the painted group, hidden in the viewport so the low and its cage show. */
    std::set<NodeId> paintHighs;
    std::deque<NodeId> queue;
    Clock::time_point lastRequest{};
    QTimer* debounce = nullptr;
    int graphListener = 0;
    bool listening = false;
    std::vector<std::pair<QPointer<QObject>, std::function<void()>>> changed;
};

State& state()
{
    static State s;
    return s;
}

void notifyChanged()
{
    auto& cbs = state().changed;
    std::erase_if(cbs, [](const auto& c) { return !c.first; });
    for (const auto& c : std::vector(cbs))
        if (c.first)
            c.second();
}

std::set<NodeId> displayed()
{
    State& st = state();
    std::set<NodeId> out = st.shown;
    if (st.painting != kNoNode)
        out.insert(st.painting);
    return out;
}

void rebuildWatch()
{
    State& st = state();
    scene::Graph& g = scene::Graph::current();
    st.lowGroup.clear();
    st.watched.clear();
    st.paintHighs.clear();
    for (NodeId gid : displayed())
    {
        scene::Node* grp = g.node(gid);
        if (!grp || !bake::isGroup(*grp))
            continue;
        st.watched[gid] = gid;
        const bake::GroupSettings s = bake::groupSettings(*grp);
        for (const std::string& l : s.lows)
            if (scene::Node* n = findNode(g, l))
            {
                st.lowGroup[n->id()] = gid;
                st.watched[n->id()] = gid;
            }
        for (const bake::HighEntry& h : s.highs)
            if (scene::Node* n = findNode(g, h.node))
            {
                st.watched[n->id()] = gid;
                if (gid == st.painting && !h.self && !st.lowGroup.contains(n->id()))
                    st.paintHighs.insert(n->id());
            }
        if (scene::Node* c = findNode(g, s.cage))
            st.watched[c->id()] = gid;
    }
}

/** Starts the next queued preview when none runs. */
void pump()
{
    State& st = state();
    if (!st.live || bake::cagePreviewBusy())
        return;
    scene::Graph& g = scene::Graph::current();
    while (!st.queue.empty())
    {
        const NodeId gid = st.queue.front();
        st.queue.pop_front();
        scene::Node* grp = g.node(gid);
        if (!grp || !bake::isGroup(*grp) || !displayed().contains(gid))
            continue;
        bake::CagePreviewOptions o;
        for (const auto& [low, values] : st.working)
            if (scene::Node* n = g.node(low); n && st.lowGroup.contains(low) && st.lowGroup[low] == gid)
                o.offsets[n->path()] = values;
        try
        {
            bake::requestCagePreview(g, gid, o);
            st.lastRequest = Clock::now();
            return;
        }
        catch (const std::exception& e)
        {
            log::warn("Cage preview of {}: {}", grp->name(), e.what());
        }
    }
}

void enqueue(NodeId gid)
{
    State& st = state();
    if (std::ranges::find(st.queue, gid) == st.queue.end())
        st.queue.push_back(gid);
}

void schedule(NodeId gid)
{
    State& st = state();
    if (!st.live)
        return;
    enqueue(gid);
    if (!st.debounce)
    {
        st.debounce = new QTimer();
        st.debounce->setSingleShot(true);
        st.debounce->setInterval(200);
        QObject::connect(st.debounce, &QTimer::timeout, [] { pump(); });
    }
    st.debounce->start();
}

void listen()
{
    State& st = state();
    if (st.listening)
        return;
    st.listening = true;
    st.graphListener = scene::Graph::current().addListener([](scene::Change c, NodeId id) {
        State& st = state();
        if (c == scene::Change::Selection)
            return;
        if (c == scene::Change::Created || c == scene::Change::Deleted || c == scene::Change::Renamed || c == scene::Change::Reset ||
            c == scene::Change::Cleared)
        {
            if (c == scene::Change::Cleared || c == scene::Change::Reset)
            {
                scene::Graph& g = scene::Graph::current();
                std::erase_if(st.shown, [&](NodeId gid) { return !g.node(gid); });
                if (st.painting != kNoNode && !g.node(st.painting))
                    st.painting = kNoNode;
            }
            rebuildWatch();
            for (NodeId gid : displayed())
                schedule(gid);
            return;
        }
        if (auto it = st.watched.find(id); it != st.watched.end())
        {
            if (id == it->second)
                rebuildWatch();
            schedule(it->second);
        }
    });
    bake::addCagePreviewListener([](const std::shared_ptr<const bake::CagePreview>&) {
        viewportTools::requestRedraw();
        pump();
    });
}

render::TexturePtr toTexture(const bake::CagePreviewTexture& t, bool srgb)
{
    auto img = std::make_shared<render::TextureImage>();
    img->width = t.width;
    img->height = t.height;
    img->srgb = srgb;
    img->rgba.resize(size_t(t.width) * size_t(t.height) * 4);
    for (size_t i = 0; i < t.value.size(); ++i)
        for (int k = 0; k < 4; ++k)
            img->rgba[i * 4 + size_t(k)] = uint8_t(std::clamp(t.value[i][k], 0.0f, 1.0f) * 255.0f + 0.5f);
    return img;
}

const PreviewTextures* texturesOf(NodeId gid)
{
    State& st = state();
    const auto pv = bake::latestCagePreview(gid);
    if (!pv)
        return nullptr;
    PreviewTextures& pt = st.textures[gid];
    if (pt.generation != pv->generation)
    {
        pt = {};
        pt.generation = pv->generation;
        for (const bake::CagePreviewTexture& t : pv->textures)
        {
            if (t.map != "normal")
                continue;
            const bake::CagePreviewTexture* err = pv->find(t.mesh, "error");
            pt.byMesh[t.mesh] = {toTexture(t, false), err ? toTexture(*err, true) : nullptr};
            pt.greenUp = t.greenUp;
        }
    }
    return &pt;
}

bool previewMaterials(NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& out)
{
    State& st = state();
    if (!st.live)
        return false;
    auto it = st.lowGroup.find(id);
    if (it == st.lowGroup.end())
        return false;
    const PreviewTextures* pt = texturesOf(it->second);
    scene::Node* n = scene::Graph::current().node(id);
    if (!pt || !n)
        return false;
    auto tex = pt->byMesh.find(n->path());
    if (tex == pt->byMesh.end())
        return false;
    out = drawMaterials(mesh);
    if (out.empty())
        out.resize(1);
    for (render::Material& m : out)
    {
        m.normalMap = tex->second.first;
        m.flags |= render::MatNormalMap;
        if (pt->greenUp)
            m.flags |= render::MatNormalGreenUp;
        else
            m.flags &= ~uint32_t(render::MatNormalGreenUp);
        if (tex->second.second)
        {
            m.emissiveMap = tex->second.second;
            m.emissive = glm::vec3(1.0f);
            m.flags |= render::MatEmissiveMap;
        }
    }
    return true;
}

/** Shell triangles and wire for a mesh whose points sit at pts (world). */
void buildShell(Shell& sh, const geo::Mesh& m, const std::vector<glm::dvec3>& pts)
{
    const glm::vec4 fill(0.5f, 0.5f, 0.5f, 0.22f), line(0.62f, 0.62f, 0.62f, 0.75f);
    const geo::Triangulation tris = geo::triangulate(m);
    sh.tris.resize(tris.corners.size());
    tbb::parallel_for(size_t(0), tris.corners.size(), [&](size_t c) { sh.tris[c] = {glm::vec3(pts[m.cornerPoint(tris.corners[c])]), fill}; });
    sh.wire.clear();
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            sh.wire.push_back({glm::vec3(pts[m.edgePoint(e, 0)]), line});
            sh.wire.push_back({glm::vec3(pts[m.edgePoint(e, 1)]), line});
        }
}

/** The group's frontal as bakes resolve it: set, else the last preview's measurement, else the size fallback. */
double frontalOf(scene::Graph& g, NodeId gid, const bake::GroupSettings& s)
{
    if (s.frontal > 0.0)
        return s.frontal;
    if (const auto pv = bake::latestCagePreview(gid); pv && pv->stats.measured && pv->stats.frontal > 0.0)
        return pv->stats.frontal;
    return bake::groupFrontal(g, s);
}

void drawGroup(render::FrameState& f, scene::Graph& g, NodeId gid)
{
    LILA_ZONE();

    State& st = state();
    scene::Node* grp = g.node(gid);
    if (!grp || !bake::isGroup(*grp))
        return;
    const bake::GroupSettings s = bake::groupSettings(*grp);
    auto pushShell = [&](const Shell& sh) {
        f.tris3d.insert(f.tris3d.end(), sh.tris.begin(), sh.tris.end());
        f.lines.insert(f.lines.end(), sh.wire.begin(), sh.wire.end());
    };
    if (scene::Node* c = findNode(g, s.cage); c && c->type().isA("mesh"))
    {
        scene::MeshPtr m = exported(g, c->id());
        if (!m)
            return;
        Shell& sh = st.shells[c->id()];
        const glm::dmat4 w = displayWorld(g, c->id());
        if (sh.mesh != m || sh.world != w)
        {
            sh.mesh = m;
            sh.world = w;
            std::vector<glm::dvec3> pts(m->pointCapacity());
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                pts[p] = glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
            buildShell(sh, *m, pts);
        }
        pushShell(sh);
        return;
    }
    const double frontal = frontalOf(g, gid, s);
    for (const std::string& l : s.lows)
    {
        scene::Node* n = findNode(g, l);
        if (!n || !n->type().isA("mesh"))
            continue;
        scene::MeshPtr m = exported(g, n->id());
        if (!m)
            continue;
        Shell& sh = st.shells[n->id()];
        const glm::dmat4 w = displayWorld(g, n->id());
        auto work = st.working.find(n->id());
        const uint64_t version = work != st.working.end() ? st.workingVersion : 0;
        if (sh.mesh != m || sh.world != w || sh.frontal != frontal || sh.offset != s.cageOffset || sh.workingVersion != version)
        {
            sh.mesh = m;
            sh.world = w;
            sh.frontal = frontal;
            sh.offset = s.cageOffset;
            sh.workingVersion = version;
            const std::vector<float> values = work != st.working.end() && work->second.size() == m->pointCapacity()
                                                  ? work->second
                                                  : bake::pointValues(*m, *bake::paintAttr(bake::kOffsetAttr));
            buildShell(sh, *m, bake::autoCagePoints(*m, w, frontal, s.cageOffset, values));
        }
        pushShell(sh);
    }
}

void toggleShown()
{
    scene::Graph& g = scene::Graph::current();
    const std::vector<NodeId> groups = selectedGroups(g);
    if (groups.empty())
    {
        log::warn("Show Bake Cage: select a bake group or its low");
        return;
    }
    const bool on = !std::ranges::all_of(groups, [](NodeId id) { return shown(id); });
    for (NodeId id : groups)
        setShown(id, on);
}
} // namespace

scene::Node* groupOfMesh(scene::Graph& g, NodeId mesh)
{
    scene::Node* m = g.node(mesh);
    if (!m)
        return nullptr;
    const std::string path = m->path();
    for (NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n && bake::isGroup(*n))
        {
            const bake::GroupSettings s = bake::groupSettings(*n);
            if (std::ranges::find(s.lows, path) != s.lows.end() || s.cage == path)
                return n;
        }
    return nullptr;
}

std::vector<NodeId> selectedGroups(scene::Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        scene::Node* grp = n && bake::isGroup(*n) ? n : n && n->type().isA("mesh") ? groupOfMesh(g, id) : nullptr;
        if (grp && std::ranges::find(out, grp->id()) == out.end())
            out.push_back(grp->id());
    }
    return out;
}

bool shown(NodeId group) { return state().shown.contains(group); }

void setShown(NodeId group, bool on)
{
    State& st = state();
    listen();
    if (on)
        st.shown.insert(group);
    else
    {
        st.shown.erase(group);
        st.textures.erase(group);
    }
    rebuildWatch();
    if (on)
        schedule(group);
    viewportTools::requestRedraw();
    notifyChanged();
}

bool livePreview() { return state().live; }

void setLivePreview(bool on)
{
    State& st = state();
    listen();
    st.live = on;
    if (on)
        for (NodeId gid : displayed())
            schedule(gid);
    else
    {
        st.queue.clear();
        bake::cancelCagePreview(false);
    }
    viewportTools::requestRedraw();
    notifyChanged();
}

void onChanged(QObject* owner, std::function<void()> fn)
{
    state().changed.emplace_back(owner, std::move(fn));
}

void setWorkingOffsets(NodeId low, std::span<const float> values, bool rebake)
{
    State& st = state();
    st.working[low].assign(values.begin(), values.end());
    ++st.workingVersion;
    if (!rebake || !st.live)
        return;
    auto it = st.lowGroup.find(low);
    if (it == st.lowGroup.end())
        return;
    // Throttled: a preview at most every 150 ms, and never queued behind a running one mid-stroke.
    if (bake::cagePreviewBusy() || Clock::now() - st.lastRequest < std::chrono::milliseconds(150))
        return;
    enqueue(it->second);
    pump();
}

void clearWorkingOffsets(NodeId low)
{
    State& st = state();
    if (st.working.erase(low))
        ++st.workingVersion;
}

void setPainting(NodeId group)
{
    State& st = state();
    if (st.painting == group)
        return;
    listen();
    st.painting = group;
    rebuildWatch();
    if (group != kNoNode)
        schedule(group);
    viewportTools::requestRedraw();
}

void registerBakeCageView()
{
    viewportHooks::addDrawer("lila.bakeCage", [](render::FrameState& f, OverlayPainter&, const ViewInfo&) {
        if (state().shown.empty() && state().painting == kNoNode)
            return;
        scene::Graph& g = scene::Graph::current();
        for (NodeId gid : displayed())
            drawGroup(f, g, gid);
    });
    viewportHooks::setMaterialHook("lila.bakeCagePreview", previewMaterials);
    viewportHooks::setHideHook("lila.bakeCagePaint", [](NodeId id) { return state().paintHighs.contains(id); });

    actions::add({.id = "bake.showCage",
                  .label = "Show Bake Cage",
                  .category = "Bake",
                  .run = toggleShown,
                  .doc = "Toggles the selected bake groups' cage in the viewport: a translucent shell with its wire where rays start "
                         "(the editable cage, or the auto cage with its painted offsets).",
                  .python = "lila.ui.run_action(\"bake.showCage\")"});
    actions::add({.id = "bake.livePreview",
                  .label = "Live Bake Preview",
                  .category = "Bake",
                  .run = [] { setLivePreview(!livePreview()); },
                  .doc = "Toggles live preview bakes: shown and painted groups rebake a low resolution normal map in memory as their "
                         "cage changes, drawn on the lows with missed rays glowing red. Nothing is written.",
                  .python = "lila.ui.run_action(\"bake.livePreview\")"});
    actions::add({.id = "bake.estimateCage",
                  .label = "Estimate Cage",
                  .category = "Bake",
                  .doc = "Paints the selected group's cage offsets so every ray starts just outside the high and meets it, and sets "
                         "its frontal and rear distances. One undo step.",
                  .command = "bakeEstimateCage",
                  .call = json::object()});
}
} // namespace lila::ui::bakeCage
