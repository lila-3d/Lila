// Tailor's viewport tools. Grab Cloth pulls a live garment: the nearest cloth point under the cursor follows it until the
// button comes up. Draw Panel outlines a panel on the body with clicks.

#include "Tailor.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Transform.h>
#include <lila/ViewportTool.h>

#include <QKeyEvent>
#include <QMouseEvent>

#include <cmath>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace tailor
{
namespace
{
constexpr double kPickPx = 25.0;

class GrabTool final : public ViewportTool
{
public:
    const char* name() const override { return "Grab Cloth"; }

    void activate() override
    {
        // With nothing live yet, the selected garment goes live: grabbing a still garment would do nothing.
        if (!liveGarments().empty())
            return;
        Graph& g = Graph::current();
        for (NodeId id : g.selection())
            if (Node* n = g.node(id))
            {
                Node* garment = n->type().isA(kGarmentType) ? n : nullptr;
                if (!garment)
                    if (const int in = n->type().attrIndex("inMesh"); in >= 0)
                        if (const auto src = g.source({n->id(), in}))
                            if (Node* s = g.node(src->node); s && s->type().isA(kGarmentType))
                                garment = s;
                if (garment)
                {
                    g.setValue({garment->id(), garment->type().attrIndex("live")}, true);
                    return;
                }
            }
    }

    void deactivate() override { releaseGrab(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        Graph& g = Graph::current();
        double best = kPickPx;
        for (NodeId id : liveGarments())
        {
            const auto live = liveDrape(id);
            if (!live)
                continue;
            const glm::dmat4 world = garmentWorld(g, id);
            const std::vector<Vec3> x = live->positions();
            for (size_t i = 0; i < x.size(); ++i)
            {
                const glm::dvec3 w = glm::dvec3(world * glm::dvec4(glm::dvec3(x[i][0], x[i][1], x[i][2]) * kCmPerM, 1.0));
                const std::optional<QPointF> s = view.project(w);
                if (!s)
                    continue;
                if (const double d = std::hypot(s->x() - e->position().x(), s->y() - e->position().y()); d < best)
                {
                    best = d;
                    mGarment = id;
                    mVertex = int(i);
                    mWorld = world;
                    mAnchor = w;
                }
            }
        }
        if (mVertex < 0)
            return false;
        drag(e->position(), view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mVertex < 0)
            return false;
        drag(e->position(), view);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || mVertex < 0)
            return false;
        releaseGrab();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolStatus(p, view,
                       {.tool = name(),
                        .prompt = liveGarments().empty() ? "select a garment, then pick Grab Cloth again" : "drag the cloth"});
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::OpenHandCursor); }

private:
    /** Moves the grabbed point to where the cursor's ray meets the plane through it facing the camera. */
    void drag(QPointF px, const ViewInfo& view)
    {
        const auto live = liveDrape(mGarment);
        if (!live)
        {
            mVertex = -1;
            return;
        }
        glm::dvec3 o, d;
        view.ray(px, o, d);
        const double t = glm::dot(mAnchor - o, d) / glm::dot(d, d);
        const glm::dvec3 hit = o + d * t;
        const glm::dvec3 local = glm::dvec3(glm::inverse(mWorld) * glm::dvec4(hit, 1.0)) / kCmPerM;
        live->grab(mVertex, {float(local.x), float(local.y), float(local.z)});
    }

    void releaseGrab()
    {
        if (const auto live = liveDrape(mGarment))
            live->release();
        mVertex = -1;
        mGarment = kNoNode;
    }

    NodeId mGarment = kNoNode;
    int mVertex = -1;
    glm::dmat4 mWorld{1.0};
    glm::dvec3 mAnchor{0.0};
};
/** The body to draw on: the selected garment's collider, else the selected mesh. */
Node* drawTarget(Graph& g, NodeId& garment)
{
    garment = kNoNode;
    for (NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        Node* gn = n->type().isA(kGarmentType) ? n : nullptr;
        if (!gn)
            if (const int in = n->type().attrIndex("inMesh"); in >= 0)
                if (const auto src = g.source({n->id(), in}))
                    if (Node* s = g.node(src->node); s && s->type().isA(kGarmentType))
                        gn = s;
        if (gn)
        {
            garment = gn->id();
            const auto src = g.source({gn->id(), gn->type().attrIndex("colliderMesh")});
            return src ? g.node(src->node) : nullptr;
        }
        if (n->type().isA("mesh"))
            return n;
    }
    return nullptr;
}

class DrawPanelTool final : public ViewportTool
{
public:
    const char* name() const override { return "Draw Panel"; }

    void activate() override
    {
        Graph& g = Graph::current();
        mBody = drawTarget(g, mGarment);
        mPoints.clear();
        mSdf = Sdf{};
        if (!mBody)
            return;
        g.evaluate();
        const PlugValue v = g.value({mBody->id(), MeshNode::kOutMesh});
        const MeshPtr* m = std::get_if<MeshPtr>(&v);
        const PlugValue w = g.value({mBody->id(), Transform::kWorldMatrix});
        if (!m || !*m || !std::get_if<glm::dmat4>(&w))
            return;
        std::vector<Vec3> pts;
        std::vector<std::array<int, 3>> tris;
        colliderTriangles(**m, std::get<glm::dmat4>(w), pts, tris);
        mSdf = sdfFromMesh(pts, tris, 0.01f, 0.1f);
        mBodyPath = mBody->path();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || mSdf.nx == 0)
            return false;
        if (const auto hit = trace(e->position(), view))
            mPoints.push_back(*hit);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo&) override
    {
        mCursor = e->position();
        if (!mPoints.empty())
            viewportTools::requestRedraw();
        return false;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            finish();
            return true;
        case Qt::Key_Backspace:
            if (!mPoints.empty())
                mPoints.pop_back();
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_M:
            mMirror = !mMirror;
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            mPoints.clear();
            viewportTools::requestRedraw();
            return true;
        default:
            return false;
        }
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return e.key() == Qt::Key_M || e.key() == Qt::Key_Backspace; }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        if (mSdf.nx == 0)
        {
            drawToolStatus(p, view, {.tool = name(), .prompt = "select a body mesh or a garment, then pick Draw Panel"});
            return;
        }
        std::vector<QPointF> screen;
        for (const glm::dvec3& q : mPoints)
            if (const auto s = view.project(q))
                screen.push_back(*s);
        const glm::vec4 colour(0.95f, 0.75f, 0.3f, 1.f);
        if (screen.size() >= 2)
            p.polyline(screen, 2.f, colour);
        if (!screen.empty())
        {
            p.dashedLine(screen.back(), mCursor, 1.f, 5.f, colour);
            if (screen.size() >= 3)
                p.dashedLine(screen.back(), screen.front(), 1.f, 3.f, {colour.r, colour.g, colour.b, 0.5f});
        }
        for (const QPointF& s : screen)
            p.disc(s, 3.5f, colour);
        drawToolStatus(p, view,
                       {.tool = name(),
                        .prompt = "click around the panel on the body",
                        .keys = {{"Enter", "make"}, {"Backspace", "undo point"}, {"M", "mirror"}},
                        .symmetry = ToolSymmetry{mMirror}});
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    /** Sphere-traces the cursor's ray to the body's surface, world cm. */
    std::optional<glm::dvec3> trace(QPointF px, const ViewInfo& view) const
    {
        glm::dvec3 o, d;
        view.ray(px, o, d);
        d = glm::normalize(d);
        glm::dvec3 q = o / kCmPerM;
        for (int step = 0; step < 512; ++step)
        {
            const float phi = mSdf.sample({float(q.x), float(q.y), float(q.z)});
            if (phi < 0.002f)
                return q * kCmPerM;
            q += d * double(std::max(phi, 0.002f));
            if (glm::length(q - o / kCmPerM) > 100.0)
                break;
        }
        return std::nullopt;
    }

    void finish()
    {
        if (mPoints.size() < 3)
            return;
        nlohmann::json pts = nlohmann::json::array();
        for (const glm::dvec3& q : mPoints)
            pts.push_back({q.x, q.y, q.z});
        nlohmann::json call = {{"points", pts}, {"mirror", mMirror}};
        Graph& g = Graph::current();
        if (const Node* gn = g.node(mGarment))
            call["args"] = {gn->path()};
        else
            call["collider"] = mBodyPath;
        try
        {
            const nlohmann::json r = ops::execute("tailorDrawPanel", call, CallSource::UI);
            // Later strokes add to the garment this one made or extended.
            if (r.is_array() && !r.empty() && r[0].is_string())
                if (const Node* made = g.find(r[0].get<std::string>()))
                    mGarment = made->id();
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
        mPoints.clear();
        viewportTools::requestRedraw();
    }

    Node* mBody = nullptr;
    std::string mBodyPath;
    NodeId mGarment = kNoNode;
    Sdf mSdf;
    std::vector<glm::dvec3> mPoints;
    QPointF mCursor;
    bool mMirror = true;
};

/** The garment behind the selection: a garment node, or a mesh one feeds. */
NodeId selectedGarmentId(Graph& g)
{
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
        {
            if (n->type().isA(kGarmentType))
                return n->id();
            if (const int in = n->type().attrIndex("inMesh"); in >= 0)
                if (const auto src = g.source({n->id(), in}))
                    if (Node* s = g.node(src->node); s && s->type().isA(kGarmentType))
                        return s->id();
        }
    return kNoNode;
}

/**
 * Cloth brushes on a garment's drape. Pin holds the cloth where it hangs (Shift unpins), Fold presses a fold line along
 * the stroke (Shift folds the other way), Shrink gathers the rest shape (Shift stretches it) and Steam smooths a live
 * garment as you drag. [ and ] size the brush.
 */
class ClothBrushTool final : public ViewportTool
{
public:
    enum class Kind
    {
        Pin,
        Fold,
        Shrink,
        Steam
    };

    explicit ClothBrushTool(Kind kind) : mKind(kind) {}

    const char* name() const override
    {
        switch (mKind)
        {
        case Kind::Pin: return "Pin Cloth";
        case Kind::Fold: return "Fold Cloth";
        case Kind::Shrink: return "Shrink Cloth";
        default: return "Steam Cloth";
        }
    }

    void activate() override
    {
        Graph& g = Graph::current();
        mGarment = selectedGarmentId(g);
        if (mKind == Kind::Steam && mGarment != kNoNode)
            if (Node* n = g.node(mGarment))
                g.setValue({n->id(), n->type().attrIndex("live")}, true);
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || mGarment == kNoNode)
            return false;
        mStroke.clear();
        mInvert = e->modifiers() & Qt::ShiftModifier;
        sample(e->position(), view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursor = e->position();
        if (!(e->buttons() & Qt::LeftButton) || mGarment == kNoNode)
        {
            viewportTools::requestRedraw();
            return false;
        }
        sample(e->position(), view);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || mStroke.empty())
            return false;
        apply();
        mStroke.clear();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight)
        {
            mRadius = std::clamp(mRadius * (e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25), 0.5, 60.0);
            viewportTools::requestRedraw();
            return true;
        }
        return false;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override { return e.key() == Qt::Key_BracketLeft || e.key() == Qt::Key_BracketRight; }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        if (mGarment == kNoNode)
        {
            drawToolStatus(p, view, {.tool = name(), .prompt = QString("select a garment, then pick %1 again").arg(name())});
            return;
        }
        const glm::vec4 colour(0.95f, 0.75f, 0.3f, 1.f);
        std::vector<QPointF> screen;
        for (const glm::dvec3& q : mStroke)
            if (const auto s = view.project(q))
                screen.push_back(*s);
        if (screen.size() >= 2)
            p.polyline(screen, 2.f, colour);
        // The brush ring at the cursor, sized by projecting the radius at the last hit.
        const double px = mStroke.empty() ? 20.0 : mRadius / std::max(view.unitsPerPixel(mStroke.back()), 1e-9);
        std::vector<QPointF> ring;
        for (int k = 0; k < 40; ++k)
            ring.push_back(mCursor + QPointF(std::cos(k * 0.157079633), std::sin(k * 0.157079633)) * px);
        p.polyline(ring, 1.5f, colour, true);
        static const char* prompts[] = {"paint to pin the cloth where it hangs", "draw a fold line", "paint to shrink the cloth",
                                        "drag to steam the live cloth smooth"};
        static const char* shifts[] = {"unpin", "fold the other way", "stretch", nullptr};
        ToolStatus status{.tool = name(), .fields = {{"radius", QString("%1 cm").arg(mRadius, 0, 'f', 1)}}, .prompt = prompts[int(mKind)]};
        if (shifts[int(mKind)])
            status.keys.push_back({"Shift", shifts[int(mKind)]});
        status.keys.push_back({"[ ]", "radius"});
        drawToolStatus(p, view, status);
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    /** The cursor's ray against the garment's drape, world cm. */
    std::optional<glm::dvec3> pick(QPointF px, const ViewInfo& view) const
    {
        Graph& g = Graph::current();
        const Node* n = g.node(mGarment);
        if (!n)
            return std::nullopt;
        const PlugValue v = g.value({mGarment, n->type().attrIndex("outMesh")});
        const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
        if (!mesh || !*mesh)
            return std::nullopt;
        const geo::Mesh& m = **mesh;
        const glm::dmat4 world = garmentWorld(g, mGarment);
        glm::dvec3 o, d;
        view.ray(px, o, d);
        double best = 1e30;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            const std::vector<geo::Index> pts = m.facePoints(f);
            if (pts.size() != 3)
                continue;
            const glm::dvec3 a = glm::dvec3(world * glm::dvec4(m.position(pts[0]), 1.0));
            const glm::dvec3 e1 = glm::dvec3(world * glm::dvec4(m.position(pts[1]), 1.0)) - a;
            const glm::dvec3 e2 = glm::dvec3(world * glm::dvec4(m.position(pts[2]), 1.0)) - a;
            // Moller-Trumbore, both sides.
            const glm::dvec3 h = glm::cross(d, e2);
            const double det = glm::dot(e1, h);
            if (std::abs(det) < 1e-12)
                continue;
            const glm::dvec3 s = o - a;
            const double u = glm::dot(s, h) / det;
            if (u < 0.0 || u > 1.0)
                continue;
            const glm::dvec3 q = glm::cross(s, e1);
            const double w = glm::dot(d, q) / det;
            if (w < 0.0 || u + w > 1.0)
                continue;
            if (const double t = glm::dot(e2, q) / det; t > 0.0 && t < best)
                best = t;
        }
        if (best >= 1e30)
            return std::nullopt;
        return o + d * best;
    }

    void sample(QPointF px, const ViewInfo& view)
    {
        const auto hit = pick(px, view);
        if (!hit)
            return;
        // Dabs a third of the brush apart; folds keep every point the stroke needs to bend round.
        const double spacing = mKind == Kind::Fold ? 0.5 : mRadius / 3.0;
        if (!mStroke.empty() && glm::length(*hit - mStroke.back()) < spacing)
            return;
        mStroke.push_back(*hit);
        if (mKind == Kind::Steam)
            run("tailorRelax", {{"points", {{hit->x, hit->y, hit->z}}}, {"radius", mRadius}});
        viewportTools::requestRedraw();
    }

    void apply()
    {
        if (mKind == Kind::Steam)
            return;
        nlohmann::json pts = nlohmann::json::array();
        for (const glm::dvec3& q : mStroke)
            pts.push_back({q.x, q.y, q.z});
        switch (mKind)
        {
        case Kind::Pin: run("tailorPin", {{"points", pts}, {"radius", mRadius}, {"remove", mInvert}}); break;
        case Kind::Fold:
            if (mStroke.size() >= 2)
                run("tailorFold", {{"points", pts}, {"angle", mInvert ? -90.0 : 90.0}});
            break;
        case Kind::Shrink: run("tailorShrink", {{"points", pts}, {"radius", mRadius}, {"scale", mInvert ? 1.1 : 0.9}}); break;
        default: break;
        }
    }

    void run(const char* command, nlohmann::json call) const
    {
        Graph& g = Graph::current();
        const Node* n = g.node(mGarment);
        if (!n)
            return;
        call["args"] = {n->path()};
        try
        {
            ops::execute(command, call, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
    }

    Kind mKind;
    NodeId mGarment = kNoNode;
    std::vector<glm::dvec3> mStroke;
    QPointF mCursor;
    double mRadius = 4.0; // cm
    bool mInvert = false;
};
} // namespace

void registerGrabTool(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = "tailor.grab", .label = "Grab Cloth", .icon = "hand-grab", .create = [] {
                                 return std::make_unique<GrabTool>();
                             }});
    viewportTools::add(ctx, {.id = "tailor.drawPanel", .label = "Draw Panel", .icon = "pencil", .create = [] {
                                 return std::make_unique<DrawPanelTool>();
                             }});
    viewportTools::add(ctx, {.id = "tailor.pin", .label = "Pin Cloth", .icon = "pinned", .create = [] {
                                 return std::make_unique<ClothBrushTool>(ClothBrushTool::Kind::Pin);
                             }});
    viewportTools::add(ctx, {.id = "tailor.fold", .label = "Fold Cloth", .icon = "fold", .create = [] {
                                 return std::make_unique<ClothBrushTool>(ClothBrushTool::Kind::Fold);
                             }});
    viewportTools::add(ctx, {.id = "tailor.shrink", .label = "Shrink Cloth", .icon = "arrows-minimize", .create = [] {
                                 return std::make_unique<ClothBrushTool>(ClothBrushTool::Kind::Shrink);
                             }});
    viewportTools::add(ctx, {.id = "tailor.steam", .label = "Steam Cloth", .icon = "ironing-steam", .create = [] {
                                 return std::make_unique<ClothBrushTool>(ClothBrushTool::Kind::Steam);
                             }});
}
} // namespace tailor
