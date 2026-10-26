// The Kit Place tool: drag the selected piece over the scene's surfaces, its bottom on whatever is under the cursor; near
// a compatible snap point of another piece it jumps to join it. Release keeps the move as one undo step.

#include "Kit.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Mesh.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/ViewportTool.h>

#include <QKeyEvent>
#include <QMouseEvent>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <set>

using namespace lila;
using namespace lila::ui;

namespace stagehand
{
namespace
{
constexpr const char* kTool = "stagehand.kitPlace";
/** How near on screen a snap point has to come to a compatible one to join it, pixels. */
constexpr double kSnapPixels = 24.0;

template <class Fn> void unrecorded(Fn&& fn)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    fn();
    u.setEnabled(was);
}

class KitPlaceTool final : public ViewportTool
{
public:
    const char* name() const override { return "stagehandKitPlace"; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        Graph& g = Graph::current();
        if (g.selection().empty())
        {
            log::write(LogLevel::Warning, "Kit Place: select the piece to move");
            return true;
        }
        g.evaluate();
        mPiece = kit::pieceOf(g, g.selection().back());
        mStart = g.node(mPiece)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        glm::dvec3 lo, hi;
        if (!boundsUnder(g, mPiece, lo, hi))
            return true;
        mBottom = glm::dvec3((lo.x + hi.x) * 0.5, lo.y, (lo.z + hi.z) * 0.5);
        mTurn = 0.0;
        buildSurfaces(g);
        mDragging = true;
        drag(e->position(), view, e->modifiers());
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mView = view;
        if (!mDragging)
            return false;
        drag(e->position(), view, e->modifiers());
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mDragging)
            return false;
        mDragging = false;
        Graph& g = Graph::current();
        const glm::dmat4 final = mCurrent;
        unrecorded([&] { setWorld(g, mPiece, mStart); });
        if (final != mStart)
        {
            UndoChunk chunk("Kit Place");
            setWorld(g, mPiece, final);
        }
        mSurfaces.reset();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        switch (e->key())
        {
        case Qt::Key_Comma:
        case Qt::Key_Period:
            mTurn += e->key() == Qt::Key_Comma ? -15.0 : 15.0;
            if (mDragging)
                drag(mCursor, view, e->modifiers());
            return true;
        case Qt::Key_Escape:
            if (mDragging)
            {
                unrecorded([&] { setWorld(Graph::current(), mPiece, mStart); });
                mDragging = false;
                mSurfaces.reset();
            }
            else
                viewportTools::set(nullptr);
            return true;
        default:
            return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolMessage(p, view, "Kit Place: drag the selected piece over surfaces; it snaps to compatible snap points (Ctrl: no snap). "
                                 "Shift stands it along the surface; , and . turn it 15 degrees. Esc cancels.");
        if (!mSnapAt)
            return;
        if (auto at = view.project(*mSnapAt))
            p.disc(*at, 6.0f, {0.95f, 0.95f, 0.95f, 0.9f});
    }

private:
    /** Every visible mesh but the piece's, as static triangles to slide over. */
    void buildSurfaces(Graph& g)
    {
        std::set<NodeId> own;
        for (NodeId m : meshesOf(g, mPiece))
            own.insert(m);
        mSurfaces = std::make_unique<physics::RigidScene>();
        for (NodeId id : g.nodes())
        {
            if (own.count(id) || !physical(g, id))
                continue;
            physics::BodyDesc b;
            b.role = physics::Role::Static;
            b.shape = shapeOf(g, id, ShapeChoice::Mesh, 0.0, true);
            const Frame f = frameOf(g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix));
            b.position = f.position;
            b.rotation = f.rotation;
            mSurfaces->add(b);
        }
    }

    void drag(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods)
    {
        mCursor = px;
        Graph& g = Graph::current();
        glm::dvec3 o, d;
        view.ray(px, o, d);
        glm::dvec3 at, normal(0, 1, 0);
        if (auto hit = mSurfaces ? mSurfaces->raycast(o, d, 1e7) : std::nullopt)
        {
            at = hit->point;
            normal = hit->normal;
        }
        else if (std::abs(d.y) > 1e-9 && -o.y / d.y > 0.0)
            at = o + d * (-o.y / d.y);
        else
            return;

        // Keep the piece's own turn, add the tool's, and stand it along the surface with Shift.
        glm::dmat4 turn = glm::rotate(glm::dmat4(1.0), glm::radians(mTurn), glm::dvec3(0, 1, 0));
        if (mods & Qt::ShiftModifier)
        {
            const glm::dvec3 up(0, 1, 0), n = glm::normalize(normal);
            const double c = glm::dot(up, n);
            if (c < 1.0 - 1e-9 && c > -1.0 + 1e-9)
                turn = glm::mat4_cast(glm::normalize(glm::dquat(1.0 + c, glm::cross(up, n)))) * turn;
        }
        const glm::dmat4 local = mStart;
        const glm::dvec3 bottomWorld(local * glm::dvec4(mBottom, 1.0));
        glm::dmat4 world = glm::translate(glm::dmat4(1.0), at) * turn * glm::translate(glm::dmat4(1.0), -bottomWorld) * local;

        mSnapAt.reset();
        unrecorded([&] { setWorld(g, mPiece, world); });
        if (!(mods & Qt::ControlModifier))
        {
            g.evaluate();
            const std::vector<kit::SnapPoint> mine = kit::snapPointsOf(g, mPiece);
            const std::vector<kit::SnapPoint> all = kit::allSnapPoints(g);
            double best = kSnapPixels;
            const kit::SnapPoint *bm = nullptr, *bt = nullptr;
            for (const kit::SnapPoint& m : mine)
            {
                const auto pm = view.project(glm::dvec3(m.world[3]));
                if (!pm)
                    continue;
                for (const kit::SnapPoint& t : all)
                {
                    if (t.piece == mPiece || !kit::compatible(m, t))
                        continue;
                    const auto pt = view.project(glm::dvec3(t.world[3]));
                    if (!pt)
                        continue;
                    const double dpx = std::hypot(pm->x() - pt->x(), pm->y() - pt->y());
                    if (dpx < best)
                        best = dpx, bm = &m, bt = &t;
                }
            }
            if (bm)
            {
                world = kit::joinedPieceMatrix(g, *bm, *bt);
                mSnapAt = glm::dvec3(bt->world[3]);
                unrecorded([&] { setWorld(g, mPiece, world); });
            }
        }
        mCurrent = world;
        viewportTools::requestRedraw();
    }

    NodeId mPiece = scene::kNoNode;
    glm::dmat4 mStart{1.0}, mCurrent{1.0};
    glm::dvec3 mBottom{0.0};
    double mTurn = 0.0;
    bool mDragging = false;
    QPointF mCursor;
    ViewInfo mView;
    std::optional<glm::dvec3> mSnapAt;
    std::unique_ptr<physics::RigidScene> mSurfaces;
};
} // namespace

void registerKitUi(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = kTool, .label = "Kit Place", .icon = "puzzle", .create = [] { return std::make_unique<KitPlaceTool>(); }});
    actions::add(ctx, {.id = "stagehand.tool.kitPlace",
                       .label = "Kit Place Tool",
                       .category = "Dress",
                       .run = [] { viewportTools::activate(kTool); },
                       .doc = "Drag a piece over surfaces; it snaps to compatible snap points of other pieces."});
    const struct
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
    } helpers[] = {
        {"stagehand.dropToSurface", "Drop to Surface", "dropToSurface", "Move the selection straight down onto what is below it, no physics."},
        {"stagehand.alignObjects", "Align Objects", "alignObjects", "Line the selection up on an axis by its bounds."},
        {"stagehand.distribute", "Distribute", "distribute", "Space the selection evenly along an axis."},
        {"stagehand.randomize", "Randomize Transforms", "randomizeTransforms", "Nudge, turn and scale the selection by seeded random amounts."},
        {"stagehand.snapAll", "Snap All Pieces", "snapAll", "Join kit pieces whose snap points are near each other."},
        {"stagehand.addSnapPoint", "Add Snap Point", "addSnapPoint", "Add a modular snap point to the selected piece."},
    };
    menus::addItem(ctx, {.menu = "Dress", .kind = menus::Kind::Section, .label = "Level Kit"});
    menus::addItem(ctx, {.menu = "Dress", .action = "stagehand.tool.kitPlace"});
    for (const auto& h : helpers)
    {
        actions::add(ctx, {.id = h.id, .label = h.label, .category = "Dress", .doc = h.doc, .command = h.command, .call = nlohmann::json::object()});
        menus::addItem(ctx, {.menu = "Dress", .action = h.id});
    }
}
} // namespace stagehand
