#include "ui/RigViewport.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/Ik.h"
#include "scene/Rig.h"
#include "ui/Actions.h"
#include "ui/DominantInfluenceUi.h"
#include "ui/SpacesUi.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/WeightPaintTool.h"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace lila::ui
{
namespace
{
// Same grayscale ranks as locators: unselected, under a selected node, selected.
const glm::vec4 kJointColor(0.02f, 0.02f, 0.02f, 1.0f);
const glm::vec4 kChildOfSelectedColor(0.55f, 0.55f, 0.55f, 1.0f);
const glm::vec4 kSelectedColor(0.95f, 0.95f, 0.95f, 1.0f);
constexpr double kPi = 3.14159265358979323846;

using scene::Joint;
using scene::NodeId;

double displayScale() { return PrefStore::get().value<double>("general", "/rig/jointDisplayScale", 1.0); }

bool drawn(const scene::Node& n)
{
    return scene::rig::isJoint(n) && scene::effectivelyVisible(scene::Graph::current(), n.id()) && n.as<int64_t>(Joint::kDrawStyle) != Joint::kHidden;
}

glm::dmat3 axesOf(const glm::dmat4& world)
{
    glm::dmat3 m(world);
    for (int i = 0; i < 3; ++i)
        m[i] = glm::length(m[i]) > 1e-12 ? glm::normalize(m[i]) : glm::dvec3(i == 0, i == 1, i == 2);
    return m;
}

void addCircle(std::vector<render::LineVertex>& out, const glm::dvec3& c, const glm::dvec3& u, const glm::dvec3& v, double r,
               const glm::vec4& color, int segments = 20)
{
    for (int k = 0; k < segments; ++k)
    {
        double a0 = 2.0 * kPi * k / segments, a1 = 2.0 * kPi * (k + 1) / segments;
        out.push_back({glm::vec3(c + (u * std::cos(a0) + v * std::sin(a0)) * r), color});
        out.push_back({glm::vec3(c + (u * std::cos(a1) + v * std::sin(a1)) * r), color});
    }
}

void addLine(std::vector<render::LineVertex>& out, const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& color)
{
    out.push_back({glm::vec3(a), color});
    out.push_back({glm::vec3(b), color});
}

/** Maya-style bone: a four-sided double pyramid from the joint to the child, widest just past the joint sphere. */
void addBone(std::vector<render::LineVertex>& out, const glm::dvec3& from, const glm::dvec3& to, double r, const glm::vec4& color)
{
    glm::dvec3 d = to - from;
    double len = glm::length(d);
    if (len < 1e-6)
        return;
    d /= len;
    glm::dvec3 ref = std::abs(d.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    glm::dvec3 u = glm::normalize(glm::cross(d, ref)), v = glm::cross(d, u);
    double w = std::min(r, len * 0.2);
    glm::dvec3 base = from + d * std::min(r, len * 0.25);
    glm::dvec3 corners[4] = {base + u * w, base + v * w, base - u * w, base - v * w};
    for (int k = 0; k < 4; ++k)
    {
        addLine(out, corners[k], corners[(k + 1) % 4], color);
        addLine(out, corners[k], to, color);
        addLine(out, corners[k], from, color);
    }
}

void addBox(std::vector<render::LineVertex>& out, const glm::dvec3& c, const glm::dmat3& axes, double r, const glm::vec4& color)
{
    for (int a = 0; a < 3; ++a)
        for (int s1 = -1; s1 <= 1; s1 += 2)
            for (int s2 = -1; s2 <= 1; s2 += 2)
            {
                glm::dvec3 off = axes[(a + 1) % 3] * double(s1) * r + axes[(a + 2) % 3] * double(s2) * r;
                addLine(out, c + off - axes[a] * r, c + off + axes[a] * r, color);
            }
}

std::vector<NodeId> childJoints(scene::Graph& g, NodeId id)
{
    std::vector<NodeId> out;
    for (NodeId c : g.children(id))
        if (drawn(*g.node(c)))
            out.push_back(c);
    return out;
}

void drawJoints(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::unordered_set<NodeId> under;
    for (NodeId id : g.selection())
        for (NodeId d : g.descendants(id))
            under.insert(d);
    bool xray = PrefStore::get().value<bool>("general", "/rig/xrayJoints", false);
    auto& out = xray ? s.linesOnTop : s.lines;
    double scale = displayScale();

    // Highlighted ranks last, so a selected bone wins over an unselected one at equal depth.
    for (int pass = 0; pass < 3; ++pass)
        for (NodeId id : g.nodes())
        {
            scene::Node* n = g.node(id);
            if (!drawn(*n))
                continue;
            int rank = g.isSelected(id) ? 2 : under.contains(id) ? 1 : 0;
            if (rank != pass)
                continue;
            const glm::vec4& color = rank == 2 ? kSelectedColor : rank == 1 ? kChildOfSelectedColor : kJointColor;
            glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            glm::dvec3 p(w[3]);
            glm::dmat3 axes = axesOf(w);
            double r = n->as<double>(Joint::kRadius) * scale;
            if (n->as<int64_t>(Joint::kDrawStyle) == Joint::kBox)
                addBox(out, p, axes, r, color);
            else
            {
                addCircle(out, p, axes[0], axes[1], r, color);
                addCircle(out, p, axes[1], axes[2], r, color);
                addCircle(out, p, axes[2], axes[0], r, color);
            }
            for (NodeId c : childJoints(g, id))
                addBone(out, p, glm::dvec3(g.node(c)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]), r, color);
        }
}

bool isIkHandle(const scene::Node& n)
{
    return n.type().isA("ikHandle") && scene::effectivelyVisible(scene::Graph::current(), n.id());
}

/** IK handles: a cross at the target, a line from the chain's start to it, and the pole vector from the start. */
void drawIkHandles(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    auto& out = PrefStore::get().value<bool>("general", "/rig/xrayJoints", false) ? s.linesOnTop : s.lines;
    double scale = displayScale();
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!isIkHandle(*n))
            continue;
        const glm::vec4& color = g.isSelected(id) ? kSelectedColor : kChildOfSelectedColor;
        glm::dvec3 p(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        std::vector<NodeId> chain = scene::ik::chainOf(g, id);
        double r = 2.0 * scale;
        if (!chain.empty())
            r = g.node(chain.front())->as<double>(Joint::kRadius) * 1.5 * scale;
        for (int a = 0; a < 3; ++a)
        {
            glm::dvec3 d(0.0);
            d[a] = r;
            addLine(out, p - d, p + d, color);
        }
        addCircle(out, p, {1, 0, 0}, {0, 0, 1}, r * 0.6, color, 12);
        if (chain.empty())
            continue;
        glm::dvec3 start(g.node(chain.front())->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        addLine(out, start, p, color);
        glm::dvec3 pole = n->as<glm::dvec3>(scene::IkHandle::kPoleVector);
        if (glm::length(pole) > 1e-9)
            addLine(out, start, start + glm::normalize(pole) * glm::length(p - start) * 0.5, kChildOfSelectedColor);
    }
}

std::vector<viewportHooks::PickHit> pickIkHandles(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    scene::Graph& g = scene::Graph::current();
    QRectF box = QRectF(a, b).normalized();
    std::vector<viewportHooks::PickHit> hits;
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!isIkHandle(*n))
            continue;
        auto c = view.project(glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]));
        if (!c)
            continue;
        if (!click)
        {
            if (box.contains(*c))
                hits.push_back({n->name(), 0.0});
            continue;
        }
        if (double d = std::hypot(c->x() - a.x(), c->y() - a.y()); d < 10.0)
            hits.push_back({n->name(), d});
    }
    return hits;
}

double segDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t - p;
    return std::hypot(q.x(), q.y());
}

std::vector<viewportHooks::PickHit> pickJoints(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    QRectF box = QRectF(a, b).normalized();
    double scale = displayScale();
    std::vector<viewportHooks::PickHit> hits;
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!drawn(*n))
            continue;
        glm::dvec3 p(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        auto c = view.project(p);
        if (!c)
            continue;
        if (!click)
        {
            if (box.contains(*c))
                hits.push_back({n->name(), 0.0});
            continue;
        }
        double rpx = n->as<double>(Joint::kRadius) * scale / view.unitsPerPixel(p);
        double best = std::max(0.0, std::hypot(c->x() - a.x(), c->y() - a.y()) - rpx);
        // A bone picks the joint it starts from, like Maya.
        for (NodeId ch : childJoints(g, id))
            if (auto cc = view.project(glm::dvec3(g.node(ch)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3])))
                best = std::min(best, segDistance(a, *c, *cc));
        if (best < 8.0)
            hits.push_back({n->name(), best});
    }
    return hits;
}

/** Click to place joints in a chain on the view plane through the last joint; Enter starts a new chain. */
class JointTool final : public ViewportTool
{
public:
    const char* name() const override { return "jointTool"; }

    void activate() override { log::info("Joint tool: click to place joints, Enter to start a new chain, Esc to finish"); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        glm::dvec3 anchor(0.0);
        for (NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && scene::rig::isJoint(*n))
                anchor = glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        glm::dvec3 o, d;
        view.ray(e->position(), o, d);
        // The world plane most facing the camera, so front/side views place joints where they're drawn.
        glm::dvec3 vd = glm::abs(view.viewDir(anchor));
        glm::dvec3 normal(0.0);
        normal[vd.x > vd.y && vd.x > vd.z ? 0 : vd.y > vd.z ? 1 : 2] = 1.0;
        double denom = glm::dot(d, normal);
        if (std::abs(denom) < 1e-9)
            return true;
        double t = glm::dot(anchor - o, normal) / denom;
        glm::dvec3 hit = o + d * t;
        actions::run({{"command", "joint"}, {"args", {{"position", {hit.x, hit.y, hit.z}}}}});
        return true;
    }

    bool mouseRelease(QMouseEvent*, const ViewInfo&) override { return true; }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            scene::Graph::current().select({});
            return true;
        }
        if (e->key() == Qt::Key_Escape)
        {
            // Not from inside our own handler: set() destroys this tool.
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolMessage(p, view, "Joint tool: click to place, Enter for a new chain, Esc to finish");
    }
};

void command(const char* name, nlohmann::json args = nlohmann::json::object())
{
    actions::run({{"command", name}, {"args", std::move(args)}});
}

void registerRigActions()
{
    auto add = [](const char* id, const char* label, std::function<void()> fn, const char* python, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Rig", .run = std::move(fn), .doc = doc, .python = python});
    };
    add("rig.jointTool", "Joint Tool", [] { viewportTools::set(std::make_unique<JointTool>()); }, "",
        "Click in the viewport to place joints in a chain; Enter starts a new chain, Esc finishes.");
    add("rig.orientJoint", "Orient Joint", [] { command("orientJoint"); }, "orientJoint()",
        "Aims each selected joint's primary axis at its child, with the secondary axis toward a world axis.");
    add("rig.mirrorJoints", "Mirror Joints", [] { command("mirrorJoints"); }, "mirrorJoints()",
        "Copies the selected joint hierarchy to the other side of a plane through the world origin.");
    add("rig.insertJoint", "Insert Joint", [] { command("insertJoint"); }, "insertJoint()", "Splits the bone below the selected joint with evenly spaced joints.");
    add("rig.rerootSkeleton", "Reroot Skeleton", [] { command("rerootSkeleton"); }, "rerootSkeleton()",
        "Makes the selected joint the root by reversing the chain above it, keeping every joint in place.");
    add("rig.bindSkin", "Bind Skin", [] { command("skinCluster"); }, "skinCluster()",
        "Binds the selected mesh to the selected joints, weighting each point by its closest bone.");
    add("rig.bindSkinHeatmap", "Bind Skin (Heat Map)", [] { command("skinCluster", {{"method", "heatmap"}}); },
        "skinCluster(method=\"heatmap\")", "Binds like Bind Skin, but ignores bones hidden behind the surface and smooths the weights.");
    add("rig.unbindSkin", "Unbind Skin", [] { command("unbindSkin"); }, "unbindSkin()",
        "Removes the selected mesh's skinning; the mesh returns to its bind shape.");
    add("rig.goToBindPose", "Go to Bind Pose", [] { command("goToBindPose"); }, "goToBindPose()",
        "Puts the skinned mesh's joints back where they were when bound.");
    add("rig.paintWeights", "Paint Skin Weights", [] { startWeightPaint(); }, "", "Paint how strongly each joint moves the selected skinned mesh.");
    add("rig.mirrorSkinWeights", "Mirror Skin Weights", [] { command("mirrorSkinWeights"); }, "mirrorSkinWeights()",
        "Copies weights from one side of the mesh to the matching joints on the other.");
    add("rig.copySkinWeights", "Copy Skin Weights", [] { command("copySkinWeights"); }, "copySkinWeights()",
        "Copies weights from the first selected skinned mesh to the second by closest surface point.");
    add("rig.normalizeWeights", "Normalize Weights", [] { command("normalizeWeights"); }, "normalizeWeights()",
        "Scales each point's weights to sum to 1; locked joints keep theirs.");
    add("rig.pruneWeights", "Prune Small Weights", [] { command("pruneWeights"); }, "pruneWeights()",
        "Drops tiny weights and any beyond the max influences per point, then renormalizes.");
    add("rig.parentConstraint", "Parent Constraint", [] { command("parentConstraint"); }, "parentConstraint()",
        "The last selected object follows the others' position and rotation as if parented to them.");
    add("rig.pointConstraint", "Point Constraint", [] { command("pointConstraint"); }, "pointConstraint()",
        "The last selected object follows the others' position only.");
    add("rig.orientConstraint", "Orient Constraint", [] { command("orientConstraint"); }, "orientConstraint()",
        "The last selected object follows the others' rotation only.");
    add("rig.aimConstraint", "Aim Constraint", [] { command("aimConstraint"); }, "aimConstraint()",
        "The last selected object turns to point at the others.");
    add("rig.removeConstraint", "Remove Constraint", [] { command("removeConstraint"); }, "removeConstraint()",
        "Deletes the selected constraints, or those on the selected objects, keeping the current pose.");
    add("rig.ikHandle", "IK Handle", [] { command("ikHandle"); }, "ikHandle()",
        "Solves the chain from the first selected joint to the last with an IK handle at its end.");
    add("rig.editSkeleton", "Edit Skeleton", [] { command("editSkeleton"); }, "editSkeleton()",
        "Toggles skeleton editing: the skinned skeleton goes to bind pose so joints can be moved and reoriented "
        "without deforming the mesh; turning it off rebinds and keeps the animation.");
    add("rig.xrayJoints", "X-Ray Joints", [] {
        bool on = PrefStore::get().value<bool>("general", "/rig/xrayJoints", false);
        PrefStore::get().set("general", "/rig/xrayJoints", !on);
        viewportTools::requestRedraw();
    }, "", "Draws joints through meshes so they can be seen and picked inside.");
}
} // namespace

void registerRigViewport()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    viewportHooks::addDrawer(drawJoints);
    viewportHooks::addPicker(pickJoints);
    viewportHooks::addDrawer(drawIkHandles);
    viewportHooks::addPicker(pickIkHandles);
    registerRigActions();
    registerDominantInfluenceUi();
    registerSpacesUi();
}
} // namespace lila::ui
