#include "ui/PlacePrimitive.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/SurfaceSnap.h"
#include "ui/ViewportTool.h"

#include <QTimer>

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

void setVisible(NodeId mesh, bool on)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    scene::Graph::current().setValue({mesh, scene::Transform::kVisibility, -1}, on);
    u.setEnabled(was);
}

/**
 * Carries a just-created primitive under the cursor, standing on whatever surface is there (the ground plane where there
 * is none), until a click drops it. Dropping replaces the creation with one made at the spot, so create and place are a
 * single undo step; Esc or a right-click undoes the creation.
 */
class PlacePrimitiveTool : public ViewportTool
{
public:
    PlacePrimitiveTool(NodeId mesh, std::string command, QString label, uint64_t undoSerial)
        : mMesh(mesh), mCommand(std::move(command)), mLabel(std::move(label)), mSerial(undoSerial)
    {
    }

    const char* name() const override { return "placePrimitive"; }

    void deactivate() override
    {
        if (!mEnded)
            drop();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() == Qt::LeftButton)
        {
            follow(e->position(), view);
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
        follow(e->position(), view);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override { return e->button() != Qt::MiddleButton; }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_Escape)
            return false;
        cancel();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolMessage(p, view, QString("Click to drop the %1 on the surface. Esc or right-click cancels.").arg(mLabel));
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    void follow(QPointF px, const ViewInfo& view)
    {
        if (std::optional<surfaceSnap::Hit> hit = surfaceSnap::underCursor(px, view, {mMesh}))
            mLast = Placement{hit->position, hit->normal};
        else
        {
            glm::dvec3 o, d;
            view.ray(px, o, d);
            const double t = std::abs(d.y) > 1e-9 ? -o.y / d.y : -1.0;
            if (t <= 0.0)
                return;
            mLast = Placement{o + d * t, {0.0, 1.0, 0.0}};
        }
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        scene::placePrimitive(scene::Graph::current(), mMesh, mLast->position, mLast->normal);
        u.setEnabled(was);
        setVisible(mMesh, true);
        viewportTools::requestRedraw();
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
        UndoStack& u = UndoStack::get();
        if (u.serial() == mSerial && u.undo())
        {
            u.flushFuture();
            actions::runWithOptions(mCommand, {{"position", vec(mLast->position)}, {"normal", vec(mLast->normal)}});
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
    bool mEnded = false;
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

void createAt(const std::string& command, const QString& label)
{
    if (hasComponentSelection())
    {
        actions::runWithOptions(command, {{"position", vec(scene::selectionCenter())}, {"normal", vec(scene::selectionNormal())}});
        return;
    }
    json result;
    try
    {
        result = ops::execute(command, ops::options::callWith(command), CallSource::UI);
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
                             "selected it follows the cursor over the surfaces until you click to drop it. Uses the option box settings.",
                      .python = std::string(command) + "(position=[0, 0, 0], normal=[0, 1, 0])"});
    }
}
} // namespace lila::ui
