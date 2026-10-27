#include "ui/Theme.h"
#include "ui/UvEditor.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Topology.h"
#include "scene/ComponentPick.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/MarkingMenu.h"
#include "ui/MenuButtons.h"
#include "ui/UvLive.h"
#include "ui/UvTexturePaint.h"
#include "ui/UvTools.h"
#include "ui/UvPackPanel.h"
#include "ui/UvProjectTool.h"
#include "ui/ViewportTool.h"
#include "uv/LiveSession.h"
#include "uv/SymmetryUv.h"
#include "uv/Uv.h"
#include "uv/UvCommands.h"
#include "uv/UvEdit.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

namespace lila::ui
{
using json = nlohmann::json;
using scene::ComponentMode;
using geo::Index;

namespace
{
QColor gray(int v, int a = 255)
{
    return themeShade(v, a);
}

std::function<void()>& opener()
{
    static std::function<void()> fn;
    return fn;
}

std::vector<scene::NodeId> shownMeshes()
{
    return uv::shownMeshes();
}

/** Arrows and the wheel step through power-of-two texture sizes; any other size can still be typed. */
class TextureSizeBox : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

    void stepBy(int steps) override
    {
        int v = value();
        for (; steps > 0; --steps)
            v = int(std::bit_floor(unsigned(v))) * 2;
        for (; steps < 0; ++steps)
            v = std::has_single_bit(unsigned(v)) ? v / 2 : int(std::bit_floor(unsigned(v)));
        setValue(v);
    }
};

json safeRun(const char* name, const json& call, CallSource src)
{
    try
    {
        return ops::execute(name, call, src);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return nullptr;
}

/** "vertex", "edge" or "face" switch the component mode; "island" is face mode with island select. */
void setPickMode(const std::string& m)
{
    const bool island = m == "island";
    if (!island)
        uvLive::setIslandSelect(false);
    safeRun("selectMode", {{"args", {island ? std::string("face") : m}}}, CallSource::UI);
    if (island)
        uvLive::setIslandSelect(true);
}
} // namespace

/** The 2D uv canvas. */
class UvView : public QWidget
{
public:
    enum class Tool
    {
        Select,
        Move,
        Rotate,
        Scale,
        Brush
    };
    enum class Display
    {
        Plain,
        Checker,
        Distortion,
        Stretch,
        Islands
    };

    explicit UvView(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(240, 200);
        setAttribute(Qt::WA_OpaquePaintEvent);
        // Under the cursor the view's keys (F9-F12 modes) win over the focused panel's and the global ones.
        setProperty("hotkeyContext", "uvEditor");
        setProperty("hotkeyHover", true);
        // Its selection is the meshes' components, so Delete removes them as in the viewport.
        deletion::claimScene(this);
        mUvToolListener = uvTools::addRedrawListener([this] { update(); });
        mTexPaint.onUpdate = [this] { update(); };
    }

    UvTexturePaint& texturePaint() { return mTexPaint; }

    ~UvView() override { uvTools::removeRedrawListener(mUvToolListener); }

    /** The canvas a borrowed uvTools tool paints on. */
    uvTools::Canvas canvas() const
    {
        return {[this](const glm::dvec2& uv) { return toScreen(uv); }, [this](const QPointF& s) { return toUv(s); }, mZoom};
    }

    void setTool(Tool t)
    {
        mTool = t;
        update();
    }
    Tool tool() const { return mTool; }
    void setDisplay(Display d)
    {
        mDisplay = d;
        update();
    }
    void setShowUdim(bool on)
    {
        mUdim = on;
        update();
    }
    void setShowStacks(bool on)
    {
        mShowStacks = on;
        update();
    }
    void invalidate() { update(); }
    std::function<void(Tool)> onToolChanged;

    void frameAll()
    {
        sync();
        glm::dvec2 lo(0.0), hi(1.0);
        bool any = false;
        for (const MeshUv& m : mMeshes)
            for (const glm::dvec2& p : m.uv)
            {
                lo = any ? glm::min(lo, p) : p;
                hi = any ? glm::max(hi, p) : p;
                any = true;
            }
        if (any)
        {
            lo = glm::min(lo, glm::dvec2(0.0));
            hi = glm::max(hi, glm::dvec2(1.0));
        }
        frame(lo, hi);
    }

    void frameSelection()
    {
        sync();
        glm::dvec2 lo, hi;
        if (selectionBounds(lo, hi))
            frame(lo, hi);
        else
            frameAll();
    }

protected:
    void resizeEvent(QResizeEvent*) override
    {
        if (!mFramed && width() > 10)
        {
            mFramed = true;
            frame(glm::dvec2(0.0), glm::dvec2(1.0));
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        LILA_ZONE();

        sync();
        QPainter p(this);
        p.fillRect(rect(), gray(38));
        drawBackground(p);
        p.setRenderHint(QPainter::Antialiasing, false);
        drawFaces(p);
        p.setRenderHint(QPainter::Antialiasing, true);
        drawEdges(p);
        drawPoints(p);
        drawPins(p);
        drawPreselect(p);
        drawManipulator(p);
        drawSewDrag(p);
        drawBrush(p);
        mTexPaint.drawCursor(p, mMousePos);
        if (uvTools::UvTool* t = uvTools::active())
            t->draw(p, canvas());
        drawStacks(p);
        if (mMarquee)
        {
            p.setPen(QPen(gray(230), 1.0, Qt::DashLine));
            p.setBrush(gray(255, 24));
            p.drawRect(QRectF(mPressPos, mMousePos).normalized());
        }
        drawHud(p);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        double f = std::pow(1.0015, e->angleDelta().y());
        zoomAbout(e->position(), f);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        setPreselect({});
        setFocus();
        mPressPos = mMousePos = e->position();
        mLastPos = e->position();
        bool alt = e->modifiers() & Qt::AltModifier;
        if (e->button() == Qt::MiddleButton && !alt && mTool == Tool::Move && hasSelection())
        {
            beginDrag(Handle::Free);
            return;
        }
        if (e->button() == Qt::MiddleButton || (alt && e->button() == Qt::LeftButton))
        {
            mNav = Nav::Pan;
            return;
        }
        if (alt && e->button() == Qt::RightButton)
        {
            mNav = Nav::Zoom;
            return;
        }
        if (e->button() == Qt::RightButton)
        {
            // Ctrl converts the component selection exactly as in the viewport.
            const char* menu = (e->modifiers() & Qt::ShiftModifier) ? "uvEditorShift" : (e->modifiers() & Qt::ControlModifier) ? "viewportCtrlObject" : "uvEditor";
            MarkingMenu::popup(menu, MarkingMenu::Trigger::MouseButton, Qt::RightButton, e->globalPosition().toPoint());
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        if (mTexPaint.mousePress(e, uvOf(), mZoom))
        {
            update();
            return;
        }
        if (uvTools::UvTool* t = uvTools::active(); t && t->mousePress(e, canvas()))
        {
            mUvToolDrag = true;
            update();
            return;
        }
        if (mTool == Tool::Brush)
        {
            brushPress(e);
            return;
        }
        Handle h = handleAt(e->position());
        if (h != Handle::None)
        {
            beginDrag(h);
            return;
        }
        mMods = e->modifiers();
        if (mTool == Tool::Select && armSewDrag(e->position()))
            return;
        if (mTool == Tool::Select && uvLive::liveMode() && armLiveDrag(e->position()))
            return;
        mMarquee = true;
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        QPointF d = e->position() - mLastPos;
        mLastPos = mMousePos = e->position();
        if (mNav == Nav::Pan)
        {
            mPan += glm::dvec2(d.x(), d.y());
            update();
            return;
        }
        if (mNav == Nav::Zoom)
        {
            zoomAbout(mPressPos, std::pow(1.006, d.x() - d.y()));
            return;
        }
        if (mDrag != Handle::None)
        {
            updateDrag(e->modifiers());
            update();
            return;
        }
        if (mTexPaint.mouseMove(e, uvOf(), mZoom) || mTexPaint.painting())
        {
            update();
            if (e->buttons() & Qt::LeftButton)
                return;
        }
        if (uvTools::UvTool* t = uvTools::active(); t && (mUvToolDrag || !(e->buttons() & Qt::LeftButton)) && t->mouseMove(e, canvas()))
        {
            update();
            return;
        }
        if (mTool == Tool::Brush)
        {
            brushMove(e);
            update();
            return;
        }
        if (mSew.armed || mLiveDrag.armed)
        {
            QPointF moved = e->position() - mPressPos;
            bool far = std::abs(moved.x()) + std::abs(moved.y()) >= 4.0;
            if (mSew.armed && far)
                mSew.dragging = true;
            if (mLiveDrag.armed && far && !mLiveDrag.started)
                startLiveDrag();
            if (mLiveDrag.started)
                updateLiveDrag();
            update();
            return;
        }
        if (mMarquee)
        {
            update();
            return;
        }
        Handle h = handleAt(e->position());
        if (h != mHover)
        {
            mHover = h;
            update();
        }
        bool on = PrefStore::get().value<bool>("general", "/viewport/preselect/components", true);
        setPreselect(h == Handle::None && on && !(e->modifiers() & Qt::AltModifier) ? hoverAt(e->position()) : scene::componentPick::Hover{});
    }

    void leaveEvent(QEvent* e) override
    {
        if (uvTools::UvTool* t = uvTools::active())
        {
            t->leave();
            update();
        }
        setPreselect({});
        QWidget::leaveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (mNav != Nav::None)
        {
            mNav = Nav::None;
            return;
        }
        if (mTexPaint.mouseRelease(e, uvOf(), mZoom))
        {
            update();
            return;
        }
        if (mDrag != Handle::None)
        {
            commitDrag();
            return;
        }
        if (mUvToolDrag && e->button() == Qt::LeftButton)
        {
            mUvToolDrag = false;
            if (uvTools::UvTool* t = uvTools::active())
                t->mouseRelease(e, canvas());
            update();
            return;
        }
        if (mTool == Tool::Brush && e->button() == Qt::LeftButton)
        {
            brushRelease(e);
            return;
        }
        if (mSew.armed && e->button() == Qt::LeftButton)
        {
            bool dragged = mSew.dragging;
            if (dragged)
                finishSewDrag(e->position());
            mSew = {};
            if (!dragged)
                pickAt(e->position(), mMods);
            update();
            return;
        }
        if (mLiveDrag.armed && e->button() == Qt::LeftButton)
        {
            bool started = mLiveDrag.started;
            if (started)
                endLiveDrag(e->modifiers());
            mLiveDrag = {};
            if (!started)
                pickAt(e->position(), mMods);
            update();
            return;
        }
        if (mMarquee && e->button() == Qt::LeftButton)
        {
            mMarquee = false;
            QPointF delta = e->position() - mPressPos;
            if (std::abs(delta.x()) + std::abs(delta.y()) < 4.0)
                pickAt(e->position(), mMods);
            else
                pickRect(QRectF(mPressPos, e->position()).normalized(), mMods);
            update();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::AltModifier))
            return;
        if (pickMode() == ComponentMode::Edge)
            selectLoopAt(e->position(), e->modifiers());
        else
            selectIslandAt(e->position(), e->modifiers());
    }

    bool event(QEvent* e) override
    {
        // Brush keys are held (B, M and F drag size and strength), so they must reach the view, not the hotkeys.
        if (e->type() == QEvent::ShortcutOverride)
        {
            auto* k = static_cast<QKeyEvent*>(e);
            bool brushKey = mTool == Tool::Brush && (k->key() == Qt::Key_B || k->key() == Qt::Key_M || k->key() == Qt::Key_F ||
                                                     k->key() == Qt::Key_BracketLeft || k->key() == Qt::Key_BracketRight);
            if ((brushKey || k->key() == Qt::Key_P) && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier)))
            {
                e->accept();
                return true;
            }
        }
        return QWidget::event(e);
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (uvTools::UvTool* t = uvTools::active(); t && t->keyPress(e, canvas()))
        {
            update();
            return;
        }
        if (mTool == Tool::Brush && brushKey(e, true))
            return;
        switch (e->key())
        {
        case Qt::Key_Q: changeTool(Tool::Select); break;
        case Qt::Key_W: changeTool(Tool::Move); break;
        case Qt::Key_E: changeTool(Tool::Rotate); break;
        case Qt::Key_R: changeTool(Tool::Scale); break;
        case Qt::Key_F: frameSelection(); break;
        case Qt::Key_A: frameAll(); break;
        case Qt::Key_P: togglePinAt(mMousePos); break;
        default: QWidget::keyPressEvent(e); return;
        }
    }

    void keyReleaseEvent(QKeyEvent* e) override
    {
        if (mTool == Tool::Brush && brushKey(e, false))
            return;
        QWidget::keyReleaseEvent(e);
    }

private:
    struct Segment
    {
        uint32_t a, b; // entries
        Index edge;
        bool seam;
    };

    /** One mesh's uvs, flattened per face corner. Rebuilt when the mesh or uv set changes. */
    struct MeshUv
    {
        scene::NodeId node = scene::kNoNode;
        std::string name;
        scene::MeshPtr mesh;
        std::string set;
        std::vector<Index> faces;
        std::vector<uint32_t> faceStart; // per face, plus one
        std::vector<Index> corner;       // per entry
        std::vector<glm::dvec2> uv;      // per entry
        std::vector<uint32_t> entryOf;   // per corner capacity
        std::vector<Segment> segments;
        std::vector<float> distortion; // per face (local)
        bool distortionValid = false;
        std::vector<uint8_t> pinned;   // per entry
        std::vector<int> island;       // per face, for the stretch and island displays (empty until needed)
        std::vector<double> area3;     // per face
        std::vector<double> islandRatio;
        std::vector<int> pickIsland; // per face, its island as island select sees it (empty until needed)
        uint64_t liveGen = 0, liveEpoch = 0;
        // Symmetry caches, built on demand for the axis they were made for (-2: not yet).
        mutable int symAxis = -2;
        mutable std::shared_ptr<const geo::SymmetryMap> sym;
        mutable std::shared_ptr<const uv::UvMirror> mirror;
    };

    enum class Nav
    {
        None,
        Pan,
        Zoom
    };
    enum class Handle
    {
        None,
        Free,
        U,
        V,
        Ring,
    };

    // View mapping: v points up.
    QPointF toScreen(const glm::dvec2& uv) const { return QPointF(mPan.x + uv.x * mZoom, mPan.y - uv.y * mZoom); }
    glm::dvec2 toUv(const QPointF& s) const { return {(s.x() - mPan.x) / mZoom, (mPan.y - s.y()) / mZoom}; }
    UvTexturePaint::ToUv uvOf() const
    {
        return [this](const QPointF& s) { return toUv(s); };
    }

    void frame(glm::dvec2 lo, glm::dvec2 hi)
    {
        glm::dvec2 ext = glm::max(hi - lo, glm::dvec2(1e-6));
        double z = std::min((width() - 40.0) / ext.x, (height() - 60.0) / ext.y);
        mZoom = std::clamp(z, 1.0, 1e7);
        glm::dvec2 c = (lo + hi) * 0.5;
        mPan = glm::dvec2(width() * 0.5 - c.x * mZoom, height() * 0.5 + 10.0 + c.y * mZoom);
        update();
    }

    void zoomAbout(QPointF at, double f)
    {
        glm::dvec2 before = toUv(at);
        mZoom = std::clamp(mZoom * f, 1.0, 1e7);
        QPointF after = toScreen(before);
        mPan += glm::dvec2(at.x() - after.x(), at.y() - after.y());
        update();
    }

    void changeTool(Tool t)
    {
        setTool(t);
        if (onToolChanged)
            onToolChanged(t);
    }

    void sync()
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        const std::string set = uv::currentUvSet();
        std::vector<scene::NodeId> ids = shownMeshes();
        std::vector<MeshUv> next;
        for (scene::NodeId id : ids)
        {
            scene::Node* n = g.node(id);
            scene::MeshPtr mesh = n ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
            if (!mesh)
                continue;
            auto old = std::find_if(mMeshes.begin(), mMeshes.end(), [&](const MeshUv& m) { return m.node == id; });
            // Meshes are immutable once published, so the same pointer means the same uvs.
            if (old != mMeshes.end() && old->mesh == mesh && old->set == set)
            {
                old->name = n->path();
                next.push_back(std::move(*old));
                continue;
            }
            next.push_back(build(id, n->path(), mesh, set));
        }
        mMeshes = std::move(next);
        applyLive();
    }

    /** Shows the live solver's latest uvs and pins in place of the mesh's while a session matches it. */
    void applyLive()
    {
        for (MeshUv& mu : mMeshes)
        {
            uv::live::Session* s = mu.corner.empty() ? nullptr : uv::live::find(mu.node);
            if (!s || s->uvSet() != mu.set)
                continue;
            auto f = s->solver().frame();
            if (f->epoch != s->epoch() || (f->generation == mu.liveGen && f->epoch == mu.liveEpoch))
                continue;
            mu.liveGen = f->generation;
            mu.liveEpoch = f->epoch;
            mu.pinned.assign(mu.corner.size(), 0);
            for (size_t k = 0; k < mu.corner.size(); ++k)
            {
                auto [i, v] = s->wedge(mu.corner[k]);
                if (i >= 0 && size_t(i) < f->islands.size() && f->islands[size_t(i)])
                {
                    mu.uv[k] = f->islands[size_t(i)]->uv[v];
                    mu.pinned[k] = f->islands[size_t(i)]->pinned[v];
                }
            }
            mu.distortionValid = false;
        }
    }

    static MeshUv build(scene::NodeId id, const std::string& name, scene::MeshPtr mesh, const std::string& set)
    {
        LILA_ZONE();

        MeshUv out;
        out.node = id;
        out.name = name;
        out.mesh = mesh;
        out.set = set;
        const geo::Mesh& m = *mesh;
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
        if (uv.empty())
            return out;
        std::span<const bool> seam = m.edgeAttrs().get<bool>(geo::attr::kSeam);
        out.entryOf.assign(m.cornerCapacity(), ~0u);
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            out.faces.push_back(f);
            out.faceStart.push_back(uint32_t(out.corner.size()));
            m.forEachFaceCorner(f, [&](Index c) {
                out.entryOf[c] = uint32_t(out.corner.size());
                out.corner.push_back(c);
                out.uv.push_back(glm::dvec2(uv[c]));
            });
        }
        out.faceStart.push_back(uint32_t(out.corner.size()));
        if (std::span<const bool> pin = m.cornerAttrs().get<bool>("uvPin"); !pin.empty())
        {
            out.pinned.resize(out.corner.size());
            for (size_t k = 0; k < out.corner.size(); ++k)
                out.pinned[k] = pin[out.corner[k]] ? 1 : 0;
        }
        auto same = [](glm::vec2 a, glm::vec2 b) { return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec2(1e-6f))); };
        for (size_t fi = 0; fi + 1 < out.faceStart.size(); ++fi)
            for (uint32_t k = out.faceStart[fi]; k < out.faceStart[fi + 1]; ++k)
            {
                uint32_t k2 = k + 1 < out.faceStart[fi + 1] ? k + 1 : out.faceStart[fi];
                Index c = out.corner[k];
                Index e = m.cornerEdge(c);
                Index r = m.cornerRadial(c);
                bool split = r == c || m.cornerRadial(r) != c;
                if (!split)
                {
                    bool sameDir = m.cornerPoint(r) == m.cornerPoint(c);
                    glm::vec2 ra = sameDir ? uv[r] : uv[m.cornerNext(r)], rb = sameDir ? uv[m.cornerNext(r)] : uv[r];
                    split = !same(ra, uv[c]) || !same(rb, uv[m.cornerNext(c)]);
                    // Shared edges are drawn once, from the lower corner.
                    if (!split && r < c)
                        continue;
                }
                out.segments.push_back({k, k2, e, split || (!seam.empty() && seam[e])});
            }
        return out;
    }

    /** Entries the selection moves: wedges of selected faces, or corners of selected points and edge ends. */
    std::vector<uint8_t> selectedEntries(const MeshUv& mu) const
    {
        std::vector<uint8_t> mask(mu.corner.size(), 0);
        if (!mu.mesh || mu.corner.empty())
            return mask;
        const geo::Mesh& m = *mu.mesh;
        ComponentMode mode = scene::components::mode();
        if (mode == ComponentMode::Object)
            return mask;
        const auto& ids = scene::components::selected(mu.node, mode);
        if (ids.empty())
            return mask;
        std::vector<Index> corners;
        if (mode == ComponentMode::Face)
            corners = uv::uvCornersOfFaces(m, mu.set, ids);
        else
            corners = uv::cornersOfPoints(m, scene::components::convert(m, mode, ids, ComponentMode::Vertex));
        for (Index c : corners)
            if (c < mu.entryOf.size() && mu.entryOf[c] != ~0u)
                mask[mu.entryOf[c]] = 1;
        return mask;
    }

    /** The mesh's mirror pairs while its modeling symmetry is on, else null. */
    const geo::SymmetryMap* symmetryOf(const MeshUv& mu) const
    {
        scene::Node* n = scene::Graph::current().node(mu.node);
        const int axis = n && mu.mesh ? uv::uvSymmetryAxis(*n) : -1;
        if (axis != mu.symAxis)
        {
            mu.symAxis = axis;
            mu.sym.reset();
            mu.mirror.reset();
            if (axis >= 0)
                if (std::optional<geo::SymmetryMap> map = uv::uvSymmetryMap(*n, *mu.mesh))
                    mu.sym = std::make_shared<const geo::SymmetryMap>(std::move(*map));
        }
        return mu.sym.get();
    }

    /** Its uv corner pairs while symmetry is on, else null. */
    const uv::UvMirror* mirrorOf(const MeshUv& mu) const
    {
        const geo::SymmetryMap* s = symmetryOf(mu);
        if (!s || mu.corner.empty())
            return nullptr;
        if (!mu.mirror)
            mu.mirror = std::make_shared<const uv::UvMirror>(uv::buildUvMirror(*mu.mesh, *s, mu.set));
        return mu.mirror.get();
    }

    /** ids plus their mirror images while the mesh's symmetry is on. */
    std::vector<Index> withMirror(scene::NodeId node, ComponentMode mode, const std::vector<Index>& ids) const
    {
        auto it = std::find_if(mMeshes.begin(), mMeshes.end(), [&](const MeshUv& m) { return m.node == node; });
        const geo::SymmetryMap* s = it != mMeshes.end() ? symmetryOf(*it) : nullptr;
        return s ? uv::mirrorComponents(*s, *it->mesh, mode, ids) : ids;
    }

    bool hasSelection() const
    {
        for (const MeshUv& mu : mMeshes)
        {
            auto mask = selectedEntries(mu);
            if (std::find(mask.begin(), mask.end(), uint8_t(1)) != mask.end())
                return true;
        }
        return false;
    }

    /** With drivingSide, a symmetric mesh counts only the side its selection mostly is on (the side edits follow). */
    bool selectionBounds(glm::dvec2& lo, glm::dvec2& hi, bool drivingSide = false) const
    {
        bool any = false;
        for (const MeshUv& mu : mMeshes)
        {
            auto mask = selectedEntries(mu);
            if (const uv::UvMirror* mirror = drivingSide ? mirrorOf(mu) : nullptr)
            {
                std::vector<Index> corners;
                for (size_t i = 0; i < mask.size(); ++i)
                    if (mask[i])
                        corners.push_back(mu.corner[i]);
                const int drive = uv::uvMirrorDrive(*mirror, corners);
                bool kept = false;
                for (size_t i = 0; i < mask.size(); ++i)
                    kept |= mask[i] && mirror->side[mu.corner[i]] != -drive;
                for (size_t i = 0; i < mask.size() && kept; ++i)
                    if (mirror->side[mu.corner[i]] == -drive)
                        mask[i] = 0;
            }
            for (size_t i = 0; i < mask.size(); ++i)
                if (mask[i])
                {
                    lo = any ? glm::min(lo, mu.uv[i]) : mu.uv[i];
                    hi = any ? glm::max(hi, mu.uv[i]) : mu.uv[i];
                    any = true;
                }
        }
        return any;
    }

    /** Where an entry is drawn, with the live drag applied (and mirrored onto its partner with symmetry on). */
    glm::dvec2 drawnUv(size_t mesh, size_t entry) const
    {
        const glm::dvec2& p = mMeshes[mesh].uv[entry];
        if (mDrag == Handle::None || mesh >= mDragMask.size())
            return p;
        if (mesh < mDragFollow.size() && !mDragFollow[mesh].empty() && mDragFollow[mesh][entry].first != ~0u)
        {
            auto& [src, linear] = mDragFollow[mesh][entry];
            const glm::dvec2& ps = mMeshes[mesh].uv[src];
            return p + linear * ((mDragMask[mesh][src] ? dragTransform(ps) : ps) - ps);
        }
        if (!mDragMask[mesh][entry])
            return p;
        return dragTransform(p);
    }

    glm::dvec2 dragTransform(const glm::dvec2& p) const
    {
        glm::dvec2 d = (p - mPivot) * mDragScale;
        double c = std::cos(mDragAngle), s = std::sin(mDragAngle);
        return mPivot + glm::dvec2(d.x * c - d.y * s, d.x * s + d.y * c) + mDragMove;
    }

    void drawBackground(QPainter& p)
    {
        int tilesU = mUdim ? 10 : 1, tilesV = mUdim ? 2 : 1;
        for (int tv = 0; tv < tilesV; ++tv)
            for (int tu = 0; tu < tilesU; ++tu)
            {
                QRectF tile(toScreen({double(tu), double(tv + 1)}), toScreen({double(tu + 1), double(tv)}));
                p.fillRect(tile, tu == 0 && tv == 0 ? gray(50) : gray(45));
                if (mDisplay == Display::Checker)
                {
                    const int n = uvLive::checkerTiling();
                    for (int y = 0; y < n; ++y)
                        for (int x = 0; x < n; ++x)
                            if ((x + y) % 2)
                                p.fillRect(QRectF(toScreen({tu + double(x) / n, tv + double(y + 1) / n}),
                                                  toScreen({tu + double(x + 1) / n, tv + double(y) / n})),
                                           gray(64));
                }
                if (mUdim)
                {
                    p.setPen(gray(110));
                    p.drawText(tile.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop, QString::number(1001 + tu + 10 * tv));
                }
            }
        mTexPaint.drawBackground(p, rect(), [this](const glm::dvec2& uv) { return toScreen(uv); }, uvOf(), mUdim);
        // Tenths grid inside the tiles, tile borders brighter.
        p.setPen(QPen(gray(58), 0));
        for (int i = 1; i < 10 * tilesU; ++i)
            if (i % 10)
                p.drawLine(toScreen({i / 10.0, 0.0}), toScreen({i / 10.0, double(tilesV)}));
        for (int i = 1; i < 10 * tilesV; ++i)
            if (i % 10)
                p.drawLine(toScreen({0.0, i / 10.0}), toScreen({double(tilesU), i / 10.0}));
        p.setPen(QPen(gray(96), 0));
        for (int i = 0; i <= tilesU; ++i)
            p.drawLine(toScreen({double(i), 0.0}), toScreen({double(i), double(tilesV)}));
        for (int i = 0; i <= tilesV; ++i)
            p.drawLine(toScreen({0.0, double(i)}), toScreen({double(tilesU), double(i)}));
    }

    void drawFaces(QPainter& p)
    {
        ComponentMode mode = scene::components::mode();
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        {
            MeshUv& mu = mMeshes[mi];
            if (mu.corner.empty())
                continue;
            if (mDisplay == Display::Distortion && !mu.distortionValid)
            {
                std::vector<uv::FaceDistortion> d = uv::faceDistortion(*mu.mesh, mu.set);
                mu.distortion.resize(mu.faces.size());
                for (size_t i = 0; i < mu.faces.size(); ++i)
                    mu.distortion[i] = uv::distortionValue(d[mu.faces[i]]);
                mu.distortionValid = true;
            }
            std::unordered_set<Index> selFaces;
            if (mode == ComponentMode::Face)
                for (uint32_t f : scene::components::selected(mu.node, ComponentMode::Face))
                    selFaces.insert(f);
            const bool shaded = mDisplay == Display::Stretch || mDisplay == Display::Islands;
            std::vector<double> stretch;
            if (shaded)
                stretch = faceStretch(mi);
            const bool twoColour = uvLive::twoColourHeatmap();
            p.setPen(Qt::NoPen);
            QPolygonF poly;
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
            {
                bool sel = selFaces.count(mu.faces[fi]) > 0;
                if (shaded && fi < mu.island.size())
                {
                    poly.clear();
                    for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                        poly << toScreen(drawnUv(mi, k));
                    p.setBrush(mDisplay == Display::Stretch ? stretchColor(stretch[fi], twoColour)
                                                            : gray(70 + (mu.island[fi] * 53) % 120, 210));
                    p.drawPolygon(poly);
                }
                if (mDisplay != Display::Distortion && !sel)
                    continue;
                poly.clear();
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    poly << toScreen(drawnUv(mi, k));
                if (mDisplay == Display::Distortion)
                {
                    // Lightness only: even faces stay dark, stretched ones turn bright.
                    int v = 58 + int(mu.distortion[fi] * 170.0f);
                    p.setBrush(QColor(v, v, v, 220));
                    p.drawPolygon(poly);
                }
                if (sel)
                {
                    p.setBrush(gray(200, 90));
                    p.drawPolygon(poly);
                }
            }
        }
    }

    void drawEdges(QPainter& p)
    {
        ComponentMode mode = scene::components::mode();
        QList<QLineF> plain, seams, selected;
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        {
            const MeshUv& mu = mMeshes[mi];
            std::unordered_set<Index> selEdges;
            if (mode == ComponentMode::Edge)
                for (uint32_t e : scene::components::selected(mu.node, ComponentMode::Edge))
                    selEdges.insert(e);
            for (const Segment& s : mu.segments)
            {
                QLineF l(toScreen(drawnUv(mi, s.a)), toScreen(drawnUv(mi, s.b)));
                if (selEdges.count(s.edge))
                    selected.push_back(l);
                else if (s.seam)
                    seams.push_back(l);
                else
                    plain.push_back(l);
            }
        }
        p.setPen(QPen(gray(128), 1.0));
        p.drawLines(plain);
        p.setPen(QPen(gray(232), 1.6));
        p.drawLines(seams);
        p.setPen(QPen(gray(255), 2.6));
        p.drawLines(selected);
    }

    void drawPoints(QPainter& p)
    {
        ComponentMode mode = scene::components::mode();
        if (mode != ComponentMode::Vertex && mode != ComponentMode::Edge && mode != ComponentMode::Face)
            return;
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        {
            const MeshUv& mu = mMeshes[mi];
            std::vector<uint8_t> mask = selectedEntries(mu);
            if (mode == ComponentMode::Vertex && mu.corner.size() < 200000)
            {
                p.setPen(Qt::NoPen);
                p.setBrush(gray(170));
                for (size_t i = 0; i < mu.uv.size(); ++i)
                    if (!mask[i])
                        p.drawRect(QRectF(toScreen(drawnUv(mi, i)) - QPointF(1.5, 1.5), QSizeF(3.0, 3.0)));
            }
            p.setPen(QPen(gray(20), 1.0));
            p.setBrush(gray(255));
            for (size_t i = 0; i < mu.uv.size(); ++i)
                if (mask[i])
                    p.drawRect(QRectF(toScreen(drawnUv(mi, i)) - QPointF(2.5, 2.5), QSizeF(5.0, 5.0)));
        }
    }

    QPointF pivotScreen() const { return toScreen(mDrag != Handle::None ? mPivot + mDragMove : currentPivot()); }

    glm::dvec2 currentPivot() const
    {
        glm::dvec2 lo, hi;
        return selectionBounds(lo, hi, true) ? (lo + hi) * 0.5 : glm::dvec2(0.5);
    }

    void drawManipulator(QPainter& p)
    {
        if (mTool == Tool::Select || mTool == Tool::Brush || (mDrag == Handle::None && !hasSelection()))
            return;
        QPointF c = pivotScreen();
        auto col = [&](Handle h, int base) { return gray((mHover == h || mDrag == h) ? 255 : base); };
        const double len = 64.0;
        if (mTool == Tool::Rotate)
        {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(col(Handle::Ring, 200), 2.0));
            p.drawEllipse(c, len * 0.8, len * 0.8);
            if (mDrag == Handle::Ring)
            {
                p.setPen(QPen(gray(255), 1.0, Qt::DashLine));
                p.drawLine(c, c + QPointF(std::cos(mDragAngle), -std::sin(mDragAngle)) * len * 0.8);
            }
        }
        else
        {
            for (auto [h, dir, base] : {std::tuple{Handle::U, QPointF(1, 0), 220}, std::tuple{Handle::V, QPointF(0, -1), 160}})
            {
                QPointF tip = c + dir * len;
                p.setPen(QPen(col(h, base), 2.0));
                p.drawLine(c, tip);
                p.setPen(Qt::NoPen);
                p.setBrush(col(h, base));
                if (mTool == Tool::Move)
                {
                    QPointF n(-dir.y(), dir.x());
                    p.drawPolygon(QPolygonF() << tip + dir * 10.0 << tip + n * 5.0 << tip - n * 5.0);
                }
                else
                    p.drawRect(QRectF(tip - QPointF(5, 5), QSizeF(10, 10)));
                p.setPen(gray(200));
                p.drawText(tip + dir * 14.0 + QPointF(-4, 4), h == Handle::U ? "U" : "V");
            }
        }
        p.setPen(QPen(gray(20), 1.0));
        p.setBrush(col(Handle::Free, 235));
        p.drawRect(QRectF(c - QPointF(6, 6), QSizeF(12, 12)));
    }

    /** A "x3" badge on every stack of identical islands, so stacking from uvPack stays visible. */
    void drawStacks(QPainter& p)
    {
        if (!mShowStacks)
            return;
        std::vector<StackBadges> next;
        for (const MeshUv& mu : mMeshes)
        {
            if (!mu.mesh)
                continue;
            auto old = std::find_if(mStacks.begin(), mStacks.end(), [&](const StackBadges& s) { return s.mesh == mu.mesh && s.set == mu.set; });
            if (old != mStacks.end())
            {
                next.push_back(std::move(*old));
                continue;
            }
            StackBadges sb{mu.mesh, mu.set, {}};
            std::span<const glm::vec2> uv = mu.mesh->cornerAttrs().get<glm::vec2>(mu.set);
            for (const auto& stack : uv::stackedIslands(*mu.mesh, mu.set))
            {
                glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
                for (Index f : stack.front())
                    mu.mesh->forEachFaceCorner(f, [&](Index c) {
                        lo = glm::min(lo, glm::dvec2(uv[c]));
                        hi = glm::max(hi, glm::dvec2(uv[c]));
                    });
                sb.badges.emplace_back((lo + hi) * 0.5, int(stack.size()));
            }
            next.push_back(std::move(sb));
        }
        mStacks = std::move(next);
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        for (const StackBadges& sb : mStacks)
            for (const auto& [at, count] : sb.badges)
            {
                QPointF c = toScreen(at);
                QRectF r(c.x() - 14, c.y() - 9, 28, 18);
                p.setPen(QPen(gray(20), 1.0));
                p.setBrush(gray(225));
                p.drawRoundedRect(r, 4, 4);
                p.setPen(gray(20));
                p.drawText(r, Qt::AlignCenter, QString("x%1").arg(count));
            }
        p.setBrush(Qt::NoBrush);
        f.setBold(false);
        p.setFont(f);
    }

    void drawHud(QPainter& p)
    {
        p.setPen(gray(200));
        QString tool = mTool == Tool::Select ? "Select (Q)" : mTool == Tool::Move ? "Move (W)" : mTool == Tool::Rotate ? "Rotate (E)" : "Scale (R)";
        if (mTool == Tool::Brush)
        {
            std::string_view b = uv::liveBrushName(uvLive::brush().brush);
            tool = QString("Brush %1  radius %2  strength %3")
                       .arg(QString::fromUtf8(b.data(), qsizetype(b.size())))
                       .arg(int(std::lround(uvLive::brush().radius)))
                       .arg(uvLive::brush().strength, 0, 'f', 2);
        }
        if (uvLive::liveMode())
            tool += "   Live";
        QString meshes;
        bool missing = false;
        for (const MeshUv& mu : mMeshes)
        {
            meshes += (meshes.isEmpty() ? "" : ", ") + QString::fromStdString(mu.name);
            missing |= mu.corner.empty();
        }
        if (meshes.isEmpty())
            meshes = "select a mesh";
        QString text = QString::fromStdString(uv::currentUvSet()) + "   " + meshes + "   " + tool;
        for (const MeshUv& mu : mMeshes)
            if (scene::Node* n = scene::Graph::current().node(mu.node); n && uv::uvSymmetryAxis(*n) >= 0)
            {
                text += QString("   Symmetry %1").arg(QChar('X' + uv::uvSymmetryAxis(*n)));
                break;
            }
        if (missing)
            text += "   (no uvs in this set)";
        p.drawText(QRectF(8, 4, width() - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, text);
        glm::dvec2 at = toUv(mMousePos);
        p.setPen(gray(140));
        p.drawText(QRectF(8, height() - 22, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QString("u %1  v %2").arg(at.x, 0, 'f', 3).arg(at.y, 0, 'f', 3));
    }

    Handle handleAt(QPointF s) const
    {
        if (mTool == Tool::Select || mTool == Tool::Brush || !hasSelection())
            return Handle::None;
        QPointF c = pivotScreen();
        QPointF d = s - c;
        const double len = 64.0;
        if (std::abs(d.x()) <= 8 && std::abs(d.y()) <= 8)
            return Handle::Free;
        if (mTool == Tool::Rotate)
        {
            double r = std::hypot(d.x(), d.y());
            return std::abs(r - len * 0.8) < 8.0 ? Handle::Ring : Handle::None;
        }
        if (d.x() > 8 && d.x() < len + 14 && std::abs(d.y()) < 7)
            return Handle::U;
        if (-d.y() > 8 && -d.y() < len + 14 && std::abs(d.x()) < 7)
            return Handle::V;
        return Handle::None;
    }

    void beginDrag(Handle h)
    {
        mDrag = h;
        mAxisLock = 0;
        mPivot = currentPivot();
        mDragMove = glm::dvec2(0.0);
        mDragScale = glm::dvec2(1.0);
        mDragAngle = 0.0;
        mDragMask.clear();
        mDragFollow.clear();
        for (const MeshUv& mu : mMeshes)
        {
            mDragMask.push_back(selectedEntries(mu));
            mDragFollow.push_back(dragFollow(mu, mDragMask.back()));
        }
        mPressUv = toUv(mPressPos);
    }

    /** Per entry, the entry whose drag it follows and through what map, as uvTransform will mirror it; empty without symmetry. */
    std::vector<std::pair<uint32_t, glm::dmat2>> dragFollow(const MeshUv& mu, const std::vector<uint8_t>& mask) const
    {
        std::vector<std::pair<uint32_t, glm::dmat2>> out;
        const uv::UvMirror* mirror = mirrorOf(mu);
        if (!mirror)
            return out;
        std::vector<Index> moved;
        for (size_t k = 0; k < mask.size(); ++k)
            if (mask[k])
                moved.push_back(mu.corner[k]);
        out.assign(mu.corner.size(), {~0u, glm::dmat2(1.0)});
        for (const uv::UvFollow& r : uv::uvFollowRules(*mirror, moved, uv::uvMirrorDrive(*mirror, moved)))
            if (r.corner < mu.entryOf.size() && r.source < mu.entryOf.size() && mu.entryOf[r.corner] != ~0u && mu.entryOf[r.source] != ~0u)
                out[mu.entryOf[r.corner]] = {mu.entryOf[r.source], r.linear};
        return out;
    }

    void updateDrag(Qt::KeyboardModifiers mods)
    {
        glm::dvec2 now = toUv(mMousePos), d = now - mPressUv;
        Tool t = mTool == Tool::Select ? Tool::Move : mTool;
        if (t == Tool::Move)
        {
            // Shift on a free move locks to whichever axis the drag first heads along, until release.
            QPointF px = mMousePos - mPressPos;
            if (mDrag != Handle::U && mDrag != Handle::V && (mods & Qt::ShiftModifier) && !mAxisLock && std::hypot(px.x(), px.y()) > 4.0)
                mAxisLock = std::abs(px.x()) >= std::abs(px.y()) ? 1 : 2;
            if (!(mods & Qt::ShiftModifier))
                mAxisLock = 0;
            const bool lockU = mDrag == Handle::U || mAxisLock == 1, lockV = mDrag == Handle::V || mAxisLock == 2;
            mDragMove = lockU ? glm::dvec2(d.x, 0.0) : lockV ? glm::dvec2(0.0, d.y) : d;
            if (mods & Qt::ControlModifier)
            {
                // Snap to sixty-fourths of the tile.
                mDragMove = glm::round(mDragMove * 64.0) / 64.0;
            }
        }
        else if (t == Tool::Rotate)
        {
            glm::dvec2 a = mPressUv - mPivot, b = now - mPivot;
            mDragAngle = std::atan2(a.x * b.y - a.y * b.x, glm::dot(a, b));
            if (mods & Qt::ShiftModifier)
                mDragAngle = std::round(mDragAngle / glm::radians(15.0)) * glm::radians(15.0);
        }
        else
        {
            QPointF px = mMousePos - mPressPos;
            double su = std::exp(px.x() / 120.0), sv = std::exp(-px.y() / 120.0);
            if (mDrag == Handle::U)
                mDragScale = {su, 1.0};
            else if (mDrag == Handle::V)
                mDragScale = {1.0, sv};
            else
            {
                double s = std::exp((px.x() - px.y()) / 160.0);
                mDragScale = {s, s};
            }
        }
    }

    void commitDrag()
    {
        Handle h = mDrag;
        mDrag = Handle::None;
        mDragMask.clear();
        mDragFollow.clear();
        (void)h;
        json call = {{"pivot", {mPivot.x, mPivot.y}}};
        bool any = false;
        if (glm::length(mDragMove) > 0.0)
        {
            call["translate"] = {mDragMove.x, mDragMove.y};
            any = true;
        }
        if (std::abs(mDragAngle) > 1e-9)
        {
            call["rotate"] = glm::degrees(mDragAngle);
            any = true;
        }
        if (mDragScale != glm::dvec2(1.0))
        {
            call["scale"] = {mDragScale.x, mDragScale.y};
            any = true;
        }
        if (any)
            safeRun("uvTransform", call, CallSource::UI);
        update();
    }

    struct Hit
    {
        size_t mesh = 0;
        Index id = geo::kInvalid;
    };

    static bool insidePoly(const std::vector<QPointF>& poly, QPointF p)
    {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
            if ((poly[i].y() > p.y()) != (poly[j].y() > p.y()) &&
                p.x() < (poly[j].x() - poly[i].x()) * (p.y() - poly[i].y()) / (poly[j].y() - poly[i].y()) + poly[i].x())
                in = !in;
        return in;
    }

    static double segDistance(QPointF p, QPointF a, QPointF b)
    {
        QPointF d = b - a;
        double l2 = d.x() * d.x() + d.y() * d.y();
        double t = l2 > 0.0 ? std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 0.0, 1.0) : 0.0;
        QPointF q = a + d * t - p;
        return std::hypot(q.x(), q.y());
    }

    ComponentMode pickMode() const
    {
        ComponentMode m = scene::components::mode();
        return m == ComponentMode::Object || uvLive::islandSelect() ? ComponentMode::Face : m;
    }

    Hit faceAt(QPointF s) const
    {
        Hit best;
        std::vector<QPointF> poly;
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        {
            const MeshUv& mu = mMeshes[mi];
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
            {
                poly.clear();
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    poly.push_back(toScreen(mu.uv[k]));
                if (insidePoly(poly, s))
                    best = {mi, mu.faces[fi]};
            }
        }
        return best;
    }

    /** What a click at s would select. */
    Hit hitAt(QPointF s, ComponentMode mode) const
    {
        Hit hit;
        double bestPx = mode == ComponentMode::Vertex ? 9.0 : 7.0;
        if (mode == ComponentMode::Vertex)
        {
            for (size_t mi = 0; mi < mMeshes.size(); ++mi)
                for (size_t k = 0; k < mMeshes[mi].uv.size(); ++k)
                {
                    QPointF d = toScreen(mMeshes[mi].uv[k]) - s;
                    double px = std::hypot(d.x(), d.y());
                    if (px < bestPx)
                    {
                        bestPx = px;
                        hit = {mi, mMeshes[mi].mesh->cornerPoint(mMeshes[mi].corner[k])};
                    }
                }
        }
        else if (mode == ComponentMode::Edge)
        {
            for (size_t mi = 0; mi < mMeshes.size(); ++mi)
                for (const Segment& sg : mMeshes[mi].segments)
                {
                    double px = segDistance(s, toScreen(mMeshes[mi].uv[sg.a]), toScreen(mMeshes[mi].uv[sg.b]));
                    if (px < bestPx)
                    {
                        bestPx = px;
                        hit = {mi, sg.edge};
                    }
                }
        }
        else
            hit = faceAt(s);
        return hit;
    }

    scene::componentPick::Hover hoverAt(QPointF s) const
    {
        ComponentMode mode = pickMode();
        Hit hit = hitAt(s, mode);
        if (hit.id == geo::kInvalid)
            return {};
        return {mode, mMeshes[hit.mesh].node, hit.id};
    }

    void setPreselect(const scene::componentPick::Hover& h)
    {
        if (h == mPreselect)
            return;
        mPreselect = h;
        update();
    }

    /** The hovered component, light gray and heavier than the selection's white. */
    void drawPreselect(QPainter& p)
    {
        if (mPreselect.empty() || mPreselect.mode != pickMode())
            return;
        auto it = std::find_if(mMeshes.begin(), mMeshes.end(), [&](const MeshUv& m) { return m.node == mPreselect.node; });
        if (it == mMeshes.end() || it->corner.empty())
            return;
        size_t mi = size_t(it - mMeshes.begin());
        const MeshUv& mu = *it;
        const geo::Mesh& m = *mu.mesh;
        const QColor hot = gray(200);
        // With symmetry the mirror image lights up too, since a click selects both.
        Index mirrorId = geo::kInvalid;
        if (const geo::SymmetryMap* sym = symmetryOf(mu))
            mirrorId = mPreselect.mode == ComponentMode::Vertex ? sym->mirrorPoint(mPreselect.id)
                       : mPreselect.mode == ComponentMode::Edge ? sym->mirrorEdge(m, mPreselect.id)
                                                                 : sym->mirrorFace(m, mPreselect.id);
        auto lit = [&](Index id) { return id == mPreselect.id || (mirrorId != geo::kInvalid && id == mirrorId); };
        if (mPreselect.mode == ComponentMode::Vertex)
        {
            p.setPen(QPen(gray(20), 1.0));
            p.setBrush(hot);
            for (size_t k = 0; k < mu.uv.size(); ++k)
                if (lit(m.cornerPoint(mu.corner[k])))
                    p.drawRect(QRectF(toScreen(drawnUv(mi, k)) - QPointF(4.0, 4.0), QSizeF(8.0, 8.0)));
        }
        else if (mPreselect.mode == ComponentMode::Edge)
        {
            p.setPen(QPen(hot, 3.5, Qt::SolidLine, Qt::RoundCap));
            for (const Segment& sg : mu.segments)
                if (lit(sg.edge))
                    p.drawLine(toScreen(drawnUv(mi, sg.a)), toScreen(drawnUv(mi, sg.b)));
        }
        else
        {
            auto hovered = std::find(mu.faces.begin(), mu.faces.end(), mPreselect.id);
            if (hovered == mu.faces.end())
                return;
            const size_t hf = size_t(hovered - mu.faces.begin());
            auto mirrored = mirrorId == geo::kInvalid ? mu.faces.end() : std::find(mu.faces.begin(), mu.faces.end(), mirrorId);
            const size_t hm = mirrored == mu.faces.end() ? hf : size_t(mirrored - mu.faces.begin());
            const bool island = uvLive::islandSelect();
            const std::vector<int>* islands = island ? &pickIslands(mi) : nullptr;
            p.setPen(QPen(hot, island ? 1.0 : 2.0));
            p.setBrush(gray(255, 40));
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
                if (island ? (*islands)[fi] == (*islands)[hf] || (*islands)[fi] == (*islands)[hm] : fi == hf || fi == hm)
                {
                    QPolygonF poly;
                    for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                        poly << toScreen(drawnUv(mi, k));
                    p.drawPolygon(poly);
                }
        }
    }

    /** Per shown face, the index of its island under islandOptions; built once per mesh. */
    const std::vector<int>& pickIslands(size_t mi)
    {
        MeshUv& mu = mMeshes[mi];
        if (mu.pickIsland.size() != mu.faces.size())
        {
            std::vector<int> faceIsland(mu.mesh->faceCapacity(), -1);
            std::vector<std::vector<Index>> islands = uv::islandFaces(*mu.mesh, {}, islandOptions(mu));
            for (size_t i = 0; i < islands.size(); ++i)
                for (Index f : islands[i])
                    faceIsland[f] = int(i);
            mu.pickIsland.resize(mu.faces.size());
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
                mu.pickIsland[fi] = faceIsland[mu.faces[fi]];
        }
        return mu.pickIsland;
    }

    void pickAt(QPointF s, Qt::KeyboardModifiers mods)
    {
        if (uvLive::islandSelect())
        {
            selectIslandAt(s, mods);
            return;
        }
        ComponentMode mode = pickMode();
        Hit hit = hitAt(s, mode);
        std::map<scene::NodeId, std::vector<Index>> hits;
        if (hit.id != geo::kInvalid)
            hits[mMeshes[hit.mesh].node].push_back(hit.id);
        apply(mode, hits, mods);
    }

    void pickRect(QRectF r, Qt::KeyboardModifiers mods)
    {
        ComponentMode mode = pickMode();
        std::map<scene::NodeId, std::vector<Index>> hits;
        for (const MeshUv& mu : mMeshes)
        {
            if (mu.corner.empty())
                continue;
            const geo::Mesh& m = *mu.mesh;
            std::vector<Index>& out = hits[mu.node];
            if (mode == ComponentMode::Vertex)
            {
                for (size_t k = 0; k < mu.uv.size(); ++k)
                    if (r.contains(toScreen(mu.uv[k])))
                        out.push_back(m.cornerPoint(mu.corner[k]));
            }
            else if (mode == ComponentMode::Edge)
            {
                for (const Segment& sg : mu.segments)
                    if (r.contains(toScreen(mu.uv[sg.a])) && r.contains(toScreen(mu.uv[sg.b])))
                        out.push_back(sg.edge);
            }
            else if (uvLive::islandSelect())
            {
                for (size_t fi = 0; fi < mu.faces.size(); ++fi)
                    for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                        if (r.contains(toScreen(mu.uv[k])))
                        {
                            out.push_back(mu.faces[fi]);
                            break;
                        }
                out = uv::islandsOfFaces(m, out, islandOptions(mu));
            }
            else
                for (size_t fi = 0; fi < mu.faces.size(); ++fi)
                {
                    glm::dvec2 c(0.0);
                    for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                        c += mu.uv[k];
                    c /= double(mu.faceStart[fi + 1] - mu.faceStart[fi]);
                    if (r.contains(toScreen(c)))
                        out.push_back(mu.faces[fi]);
                }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            if (out.empty())
                hits.erase(mu.node);
        }
        apply(mode, hits, mods);
    }

    static uv::ChartOptions islandOptions(const MeshUv& mu)
    {
        uv::ChartOptions co;
        co.source = uv::IslandSource::Uvs;
        co.uvSet = mu.set;
        return co;
    }

    void selectIslandAt(QPointF s, Qt::KeyboardModifiers mods)
    {
        Hit hit = faceAt(s);
        std::map<scene::NodeId, std::vector<Index>> hits;
        if (hit.id != geo::kInvalid)
        {
            const MeshUv& mu = mMeshes[hit.mesh];
            const Index seed[] = {hit.id};
            hits[mu.node] = uv::islandsOfFaces(*mu.mesh, seed, islandOptions(mu));
        }
        // A click on empty space in island mode clears, like the other modes; a double-click elsewhere does nothing.
        if (!hits.empty() || uvLive::islandSelect())
            apply(ComponentMode::Face, hits, mods);
    }

    void selectLoopAt(QPointF s, Qt::KeyboardModifiers mods)
    {
        Hit hit = hitAt(s, ComponentMode::Edge);
        if (hit.id == geo::kInvalid)
            return;
        const MeshUv& mu = mMeshes[hit.mesh];
        const scene::isolation::Visibility vis = scene::isolation::activeVisibility(mu.node, *mu.mesh);
        apply(ComponentMode::Edge, {{mu.node, scene::isolation::clipLoop(*mu.mesh, vis, geo::edgeLoop(*mu.mesh, hit.id), hit.id, ComponentMode::Edge, false)}},
              mods);
    }

    /** Same modifiers as the viewport: shift toggles, ctrl removes, both add. */
    void apply(ComponentMode mode, const std::map<scene::NodeId, std::vector<Index>>& hits, Qt::KeyboardModifiers mods)
    {
        scene::Graph& g = scene::Graph::current();
        bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
        const char* flag = mode == ComponentMode::Vertex ? "vertices" : mode == ComponentMode::Edge ? "edges" : "faces";
        UndoChunk chunk("Select UVs");
        if (hits.empty() && !shift && !ctrl)
        {
            for (const MeshUv& mu : mMeshes)
                if (g.node(mu.node))
                {
                    safeRun("selectComponents", {{"args", {mu.name}}, {flag, json::array()}}, CallSource::UI);
                    break;
                }
            return;
        }
        bool first = true;
        for (auto& [id, picked] : hits)
        {
            scene::Node* n = g.node(id);
            if (!n)
                continue;
            const std::vector<Index> ids = withMirror(id, mode, picked);
            json call = {{"args", {n->path()}}, {flag, ids}};
            if (shift && ctrl)
                call["add"] = true;
            else if (ctrl)
                call["deselect"] = true;
            else if (shift)
                call["toggle"] = true;
            else if (!first)
                call["add"] = true;
            first = false;
            safeRun("selectComponents", call, CallSource::UI);
        }
    }

    // Live editing: pins, the stretch display, drags that steer the solver, brushes and sewing by dragging.

    void drawPins(QPainter& p)
    {
        p.setPen(QPen(gray(10), 1.5));
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        {
            const MeshUv& mu = mMeshes[mi];
            if (mu.pinned.size() != mu.uv.size())
                continue;
            for (size_t k = 0; k < mu.uv.size(); ++k)
                if (mu.pinned[k])
                {
                    QPointF c = toScreen(drawnUv(mi, k));
                    p.setBrush(gray(250));
                    p.drawEllipse(c, 4.5, 4.5);
                    p.setBrush(gray(10));
                    p.drawEllipse(c, 1.2, 1.2);
                }
        }
    }

    /** Light = stretched (bigger in uv than on the surface), dark = compressed, relative to the face's island. */
    static QColor stretchColor(double v, bool twoColour)
    {
        v = std::clamp(v, -1.0, 1.0);
        if (!twoColour)
        {
            int g = int(std::lround(128.0 + 105.0 * v));
            return QColor(g, g, g, 225);
        }
        auto mix = [](QColor a, QColor b, double t) {
            return QColor(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
                          int(a.blue() + (b.blue() - a.blue()) * t), 225);
        };
        return v < 0.0 ? mix(QColor(128, 128, 128), QColor(45, 70, 150), -v) : mix(QColor(128, 128, 128), QColor(240, 165, 95), v);
    }

    /** Per face: log2 of its uv-to-surface area ratio over its island's, clamped to -1..1; folded faces read -1. */
    std::vector<double> faceStretch(size_t mi)
    {
        MeshUv& mu = mMeshes[mi];
        const geo::Mesh& m = *mu.mesh;
        const size_t nf = mu.faces.size();
        if (mu.island.size() != nf)
        {
            std::vector<uint8_t> seams = uv::seamFlags(m);
            uv::ChartOptions co;
            co.source = uv::IslandSource::Uvs;
            co.uvSet = mu.set;
            co.extraSeams = seams;
            std::vector<int> faceIsland(m.faceCapacity(), 0);
            std::vector<std::vector<Index>> islands = uv::islandFaces(m, {}, co);
            for (size_t i = 0; i < islands.size(); ++i)
                for (Index f : islands[i])
                    faceIsland[f] = int(i);
            mu.island.resize(nf);
            mu.area3.resize(nf);
            mu.islandRatio.assign(islands.size(), 0.0);
            for (size_t fi = 0; fi < nf; ++fi)
            {
                mu.island[fi] = faceIsland[mu.faces[fi]];
                glm::dvec3 sum(0.0), p0 = m.position(m.cornerPoint(mu.corner[mu.faceStart[fi]]));
                for (uint32_t k = mu.faceStart[fi] + 1; k + 1 < mu.faceStart[fi + 1]; ++k)
                    sum += glm::cross(m.position(m.cornerPoint(mu.corner[k])) - p0, m.position(m.cornerPoint(mu.corner[k + 1])) - p0);
                mu.area3[fi] = 0.5 * glm::length(sum);
            }
        }
        std::vector<double> auv(nf, 0.0), sumUv(mu.islandRatio.size(), 0.0), sum3(mu.islandRatio.size(), 0.0);
        for (size_t fi = 0; fi < nf; ++fi)
        {
            double a = 0.0;
            for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
            {
                uint32_t k2 = k + 1 < mu.faceStart[fi + 1] ? k + 1 : mu.faceStart[fi];
                glm::dvec2 p0 = drawnUv(mi, k), p1 = drawnUv(mi, k2);
                a += p0.x * p1.y - p1.x * p0.y;
            }
            auv[fi] = 0.5 * a;
            sumUv[size_t(mu.island[fi])] += auv[fi];
            sum3[size_t(mu.island[fi])] += mu.area3[fi];
        }
        std::vector<double> out(nf, 0.0);
        for (size_t fi = 0; fi < nf; ++fi)
        {
            size_t i = size_t(mu.island[fi]);
            double sign = sumUv[i] < 0.0 ? -1.0 : 1.0, ratio = sum3[i] > 0.0 ? std::abs(sumUv[i]) / sum3[i] : 0.0;
            double a = auv[fi] * sign;
            if (mu.area3[fi] <= 0.0 || ratio <= 0.0)
                continue;
            out[fi] = a <= 0.0 ? -1.0 : std::clamp(std::log2(a / mu.area3[fi] / ratio), -1.0, 1.0);
        }
        return out;
    }

    /** Pins or unpins the uv vertex nearest the cursor (P, or a click with the pin brushes). */
    void togglePinAt(QPointF s)
    {
        sync();
        double best = 12.0;
        size_t bm = 0, bk = SIZE_MAX;
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
            for (size_t k = 0; k < mMeshes[mi].uv.size(); ++k)
            {
                QPointF d = toScreen(drawnUv(mi, k)) - s;
                if (double px = std::hypot(d.x(), d.y()); px < best)
                {
                    best = px;
                    bm = mi;
                    bk = k;
                }
            }
        if (bk == SIZE_MAX)
            return;
        uv::live::Session* sess = uv::live::get(mMeshes[bm].node);
        if (!sess)
            return;
        auto [i, v] = sess->wedge(mMeshes[bm].corner[bk]);
        if (i < 0)
            return;
        bool wake = uvLive::liveMode();
        sess->begin("UV Pin");
        sess->solver().edit([i, v, wake](std::vector<uv::LiveIsland>& is) {
            if (size_t(i) >= is.size())
                return;
            is[size_t(i)].setPinned(v, !is[size_t(i)].pinned(v));
            if (wake)
                is[size_t(i)].wake();
        });
        sess->setStroking(false);
        uvLive::ensureDriver();
    }

    struct SewDrag
    {
        bool armed = false;
        bool dragging = false;
        size_t mesh = 0;
        size_t segment = 0;
    };

    /** The other sides of a split edge: segments of the same mesh edge drawn elsewhere. */
    std::vector<size_t> partners(size_t mi, size_t si) const
    {
        std::vector<size_t> out;
        const MeshUv& mu = mMeshes[mi];
        for (size_t k = 0; k < mu.segments.size(); ++k)
            if (k != si && mu.segments[k].edge == mu.segments[si].edge)
                out.push_back(k);
        return out;
    }

    /** In edge mode a press on a border edge that has a partner arms sewing by dragging it onto the partner. */
    bool armSewDrag(QPointF s)
    {
        if (pickMode() != ComponentMode::Edge)
            return false;
        double best = 7.0;
        bool found = false;
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
            for (size_t si = 0; si < mMeshes[mi].segments.size(); ++si)
            {
                const Segment& sg = mMeshes[mi].segments[si];
                if (!sg.seam)
                    continue;
                double d = segDistance(s, toScreen(mMeshes[mi].uv[sg.a]), toScreen(mMeshes[mi].uv[sg.b]));
                if (d < best && !partners(mi, si).empty())
                {
                    best = d;
                    mSew = {true, false, mi, si};
                    found = true;
                }
            }
        return found;
    }

    QPointF segmentMid(size_t mi, size_t si) const
    {
        const Segment& sg = mMeshes[mi].segments[si];
        return (toScreen(drawnUv(mi, sg.a)) + toScreen(drawnUv(mi, sg.b))) * 0.5;
    }

    void drawSewDrag(QPainter& p)
    {
        // Hovering a split edge links its two sides, so the partner is easy to find.
        if (!mSew.dragging && mPreselect.mode == ComponentMode::Edge && !mPreselect.empty())
            for (size_t mi = 0; mi < mMeshes.size(); ++mi)
            {
                if (mMeshes[mi].node != mPreselect.node)
                    continue;
                std::vector<size_t> sides;
                for (size_t si = 0; si < mMeshes[mi].segments.size(); ++si)
                    if (mMeshes[mi].segments[si].edge == mPreselect.id)
                        sides.push_back(si);
                if (sides.size() == 2)
                {
                    QPointF a = segmentMid(mi, sides[0]), b = segmentMid(mi, sides[1]);
                    if (std::hypot(a.x() - b.x(), a.y() - b.y()) > 2.0)
                    {
                        p.setPen(QPen(gray(200), 1.0, Qt::DashLine));
                        p.drawLine(a, b);
                    }
                }
            }
        if (!mSew.dragging)
            return;
        const MeshUv& mu = mMeshes[mSew.mesh];
        for (size_t k : partners(mSew.mesh, mSew.segment))
        {
            const Segment& sg = mu.segments[k];
            QPointF a = toScreen(drawnUv(mSew.mesh, sg.a)), b = toScreen(drawnUv(mSew.mesh, sg.b));
            bool near = segDistance(mMousePos, a, b) <= 25.0;
            p.setPen(QPen(gray(255), near ? 5.0 : 3.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(a, b);
        }
        p.setPen(QPen(gray(235), 1.5, Qt::DashLine));
        p.drawLine(segmentMid(mSew.mesh, mSew.segment), mMousePos);
    }

    /** Released over the partner side: Move and Sew that edge (the smaller island travels). */
    void finishSewDrag(QPointF s)
    {
        if (mSew.mesh >= mMeshes.size())
            return;
        const MeshUv& mu = mMeshes[mSew.mesh];
        bool hit = false;
        for (size_t k : partners(mSew.mesh, mSew.segment))
        {
            const Segment& sg = mu.segments[k];
            hit |= segDistance(s, toScreen(mu.uv[sg.a]), toScreen(mu.uv[sg.b])) <= 25.0;
        }
        if (!hit)
            return;
        Index edge = mu.segments[mSew.segment].edge;
        scene::NodeId node = mu.node;
        std::vector<Index> faces;
        mu.mesh->forEachEdgeCorner(edge, [&](Index c) { faces.push_back(mu.mesh->cornerFace(c)); });
        safeRun("uvSew", {{"args", {mu.name}}, {"edges", {edge}}, {"relax", uvLive::liveMode()}}, CallSource::UI);
        uvLive::settleAfter(node, faces, "UV Live Settle");
    }

    struct LiveDrag
    {
        bool armed = false;
        bool started = false;
        size_t mesh = 0;
        ComponentMode mode = ComponentMode::Vertex;
        Index id = geo::kInvalid;
        bool island = false;
        scene::NodeId node = scene::kNoNode;
        glm::dvec2 pressUv{0.0};
        std::vector<std::tuple<int, Index, glm::dvec2>> wedges; // island, vertex, start
        std::vector<int> islands;
    };

    /** In live mode a press on a uv component arms a drag of it; it starts once the cursor moves. */
    bool armLiveDrag(QPointF s)
    {
        ComponentMode mode = pickMode();
        Hit hit = hitAt(s, mode);
        if (hit.id == geo::kInvalid)
            return false;
        mLiveDrag = {};
        mLiveDrag.armed = true;
        mLiveDrag.mesh = hit.mesh;
        mLiveDrag.mode = mode;
        mLiveDrag.id = hit.id;
        mLiveDrag.island = (mMods & Qt::ShiftModifier) != 0;
        mLiveDrag.pressUv = toUv(s);
        return true;
    }

    /** The grabbed uvs become soft pins that follow the cursor; the islands relax around them. */
    void startLiveDrag()
    {
        LiveDrag& d = mLiveDrag;
        d.started = true;
        if (d.mesh >= mMeshes.size())
            return;
        const MeshUv& mu = mMeshes[d.mesh];
        uv::live::Session* s = uv::live::get(mu.node);
        if (!s)
            return;
        d.node = mu.node;
        const geo::Mesh& m = *mu.mesh;
        std::vector<uint8_t> use(mu.corner.size(), 0);
        if (d.mode == ComponentMode::Vertex)
        {
            for (size_t k = 0; k < mu.corner.size(); ++k)
            {
                QPointF q = toScreen(mu.uv[k]) - mPressPos;
                if (m.cornerPoint(mu.corner[k]) == d.id && std::hypot(q.x(), q.y()) < 12.0)
                    use[k] = 1;
            }
        }
        else if (d.mode == ComponentMode::Edge)
        {
            double best = 1e30;
            const Segment* pick = nullptr;
            for (const Segment& sg : mu.segments)
                if (sg.edge == d.id)
                    if (double px = segDistance(mPressPos, toScreen(mu.uv[sg.a]), toScreen(mu.uv[sg.b])); px < best)
                    {
                        best = px;
                        pick = &sg;
                    }
            if (pick)
                use[pick->a] = use[pick->b] = 1;
        }
        else
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
                if (mu.faces[fi] == d.id)
                    for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                        use[k] = 1;
        // With symmetry the grabbed side drives and its partners follow it mirrored, so only that side is held.
        const uv::UvMirror* mirror = mirrorOf(mu);
        int grabSide = 0;
        if (mirror)
        {
            std::vector<Index> grabbed;
            for (size_t k = 0; k < use.size(); ++k)
                if (use[k])
                    grabbed.push_back(mu.corner[k]);
            grabSide = uv::uvMirrorDrive(*mirror, grabbed);
        }
        // Grabbing part of the selection drags the whole selection.
        std::vector<uint8_t> sel = selectedEntries(mu);
        bool inSelection = false;
        for (size_t k = 0; k < use.size(); ++k)
            inSelection |= use[k] && sel[k];
        if (inSelection)
            for (size_t k = 0; k < use.size(); ++k)
                use[k] |= sel[k];
        if (mirror)
            for (size_t k = 0; k < use.size(); ++k)
                if (mirror->side[mu.corner[k]] == -grabSide)
                    use[k] = 0;
        std::set<int> islandSet;
        for (size_t k = 0; k < use.size(); ++k)
            if (use[k])
                islandSet.insert(s->wedge(mu.corner[k]).first);
        std::set<std::pair<int, Index>> seen;
        for (size_t k = 0; k < mu.corner.size(); ++k)
        {
            auto w = s->wedge(mu.corner[k]);
            bool take = use[k] || (d.island && islandSet.count(w.first));
            if (w.first < 0 || !take || !seen.insert(w).second)
                continue;
            d.wedges.emplace_back(w.first, w.second, mu.uv[k]);
        }
        d.islands.assign(islandSet.begin(), islandSet.end());
        std::erase(d.islands, -1);
        if (d.wedges.empty())
            return;
        s->begin("UV Live Drag");
        s->setStroking(true);
        updateLiveDrag();
        uvLive::ensureDriver();
    }

    void updateLiveDrag()
    {
        uv::live::Session* s = uv::live::get(mLiveDrag.node, false);
        if (!s || mLiveDrag.wedges.empty())
            return;
        glm::dvec2 delta = toUv(mMousePos) - mLiveDrag.pressUv;
        s->solver().edit([w = mLiveDrag.wedges, islands = mLiveDrag.islands, delta](std::vector<uv::LiveIsland>& is) {
            for (auto& [i, v, start] : w)
                if (size_t(i) < is.size())
                    is[size_t(i)].setTarget(v, start + delta, 1.0);
            for (int i : islands)
                if (size_t(i) < is.size())
                    is[size_t(i)].wake();
        });
    }

    /** Release lets the soft pins go (Ctrl keeps them as pins where they are). */
    void endLiveDrag(Qt::KeyboardModifiers mods)
    {
        uv::live::Session* s = uv::live::get(mLiveDrag.node, false);
        if (!s)
            return;
        bool pin = mods & Qt::ControlModifier;
        s->solver().edit([w = mLiveDrag.wedges, islands = mLiveDrag.islands, pin](std::vector<uv::LiveIsland>& is) {
            if (pin)
                for (auto& [i, v, start] : w)
                    if (size_t(i) < is.size())
                        is[size_t(i)].setPinned(v, true);
            for (int i : islands)
                if (size_t(i) < is.size())
                {
                    is[size_t(i)].clearTargets();
                    is[size_t(i)].wake();
                }
        });
        s->setStroking(false);
    }

    enum class Adjust
    {
        None,
        Radius,
        Strength
    };

    struct BrushStroke
    {
        bool active = false;
        bool started = false;
        std::vector<scene::NodeId> nodes;
        QPointF last;
        glm::dvec2 lastUv{0.0}, fromUv{0.0};
        uv::LiveBrush brush = uv::LiveBrush::Relax;
        bool invert = false;
    };

    /** B or F held + drag sizes the brush, M or Shift+F + drag sets its strength, [ ] resize; as in the sculpt tool. */
    bool brushKey(QKeyEvent* e, bool down)
    {
        int k = e->key();
        if (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight)
        {
            if (down)
            {
                uvLive::brush().radius = std::clamp(uvLive::brush().radius * (k == Qt::Key_BracketLeft ? 0.8 : 1.25), 2.0, 2000.0);
                uvLive::settingsChanged();
                update();
            }
            return true;
        }
        Adjust kind = Adjust::None;
        if (k == Qt::Key_B || (k == Qt::Key_F && !(e->modifiers() & Qt::ShiftModifier)))
            kind = Adjust::Radius;
        else if (k == Qt::Key_M || (k == Qt::Key_F && (e->modifiers() & Qt::ShiftModifier)))
            kind = Adjust::Strength;
        if (kind == Adjust::None)
            return false;
        if (e->isAutoRepeat())
            return true;
        if (down)
        {
            mArmed = kind;
            mArmedKey = k;
            mArmedUsed = false;
            return true;
        }
        if (k != mArmedKey)
            return true;
        // A tapped F still frames the selection.
        if (k == Qt::Key_F && !mArmedUsed && mAdjusting == Adjust::None)
            frameSelection();
        mArmed = Adjust::None;
        mArmedKey = 0;
        return true;
    }

    void brushPress(QMouseEvent* e)
    {
        if (mArmed != Adjust::None)
        {
            mAdjusting = mArmed;
            mArmedUsed = true;
            mAdjustStart = e->position();
            mAdjustFrom = mAdjusting == Adjust::Radius ? uvLive::brush().radius : uvLive::brush().strength;
            return;
        }
        sync();
        BrushStroke& b = mStroke;
        b = {};
        b.active = true;
        b.brush = (e->modifiers() & Qt::ShiftModifier) ? uv::LiveBrush::Relax : uvLive::brush().brush;
        b.invert = (e->modifiers() & Qt::ControlModifier) != 0;
        b.last = e->position();
        b.lastUv = b.fromUv = toUv(e->position());
        std::string label = "UV " + std::string(uv::liveBrushName(b.brush)) + " Brush";
        for (const MeshUv& mu : mMeshes)
            if (!mu.corner.empty())
                if (uv::live::Session* s = uv::live::get(mu.node))
                {
                    s->begin(label);
                    s->setStroking(true);
                    b.nodes.push_back(mu.node);
                }
        uvLive::ensureDriver();
        // The pin brushes wait for a drag, so a click can toggle one pin instead.
        if (b.brush != uv::LiveBrush::Pin && b.brush != uv::LiveBrush::Unpin)
        {
            dabAt(e->position(), true);
            b.started = true;
        }
    }

    void brushMove(QMouseEvent* e)
    {
        QPointF pos = e->position();
        if (mAdjusting != Adjust::None)
        {
            double dx = pos.x() - mAdjustStart.x();
            if (mAdjusting == Adjust::Radius)
                uvLive::brush().radius = std::clamp(mAdjustFrom + dx, 2.0, 2000.0);
            else
                uvLive::brush().strength = std::clamp(mAdjustFrom + dx / 300.0, 0.0, 1.0);
            uvLive::settingsChanged();
            return;
        }
        BrushStroke& b = mStroke;
        if (!b.active)
            return;
        if (!b.started)
        {
            QPointF d = pos - mPressPos;
            if (std::abs(d.x()) + std::abs(d.y()) < 3.0)
                return;
            dabAt(b.last, true);
            b.started = true;
        }
        if (b.brush == uv::LiveBrush::Grab)
        {
            dabAt(pos, false);
            b.last = pos;
            return;
        }
        QPointF d = pos - b.last;
        double len = std::hypot(d.x(), d.y()), step = std::max(2.0, uvLive::brush().radius * 0.15);
        int count = int(len / step);
        for (int k = 1; k <= count; ++k)
            dabAt(b.last + d * (k * step / len), false);
        if (count > 0)
            b.last += d * (count * step / len);
    }

    void dabAt(QPointF px, bool first)
    {
        BrushStroke& b = mStroke;
        uv::LiveDab d;
        d.brush = b.brush;
        d.center = toUv(px);
        d.radius = uvLive::brush().radius / mZoom;
        d.strength = uvLive::brush().strength;
        d.invert = b.invert;
        d.delta = d.center - b.lastUv;
        d.lineFrom = b.fromUv;
        d.first = first;
        b.lastUv = d.center;
        bool wake = uvLive::liveMode();
        for (scene::NodeId node : b.nodes)
            if (uv::live::Session* s = uv::live::get(node, false))
                s->solver().edit([d, wake](std::vector<uv::LiveIsland>& is) {
                    for (uv::LiveIsland& i : is)
                        if (i.applyDab(d) && wake)
                            i.wake();
                });
    }

    void brushRelease(QMouseEvent* e)
    {
        if (mAdjusting != Adjust::None)
        {
            mAdjusting = Adjust::None;
            return;
        }
        BrushStroke b = std::move(mStroke);
        mStroke = {};
        if (!b.active)
            return;
        bool wake = uvLive::liveMode();
        for (scene::NodeId node : b.nodes)
            if (uv::live::Session* s = uv::live::get(node, false))
            {
                if (b.brush == uv::LiveBrush::Grab)
                    s->solver().edit([wake](std::vector<uv::LiveIsland>& is) {
                        for (uv::LiveIsland& i : is)
                            if (i.hasTargets())
                            {
                                i.clearTargets();
                                if (wake)
                                    i.wake();
                            }
                    });
                s->setStroking(false);
            }
        if (!b.started)
            togglePinAt(e->position());
    }

    void drawBrush(QPainter& p)
    {
        if (mTool != Tool::Brush)
            return;
        QPointF c = mAdjusting != Adjust::None ? mAdjustStart : mMousePos;
        double r = uvLive::brush().radius;
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(gray(240), 1.3));
        p.drawEllipse(c, r, r);
        if (mAdjusting == Adjust::Strength)
        {
            p.setPen(QPen(gray(160), 1.0));
            p.drawEllipse(c, r * uvLive::brush().strength, r * uvLive::brush().strength);
        }
    }

    SewDrag mSew;
    LiveDrag mLiveDrag;
    BrushStroke mStroke;
    Adjust mArmed = Adjust::None;
    Adjust mAdjusting = Adjust::None;
    int mArmedKey = 0;
    bool mArmedUsed = false;
    QPointF mAdjustStart;
    double mAdjustFrom = 0.0;

    std::vector<MeshUv> mMeshes;
    bool mFramed = false;
    glm::dvec2 mPan{0.0};
    double mZoom = 300.0;
    UvTexturePaint mTexPaint;
    Tool mTool = Tool::Select;
    int mUvToolListener = 0;
    bool mUvToolDrag = false;
    Display mDisplay = Display::Checker;
    bool mUdim = false;
    bool mShowStacks = false;
    /** Stacked islands per shown mesh: a badge position (uv) and how many islands share it. */
    struct StackBadges
    {
        scene::MeshPtr mesh;
        std::string set;
        std::vector<std::pair<glm::dvec2, int>> badges;
    };
    std::vector<StackBadges> mStacks;

    Nav mNav = Nav::None;
    bool mMarquee = false;
    Qt::KeyboardModifiers mMods;
    QPointF mPressPos, mMousePos, mLastPos;
    Handle mHover = Handle::None;
    scene::componentPick::Hover mPreselect;
    Handle mDrag = Handle::None;
    int mAxisLock = 0; // a Shift free move: 1 along U, 2 along V
    glm::dvec2 mPivot{0.5}, mPressUv{0.0}, mDragMove{0.0}, mDragScale{1.0};
    double mDragAngle = 0.0;
    std::vector<std::vector<uint8_t>> mDragMask;
    std::vector<std::vector<std::pair<uint32_t, glm::dmat2>>> mDragFollow;
};

namespace
{
QToolButton* button(const QString& text, const QString& tip, QWidget* parent, const char* iconName = nullptr)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(false);
    if (iconName)
    {
        b->setIcon(icon(iconName));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    return b;
}

QDoubleSpinBox* spin(double lo, double hi, double value, int decimals, QWidget* parent)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setValue(value);
    s->setKeyboardTracking(false);
    return s;
}
} // namespace

UvEditor::UvEditor(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // Tools and display.
    auto* row1 = new FlowLayout();
    auto* tools = new QButtonGroup(this);
    tools->setExclusive(true);
    const std::tuple<const char*, const char*, UvView::Tool> toolDefs[] = {{"Select", "pointer", UvView::Tool::Select},
                                                                          {"Move", "arrows-move", UvView::Tool::Move},
                                                                          {"Rotate", "rotate", UvView::Tool::Rotate},
                                                                          {"Scale", "resize", UvView::Tool::Scale},
                                                                          {"Brush", "brush", UvView::Tool::Brush}};
    std::vector<QToolButton*> toolButtons;
    for (auto [name, ic, t] : toolDefs)
    {
        QToolButton* b = button(name, QString(name) + " uvs (Q W E R in the view)", this, ic);
        b->setCheckable(true);
        tools->addButton(b);
        row1->addWidget(b);
        toolButtons.push_back(b);
        connect(b, &QToolButton::clicked, this, [this, t] { mView->setTool(t); });
    }
    toolButtons.front()->setChecked(true);
    toolButtons.back()->setToolTip("Brush the uvs with the brush picked in the live row. B or F held + drag: size, M + drag: strength, "
                                   "Ctrl inverts, Shift relaxes. A click with the pin brushes toggles one pin");
    auto* modes = new QButtonGroup(this);
    modes->setExclusive(true);
    const std::tuple<const char*, const char*, const char*, const char*> modeDefs[] = {
        {"Vertex", "vertex", "Select uv points (F9 in the view)", "point"},
        {"Edge", "edge", "Select uv edges (F10 in the view)", "line"},
        {"Face", "face", "Select uv faces (F11 in the view)", "square"},
        {"Island", "island", "Select whole uv islands (F12 in the view)", "shape"}};
    std::vector<QToolButton*> modeButtons;
    for (auto [label, mode, tip, ic] : modeDefs)
    {
        QToolButton* b = button(label, tip, this, ic);
        b->setCheckable(true);
        modes->addButton(b);
        row1->addWidget(b);
        modeButtons.push_back(b);
        connect(b, &QToolButton::clicked, this, [m = std::string(mode)] { setPickMode(m); });
    }
    auto showMode = [modes, modeButtons] {
        ComponentMode m = scene::components::mode();
        if (uvLive::islandSelect() && m != ComponentMode::Face)
            uvLive::setIslandSelect(false);
        int i = uvLive::islandSelect() ? 3 : m == ComponentMode::Vertex ? 0 : m == ComponentMode::Edge ? 1 : m == ComponentMode::Face ? 2 : -1;
        modes->setExclusive(false);
        for (size_t k = 0; k < modeButtons.size(); ++k)
            modeButtons[k]->setChecked(int(k) == i);
        modes->setExclusive(true);
    };
    showMode();
    QToolButton* frameBtn = button("Frame", "Frame the selection (F), or everything (A)", this, "focus-centered");
    row1->addWidget(frameBtn);
    row1->addWidget(new QLabel("Set", this));
    mSet = new QComboBox(this);
    mSet->setEditable(true);
    mSet->setMinimumContentsLength(4);
    mSet->setToolTip("UV set the editor and tools work on (uvSetCurrent)");
    row1->addWidget(mSet);
    mDisplay = new QComboBox(this);
    mDisplay->addItems({"Plain", "Checker", "Distortion", "Stretch", "Islands"});
    mDisplay->setCurrentIndex(1);
    mDisplay->setToolTip("Distortion shades distorted faces brighter; Stretch shades stretched faces light and compressed ones dark; "
                         "Islands shades each island apart");
    row1->addWidget(mDisplay);
    auto* udim = new QCheckBox("UDIM", this);
    udim->setIcon(icon("grid-4x4"));
    row1->addWidget(udim);
    // A texture set channel under the UVs, and the texture paint brush in UV space.
    auto* texture = new QToolButton(this);
    texture->setText("Texture");
    texture->setIcon(icon("photo"));
    texture->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    texture->setPopupMode(QToolButton::InstantPopup);
    texture->setToolTip("Show a texture set channel under the UVs (drawn on the GPU at the view's resolution)");
    auto* textureMenu = new QMenu(texture);
    texture->setMenu(textureMenu);
    connect(textureMenu, &QMenu::aboutToShow, this, [this, textureMenu, texture] {
        textureMenu->clear();
        textureMenu->addAction("None", this, [this, texture] {
            mView->texturePaint().setChannel({}, {});
            texture->setText("Texture");
        });
        for (const auto& [set, channel] : UvTexturePaint::choices())
        {
            const QString label = QString::fromStdString(set + " / " + channel);
            textureMenu->addAction(label, this, [this, texture, set, channel, label] {
                mView->texturePaint().setChannel(set, channel);
                texture->setText(texture->fontMetrics().elidedText(label, Qt::ElideMiddle, 160));
            });
        }
    });
    row1->addWidget(texture);
    auto* paint = new QCheckBox("Paint", this);
    paint->setIcon(icon("brush"));
    paint->setToolTip("Paint the shown texture set in UV space with the Texture Paint tool's brush and channels");
    connect(paint, &QCheckBox::toggled, this, [this](bool on) {
        mView->texturePaint().setPainting(on);
        mView->update();
    });
    row1->addWidget(paint);
    root->addLayout(row1);

    // Operations.
    auto* row2 = new FlowLayout();
    auto addOp = [&](const char* ic, const QString& text, const QString& tip, std::function<void()> fn) {
        QToolButton* b = button(text, tip, this, ic);
        connect(b, &QToolButton::clicked, this, std::move(fn));
        row2->addWidget(b);
        return b;
    };
    auto packCall = [this] { return json{{"resolution", mResolution->value()}, {"padding", mPadding->value()}}; };
    addOp("wand", "Auto", "Seams, unwrap and pack in one step (uvAuto)", [this, packCall] { runCommand("uvAuto", packCall()); });
    addOp("line-dashed", "Seams", "Place seams automatically (uvAutoSeams)", [this] { runCommand("uvAutoSeams"); });
    addOp("cube-unfolded", "Unwrap", "Unwrap along the seams (uvUnwrap)", [this] { runCommand("uvUnwrap"); });
    QToolButton* projectBtn = addOp("cube-send", "Project", "Planar, box, cylindrical or spherical projection (uvProject)", [] {});
    auto* projectMenu = new QMenu(projectBtn);
    for (auto [label, type, axis] : {std::tuple{"Planar (fit)", "planar", "best"}, {"Planar X", "planar", "x"}, {"Planar Y", "planar", "y"},
                                     {"Planar Z", "planar", "z"}, {"Box", "box", "best"}, {"Cylindrical", "cylindrical", "best"},
                                     {"Spherical", "spherical", "best"}})
    {
        std::string t = type, a = axis;
        projectMenu->addAction(label, this, [this, t, a] { runCommand("uvProject", {{"type", t}, {"axis", a}}); });
    }
    projectBtn->setMenu(projectMenu);
    projectBtn->setPopupMode(QToolButton::InstantPopup);
    setRightClickAction(projectBtn, projectMenu->actions().front());
    // Cut and sew re-solve the islands they touch straight away in live mode.
    auto cutOrSew = [this](const char* command, json call) {
        json r = uvLive::cutOrSew(command, std::move(call));
        if (!r.is_null())
            mStatus->setText(QString(command) + ": " + QString::fromStdString(r.dump()));
        mView->invalidate();
    };
    addOp("scissors", "Cut", "Cut the uvs along the selected edges and let the cut open (uvCut)", [cutOrSew] { cutOrSew("uvCut", json::object()); });
    addOp("needle-thread", "Sew", "Move and Sew: weld the selected seams, the smaller island travelling to meet the other (uvSew). In edge mode you "
                 "can also drag a border edge onto its partner",
          [cutOrSew] { cutOrSew("uvSew", json{{"move", true}}); });
    addOp("link", "Weld", "Weld the selected seams where they are, both sides meeting halfway (uvSew move=False)",
          [cutOrSew] { cutOrSew("uvSew", json{{"move", false}}); });
    addOp("layout-grid", "Straighten", "Rectify islands into grids and rectangles (uvStraighten)", [this] { runCommand("uvStraighten"); });
    addOp("arrow-up-circle", "Orient", "Turn islands upright (uvOrient)", [this] { runCommand("uvOrient"); });
    auto addMenuOp = [&](const char* ic, const QString& text, const QString& tip,
                         std::initializer_list<std::tuple<const char*, const char*, json>> items) {
        QToolButton* b = addOp(ic, text, tip, [] {});
        auto* menu = new QMenu(b);
        for (const auto& [label, command, call] : items)
        {
            std::string c = command;
            json j = call;
            menu->addAction(label, this, [this, c, j] { runCommand(c.c_str(), j); });
        }
        b->setMenu(menu);
        b->setPopupMode(QToolButton::InstantPopup);
        setRightClickAction(b, menu->actions().front());
    };
    addMenuOp("circle-dashed", "BFF", "Boundary First Flattening: conformal unwrap with the boundary chosen first (uvBff)",
              {{"Free boundary (least area stretch)", "uvBff", json::object()},
               {"Disk", "uvBff", json{{"boundary", "disk"}}},
               {"Rectangle (selected or sharpest corners)", "uvBff", json{{"boundary", "rectangle"}}},
               {"Polygon (selected corners)", "uvBff", json{{"boundary", "polygon"}}},
               {"Free with 4 cones", "uvBff", json{{"cones", 4}}},
               {"Free with 8 cones", "uvBff", json{{"cones", 8}}}});
    addMenuOp("vector-triangle", "Optimize", "Relax islands from where they are, keeping angles, areas or a blend (uvOptimize, uvMosaic)",
              {{"Conformal (angles)", "uvOptimize", json{{"mode", "conformal"}}},
               {"Authalic (areas)", "uvOptimize", json{{"mode", "authalic"}}},
               {"Blend", "uvOptimize", json{{"mode", "blend"}}},
               {"Isometric (SLIM)", "uvOptimize", json{{"mode", "isometric"}}},
               {"Mosaic: split stretched islands", "uvMosaic", json::object()}});
    addMenuOp("route", "Seams 2", "Auto Seams 2: seams and stretch optimised together, short and hidden (uvAutoSeams method=optcuts)",
              {{"Auto Seams 2", "uvAutoSeams", json{{"method", "optcuts"}}},
               {"Auto Seams 2, tighter (bound 0.02)", "uvAutoSeams", json{{"method", "optcuts"}, {"distortionBound", 0.02}}},
               {"Auto Seams 2, looser (bound 0.1)", "uvAutoSeams", json{{"method", "optcuts"}, {"distortionBound", 0.1}}}});
    addMenuOp("flip-horizontal", "Symmetric", "Unwrap one side and mirror it to the other (uvUnwrapSymmetric)",
              {{"Mirror flipped", "uvUnwrapSymmetric", json::object()}, {"Mirror stacked", "uvUnwrapSymmetric", json{{"mode", "stack"}}}});
    addMenuOp("align-left", "Align", "Straighten selected edges, align island sides to U or V (uvStraightenEdges, uvAlignToEdge)",
              {{"Straighten edges", "uvStraightenEdges", json::object()},
               {"Align side to U", "uvAlignToEdge", json{{"axis", "u"}}},
               {"Align side to V", "uvAlignToEdge", json{{"axis", "v"}}},
               {"Orient by edge", "uvAlignToEdge", json::object()}});
    addOp("packages", "Pack", "Pack islands with padding for the resolution and the Pack options (uvPack)", [this] { runPack(); });
    QToolButton* packOptions = addOp("adjustments", "Pack Options", "Show the Pack options: rotation, scale, stacking, tiles, groups, effort", [] {});
    packOptions->setCheckable(true);
    addOp("bulb", "Lightmap", "Generate Unreal lightmap uvs in uv1 (uvLightmap)", [this] { runCommand("uvLightmap"); });
    addOp("info-circle", "Info", "Islands, overlaps, distortion and density (uvLayout)", [this] { showLayout(); });
    root->addLayout(row2);

    // Texture size, padding and texel density.
    auto* row3 = new FlowLayout();
    row3->addWidget(new QLabel("Res", this));
    mResolution = new TextureSizeBox(this);
    mResolution->setRange(16, 16384);
    mResolution->setValue(1024);
    mResolution->setToolTip("Texture size for packing and texel density, px");
    row3->addWidget(mResolution);
    row3->addWidget(new QLabel("Pad", this));
    mPadding = spin(0.0, 64.0, 4.0, 1, this);
    mPadding->setToolTip("Texels between islands");
    row3->addWidget(mPadding);
    row3->addWidget(new QLabel("Density", this));
    mDensity = spin(0.0, 100000.0, 10.24, 3, this);
    mDensity->setToolTip("Texel density, px/cm");
    row3->addWidget(mDensity);
    QToolButton* getTd = button("Get", "Measure texel density of the selection", this, "color-picker");
    QToolButton* setTd = button("Set", "Scale each selected island to the density", this, "ruler-measure");
    row3->addWidget(getTd);
    row3->addWidget(setTd);
    root->addLayout(row3);

    mPackPanel = new UvPackPanel(this);
    mPackPanel->setVisible(false);
    root->addWidget(mPackPanel);
    mPackPanel->onPack = [this] { runPack(); };
    mPackPanel->onShowStacks = [this](bool on) { mView->setShowStacks(on); };
    connect(packOptions, &QToolButton::toggled, mPackPanel, &QWidget::setVisible);

    // Live solving: the mode, one-shot solves, the stretch/compress balance, brushes, pins and the 3D display.
    auto* row5 = new FlowLayout();
    auto* live = button("Live", "Live mode: drag uvs and the island relaxes around them (Ctrl on release pins them); cuts, sews and pins "
                                "re-solve as they happen",
                        this, "bolt");
    live->setCheckable(true);
    live->setChecked(uvLive::liveMode());
    row5->addWidget(live);
    QToolButton* relaxBtn = button("Relax", "Relax the selected islands (or all) live until they settle", this, "ripple");
    QToolButton* flattenBtn = button("Flatten", "Solve the selected islands from scratch, keeping pins, size and placement, then relax live", this,
                                    "arrows-diagonal-minimize-2");
    QToolButton* settleBtn = button("Settle", "Relax to convergence and finish with a fold-free SLIM pass", this, "circle-check");
    row5->addWidget(relaxBtn);
    row5->addWidget(flattenBtn);
    row5->addWidget(settleBtn);
    row5->addWidget(new QLabel("Squash", this));
    auto* balance = new QSlider(Qt::Horizontal, this);
    balance->setRange(-100, 100);
    balance->setValue(int(std::lround(uvLive::balance() * 100.0)));
    balance->setFixedWidth(90);
    balance->setToolTip("Stretch against compression: right tolerates stretch and fixes compression, left the reverse, middle treats both "
                        "alike");
    row5->addWidget(balance);
    row5->addWidget(new QLabel("Stretch", this));
    auto* brushCombo = new QComboBox(this);
    for (int i = 0; i <= int(uv::LiveBrush::Soften); ++i)
    {
        std::string_view n = uv::liveBrushName(uv::LiveBrush(i));
        brushCombo->addItem(QString::fromUtf8(n.data(), qsizetype(n.size())));
    }
    brushCombo->setCurrentIndex(int(uvLive::brush().brush));
    brushCombo->setToolTip("Brush for the Brush tool here and the UV brush on the mesh (uv.brush3d)");
    row5->addWidget(brushCombo);
    QToolButton* brush3d = button("3D", "Paint the brush on the mesh in the viewport (uv.brush3d)", this, "cube");
    row5->addWidget(brush3d);
    QToolButton* pinBtn = button("Pin", "Pin the selected uvs (P pins the one under the cursor)", this, "pin");
    QToolButton* unpinBtn = button("Unpin", "Unpin the selected uvs", this, "pinned-off");
    row5->addWidget(pinBtn);
    row5->addWidget(unpinBtn);
    auto* checker = new QCheckBox("3D checker", this);
    checker->setChecked(uvLive::checker3d());
    checker->setIcon(icon("chess"));
    checker->setToolTip("Checker texture on the meshes in the viewport");
    auto* tiling = new QSpinBox(this);
    tiling->setRange(1, 256);
    tiling->setValue(uvLive::checkerTiling());
    tiling->setKeyboardTracking(false);
    tiling->setToolTip("Checks across one 0-1 tile, in the viewport and the editor's checker display");
    auto* seams = new QCheckBox("3D seams", this);
    seams->setChecked(uvLive::seams3d());
    seams->setIcon(icon("line-dashed"));
    seams->setToolTip("Draw the uv seams over the meshes in the viewport");
    auto* children = new QCheckBox("Children", this);
    children->setChecked(uv::includeChildren());
    children->setIcon(icon("hierarchy"));
    children->setToolTip("Show and edit the uvs of meshes under the selected objects too, not only the selected meshes");
    auto* twoColour = new QCheckBox("2-colour", this);
    twoColour->setChecked(uvLive::twoColourHeatmap());
    twoColour->setIcon(icon("palette"));
    twoColour->setToolTip("Stretch display in two colours instead of grayscale");
    row5->addWidget(checker);
    row5->addWidget(tiling);
    row5->addWidget(seams);
    row5->addWidget(children);
    row5->addWidget(twoColour);
    root->addLayout(row5);
    connect(live, &QToolButton::toggled, this, [](bool on) { uvLive::setLiveMode(on); });
    connect(relaxBtn, &QToolButton::clicked, this, [] { uvLive::run(uvLive::Op::Relax); });
    connect(flattenBtn, &QToolButton::clicked, this, [] { uvLive::run(uvLive::Op::Flatten); });
    connect(settleBtn, &QToolButton::clicked, this, [] { uvLive::run(uvLive::Op::Settle); });
    connect(balance, &QSlider::valueChanged, this, [](int v) { uvLive::setBalance(v / 100.0); });
    connect(brushCombo, &QComboBox::currentIndexChanged, this, [this, toolButtons](int i) {
        uvLive::brush().brush = uv::LiveBrush(std::clamp(i, 0, int(uv::LiveBrush::Soften)));
        uvLive::settingsChanged();
        toolButtons.back()->setChecked(true);
        mView->setTool(UvView::Tool::Brush);
    });
    connect(brush3d, &QToolButton::clicked, this, [] { uvLive::startBrushTool3d(); });
    connect(pinBtn, &QToolButton::clicked, this, [this] { runCommand("uvPin"); });
    connect(unpinBtn, &QToolButton::clicked, this, [this] { runCommand("uvUnpin"); });
    connect(checker, &QCheckBox::toggled, this, [](bool on) { uvLive::setChecker3d(on); });
    connect(tiling, &QSpinBox::valueChanged, this, [this](int v) {
        uvLive::setCheckerTiling(v);
        mView->update();
    });
    connect(seams, &QCheckBox::toggled, this, [](bool on) { uvLive::setSeams3d(on); });
    connect(children, &QCheckBox::toggled, this, [](bool on) {
        uv::setIncludeChildren(on);
        uvLive::settingsChanged();
        viewportTools::requestRedraw();
    });
    connect(twoColour, &QCheckBox::toggled, this, [this](bool on) {
        uvLive::setTwoColourHeatmap(on);
        mView->update();
    });
    mLiveListener = uvLive::addListener([this, live, brushCombo, checker, tiling, seams, children, showMode] {
        QSignalBlocker b1(live), b2(brushCombo), b3(checker), b4(seams), b5(tiling), b6(children);
        children->setChecked(uv::includeChildren());
        live->setChecked(uvLive::liveMode());
        brushCombo->setCurrentIndex(int(uvLive::brush().brush));
        checker->setChecked(uvLive::checker3d());
        tiling->setValue(uvLive::checkerTiling());
        showMode();
        seams->setChecked(uvLive::seams3d());
        mView->update();
    });

    mView = new UvView(this);
    root->addWidget(mView, 1);

    // Numeric transforms.
    auto* row4 = new FlowLayout();
    mMoveU = spin(-100.0, 100.0, 0.0, 4, this);
    mMoveV = spin(-100.0, 100.0, 0.0, 4, this);
    mRotate = spin(-360.0, 360.0, 90.0, 2, this);
    mScaleU = spin(-100.0, 100.0, 1.0, 4, this);
    mScaleV = spin(-100.0, 100.0, 1.0, 4, this);
    row4->addWidget(new QLabel("U", this));
    row4->addWidget(mMoveU);
    row4->addWidget(new QLabel("V", this));
    row4->addWidget(mMoveV);
    QToolButton* moveBtn = button("Move", "Move the selected uvs by U, V", this, "arrows-move");
    row4->addWidget(moveBtn);
    row4->addWidget(mRotate);
    QToolButton* rotBtn = button("Rotate", "Rotate the selected uvs, degrees counter-clockwise", this, "rotate");
    row4->addWidget(rotBtn);
    std::vector<QToolButton*> iconOnly;
    for (auto [ic, text, tip, call] : {std::tuple{"rotate-2", "Rotate 90 Left", "Rotate 90 left: each selected island a quarter turn counter-clockwise",
                                                  json{{"rotate", 90.0}, {"perIsland", true}}},
                                       {"rotate-clockwise-2", "Rotate 90 Right", "Rotate 90 right: each selected island a quarter turn clockwise",
                                        json{{"rotate", -90.0}, {"perIsland", true}}},
                                       {"flip-horizontal", "Flip Horizontal", "Flip horizontal: mirror each selected island left to right",
                                        json{{"scale", {-1.0, 1.0}}, {"perIsland", true}}},
                                       {"flip-vertical", "Flip Vertical", "Flip vertical: mirror each selected island top to bottom",
                                        json{{"scale", {1.0, -1.0}}, {"perIsland", true}}}})
    {
        QToolButton* b = button(text, tip, this, ic);
        connect(b, &QToolButton::clicked, this, [this, call] { runCommand("uvTransform", call); });
        row4->addWidget(b);
        iconOnly.push_back(b);
    }
    row4->addWidget(mScaleU);
    row4->addWidget(mScaleV);
    QToolButton* scaleBtn = button("Scale", "Scale the selected uvs about their centre", this, "resize");
    row4->addWidget(scaleBtn);
    iconOnly.insert(iconOnly.end(), {moveBtn, rotBtn, scaleBtn});
    for (QToolButton* b : iconOnly)
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    root->addLayout(row4);

    mStatus = new QLabel(this);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Command results can be one long line; without this the dock could not be narrower than the last one.
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(mStatus);

    mView->onToolChanged = [toolButtons](UvView::Tool t) { toolButtons[size_t(t)]->setChecked(true); };
    connect(frameBtn, &QToolButton::clicked, this, [this] { mView->frameSelection(); });
    connect(mDisplay, &QComboBox::currentIndexChanged, this, [this](int i) { mView->setDisplay(UvView::Display(i)); });
    connect(udim, &QCheckBox::toggled, this, [this](bool on) { mView->setShowUdim(on); });
    connect(mSet, &QComboBox::textActivated, this, [this](const QString& s) {
        if (!s.isEmpty())
            safeRun("uvSetCurrent", {{"args", {s.toStdString()}}}, CallSource::UI);
        mView->invalidate();
    });
    connect(getTd, &QToolButton::clicked, this, [this] {
        json r = runCommand("uvTexelDensity", {{"resolution", mResolution->value()}});
        if (r.is_object() && !r.empty())
            r = r.begin().value();
        if (r.is_number())
            mDensity->setValue(r.get<double>());
    });
    connect(setTd, &QToolButton::clicked, this,
            [this] { runCommand("uvTexelDensity", {{"resolution", mResolution->value()}, {"density", mDensity->value()}}); });
    connect(moveBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"translate", {mMoveU->value(), mMoveV->value()}}}); });
    connect(rotBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"rotate", mRotate->value()}}); });
    connect(scaleBtn, &QToolButton::clicked, this, [this] { runCommand("uvTransform", {{"scale", {mScaleU->value(), mScaleV->value()}}}); });

    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) {
        mView->invalidate();
        refreshSets();
    });
    mComponentListener = scene::components::addListener([this, showMode] {
        showMode();
        mView->update();
    });
    refreshSets();
}

UvEditor::~UvEditor()
{
    uvLive::setEditorShown(this, false);
    scene::Graph::current().removeListener(mGraphListener);
    scene::components::removeListener(mComponentListener);
    uvLive::removeListener(mLiveListener);
}

void UvEditor::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    uvLive::setEditorShown(this, true);
}

void UvEditor::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    uvLive::setEditorShown(this, false);
}

json UvEditor::runCommand(const char* command, const json& call)
{
    json r;
    try
    {
        r = ops::execute(command, call, CallSource::UI);
        actions::remember({{"command", command}, {"args", call}});
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    if (!r.is_null())
    {
        std::string text = r.dump();
        if (text.size() > 200)
            text = text.substr(0, 197) + "...";
        mStatus->setText(QString(command) + ": " + QString::fromStdString(text));
    }
    mView->invalidate();
    return r;
}

void UvEditor::refreshSets()
{
    std::set<std::string> names{uv::currentUvSet(), "uv0", "uv1"};
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : shownMeshes())
        if (scene::Node* n = g.node(id))
            if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
                for (const std::string& name : m->cornerAttrs().names())
                    if (const geo::Attribute* a = m->cornerAttrs().find(name); a && a->type() == geo::AttrType::Float2)
                        names.insert(name);
    QStringList items;
    for (const std::string& n : names)
        items << QString::fromStdString(n);
    QString cur = QString::fromStdString(uv::currentUvSet());
    QStringList existing;
    for (int i = 0; i < mSet->count(); ++i)
        existing << mSet->itemText(i);
    if (existing != items)
    {
        QSignalBlocker block(mSet);
        mSet->clear();
        mSet->addItems(items);
    }
    if (mSet->currentText() != cur)
    {
        QSignalBlocker block(mSet);
        mSet->setCurrentText(cur);
    }
}

void UvEditor::runPack()
{
    json call = mPackPanel->options();
    call["resolution"] = mResolution->value();
    call["padding"] = mPadding->value();
    setUvPackOptions(call);
    json r = runCommand("uvPack", call);
    mPackPanel->showResult(r);
}

void UvEditor::showLayout()
{
    json r = safeRun("uvLayout", {{"resolution", mResolution->value()}}, CallSource::Script);
    if (r.is_null())
        return;
    auto line = [](const std::string& name, const json& j) {
        if (!j.value("exists", true))
            return QString::fromStdString(name + ": no " + j.value("uvSet", std::string("uv")) + " set");
        return QString::fromStdString(name) + QString(": %1 islands, %2 overlapping, %3 folded, stretch %4 / %5, %6 px/cm")
                                                  .arg(j.value("islands", 0))
                                                  .arg(j.value("overlappingFaces", 0))
                                                  .arg(j.value("foldedFaces", 0))
                                                  .arg(j["areaDistortion"].value("mean", 0.0), 0, 'f', 3)
                                                  .arg(j["angleDistortion"].value("mean", 0.0), 0, 'f', 3)
                                                  .arg(j.value("texelDensity", 0.0), 0, 'f', 2);
    };
    QStringList lines;
    if (r.contains("islands"))
        lines << line("selection", r);
    else
        for (auto& [name, j] : r.items())
            lines << line(name, j);
    mStatus->setText(lines.join("\n"));
    log::info("{}", lines.join("; ").toStdString());
}

void setUvEditorOpener(std::function<void()> open)
{
    opener() = std::move(open);
}

void registerUvActions()
{
    registerUvPackActions();
    installUvProjectTool();
    actions::add({.id = "window.uvEditor", .label = "UV Editor", .category = "Windows", .run = [] {
                      if (opener())
                          opener()();
                  },
                  .doc = "2D uv view with selection, manipulators and the uv tools.",
                  .python = "lila.ui.run_action(\"window.uvEditor\")"});
    auto cmd = [](std::string id, std::string label, const char* command, json call, std::string doc) {
        std::string py = ops::toPython(command, call);
        actions::add({.id = std::move(id), .label = std::move(label), .category = "UV", .doc = std::move(doc), .python = py, .command = command,
                      .call = call});
    };
    cmd("uv.auto", "Auto UV", "uvAuto", json::object(), "Seams, unwrap and pack in one step.");
    cmd("uv.autoSeams", "Auto Seams", "uvAutoSeams", json::object(), "Place seams for game assets.");
    cmd("uv.unwrap", "Unwrap", "uvUnwrap", json::object(), "Unwrap along the seams with low stretch.");
    // uv.cut, uv.sew and uv.weld come from uvLive::install, which re-solves the touched islands in live mode.
    uvLive::install();
    for (auto [mode, label, doc] : {std::tuple{"vertex", "UV Vertex Select", "Select uv points in the UV editor (vertex mode)."},
                                    {"edge", "UV Edge Select", "Select uv edges in the UV editor (edge mode)."},
                                    {"face", "UV Face Select", "Select uv faces in the UV editor (face mode)."},
                                    {"island", "UV Island Select", "Face mode where clicks and marquees in the UV editor select whole uv islands."}})
    {
        std::string id = std::string("uv.") + mode + "Mode";
        actions::add({.id = id, .label = label, .category = "UV", .run = [m = std::string(mode)] { setPickMode(m); }, .doc = doc,
                      .python = "lila.ui.run_action(\"" + id + "\")"});
    }
    cmd("uv.straighten", "Straighten UVs", "uvStraighten", json::object(), "Rectify islands into grids.");
    cmd("uv.orient", "Orient UVs", "uvOrient", json::object(), "Turn islands upright.");
    cmd("uv.rotateLeft", "Rotate Islands Left", "uvTransform", json{{"rotate", 90.0}, {"perIsland", true}},
        "Rotate each selected island 90 degrees counter-clockwise about its centre.");
    cmd("uv.rotateRight", "Rotate Islands Right", "uvTransform", json{{"rotate", -90.0}, {"perIsland", true}},
        "Rotate each selected island 90 degrees clockwise about its centre.");
    cmd("uv.flipX", "Flip Islands X", "uvTransform", json{{"scale", {-1.0, 1.0}}, {"perIsland", true}},
        "Mirror each selected island left to right about its centre.");
    cmd("uv.flipY", "Flip Islands Y", "uvTransform", json{{"scale", {1.0, -1.0}}, {"perIsland", true}},
        "Mirror each selected island top to bottom about its centre.");
    cmd("uv.lightmap", "Lightmap UVs", "uvLightmap", json::object(), "Generate Unreal lightmap uvs in uv1.");
    cmd("uv.bff", "BFF Unwrap", "uvBff", json::object(), "Boundary First Flattening: conformal, boundary lengths kept, least area stretch.");
    cmd("uv.bffDisk", "BFF Unwrap to Disk", "uvBff", json{{"boundary", "disk"}}, "Flatten islands conformally onto circles.");
    cmd("uv.bffRectangle", "BFF Unwrap to Rectangle", "uvBff", json{{"boundary", "rectangle"}},
        "Flatten islands conformally onto rectangles; the selected vertices (or the sharpest four) are the corners.");
    cmd("uv.bffCones", "BFF Unwrap with Cones", "uvBff", json{{"cones", 6}}, "BFF with six automatic cone points to cut area stretch.");
    cmd("uv.optimizeConformal", "Optimize Angles", "uvOptimize", json{{"mode", "conformal"}}, "Relax islands in place to keep angles.");
    cmd("uv.optimizeAuthalic", "Optimize Areas", "uvOptimize", json{{"mode", "authalic"}}, "Relax islands in place to keep areas.");
    cmd("uv.optimizeBlend", "Optimize Blend", "uvOptimize", json{{"mode", "blend"}}, "Relax islands in place, half angles, half areas.");
    cmd("uv.optimizeIsometric", "Optimize Isometric", "uvOptimize", json{{"mode", "isometric"}}, "Relax islands in place with SLIM.");
    cmd("uv.mosaic", "Mosaic", "uvMosaic", json::object(), "Split islands that cannot flatten without stretch, then re-solve the pieces.");
    cmd("uv.autoSeams2", "Auto Seams 2", "uvAutoSeams", json{{"method", "optcuts"}},
        "Seams and stretch optimised together (OptCuts): short, hidden seams under a stretch bound.");
    cmd("uv.unwrapSymmetric", "Unwrap Symmetric", "uvUnwrapSymmetric", json::object(),
        "Unwrap one side and mirror it onto the other; islands across the plane come out symmetric.");
    cmd("uv.unwrapSymmetricStack", "Unwrap Symmetric Stacked", "uvUnwrapSymmetric", json{{"mode", "stack"}},
        "Unwrap one side; mirrored islands share its uvs.");
    cmd("uv.straightenEdges", "Straighten UV Edges", "uvStraightenEdges", json::object(),
        "Lay the selected edge chains straight in uv and re-solve around them.");
    cmd("uv.alignU", "Align Side to U", "uvAlignToEdge", json{{"axis", "u"}}, "Turn islands so the selected edges run along U.");
    cmd("uv.alignV", "Align Side to V", "uvAlignToEdge", json{{"axis", "v"}}, "Turn islands so the selected edges run along V.");
    cmd("uv.orientByEdge", "Orient by Edge", "uvAlignToEdge", json::object(), "Turn islands so the selected edges run along the nearest axis.");
    cmd("uv.projectPlanar", "Planar Projection", "uvProject", json{{"type", "planar"}}, "Project uvs along the best-fit plane.");
    cmd("uv.projectBox", "Box Projection", "uvProject", json{{"type", "box"}}, "Project uvs from six sides.");
    cmd("uv.projectCylindrical", "Cylindrical Projection", "uvProject", json{{"type", "cylindrical"}}, "Project uvs around an axis.");
    cmd("uv.projectSpherical", "Spherical Projection", "uvProject", json{{"type", "spherical"}}, "Project uvs from the centre.");
}
} // namespace lila::ui
