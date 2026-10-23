#include "ui/ViewportWidget.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
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
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/RenderSettings.h"
#include "scene/Skin.h"
#include "scene/Transform.h"
#include "ui/Animation.h"
#include "ui/ChannelBox.h"
#include "ui/CursorBadges.h"
#include "ui/DrawOverrides.h"
#include "ui/MarkingMenu.h"
#include "ui/Environments.h"
#include "ui/MaterialDraw.h"
#include "ui/MeshCache.h"
#include "ui/PaintSelect.h"
#include "ui/Outliner.h"
#include "ui/Overlay.h"
#include "ui/RenderPasses.h"
#include "ui/SelectionHandlers.h"
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
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include <chrono>
#include <cmath>
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
    return {lin(0.45f, 0.78f, 1.0f, 1.0f),  lin(0.95f, 0.42f, 0.78f, 1.0f), lin(1.0f, 0.95f, 0.15f, 1.0f),
            lin(1.0f, 0.95f, 0.15f, 0.25f), lin(1.0f, 0.18f, 0.18f, 1.0f),  lin(1.0f, 0.18f, 0.18f, 0.25f)};
}

std::vector<scene::NodeId> hilited();

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
        requestFrame();
    });
    mWheelCommit = new QTimer(this);
    mWheelCommit->setSingleShot(true);
    mWheelCommit->setInterval(500);
    connect(mWheelCommit, &QTimer::timeout, this, [this] { commitCameraEdit(); });
    mToolListener = tools::addListener([this] { requestFrame(); });
    mComponentListener = scene::components::addListener([this] { requestFrame(); });
    // Display layers hide on playback: starting or stopping changes what is drawn without changing the time.
    mPlaybackListener = scene::anim::addPlaybackListener([this] { requestFrame(); });
    mViewportToolListener = viewportTools::addListener([this] { requestFrame(); });
    mGizmoListener = gizmos::addListener([this] {
        syncGizmos();
        updateCursor();
        requestFrame();
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
    scene::components::removeListener(mComponentListener);
    scene::anim::removePlaybackListener(mPlaybackListener);
    viewportTools::removeListener(mViewportToolListener);
    gizmos::removeListener(mGizmoListener);
    selectionHandlers::removeListener(mHandlerListener);
    scene::removeRenderSettingsListener(mRenderSettingsListener);
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
    emit viewStateChanged();
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
        mTabHeld = false;
    // Tab is a held modifier for drag selection here, not focus navigation. Viewport tools (Retopo Draw) keep their own Tab.
    if (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease)
        if (auto* k = static_cast<QKeyEvent*>(e); k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab)
        {
            if (e->type() == QEvent::KeyRelease && !k->isAutoRepeat())
                mTabHeld = false;
            if (!viewportTools::active())
            {
                if (e->type() == QEvent::KeyPress && !k->isAutoRepeat())
                    mTabHeld = true;
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
    auto bg = PrefStore::get().value("general", "/viewport/background", json::array({0.30, 0.30, 0.30}));
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
        viewportDisplay::apply(s);
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
    const float componentWireWidth = viewportDisplay::hairlineWidth(viewportDisplay::componentSizes().edge, devicePixelRatioF());
    std::unordered_set<scene::NodeId> hilite;
    if (scene::components::mode() != scene::ComponentMode::Object)
        for (scene::NodeId id : hilited())
            hilite.insert(id);
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || overridden(*n) || !scene::effectivelyVisible(g, id, inherit) || (!mIsolated.empty() && !mIsolated.contains(id)) ||
            !scene::selectionMask::shown(*n))
            continue;
        int smooth = int(n->as<int64_t>(scene::MeshNode::kSmoothPreview));
        scene::MeshPtr shown = smoothedFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh), smooth);
        bool textured = showMaterials && shown;
        auto rm = renderMeshFor(shown, textured && needsTangents(*shown));
        if (!rm)
            continue;
        render::MeshDraw d;
        d.mesh = std::move(rm);
        if (textured)
            d.materials = drawMaterials(*shown);
        d.world = glm::mat4(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
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
        if (mode == "wireframe" || wireOnShaded || wireOnly || templated || scene::components::mode() != scene::ComponentMode::Object)
            d.flags |= render::DrawWire;
        if (templated && mode != "wireframe")
            d.flags |= render::DrawWireDepth;
        if (layer.color)
            d.wireColor = glm::vec4(glm::vec3(*layer.color), 1.0f);
        else if (templated)
            d.wireColor = kTemplateWireColor;
        if (hilite.contains(id))
        {
            if (componentWire.a > 0.0f)
                d.wireColor = componentWire;
            d.wireWidth = componentWireWidth;
        }
        // In component mode the hilited mesh shows a plain wire so the selected components stand out.
        bool componentMode = scene::components::mode() != scene::ComponentMode::Object;
        if (g.isSelected(id) && !componentMode)
            d.flags |= render::DrawSelected;
        else if (mPreselect.mode == scene::ComponentMode::Object && mPreselect.node == id && !componentMode)
            d.flags |= render::DrawPreselect;
        else if (underSelection.contains(id) && !componentMode)
            d.flags |= render::DrawUnderSelected;
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
    // Locators and extension node types. Highlighted draws last: with an equal-depth test the later line wins, so a
    // selected locator sitting exactly on an unselected one stays visible.
    for (int pass = 0; pass < 3; ++pass)
        for (scene::NodeId id : g.nodes())
        {
            scene::Node* n = g.node(id);
            const drawOverrides::DrawOverride* o = drawOverrides::forType(n->type());
            if (!o || !o->draw || id == mLookThrough || !scene::effectivelyVisible(g, id, inherit) || !scene::selectionMask::shown(*n))
                continue;
            bool hot = mPreselect.mode == scene::ComponentMode::Object && mPreselect.node == id;
            int rank = g.isSelected(id) ? 2 : hot || underSelection.contains(id) ? 1 : 0;
            if (rank != pass)
                continue;
            using HL = drawOverrides::Highlight;
            drawOverrides::DrawContext dc{.frame = s, .painter = painter, .view = view, .node = *n};
            dc.world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            dc.highlight = rank == 2 ? HL::Selected : hot ? HL::Preselected : rank == 1 ? HL::UnderSelected : HL::None;
            dc.color = rank == 2 ? kSelectedColor : hot ? kPreselectColor : rank == 1 ? kChildOfSelectedColor : layerLineColor(g, id, kLocatorColor);
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

    if (mMarquee)
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
    return v;
}

void ViewportWidget::wrapCursor(QMouseEvent* e)
{
    QPointF p = e->position();
    const double m = 2.0;
    QPointF to = p;
    if (p.x() <= m)
        to.setX(width() - m - 2);
    else if (p.x() >= width() - m)
        to.setX(m + 2);
    if (p.y() <= m)
        to.setY(height() - m - 2);
    else if (p.y() >= height() - m)
        to.setY(m + 2);
    if (to == p)
        return;
    QCursor::setPos(mapToGlobal(to.toPoint()));
    if (mGizmoDrag)
        mGizmoDrag->warped(QPointF(to.toPoint()) - p);
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
        if (!o || !o->pick || id == mLookThrough || !viewportPickable(g, id, inherit) || !scene::selectionMask::pickable(*n))
            continue;
        std::optional<double> px = o->pick({.view = view, .node = *n, .world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix), .a = a, .b = b, .click = click});
        if (!px)
            continue;
        if (!click)
            hits.push_back(n->name());
        else if (*px < best)
        {
            best = *px;
            bestName = n->name();
        }
    }
    // Meshes: a click takes the nearest surface under the cursor unless a locator line is right there; a box takes
    // any mesh with a vertex inside it.
    glm::dvec3 rayO, rayD;
    view.ray(a, rayO, rayD);
    double nearest = 1e300;
    std::string nearestMesh;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || overridden(*n) || !viewportPickable(g, id, inherit) || !scene::selectionMask::pickable(*n))
            continue;
        auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
        if (!rm || rm->vertices.empty())
            continue;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        if (click)
        {
            if (auto t = raycast(*rm, world, rayO, rayD); t && t->distance < nearest)
            {
                nearest = t->distance;
                nearestMesh = n->name();
            }
            continue;
        }
        size_t step = std::max<size_t>(1, rm->vertices.size() / 20000);
        for (size_t k = 0; k < rm->vertices.size(); k += step)
            if (auto p = project(glm::dvec3(world * glm::dvec4(glm::dvec3(rm->vertices[k].pos), 1.0))); p && box.contains(*p))
            {
                hits.push_back(n->name());
                break;
            }
    }
    for (const auto& picker : viewportHooks::pickers())
        for (const viewportHooks::PickHit& h : picker(a, b, click, view))
        {
            const scene::Node* hn = g.find(h.name);
            if (hn ? !scene::selectionMask::pickable(*hn) : !scene::selectionMask::enabled(scene::selectionMask::Kind::Other))
                continue;
            if (hn && !scene::displayLayers::pickable(g, hn->id()))
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
/** Meshes components can be picked on: the selected ones (Maya's hilite) and those under selected groups, or every visible mesh if none. */
std::vector<scene::NodeId> hilited()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> out;
    const bool inherit = scene::inheritVisibility();
    // Meshes under a selected group count too; hidden ones only when selected themselves.
    for (scene::NodeId id : uv::meshesUnder(g, g.selection()))
        if (std::ranges::find(g.selection(), id) != g.selection().end() || viewportPickable(g, id, inherit))
            out.push_back(id);
    if (out.empty())
        for (scene::NodeId id : g.nodes())
            if (scene::Node* n = g.node(id); n->type().isA("mesh") && viewportPickable(g, id, inherit))
                out.push_back(id);
    return out;
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
    std::vector<scene::NodeId> live = hilited();

    // Visible means nothing on any hilited mesh is closer along the view ray.
    auto visible = [&](const glm::dvec3& world) {
        glm::dvec3 dir = world - view.eye;
        double dist = glm::length(dir);
        dir /= dist;
        for (scene::NodeId id : live)
        {
            scene::Node* n = g.node(id);
            auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
            if (!rm)
                continue;
            if (auto hit = raycast(*rm, n->as<glm::dmat4>(scene::Transform::kWorldMatrix), view.eye, dir); hit && hit->distance < dist * (1.0 - 1e-4) - 1e-3)
                return false;
        }
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
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        if (mode == ComponentMode::Face)
        {
            auto rm = renderMeshFor(mesh);
            auto pd = pickDataFor(mesh);
            if (!rm || !pd)
                continue;
            if (auto hit = raycast(*rm, world, rayO, rayD); hit && hit->distance < bestRay && hit->triangle < pd->triangleFaces.size() &&
                                                              (!cull || paintSelect::frontFacing(*mesh, world, mode, pd->triangleFaces[hit->triangle], view)))
            {
                bestRay = hit->distance;
                best.node = id;
                best.id = pd->triangleFaces[hit->triangle];
            }
            continue;
        }
        for (const cp::Candidate& c : cp::nearby(*mesh, world, mode, screen, {a.x(), a.y()}, bestPx))
            if ((!cull || paintSelect::frontFacing(*mesh, world, mode, c.id, view)) && visible(cp::anchor(*mesh, world, mode, c.id)))
            {
                bestPx = c.px;
                best.node = id;
                best.id = c.id;
                break;
            }
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
            for (scene::NodeId id : hilited())
            {
                scene::Node* n = g.node(id);
                auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
                if (!rm)
                    continue;
                if (auto hit = raycast(*rm, n->as<glm::dmat4>(scene::Transform::kWorldMatrix), eye, dir); hit && hit->distance < dist * (1.0 - 1e-4) - 1e-3)
                    return false;
            }
            return true;
        };
        for (scene::NodeId id : hilited())
        {
            scene::Node* n = g.node(id);
            scene::MeshPtr mesh = scene::modifiers::cage(g, id);
            if (!mesh)
                continue;
            const geo::Mesh& m = *mesh;
            glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            auto worldOf = [&](uint32_t p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
            if (mode == ComponentMode::Vertex)
            {
                for (uint32_t p = 0; p < m.pointCapacity(); ++p)
                    if (m.pointValid(p))
                        if (auto s = project(worldOf(p)); s && box.contains(*s) && visible(worldOf(p)))
                            hits[id].push_back(p);
            }
            else if (mode == ComponentMode::Edge)
            {
                for (uint32_t e = 0; e < m.edgeCapacity(); ++e)
                {
                    if (!m.edgeValid(e))
                        continue;
                    auto s0 = project(worldOf(m.edgePoint(e, 0))), s1 = project(worldOf(m.edgePoint(e, 1)));
                    if (s0 && s1 && box.contains((*s0 + *s1) * 0.5) && visible((worldOf(m.edgePoint(e, 0)) + worldOf(m.edgePoint(e, 1))) * 0.5))
                        hits[id].push_back(e);
                }
            }
            else if (mode == ComponentMode::Face)
            {
                for (uint32_t f = 0; f < m.faceCapacity(); ++f)
                    if (m.faceValid(f))
                    {
                        glm::dvec3 c = glm::dvec3(world * glm::dvec4(m.faceCentroid(f), 1.0));
                        if (auto s = project(c); s && box.contains(*s) && visible(c))
                            hits[id].push_back(f);
                    }
            }
        }
    }

    if (!click && paintSelect::backfaceCulling())
    {
        ViewInfo view = viewInfo();
        for (auto it = hits.begin(); it != hits.end();)
        {
            scene::MeshPtr mesh = scene::modifiers::cage(g, it->first);
            glm::dmat4 world = g.node(it->first)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            std::erase_if(it->second, [&](uint32_t id) { return !mesh || !paintSelect::frontFacing(*mesh, world, mode, id, view); });
            it = it->second.empty() ? hits.erase(it) : std::next(it);
        }
    }

    bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    const char* flag = mode == ComponentMode::Vertex ? "vertices" : mode == ComponentMode::Edge ? "edges" : "faces";
    try
    {
        UndoChunk chunk("Select Components");
        bool first = true;
        if (hits.empty() && !shift && !ctrl)
            for (scene::NodeId id : hilited())
            {
                ops::execute("selectComponents", {{"args", {g.node(id)->name()}}, {flag, json::array()}}, CallSource::UI);
                break;
            }
        for (auto& [id, ids] : hits)
        {
            json call = {{"args", {g.node(id)->name()}}, {flag, ids}};
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

void ViewportWidget::drawComponents(render::FrameState& s)
{
    LILA_ZONE();

    using scene::ComponentMode;
    ComponentMode mode = scene::components::mode();
    if (mode == ComponentMode::Object)
        return;
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> live = hilited();
    const ComponentPalette palette = componentPalette();
    const glm::vec4 dotLinear = palette.point, hotLinear = palette.selected, faceFillLinear = palette.selectedFill;
    const viewportDisplay::ComponentSizes sizes = viewportDisplay::componentSizes();
    const float wireWidth = viewportDisplay::hairlineWidth(sizes.edge, devicePixelRatioF());
    s.selectedEdgeWidth = viewportDisplay::hairlineWidth(sizes.edgeSelected, devicePixelRatioF());
    for (scene::NodeId id : live)
    {
        scene::Node* n = g.node(id);
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        const geo::Mesh& m = *mesh;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        // With modifiers on, the cage's wire shows what you're editing.
        if (mesh != n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            if (auto rm = renderMeshFor(mesh))
            {
                render::MeshDraw cageDraw;
                cageDraw.mesh = rm;
                cageDraw.world = glm::mat4(world);
                cageDraw.flags = render::DrawWire;
                cageDraw.wireColor = palette.wire;
                cageDraw.wireWidth = wireWidth;
                s.meshes.push_back(std::move(cageDraw));
            }
        auto wp = [&](uint32_t p) { return glm::vec3(world * glm::dvec4(m.position(p), 1.0)); };
        const auto& selFaces = scene::components::selected(id, ComponentMode::Face);
        const auto& selEdges = scene::components::selected(id, ComponentMode::Edge);
        const auto& selPoints = scene::components::selected(id, ComponentMode::Vertex);

        // Depth-tested so components behind the surface stay hidden. These streams take linear colors.
        if (mode == ComponentMode::Face)
            for (uint32_t f : selFaces)
            {
                if (!m.faceValid(f))
                    continue;
                std::vector<glm::vec3> poly;
                m.forEachFaceCorner(f, [&](geo::Index c) { poly.push_back(wp(m.cornerPoint(c))); });
                for (uint32_t p : scene::componentPick::fillTriangles(m, f))
                    s.tris3d.push_back({wp(p), faceFillLinear});
                for (size_t k = 0; k < poly.size(); ++k)
                {
                    s.linesOnTop.push_back({poly[k], hotLinear});
                    s.linesOnTop.push_back({poly[(k + 1) % poly.size()], hotLinear});
                }
            }
        if (mode == ComponentMode::Edge)
            for (uint32_t e : selEdges)
                if (m.edgeValid(e))
                {
                    s.selectedEdges.push_back({wp(m.edgePoint(e, 0)), hotLinear});
                    s.selectedEdges.push_back({wp(m.edgePoint(e, 1)), hotLinear});
                }
        bool soft = PrefStore::get().value<bool>("general", "/softSelect/enabled", false);
        std::vector<float> weights;
        if (soft)
        {
            const auto& sel = scene::components::selected(id, mode);
            weights = scene::softWeights(m, world, scene::components::convert(m, mode, sel, ComponentMode::Vertex));
        }
        if (mode == ComponentMode::Vertex || soft)
        {
            std::vector<char> on(m.pointCapacity(), 0);
            if (mode == ComponentMode::Vertex)
                for (uint32_t p : selPoints)
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
                if (!m.pointValid(p))
                    continue;
                glm::dvec3 c(wp(p));
                double depth = glm::dot(c - view.eye, fwd);
                if (depth <= 0.0)
                    continue;
                float wgt = p < weights.size() ? weights[p] : 0.0f;
                // Outside vertex mode only the falloff shows, as dots shaded by weight.
                if (mode != ComponentMode::Vertex && wgt <= 0.0f)
                    continue;
                double half = 0.5 * (on[p] || wgt >= 1.0f ? sizes.vertexSelected : sizes.vertex) * perUnitDepth * depth;
                glm::dvec3 r = right * half, u = up * half;
                glm::vec4 col = on[p] ? hotLinear : wgt > 0.0f ? viewportDisplay::falloffColor(wgt, grayWeights) : dotLinear;
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
            if (scene::Node* n = scene::Graph::current().find(names.front()))
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
    scene::Graph& g = scene::Graph::current();
    hit.name = names.front();
    scene::Node* n = g.find(hit.name);
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
    const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    glm::dvec3 o, d;
    viewInfo().ray(px, o, d);
    std::optional<RayHit> ray = raycast(*rm, world, o, d);
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
    const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::vec4 color = srgbToLinear(h.color);
    auto weight = [&](uint32_t i) { return i < h.weights.size() ? std::clamp(h.weights[i], 0.0f, 1.0f) : 0.0f; };
    for (uint32_t f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || (h.domain == Domain::Face && weight(f) <= 0.0f))
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
    glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    auto wp = [&](uint32_t p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
    ViewInfo view = viewInfo();
    glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]), up(view.view[0][1], view.view[1][1], view.view[2][1]);
    glm::dvec3 fwd = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
    // Thicker than the selection, so hovered and selected read apart.
    const ComponentPalette palette = componentPalette();
    glm::vec4 hot = palette.hover, faceFill = palette.hoverFill;
    const viewportDisplay::ComponentSizes sizes = viewportDisplay::componentSizes();
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
    auto ribbon = [&](glm::dvec3 p0, glm::dvec3 p1, double halfPx) {
        glm::dvec3 side = glm::cross(p1 - p0, fwd);
        if (glm::length(side) < 1e-12)
            return;
        side = glm::normalize(side);
        glm::dvec3 s0 = side * (halfPx * view.unitsPerPixel(p0)), s1 = side * (halfPx * view.unitsPerPixel(p1));
        quad(p0 - s0, p1 - s1, p1 + s1, p0 + s0, hot);
    };
    auto square = [&](glm::dvec3 c, double halfPx, const glm::vec4& col) {
        double half = halfPx * view.unitsPerPixel(c);
        glm::dvec3 r = right * half, u = up * half;
        quad(c - r - u, c + r - u, c + r + u, c - r + u, col);
    };
    if (mode == ComponentMode::Vertex)
        square(wp(id), 0.5 * sizes.vertexHover, hot);
    else if (mode == ComponentMode::Edge)
    {
        ribbon(wp(m.edgePoint(id, 0)), wp(m.edgePoint(id, 1)), 0.5 * sizes.edgeHover);
        // Edge hover calls out the poles at its ends.
        if (std::optional<glm::vec4> pole = topologyTint(cp::Topology::Pole))
            for (int k = 0; k < 2; ++k)
                if (cp::pointTopology(m, m.edgePoint(id, k)) == cp::Topology::Pole)
                    square(wp(m.edgePoint(id, k)), 3.5, *pole);
    }
    else
    {
        std::vector<glm::dvec3> poly;
        m.forEachFaceCorner(id, [&](geo::Index c) { poly.push_back(wp(m.cornerPoint(c))); });
        for (uint32_t p : cp::fillTriangles(m, id))
            s.tris3d.push_back({glm::vec3(wp(p)), faceFill});
        for (size_t k = 0; k < poly.size(); ++k)
            ribbon(poly[k], poly[(k + 1) % poly.size()], 1.0);
    }
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
                if (scene::Node* n = scene::Graph::current().find(name))
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
        if (scene::Node* n = scene::Graph::current().find(name); n && n->type().isA("camera"))
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
    emit viewStateChanged();
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
    emit viewStateChanged();
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
    mViewName = n->name();
    syncLookThrough();
    syncCameraSettings();
    requestFrame();
    emit viewStateChanged();
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
    bool renamed = n->name() != mViewName;
    mViewName = n->name();
    // The gate settings are the node's attributes, which can change under any edit.
    syncCameraSettings();
    if (renamed)
        emit viewStateChanged();
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
            log::warn("Camera '{}' is locked; unlock it (locked attribute) to move it through the view.", n->name());
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
    scene::Graph& g = scene::Graph::current();
    for (const QString& name : QString::fromUtf8(mime->data(kNodeNamesMime)).split(QChar('\n'), Qt::SkipEmptyParts))
        if (scene::Node* n = g.find(name.toStdString()); n && n->type().isA("camera"))
            return n->id();
    return scene::kNoNode;
}

void ViewportWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (droppedCamera(e->mimeData()) == scene::kNoNode)
        return e->ignore();
    e->setDropAction(Qt::LinkAction);
    e->accept();
}

void ViewportWidget::dragMoveEvent(QDragMoveEvent* e)
{
    if (droppedCamera(e->mimeData()) == scene::kNoNode)
        return e->ignore();
    e->setDropAction(Qt::LinkAction);
    e->accept();
}

void ViewportWidget::dropEvent(QDropEvent* e)
{
    scene::NodeId cam = droppedCamera(e->mimeData());
    if (cam == scene::kNoNode)
        return e->ignore();
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
    if (!mIsolated.empty())
        mIsolated.clear();
    else
    {
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.selection())
        {
            mIsolated.insert(id);
            for (scene::NodeId d : g.descendants(id))
                mIsolated.insert(d);
            // Parents stay so the isolated objects keep their transforms visible in context.
            for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
                mIsolated.insert(p);
        }
    }
    requestFrame();
    emit viewStateChanged();
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
    if (!mIsolated.empty())
        ctx.shownNode = [this](scene::NodeId id) { return mIsolated.contains(id); };
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
        if (scene::Node* n = g.find(name); n && n->type().isA("mesh") && scene::skin::find(g, n->id()) != scene::kNoNode)
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
            const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            if (!mesh)
                continue;
            glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
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
        if (!n->type().isA("mesh"))
            return false;
        auto rm = renderMeshFor(n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
        if (!rm || rm->vertices.empty())
            return false;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (int c = 0; c < 8; ++c)
        {
            glm::dvec3 corner((c & 1) ? rm->boundsMax.x : rm->boundsMin.x, (c & 2) ? rm->boundsMax.y : rm->boundsMin.y,
                              (c & 4) ? rm->boundsMax.z : rm->boundsMin.z);
            glm::dvec3 w(world * glm::dvec4(corner, 1.0));
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
    if (mTabHeld && !viewportTools::active() && e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier) &&
        scene::components::mode() != scene::ComponentMode::Object)
    {
        mTabStroke.begin(paintSelect::Stroke::opFor(e->modifiers(), paintSelect::Stroke::Op::Replace));
        mTabStroke.paint(viewInfo(), e->position(), e->position(), paintSelect::tabRadius(scene::components::mode()));
        submit();
        return;
    }
    // The Paint Select tool takes B + drag for its own brush radius.
    if (tools::softSelectAdjusting() && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) &&
        viewportTools::activeId() != paintSelect::kToolId)
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
        if (!MarkingMenu::popup(base + (overObject ? "Object" : "Empty"), MarkingMenu::Trigger::MouseButton, Qt::RightButton, at))
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
                submit();
                return;
            }
        }
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::AltModifier))
    {
        mSelecting = true;
        mMarquee = false;
        mPressPos = e->position();
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
        gz->setView(viewInfo());
        gz->drag(e->position(), e->modifiers());
        if (!gz->dragging())
            mGizmoDrag = nullptr;
        if (gz->wrapsCursor())
            wrapCursor(e);
        submit();
        return;
    }
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
    submit();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* e)
{
    // Runs after this release is handled, so it sees the new selection and hilite.
    if (e->buttons() == Qt::NoButton && !(e->modifiers() & Qt::AltModifier))
        queuePreselect(e->position());
    if (mTabStroke.active())
    {
        if (e->button() == Qt::LeftButton)
            mTabStroke.end();
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
        pick(mPressPos, wasMarquee ? e->position() : mPressPos, e->modifiers());
        submit();
    }
}

void ViewportWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    Gizmo* gz = currentGizmo();
    if (e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) && !viewportTools::active() &&
        !mTabHeld && !(gz && gz->hovering()) && scene::components::mode() != scene::ComponentMode::Object &&
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
            if (prev.node == h.node && prev.mode == mode && prev.id != h.id)
                from = prev.id;
            else
                for (uint32_t id : scene::components::selected(h.node, mode))
                    if (id != h.id)
                    {
                        from = id;
                        break;
                    }
            if (!from)
                return false;
            ops::execute("selectPathBetween",
                         {{"args", {n->name()}}, {"kind", scene::components::modeName(mode)}, {"from", *from}, {"to", h.id}}, CallSource::UI);
            return true;
        }
        if (mode == ComponentMode::Vertex)
            return false;
        json call = {{"args", {n->name()}}, {"edge", h.id}};
        if (mode == ComponentMode::Face)
        {
            // The face loop crosses the face's edge nearest the cursor.
            const geo::Mesh& m = *mesh;
            glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            double best = 1e300;
            geo::Index nearest = geo::kInvalid;
            m.forEachFaceCorner(h.id, [&](geo::Index c) {
                geo::Index e = m.cornerEdge(c);
                auto s0 = project(glm::dvec3(world * glm::dvec4(m.position(m.edgePoint(e, 0)), 1.0)));
                auto s1 = project(glm::dvec3(world * glm::dvec4(m.position(m.edgePoint(e, 1)), 1.0)));
                if (!s0 || !s1)
                    return;
                QPointF d = *s1 - *s0;
                double len2 = QPointF::dotProduct(d, d);
                double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(at - *s0, d) / len2, 0.0, 1.0) : 0.0;
                QPointF q = *s0 + d * t - at;
                if (double dist = std::hypot(q.x(), q.y()); dist < best)
                {
                    best = dist;
                    nearest = e;
                }
            });
            if (nearest == geo::kInvalid)
                return false;
            call["edge"] = nearest;
            call["faces"] = true;
        }
        ops::execute("selectLoopThrough", call, CallSource::UI);
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
} // namespace lila::ui
