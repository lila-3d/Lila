#include "ui/PlacePrimitive.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"
#include "ui/AchievementRules.h"
#include "ui/Actions.h"
#include "ui/SurfaceSnap.h"
#include "ui/Tools.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QCursor>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>

namespace lila::ui
{
namespace
{
using scene::NodeId;

struct Placement
{
    glm::dvec3 position{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
};

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

/** Runs fn without recording undo: the live preview is replaced by one undoable create on drop. */
template <class Fn> void unrecorded(Fn&& fn)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    fn();
    u.setEnabled(was);
}

void setVisible(NodeId mesh, bool on)
{
    unrecorded([&] { scene::Graph::current().setValue({mesh, scene::Transform::kVisibility, -1}, on); });
}

scene::Node* generatorOf(NodeId mesh)
{
    scene::Graph& g = scene::Graph::current();
    std::optional<scene::PlugRef> src = g.source({mesh, scene::MeshNode::kInMesh, -1});
    return src ? g.node(src->node) : nullptr;
}

constexpr const char* kSizeAttrs[] = {"width", "height", "depth", "radius", "sectionRadius", "thickness", "tipRadius"};

/** The generator's size settings, in cm, by name: what the resize gesture scales. */
std::map<std::string, double> sizesOf(NodeId mesh)
{
    std::map<std::string, double> out;
    if (scene::Node* gen = generatorOf(mesh))
        for (const char* attr : kSizeAttrs)
            if (int idx = gen->type().attrIndex(attr); idx >= 0)
                out[attr] = gen->as<double>(idx);
    return out;
}

/** The brush-size keys: [ shrinks, ] grows, by the step brushes use. */
std::optional<double> bracketStep(const QKeyEvent* e)
{
    if (e->key() == Qt::Key_BracketLeft)
        return 0.8;
    if (e->key() == Qt::Key_BracketRight)
        return 1.25;
    return std::nullopt;
}

/**
 * Carries a just-created primitive under the cursor, standing on whatever surface is there (the ground plane where there
 * is none), until a click drops it. Shift keeps it upright instead of turning it to the surface; B-drag or Ctrl+scroll
 * resizes it. Dropping replaces the creation with one made at the spot and size, so create and place are a single undo
 * step; Esc or a right-click undoes the creation.
 */
class PlacePrimitiveTool : public ViewportTool
{
public:
    PlacePrimitiveTool(NodeId mesh, std::string command, QString label, uint64_t undoSerial)
        : mMesh(mesh), mCommand(std::move(command)), mLabel(std::move(label)), mSerial(undoSerial), mSizes(sizesOf(mesh))
    {
    }

    const char* name() const override { return "placePrimitive"; }

    void deactivate() override
    {
        if (!mEnded)
            drop();
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (tools::brushRadiusDrag(e, false))
        {
            mResizeFrom = e->position();
            mResizeStart = mScale;
            return true;
        }
        if (e->button() == Qt::LeftButton)
        {
            follow(e->position(), view, e->modifiers());
            drop();
        }
        else if (e->button() == Qt::RightButton)
            cancel();
        else
            return false;
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!scene::Graph::current().node(mMesh))
        {
            mEnded = true;
            end();
            return false;
        }
        if (mResizeFrom)
        {
            resize(mResizeStart * std::exp((e->position().x() - mResizeFrom->x()) * 0.01));
            return true;
        }
        follow(e->position(), view, e->modifiers());
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mResizeFrom && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton))
        {
            mResizeFrom.reset();
            return true;
        }
        return e->button() != Qt::MiddleButton;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!(e->modifiers() & Qt::ControlModifier) || mSizes.empty())
            return false;
        resize(mScale * std::pow(1.1, e->angleDelta().y() / 120.0));
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        std::optional<double> step = bracketStep(&e);
        if (!step || mSizes.empty())
            return false;
        resize(mScale * *step);
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        if (std::optional<double> step = bracketStep(e); step && !mSizes.empty())
        {
            resize(mScale * *step);
            return true;
        }
        if (e->key() == Qt::Key_Shift && mLastPx && !mResizeFrom)
        {
            follow(*mLastPx, view, Qt::ShiftModifier);
            return false;
        }
        if (e->key() != Qt::Key_Escape)
            return false;
        cancel();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolMessage(p, view,
                        QString("Click to drop the %1 on the surface. Shift keeps it upright, B-drag, Ctrl+scroll or [ ] resizes. Esc "
                                "or right-click cancels.")
                            .arg(mLabel));
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    void follow(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods)
    {
        mLastPx = px;
        const glm::dvec3 up = scene::sceneUp();
        if (std::optional<surfaceSnap::Hit> hit = surfaceSnap::underCursor(px, view, {mMesh}))
            mLast = Placement{hit->position, hit->normal};
        else
        {
            glm::dvec3 o, d;
            view.ray(px, o, d);
            const double den = glm::dot(d, up);
            const double t = std::abs(den) > 1e-9 ? -glm::dot(o, up) / den : -1.0;
            if (t <= 0.0)
                return;
            mLast = Placement{o + d * t, up};
        }
        if (mods & Qt::ShiftModifier)
            mLast->normal = up;
        place();
        setVisible(mMesh, true);
    }

    void place()
    {
        if (mLast)
            unrecorded([&] { scene::placePrimitive(scene::Graph::current(), mMesh, mLast->position, mLast->normal); });
        viewportTools::requestRedraw();
    }

    void resize(double scale)
    {
        mScale = std::clamp(scale, 0.01, 100.0);
        scene::Node* gen = generatorOf(mMesh);
        if (!gen)
            return;
        unrecorded([&] {
            for (const auto& [attr, size] : mSizes)
                scene::Graph::current().setValue({gen->id(), gen->type().attrIndex(attr), -1}, size * mScale);
        });
        place();
    }

    json sizeFlags() const
    {
        json out = json::object();
        if (mScale != 1.0)
            for (const auto& [attr, size] : mSizes)
                out[attr] = size * mScale;
        return out;
    }

    void drop()
    {
        mEnded = true;
        scene::Graph& g = scene::Graph::current();
        if (!g.node(mMesh))
            return end();
        if (!mLast)
        {
            setVisible(mMesh, true);
            return end();
        }
        achievementRules::primitivePlaced();
        UndoStack& u = UndoStack::get();
        if (u.serial() == mSerial && u.undo())
        {
            u.flushFuture();
            json call = sizeFlags();
            call["position"] = vec(mLast->position);
            call["normal"] = vec(mLast->normal);
            actions::runWithOptions(mCommand, call);
        }
        else
        {
            setVisible(mMesh, true);
            scene::placePrimitive(g, mMesh, mLast->position, mLast->normal);
        }
        end();
    }

    void cancel()
    {
        mEnded = true;
        scene::Graph& g = scene::Graph::current();
        UndoStack& u = UndoStack::get();
        if (u.serial() == mSerial && u.undo())
            u.flushFuture();
        else if (scene::Node* n = g.node(mMesh))
            try
            {
                ops::execute("delete", {{"args", json::array({n->path()})}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        end();
    }

    static void end()
    {
        // Not from inside this tool's own handler: set() destroys it.
        QTimer::singleShot(0, [] {
            if (viewportTools::active() && std::string_view(viewportTools::active()->name()) == "placePrimitive")
                viewportTools::set(nullptr);
        });
        viewportTools::requestRedraw();
    }

    NodeId mMesh;
    std::string mCommand;
    QString mLabel;
    uint64_t mSerial;
    std::optional<Placement> mLast;
    std::optional<QPointF> mLastPx;
    /** The generator's sizes when it was created; the resize gesture scales them all by mScale. */
    std::map<std::string, double> mSizes;
    double mScale = 1.0;
    double mResizeStart = 1.0;
    std::optional<QPointF> mResizeFrom;
    bool mEnded = false;
};

/**
 * After a primitive is made on the selected components, the brush-size gestures (B-drag, Ctrl+scroll, [ ]) resize it in
 * place by adjusting the create, as its settings popup would. Any other click, or another edit, ends it.
 */
class ResizeCreatedTool : public ViewportTool
{
public:
    ResizeCreatedTool(std::string command, QString label) : mCommand(std::move(command)), mLabel(std::move(label))
    {
        if (const scene::lastOp::Op* op = scene::lastOp::current())
            for (const FlagDef* f : scene::lastOp::editableFlags(mCommand))
                if (std::ranges::find_if(kSizeAttrs, [&](const char* a) { return f->name == a; }) != std::end(kSizeAttrs))
                    if (json v = scene::lastOp::flagValue(*op, *f); v.is_number())
                        mSizes[f->name] = v.get<double>();
    }

    const char* name() const override { return "resizeCreated"; }
    bool ownsBrushRadius() const override { return true; }

    void deactivate() override
    {
        if (mResizeFrom)
            scene::lastOp::endDrag();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo&) override
    {
        if (adjustable() && tools::brushRadiusDrag(e, false))
        {
            mResizeFrom = e->position();
            mResizeStart = mScale;
            return true;
        }
        end();
        return false;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo&) override
    {
        if (!mResizeFrom)
            return false;
        resize(mResizeStart * std::exp((e->position().x() - mResizeFrom->x()) * 0.01), true);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (!mResizeFrom || (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton))
            return false;
        mResizeFrom.reset();
        scene::lastOp::endDrag();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!(e->modifiers() & Qt::ControlModifier) || !adjustable())
            return false;
        resize(mScale * std::pow(1.1, e->angleDelta().y() / 120.0), false);
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return stepSize(e); }
    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Escape)
            end();
        return stepSize(*e);
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        if (!adjustable())
            return end();
        drawToolMessage(p, view, QString("B-drag, Ctrl+scroll or [ ] resizes the %1. Click anywhere to carry on.").arg(mLabel));
    }

private:
    /** Still the newest create of its command, so resizing re-runs it. */
    bool adjustable() const
    {
        const scene::lastOp::Op* op = scene::lastOp::current();
        return !mSizes.empty() && op && op->command == mCommand && scene::lastOp::reachable();
    }

    bool stepSize(const QKeyEvent& e)
    {
        std::optional<double> step = bracketStep(&e);
        if (!step || !adjustable())
            return false;
        resize(mScale * *step, false);
        return true;
    }

    void resize(double scale, bool drag)
    {
        mScale = std::clamp(scale, 0.01, 100.0);
        json flags = json::object();
        for (const auto& [attr, size] : mSizes)
            flags[attr] = size * mScale;
        try
        {
            scene::lastOp::rerun(flags, CallSource::UI, drag);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            end();
        }
        viewportTools::requestRedraw();
    }

    static void end()
    {
        QTimer::singleShot(0, [] {
            if (viewportTools::active() && std::string_view(viewportTools::active()->name()) == "resizeCreated")
                viewportTools::set(nullptr);
        });
        viewportTools::requestRedraw();
    }

    std::string mCommand;
    QString mLabel;
    std::map<std::string, double> mSizes;
    double mScale = 1.0;
    double mResizeStart = 1.0;
    std::optional<QPointF> mResizeFrom;
};

bool hasComponentSelection()
{
    using namespace scene;
    if (components::mode() == ComponentMode::Object)
        return false;
    for (const auto& [node, ids] : components::selected(components::mode()))
        if (!ids.empty())
            return true;
    return false;
}

/** Focuses the view under the cursor: B and the other viewport keys only reach a focused viewport, and the selection may have come from the outliner. */
void focusViewportUnderCursor()
{
    for (QWidget* w = QApplication::widgetAt(QCursor::pos()); w; w = w->parentWidget())
        if (w->property("hotkeyContext").toString() == "viewport")
            return w->setFocus();
}

void createAt(const std::string& command, const QString& label)
{
    focusViewportUnderCursor();
    if (hasComponentSelection())
    {
        actions::runWithOptions(command, {{"position", vec(scene::symmetricSelectionCenter())}, {"normal", vec(scene::selectionNormal())}});
        achievementRules::primitivePlaced();
        if (const scene::lastOp::Op* op = scene::lastOp::current(); op && op->command == command && scene::lastOp::live())
            viewportTools::set(std::make_unique<ResizeCreatedTool>(command, label));
        return;
    }
    json result;
    try
    {
        // Not as UI: the create made on drop is the one echoed and offered for adjusting.
        result = ops::execute(command, ops::options::callWith(command), CallSource::Script);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return;
    }
    scene::Node* mesh = result.is_array() && !result.empty() && result[0].is_string() ? scene::Graph::current().find(result[0].get<std::string>()) : nullptr;
    if (!mesh)
        return;
    // Hidden until the cursor first gives it a spot, so it never flashes at the origin.
    setVisible(mesh->id(), false);
    viewportTools::set(std::make_unique<PlacePrimitiveTool>(mesh->id(), command, label, UndoStack::get().serial()));
}
} // namespace

void registerPlacePrimitiveUi()
{
    for (auto [id, label, command] : {
             std::tuple{"createAt.cube", "Cube", "polyCube"},
             {"createAt.sphere", "Sphere", "polySphere"},
             {"createAt.quadSphere", "Quad Sphere", "polyQuadSphere"},
             {"createAt.icoSphere", "Ico Sphere", "polyIcoSphere"},
             {"createAt.cylinder", "Cylinder", "polyCylinder"},
             {"createAt.capsule", "Capsule", "polyCapsule"},
             {"createAt.cone", "Cone", "polyCone"},
             {"createAt.plane", "Plane", "polyPlane"},
             {"createAt.torus", "Torus", "polyTorus"},
             {"createAt.disc", "Disc", "polyDisc"},
             {"createAt.pipe", "Pipe", "polyPipe"},
         })
    {
        const std::string cmd = command;
        const QString text = QString(label).toLower();
        actions::add({.id = id,
                      .label = label,
                      .category = "Create",
                      .run = [cmd, text] { createAt(cmd, text); },
                      .doc = std::string("Creates a ") + QString(label).toLower().toStdString() +
                             " standing on the selected components, centred on them and facing along their normal. With no components "
                             "selected it follows the cursor over the surfaces until you click to drop it: Shift keeps it upright. Either "
                             "way B-drag, Ctrl+scroll or [ ] resizes it. Uses the option box settings.",
                      .python = std::string(command) + "(position=[0, 0, 0], normal=[0, 1, 0])"});
    }
}
} // namespace lila::ui
