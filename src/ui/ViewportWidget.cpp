#include "ui/ViewportWidget.h"

#include "bake/Arrange.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Lasso.h"
#include "geo/Mesh.h"
#include "geo/RenderMesh.h"
#include "io/SceneFile.h"
#include "render/Backdrop.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/ComponentPick.h"
#include "scene/Components.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "gn/InstanceView.h"
#include "scene/ArrayGroup.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/NodeBounds.h"
#include "scene/RenderSettings.h"
#include "scene/Skin.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/ChannelBox.h"
#include "ui/CursorBadges.h"
#include "ui/Deletion.h"
#include "ui/NodeLookup.h"
#include "ui/DrawOverrides.h"
#include "ui/MarkingMenu.h"
#include "ui/Environments.h"
#include "ui/MaterialDraw.h"
#include "ui/Isolate.h"
#include "ui/LodDisplay.h"
#include "ui/DisplayAids.h"
#include "ui/MeshCache.h"
#include "ui/PaintSelect.h"
#include "ui/Outliner.h"
#include "ui/Overlay.h"
#include "ui/RenderPasses.h"
#include "ui/SelectionHandlers.h"
#include "ui/SurfaceSnap.h"
#include "ui/TabSelectLogic.h"
#include "ui/Theme.h"
#include "ui/Tools.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportTool.h"
#include "uv/UvCommands.h"

#include <QColor>
#include <QCursor>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

// Qt's emit macro collides with a TBB member of the same name.
#pragma push_macro("emit")
#undef emit
#include <tbb/parallel_for.h>
#pragma pop_macro("emit")

#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace lila::ui
{
namespace
{
// Grayscale only (see plan: nothing colored near the work).
const glm::vec4 kLocatorColor(0.02f, 0.02f, 0.02f, 1.0f);
const glm::vec4 kSelectedColor(0.95f, 0.95f, 0.95f, 1.0f);
const glm::vec4 kChildOfSelectedColor(0.55f, 0.55f, 0.55f, 1.0f);
const glm::vec4 kPreselectColor(0.75f, 0.75f, 0.75f, 1.0f);
const glm::vec4 kTemplateWireColor(0.22f, 0.22f, 0.22f, 1.0f);
const glm::vec4 kSnapSurfaceWireColor(0.10f, 0.35f, 0.22f, 1.0f);

/** True when `hot`, the node under the cursor, is an array above `id`: hovering a copy highlights the whole array. */
bool preselectedArray(const scene::Graph& g, scene::NodeId id, scene::NodeId hot)
{
    for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
        if (p == hot)
            return scene::arrays::isArray(*g.node(p));
    return false;
}

/** Where the viewport draws and picks a node: its world matrix plus any view-only display offset (bake Arrange). */
glm::dmat4 shownWorld(const scene::Node& n)
{
    return bake::arrange::displayWorld(scene::Graph::current(), n.id(), n.as<glm::dmat4>(scene::Transform::kWorldMatrix));
}

/**
 * A geometry nodes result that ends in instances: d (the node's draw, without a mesh) once per referenced mesh with that
 * mesh's placements, plus the geometry beside the instances.
 */
void pushInstanceDraws(render::FrameState& s, const render::MeshDraw& d, const gn::InstanceView& view, bool textured)
{
    auto push = [&](const scene::MeshPtr& mesh, std::shared_ptr<const std::vector<glm::mat4>> placements) {
        const bool tangents = textured && needsTangents(*mesh);
        auto rm = renderMeshFor(mesh, tangents);
        if (!rm)
            return;
        render::MeshDraw part = d;
        part.mesh = std::move(rm);
        // Array copies of the node repeat every instance placement.
        if (d.instances && placements)
        {
            auto both = std::make_shared<std::vector<glm::mat4>>();
            both->reserve(d.instances->size() * placements->size());
            for (const glm::mat4& copy : *d.instances)
                for (const glm::mat4& p : *placements)
                    both->push_back(copy * p);
            part.instances = std::move(both);
        }
        else
            part.instances = placements ? std::move(placements) : d.instances;
        if (textured)
            part.materials = drawMaterials(*mesh);
        s.meshes.push_back(std::move(part));
    };
    if (view.base)
        push(view.base, nullptr);
    for (const gn::InstancePart& p : view.parts)
        push(p.mesh, p.transforms);
}

/**
 * Where a node's array copies draw relative to its world matrix, the node itself first, for an instanced draw. Null when
 * no array repeats it.
 */
std::shared_ptr<const std::vector<glm::mat4>> arrayPlacements(const scene::Graph& g, scene::NodeId id, const glm::dmat4& world)
{
    const std::vector<scene::arrays::Copy> copies = scene::arrays::copiesOf(g, id);
    if (copies.empty())
        return nullptr;
    auto out = std::make_shared<std::vector<glm::mat4>>();
    out->reserve(copies.size() + 1);
    out->push_back(glm::mat4(1.0f));
    const glm::dmat4 inv = glm::inverse(world);
    for (const scene::arrays::Copy& c : copies)
        out->push_back(glm::mat4(inv * c.transform * world));
    return out;
}

/** An unhighlighted object's line color as its display layer has it: the layer's tint, template grey, else fallback. */
glm::vec4 layerLineColor(const scene::Graph& g, scene::NodeId id, const glm::vec4& fallback)
{
    const scene::displayLayers::Display layer = scene::displayLayers::display(g, id);
    if (layer.color)
        return glm::vec4(glm::vec3(*layer.color), 1.0f);
    return layer.type == scene::displayLayers::DisplayType::Template ? kTemplateWireColor : fallback;
}

/** Linear colors for component display: Maya's hues, or greys when monotone. */
struct ComponentPalette
{
    glm::vec4 wire;     // the hilited meshes' wire; alpha 0 keeps the renderer's default
    glm::vec4 point;    // unselected vertices
    glm::vec4 selected; // selected vertices, edges and face outlines
    glm::vec4 selectedFill;
    glm::vec4 hover;
    glm::vec4 hoverFill;
};

ComponentPalette componentPalette()
{
    if (!viewportDisplay::accentColors(viewportDisplay::kComponentColors))
        return {glm::vec4(0.0f), glm::vec4(0.007f, 0.007f, 0.007f, 1.0f), glm::vec4(1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.4f),
                glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.12f)};
    auto lin = [](float r, float g, float b, float a) {
        auto c = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        return glm::vec4(c(r), c(g), c(b), a);
    };
    auto tok = [&](const char* token, float r, float g, float b, float a) {
        glm::vec4 c = themeSrgb(token, glm::vec4(r, g, b, a));
        return lin(c.r, c.g, c.b, a);
    };
    return {tok("componentWire", 0.45f, 0.78f, 1.0f, 1.0f),      tok("componentPoint", 0.95f, 0.42f, 0.78f, 1.0f),
            tok("componentSelected", 1.0f, 0.95f, 0.15f, 1.0f),  tok("componentSelected", 1.0f, 0.95f, 0.15f, 0.25f),
            tok("componentHover", 1.0f, 0.18f, 0.18f, 1.0f),     tok("componentHover", 1.0f, 0.18f, 0.18f, 0.25f)};
}

std::vector<scene::NodeId> hilited(const scene::isolation::Set& iso);

/** In the viewport's pick: shown, and not templated or referenced by a display layer. */
bool viewportPickable(const scene::Graph& g, scene::NodeId id, bool inherit)
{
    return scene::effectivelyVisible(g, id, inherit) && scene::displayLayers::pickable(g, id);
}

constexpr const char* kBuiltInViews[] = {"persp", "top", "bottom", "front", "back", "left", "right"};

bool builtInView(const std::string& name)
{
    return std::ranges::any_of(kBuiltInViews, [&](const char* v) { return name == v; });
}

double nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** The attrs a navigation edit changes on a camera node, saved to put it back before the committing command. */
constexpr int kEditedAttrs[] = {scene::Transform::kTranslate, scene::Transform::kRotate, scene::Transform::kScale,
                                scene::Camera::kOrthographicWidth};

/** Drawn and picked by a draw override rather than as the viewport's own mesh. */
bool overridden(const scene::Node& n) { return drawOverrides::forType(n.type()) != nullptr; }

glm::vec4 srgbToLinear(const glm::vec4& c)
{
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {lin(c.r), lin(c.g), lin(c.b), c.a};
}

const glm::vec4 kSymmetryRed = srgbToLinear({0.95f, 0.18f, 0.16f, 1.0f});

/** What a mesh's symmetry setting is, as a cache key: its layer and where its plane or axis lies. */
std::string symmetryKey(const geo::SymmetrySpec& spec)
{
    if (!spec.radial)
        return std::format("{}:{}", geo::symmetryLayerName(spec.axis), spec.offset);
    const glm::dvec3 o = spec.radial->origin();
    return std::format("{}:{},{},{}", geo::radialLayerName(*spec.radial), o.x, o.y, o.z);
}

/** The edges of a mesh with symmetry on that lie in its plane (radial: on its axis or mirror planes), cached per mesh and setting. */
const std::vector<uint32_t>& symmetryPlaneEdges(const scene::Node& n, const scene::MeshPtr& mesh)
{
    struct Cached
    {
        std::weak_ptr<const geo::Mesh> mesh;
        std::string key;
        std::vector<uint32_t> edges;
    };
    static std::unordered_map<scene::NodeId, Cached> cache;
    static const std::vector<uint32_t> none;
    const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
    if (!spec || !mesh)
        return none;
    const std::string key = symmetryKey(*spec);
    Cached& c = cache[n.id()];
    if (c.mesh.lock() == mesh && c.key == key)
        return c.edges;
    const geo::Mesh& m = *mesh;
    const double tol = std::max(1e-4, glm::length(m.bounds().size()) * 1e-5);
    c = {mesh, key, {}};
    // The stored pairs mark the on-plane points even after they drift; without them, go by position.
    std::span<const int32_t> keys = spec->radial ? std::span<const int32_t>() : m.pointAttrs().get<int32_t>(geo::symmetryLayerName(spec->axis));
    auto on = [&](geo::Index p) {
        if (const std::optional<geo::RadialSymmetry>& r = spec->radial)
        {
            if (r->axisDistance(m.position(p)) <= tol)
                return true;
            for (int k = 0; r->mirror() && k < r->count(); ++k)
                if (std::abs(r->mirrorPlane(r->count() + k).distance(m.position(p))) <= tol)
                    return true;
            return false;
        }
        return keys.empty() ? std::abs(m.position(p)[spec->axis] - spec->offset) <= tol : keys[p] == geo::kSymmetryOnPlane;
    };
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && on(m.edgePoint(e, 0)) && on(m.edgePoint(e, 1)))
            c.edges.push_back(e);
    return c.edges;
}

/** Opacity of the mirror images of hovered and selected components while symmetry is on. */
constexpr float kMirrorAlpha = 0.22f;

/** The mirror map (or radial orbits) of a mesh with symmetry on, null when off. Moving points keeps the pairs, so only topology rebuilds it. */
const geo::SymmetryMap* viewSymmetry(const scene::Node& n, const scene::MeshPtr& mesh)
{
    struct Cached
    {
        geo::Index points = 0, edges = 0, faces = 0;
        std::string key;
        std::optional<geo::SymmetryMap> map;
    };
    static std::unordered_map<scene::NodeId, Cached> cache;
    const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
    if (!spec || !mesh)
        return nullptr;
    const std::string key = symmetryKey(*spec);
    const geo::Mesh& m = *mesh;
    Cached& c = cache[n.id()];
    if (!c.map || c.key != key || c.points != m.pointCapacity() || c.edges != m.edgeCapacity() || c.faces != m.faceCapacity())
    {
        geo::Mesh copy = m;
        c = {m.pointCapacity(), m.edgeCapacity(), m.faceCapacity(), key,
             geo::SymmetryMap::persistent(copy, *spec, std::max(1e-4, glm::length(m.bounds().size()) * 1e-5))};
    }
    return &*c.map;
}

const glm::vec4 kWedgePlaneGray = srgbToLinear({0.62f, 0.62f, 0.62f, 0.55f});

/**
 * A radially symmetric mesh's wedge boundaries (with mirror, its mirror planes) as half-plane outlines from the axis out
 * past the mesh, spanning its height along the axis. Nothing for other meshes.
 */
void radialPlaneLines(const scene::Node& n, const geo::Mesh& m, const glm::dmat4& world, std::vector<render::LineVertex>& out)
{
    const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
    if (!spec || !spec->radial || m.numPoints() == 0)
        return;
    const geo::RadialSymmetry& r = *spec->radial;
    double lo = std::numeric_limits<double>::max(), hi = -lo, reach = 0.0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const double h = glm::dot(m.position(p) - r.origin(), r.axis());
            lo = std::min(lo, h), hi = std::max(hi, h), reach = std::max(reach, r.axisDistance(m.position(p)));
        }
    reach *= 1.15;
    auto wp = [&](const glm::dvec3& p) { return glm::vec3(world * glm::dvec4(p, 1.0)); };
    auto line = [&](const glm::dvec3& a, const glm::dvec3& b) {
        out.push_back({wp(a), kWedgePlaneGray});
        out.push_back({wp(b), kWedgePlaneGray});
    };
    const glm::dvec3 bottom = r.origin() + r.axis() * lo, top = r.origin() + r.axis() * hi;
    line(bottom, top);
    const glm::dvec3 v = glm::cross(r.axis(), r.reference());
    const int planes = r.mirror() ? 2 * r.count() : r.count();
    for (int k = 0; k < planes; ++k)
    {
        const double t = 2.0 * 3.14159265358979323846 * k / planes;
        const glm::dvec3 out2 = (std::cos(t) * r.reference() + std::sin(t) * v) * reach;
        line(bottom, bottom + out2);
        line(top, top + out2);
        line(bottom + out2, top + out2);
    }
}

bool sameHighlight(const std::optional<selectionHandlers::Highlight>& a, const std::optional<selectionHandlers::Highlight>& b)
{
    if (!a || !b)
        return !a && !b;
    return a->mesh == b->mesh && a->domain == b->domain && a->color == b->color && a->weights == b->weights;
}
} // namespace

ViewportWidget::ViewportWidget(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true); // manipulator hover
    setCursor(Qt::ArrowCursor);
    setMinimumSize(64, 64);
    setProperty("hotkeyContext", "viewport");
    deletion::claimScene(this);
    setAcceptDrops(true);
    static int panels = 0;
    mHudPanel = panels++;

    mCamera.zUp = PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";

    mGraphListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId id) {
        // Synchronous, so a scene load that clears and then restores the view's camera ends on the restored one.
        if (mLookThrough != scene::kNoNode && (c == scene::Change::Cleared || (c == scene::Change::Deleted && id == mLookThrough)))
        {
            mCameraEdit.reset();
            setView(mReturnView);
        }
        if (c == scene::Change::Cleared && mIsolation.active())
            mIsolation.clear();
        // Nodes made while the active view isolates join it, so a curve drawn or primitive placed there stays visible.
        // Undo/redo restoring a node is not making one.
        if (c == scene::Change::Created && mIsolation.active() && scene::isolation::active() == &mIsolation && !UndoStack::get().replaying())
        {
            mIsolation.nodes.insert(id);
            mIsolation.touch();
        }
        requestFrame();
    });
    mWheelCommit = new QTimer(this);
    mWheelCommit->setSingleShot(true);
    mWheelCommit->setInterval(500);
    connect(mWheelCommit, &QTimer::timeout, this, [this] { commitCameraEdit(); });
    mToolListener = tools::addListener([this] { requestFrame(); });
    mSnapSurfaceListener = surfaceSnap::addListener([this] { requestFrame(); });
    mComponentListener = scene::components::addListener([this] { requestFrame(); });
    // Display layers hide on playback: starting or stopping changes what is drawn without changing the time.
    mPlaybackListener = scene::anim::addPlaybackListener([this] { requestFrame(); });
    mViewportToolListener = viewportTools::addListener([this] { requestFrame(); });
    mGizmoListener = gizmos::addListener([this] {
        syncGizmos();
        updateCursor();
        requestFrame();
    });
    mArrangeListener = bake::arrange::addListener([this] { requestFrame(); });
    mInheritListener = scene::isolation::addInheritListener([this](const std::vector<scene::NodeId>& made, const std::vector<scene::NodeId>& from) {
        if (scene::isolation::inheritInto(mIsolation, made, from))
            setIsolation(std::move(mIsolation));
    });
    mHandlerListener = selectionHandlers::addListener([this] {
        mHandlerHover.reset();
        requestFrame();
    });
    // Changing the time only dirties the graph; nothing else asks for a frame.
    onTimeChanged(this, [this] { requestFrame(); });
    mRenderSettingsListener = scene::addRenderSettingsListener([this] {
        syncCameraSettings();
        requestFrame();
    });
    syncCameraSettings();
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file != "general")
            return;
        mCamera.zUp = PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
        // Display modes, toggles and environment settings all live in this file.
        requestFrame();
    });
}

ViewportWidget::~ViewportWidget()
{
    cursorBadges::publish(this, {});
    scene::Graph::current().removeListener(mGraphListener);
    PrefStore::get().removeOnChanged(mPrefListener);
    tools::removeListener(mToolListener);
    surfaceSnap::removeListener(mSnapSurfaceListener);
    scene::components::removeListener(mComponentListener);
    scene::anim::removePlaybackListener(mPlaybackListener);
    viewportTools::removeListener(mViewportToolListener);
    gizmos::removeListener(mGizmoListener);
    selectionHandlers::removeListener(mHandlerListener);
    bake::arrange::removeListener(mArrangeListener);
    scene::removeRenderSettingsListener(mRenderSettingsListener);
    scene::isolation::removeInheritListener(mInheritListener);
    if (mId)
        render::Renderer::get().removeViewport(mId);
}

std::string ViewportWidget::historyKey() const
{
    return mLookThrough != scene::kNoNode ? "#" + std::to_string(mLookThrough) : mViewName;
}

CameraView ViewportWidget::cameraView() const
{
    CameraView v{historyKey(), mCamera.target, mCamera.distance, mCamera.yaw, mCamera.pitch, mCamera.orthoWidth};
    if (const scene::Node* n = scene::Graph::current().node(mLookThrough))
    {
        v.node = true;
        v.translate = n->as<glm::dvec3>(scene::Transform::kTranslate);
        v.rotate = n->as<glm::dvec3>(scene::Transform::kRotate);
        v.scale = n->as<glm::dvec3>(scene::Transform::kScale);
        v.orthoWidth = n->as<double>(scene::Camera::kOrthographicWidth);
    }
    return v;
}

void ViewportWidget::applyCameraView(const CameraView& v)
{
    if (v.view != historyKey())
        return;
    if (v.node)
    {
        scene::Graph& g = scene::Graph::current();
        if (!g.node(mLookThrough))
            return;
        UndoStack& u = UndoStack::get();
        bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({mLookThrough, scene::Transform::kTranslate, -1}, v.translate);
        g.setValue({mLookThrough, scene::Transform::kRotate, -1}, v.rotate);
        g.setValue({mLookThrough, scene::Transform::kScale, -1}, v.scale);
        g.setValue({mLookThrough, scene::Camera::kOrthographicWidth, -1}, v.orthoWidth);
        u.setEnabled(was);
        io::markModified();
        mCamera.distance = v.distance;
    }
    else
    {
        mCamera.target = v.target;
        mCamera.distance = v.distance;
        mCamera.yaw = v.yaw;
        mCamera.pitch = v.pitch;
        mCamera.orthoWidth = v.orthoWidth;
    }
    requestFrame();
    Q_EMIT viewStateChanged();
}

void ViewportWidget::recordCamera(const CameraView& before, uint64_t gesture)
{
    // Only moves are camera undo steps, on the camera that moved; switching cameras is not one.
    if (mCameraRestoring || before.view != historyKey())
        return;
    mCameraHistories[before.view].record(before, cameraView(), gesture, nowMs());
    io::uiChanged("viewports");
}

void ViewportWidget::cameraUndo()
{
    commitCameraEdit();
    if (auto v = mCameraHistories[historyKey()].undo(cameraView()))
    {
        applyCameraView(*v);
        io::uiChanged("viewports");
    }
}

void ViewportWidget::cameraRedo()
{
    commitCameraEdit();
    if (auto v = mCameraHistories[historyKey()].redo(cameraView()))
    {
        applyCameraView(*v);
        io::uiChanged("viewports");
    }
}

QSize ViewportWidget::pixelSize() const
{
    qreal dpr = devicePixelRatioF();
    return {std::max(1, int(width() * dpr)), std::max(1, int(height() * dpr))};
}

double ViewportWidget::aspect() const
{
    QSize s = pixelSize();
    return double(s.width()) / double(s.height());
}

void ViewportWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (!mId && render::Renderer::get().running())
    {
        QSize s = pixelSize();
        mId = render::Renderer::get().addViewport(uintptr_t(winId()), s.width(), s.height());
    }
    requestFrame();
}

void ViewportWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (mId)
    {
        QSize s = pixelSize();
        render::Renderer::get().resize(mId, s.width(), s.height());
    }
    submit();
    // The native window can settle after the event (dock layout restore); a second frame picks up its final size.
    requestFrame();
}

void ViewportWidget::paintEvent(QPaintEvent*)
{
    requestFrame();
}

bool ViewportWidget::event(QEvent* e)
{
    // Native child windows don't get a paint after un-minimize on every platform.
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::UpdateRequest)
        requestFrame();
    if (e->type() == QEvent::Leave)
        clearPreselect();
    if (e->type() == QEvent::FocusOut)
    {
        mTabHeld = false;
        mTabUnder.reset();
    }
    // Tab is a held modifier for drag selection here, not focus navigation. Viewport tools (Retopo Draw) keep their own Tab.
    if (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease)
        if (auto* k = static_cast<QKeyEvent*>(e); k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab)
        {
            if (e->type() == QEvent::KeyRelease && !k->isAutoRepeat())
            {
                mTabHeld = false;
                mTabUnder.reset();
                requestFrame();
            }
            if (!viewportTools::active())
            {
                if (e->type() == QEvent::KeyPress && !k->isAutoRepeat())
                {
                    mTabHeld = true;
                    updateTabHover(mapFromGlobal(QCursor::pos().toPointF()));
                }
                e->accept();
                return true;
            }
        }
    return QWidget::event(e);
}

void ViewportWidget::leaveEvent(QEvent* e)
{
    // Drops the cursor badges from the view the cursor left.
    requestFrame();
    QWidget::leaveEvent(e);
}

void ViewportWidget::requestFrame()
{
    if (mFrameQueued)
        return;
    mFrameQueued = true;
    QTimer::singleShot(0, this, [this] {
        mFrameQueued = false;
        submit();
    });
}

void ViewportWidget::updateCursor()
{
    if (ViewportTool* tool = viewportTools::active())
    {
        QCursor want = tool->cursor().value_or(QCursor(Qt::ArrowCursor));
        QCursor have = cursor();
        if (want.shape() != have.shape() || (want.shape() == Qt::BitmapCursor && want.pixmap().cacheKey() != have.pixmap().cacheKey()))
            setCursor(want);
        return;
    }
    // Set explicitly: the native viewport window otherwise keeps whatever cursor it was entered with, like a text caret.
    Gizmo* gz = currentGizmo();
    Qt::CursorShape shape = !gz                    ? Qt::ArrowCursor
                            : gz->dragging()       ? Qt::ClosedHandCursor
                            : gz->hoveringLocked() ? Qt::ForbiddenCursor
                            : gz->hovering()       ? Qt::OpenHandCursor
                                                   : Qt::ArrowCursor;
    if (cursor().shape() != shape)
        setCursor(shape);
}

void ViewportWidget::submit()
{
    LILA_ZONE();
    updateCursor();

    syncLookThrough();
    if (!mId)
        return;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const bool inherit = scene::inheritVisibility();

    render::FrameState s;
    s.view = mCamera.view();
    s.proj = mCamera.proj(aspect());
    s.eye = mCamera.eye();
    s.zUp = mCamera.zUp;
    const glm::vec4 themeBg = themeSrgb("viewportBackground", glm::vec4(0.30f, 0.30f, 0.30f, 1.0f));
    auto bg = PrefStore::get().value("general", "/viewport/background", json::array({themeBg.r, themeBg.g, themeBg.b}));
    if (bg.is_array() && bg.size() == 3)
        s.background = {bg[0].get<float>(), bg[1].get<float>(), bg[2].get<float>()};
    {
        const PrefStore& p = PrefStore::get();
        s.envPath = environments::resolve(p.value<std::string>("general", "/viewport/env/path", ""));
        s.envRotation = p.value<float>("general", "/viewport/env/rotation", 0.0f);
        s.envIntensity = environments::intensity();
        s.envBackground = p.value<bool>("general", "/viewport/env/background", true);
        s.envBlur = p.value<float>("general", "/viewport/env/blur", 0.6f);
        s.envSaturation = p.value<float>("general", "/viewport/env/saturation", 0.0f);
        s.envBackgroundExposure = p.value<float>("general", "/viewport/env/backgroundExposure", 0.25f);
        // Orthographic views use the flat backdrop, like Maya's; an environment has no single direction there.
        if (mCamera.ortho)
            s.envBackground = false;
        s.groundShadow = p.value<float>("general", "/viewport/groundShadow", 0.5f);
        s.ambientOcclusion = p.value<bool>("general", "/viewport/ao/enabled", true);
        s.aoRadius = p.value<float>("general", "/viewport/ao/radius", 30.0f);
        s.aoIntensity = p.value<float>("general", "/viewport/ao/intensity", 1.0f);
        s.aoFast = p.value<std::string>("general", "/viewport/ao/mode", "accurate") == "fast";
        viewportDisplay::apply(s);
        viewportDisplay::applyCurveWidths(s, devicePixelRatioF());
        displayAids::apply(s, mHudPanel);
    }

    // Like Maya, everything under a selected transform highlights too, a step dimmer.
    std::unordered_set<scene::NodeId> underSelection;
    for (scene::NodeId id : g.selection())
        for (scene::NodeId d : g.descendants(id))
            underSelection.insert(d);

    std::string mode = PrefStore::get().value<std::string>("general", "/viewport/displayMode", "shaded");
    bool wireOnShaded = PrefStore::get().value<bool>("general", "/viewport/wireOnShaded", false);
    bool showMaterials = materialsShown() && !viewportDisplay::defaultMaterial();
    const render::VertexColorMode vertexColors = viewportDisplay::vertexColorMode();
    const render::ColorChannel colorChannel = viewportDisplay::colorChannel();
    const glm::vec4 componentWire = componentPalette().wire;
    const float componentWireWidth = viewportDisplay::hairlineWidth(viewportDisplay::componentThickness().edge, devicePixelRatioF());
    std::unordered_set<scene::NodeId> hilite;
    if (scene::components::mode() != scene::ComponentMode::Object)
        for (scene::NodeId id : hilited(mIsolation))
            hilite.insert(id);
    const bool aids = displayAids::anyDrawn(mHudPanel);
    const bool smoothSubdivisions = PrefStore::get().value<bool>("general", viewportDisplay::kSmoothSubdivisions, false);
    std::vector<displayAids::MeshEntry> aidMeshes;
    const std::unordered_set<scene::NodeId> lodHidden = scene::lod::hidden(g, lodDisplay::view(mHudPanel, viewInfo()));
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || overridden(*n) || !scene::effectivelyVisible(g, id, inherit) || !mIsolation.shows(id) ||
            !scene::selectionMask::shown(*n) || viewportHooks::hiddenInView(id) || lodHidden.contains(id))
            continue;
        int smooth = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
        const scene::MeshPtr& outMesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        scene::MeshPtr shown = smoothedFor(outMesh, smooth);
        std::vector<render::Material> hooked;
        const bool previewed = shown && viewportHooks::hookedMaterials(id, *shown, hooked);
        bool textured = (showMaterials || previewed) && shown;
        const bool tangents = textured && (previewed || needsTangents(*shown));
        // Geometry nodes instances draw each referenced mesh once, instanced, unless something needs the flat mesh's own draw.
        const auto cageWire = smoothSubdivisions ? nullptr : smoothCageFor(outMesh, smooth);
        auto part = isolate::masked(&mIsolation, id, shown, smooth, tangents, cageWire.get());
        gn::InstanceViewPtr instanced;
        if (smooth == 0 && !previewed && !hilite.contains(id) && !part && !cageWire)
            instanced = gn::instanceViewFor(g, id);
        auto rm = instanced ? nullptr : renderMeshFor(shown, tangents, id);
        if (!rm && !instanced)
            continue;
        render::MeshDraw d;
        if (part)
            d.mesh = part->mesh;
        else if (cageWire)
            d.mesh = cageWireMeshFor(*cageWire, tangents);
        else
            d.mesh = std::move(rm);
        if (previewed)
            d.materials = std::move(hooked);
        else if (textured && !instanced)
            d.materials = drawMaterials(*shown);
        d.world = glm::mat4(shownWorld(*n));
        if (aids)
            aidMeshes.push_back({shown, shownWorld(*n)});
        if (vertexColors != render::VertexColorMode::Off && shown && !geo::colors::current(*shown).empty())
        {
            d.vertexColors = vertexColors;
            d.colorChannel = colorChannel;
        }
        d.flags = 0;
        // Lattices are cages to see through, so they only ever draw as wire.
        const bool wireOnly = n->type().isA("lattice");
        const scene::displayLayers::Display layer = scene::displayLayers::display(g, id);
        const bool templated = layer.type == scene::displayLayers::DisplayType::Template;
        if (mode != "wireframe" && !wireOnly && !templated)
            d.flags |= render::DrawShaded;
        if (mode == "wireframe" || wireOnShaded || wireOnly || templated || hilite.contains(id))
            d.flags |= render::DrawWire;
        if (templated && mode != "wireframe")
            d.flags |= render::DrawWireDepth;
        if (layer.color)
            d.wireColor = glm::vec4(glm::vec3(*layer.color), 1.0f);
        else if (templated)
            d.wireColor = kTemplateWireColor;
        if (surfaceSnap::isSurface(id))
        {
            d.flags |= render::DrawWire;
            d.wireColor = themeLinear("wireSnapSurface", kSnapSurfaceWireColor);
        }
        if (hilite.contains(id))
        {
            if (componentWire.a > 0.0f)
                d.wireColor = componentWire;
            d.wireWidth = componentWireWidth;
        }
        // In component mode the hilited mesh shows a plain wire so the selected components stand out.
        bool componentMode = scene::components::mode() != scene::ComponentMode::Object;
        const scene::NodeId arrayed = scene::arrays::arrayAbove(g, id);
        if (g.isSelected(id) && !componentMode)
            d.flags |= render::DrawSelected;
        else if (mPreselect.mode == scene::ComponentMode::Object && !componentMode &&
                 (mPreselect.node == id || (arrayed != scene::kNoNode && preselectedArray(g, id, mPreselect.node))))
            d.flags |= render::DrawPreselect;
        else if (underSelection.contains(id) && !componentMode)
            d.flags |= render::DrawUnderSelected;
        if (arrayed != scene::kNoNode)
            d.instances = arrayPlacements(g, id, n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        if (instanced)
            pushInstanceDraws(s, d, *instanced, textured);
        else
            s.meshes.push_back(std::move(d));
    }

    drawComponents(s);
    drawPreselect(s);
    drawHandlerHover(s);
    OverlayPainter painter(s.overlayTris, devicePixelRatioF());
    {
        constexpr int kCols = 8, kRows = 6;
        auto probe = render::Renderer::get().environmentProbe(s.envPath);
        OverlayPainter::classifyBackdrop(render::backdropLuminance(s, probe.get(), kCols, kRows), mBackdropBright);
        painter.setBackdrop(mBackdropBright, kCols, kRows, QSizeF(width(), height()));
    }
    drawGate(painter);
    const ViewInfo view = viewInfo();
    if (aids)
    {
        for (scene::NodeId id : g.nodes())
            if (scene::Node* n = g.node(id); scene::components::pointProvider(*n) && id != mLookThrough && scene::effectivelyVisible(g, id, inherit) &&
                                             mIsolation.shows(id) && scene::selectionMask::shown(*n))
                aidMeshes.push_back({nullptr, shownWorld(*n), scene::objectBounds(g, id)});
        displayAids::draw(s, painter, view, mHudPanel, aidMeshes, devicePixelRatioF());
    }
    // Locators and extension node types. Highlighted draws last: with an equal-depth test the later line wins, so a
    // selected locator sitting exactly on an unselected one stays visible.
    for (int pass = 0; pass < 3; ++pass)
        for (scene::NodeId id : g.nodes())
        {
            scene::Node* n = g.node(id);
            const drawOverrides::DrawOverride* o = drawOverrides::forType(n->type());
            if (!o || !o->draw || id == mLookThrough || !scene::effectivelyVisible(g, id, inherit) || !scene::selectionMask::shown(*n) ||
                !mIsolation.shows(id))
                continue;
            bool hot = mPreselect.mode == scene::ComponentMode::Object && mPreselect.node == id;
            int rank = g.isSelected(id) ? 2 : hot || underSelection.contains(id) ? 1 : 0;
            if (rank != pass)
                continue;
            using HL = drawOverrides::Highlight;
            drawOverrides::DrawContext dc{.frame = s, .painter = painter, .view = view, .node = *n};
            dc.world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            dc.highlight = rank == 2 ? HL::Selected : hot ? HL::Preselected : rank == 1 ? HL::UnderSelected : HL::None;
            dc.color = rank == 2 ? themeLinear("wireSelected", kSelectedColor)
                       : hot ? themeLinear("wirePreselect", kPreselectColor)
                       : rank == 1 ? themeLinear("wireUnderSelected", kChildOfSelectedColor)
                                   : layerLineColor(g, id, themeLinear("wireframe", kLocatorColor));
            o->draw(dc);
        }
    renderPasses::Context passes{.frame = s, .painter = painter, .view = view, .panel = mHudPanel};
    renderPasses::run(renderPasses::Stage::Scene, passes);
    for (const auto& drawer : viewportHooks::drawers())
        drawer(s, painter, view);
    if (ViewportTool* tool = viewportTools::active())
        tool->draw(s, painter, view);
    if (Gizmo* gz = drawnGizmo())
    {
        gz->setView(view);
        gz->draw(painter);
    }
    renderPasses::run(renderPasses::Stage::Overlay, passes);
    if (hud::shown(mHudPanel, "originAxis"))
        hud::drawOriginAxis(s, painter, view);
    drawViewIndicator(painter);
    drawHud(painter);
    if (QPointF cursor = mapFromGlobal(QCursor::pos().toPointF()); rect().contains(cursor.toPoint()))
    {
        std::vector<const cursorBadges::Badge*> badges = cursorBadges::active(state(cursor));
        cursorBadges::draw(painter, cursor, badges);
        cursorBadges::publish(this, badges);
    }
    else
        cursorBadges::publish(this, {});
    renderPasses::run(renderPasses::Stage::Final, passes);
    // The ground grid is edge-on in side views, so they get a grid in the view plane through the origin instead.
    if (mCamera.ortho && std::abs(glm::dot(mCamera.forward(), mCamera.up())) < 0.5)
    {
        glm::dvec3 r = mCamera.right(), u = glm::cross(r, mCamera.forward());
        glm::dvec2 ext = mCamera.viewExtents(aspect());
        double halfW = ext.x, halfH = ext.y;
        double step = std::pow(10.0, std::floor(std::log10(std::max(1e-3, mCamera.orthoWidth / 20.0))));
        glm::dvec3 centre = mCamera.target - mCamera.forward() * glm::dot(mCamera.target, mCamera.forward());
        double cx = glm::dot(centre, r), cy = glm::dot(centre, u);
        auto lineColor = [](double v, double stepSize) {
            bool axis = std::abs(v) < stepSize * 0.5;
            bool major = std::abs(std::fmod(std::abs(v) + stepSize * 0.5, stepSize * 10.0)) < stepSize;
            float g = axis ? 0.0f : major ? 0.045f : 0.025f; // linear grays
            return glm::vec4(g, g, g, 1.0f);
        };
        for (double x = std::floor((cx - halfW) / step) * step; x <= cx + halfW; x += step)
        {
            glm::vec4 c = lineColor(x, step);
            s.lines.push_back({glm::vec3(r * x + u * (cy - halfH)), c});
            s.lines.push_back({glm::vec3(r * x + u * (cy + halfH)), c});
        }
        for (double y = std::floor((cy - halfH) / step) * step; y <= cy + halfH; y += step)
        {
            glm::vec4 c = lineColor(y, step);
            s.lines.push_back({glm::vec3(r * (cx - halfW) + u * y), c});
            s.lines.push_back({glm::vec3(r * (cx + halfW) + u * y), c});
        }
    }
    if (!s.overlayTris.empty())
        s.atlas = painter.atlas();

    if (mMarquee && mLassoDrag && mLasso.size() > 1)
    {
        auto ndc = [&](glm::dvec2 p) { return glm::vec3(float(p.x / width() * 2.0 - 1.0), float(p.y / height() * 2.0 - 1.0), 0.5f); };
        // Dashes alternate light and dark so the outline reads on any background.
        constexpr double kDash = 6.0;
        const glm::vec4 shades[2] = {{0.9f, 0.9f, 0.9f, 0.95f}, {0.12f, 0.12f, 0.12f, 0.95f}};
        double run = 0.0;
        for (size_t i = 0; i < mLasso.size(); ++i)
        {
            glm::dvec2 a = mLasso[i], b = mLasso[(i + 1) % mLasso.size()];
            double len = glm::distance(a, b);
            for (double t = 0.0; t < len;)
            {
                double step = std::min(len - t, kDash - std::fmod(run, kDash));
                const glm::vec4& color = shades[int(run / kDash) % 2];
                s.overlay.push_back({ndc(a + (b - a) * (t / len)), color});
                s.overlay.push_back({ndc(a + (b - a) * ((t + step) / len)), color});
                t += step;
                run += step;
            }
        }
    }
    else if (mMarquee)
    {
        auto ndc = [&](QPointF p) {
            return glm::vec3(float(p.x() / width() * 2.0 - 1.0), float(p.y() / height() * 2.0 - 1.0), 0.5f);
        };
        glm::vec3 a = ndc(mPressPos), b = ndc(mLastPos);
        glm::vec3 c[4] = {a, {b.x, a.y, 0.5f}, b, {a.x, b.y, 0.5f}};
        const glm::vec4 color(0.9f, 0.9f, 0.9f, 0.9f);
        for (int i = 0; i < 4; ++i)
        {
            s.overlay.push_back({c[i], color});
            s.overlay.push_back({c[(i + 1) % 4], color});
        }
    }
    render::Renderer::get().submit(mId, std::move(s));
}

ViewInfo ViewportWidget::viewInfo() const
{
    ViewInfo v;
    v.view = mCamera.view();
    v.proj = mCamera.proj(aspect());
    v.eye = mCamera.eye();
    v.width = width();
    v.height = height();
    v.camera = mViewName;
    v.ortho = mCamera.ortho;
    v.film = mCamera.filmExtents();
    v.isolation = &mIsolation;
    return v;
}

void ViewportWidget::wrapCursor(QPointF p)
{
    const glm::dvec2 target = gizmo::CursorWrap::target({p.x(), p.y()}, width(), height());
    if (target == glm::dvec2(p.x(), p.y()))
        return;
    const QPoint to = QPointF(target.x, target.y).toPoint();
    QCursor::setPos(mapToGlobal(to));
    mCursorWrap.warped({p.x(), p.y()}, {double(to.x()), double(to.y())});
    if (mGizmoDrag)
        mGizmoDrag->warped(QPointF(to) - p);
}

void ViewportWidget::syncGizmos()
{
    auto fit = [this](GizmoSlot& slot, const std::string& id) {
        if (slot.gizmo && slot.id == id && gizmos::find(id))
            return;
        if (slot.gizmo && mGizmoDrag == slot.gizmo.get())
        {
            mGizmoDrag->cancel();
            mGizmoDrag = nullptr;
        }
        slot.gizmo = id.empty() ? nullptr : gizmos::create(id);
        slot.id = slot.gizmo ? id : std::string();
    };
    const std::string active = gizmos::activeId();
    fit(mGizmo, active);
    fit(mDefaultGizmo, active == gizmos::kDefault ? std::string() : std::string(gizmos::kDefault));
}

Gizmo* ViewportWidget::currentGizmo()
{
    syncGizmos();
    if (mGizmoDrag)
        return mGizmoDrag;
    auto pick = [this](std::string_view mode) -> Gizmo* {
        for (GizmoSlot* slot : {&mGizmo, &mDefaultGizmo})
            if (slot->gizmo && slot->gizmo->supports(mode))
                return slot->gizmo.get();
        return nullptr;
    };
    if (ViewportTool* tool = viewportTools::active())
        if (Gizmo* gz = pick(tool->name()))
            return gz;
    return pick(tools::name(tools::current()));
}

Gizmo* ViewportWidget::drawnGizmo()
{
    Gizmo* gz = currentGizmo();
    if (!gz)
        return nullptr;
    ViewportTool* tool = viewportTools::active();
    return !tool || gz->supports(tool->name()) ? gz : nullptr;
}

ViewportState ViewportWidget::state(QPointF cursor)
{
    ViewportState st;
    st.view = viewInfo();
    st.panel = mHudPanel;
    st.cursor = cursor;
    st.gizmo = currentGizmo();
    if (ViewportTool* tool = viewportTools::active())
        st.tool = tool->name();
    st.navigating = mNav != Nav::None;
    if (mTabStroke.active())
        st.tabSelects = mTabStroke.op() != paintSelect::Stroke::Op::Remove;
    else if (mTabHeld && !viewportTools::active())
    {
        Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
        st.tabSelects = tabSelect::modeAtPress(mTabUnder, mods & Qt::ShiftModifier, mods & Qt::ControlModifier) == tabSelect::Mode::Select;
    }
    return st;
}

std::optional<QPointF> ViewportWidget::project(const glm::dvec3& world) const
{
    glm::dvec4 clip = mCamera.proj(aspect()) * mCamera.view() * glm::dvec4(world, 1.0);
    if (clip.w <= 1e-9)
        return std::nullopt;
    return QPointF((clip.x / clip.w * 0.5 + 0.5) * width(), (clip.y / clip.w * 0.5 + 0.5) * height());
}

std::vector<std::string> ViewportWidget::hitTest(QPointF a, QPointF b)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const bool inherit = scene::inheritVisibility();
    QRectF box = QRectF(a, b).normalized();
    bool click = box.width() < 4 && box.height() < 4;

    json hits = json::array();
    double best = 6.0; // px
    std::string bestName;
    ViewInfo view = viewInfo();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        const drawOverrides::DrawOverride* o = drawOverrides::forType(n->type());
        if (!o || !o->pick || id == mLookThrough || !viewportPickable(g, id, inherit) || !scene::selectionMask::pickable(*n) ||
            !mIsolation.shows(id))
            continue;
        std::optional<double> px = o->pick({.view = view, .node = *n, .world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix), .a = a, .b = b, .click = click});
        if (!px)
            continue;
        // The override only knows the lasso's bounds, so its pivot has to fall inside the lasso itself.
        if (!click && mLassoPick)
            if (auto s = project(glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3])); !s || !inSelectRegion(box, *s))
                continue;
        if (!click)
            hits.push_back(n->path());
        else if (*px < best)
        {
            best = *px;
            bestName = n->path();
        }
    }
    // Meshes: a click takes the nearest surface under the cursor unless a locator line is right there; a box takes
    // any mesh with a vertex inside it.
    glm::dvec3 rayO, rayD;
    view.ray(a, rayO, rayD);
    double nearest = 1e300;
    std::string nearestMesh;
    const std::unordered_set<scene::NodeId> lodHidden = scene::lod::hidden(g, lodDisplay::view(mHudPanel, view));
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || overridden(*n) || !viewportPickable(g, id, inherit) || !scene::selectionMask::pickable(*n) ||
            !mIsolation.shows(id) || viewportHooks::hiddenInView(id) || lodHidden.contains(id))
            continue;
        scene::MeshPtr outMesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        auto rm = renderMeshFor(outMesh);
        if (!rm || rm->vertices.empty())
            continue;
        glm::dmat4 world = shownWorld(*n);
        // Array copies pick as their array, so a click on one selects the generator rather than the source.
        const std::vector<scene::arrays::Copy> copies = scene::arrays::copiesOf(g, id);
        if (click)
        {
            if (auto t = isolate::raycast(&mIsolation, id, outMesh, world, rayO, rayD); t && t->distance < nearest)
            {
                nearest = t->distance;
                nearestMesh = n->path();
            }
            for (const scene::arrays::Copy& c : copies)
                if (auto t = isolate::raycast(&mIsolation, id, outMesh, c.transform * world, rayO, rayD); t && t->distance < nearest)
                {
                    nearest = t->distance;
                    nearestMesh = g.node(c.array)->path();
                }
            continue;
        }
        size_t step = std::max<size_t>(1, rm->vertices.size() / 20000);
        for (const scene::arrays::Copy& c : copies)
        {
            const std::string arrayPath = g.node(c.array)->path();
            if (std::ranges::any_of(hits, [&](const json& h) { return h.get<std::string>() == arrayPath; }))
                continue;
            const glm::dmat4 at = c.transform * world;
            for (size_t k = 0; k < rm->vertices.size(); k += step)
                if (auto p = project(glm::dvec3(at * glm::dvec4(glm::dvec3(rm->vertices[k].pos), 1.0))); p && inSelectRegion(box, *p))
                {
                    hits.push_back(arrayPath);
                    break;
                }
        }
        if (mIsolation.isPartial(id))
        {
            // Only the isolated faces' points count toward a marquee.
            const geo::Mesh& m = *outMesh;
            auto vis = isolate::visibility(&mIsolation, id, outMesh);
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p) && vis->point(p))
                    if (auto s = project(glm::dvec3(world * glm::dvec4(m.position(p), 1.0))); s && inSelectRegion(box, *s))
                    {
                        hits.push_back(n->path());
                        break;
                    }
            continue;
        }
        for (size_t k = 0; k < rm->vertices.size(); k += step)
            if (auto p = project(glm::dvec3(world * glm::dvec4(glm::dvec3(rm->vertices[k].pos), 1.0))); p && inSelectRegion(box, *p))
            {
                hits.push_back(n->path());
                break;
            }
    }
    for (const auto& picker : viewportHooks::pickers())
        for (const viewportHooks::PickHit& h : picker(a, b, click, view))
        {
            const scene::Node* hn = findNode(h.name);
            if (hn ? !scene::selectionMask::pickable(*hn) : !scene::selectionMask::enabled(scene::selectionMask::Kind::Other))
                continue;
            if (hn && (!scene::displayLayers::pickable(g, hn->id()) || !mIsolation.shows(hn->id())))
                continue;
            if (!click)
                hits.push_back(h.name);
            else if (h.pixels < best)
            {
                best = h.pixels;
                bestName = h.name;
            }
        }
    if (click && bestName.empty() && !nearestMesh.empty())
        hits.push_back(nearestMesh);
    if (click && !bestName.empty())
        hits.push_back(bestName);
    return hits.get<std::vector<std::string>>();
}

namespace
{
/** Meshes components can be picked on: the selected ones (Maya's hilite) and those under selected groups, or every visible mesh if nothing selected has components. */
std::vector<scene::NodeId> hilited(const scene::isolation::Set& iso)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> out;
    const bool inherit = scene::inheritVisibility();
    // Meshes under a selected group count too; hidden ones only when selected themselves.
    for (scene::NodeId id : uv::meshesUnder(g, g.selection()))
        if ((std::ranges::find(g.selection(), id) != g.selection().end() || viewportPickable(g, id, inherit)) && iso.shows(id))
            out.push_back(id);
    if (out.empty() && !scene::components::selectionHasPointNodes(g))
        for (scene::NodeId id : g.nodes())
            if (scene::Node* n = g.node(id); n->type().isA("mesh") && viewportPickable(g, id, inherit) && iso.shows(id))
                out.push_back(id);
    return out;
}

/** Selected nodes whose points a provider gives (curves), and those already holding a point selection. */
std::vector<scene::NodeId> pointNodes(const scene::isolation::Set& iso)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> out;
    auto add = [&](scene::NodeId id) {
        scene::Node* n = g.node(id);
        if (n && scene::components::pointProvider(*n) && iso.shows(id) && std::ranges::find(out, id) == out.end())
            out.push_back(id);
    };
    for (scene::NodeId id : g.selection())
        add(id);
    for (auto& [id, ids] : scene::components::selected(scene::ComponentMode::Vertex))
        add(id);
    return out;
}

/**
 * Where a mesh's components draw and pick, world space: on its cage, or along the surface of its smooth preview when it
 * has one and is edited directly (modifiers put the cage somewhere else).
 */
struct ComponentShape
{
    const geo::Mesh& cage;
    glm::dmat4 world;
    std::shared_ptr<const SmoothCage> smooth;
    int level = 0;

    ComponentShape(const scene::Node& n, const geo::Mesh& cageMesh, const scene::MeshPtr& cagePtr, const glm::dmat4& w) : cage(cageMesh), world(w)
    {
        if (n.type().isA("mesh") && cagePtr == n.as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            level = int(n.as<int64_t>(scene::MeshNode::kSmoothPreview));
        smooth = smoothCageFor(cagePtr, level);
        if (!smooth)
            level = 0;
    }

    /** The mesh components are picked on: the smooth one, whose ids map back with cageOf. */
    const geo::Mesh& picked() const { return smooth ? *smooth->mesh : cage; }
    /** A component of picked() as the cage's, kInvalid for one that is not on the cage. */
    uint32_t cageOf(scene::ComponentMode mode, uint32_t id) const
    {
        if (!smooth)
            return id;
        const std::vector<geo::Index>& map = mode == scene::ComponentMode::Vertex ? smooth->cagePoints
                                             : mode == scene::ComponentMode::Edge ? smooth->map.edges
                                                                                  : smooth->map.faces;
        return id < map.size() ? map[id] : geo::kInvalid;
    }

    glm::dvec3 at(const geo::Mesh& m, geo::Index p) const { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); }
    glm::dvec3 point(uint32_t p) const { return smooth ? at(*smooth->mesh, smooth->map.points[p]) : at(cage, p); }
    /** A point on the surface along the edge, for occlusion tests. */
    glm::dvec3 edgeAnchor(uint32_t e) const
    {
        if (!smooth || smooth->segments(e).empty())
            return (point(cage.edgePoint(e, 0)) + point(cage.edgePoint(e, 1))) * 0.5;
        std::span<const geo::Index> segs = smooth->segments(e);
        return at(*smooth->mesh, smooth->mesh->edgePoint(segs[segs.size() / 2], 0));
    }
    glm::dvec3 faceCentre(uint32_t f) const
    {
        return smooth ? at(*smooth->mesh, smooth->map.facePoints[f]) : glm::dvec3(world * glm::dvec4(cage.faceCentroid(f), 1.0));
    }
    /** fn(a, b) for each straight piece of edge e. */
    template <class Fn> void forEachSegment(uint32_t e, Fn&& fn) const
    {
        if (!smooth)
            return fn(point(cage.edgePoint(e, 0)), point(cage.edgePoint(e, 1)));
        const geo::Mesh& sm = *smooth->mesh;
        for (geo::Index s : smooth->segments(e))
            fn(at(sm, sm.edgePoint(s, 0)), at(sm, sm.edgePoint(s, 1)));
    }
    /** fn(a, b) for each straight piece of face f's border. */
    template <class Fn> void forEachOutline(uint32_t f, Fn&& fn) const
    {
        cage.forEachFaceCorner(f, [&](geo::Index c) { forEachSegment(cage.cornerEdge(c), fn); });
    }
    /** fn(p) for each corner of the triangles filling face f, three per triangle. */
    template <class Fn> void forEachFillVertex(uint32_t f, Fn&& fn) const
    {
        if (!smooth)
        {
            for (uint32_t p : scene::componentPick::fillTriangles(cage, f))
                fn(at(cage, p));
            return;
        }
        for (geo::Index part : smooth->parts(f))
            for (uint32_t p : scene::componentPick::fillTriangles(*smooth->mesh, part))
                fn(at(*smooth->mesh, p));
    }
    /** Screen-space polygons covering face f: the face itself, or each smoothed piece of it. */
    template <class Fn> void forEachPolygon(uint32_t f, Fn&& fn) const
    {
        auto corners = [&](const geo::Mesh& m, geo::Index face) {
            std::vector<glm::dvec3> poly;
            m.forEachFaceCorner(face, [&](geo::Index c) { poly.push_back(at(m, m.cornerPoint(c))); });
            fn(poly);
        };
        if (!smooth)
            return corners(cage, f);
        for (geo::Index part : smooth->parts(f))
            corners(*smooth->mesh, part);
    }
};

/** Whether the node's surface as drawn, smooth preview included, is hit along the ray nearer than dist. */
bool occludes(const isolate::Set* set, scene::NodeId id, const glm::dvec3& eye, const glm::dvec3& dir, double dist)
{
    scene::Node* n = scene::Graph::current().node(id);
    const int level = n->type().isA("mesh") ? int(n->as<int64_t>(scene::MeshNode::kSmoothPreview)) : 0;
    scene::MeshPtr shown = smoothedFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh), level);
    auto hit = isolate::raycast(set, id, shown, shownWorld(*n), eye, dir, level);
    return hit && hit->distance < dist * (1.0 - 1e-4) - 1e-3;
}

/** Provider points of the node, world space, with their component ids. */
std::vector<std::pair<uint32_t, glm::dvec3>> worldPoints(scene::Graph& g, scene::NodeId id)
{
    scene::Node* n = g.node(id);
    const scene::components::PointProvider* pp = n ? scene::components::pointProvider(*n) : nullptr;
    if (!pp)
        return {};
    glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    auto pts = pp->points(g, id);
    for (auto& [cid, p] : pts)
        p = glm::dvec3(w * glm::dvec4(p, 1.0));
    return pts;
}
} // namespace

scene::componentPick::Hover ViewportWidget::componentAt(QPointF a)
{
    LILA_ZONE();

    using scene::ComponentMode;
    namespace cp = scene::componentPick;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    ComponentMode mode = scene::components::mode();
    cp::Hover best{mode};
    if (mode == ComponentMode::Object)
        return best;
    ViewInfo view = viewInfo();
    glm::dvec3 rayO, rayD;
    view.ray(a, rayO, rayD);
    std::vector<scene::NodeId> live = hilited(mIsolation);

    // Visible means nothing on any hilited mesh is closer along the view ray.
    auto visible = [&](const glm::dvec3& world) {
        glm::dvec3 dir = world - view.eye;
        double dist = glm::length(dir);
        dir /= dist;
        for (scene::NodeId id : live)
            if (occludes(&mIsolation, id, view.eye, dir, dist))
                return false;
        return true;
    };

    cp::Screen screen{mCamera.proj(aspect()) * mCamera.view(), double(width()), double(height())};
    const bool cull = paintSelect::backfaceCulling();
    double bestPx = cp::radius(mode);
    double bestRay = 1e300;
    for (scene::NodeId id : live)
    {
        scene::Node* n = g.node(id);
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        glm::dmat4 world = shownWorld(*n);
        const ComponentShape shape(*n, *mesh, mesh, world);
        if (mode == ComponentMode::Face)
        {
            const scene::MeshPtr& target = shape.smooth ? shape.smooth->mesh : mesh;
            auto pd = pickDataFor(target);
            if (!pd)
                continue;
            auto hit = isolate::raycast(&mIsolation, id, target, world, rayO, rayD, shape.level);
            if (!hit || hit->distance >= bestRay || hit->triangle >= pd->triangleFaces.size())
                continue;
            const uint32_t f = shape.cageOf(mode, pd->triangleFaces[hit->triangle]);
            if (f != geo::kInvalid && (!cull || paintSelect::frontFacing(*mesh, world, mode, f, view)))
            {
                bestRay = hit->distance;
                best.node = id;
                best.id = f;
            }
            continue;
        }
        auto vis = isolate::visibility(&mIsolation, id, mesh);
        for (const cp::Candidate& c : cp::nearby(shape.picked(), world, mode, screen, {a.x(), a.y()}, bestPx))
        {
            const uint32_t cid = shape.cageOf(mode, c.id);
            if (cid != geo::kInvalid && vis->shows(mode, cid) && (!cull || paintSelect::frontFacing(*mesh, world, mode, cid, view)) &&
                visible(mode == ComponentMode::Vertex ? shape.point(cid) : shape.edgeAnchor(cid)))
            {
                bestPx = c.px;
                best.node = id;
                best.id = cid;
                break;
            }
        }
    }
    if (mode == ComponentMode::Vertex)
        for (scene::NodeId id : pointNodes(mIsolation))
            for (auto& [cid, p] : worldPoints(g, id))
                if (auto s = view.project(p))
                    if (double d = std::hypot(s->x() - a.x(), s->y() - a.y()); d < bestPx)
                    {
                        bestPx = d;
                        best.node = id;
                        best.id = cid;
                    }
    return best;
}

void ViewportWidget::pickComponents(QPointF a, QPointF b, Qt::KeyboardModifiers mods)
{
    LILA_ZONE();

    using scene::ComponentMode;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    ComponentMode mode = scene::components::mode();
    QRectF box = QRectF(a, b).normalized();
    bool click = box.width() < 4 && box.height() < 4;

    std::map<scene::NodeId, std::vector<uint32_t>> hits;
    if (click)
    {
        if (scene::componentPick::Hover h = componentAt(a); !h.empty())
        {
            hits[h.node].push_back(h.id);
            mClicked[1] = mClicked[0];
            mClicked[0] = h;
        }
    }
    else
    {
        // A marquee picks through the mesh unless camera-based selection limits it to what the camera sees.
        bool cameraBased = PrefStore::get().value<bool>("general", "/select/cameraBased", false);
        glm::dvec3 eye = viewInfo().eye;
        auto visible = [&](const glm::dvec3& world) {
            if (!cameraBased)
                return true;
            glm::dvec3 dir = world - eye;
            double dist = glm::length(dir);
            dir /= dist;
            for (scene::NodeId id : hilited(mIsolation))
                if (occludes(&mIsolation, id, eye, dir, dist))
                    return false;
            return true;
        };
        // Edges and faces count when the region touches them anywhere, or with the pref off only when it holds their centre.
        const bool touch = PrefStore::get().value<bool>("general", "/select/marqueeTouch", true);
        for (scene::NodeId id : hilited(mIsolation))
        {
            scene::Node* n = g.node(id);
            scene::MeshPtr mesh = scene::modifiers::cage(g, id);
            if (!mesh)
                continue;
            const geo::Mesh& m = *mesh;
            const ComponentShape shape(*n, m, mesh, shownWorld(*n));
            auto toPx = [](QPointF p) { return glm::dvec2(p.x(), p.y()); };
            if (mode == ComponentMode::Vertex)
            {
                for (uint32_t p = 0; p < m.pointCapacity(); ++p)
                    if (m.pointValid(p))
                        if (auto s = project(shape.point(p)); s && inSelectRegion(box, *s) && visible(shape.point(p)))
                            hits[id].push_back(p);
            }
            else if (mode == ComponentMode::Edge)
            {
                for (uint32_t e = 0; e < m.edgeCapacity(); ++e)
                {
                    if (!m.edgeValid(e))
                        continue;
                    bool in = false;
                    if (touch)
                        shape.forEachSegment(e, [&](const glm::dvec3& a, const glm::dvec3& b) {
                            auto s0 = project(a), s1 = project(b);
                            in = in || (s0 && s1 && touchesSelectRegion(box, {toPx(*s0), toPx(*s1)}));
                        });
                    else if (auto s = project(shape.edgeAnchor(e)))
                        in = inSelectRegion(box, *s);
                    if (in && visible(shape.edgeAnchor(e)))
                        hits[id].push_back(e);
                }
            }
            else if (mode == ComponentMode::Face)
            {
                for (uint32_t f = 0; f < m.faceCapacity(); ++f)
                    if (m.faceValid(f))
                    {
                        glm::dvec3 c = shape.faceCentre(f);
                        bool in = false;
                        if (touch)
                            shape.forEachPolygon(f, [&](const std::vector<glm::dvec3>& corners) {
                                if (in)
                                    return;
                                std::vector<glm::dvec2> poly;
                                for (const glm::dvec3& w : corners)
                                    if (auto s = project(w))
                                        poly.push_back(toPx(*s));
                                in = poly.size() == corners.size() && touchesSelectRegion(box, poly);
                            });
                        else if (auto s = project(c))
                            in = inSelectRegion(box, *s);
                        if (in && visible(c))
                            hits[id].push_back(f);
                    }
            }
        }
    }

    if (!click && mode == ComponentMode::Vertex)
        for (scene::NodeId id : pointNodes(mIsolation))
            for (auto& [cid, p] : worldPoints(g, id))
                if (auto sp = project(p); sp && inSelectRegion(box, *sp))
                    hits[id].push_back(cid);

    for (auto it = hits.begin(); it != hits.end();)
    {
        auto vis = isolate::visibility(&mIsolation, it->first, scene::modifiers::cage(g, it->first));
        std::erase_if(it->second, [&](uint32_t id) { return !vis->shows(mode, id); });
        it = it->second.empty() ? hits.erase(it) : std::next(it);
    }

    if (!click && paintSelect::backfaceCulling())
    {
        ViewInfo view = viewInfo();
        for (auto it = hits.begin(); it != hits.end();)
        {
            scene::MeshPtr mesh = scene::modifiers::cage(g, it->first);
            if (!mesh && scene::components::pointProvider(*g.node(it->first)))
            {
                ++it;
                continue;
            }
            glm::dmat4 world = shownWorld(*g.node(it->first));
            std::erase_if(it->second, [&](uint32_t id) { return !mesh || !paintSelect::frontFacing(*mesh, world, mode, id, view); });
            it = it->second.empty() ? hits.erase(it) : std::next(it);
        }
    }

    bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    if (click && hits.empty() && !shift && !ctrl && pickOtherObject(a))
        return;
    const char* flag = mode == ComponentMode::Vertex ? "vertices" : mode == ComponentMode::Edge ? "edges" : "faces";
    try
    {
        UndoChunk chunk("Select Components");
        bool first = true;
        if (hits.empty() && !shift && !ctrl)
            for (scene::NodeId id : hilited(mIsolation))
            {
                ops::execute("selectComponents", {{"args", {g.node(id)->path()}}, {flag, json::array()}}, CallSource::UI);
                break;
            }
        for (auto& [id, ids] : hits)
        {
            json call = {{"args", {g.node(id)->path()}}, {flag, ids}};
            if (shift && ctrl)
                call["add"] = true;
            else if (ctrl)
                call["deselect"] = true;
            else if (shift)
                call["toggle"] = true;
            else if (!first)
                call["add"] = true;
            first = false;
            ops::execute("selectComponents", call, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

bool ViewportWidget::pickOtherObject(QPointF at)
{
    std::vector<std::string> hits = hitTest(at, at);
    const scene::Node* n = hits.empty() ? nullptr : findNode(hits.front());
    if (!n)
        return false;
    const std::vector<scene::NodeId> live = hilited(mIsolation);
    if (std::ranges::find(live, n->id()) != live.end())
        return false;
    try
    {
        ops::execute("select", {{"args", {n->path()}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return true;
}

void ViewportWidget::drawObjectSymmetryEdges(render::FrameState& s)
{
    LILA_ZONE();

    const PrefStore& prefs = PrefStore::get();
    if (!prefs.value<bool>("general", viewportDisplay::kSymmetryEdges, true) ||
        !prefs.value<bool>("general", viewportDisplay::kSymmetryEdgesObjectMode, true))
        return;
    scene::Graph& g = scene::Graph::current();
    s.symmetryLineWidth = viewportDisplay::hairlineWidth(viewportDisplay::componentThickness().symmetryEdge, devicePixelRatioF());
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || !scene::effectivelyVisible(g, id) || !isolate::shows(&mIsolation, id))
            continue;
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        const ComponentShape shape(*n, *mesh, mesh, shownWorld(*n));
        const auto vis = isolate::visibility(&mIsolation, id, mesh);
        for (uint32_t e : symmetryPlaneEdges(*n, mesh))
            if (vis->edge(e))
                shape.forEachSegment(e, [&](const glm::dvec3& a, const glm::dvec3& b) {
                    s.symmetryLines.push_back({glm::vec3(a), kSymmetryRed});
                    s.symmetryLines.push_back({glm::vec3(b), kSymmetryRed});
                });
        radialPlaneLines(*n, *mesh, shownWorld(*n), s.lines);
    }
}

/**
 * A mesh's vertex dots in world space, kept until the mesh, its placement, smoothing, isolation, symmetry or the
 * component selection changes, so a dense mesh's dots are built once instead of every frame.
 */
static std::shared_ptr<const render::PointCloud> vertexDots(scene::NodeId id, const ComponentShape& shape, const scene::MeshPtr& mesh,
                                                            const std::shared_ptr<const scene::isolation::Visibility>& vis,
                                                            const geo::SymmetryMap* sym, const scene::components::Ids& selected)
{
    struct Entry
    {
        std::weak_ptr<const geo::Mesh> mesh;
        std::shared_ptr<const SmoothCage> smooth;
        std::shared_ptr<const scene::isolation::Visibility> vis;
        glm::dmat4 world;
        const geo::SymmetryMap* sym = nullptr;
        uint64_t serial = 0;
        std::shared_ptr<const render::PointCloud> cloud;
    };
    static std::unordered_map<scene::NodeId, Entry> cache;
    Entry& e = cache[id];
    const uint64_t serial = scene::components::serial();
    if (e.cloud && e.mesh.lock() == mesh && e.smooth == shape.smooth && e.vis == vis && e.world == shape.world && e.sym == sym && e.serial == serial)
        return e.cloud;
    LILA_ZONE_N("vertexDots");

    const geo::Mesh& m = *mesh;
    std::vector<uint8_t> on(m.pointCapacity(), render::PointDraw::Normal);
    if (sym)
        for (uint32_t p : sym->expandPoints(selected))
            if (p < on.size())
                on[p] = render::PointDraw::Mirror;
    for (uint32_t p : selected)
        if (p < on.size())
            on[p] = render::PointDraw::Selected;
    auto cloud = std::make_shared<render::PointCloud>();
    cloud->points.resize(m.pointCapacity());
    tbb::parallel_for(tbb::blocked_range<geo::Index>(0, m.pointCapacity(), 16384), [&](const tbb::blocked_range<geo::Index>& r) {
        for (geo::Index p = r.begin(); p != r.end(); ++p)
        {
            const bool shown = m.pointValid(p) && vis->point(p);
            cloud->points[p] = {shown ? glm::vec3(shape.point(p)) : glm::vec3(0.0f), shown ? uint32_t(on[p]) : uint32_t(render::PointDraw::Hidden)};
        }
    });
    // Dots of meshes that are gone are not needed again, and each holds a point per vertex.
    std::erase_if(cache, [&](const auto& kv) { return kv.first != id && kv.second.mesh.expired(); });
    if (cache.size() > 64)
        std::erase_if(cache, [&](const auto& kv) { return kv.first != id; });
    Entry& slot = cache[id];
    slot = {mesh, shape.smooth, vis, shape.world, sym, serial, std::move(cloud)};
    return slot.cloud;
}

void ViewportWidget::drawComponents(render::FrameState& s)
{
    LILA_ZONE();

    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    if (mode == ComponentMode::Object)
    {
        drawObjectSymmetryEdges(s);
        return;
    }
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> live = hilited(mIsolation);
    const ComponentPalette palette = componentPalette();
    const glm::vec4 dotLinear = palette.point, hotLinear = palette.selected, faceFillLinear = palette.selectedFill;
    const viewportDisplay::ComponentThickness thickness = viewportDisplay::componentThickness();
    const float wireWidth = viewportDisplay::hairlineWidth(thickness.edge, devicePixelRatioF());
    s.selectedEdgeWidth = viewportDisplay::hairlineWidth(thickness.edgeSelected, devicePixelRatioF());
    s.symmetryLineWidth = viewportDisplay::hairlineWidth(thickness.symmetryEdge, devicePixelRatioF());
    const bool showSymmetry = PrefStore::get().value<bool>("general", viewportDisplay::kSymmetryEdges, true);
    for (scene::NodeId id : live)
    {
        scene::Node* n = g.node(id);
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        const geo::Mesh& m = *mesh;
        glm::dmat4 world = shownWorld(*n);
        // With modifiers on, the cage's wire shows what you're editing.
        if (mesh != n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            if (auto rm = renderMeshFor(mesh))
            {
                render::MeshDraw cageDraw;
                cageDraw.mesh = rm;
                if (auto part = isolate::masked(&mIsolation, id, mesh))
                    cageDraw.mesh = part->mesh;
                cageDraw.world = glm::mat4(world);
                // Depth tested against the result, as picking is: hidden cage edges stay hidden.
                cageDraw.flags = render::DrawWire | render::DrawWireDepth;
                cageDraw.wireColor = palette.wire;
                cageDraw.wireWidth = wireWidth;
                s.meshes.push_back(std::move(cageDraw));
            }
        const ComponentShape shape(*n, m, mesh, world);
        auto wp = [&](uint32_t p) { return glm::vec3(shape.point(p)); };
        auto edgeLines = [&](uint32_t e, std::vector<render::LineVertex>& out, const glm::vec4& col) {
            shape.forEachSegment(e, [&](const glm::dvec3& a, const glm::dvec3& b) {
                out.push_back({glm::vec3(a), col});
                out.push_back({glm::vec3(b), col});
            });
        };
        auto faceLines = [&](uint32_t f, const glm::vec4& fill, const glm::vec4& line) {
            shape.forEachFillVertex(f, [&](const glm::dvec3& p) { s.tris3d.push_back({glm::vec3(p), fill}); });
            shape.forEachOutline(f, [&](const glm::dvec3& a, const glm::dvec3& b) {
                s.linesOnTop.push_back({glm::vec3(a), line});
                s.linesOnTop.push_back({glm::vec3(b), line});
            });
        };
        const auto vis = isolate::visibility(&mIsolation, id, mesh);
        const auto& selFaces = scene::components::selected(id, ComponentMode::Face);
        const auto& selEdges = scene::components::selected(id, ComponentMode::Edge);
        const auto& selPoints = scene::components::selected(id, ComponentMode::Vertex);

        // Depth-tested so components behind the surface stay hidden. These streams take linear colors.
        if (mode == ComponentMode::Face)
            for (uint32_t f : selFaces)
                if (m.faceValid(f) && vis->face(f))
                    faceLines(f, faceFillLinear, hotLinear);
        if (mode == ComponentMode::Edge)
            for (uint32_t e : selEdges)
                if (m.edgeValid(e) && vis->edge(e))
                    edgeLines(e, s.selectedEdges, hotLinear);
        if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh))
        {
            const glm::vec4 ghost(glm::vec3(hotLinear), kMirrorAlpha);
            if (mode == ComponentMode::Face)
                for (uint32_t f : sym->expandFaces(m, selFaces))
                    if (f != geo::kInvalid && m.faceValid(f) && vis->face(f) && !std::ranges::binary_search(selFaces, f))
                        faceLines(f, glm::vec4(glm::vec3(faceFillLinear), faceFillLinear.a * kMirrorAlpha), ghost);
            if (mode == ComponentMode::Edge)
                for (uint32_t e : sym->expandEdges(m, selEdges))
                    if (e != geo::kInvalid && m.edgeValid(e) && vis->edge(e) && !std::ranges::binary_search(selEdges, e))
                        edgeLines(e, s.selectedEdges, ghost);
        }
        if (showSymmetry)
        {
            for (uint32_t e : symmetryPlaneEdges(*n, mesh))
                if (vis->edge(e) && !(mode == ComponentMode::Edge && std::ranges::binary_search(selEdges, e)))
                    edgeLines(e, s.symmetryLines, kSymmetryRed);
            radialPlaneLines(*n, m, world, s.lines);
            // Radial symmetry's mismatch: the points without a full orbit, marked where they are.
            if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh); sym && sym->isRadial())
            {
                const ViewInfo view = viewInfo();
                const glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]), up(view.view[0][1], view.view[1][1], view.view[2][1]);
                for (geo::Index p : sym->unmatchedPoints(m))
                {
                    if (!vis->point(p))
                        continue;
                    const glm::dvec3 c(wp(p));
                    const double half = 0.5 * thickness.vertexSelected * view.unitsPerPixel(c);
                    const glm::dvec3 r = right * half, u = up * half;
                    const glm::vec3 a(c - r - u), b(c + r - u), cc(c + r + u), d(c - r + u);
                    for (const glm::vec3& v : {a, b, cc, a, cc, d})
                        s.tris3d.push_back({v, kSymmetryRed});
                }
            }
        }
        bool soft = PrefStore::get().value<bool>("general", "/softSelect/enabled", false);
        if (mode == ComponentMode::Vertex && !soft)
        {
            render::PointDraw dots;
            dots.cloud = vertexDots(id, shape, mesh, vis, viewSymmetry(*n, mesh), selPoints);
            dots.colors[render::PointDraw::Normal] = dotLinear;
            dots.colors[render::PointDraw::Selected] = hotLinear;
            dots.colors[render::PointDraw::Mirror] = glm::mix(dotLinear, hotLinear, kMirrorAlpha);
            dots.size = float(thickness.vertex * devicePixelRatioF());
            dots.selectedSize = float(thickness.vertexSelected * devicePixelRatioF());
            s.points.push_back(std::move(dots));
            continue;
        }
        std::vector<float> weights;
        if (soft)
        {
            const auto& sel = scene::components::selected(id, mode);
            weights = scene::softWeights(m, world, scene::components::convert(m, mode, sel, ComponentMode::Vertex));
        }
        const bool edgeFaceVertices =
            mode != ComponentMode::Vertex && PrefStore::get().value<bool>("general", viewportDisplay::kVerticesInEdgeFaceModes, false);
        if (mode == ComponentMode::Vertex || soft || edgeFaceVertices)
        {
            // 1 selected, 2 the mirror of a selected point.
            std::vector<char> on(m.pointCapacity(), 0);
            if (mode == ComponentMode::Vertex)
            {
                if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh))
                    for (uint32_t p : sym->expandPoints(selPoints))
                        if (p < on.size())
                            on[p] = 2;
                for (uint32_t p : selPoints)
                    if (p < on.size())
                        on[p] = 1;
            }
            else if (edgeFaceVertices)
                for (uint32_t p : scene::components::convert(m, mode, scene::components::selected(id, mode), ComponentMode::Vertex))
                    if (p < on.size())
                        on[p] = 1;
            // Camera-facing squares a few pixels wide; size scales with depth so they stay constant on screen.
            ViewInfo view = viewInfo();
            glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]), up(view.view[0][1], view.view[1][1], view.view[2][1]);
            glm::dvec3 fwd = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
            glm::dvec3 centre = glm::dvec3(world * glm::dvec4(m.bounds().center(), 1.0));
            double depth0 = std::max(1e-6, glm::dot(centre - view.eye, fwd));
            double perUnitDepth = view.unitsPerPixel(centre) / depth0;
            const bool grayWeights = viewportDisplay::grayscaleWeights();
            for (uint32_t p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p) || !vis->point(p))
                    continue;
                glm::dvec3 c(wp(p));
                double depth = glm::dot(c - view.eye, fwd);
                if (depth <= 0.0)
                    continue;
                float wgt = p < weights.size() ? weights[p] : 0.0f;
                // Outside vertex mode only the falloff shows, as dots shaded by weight, and the selection's own vertices by pref.
                if (mode != ComponentMode::Vertex && (wgt >= 1.0f ? !edgeFaceVertices : (wgt <= 0.0f && on[p] != 1)))
                    continue;
                double half = 0.5 * (on[p] == 1 || wgt >= 1.0f ? thickness.vertexSelected : thickness.vertex) * perUnitDepth * depth;
                glm::dvec3 r = right * half, u = up * half;
                glm::vec4 col = on[p] == 1 ? hotLinear
                                : wgt > 0.0f ? viewportDisplay::falloffColor(wgt, grayWeights)
                                : on[p] == 2 ? glm::mix(dotLinear, hotLinear, kMirrorAlpha)
                                             : dotLinear;
                glm::vec3 a(c - r - u), b(c + r - u), cc(c + r + u), d(c - r + u);
                for (const glm::vec3& v : {a, b, cc, a, cc, d})
                    s.tris3d.push_back({v, col});
            }
        }
    }
}

void ViewportWidget::queuePreselect(QPointF at)
{
    mPreselectAt = at;
    if (mPreselectQueued)
        return;
    mPreselectQueued = true;
    // A slow pick (dense mesh) waits out its own cost, so hovering never takes more than half the main thread.
    QTimer::singleShot(int(std::min(50.0, mPreselectCostMs)), this, [this] {
        mPreselectQueued = false;
        updatePreselect();
    });
}

void ViewportWidget::updatePreselect()
{
    LILA_ZONE();

    if (!mPreselectAt)
        return;
    QPointF at = *mPreselectAt;
    mPreselectAt.reset();
    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    const char* pref = mode == ComponentMode::Object ? "/viewport/preselect/objects" : "/viewport/preselect/components";
    scene::componentPick::Hover next{mode};
    QElapsedTimer timer;
    timer.start();
    Gizmo* gz = currentGizmo();
    bool busy = (gz && (gz->hovering() || gz->dragging())) || viewportTools::active() || mNav != Nav::None || mSelecting;
    const selectionHandlers::Handler* handler = mode == ComponentMode::Object && !busy ? selectionHandlers::active() : nullptr;
    std::optional<selectionHandlers::Highlight> highlight;
    if (handler && handler->hover)
    {
        selectionHandlers::Hit hit = selectionHit(at);
        try
        {
            highlight = handler->hover(hit);
        }
        catch (const std::exception& e)
        {
            log::error("selection handler {}: {}", handler->id, e.what());
        }
        if (PrefStore::get().value<bool>("general", pref, false))
            next.node = hit.node;
    }
    else if (!busy && PrefStore::get().value<bool>("general", pref, mode != ComponentMode::Object))
    {
        if (mode != ComponentMode::Object)
            next = componentAt(at);
        else if (std::vector<std::string> names = hitTest(at, at); !names.empty())
            if (scene::Node* n = findNode(names.front()))
                next.node = n->id();
    }
    mPreselectCostMs = double(timer.nsecsElapsed()) / 1e6;
    const bool highlightChanged = !sameHighlight(highlight, mHandlerHover);
    mHandlerHover = std::move(highlight);
    if (next == mPreselect && !highlightChanged)
        return;
    mPreselect = next;
    requestFrame();
}

void ViewportWidget::clearPreselect()
{
    mPreselectAt.reset();
    if (mPreselect.empty() && !mHandlerHover)
        return;
    mPreselect = {};
    mHandlerHover.reset();
    requestFrame();
}

selectionHandlers::Hit ViewportWidget::selectionHit(QPointF px)
{
    selectionHandlers::Hit hit;
    hit.px = px;
    std::vector<std::string> names = hitTest(px, px);
    if (names.empty())
        return hit;
    hit.name = names.front();
    scene::Node* n = findNode(hit.name);
    if (!n)
        return hit;
    hit.node = n->id();
    if (!n->type().isA("mesh") || overridden(*n))
        return hit;
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    auto rm = renderMeshFor(mesh);
    auto pd = pickDataFor(mesh);
    if (!rm || !pd)
        return hit;
    const glm::dmat4 world = shownWorld(*n);
    glm::dvec3 o, d;
    viewInfo().ray(px, o, d);
    std::optional<RayHit> ray = isolate::raycast(&mIsolation, hit.node, mesh, world, o, d);
    if (!ray || ray->triangle >= pd->triangleFaces.size() || 3 * size_t(ray->triangle) + 2 >= pd->indices.size())
        return hit;
    hit.face = pd->triangleFaces[ray->triangle];
    hit.position = o + d * ray->distance;
    double nearest = 1e300;
    for (int k = 0; k < 3; ++k)
    {
        uint32_t v = pd->indices[3 * size_t(ray->triangle) + k];
        if (v >= pd->positions.size() || v >= pd->vertexPoint.size())
            continue;
        double dist = glm::length(glm::dvec3(world * glm::dvec4(glm::dvec3(pd->positions[v]), 1.0)) - hit.position);
        if (dist < nearest)
        {
            nearest = dist;
            hit.point = pd->vertexPoint[v];
        }
    }
    return hit;
}

void ViewportWidget::drawHandlerHover(render::FrameState& s)
{
    if (!mHandlerHover || scene::components::mode() != scene::ComponentMode::Object)
        return;
    using Domain = selectionHandlers::Highlight::Domain;
    const selectionHandlers::Highlight& h = *mHandlerHover;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(h.mesh);
    if (!n || !n->type().isA("mesh"))
        return;
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh)
        return;
    const geo::Mesh& m = *mesh;
    const glm::dmat4 world = shownWorld(*n);
    const glm::vec4 color = srgbToLinear(h.color);
    auto weight = [&](uint32_t i) { return i < h.weights.size() ? std::clamp(h.weights[i], 0.0f, 1.0f) : 0.0f; };
    if (!mIsolation.shows(h.mesh))
        return;
    const auto vis = isolate::visibility(&mIsolation, h.mesh, mesh);
    for (uint32_t f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || !vis->face(f) || (h.domain == Domain::Face && weight(f) <= 0.0f))
            continue;
        std::vector<uint32_t> tris = scene::componentPick::fillTriangles(m, f);
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            float w[3];
            for (int k = 0; k < 3; ++k)
                w[k] = h.domain == Domain::Face ? weight(f) : weight(tris[t + k]);
            if (w[0] <= 0.0f && w[1] <= 0.0f && w[2] <= 0.0f)
                continue;
            for (int k = 0; k < 3; ++k)
                s.tris3d.push_back({glm::vec3(world * glm::dvec4(m.position(tris[t + k]), 1.0)), glm::vec4(glm::vec3(color), color.a * w[k])});
        }
    }
}

namespace
{
/** Linear color of a topology callout, or nullopt when its pref is off or it is plain. */
std::optional<glm::vec4> topologyTint(scene::componentPick::Topology t)
{
    namespace cp = scene::componentPick;
    if (t == cp::Topology::Plain || !PrefStore::get().value<bool>("general", cp::topologyEnabledKey(t), true))
        return std::nullopt;
    QColor c(QString::fromStdString(PrefStore::get().value<std::string>("general", cp::topologyColorKey(t), cp::topologyDefaultColor(t))));
    if (!c.isValid())
        c = QColor(cp::topologyDefaultColor(t));
    auto lin = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
    return glm::vec4(lin(c.redF()), lin(c.greenF()), lin(c.blueF()), 1.0f);
}
} // namespace

void ViewportWidget::drawPreselect(render::FrameState& s)
{
    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    if (mPreselect.empty() || mPreselect.mode != mode || mode == ComponentMode::Object)
        return;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(mPreselect.node);
    scene::MeshPtr mesh = n ? scene::modifiers::cage(g, mPreselect.node) : nullptr;
    if (!mesh)
        return;
    const geo::Mesh& m = *mesh;
    uint32_t id = mPreselect.id;
    if ((mode == ComponentMode::Vertex && !m.pointValid(id)) || (mode == ComponentMode::Edge && !m.edgeValid(id)) ||
        (mode == ComponentMode::Face && !m.faceValid(id)))
        return;
    const ComponentShape shape(*n, m, mesh, shownWorld(*n));
    auto wp = [&](uint32_t p) { return shape.point(p); };
    ViewInfo view = viewInfo();
    glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]), up(view.view[0][1], view.view[1][1], view.view[2][1]);
    glm::dvec3 fwd = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
    // Thicker than the selection, so hovered and selected read apart.
    const ComponentPalette palette = componentPalette();
    glm::vec4 hot = palette.hover, faceFill = palette.hoverFill;
    const viewportDisplay::ComponentThickness thickness = viewportDisplay::componentThickness();
    namespace cp = scene::componentPick;
    cp::Topology topo = mode == ComponentMode::Face ? cp::faceTopology(m, id) : mode == ComponentMode::Vertex ? cp::pointTopology(m, id) : cp::Topology::Plain;
    if (std::optional<glm::vec4> tint = topologyTint(topo))
    {
        hot = *tint;
        faceFill = glm::vec4(glm::vec3(*tint), 0.3f);
    }
    auto quad = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d, const glm::vec4& col) {
        for (const glm::dvec3& v : {a, b, c, a, c, d})
            s.tris3d.push_back({glm::vec3(v), col});
    };
    // Screen-constant width, facing the camera.
    auto ribbon = [&](glm::dvec3 p0, glm::dvec3 p1, double halfPx, const glm::vec4& col) {
        glm::dvec3 side = glm::cross(p1 - p0, fwd);
        if (glm::length(side) < 1e-12)
            return;
        side = glm::normalize(side);
        glm::dvec3 s0 = side * (halfPx * view.unitsPerPixel(p0)), s1 = side * (halfPx * view.unitsPerPixel(p1));
        quad(p0 - s0, p1 - s1, p1 + s1, p0 + s0, col);
    };
    auto square = [&](glm::dvec3 c, double halfPx, const glm::vec4& col) {
        double half = halfPx * view.unitsPerPixel(c);
        glm::dvec3 r = right * half, u = up * half;
        quad(c - r - u, c + r - u, c + r + u, c - r + u, col);
    };
    auto component = [&](uint32_t id, float alpha) {
        const glm::vec4 col(glm::vec3(hot), hot.a * alpha), fill(glm::vec3(faceFill), faceFill.a * alpha);
        if (mode == ComponentMode::Vertex)
            square(wp(id), 0.5 * thickness.vertexHover, col);
        else if (mode == ComponentMode::Edge)
            shape.forEachSegment(id, [&](const glm::dvec3& a, const glm::dvec3& b) { ribbon(a, b, 0.5 * thickness.edgeHover, col); });
        else
        {
            shape.forEachFillVertex(id, [&](const glm::dvec3& p) { s.tris3d.push_back({glm::vec3(p), fill}); });
            shape.forEachOutline(id, [&](const glm::dvec3& a, const glm::dvec3& b) { ribbon(a, b, 1.0, col); });
        }
    };
    if (const geo::SymmetryMap* sym = viewSymmetry(*n, mesh))
    {
        const auto vis = isolate::visibility(&mIsolation, mPreselect.node, mesh);
        std::vector<uint32_t> drawn{id};
        for (int k = 1; k < sym->order(); ++k)
        {
            const uint32_t mirror = mode == ComponentMode::Vertex ? sym->image(k, id)
                                    : mode == ComponentMode::Edge  ? sym->imageEdge(m, k, id)
                                                                   : sym->imageFace(m, k, id);
            if (mirror == geo::kInvalid || std::ranges::count(drawn, mirror))
                continue;
            const bool shown = mode == ComponentMode::Vertex ? m.pointValid(mirror) && vis->point(mirror)
                               : mode == ComponentMode::Edge ? m.edgeValid(mirror) && vis->edge(mirror)
                                                             : m.faceValid(mirror) && vis->face(mirror);
            if (shown)
                component(mirror, kMirrorAlpha);
            drawn.push_back(mirror);
        }
    }
    component(id, 1.0f);
    // Edge hover calls out the poles at its ends.
    if (mode == ComponentMode::Edge)
        if (std::optional<glm::vec4> pole = topologyTint(cp::Topology::Pole))
            for (int k = 0; k < 2; ++k)
                if (cp::pointTopology(m, m.edgePoint(id, k)) == cp::Topology::Pole)
                    square(wp(m.edgePoint(id, k)), 3.5, *pole);
}

bool ViewportWidget::touchesSelectRegion(const QRectF& box, const std::vector<glm::dvec2>& shape) const
{
    if (mLassoPick)
        return geo::lasso::touches(mLasso, shape);
    return geo::lasso::touches({{box.left(), box.top()}, {box.right(), box.top()}, {box.right(), box.bottom()}, {box.left(), box.bottom()}}, shape);
}

bool ViewportWidget::inSelectRegion(const QRectF& box, QPointF p) const
{
    return mLassoPick ? geo::lasso::contains(mLasso, {p.x(), p.y()}) : box.contains(p);
}

void ViewportWidget::pickLasso(Qt::KeyboardModifiers mods)
{
    auto [lo, hi] = geo::lasso::bounds(mLasso);
    if (mLasso.size() < 3 || (hi.x - lo.x < 4 && hi.y - lo.y < 4))
    {
        pick(mPressPos, mPressPos, mods);
        return;
    }
    mLassoPick = true;
    pick(QPointF(lo.x, lo.y), QPointF(hi.x, hi.y), mods);
    mLassoPick = false;
}

std::optional<bool> ViewportWidget::selectedUnder(QPointF at)
{
    scene::Graph& g = scene::Graph::current();
    scene::ComponentMode mode = scene::components::mode();
    if (mode != scene::ComponentMode::Object)
    {
        scene::componentPick::Hover h = componentAt(at);
        if (h.empty())
            return std::nullopt;
        const scene::components::Ids& sel = scene::components::selected(h.node, mode);
        return std::ranges::find(sel, h.id) != sel.end();
    }
    std::vector<std::string> hits = hitTest(at, at);
    const scene::Node* n = hits.empty() ? nullptr : findNode(hits.front());
    if (!n)
        return std::nullopt;
    return std::ranges::find(g.selection(), n->id()) != g.selection().end();
}

void ViewportWidget::updateTabHover(QPointF at)
{
    std::optional<bool> under = rect().contains(at.toPoint()) ? selectedUnder(at) : std::nullopt;
    if (under != mTabUnder)
        requestFrame();
    mTabUnder = under;
}

void ViewportWidget::pick(QPointF a, QPointF b, Qt::KeyboardModifiers mods)
{
    if (scene::components::mode() != scene::ComponentMode::Object)
    {
        pickComponents(a, b, mods);
        return;
    }
    json hits = hitTest(a, b);
    if (selectionHandlers::anyRedirect())
    {
        QRectF box = QRectF(a, b).normalized();
        std::vector<selectionHandlers::Hit> asked;
        if (box.width() < 4 && box.height() < 4)
        {
            if (!hits.empty())
                asked.push_back(selectionHit(a));
        }
        else
            for (const std::string& name : hits.get<std::vector<std::string>>())
            {
                selectionHandlers::Hit h;
                h.name = name;
                if (scene::Node* n = findNode(name))
                    h.node = n->id();
                h.px = (a + b) * 0.5;
                h.click = false;
                asked.push_back(std::move(h));
            }
        hits = selectionHandlers::redirect(asked);
    }

    // Maya: Shift toggles, Ctrl removes, Ctrl+Shift adds.
    bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    json call = {{"args", hits}};
    if (shift && ctrl)
        call["add"] = true;
    else if (ctrl)
        call["deselect"] = true;
    else if (shift)
        call["toggle"] = true;
    else if (hits.empty())
        call = {{"clear", true}};
    if (hits.empty() && (shift || ctrl))
        return;
    try
    {
        ops::execute("select", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void ViewportWidget::setView(const std::string& name)
{
    if (name == mViewName)
        return;
    if (!builtInView(name))
    {
        if (scene::Node* n = findNode(name); n && n->type().isA("camera"))
            lookThrough(n->id());
        return;
    }
    commitCameraEdit();
    if (mViewName == "persp" && mLookThrough == scene::kNoNode)
        mPerspCamera = mCamera;
    if (mLookThrough != scene::kNoNode)
    {
        mLookThrough = scene::kNoNode;
        mCamera.leaveLookThrough();
    }
    if (name == "persp")
    {
        bool zUp = mCamera.zUp;
        mCamera = mPerspCamera;
        mCamera.zUp = zUp;
        mCamera.ortho = false;
    }
    else
    {
        glm::dvec3 target = mCamera.target;
        double width = mCamera.ortho ? mCamera.orthoWidth : 2.0 * mCamera.distance * (mCamera.horizontalAperture * 0.5) / mCamera.focalLength;
        mCamera.setView(name);
        mCamera.target = target;
        mCamera.orthoWidth = std::max(width, 1.0);
    }
    mViewName = name;
    syncCameraSettings();
    requestFrame();
    Q_EMIT viewStateChanged();
}

nlohmann::json ViewportWidget::cameraState() const
{
    const bool through = mLookThrough != scene::kNoNode;
    const std::string& view = through ? mReturnView : mViewName;
    const bool persp = view == "persp";
    nlohmann::json j{{"view", view}, {"persp", (persp && !through ? mCamera : mPerspCamera).poseJson()}};
    if (!persp && !through)
        j["camera"] = mCamera.poseJson();
    return j;
}

void ViewportWidget::restoreCameraState(const nlohmann::json& state)
{
    if (!state.is_object())
        return;
    std::string view = state.value("view", std::string("persp"));
    if (!builtInView(view))
        return;
    if (mLookThrough != scene::kNoNode)
        stopLookingThrough();
    commitCameraEdit();
    mPerspCamera.setPose(state.value("persp", nlohmann::json()));
    if (view == "persp")
    {
        bool zUp = mCamera.zUp;
        mCamera = mPerspCamera;
        mCamera.zUp = zUp;
        mCamera.ortho = false;
    }
    else
    {
        mCamera.setView(view);
        mCamera.setPose(state.value("camera", nlohmann::json()));
    }
    mViewName = view;
    syncCameraSettings();
    requestFrame();
    Q_EMIT viewStateChanged();
}

void ViewportWidget::lookThrough(scene::NodeId camera)
{
    scene::Node* n = scene::Graph::current().node(camera);
    if (!n || !n->type().isA("camera") || camera == mLookThrough)
        return;
    commitCameraEdit();
    if (mLookThrough == scene::kNoNode)
    {
        mReturnView = mViewName;
        if (mViewName == "persp")
            mPerspCamera = mCamera;
    }
    mLookThrough = camera;
    mViewName = n->path();
    syncLookThrough();
    syncCameraSettings();
    requestFrame();
    Q_EMIT viewStateChanged();
}

void ViewportWidget::syncLookThrough()
{
    if (mLookThrough == scene::kNoNode)
        return;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(mLookThrough);
    if (!n || !n->type().isA("camera"))
    {
        mCameraEdit.reset();
        setView(mReturnView);
        return;
    }
    g.evaluate();
    mCamera.lookThrough(*n, n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    bool renamed = n->path() != mViewName;
    mViewName = n->path();
    // The gate settings are the node's attributes, which can change under any edit.
    syncCameraSettings();
    if (renamed)
        Q_EMIT viewStateChanged();
}

bool ViewportWidget::beginCameraEdit()
{
    scene::Node* n = scene::Graph::current().node(mLookThrough);
    if (!n)
        return false;
    if (n->as<bool>(scene::Camera::kLocked))
    {
        // One message per gesture, not one per wheel click.
        if (double now = nowMs(); now - mLockedWarnMs > 1500.0)
        {
            mLockedWarnMs = now;
            log::warn("Camera '{}' is locked; unlock it (locked attribute) to move it through the view.", n->path());
        }
        return false;
    }
    if (mCameraEdit && mCameraEdit->id == mLookThrough)
        return true;
    commitCameraEdit();
    CameraEdit edit{mLookThrough, {}};
    for (int attr : kEditedAttrs)
        edit.saved.emplace_back(attr, n->value(attr));
    mCameraEdit = std::move(edit);
    return true;
}

void ViewportWidget::previewCameraEdit()
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = mCameraEdit ? g.node(mCameraEdit->id) : nullptr;
    if (!n)
        return;
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    try
    {
        g.evaluate();
        glm::dmat4 placed = scene::camera::placedWorld(n->as<glm::dmat4>(scene::Transform::kWorldMatrix), mCamera.eye(), mCamera.target,
                                                       mCamera.viewUp());
        scene::Transform::setLocalMatrix(g, n->id(), glm::inverse(n->as<glm::dmat4>(scene::Transform::kParentMatrix)) * placed);
        if (mCamera.ortho)
            g.setValue({n->id(), scene::Camera::kOrthographicWidth, -1}, mCamera.orthoWidth);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
    u.setEnabled(was);
}

void ViewportWidget::commitCameraEdit()
{
    mWheelCommit->stop();
    if (!mCameraEdit)
        return;
    CameraEdit edit = std::move(*mCameraEdit);
    mCameraEdit.reset();
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(edit.id);
    if (!n)
        return;
    std::vector<int> changed;
    for (auto& [attr, value] : edit.saved)
        if (n->value(attr) != value)
            changed.push_back(attr);
    if (changed.empty())
        return;
    io::markModified();
    if (!scene::anim::autoKey())
        return;
    // As Maya's Auto Key: only channels that already have keys get one.
    try
    {
        UndoChunk chunk("Auto Key Camera");
        for (const scene::anim::AnimatedPlug& a : scene::anim::animatedPlugs(g, edit.id))
        {
            if (std::ranges::find(changed, a.plug.attr) == changed.end())
                continue;
            const scene::PlugValue& v = n->value(a.plug.attr);
            double value = a.plug.component >= 0 && std::holds_alternative<glm::dvec3>(v) ? std::get<glm::dvec3>(v)[a.plug.component]
                           : std::holds_alternative<double>(v)                         ? std::get<double>(v)
                                                                                        : 0.0;
            ops::execute("setKeyframe", {{"args", {g.plugName(a.plug)}}, {"value", value}}, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

scene::NodeId ViewportWidget::droppedCamera(const QMimeData* mime) const
{
    if (!mime || !mime->hasFormat(kNodeNamesMime))
        return scene::kNoNode;
    for (const QString& name : QString::fromUtf8(mime->data(kNodeNamesMime)).split(QChar('\n'), Qt::SkipEmptyParts))
        if (scene::Node* n = findNode(name.toStdString()); n && n->type().isA("camera"))
            return n->id();
    return scene::kNoNode;
}

viewportHooks::DropTarget ViewportWidget::dropTarget(QPointF px)
{
    const selectionHandlers::Hit hit = selectionHit(px);
    viewportHooks::DropTarget t;
    t.node = hit.node;
    t.face = hit.face;
    t.position = hit.position;
    t.view = viewInfo();
    t.px = px;
    return t;
}

void ViewportWidget::dragLeaveEvent(QDragLeaveEvent* e)
{
    // Only handlers that drew a preview care; there is no mime data to offer by now.
    viewportHooks::DropTarget t;
    t.view = viewInfo();
    t.left = true;
    static const QMimeData kEmpty;
    viewportHooks::handleDrop(kEmpty, t, false);
    e->accept();
}

void ViewportWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (droppedCamera(e->mimeData()) == scene::kNoNode)
    {
        // Hooks get the move events too: entering only asks whether any of them wants this kind of drag.
        viewportHooks::DropTarget none;
        none.view = viewInfo();
        none.px = e->position();
        if (!e->mimeData() || !viewportHooks::handleDrop(*e->mimeData(), none, false))
            return e->ignore();
        e->setDropAction(Qt::CopyAction);
        return e->accept();
    }
    e->setDropAction(Qt::LinkAction);
    e->accept();
}

void ViewportWidget::dragMoveEvent(QDragMoveEvent* e)
{
    if (droppedCamera(e->mimeData()) == scene::kNoNode)
    {
        if (!e->mimeData() || !viewportHooks::handleDrop(*e->mimeData(), dropTarget(e->position()), false))
            return e->ignore();
        e->setDropAction(Qt::CopyAction);
        return e->accept();
    }
    e->setDropAction(Qt::LinkAction);
    e->accept();
}

void ViewportWidget::dropEvent(QDropEvent* e)
{
    scene::NodeId cam = droppedCamera(e->mimeData());
    if (cam == scene::kNoNode)
    {
        if (!e->mimeData() || !viewportHooks::handleDrop(*e->mimeData(), dropTarget(e->position()), true))
            return e->ignore();
        e->setDropAction(Qt::CopyAction);
        e->accept();
        setFocus();
        return;
    }
    e->setDropAction(Qt::LinkAction);
    e->accept();
    // Focus makes this the active view, as a click would.
    setFocus();
    lookThrough(cam);
}

void ViewportWidget::syncCameraSettings()
{
    const scene::RenderSettings& r = scene::renderSettings();
    scene::CameraSettings c = r.camera(mViewName);
    mCamera.filmFit = c.filmFit;
    mCamera.overscan = c.overscan;
    mCamera.gateAspect = c.resolutionGate ? std::optional<double>(r.deviceAspect()) : std::nullopt;
}

void ViewportWidget::drawGate(OverlayPainter& p)
{
    const scene::RenderSettings& r = scene::renderSettings();
    scene::CameraSettings c = r.camera(mViewName);
    if (!c.resolutionGate && !c.filmGate)
        return;
    glm::dvec2 view = mCamera.viewExtents(aspect());
    auto toRect = [&](glm::dvec2 half) {
        double w = half.x / view.x * width(), h = half.y / view.y * height();
        return QRectF((width() - w) * 0.5, (height() - h) * 0.5, w, h);
    };
    const glm::vec4 border = p.ink(0.75f, 0.9f);
    auto outline = [&](QRectF g, bool dashed) {
        QPointF pts[4] = {g.topLeft(), g.topRight(), g.bottomRight(), g.bottomLeft()};
        for (int i = 0; i < 4; ++i)
            dashed ? p.dashedLine(pts[i], pts[(i + 1) % 4], 1.0f, 6.0f, border) : p.line(pts[i], pts[(i + 1) % 4], 1.0f, border);
    };
    if (auto gate = mCamera.gateExtents(); gate && c.resolutionGate)
    {
        QRectF g = toRect(*gate);
        if (c.gateMask && c.gateMaskOpacity > 0.0)
        {
            const glm::vec4 mask(0.1f, 0.1f, 0.1f, float(c.gateMaskOpacity));
            QRectF all(0, 0, width(), height());
            QRectF in = g.intersected(all);
            p.rect(QRectF(0, 0, all.width(), in.top()), mask);
            p.rect(QRectF(0, in.bottom(), all.width(), all.bottom() - in.bottom()), mask);
            p.rect(QRectF(0, in.top(), in.left(), in.height()), mask);
            p.rect(QRectF(in.right(), in.top(), all.right() - in.right(), in.height()), mask);
        }
        outline(g, false);
        p.setShadow(true);
        QString res = QString("%1 x %2").arg(r.width).arg(r.height);
        double right = std::max(std::min(g.right(), width() - 4.0), 4.0 + OverlayPainter::textWidth(res));
        p.text(QPointF(right, std::max(g.top() - 5.0, 14.0)), res, border, OverlayPainter::Right);
        p.setShadow(false);
    }
    if (c.filmGate)
        outline(toRect(mCamera.filmExtents()), true);
}

void ViewportWidget::toggleIsolate()
{
    if (mIsolation.active())
        mIsolation.clear();
    else
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        mIsolation = scene::isolation::fromSelection(g);
    }
    setIsolation(std::move(mIsolation));
}

void ViewportWidget::addToIsolation()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::isolation::addSelection(mIsolation, g);
    setIsolation(std::move(mIsolation));
}

void ViewportWidget::removeFromIsolation()
{
    if (!mIsolation.active())
        return;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::isolation::removeSelection(mIsolation, g);
    setIsolation(std::move(mIsolation));
}

void ViewportWidget::setIsolation(scene::isolation::Set set)
{
    mIsolation = std::move(set);
    clearPreselect();
    requestFrame();
    Q_EMIT viewStateChanged();
}

render::Renderer::FrameStats ViewportWidget::frameStats() const
{
    return mId ? render::Renderer::get().stats(mId) : render::Renderer::FrameStats{};
}

void ViewportWidget::drawHud(OverlayPainter& p)
{
    double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    render::Renderer::FrameStats stats = render::Renderer::get().stats(mId);
    mFrameSamples.emplace_back(now, stats.frames);
    while (now - mFrameSamples.front().first > 1000.0)
        mFrameSamples.pop_front();
    hud::Context ctx;
    ctx.panel = mHudPanel;
    ctx.view = viewInfo();
    if (mIsolation.active())
        ctx.isolation = &mIsolation;
    // Counts cover what this view draws, so they skip the same nodes the mesh pass does.
    ctx.shownNode = [this, lodHidden = scene::lod::hidden(scene::Graph::current(), lodDisplay::view(mHudPanel, ctx.view))](scene::NodeId id) {
        const scene::Node* n = scene::Graph::current().node(id);
        return n && mIsolation.shows(id) && !lodHidden.contains(id) && scene::selectionMask::shown(*n) && !viewportHooks::hiddenInView(id);
    };
    // Frames the renderer drew, not states submitted: mouse moves submit far more often than the display can show.
    double span = mFrameSamples.back().first - mFrameSamples.front().first;
    ctx.framesPerSecond = span > 100.0 ? double(mFrameSamples.back().second - mFrameSamples.front().second) * 1000.0 / span : 0.0;
    ctx.gpuMs = stats.gpuMs;
    ctx.aoMs = stats.aoMs;
    std::vector<QRectF> drawn;
    ctx.drawn = &drawn;
    // A provisional uv count settles on the next frame once the mesh stops changing, which needs a frame to happen.
    if (hud::draw(p, ctx) && !mHudRefreshQueued)
    {
        mHudRefreshQueued = true;
        QTimer::singleShot(200, this, [this] {
            mHudRefreshQueued = false;
            requestFrame();
        });
    }
    flushToolMessages(p, ctx.view, drawn);
}

void ViewportWidget::drawViewIndicator(OverlayPainter& p)
{
    // Axis triad in the bottom-left corner plus the view's name, Maya style. Letters tell the axes apart in monotone.
    glm::dmat3 v(mCamera.view());
    QPointF origin(36, height() - 36);
    const QRectF corner(0, height() - 72, 72, 72);
    bool axes = hud::shown(mHudPanel, "viewAxis");
    bool colored = viewportDisplay::accentColors(viewportDisplay::kViewAxisColors);
    p.setShadow(true);
    for (int axis = 0; axes && axis < 3; ++axis)
    {
        glm::dvec3 d = v * glm::dvec3(axis == 0, axis == 1, axis == 2);
        QPointF tip = origin + QPointF(d.x, -d.y) * 22.0;
        // Pointing away from the viewer (view -Z) reads dimmer.
        glm::vec4 c = colored ? glm::vec4(viewportDisplay::axisColor(axis) * (d.z < 0.0 ? 0.6f : 1.0f), 1.0f)
                              : p.ink(d.z < 0.0 ? 0.45f : 0.9f, 1.0f, corner);
        p.line(origin, tip, 1.5f, c);
        p.text(tip + QPointF(d.x, -d.y) * 8.0 + QPointF(0, 4), QString(QChar('X' + axis)), c, OverlayPainter::Center);
    }
    if (hud::shown(mHudPanel, "cameraNames"))
    {
        QString name = QString::fromStdString(mViewName);
        double w = OverlayPainter::textWidth(name);
        QRectF under((width() - w) / 2.0, height() - 24, w, 16);
        p.text(QPointF(width() / 2.0, height() - 12), name, p.ink(0.75f, 0.9f, under), OverlayPainter::Center);
    }
    p.setShadow(false);
}

std::string ViewportWidget::skinnedMeshAt(QPointF at)
{
    scene::Graph& g = scene::Graph::current();
    for (const std::string& name : hitTest(at, at))
        if (scene::Node* n = findNode(name); n && n->type().isA("mesh") && scene::skin::find(g, n->id()) != scene::kNoNode)
            return name;
    return {};
}

void ViewportWidget::frameSelection(bool all)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    if (mLookThrough != scene::kNoNode)
    {
        syncLookThrough();
        if (!beginCameraEdit())
            return;
    }
    CameraView before = cameraView();
    std::vector<scene::NodeId> ids = all ? g.nodes() : g.selection();
    const bool inherit = scene::inheritVisibility();
    glm::dvec3 lo(1e30), hi(-1e30);
    int count = 0;
    if (!all && scene::components::mode() != scene::ComponentMode::Object)
    {
        for (const auto& [id, picked] : scene::components::selected(scene::components::mode()))
        {
            scene::Node* n = g.node(id);
            if (!n || picked.empty())
                continue;
            if (scene::components::pointProvider(*n))
            {
                std::unordered_set<uint32_t> want(picked.begin(), picked.end());
                for (auto& [cid, w] : worldPoints(g, id))
                    if (want.contains(cid))
                    {
                        lo = glm::min(lo, w);
                        hi = glm::max(hi, w);
                        ++count;
                    }
                continue;
            }
            const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            if (!mesh)
                continue;
            glm::dmat4 world = shownWorld(*n);
            for (uint32_t p : scene::components::pointsOf(id))
                if (mesh->pointValid(p))
                {
                    glm::dvec3 w(world * glm::dvec4(mesh->position(p), 1.0));
                    lo = glm::min(lo, w);
                    hi = glm::max(hi, w);
                    ++count;
                }
        }
        if (count > 0)
        {
            // A lone vertex has no extent; keep some context around it like Maya does.
            double radius = std::max(glm::length(hi - lo) * 0.5, 5.0);
            mCamera.frame((lo + hi) * 0.5, radius, aspect());
            if (mLookThrough != scene::kNoNode)
                previewCameraEdit();
            recordCamera(before);
            commitCameraEdit();
            requestFrame();
            return;
        }
    }
    auto addGeometry = [&](scene::Node* n) {
        if (scene::components::pointProvider(*n))
        {
            auto pts = worldPoints(g, n->id());
            for (auto& [cid, w] : pts)
            {
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
            return !pts.empty();
        }
        if (!n->type().isA("mesh"))
            return false;
        auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
        if (!rm || rm->vertices.empty())
            return false;
        const glm::dmat4 world = shownWorld(*n);
        std::vector<glm::dmat4> placed{world};
        for (const scene::arrays::Copy& c : scene::arrays::copiesOf(g, n->id()))
            placed.push_back(c.transform * world);
        for (const glm::dmat4& at : placed)
            for (int c = 0; c < 8; ++c)
            {
                glm::dvec3 corner((c & 1) ? rm->boundsMax.x : rm->boundsMin.x, (c & 2) ? rm->boundsMax.y : rm->boundsMin.y,
                                  (c & 4) ? rm->boundsMax.z : rm->boundsMin.z);
                glm::dvec3 w(at * glm::dvec4(corner, 1.0));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
        return true;
    };
    bool group = false;
    for (scene::NodeId id : ids)
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().dag || id == mLookThrough || (all && !scene::effectivelyVisible(g, id, inherit)))
            continue;
        bool geometry = addGeometry(n);
        if (!all)
            for (scene::NodeId d : g.descendants(id))
                if (scene::Node* dn = g.node(d); dn && dn->type().dag && scene::effectivelyVisible(g, d, inherit) && addGeometry(dn))
                {
                    group = group || !n->type().isA("mesh");
                    geometry = true;
                }
        // Only geometry decides the framing; a bare pivot counts when nothing under it has any.
        if (!geometry)
        {
            glm::dvec3 p(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
            lo = glm::min(lo, p - 1.0);
            hi = glm::max(hi, p + 1.0);
        }
        ++count;
    }
    if (count == 0)
    {
        lo = glm::dvec3(-50.0);
        hi = glm::dvec3(50.0);
    }
    // Maya frames a group from further out than a single mesh.
    double radius = glm::length(hi - lo) * 0.5 * (group ? 2.0 : 1.0);
    mCamera.frame((lo + hi) * 0.5, radius, aspect());
    if (mLookThrough != scene::kNoNode)
        previewCameraEdit();
    recordCamera(before);
    commitCameraEdit();
    requestFrame();
}

void ViewportWidget::mousePressEvent(QMouseEvent* e)
{
    clearPreselect();
    mLastPos = e->position();
    setFocus();
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier) && viewportHooks::handleClick(e->position(), viewInfo()))
        return;
    if (mTabHeld && !viewportTools::active() && e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier))
    {
        tabSelect::Mode m = tabSelect::modeAtPress(selectedUnder(e->position()), e->modifiers() & Qt::ShiftModifier,
                                                   e->modifiers() & Qt::ControlModifier);
        mTabStroke.begin(m == tabSelect::Mode::Select ? paintSelect::Stroke::Op::Add : paintSelect::Stroke::Op::Remove);
        mTabStroke.paint(viewInfo(), e->position(), e->position(), paintSelect::tabRadius(scene::components::mode()));
        submit();
        return;
    }
    // Brush tools take B + drag for their own radius.
    if (tools::softSelectAdjusting() && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) &&
        !(viewportTools::active() && viewportTools::active()->ownsBrushRadius()))
    {
        mSoftDrag = true;
        tools::markSoftSelectAdjusted();
        mPressPos = e->position();
        mSoftStartRadius = e->button() == Qt::MiddleButton ? 0.0 : PrefStore::get().value<double>("general", "/softSelect/radius", 25.0);
        if (e->button() == Qt::MiddleButton)
            PrefStore::get().set("general", "/softSelect/radius", 0.01);
        PrefStore::get().set("general", "/softSelect/enabled", true);
        return;
    }
    if (ViewportTool* tool = viewportTools::active(); tool && !(e->modifiers() & Qt::AltModifier) && tool->mousePress(e, viewInfo()))
    {
        mToolDrag = true;
        submit();
        return;
    }
    if (e->button() == Qt::RightButton && !(e->modifiers() & Qt::AltModifier))
        if (Gizmo* gz = currentGizmo())
        {
            gz->setView(viewInfo());
            if (gz->wantsRightButton(e->position()) && gz->press(e->position(), e->button(), e->modifiers()))
            {
                mGizmoDrag = gz;
                mCursorWrap.reset();
                submit();
                return;
            }
        }
    if (e->button() == Qt::RightButton && !(e->modifiers() & Qt::AltModifier))
    {
        // Maya-style: the menu depends on the modifier and on whether the cursor is over an object.
        bool overObject = !hitTest(e->position(), e->position()).empty();
        std::string base = (e->modifiers() & Qt::ShiftModifier) ? "viewportShift" : (e->modifiers() & Qt::ControlModifier) ? "viewportCtrl" : "viewport";
        QPoint at = e->globalPosition().toPoint();
        // Over a skinned mesh, a joint-picking selection handler offers the joints under the cursor instead.
        if (overObject && base == "viewport" && scene::components::mode() == scene::ComponentMode::Object)
            if (selectionHandlers::Hit hit = selectionHit(e->position()); hit.face != selectionHandlers::kNone)
                if (auto cands = selectionHandlers::candidates(hit); !cands.empty())
                {
                    json fallback = markingMenus::resolve(base + "Object");
                    if (!fallback.is_object() || fallback.empty())
                        fallback = markingMenus::resolve(base);
                    if (MarkingMenu::popupDefinition(selectionHandlers::candidateMenu(cands, fallback), MarkingMenu::Trigger::MouseButton,
                                                     Qt::RightButton, at))
                        return;
                }
        // Over a bound mesh, Maya offers Paint Skin Weights in the top left slot.
        if (overObject && base == "viewport")
            if (std::string mesh = skinnedMeshAt(e->position()); !mesh.empty())
                if (json def = markingMenus::resolve(base + "Object"); def.is_object() && !def.contains("NW"))
                {
                    def["NW"] = {{"label", "Paint Skin Weights"},
                                 {"action", {{"python", "lila.nodes.select(" + json(mesh).dump() + ")\nlila.ui.run_action(\"rig.paintWeights\")"}}}};
                    if (MarkingMenu::popupDefinition(def, MarkingMenu::Trigger::MouseButton, Qt::RightButton, at))
                        return;
                }
        bool componentMode = scene::components::mode() != scene::ComponentMode::Object;
        std::string name = markingMenus::viewportMenuName(base, overObject || !scene::Graph::current().selection().empty(), componentMode);
        json def = markingMenus::resolve(name);
        // Slots under the current mode's key replace the regular ones; its overflow rows go in front of the regular ones.
        static const char* kModeKeys[] = {"objectMode", "vertexMode", "edgeMode", "faceMode"};
        if (const json& over = def.value(kModeKeys[int(scene::components::mode())], json()); over.is_object())
            for (auto& [slot, item] : over.items())
                if (slot != "overflow")
                    def[slot] = item;
                else if (item.is_array())
                {
                    json rows = item;
                    for (const json& row : def.value("overflow", json::array()))
                        rows.push_back(row);
                    def["overflow"] = rows;
                }
        for (const char* key : kModeKeys)
            def.erase(key);
        if (!MarkingMenu::popupDefinition(def, MarkingMenu::Trigger::MouseButton, Qt::RightButton, at))
            MarkingMenu::popup(base, MarkingMenu::Trigger::MouseButton, Qt::RightButton, at);
        return;
    }
    // Channels highlighted in the channel box turn a middle drag into Maya's virtual slider.
    if (e->button() == Qt::MiddleButton && !(e->modifiers() & Qt::AltModifier) && channelSlider::begin(e->globalPosition().toPoint()))
    {
        mChannelDrag = true;
        return;
    }
    if ((e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) && !(e->modifiers() & Qt::AltModifier))
        if (Gizmo* gz = currentGizmo())
        {
            gz->setView(viewInfo());
            if (gz->press(e->position(), e->button(), e->modifiers()))
            {
                mGizmoDrag = gz;
                mCursorWrap.reset();
                submit();
                return;
            }
        }
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier))
    {
        mSelecting = true;
        mMarquee = false;
        mPressPos = e->position();
        mLassoDrag = paintSelect::lassoActive();
        mLasso.clear();
        if (mLassoDrag)
            mLasso.push_back({mPressPos.x(), mPressPos.y()});
        return;
    }
    if (e->modifiers() & Qt::AltModifier)
    {
        if (e->button() == Qt::LeftButton)
            mNav = Nav::Tumble;
        else if (e->button() == Qt::MiddleButton)
            mNav = Nav::Track;
        else if (e->button() == Qt::RightButton)
            mNav = Nav::Dolly;
        if (mNav != Nav::None && mLookThrough != scene::kNoNode)
        {
            syncLookThrough();
            if (!beginCameraEdit())
                mNav = Nav::None;
        }
        mNavStart = cameraView();
    }
    setFocus();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* e)
{
    QPointF d = e->position() - mLastPos;
    mLastPos = e->position();
    // Badges follow the cursor, which otherwise only redraws the view when the hover changes.
    if (e->buttons() == Qt::NoButton && !cursorBadges::active(state(e->position())).empty())
        requestFrame();
    if (mTabStroke.active())
    {
        mTabStroke.paint(viewInfo(), mLastPos - d, e->position(), paintSelect::tabRadius(scene::components::mode()));
        submit();
        return;
    }
    if (mSoftDrag)
    {
        // Drag right to grow the falloff, left to shrink; scaled by how big a pixel is at the selection.
        double upp = viewInfo().unitsPerPixel(scene::selectionCenter());
        double r = std::max(0.01, mSoftStartRadius + (e->position().x() - mPressPos.x()) * upp);
        PrefStore::get().set("general", "/softSelect/radius", r);
        requestFrame();
        return;
    }
    if (ViewportTool* tool = viewportTools::active(); tool && mNav == Nav::None && tool->mouseMove(e, viewInfo()))
    {
        submit();
        return;
    }
    if (mChannelDrag)
    {
        channelSlider::drag(e->globalPosition().toPoint(), e->modifiers());
        return;
    }
    if (Gizmo* gz = mGizmoDrag)
    {
        const glm::dvec2 at = mCursorWrap.map({e->position().x(), e->position().y()});
        const QPointF p(at.x, at.y);
        gz->setView(viewInfo());
        gz->drag(p, e->modifiers());
        if (!gz->dragging())
            mGizmoDrag = nullptr;
        if (gz->wrapsCursor())
            wrapCursor(p);
        // One frame for however many moves arrived since the last: drawing on every event backs the queue up and lags.
        requestFrame();
        return;
    }
    if (e->buttons() == Qt::NoButton && mTabHeld && !viewportTools::active())
        updateTabHover(e->position());
    if (e->buttons() == Qt::NoButton)
    {
        Gizmo* gz = currentGizmo();
        if (gz)
        {
            gz->setView(viewInfo());
            if (gz->hover(e->position()))
            {
                updateCursor();
                requestFrame();
            }
        }
        if ((gz && gz->hovering()) || (e->modifiers() & Qt::AltModifier) || viewportTools::active())
            clearPreselect();
        else
            queuePreselect(e->position());
        return;
    }
    if (mSelecting)
    {
        QPointF drag = e->position() - mPressPos;
        mMarquee = mMarquee || std::abs(drag.x()) + std::abs(drag.y()) > 4;
        if (mLassoDrag)
            geo::lasso::extend(mLasso, {e->position().x(), e->position().y()});
        if (mMarquee)
            submit();
        return;
    }
    switch (mNav)
    {
    case Nav::Tumble:
        // Orthographic views pan instead of tumbling, like Maya.
        if (mCamera.ortho)
            mCamera.track(d.x() / width(), d.y() / height(), double(width()) / height());
        else
            mCamera.tumble(d.x() * 0.4, d.y() * 0.4);
        break;
    case Nav::Track: mCamera.track(d.x() / width(), d.y() / height(), double(width()) / height()); break;
    case Nav::Dolly: mCamera.dolly(std::exp(-(d.x() + d.y()) * 0.005)); break;
    case Nav::None: return;
    }
    if (mLookThrough != scene::kNoNode)
        previewCameraEdit();
    requestFrame();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* e)
{
    // Runs after this release is handled, so it sees the new selection and hilite.
    if (e->buttons() == Qt::NoButton && !(e->modifiers() & Qt::AltModifier))
        queuePreselect(e->position());
    if (mTabStroke.active())
    {
        if (e->button() == Qt::LeftButton)
        {
            mTabStroke.end();
            updateTabHover(e->position());
        }
        submit();
        return;
    }
    if (mSwallowRelease && e->button() == Qt::LeftButton)
    {
        mSwallowRelease = false;
        mSelecting = false;
        return;
    }
    if (mSoftDrag)
    {
        mSoftDrag = false;
        return;
    }
    if (ViewportTool* tool = viewportTools::active(); tool && mToolDrag)
    {
        mToolDrag = false;
        tool->mouseRelease(e, viewInfo());
        submit();
        return;
    }
    if (mNav != Nav::None)
    {
        recordCamera(mNavStart);
        commitCameraEdit();
    }
    mNav = Nav::None;
    if (mChannelDrag && e->button() == Qt::MiddleButton)
    {
        mChannelDrag = false;
        channelSlider::end();
        return;
    }
    if (mGizmoDrag && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton || e->button() == Qt::RightButton))
    {
        Gizmo* gz = mGizmoDrag;
        mGizmoDrag = nullptr;
        gz->release();
        submit();
        return;
    }
    if (mSelecting && e->button() == Qt::LeftButton)
    {
        mSelecting = false;
        bool wasMarquee = mMarquee;
        mMarquee = false;
        if (wasMarquee && mLassoDrag)
            pickLasso(e->modifiers());
        else
            pick(mPressPos, wasMarquee ? e->position() : mPressPos, e->modifiers());
        mLassoDrag = false;
        mLasso.clear();
        submit();
    }
}

void ViewportWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    // The gizmo is ignored: the first click moved it under the cursor onto the component it selected.
    if (e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) && !viewportTools::active() &&
        !mTabHeld && scene::components::mode() != scene::ComponentMode::Object &&
        selectLoopAt(e->position(), e->modifiers()))
    {
        mSwallowRelease = true;
        mSelecting = false;
        submit();
        return;
    }
    mousePressEvent(e);
}

bool ViewportWidget::selectLoopAt(QPointF at, Qt::KeyboardModifiers mods)
{
    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    scene::componentPick::Hover h = componentAt(at);
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = h.empty() ? nullptr : g.node(h.node);
    scene::MeshPtr mesh = n ? scene::modifiers::cage(g, h.node) : nullptr;
    if (!mesh)
        return false;
    try
    {
        if (mods & Qt::ShiftModifier)
        {
            // The double-click's own first click is mClicked[0]; the one before it anchors the path.
            const scene::componentPick::Hover& prev = mClicked[0] == h ? mClicked[1] : mClicked[0];
            std::optional<uint32_t> from;
            const auto selectedNow = scene::components::selected(h.node, mode);
            // A click from before a marquee or deselect can be stale, so the anchor must still be selected.
            if (prev.node == h.node && prev.mode == mode && prev.id != h.id && std::ranges::binary_search(selectedNow, prev.id))
                from = prev.id;
            else
                for (uint32_t id : selectedNow)
                    if (id != h.id)
                    {
                        from = id;
                        break;
                    }
            if (!from)
                return false;
            // Like Maya, a second edge adds its own loop unless it shares one with the first, then only the run between them.
            if (mode == ComponentMode::Edge && std::ranges::count(geo::edgeLoop(*mesh, h.id), *from) == 0)
            {
                ops::execute("selectLoopThrough", {{"args", {n->path()}}, {"edge", h.id}, {"add", true}}, CallSource::UI);
                return true;
            }
            ops::execute("selectPathBetween",
                         {{"args", {n->path()}}, {"kind", scene::components::modeName(mode)}, {"from", *from}, {"to", h.id}}, CallSource::UI);
            return true;
        }
        // Edges select their loop; vertices and faces the whole connected piece.
        if (mode == ComponentMode::Edge)
            ops::execute("selectLoopThrough", {{"args", {n->path()}}, {"edge", h.id}}, CallSource::UI);
        else
            ops::execute("selectConnected", {{"args", {n->path()}}, {"kind", scene::components::modeName(mode)}, {"id", h.id}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return true;
}

void ViewportWidget::wheelEvent(QWheelEvent* e)
{
    if (ViewportTool* tool = viewportTools::active(); tool && tool->wheel(e, viewInfo()))
    {
        submit();
        return;
    }
    if (mLookThrough != scene::kNoNode)
    {
        if (!beginCameraEdit())
            return;
        // Restarted per click, so a run of clicks commits (and auto keys) once.
        mWheelCommit->start();
    }
    CameraView before = cameraView();
    mCamera.dolly(std::exp(-e->angleDelta().y() / 120.0 * 0.12));
    if (mLookThrough != scene::kNoNode)
        previewCameraEdit();
    // A run of wheel clicks is one camera undo step.
    recordCamera(before, 1);
    if (e->buttons() == Qt::NoButton)
        queuePreselect(e->position());
    submit();
}

void ViewportWidget::keyPressEvent(QKeyEvent* e)
{
    if (ViewportTool* tool = viewportTools::active(); tool && tool->keyPress(e, viewInfo()))
    {
        submit();
        return;
    }
    if (Gizmo* gz = currentGizmo(); gz && gz->keyPress(e))
    {
        mGizmoDrag = gz->dragging() ? gz : nullptr;
        submit();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ViewportWidget::escape()
{
    QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    if (ViewportTool* tool = viewportTools::active(); tool && tool->keyPress(&e, viewInfo()))
    {
        submit();
        return;
    }
    if (Gizmo* gz = currentGizmo(); gz && gz->keyPress(&e))
    {
        mGizmoDrag = gz->dragging() ? gz : nullptr;
        submit();
        return;
    }
    // An Escape nothing else wanted drops the object selection.
    if (!viewportTools::active() && scene::components::mode() == scene::ComponentMode::Object && !scene::Graph::current().selection().empty())
        actions::run("edit.deselect");
}
} // namespace lila::ui
