#include "ui/SculptTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

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
#include <QWidget>
#include <QTimer>

#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
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
    if (auto f = sculpt::falloffFromName(p.value<std::string>(kPrefs, "/sculpt/falloff", "smooth")))
        s.falloff = *f;
    s.radius = std::clamp(p.value<double>(kPrefs, "/sculpt/radius", s.radius), 2.0, 2000.0);
    s.strength = std::clamp(p.value<double>(kPrefs, "/sculpt/strength", s.strength), 0.0, 1.0);
    s.spacing = std::clamp(p.value<double>(kPrefs, "/sculpt/spacing", s.spacing), 0.02, 2.0);
    s.frontFacesOnly = p.value<bool>(kPrefs, "/sculpt/frontFacesOnly", s.frontFacesOnly);
    s.showCage = p.value<bool>(kPrefs, "/sculpt/showCage", s.showCage);
    s.proxyMode = p.value<std::string>(kPrefs, "/sculpt/proxyMode", s.proxyMode);
    s.levels = std::clamp(p.value<int>(kPrefs, "/sculpt/levels", s.levels), 1, 6);
    s.resolution = std::clamp(p.value<int>(kPrefs, "/sculpt/resolution", s.resolution), 16, 2048);
    s.exitMethod = p.value<std::string>(kPrefs, "/sculpt/exitMethod", s.exitMethod);
    s.keepProxy = p.value<bool>(kPrefs, "/sculpt/keepProxy", s.keepProxy);
    return s;
}

bool isViewport(QWidget* w) { return w && w->inherits("lila::ui::ViewportWidget"); }

glm::dvec3 xformPoint(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }
glm::dvec3 xformDir(const glm::dmat4& m, const glm::dvec3& d) { return glm::dvec3(m * glm::dvec4(d, 0.0)); }

json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

class SculptTool;

/** Sees F, B and M before the hotkeys do while a viewport has focus, so they can be held to resize the brush. */
class KeyFilter : public QObject
{
public:
    explicit KeyFilter(SculptTool* tool) : mTool(tool) {}
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    SculptTool* mTool;
};

class SculptTool final : public ViewportTool
{
public:
    enum class Adjust
    {
        None,
        Radius,
        Strength
    };

    const char* name() const override { return "sculpt"; }

    void activate() override
    {
        mFilter = std::make_unique<KeyFilter>(this);
        qApp->installEventFilter(mFilter.get());
        mListener = sculpt::addListener([] { viewportTools::requestRedraw(); });
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        if (mFilter)
            qApp->removeEventFilter(mFilter.get());
        mFilter.reset();
        sculpt::removeListener(mListener);
        viewportTools::requestRedraw();
    }

    /** Held-key state from the key filter. Returns true when the key is ours. */
    bool key(int k, Qt::KeyboardModifiers mods, bool down)
    {
        Adjust kind = Adjust::None;
        if (k == Qt::Key_B || (k == Qt::Key_F && !(mods & Qt::ShiftModifier)))
            kind = Adjust::Radius;
        else if (k == Qt::Key_M || (k == Qt::Key_F && (mods & Qt::ShiftModifier)))
            kind = Adjust::Strength;
        if (kind == Adjust::None)
            return false;
        if (down)
        {
            mArmed = kind;
            mArmedKey = k;
            mArmedUsed = false;
            return true;
        }
        if (k != mArmedKey)
            return true;
        // A tapped F still frames the selection, as everywhere else.
        if (k == Qt::Key_F && !mArmedUsed && mAdjusting == Adjust::None)
            QTimer::singleShot(0, [] { actions::run("viewport.frameSelected"); });
        mArmed = Adjust::None;
        mArmedKey = 0;
        return true;
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (tools::brushRadiusDrag(e, mArmed != Adjust::None))
        {
            mAdjusting = mArmed != Adjust::None ? mArmed : Adjust::Radius;
            mArmedUsed = true;
            mAdjustStart = e->position();
            mAdjustFrom = e->button() == Qt::MiddleButton ? 0.0 : mAdjusting == Adjust::Radius ? sculptSettings().radius : sculptSettings().strength;
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        auto s = sculpt::active();
        if (!s || !scene::Graph::current().node(s->node))
            return true;
        Hit hit;
        if (!surfaceHit(*s, e->position(), view, hit))
            return false;
        beginStroke(*s, e->modifiers(), hit, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mAdjusting != Adjust::None)
        {
            SculptSettings& st = sculptSettings();
            double dx = e->position().x() - mAdjustStart.x();
            if (mAdjusting == Adjust::Radius)
                st.radius = std::clamp(mAdjustFrom + dx, 2.0, 2000.0);
            else
                st.strength = std::clamp(mAdjustFrom + dx / 300.0, 0.0, 1.0);
            sculptSettingsChanged();
            return true;
        }
        if (mStroking)
        {
            strokeTo(e->position(), view);
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton && !(mAdjusting != Adjust::None && e->button() == Qt::MiddleButton))
            return false;
        if (mAdjusting != Adjust::None)
        {
            mAdjusting = Adjust::None;
            return true;
        }
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            sculptSettings().radius = std::clamp(sculptSettings().radius * (e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25), 2.0, 2000.0);
            sculptSettingsChanged();
            return true;
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
        QString brush = QString::fromUtf8(sculpt::brushName(st.brush).data(), qsizetype(sculpt::brushName(st.brush).size()));
        drawToolMessage(p, view, QString("Sculpt %1: %2   radius %3 px   strength %4%5")
                              .arg(QString::fromStdString(n->path()), brush)
                              .arg(int(std::lround(st.radius)))
                              .arg(st.strength, 0, 'f', 2)
                              .arg(s->sculpt.symmetric() ? "   symmetry" : ""));

        // While adjusting, the circle stays where the drag began so its size reads against the surface there.
        QPointF at = mAdjusting != Adjust::None ? mAdjustStart : mHover;
        const glm::vec4 rim(0.95f, 0.95f, 0.95f, 1.0f), dim(0.6f, 0.6f, 0.6f, 0.8f);
        Hit hit;
        if (surfaceHit(*s, at, view, hit))
        {
            double r = st.radius * view.unitsPerPixel(hit.world);
            glm::dvec3 nrm = hit.worldNormal;
            glm::dvec3 ref = std::abs(nrm.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            glm::dvec3 u = glm::normalize(glm::cross(nrm, ref)), v = glm::cross(nrm, u);
            std::vector<QPointF> pts;
            for (int k = 0; k < 64; ++k)
            {
                double a = 2.0 * kPi * k / 64;
                if (auto q = view.project(hit.world + (u * std::cos(a) + v * std::sin(a)) * r))
                    pts.push_back(*q);
            }
            p.polyline(pts, 1.5f, rim, true);
            auto c = view.project(hit.world), tip = view.project(hit.world + nrm * r * 0.35);
            if (c && tip)
                p.line(*c, *tip, 1.5f, rim);
            if (mAdjusting == Adjust::Strength)
            {
                // Inner circle at the strength fraction of the radius.
                std::vector<QPointF> inner;
                for (int k = 0; k < 48; ++k)
                {
                    double a = 2.0 * kPi * k / 48;
                    if (auto q = view.project(hit.world + (u * std::cos(a) + v * std::sin(a)) * (r * st.strength)))
                        inner.push_back(*q);
                }
                p.polyline(inner, 1.0f, dim, true);
            }
        }
        else
        {
            std::vector<QPointF> pts;
            for (int k = 0; k < 64; ++k)
            {
                double a = 2.0 * kPi * k / 64;
                pts.push_back(at + QPointF(std::cos(a), std::sin(a)) * st.radius);
            }
            p.polyline(pts, 1.0f, dim, true);
        }
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

    double localRadius(const sculpt::Session& s, const glm::dvec3& world, const ViewInfo& view) const
    {
        glm::dmat4 w = worldOf(s);
        double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        return sculptSettings().radius * view.unitsPerPixel(world) / scale;
    }

    void beginStroke(sculpt::Session& s, Qt::KeyboardModifiers mods, const Hit& hit, const ViewInfo& view)
    {
        const SculptSettings& st = sculptSettings();
        mSession = sculpt::active();
        sculpt::syncSymmetry(s);
        mSettings.brush = (mods & Qt::ShiftModifier) ? sculpt::Brush::Smooth : st.brush;
        mSettings.falloff = st.falloff;
        mSettings.strength = st.strength;
        mSettings.invert = (mods & Qt::ControlModifier) != 0;
        mSettings.frontFacesOnly = st.frontFacesOnly;
        s.sculpt.beginStroke(mSettings);
        mDabs = json::array();
        mStroking = true;
        mLast = mHover;
        mLastLocal = hit.local;
        mGrabLocal = hit.local;
        mRadius = localRadius(s, hit.world, view);
        dab(s, hit, glm::dvec3(0.0));
    }

    void dab(sculpt::Session& s, const Hit& hit, const glm::dvec3& localDelta)
    {
        sculpt::Dab d;
        d.center = hit.local;
        d.normal = hit.normal;
        d.radius = mRadius;
        d.viewDir = hit.viewDir;
        d.delta = localDelta;
        d.direction = hit.local - mLastLocal;
        mLastLocal = hit.local;
        s.sculpt.dab(d);

        const glm::dmat4 w = worldOf(s);
        const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(w)))));
        const glm::dmat3 nrm = glm::transpose(glm::inverse(glm::dmat3(w)));
        mDabs.push_back({{"center", vecJson(xformPoint(w, d.center))},
                         {"normal", vecJson(glm::normalize(nrm * d.normal))},
                         {"radius", d.radius * scale},
                         {"view", vecJson(xformDir(w, d.viewDir))},
                         {"delta", vecJson(xformDir(w, d.delta))},
                         {"direction", vecJson(xformDir(w, d.direction))}});
        viewportTools::requestRedraw();
    }

    void strokeTo(QPointF px, const ViewInfo& view)
    {
        LILA_ZONE();

        auto s = sculpt::active();
        if (!s || s != mSession || !scene::Graph::current().node(s->node))
            return;
        if (mSettings.brush == sculpt::Brush::Grab)
        {
            // The grabbed point follows the cursor across the plane facing the view at its starting depth.
            glm::dvec3 o, dir;
            view.ray(px, o, dir);
            glm::dmat4 w = worldOf(*s), inv = glm::inverse(w);
            glm::dvec3 world = xformPoint(w, mGrabLocal);
            glm::dvec3 axis = glm::normalize(world - view.eye);
            double denom = glm::dot(glm::normalize(dir), axis);
            if (std::abs(denom) < 1e-9)
                return;
            glm::dvec3 target = o + glm::normalize(dir) * (glm::dot(world - o, axis) / denom);
            glm::dvec3 local = xformPoint(inv, target);
            Hit h;
            h.local = local;
            h.normal = glm::dvec3(0.0, 1.0, 0.0);
            h.viewDir = glm::normalize(xformDir(inv, axis));
            dab(*s, h, local - mGrabLocal);
            mGrabLocal = local;
            return;
        }
        QPointF d = px - mLast;
        double len = std::hypot(d.x(), d.y()), step = std::max(1.0, sculptSettings().radius * sculptSettings().spacing);
        int count = int(len / step);
        for (int k = 1; k <= count; ++k)
        {
            Hit hit;
            if (surfaceHit(*s, mLast + d * (k * step / len), view, hit))
                dab(*s, hit, glm::dvec3(0.0));
        }
        if (count > 0)
            mLast += d * (count * step / len);
    }

    void endStroke()
    {
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
                     {"falloff", std::string(sculpt::falloffName(mSettings.falloff))},
                     {"strength", mSettings.strength},
                     {"invert", mSettings.invert},
                     {"frontFacesOnly", mSettings.frontFacesOnly},
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

    std::unique_ptr<KeyFilter> mFilter;
    int mListener = 0;

    Adjust mArmed = Adjust::None;
    int mArmedKey = 0;
    bool mArmedUsed = false;
    Adjust mAdjusting = Adjust::None;
    QPointF mAdjustStart;
    double mAdjustFrom = 0.0;

    std::shared_ptr<sculpt::Session> mSession;
    sculpt::BrushSettings mSettings;
    bool mStroking = false;
    json mDabs = json::array();
    QPointF mLast, mHover;
    glm::dvec3 mLastLocal{0.0}, mGrabLocal{0.0};
    double mRadius = 1.0;
};

bool KeyFilter::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease && e->type() != QEvent::ShortcutOverride)
        return false;
    QWidget* focus = QApplication::focusWidget();
    if (obj != focus || !isViewport(focus))
        return false;
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() != Qt::Key_F && k->key() != Qt::Key_B && k->key() != Qt::Key_M)
        return false;
    if (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
        return false;
    if (e->type() == QEvent::ShortcutOverride)
    {
        e->accept();
        return true;
    }
    if (k->isAutoRepeat())
        return true;
    return mTool->key(k->key(), k->modifiers(), e->type() == QEvent::KeyPress);
}

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
            addCage(s, *session, world);
        addMask(s, *session, world);
    }

private:
    void addCage(render::FrameState& s, const sculpt::Session& session, const glm::dmat4& world)
    {
        if (&session != mCageFor || world != mCageWorld)
        {
            mCageFor = &session;
            mCageWorld = world;
            mCage.clear();
            const geo::Mesh& c = session.cage;
            const glm::vec4 color(0.78f, 0.78f, 0.78f, 0.7f);
            for (geo::Index e = 0; e < c.edgeCapacity(); ++e)
                if (c.edgeValid(e))
                    for (int k = 0; k < 2; ++k)
                        mCage.push_back({glm::vec3(xformPoint(world, c.position(c.edgePoint(e, k)))), color});
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
    glm::dmat4 mCageWorld{0.0};
    std::vector<render::LineVertex> mCage;

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
    p.set(kPrefs, "/sculpt/falloff", std::string(sculpt::falloffName(s.falloff)));
    p.set(kPrefs, "/sculpt/radius", s.radius);
    p.set(kPrefs, "/sculpt/strength", s.strength);
    p.set(kPrefs, "/sculpt/spacing", s.spacing);
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
