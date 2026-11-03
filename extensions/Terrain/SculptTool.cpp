// The Sculpt Terrain viewport tool: brush strokes on a terrain, kept as terrainSculpt strokes. A stroke in the final stage
// (after erosion) re-runs only what follows it, since the modifier keeps the steps an edit leaves alone. A base-stage
// stroke previews in the final stage while it is drawn and moves to the base stage, eroded with the rest, when released.

#include "Analysis.h"
#include "Terrain.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Node.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/ViewportTool.h>

#include <QElapsedTimer>

#include <cmath>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
namespace
{
using json = nlohmann::json;

struct Target
{
    std::string mesh;
    glm::dmat4 world{1.0};
    std::shared_ptr<const Heightfield> h;
};

/** The first selected mesh whose modifiers make a heightfield. */
std::optional<Target> selectedTerrain()
{
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (gn::isGnModifier(*n))
            if (scene::NodeId m = scene::modifiers::meshOf(g, id); m != scene::kNoNode)
                n = g.node(m);
        if (!n || !n->type().isA("mesh"))
            continue;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            if (scene::Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto h = gn::modifierHeightfield(m->id()); h && !h->empty())
                    return Target{n->path(), n->as<glm::dmat4>(scene::Transform::kWorldMatrix), std::move(h)};
    }
    return std::nullopt;
}

class SculptTool final : public ViewportTool
{
public:
    const char* name() const override { return "Sculpt Terrain"; }

    void activate() override { refresh(); }

    bool ownsBrushRadius() const override { return true; }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        refresh();
        auto at = hit(view, e->position());
        if (!at)
            return false;
        mOp = currentOp(e->modifiers());
        mStroke = {*at};
        mDown = true;
        mPreviewed = false;
        UndoStack::get().openChunk("Sculpt Terrain");
        send(false);
        mTimer.start();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = hit(view, e->position());
        if (!mDown)
        {
            viewportTools::requestRedraw();
            return false;
        }
        if (mHover && glm::length(*mHover - mStroke.back()) > mRadius * 0.2)
            mStroke.push_back(*mHover);
        // Re-evaluating on every mouse event would queue up behind itself; a few times a second keeps up.
        if (mTimer.elapsed() > 60)
        {
            send(false);
            mTimer.restart();
        }
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        (void)view;
        if (e->button() != Qt::LeftButton || !mDown)
            return false;
        mDown = false;
        try
        {
            send(true);
        }
        catch (...)
        {
            UndoStack::get().closeChunk();
            throw;
        }
        UndoStack::get().closeChunk();
        refresh();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        (void)action;
        return key(e);
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        (void)view;
        if (e->key() == Qt::Key_Escape)
        {
            viewportTools::set(nullptr);
            return true;
        }
        return key(*e);
    }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        if (!mTarget)
        {
            drawToolMessage(p, view, "Select a terrain");
            return;
        }
        static const char* kOps[] = {"Raise", "Lower", "Smooth", "Flatten", "Noise"};
        drawToolMessage(p, view, QString("%1  %2 cm  %3").arg(kOps[mBaseOp]).arg(mRadius, 0, 'f', 0).arg(mFinal ? "after erosion" : "before erosion"));
        drawKeyList(p, QPointF(12.0, view.height - 12.0),
                    {{"1-5", "Raise, lower, smooth, flatten, noise"}, {"Ctrl", "Invert"}, {"Shift", "Smooth"}, {"[ ]", "Radius"},
                     {"- =", "Strength"}, {"F", "Before or after erosion"}, {"B drag", "Radius"}});
        if (!mHover)
            return;
        // The brush ring, lying on the ground.
        const glm::dmat4 inv = glm::inverse(mTarget->world);
        const glm::dvec3 c(inv * glm::dvec4(*mHover, 1.0));
        const double across = glm::length(glm::dvec3(mTarget->world[0]));
        const double r = mRadius / std::max(across, 1e-9);
        const glm::vec4 colour(1.0f, 0.75f, 0.2f, 1.0f);
        glm::vec3 prev(0.0f);
        for (int k = 0; k <= 48; ++k)
        {
            const double a = double(k) / 48.0 * 6.283185307179586;
            const double x = c.x + std::cos(a) * r, z = c.z + std::sin(a) * r;
            const glm::vec3 w(mTarget->world * glm::dvec4(x, mTarget->h->heightAt(x, z), z, 1.0));
            if (k > 0)
            {
                s.linesOnTop.push_back({prev, colour});
                s.linesOnTop.push_back({w, colour});
            }
            prev = w;
        }
    }

private:
    void refresh() { mTarget = selectedTerrain(); }

    std::optional<glm::dvec3> hit(const ViewInfo& view, QPointF px) const
    {
        if (!mTarget)
            return std::nullopt;
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const glm::dmat4 inv = glm::inverse(mTarget->world);
        glm::dvec3 at;
        if (!raycast(*mTarget->h, glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), &at))
            return std::nullopt;
        return glm::dvec3(mTarget->world * glm::dvec4(at, 1.0));
    }

    int currentOp(Qt::KeyboardModifiers m) const
    {
        if (m & Qt::ShiftModifier)
            return 2;
        if (m & Qt::ControlModifier)
            return mBaseOp == 0 ? 1 : mBaseOp == 1 ? 0 : mBaseOp;
        return mBaseOp;
    }

    bool key(const QKeyEvent& e)
    {
        const int k = e.key();
        if (k >= Qt::Key_1 && k <= Qt::Key_5)
            mBaseOp = k - Qt::Key_1;
        else if (k == Qt::Key_BracketLeft)
            mRadius = std::max(10.0, mRadius / 1.25);
        else if (k == Qt::Key_BracketRight)
            mRadius *= 1.25;
        else if (k == Qt::Key_Minus)
            mStrength /= 1.25;
        else if (k == Qt::Key_Equal)
            mStrength *= 1.25;
        else if (k == Qt::Key_F)
            mFinal = !mFinal;
        else
            return false;
        viewportTools::requestRedraw();
        return true;
    }

    /** The stroke so far as terrainSculpt: replacing its own earlier preview, and on release moving it where it belongs. */
    void send(bool done)
    {
        if (!mTarget || mStroke.empty())
            return;
        static const char* kOps[] = {"raise", "lower", "smooth", "flatten", "noise"};
        json pts = json::array();
        for (const glm::dvec3& p : mStroke)
            pts.push_back({p.x, p.y, p.z});
        const bool smoothing = mOp == 2 || mOp == 3;
        json call = {{"args", {mTarget->mesh}},
                     {"op", kOps[mOp]},
                     {mStroke.size() > 1 ? "path" : "points", pts},
                     {"radius", mRadius},
                     {"strength", smoothing ? std::clamp(mStrength / 1000.0, 0.05, 1.0) : mStrength},
                     {"stage", "final"}};
        // Final strokes and the preview of base ones live in the final stage; the preview replaces itself as it grows.
        if (mPreviewed)
            call["replaceLast"] = true;
        if (done && !mFinal)
        {
            if (mPreviewed)
                ops::execute("terrainSculpt", {{"args", {mTarget->mesh}}, {"stage", "final"}, {"removeLast", true}}, CallSource::UI);
            call["stage"] = "base";
            call.erase("replaceLast");
            ops::execute("terrainSculpt", call, CallSource::UI);
            return;
        }
        ops::execute("terrainSculpt", call, CallSource::UI);
        mPreviewed = true;
    }

    std::optional<Target> mTarget;
    std::optional<glm::dvec3> mHover;
    std::vector<glm::dvec3> mStroke;
    QElapsedTimer mTimer;
    bool mDown = false;
    bool mPreviewed = false;
    bool mFinal = true;
    int mBaseOp = 0;
    int mOp = 0;
    double mRadius = 3000.0;
    double mStrength = 300.0;
};
} // namespace

void registerSculptTool(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = "terrain.sculptTool", .label = "Sculpt Terrain", .icon = "brush", .create = [] { return std::make_unique<SculptTool>(); }});
}
} // namespace terrain
