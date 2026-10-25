#include "ui/SculptTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "brush/BrushUi.h"
#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"
#include "ui/Actions.h"
#include "ui/MeshCache.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/Tools.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QWidget>
#include <QTimer>

#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
constexpr const char* kPrefs = "general";

std::map<int, std::function<void()>>& settingsListeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}

SculptSettings loadSettings()
{
    SculptSettings s;
    const PrefStore& p = PrefStore::get();
    if (auto b = sculpt::brushFromName(p.value<std::string>(kPrefs, "/sculpt/brush", "draw")))
        s.brush = *b;
    s.stroke.loadPrefs("/sculpt");
    if (!brush::Falloff::find(s.stroke.falloff))
        s.stroke.falloff = "smooth";
    s.stroke.radius = std::clamp(s.stroke.radius, 2.0, 2000.0);
    s.stroke.spacing = std::clamp(s.stroke.spacing, 0.02, 2.0);
    s.frontFacesOnly = p.value<bool>(kPrefs, "/sculpt/frontFacesOnly", s.frontFacesOnly);
    s.showCage = p.value<bool>(kPrefs, "/sculpt/showCage", s.showCage);
    s.proxyMode = p.value<std::string>(kPrefs, "/sculpt/proxyMode", s.proxyMode);
    s.levels = std::clamp(p.value<int>(kPrefs, "/sculpt/levels", s.levels), 1, 6);
    s.resolution = std::clamp(p.value<int>(kPrefs, "/sculpt/resolution", s.resolution), 16, 2048);
    s.exitMethod = p.value<std::string>(kPrefs, "/sculpt/exitMethod", s.exitMethod);
    s.keepProxy = p.value<bool>(kPrefs, "/sculpt/keepProxy", s.keepProxy);
    return s;
}

glm::dvec3 xformPoint(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }
glm::dvec3 xformDir(const glm::dmat4& m, const glm::dvec3& d) { return glm::dvec3(m * glm::dvec4(d, 0.0)); }

json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

class SculptTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "sculpt"; }

    void activate() override
    {
        mControls.activate();
        mListener = sculpt::addListener([] { viewportTools::requestRedraw(); });
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        mControls.deactivate();
        sculpt::removeListener(mListener);
        viewportTools::requestRedraw();
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        auto s = sculpt::active();
        if (!s || !scene::Graph::current().node(s->node))
            return true;
        Hit hit;
        const bool line = sculptSettings().stroke.mode == brush::StrokeMode::Line && sculptSettings().brush != sculpt::Brush::Grab;
        if (!surfaceHit(*s, e->position(), view, hit) && !line)
            return false;
        beginStroke(*s, e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mouseMove(e))
            return true;
        if (mStroking)
        {
            strokeTo(e, view);
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        switch (e->key())
        {
        case Qt::Key_Escape:
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        default: return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        const SculptSettings& st = sculptSettings();
        auto s = sculpt::active();
        scene::Node* n = s ? scene::Graph::current().node(s->node) : nullptr;
        if (!n)
        {
            drawToolMessage(p, view, "Sculpt: select a mesh and Enter Sculpt");
            return;
        }
        // Shift smooths and Ctrl inverts the next stroke, so the message names the brush that stroke would use.
        const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        const sculpt::Brush brush = mStroking ? mSettings.brush : (mods & Qt::ShiftModifier) ? sculpt::Brush::Smooth : st.brush;
        const bool invert = mStroking ? mSettings.invert : (mods & Qt::ControlModifier) != 0;
        QString name = QString::fromUtf8(sculpt::brushName(brush).data(), qsizetype(sculpt::brushName(brush).size()));
        if (invert)
            name += " (inverted)";
        drawToolMessage(p, view, QString("Sculpt %1: %2   radius %3 px   strength %4%5")
                              .arg(QString::fromStdString(n->path()), name)
                              .arg(int(std::lround(st.stroke.radius)))
                              .arg(st.stroke.strength, 0, 'f', 2)
                              .arg(s->sculpt.symmetric() ? "   symmetry" : ""));

        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        Hit hit;
        std::optional<brush::ui::CursorSurface> surface;
        if (surfaceHit(*s, at, view, hit))
            surface = brush::ui::CursorSurface{hit.world, hit.worldNormal};
        brush::ui::drawCursor(p, view, st.stroke, at, surface,
                              {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength,
                               .normalTick = true,
                               .stroke = &mStroke});
    }

private:
    struct Hit
    {
        glm::dvec3 local{0.0}, normal{0.0};
        glm::dvec3 world{0.0}, worldNormal{0.0};
        glm::dvec3 viewDir{0.0}; // local
    };

    static glm::dmat4 worldOf(const sculpt::Session& s)
    {
        return scene::Graph::current().node(s.node)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    }

    bool surfaceHit(const sculpt::Session& s, QPointF px, const ViewInfo& view, Hit& out) const
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        glm::dmat4 w = worldOf(s), inv = glm::inverse(w);
        geo::BvhHit hit;
        glm::dvec3 lo = xformPoint(inv, o), ld = xformDir(inv, d);
        if (!s.sculpt.raycast(lo, ld, hit))
            return false;
        out.local = hit.point;
        out.normal = s.sculpt.normalAt(hit);
        out.world = xformPoint(w, hit.point);
        glm::dvec3 wn = glm::transpose(glm::inverse(glm::dmat3(w))) * out.normal;
        out.worldNormal = glm::length(wn) > 0.0 ? glm::normalize(wn) : glm::dvec3(0, 1, 0);
        out.viewDir = glm::normalize(ld);
        return true;
    }

    void beginStroke(sculpt::Session& s, const QMouseEvent* e, const ViewInfo& view)
    {
        const Qt::KeyboardModifiers mods = e->modifiers();
        const SculptSettings& st = sculptSettings();
        mSession = sculpt::active();
        sculpt::syncSymmetry(s);
        mSettings.brush = (mods & Qt::ShiftModifier) ? sculpt::Brush::Smooth : st.brush;
        mSettings.falloff = st.stroke.resolveFalloff();
        mFalloffName = st.stroke.falloff;
        mSettings.strength = st.stroke.strength;
        mSettings.invert = (mods & Qt::ControlModifier) != 0;
        mSettings.frontFacesOnly = st.frontFacesOnly;
        mSettings.radial = st.stroke.radial;
        s.sculpt.beginStroke(mSettings);
        mDabs = json::array();
        mStroking = true;
        brush::BrushSettings stroke = st.stroke;
        // Grab drags one dab across the view plane rather than laying a trail of them.
        if (mSettings.brush == sculpt::Brush::Grab)
            stroke.mode = brush::StrokeMode::Freehand;
        stroke.seed = uint32_t(e->timestamp());
        const ViewInfo v = view;
        mStroke.begin(stroke, mControls.input(e), [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return surfaceQuery(QPointF(px.x, px.y), v, out); },
                      *this);
    }

    /** The shared stroke's view of the surface under a pixel, in the proxy's object space. */
    bool surfaceQuery(QPointF px, const ViewInfo& view, brush::SurfaceHit& out) const
    {
        if (!mSession || !scene::Graph::current().node(mSession->node))
            return false;
        Hit hit;
        if (!surfaceHit(*mSession, px, view, hit))
            return false;
        const glm::dmat4 w = worldOf(*mSession);
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        out.center = hit.local;
        out.normal = hit.normal;
        out.viewDir = hit.viewDir;
        out.right = brush::ui::screenRight(view, w);
        out.pixelSize = view.unitsPerPixel(hit.world) / scale;
        out.worldToLocal = 1.0 / scale;
        return true;
    }

    void dab(const brush::Dab& d) override
    {
        if (!mSession)
            return;
        if (mDabs.empty())
        {
            mGrab = d;
            mGrabLocal = d.center;
        }
        mLastLocal = d.center;
        send(d);
    }

    void send(const brush::Dab& d)
    {
        mSession->sculpt.dab(d);
        const glm::dmat4 w = worldOf(*mSession);
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        const glm::dmat3 nrm = glm::transpose(glm::inverse(glm::dmat3(w)));
        mDabs.push_back({{"center", vecJson(xformPoint(w, d.center))},
                         {"normal", vecJson(glm::normalize(nrm * d.normal))},
                         {"radius", d.radius * scale},
                         {"pressure", d.pressure},
                         {"opacity", d.opacity},
                         {"flow", d.flow},
                         {"view", vecJson(xformDir(w, d.viewDir))},
                         {"delta", vecJson(xformDir(w, d.delta))},
                         {"direction", vecJson(xformDir(w, d.direction))}});
        viewportTools::requestRedraw();
    }

    void strokeTo(const QMouseEvent* e, const ViewInfo& view)
    {
        LILA_ZONE();

        auto s = sculpt::active();
        if (!s || s != mSession || !scene::Graph::current().node(s->node))
            return;
        if (mSettings.brush == sculpt::Brush::Grab)
        {
            if (mDabs.empty())
                return;
            // The grabbed point follows the cursor across the plane facing the view at its starting depth.
            glm::dvec3 o, dir;
            view.ray(e->position(), o, dir);
            glm::dmat4 w = worldOf(*s), inv = glm::inverse(w);
            glm::dvec3 world = xformPoint(w, mGrabLocal);
            glm::dvec3 axis = glm::normalize(world - view.eye);
            double denom = glm::dot(glm::normalize(dir), axis);
            if (std::abs(denom) < 1e-9)
                return;
            glm::dvec3 target = o + glm::normalize(dir) * (glm::dot(world - o, axis) / denom);
            glm::dvec3 local = xformPoint(inv, target);
            brush::Dab d = mGrab;
            d.center = local;
            d.normal = glm::dvec3(0.0, 1.0, 0.0);
            d.viewDir = glm::normalize(xformDir(inv, axis));
            d.delta = local - mGrabLocal;
            d.direction = local - mLastLocal;
            mLastLocal = local;
            send(d);
            mGrabLocal = local;
            return;
        }
        mStroke.move(mControls.input(e));
    }

    void endStroke()
    {
        mStroke.end();
        mStroking = false;
        auto s = sculpt::active();
        if (!s || s != mSession)
            return;
        sculpt::StrokeRecord rec = s->sculpt.endStroke();
        mSession.reset();
        if (rec.empty())
            return;
        uint64_t token = sculpt::nextLiveToken();
        s->liveToken = token;
        s->live = std::move(rec);
        json call = {{"brush", std::string(sculpt::brushName(mSettings.brush))},
                     {"falloff", mFalloffName},
                     {"strength", mSettings.strength},
                     {"invert", mSettings.invert},
                     {"frontFacesOnly", mSettings.frontFacesOnly},
                     {"radialCount", mSettings.radial.count},
                     {"radialAxis", std::string(1, "xyz"[std::clamp(mSettings.radial.axis, 0, 2)])},
                     {"dabs", std::move(mDabs)},
                     {"live", int64_t(token)}};
        mDabs = json::array();
        try
        {
            // Not echoed: a stroke is hundreds of dabs, and repeating it from the script editor is rarely wanted.
            ops::execute("sculptStroke", call, CallSource::Script);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }

    // The controls' changed callback saves the prefs on every step, as the panel needs them current.
    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return sculptSettings().stroke; }, [] { sculptSettingsChanged(); }, {}};
    int mListener = 0;

    std::shared_ptr<sculpt::Session> mSession;
    sculpt::BrushSettings mSettings;
    std::string mFalloffName;
    brush::Stroke mStroke;
    bool mStroking = false;
    json mDabs = json::array();
    QPointF mHover;
    glm::dvec3 mLastLocal{0.0}, mGrabLocal{0.0};
    brush::Dab mGrab;
};

/** Viewport content for the open session: the proxy instead of its mesh, the cage wire and the mask shading. */
class SculptDrawer
{
public:
    void draw(render::FrameState& s, OverlayPainter&, const ViewInfo&)
    {
        LILA_ZONE();

        auto session = sculpt::active();
        scene::Graph& g = scene::Graph::current();
        scene::Node* n = session ? g.node(session->node) : nullptr;
        if (!n || !scene::effectivelyVisible(g, session->node))
            return;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);

        // The mesh itself is hidden while its proxy stands in for it. Its draw is found by its cached render copy.
        int smooth = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
        auto shown = renderMeshFor(smoothedFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh), smooth));
        std::erase_if(s.meshes, [&](const render::MeshDraw& d) { return shown && d.mesh == shown; });

        render::MeshDraw d;
        d.mesh = session->sculpt.renderMesh();
        d.world = glm::mat4(world);
        d.baseColor = glm::vec3(0.42f);
        d.roughness = 0.6f;
        d.flags = render::DrawShaded;
        s.meshes.push_back(std::move(d));

        if (sculptSettings().showCage)
            addCage(s, *session, *n, world);
        addMask(s, *session, world);
    }

private:
    /** The cage as Exit Sculpt will leave it, refitted as the sculpt changes. */
    void addCage(render::FrameState& s, sculpt::Session& session, const scene::Node& node, const glm::dmat4& world)
    {
        LILA_ZONE();

        const std::string& method = sculptSettings().exitMethod;
        const int64_t symmetry = node.as<int64_t>(scene::MeshNode::kSymmetry);
        const bool stale = &session != mCageFor || session.sculpt.version() != mCageVersion || method != mCageMethod ||
                           symmetry != mCageSymmetry || world != mCageWorld;
        if (stale && mCageClock.isValid() && mCageClock.elapsed() < mCageCost * 3)
        {
            // A slow fit (a remeshed proxy) refits a few times a second at most; the frame after catches up.
            if (!mCageQueued)
            {
                mCageQueued = true;
                QTimer::singleShot(int(mCageCost * 3 - mCageClock.elapsed()) + 1, [this] {
                    mCageQueued = false;
                    viewportTools::requestRedraw();
                });
            }
        }
        else if (stale)
        {
            mCageFor = &session;
            mCageVersion = session.sculpt.version();
            mCageMethod = method;
            mCageSymmetry = symmetry;
            mCageWorld = world;
            mCage.clear();
            mCageClock.start();
            std::vector<glm::dvec3> pos;
            try
            {
                pos = sculpt::exitPositions(session, method);
            }
            catch (const std::exception& e)
            {
                log::warn("Sculpt cage preview: {}", e.what());
            }
            mCageCost = mCageClock.elapsed();
            const geo::Mesh& c = session.cage;
            const glm::vec4 color(0.78f, 0.78f, 0.78f, 0.7f);
            if (pos.size() >= c.pointCapacity())
                for (geo::Index e = 0; e < c.edgeCapacity(); ++e)
                    if (c.edgeValid(e))
                        for (int k = 0; k < 2; ++k)
                            mCage.push_back({glm::vec3(xformPoint(world, pos[c.edgePoint(e, k)])), color});
        }
        s.lines.insert(s.lines.end(), mCage.begin(), mCage.end());
    }

    void addMask(render::FrameState& s, const sculpt::Session& session, const glm::dmat4& world)
    {
        LILA_ZONE();

        const sculpt::SculptMesh& sm = session.sculpt;
        if (&session != mMaskFor || sm.maskVersion() != mMaskVersion)
        {
            mMaskFor = &session;
            mMaskVersion = sm.maskVersion();
            mMaskVertsVersion = 0;
            mMaskTris.clear();
            const auto& corners = sm.bvh().triangles().corners;
            const geo::Mesh& m = sm.mesh();
            for (size_t t = 0; t * 3 < corners.size(); ++t)
                for (int k = 0; k < 3; ++k)
                {
                    geo::Index p = m.cornerPoint(corners[t * 3 + k]);
                    if (sm.mask()[p] > 0.0f || sm.frozen()[p])
                    {
                        mMaskTris.push_back(geo::Index(t));
                        break;
                    }
                }
        }
        // Past this many the overlay costs more than it helps; the panel still reports the mask.
        if (mMaskTris.empty() || mMaskTris.size() > 250000)
            return;
        if (sm.version() != mMaskVertsVersion || world != mMaskWorld)
        {
            mMaskVertsVersion = sm.version();
            mMaskWorld = world;
            mMaskVerts.resize(mMaskTris.size() * 3);
            const auto& corners = sm.bvh().triangles().corners;
            const geo::Mesh& m = sm.mesh();
            tbb::parallel_for(size_t(0), mMaskTris.size(), [&](size_t i) {
                for (int k = 0; k < 3; ++k)
                {
                    geo::Index p = m.cornerPoint(corners[size_t(mMaskTris[i]) * 3 + k]);
                    // Masked reads dark, frozen reads light; both stay grey.
                    glm::vec4 c = sm.frozen()[p] ? glm::vec4(0.92f, 0.92f, 0.92f, 0.45f) : glm::vec4(0.02f, 0.02f, 0.02f, 0.7f * sm.mask()[p]);
                    mMaskVerts[i * 3 + k] = {glm::vec3(xformPoint(world, m.position(p))), c};
                }
            });
        }
        s.tris3d.insert(s.tris3d.end(), mMaskVerts.begin(), mMaskVerts.end());
    }

    const sculpt::Session* mCageFor = nullptr;
    uint64_t mCageVersion = 0;
    std::string mCageMethod;
    int64_t mCageSymmetry = 0;
    glm::dmat4 mCageWorld{0.0};
    std::vector<render::LineVertex> mCage;
    QElapsedTimer mCageClock;
    int64_t mCageCost = 0;
    bool mCageQueued = false;

    const sculpt::Session* mMaskFor = nullptr;
    uint64_t mMaskVersion = 0;
    uint64_t mMaskVertsVersion = 0;
    glm::dmat4 mMaskWorld{0.0};
    std::vector<geo::Index> mMaskTris;
    std::vector<render::LineVertex> mMaskVerts;
};
} // namespace

SculptSettings& sculptSettings()
{
    static SculptSettings s = loadSettings();
    return s;
}

void sculptSettingsChanged()
{
    const SculptSettings& s = sculptSettings();
    PrefStore& p = PrefStore::get();
    p.set(kPrefs, "/sculpt/brush", std::string(sculpt::brushName(s.brush)));
    s.stroke.savePrefs("/sculpt");
    p.set(kPrefs, "/sculpt/frontFacesOnly", s.frontFacesOnly);
    p.set(kPrefs, "/sculpt/showCage", s.showCage);
    p.set(kPrefs, "/sculpt/proxyMode", s.proxyMode);
    p.set(kPrefs, "/sculpt/levels", s.levels);
    p.set(kPrefs, "/sculpt/resolution", s.resolution);
    p.set(kPrefs, "/sculpt/exitMethod", s.exitMethod);
    p.set(kPrefs, "/sculpt/keepProxy", s.keepProxy);
    auto copy = settingsListeners();
    for (auto& [id, fn] : copy)
        fn();
    viewportTools::requestRedraw();
}

int addSculptSettingsListener(std::function<void()> fn)
{
    static int next = 1;
    settingsListeners()[next] = std::move(fn);
    return next++;
}

void removeSculptSettingsListener(int id) { settingsListeners().erase(id); }

void startSculptTool()
{
    if (!sculptToolActive())
        viewportTools::set(std::make_unique<SculptTool>());
}

bool sculptToolActive()
{
    ViewportTool* t = viewportTools::active();
    return t && std::string_view(t->name()) == "sculpt";
}

void installSculptDrawer()
{
    static SculptDrawer drawer;
    viewportHooks::addDrawer([](render::FrameState& s, OverlayPainter& p, const ViewInfo& v) { drawer.draw(s, p, v); });
}
} // namespace lila::ui
