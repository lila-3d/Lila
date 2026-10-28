#include "scene/RigElements.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/Ik.h"
#include "scene/Rig.h"
#include "scene/Transform.h"

#include <algorithm>
#include <set>

namespace lila::scene::rigel
{
using namespace rigmath;

namespace
{
/** Index of the first own attr of the transform-based types: the transform type's attr count. */
int gTransformAttrs = Transform::kCount;

glm::dmat3 eulerMatrix(const glm::dvec3& degrees, int64_t order) { return glm::dmat3(eulerToMatrix(degrees, RotateOrder(std::clamp<int64_t>(order, 0, 5)))); }
glm::dmat3 orientMatrix(const glm::dvec3& degrees) { return glm::dmat3(eulerToMatrix(degrees, RotateOrder::XYZ)); }

/** rotate values whose R term makes a joint's world rotation `world`, given its parent's and its rest data. */
glm::dvec3 solvedRotateFor(const glm::dmat3& parentWorld, const glm::dvec3& jointOrient, const glm::dvec3& rotateAxis, int64_t order,
                           const glm::dmat3& world)
{
    const glm::dmat3 r = localRotationFor(parentWorld, orientMatrix(jointOrient), world) * glm::transpose(orientMatrix(rotateAxis));
    return matrixToEuler(r, RotateOrder(std::clamp<int64_t>(order, 0, 5)));
}

AttrDef number(const char* name, double def, const char* doc, uint32_t extra = 0, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = name, .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc, .minValue = lo,
            .maxValue = hi};
}
AttrDef whole(const char* name, int64_t def, const char* doc, uint32_t extra = 0, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = name, .type = PlugType::Int, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc, .minValue = lo,
            .maxValue = hi};
}
/** Signed joint axes, as rig::axisVector reads them; stored as their index. */
const std::vector<std::string> kAxisNames = {"x", "-x", "y", "-y", "z", "-z"};

AttrDef axisChoice(const char* name, int64_t def, const char* doc)
{
    return {.name = name, .type = PlugType::Enum, .defaultValue = def, .flags = AttrInput | AttrStorable, .enumNames = kAxisNames, .doc = doc};
}

const std::string& axisName(int64_t index) { return kAxisNames[size_t(std::clamp<int64_t>(index, 0, int64_t(kAxisNames.size()) - 1))]; }

/** The kAxisNames index of an axis like "x", "+y" or "-z"; throws std::invalid_argument for anything else. */
int64_t axisIndex(std::string_view axis)
{
    const glm::dvec3 v = rig::axisVector(axis);
    for (int i = 0; i < 3; ++i)
        if (v[i] != 0.0)
            return i * 2 + (v[i] < 0.0 ? 1 : 0);
    return 0;
}

AttrDef text(const char* name, const char* doc, uint32_t extra = 0)
{
    return {.name = name, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
AttrDef vector3(const char* name, glm::dvec3 def, const char* doc, uint32_t extra = 0)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = AttrInput | AttrStorable | extra, .doc = doc};
    const std::string n = name;
    a.childNames = {n + "X", n + "Y", n + "Z"};
    return a;
}
AttrDef matrix(const char* name, const char* doc, uint32_t extra = 0)
{
    return {.name = name, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
AttrDef outFloat(const char* name, const char* doc)
{
    return {.name = name, .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput | AttrHidden, .doc = doc};
}
AttrDef outVec(const char* name, const char* doc)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.0), .flags = AttrOutput | AttrHidden, .doc = doc};
    const std::string n = name;
    a.childNames = {n + "X", n + "Y", n + "Z"};
    return a;
}
AttrDef outMatrix(const char* name, const char* doc)
{
    return {.name = name, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrOutput | AttrHidden, .doc = doc};
}

// IK chain --------------------------------------------------------------------------------------------------------

class IkChainNode : public Transform
{
public:
    enum Own
    {
        kStartJoint,
        kEndJoint,
        kHandle,
        kLock,
        kAlign,
        kGrab,
        kIkBlend,
    };
    static int at(int own) { return gTransformAttrs + own; }

    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        const double lock = as<double>(at(kLock)) >= 0.5 ? 1.0 : 0.0;
        setOutput(at(kIkBlend), std::max(lock, std::clamp(as<double>(at(kGrab)), 0.0, 1.0)));
    }
};

// Reverse foot ----------------------------------------------------------------------------------------------------

class ReverseFootNode : public Transform
{
public:
    enum Own
    {
        kAnkle,
        kBall,
        kToe,
        kChain,
        kAlign,
        kRoll,
        kTwist,
        kBank,
        kPoleAngle,
        kBallLimit,
        kToeLimit,
        kHeelPivot,
        kBallPivot,
        kToePivot,
        kInnerPivot,
        kOuterPivot,
        kRestAnkle,
        kRestBall,
        kBallOrient,
        kBallAxis,
        kBallRotateOrder,
        kIkWeight,
        kAnkleTarget,
        kAnkleMatrix,
        kBallRotate,
        kBallBlend,
        kRollBackMax,
        kRollForwardMax,
        kBankMax,
        kTwistMax,
        kZeroSnap,
    };
    static int at(int own) { return gTransformAttrs + own; }

    FootPivots pivots() const
    {
        return {as<glm::dvec3>(at(kHeelPivot)), as<glm::dvec3>(at(kBallPivot)), as<glm::dvec3>(at(kToePivot)), as<glm::dvec3>(at(kInnerPivot)),
                as<glm::dvec3>(at(kOuterPivot))};
    }
    FootChannels channels() const { return {as<double>(at(kRoll)), as<double>(at(kTwist)), as<double>(at(kBank))}; }
    FootLimits limits() const
    {
        return {.ballLimit = as<double>(at(kBallLimit)),
                .toeLimit = as<double>(at(kToeLimit)),
                .heelMax = as<double>(at(kRollBackMax)),
                .toeMax = as<double>(at(kRollForwardMax)),
                .bankMax = as<double>(at(kBankMax)),
                .twistMax = as<double>(at(kTwistMax)),
                .zeroSnap = as<double>(at(kZeroSnap))};
    }
    glm::dmat4 restAnkle() const { return as<glm::dmat4>(at(kRestAnkle)); }

    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        const glm::dmat4 control = as<glm::dmat4>(kWorldMatrix);
        const FootSolve s = solveFoot(pivots(), channels(), limits());
        const glm::dmat4 ankle = control * s.ankle * as<glm::dmat4>(at(kRestAnkle));
        const glm::dmat4 ball = control * s.ball * as<glm::dmat4>(at(kRestBall));
        setOutput(at(kAnkleTarget), glm::dvec3(ankle[3]));
        setOutput(at(kAnkleMatrix), ankle);
        setOutput(at(kBallRotate), solvedRotateFor(rotationOf(ankle), as<glm::dvec3>(at(kBallOrient)), as<glm::dvec3>(at(kBallAxis)),
                                                   as<int64_t>(at(kBallRotateOrder)), rotationOf(ball)));
        setOutput(at(kBallBlend), std::clamp(as<double>(at(kIkWeight)), 0.0, 1.0));
    }
};

// Align: a joint's rotation from a target matrix, under its (live) parent -------------------------------------------

class AlignNode : public Node
{
public:
    enum Own
    {
        kTarget,
        kParentMatrix,
        kWeight,
        kJointOrient,
        kRotateAxis,
        kRotateOrder,
        kSolvedRotate,
        kIkBlend,
        kOffset,
    };

    void compute(const EvalContext&) override
    {
        setOutput(kSolvedRotate, solvedRotateFor(rotationOf(as<glm::dmat4>(kParentMatrix)), as<glm::dvec3>(kJointOrient), as<glm::dvec3>(kRotateAxis),
                                                 as<int64_t>(kRotateOrder),
                                                 rotationOf(as<glm::dmat4>(kTarget)) * rotationOf(as<glm::dmat4>(kOffset))));
        setOutput(kIkBlend, std::clamp(as<double>(kWeight), 0.0, 1.0));
    }
};

// Looker ----------------------------------------------------------------------------------------------------------

class LookerNode : public Transform
{
public:
    enum Own
    {
        kJoint,
        kAimAxis,
        kUpAxis,
        kUpVector,
        kWeight,
        kJointParentMatrix,
        kRestTranslate,
        kJointOrient,
        kRotateAxis,
        kRotateOrder,
        kSolvedRotate,
        kIkBlend,
    };
    static int at(int own) { return gTransformAttrs + own; }

    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        const glm::dmat4 parent = as<glm::dmat4>(at(kJointParentMatrix));
        const glm::dvec3 from(parent * glm::dvec4(as<glm::dvec3>(at(kRestTranslate)), 1.0));
        const glm::dvec3 target(as<glm::dmat4>(kWorldMatrix)[3]);
        glm::dvec3 rotate = as<glm::dvec3>(at(kSolvedRotate));
        try
        {
            const glm::dvec3 aim = target - from;
            if (glm::length(aim) > 1e-9)
            {
                const glm::dmat3 world = rig::aimFrame(rig::axisVector(axisName(as<int64_t>(at(kAimAxis)))), rig::axisVector(axisName(as<int64_t>(at(kUpAxis)))),
                                                       glm::normalize(aim), as<glm::dvec3>(at(kUpVector)));
                rotate = solvedRotateFor(rotationOf(parent), as<glm::dvec3>(at(kJointOrient)), as<glm::dvec3>(at(kRotateAxis)),
                                         as<int64_t>(at(kRotateOrder)), world);
            }
        }
        catch (const std::exception&)
        {
        }
        setOutput(at(kSolvedRotate), rotate);
        setOutput(at(kIkBlend), std::clamp(as<double>(at(kWeight)), 0.0, 1.0));
    }
};

// Twist driver ----------------------------------------------------------------------------------------------------

class TwistNode : public Node
{
public:
    enum Own
    {
        kJoint,
        kSource,
        kSourceMatrix,
        kRestSource,
        kSourceAxis,
        kRestLocal,
        kDrivenAxis,
        kWeight,
        kJointOrient,
        kRotateAxis,
        kRotateOrder,
        kSolvedRotate,
        kIkBlend,
    };

    void compute(const EvalContext&) override
    {
        const glm::dmat3 delta = glm::transpose(rotationOf(as<glm::dmat4>(kRestSource))) * rotationOf(as<glm::dmat4>(kSourceMatrix));
        glm::dvec3 axis = as<glm::dvec3>(kSourceAxis), driven = as<glm::dvec3>(kDrivenAxis);
        double angle = 0.0;
        if (glm::length(axis) > 1e-12 && glm::length(driven) > 1e-12)
            angle = twistAngle(delta, glm::normalize(axis)) * as<double>(kWeight);
        const glm::dmat3 local = rotationOf(as<glm::dmat4>(kRestLocal)) *
                                 (angle == 0.0 ? glm::dmat3(1.0) : glm::dmat3(glm::rotate(glm::dmat4(1.0), radians(angle), glm::normalize(driven))));
        const glm::dmat3 r = glm::transpose(orientMatrix(as<glm::dvec3>(kJointOrient))) * local * glm::transpose(orientMatrix(as<glm::dvec3>(kRotateAxis)));
        setOutput(kSolvedRotate, matrixToEuler(r, RotateOrder(std::clamp<int64_t>(as<int64_t>(kRotateOrder), 0, 5))));
        setOutput(kIkBlend, 1.0);
    }
};

void registerTypesImpl()
{
    const NodeType* transform = types::find("transform");
    if (!transform)
        throw std::logic_error("rig elements need the built-in types first");
    gTransformAttrs = int(transform->attrs.size());
    const uint32_t hidden = AttrHidden;
    const uint32_t key = AttrKeyable;

    NodeType chain{.name = kIkChainType,
                   .base = transform,
                   .dag = true,
                   .doc = "An IK chain: an ikHandle from startJoint to endJoint, posing the joints' keyed rotate when its "
                          "controller is grabbed. A key with lock on holds the controller, and the hand or foot with it, "
                          "until the chain's next key.",
                   .icon = "line-dashed",
                   .create = [] { return std::make_unique<IkChainNode>(); }};
    chain.ownAttrs = {
        text("startJoint", "First joint of the chain."),
        text("endJoint", "The end effector."),
        text("handle", "The ikHandle solving the chain."),
        number("lock", 0.0, "1 at a key plants the controller: it holds its place until the chain's next key. Keyed stepped.", key, 0.0, 1.0),
        text("align", "The node turning the end effector with the controller.", hidden),
        {.name = "grab", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrHidden, .doc = "1 while the controller is being posed."},
        outFloat("ikBlend", "1 while the solver holds the chain (locked or grabbed), driving the handle's ikBlend."),
        number("drawThickness", 4.0, "Width of the controller's line in the viewport, pixels.", 0, 0.0),
        number("drawRadius", 1.0, "Size of the controller's ring in the viewport, as a multiple of its default.", 0, 0.0),
    };

    NodeType foot{.name = kReverseFootType,
                  .base = transform,
                  .dag = true,
                  .doc = "A reverse foot: the control sits on the sole and drives the leg's IK goal, rolling the foot over heel, "
                         "ball and toe, twisting on the ball and banking over the sole's edges.",
                  .icon = "shoe",
                  .create = [] { return std::make_unique<ReverseFootNode>(); }};
    foot.ownAttrs = {
        text("ankle", "The ankle joint (the IK chain's end effector)."),
        text("ball", "The ball joint."),
        text("toe", "The toe joint."),
        text("chain", "The IK chain the foot drives."),
        text("align", "The node turning the ankle with the foot.", hidden),
        number("roll", 0.0, "Negative rocks back on the heel; positive lifts the heel onto the ball, then onto the toe.", key),
        number("twist", 0.0, "Swivel on the ball, degrees.", key),
        number("bank", 0.0, "Tip onto the outer (positive) or inner (negative) edge, degrees.", key),
        number("poleAngle", 0.0, "Roll of the leg's bend plane, degrees.", key),
        number("ballLimit", 30.0, "Roll the ball takes before handing over to the toe, degrees.", 0, 0.0),
        number("toeLimit", 60.0, "Roll at which all of it is on the toe, degrees.", 0, 0.0),
        vector3("heelPivot", glm::dvec3(0.0), "Heel pivot, control space."),
        vector3("ballPivot", glm::dvec3(0.0), "Ball pivot, control space."),
        vector3("toePivot", glm::dvec3(0.0), "Toe tip pivot, control space."),
        vector3("innerPivot", glm::dvec3(0.0), "Inner edge pivot, control space."),
        vector3("outerPivot", glm::dvec3(0.0), "Outer edge pivot, control space."),
        matrix("restAnkle", "The ankle in control space at setup.", hidden),
        matrix("restBall", "The ball in control space at setup.", hidden),
        vector3("ballOrient", glm::dvec3(0.0), "The ball joint's jointOrient at setup.", hidden),
        vector3("ballAxis", glm::dvec3(0.0), "The ball joint's rotateAxis at setup.", hidden),
        whole("ballRotateOrder", 0, "The ball joint's rotate order at setup.", hidden, 0.0, 5.0),
        number("ikWeight", 1.0, "The chain's IK weight; the ball follows the foot as much.", hidden, 0.0, 1.0),
        outVec("ankleTarget", "Where the IK goal goes, world space."),
        outMatrix("ankleMatrix", "The posed ankle, world space."),
        outVec("ballRotate", "The ball joint's solved rotate."),
        outFloat("ballBlend", "The ball joint's IK weight."),
        number("rollBackMax", 60.0, "Furthest the foot rocks back onto the heel, degrees.", 0, 0.0),
        number("rollForwardMax", 90.0, "Furthest the foot rolls forward onto the toe, degrees.", 0, 0.0),
        number("bankMax", 45.0, "Furthest it tips onto either edge, degrees.", 0, 0.0),
        number("twistMax", 90.0, "Furthest it swivels either way, degrees.", 0, 0.0),
        number("zeroSnap", 0.5, "Roll, twist and bank nearer zero than this count as zero, degrees.", 0, 0.0),
        number("drawThickness", 4.0, "Width of the sole loop in the viewport, pixels.", 0, 0.0),
        number("drawRadius", 1.0, "Size of the sole loop in the viewport, as a multiple of the sole.", 0, 0.0),
    };

    NodeType align{.name = kAlignType,
                   .doc = "Turns a joint to a target matrix under its parent, through the joint's solvedRotate and ikBlend.",
                   .icon = "arrows-diagonal",
                   .create = [] { return std::make_unique<AlignNode>(); }};
    align.ownAttrs = {
        matrix("target", "World matrix the joint takes the rotation of."),
        matrix("parentMatrix", "The joint's parent world matrix."),
        number("weight", 1.0, "How much the joint follows.", 0, 0.0, 1.0),
        vector3("jointOrient", glm::dvec3(0.0), "The joint's jointOrient.", hidden),
        vector3("rotateAxis", glm::dvec3(0.0), "The joint's rotateAxis.", hidden),
        whole("rotateOrder", 0, "The joint's rotate order.", hidden, 0.0, 5.0),
        outVec("solvedRotate", "Rotate values for the joint."),
        outFloat("ikBlend", "The joint's ikBlend."),
        matrix("offset", "The joint's rotation relative to the target's, applied after it.", hidden),
    };

    NodeType looker{.name = kLookerType,
                    .base = transform,
                    .dag = true,
                    .doc = "A looker: the joint aims its aimAxis at this node, its upAxis leaning toward upVector. weight "
                           "blends it over the joint's own rotation.",
                    .icon = "eye",
                    .create = [] { return std::make_unique<LookerNode>(); }};
    looker.ownAttrs = {
        text("joint", "The joint that looks."),
        axisChoice("aimAxis", 0, "Joint axis aimed at the looker."),
        axisChoice("upAxis", 2, "Joint axis leaned toward upVector."),
        vector3("upVector", glm::dvec3(0.0, 1.0, 0.0), "World up for the aim."),
        number("weight", 1.0, "Blend of the aim over the joint's own rotation.", key, 0.0, 1.0),
        matrix("jointParentMatrix", "The joint's parent world matrix.", hidden),
        vector3("restTranslate", glm::dvec3(0.0), "The joint's translate at setup.", hidden),
        vector3("jointOrient", glm::dvec3(0.0), "The joint's jointOrient at setup.", hidden),
        vector3("jointRotateAxis", glm::dvec3(0.0), "The joint's rotateAxis at setup.", hidden),
        whole("jointRotateOrder", 0, "The joint's rotate order at setup.", hidden, 0.0, 5.0),
        outVec("solvedRotate", "Rotate values for the joint."),
        outFloat("ikBlend", "The joint's ikBlend."),
    };

    NodeType twist{.name = kTwistType,
                   .doc = "Turns a twist joint about its bone by a share of another joint's twist: its parent's (countered, "
                          "negative weight) or the next joint's along the limb (followed), as UE's control rig does.",
                   .icon = "rotate-360",
                   .create = [] { return std::make_unique<TwistNode>(); }};
    twist.ownAttrs = {
        text("joint", "The twist joint."),
        text("source", "The joint whose twist is read."),
        matrix("sourceMatrix", "The source's local matrix."),
        matrix("restSource", "The source's local matrix at setup.", hidden),
        vector3("sourceAxis", glm::dvec3(1.0, 0.0, 0.0), "The twist axis in the source's frame.", hidden),
        matrix("restLocal", "The twist joint's local rotation at setup.", hidden),
        vector3("drivenAxis", glm::dvec3(1.0, 0.0, 0.0), "The bone axis in the twist joint's frame.", hidden),
        number("weight", 0.5, "Share of the source's twist; negative counters it.", key),
        vector3("jointOrient", glm::dvec3(0.0), "The twist joint's jointOrient.", hidden),
        vector3("rotateAxis", glm::dvec3(0.0), "The twist joint's rotateAxis.", hidden),
        whole("rotateOrder", 0, "The twist joint's rotate order.", hidden, 0.0, 5.0),
        outVec("solvedRotate", "Rotate values for the joint."),
        outFloat("ikBlend", "The joint's ikBlend."),
    };

    for (NodeType* t : {&chain, &foot, &align, &looker, &twist})
        types::registerType(std::move(*t));
}

// Helpers ---------------------------------------------------------------------------------------------------------

const std::string& textAt(const Node& n, const char* attr)
{
    static const std::string none;
    const int a = n.type().attrIndex(attr);
    return a < 0 ? none : n.as<std::string>(a);
}

/** The node a string attr names, or null when it names none or several. Never throws, as drawing resolves these too. */
Node* nodeAt(Graph& g, const Node& n, const char* attr)
{
    const std::vector<NodeId> found = g.findAll(textAt(n, attr));
    return found.size() == 1 ? g.node(found[0]) : nullptr;
}

PlugRef plugAt(const Node& n, const char* attr) { return {n.id(), n.type().attrIndex(attr), -1}; }

glm::dmat4 worldOf(Graph& g, NodeId id) { return std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})); }
glm::dmat4 parentWorldOf(Graph& g, NodeId id) { return std::get<glm::dmat4>(g.value({id, Transform::kParentMatrix, -1})); }
glm::dvec3 vecOf(Graph& g, NodeId id, int attr) { return std::get<glm::dvec3>(g.value({id, attr, -1})); }
double numOf(Graph& g, const PlugRef& p) { return std::get<double>(g.value(p)); }

glm::dmat3 orientOf(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n || !rig::isJoint(*n))
        return glm::dmat3(1.0);
    return orientMatrix(vecOf(g, id, Joint::kJointOrient));
}

RotateOrder rotateOrderOf(Graph& g, NodeId id) { return RotateOrder(std::clamp<int64_t>(std::get<int64_t>(g.value({id, Transform::kRotateOrder, -1})), 0, 5)); }

/** Rotates the node so its world rotation becomes `world` through its rotate, continuous with the current values. */
void setWorldRotation(Graph& g, NodeId id, const glm::dmat3& world)
{
    const glm::dmat3 parent = rotationOf(parentWorldOf(g, id));
    const glm::dmat3 axis = orientMatrix(vecOf(g, id, Transform::kRotateAxis));
    const glm::dmat3 r = localRotationFor(parent, orientOf(g, id), world) * glm::transpose(axis);
    const glm::dvec3 now = vecOf(g, id, Transform::kRotate);
    const glm::dvec3 v = eulerNear(r, rotateOrderOf(g, id), now);
    if (glm::length(v - now) > 1e-12)
        g.setValue({id, Transform::kRotate, -1}, v);
}

void setWorldMatrix(Graph& g, NodeId id, const glm::dmat4& world)
{
    Transform::setLocalMatrix(g, id, glm::inverse(parentWorldOf(g, id)) * world);
}

bool nearMatrix(const glm::dmat4& a, const glm::dmat4& b, double eps)
{
    for (int c = 0; c < 4; ++c)
        if (glm::length(a[c] - b[c]) > eps)
            return false;
    return true;
}

/** Joints from start to end, start first; empty when end isn't below start. */
std::vector<NodeId> jointsBetween(Graph& g, NodeId start, NodeId end)
{
    std::vector<NodeId> out;
    for (NodeId cur = end; cur; cur = g.parent(cur))
    {
        out.push_back(cur);
        if (cur == start)
        {
            std::reverse(out.begin(), out.end());
            return out;
        }
    }
    return {};
}

NodeId firstChildJoint(Graph& g, NodeId id)
{
    for (NodeId c : g.children(id))
        if (rig::isJoint(*g.node(c)))
            return c;
    return kNoNode;
}

/** The pole channel the animator turns: the reverse foot's, else the handle's. */
std::optional<PlugRef> polePlug(Graph& g, NodeId chain)
{
    if (NodeId f = footForChain(g, chain))
        return plugAt(*g.node(f), "poleAngle");
    if (NodeId h = handleOf(g, chain))
        return plugAt(*g.node(h), "poleAngle");
    return std::nullopt;
}

PlugRef grabPlug(NodeId chain) { return {chain, IkChainNode::at(IkChainNode::kGrab), -1}; }
PlugRef lockPlug(NodeId chain) { return {chain, IkChainNode::at(IkChainNode::kLock), -1}; }

void setGrab(Graph& g, NodeId chain, double v)
{
    if (std::get<double>(g.value(grabPlug(chain))) != v)
        g.setValue(grabPlug(chain), v);
}

/** The average position of the chain's joints between its start and end: where its bend is. */
std::optional<glm::dvec3> bendPoint(Graph& g, const std::vector<NodeId>& joints)
{
    if (joints.size() < 3)
        return std::nullopt;
    glm::dvec3 sum(0.0);
    for (size_t i = 1; i + 1 < joints.size(); ++i)
        sum += glm::dvec3(worldOf(g, joints[i])[3]);
    return sum / double(joints.size() - 2);
}

/** Channels keyed with the controller: its translate and rotate, a foot's roll, twist, bank and pole, a handle's pole. */
std::vector<std::string> controllerPlugs(Graph& g, NodeId chain)
{
    std::vector<std::string> out;
    const NodeId c = controllerOf(g, chain);
    if (!c)
        return out;
    const std::string n = g.node(c)->path();
    out = {n + ".translate", n + ".rotate"};
    if (footForChain(g, chain))
        for (const char* ch : {".roll", ".twist", ".bank", ".poleAngle"})
            out.push_back(n + ch);
    else
        out.push_back(n + ".poleAngle");
    return out;
}

bool driven(Graph& g, NodeId id, int attr)
{
    if (g.source({id, attr, -1}))
        return true;
    if (g.attr({id, attr, -1}).type == PlugType::Vec3)
        for (int c = 0; c < 3; ++c)
            if (g.source({id, attr, c}))
                return true;
    return false;
}

/** Runs fn with the graph at time t, then puts the time back. */
template <class Fn> void atTime(Graph& g, double t, Fn&& fn)
{
    const double was = g.time();
    const bool move = std::abs(was - t) > 1e-9;
    if (move)
        g.setTime(t);
    g.evaluate();
    try
    {
        fn();
    }
    catch (...)
    {
        if (move)
            g.setTime(was);
        throw;
    }
    if (move)
        g.setTime(was);
}
} // namespace

// Finding ---------------------------------------------------------------------------------------------------------

bool isElement(const Node& n)
{
    for (const char* t : {kIkChainType, kReverseFootType, kAlignType, kLookerType, kTwistType})
        if (n.type().isA(t))
            return true;
    return false;
}

std::vector<NodeId> elementsNaming(Graph& g, const char* type, const char* attr, const std::string& name)
{
    // Compared by the node each names, as the same node can be written by name or by path.
    const std::vector<NodeId> target = g.findAll(name);
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(type) && n->type().attrIndex(attr) >= 0)
        {
            const Node* named = nodeAt(g, *n, attr);
            if (target.size() == 1 ? named && named->id() == target[0] : textAt(*n, attr) == name)
                out.push_back(id);
        }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<NodeId> chains(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kIkChainType))
            out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<NodeId> chainJoints(Graph& g, NodeId chain)
{
    const Node* c = g.node(chain);
    if (!c)
        return {};
    Node* s = nodeAt(g, *c, "startJoint");
    Node* e = nodeAt(g, *c, "endJoint");
    return s && e ? jointsBetween(g, s->id(), e->id()) : std::vector<NodeId>{};
}

std::vector<NodeId> posedJoints(Graph& g, NodeId chain)
{
    std::vector<NodeId> out = chainJoints(g, chain);
    if (NodeId f = footForChain(g, chain))
        if (Node* ball = nodeAt(g, *g.node(f), "ball"))
            out.push_back(ball->id());
    return out;
}

NodeId handleOf(Graph& g, NodeId chain)
{
    const Node* c = g.node(chain);
    Node* h = c ? nodeAt(g, *c, "handle") : nullptr;
    return h ? h->id() : kNoNode;
}

NodeId chainForEffector(Graph& g, NodeId joint)
{
    const std::vector<NodeId> c = elementsNaming(g, kIkChainType, "endJoint", g.node(joint)->path());
    return c.empty() ? kNoNode : c.front();
}

NodeId chainContaining(Graph& g, NodeId joint)
{
    for (NodeId id : chains(g))
    {
        const std::vector<NodeId> js = chainJoints(g, id);
        if (std::find(js.begin(), js.end(), joint) != js.end())
            return id;
    }
    return kNoNode;
}

NodeId footForChain(Graph& g, NodeId chain)
{
    const std::vector<NodeId> f = elementsNaming(g, kReverseFootType, "chain", g.node(chain)->path());
    return f.empty() ? kNoNode : f.front();
}

NodeId chainFrom(Graph& g, NodeId node)
{
    Node* n = g.node(node);
    if (!n)
        return kNoNode;
    if (n->type().isA(kIkChainType))
        return node;
    if (n->type().isA(kReverseFootType))
    {
        Node* c = nodeAt(g, *n, "chain");
        return c ? c->id() : kNoNode;
    }
    if (n->type().isA("ikHandle"))
    {
        const std::vector<NodeId> c = elementsNaming(g, kIkChainType, "handle", n->path());
        return c.empty() ? kNoNode : c.front();
    }
    if (rig::isJoint(*n))
    {
        if (NodeId c = chainForEffector(g, node))
            return c;
        return chainContaining(g, node);
    }
    return kNoNode;
}

NodeId chainOfController(Graph& g, NodeId node)
{
    Node* n = g.node(node);
    if (!n || !(n->type().isA(kReverseFootType) || n->type().isA("ikHandle")))
        return kNoNode;
    const NodeId chain = chainFrom(g, node);
    return chain && controllerOf(g, chain) == node ? chain : kNoNode;
}

NodeId controllerOf(Graph& g, NodeId chain)
{
    if (NodeId f = footForChain(g, chain))
        return f;
    return handleOf(g, chain);
}

// Posing ----------------------------------------------------------------------------------------------------------

bool locked(Graph& g, NodeId chain, double time)
{
    const PlugRef p = lockPlug(chain);
    if (std::abs(g.time() - time) < 1e-9)
        return numOf(g, p) >= 0.5;
    const double was = g.time();
    g.setTime(time);
    const bool on = numOf(g, p) >= 0.5;
    g.setTime(was);
    return on;
}

bool held(Graph& g, NodeId chain) { return numOf(g, {chain, IkChainNode::at(IkChainNode::kIkBlend), -1}) >= 0.5; }

glm::dmat4 controllerWorld(Graph& g, NodeId chain)
{
    const NodeId c = controllerOf(g, chain);
    if (!c)
        return glm::dmat4(1.0);
    if (held(g, chain))
        return worldOf(g, c);
    const std::vector<NodeId> joints = chainJoints(g, chain);
    if (joints.empty())
        return worldOf(g, c);
    const glm::dmat4 effector = worldOf(g, joints.back());
    if (NodeId f = footForChain(g, chain))
    {
        const auto& rf = static_cast<const ReverseFootNode&>(*g.node(f));
        glm::dmat4 m = controlFor(effector, rf.restAnkle(), rf.pivots(), rf.channels(), rf.limits());
        // Keep the control's own scale; only its placement follows the pose.
        const glm::dmat4 now = worldOf(g, f);
        for (int i = 0; i < 3; ++i)
            m[i] = glm::dvec4(glm::normalize(glm::dvec3(m[i])) * glm::length(glm::dvec3(now[i])), 0.0);
        return m;
    }
    glm::dmat3 offset(1.0);
    if (Node* al = nodeAt(g, *g.node(chain), "align"))
        offset = rotationOf(al->as<glm::dmat4>(AlignNode::kOffset));
    const glm::dmat4 now = worldOf(g, c);
    glm::dmat3 r = rotationOf(effector) * glm::transpose(offset);
    glm::dmat4 m(1.0);
    for (int i = 0; i < 3; ++i)
        m[i] = glm::dvec4(r[i] * glm::length(glm::dvec3(now[i])), 0.0);
    m[3] = effector[3];
    return m;
}

void matchController(Graph& g, NodeId chain)
{
    LILA_ZONE();

    g.evaluate();
    if (held(g, chain))
        return;
    const NodeId c = controllerOf(g, chain);
    const std::vector<NodeId> joints = chainJoints(g, chain);
    if (!c || joints.size() < 2)
        return;
    const glm::dmat4 want = controllerWorld(g, chain);
    if (!nearMatrix(want, worldOf(g, c), 1e-9))
        setWorldMatrix(g, c, want);

    // The bend: the pole turns until the solved knee or elbow lies where FK has it.
    const std::optional<PlugRef> pole = polePlug(g, chain);
    const std::optional<glm::dvec3> fkBend = bendPoint(g, joints);
    if (!pole || !fkBend)
        return;
    const glm::dvec3 root(worldOf(g, joints.front())[3]);
    const glm::dvec3 end(worldOf(g, joints.back())[3]);
    setGrab(g, chain, 1.0);
    const glm::dvec3 ikBend = *bendPoint(g, joints);
    setGrab(g, chain, 0.0);
    if (glm::length(end - root) < 1e-9)
        return;
    const double delta = signedAngle(ikBend - root, *fkBend - root, end - root);
    if (std::abs(delta) > 1e-9)
        g.setValue(*pole, numOf(g, *pole) + delta);
}

bool keysWhenPosed(Graph& g, NodeId chain)
{
    if (!UndoStack::get().enabled())
        return false;
    for (NodeId j : posedJoints(g, chain))
        if (driven(g, j, Transform::kRotate))
            return true;
    if (driven(g, chain, IkChainNode::at(IkChainNode::kLock)))
        return true;
    if (NodeId c = controllerOf(g, chain))
        return driven(g, c, Transform::kTranslate) || driven(g, c, Transform::kRotate);
    return false;
}

void solveToFk(Graph& g, NodeId chain, bool key)
{
    LILA_ZONE();

    const std::vector<NodeId> joints = posedJoints(g, chain);
    if (joints.empty())
        return;
    setGrab(g, chain, 1.0);
    std::vector<glm::dmat3> worlds;
    for (NodeId j : joints)
        worlds.push_back(rotationOf(worldOf(g, j)));
    setGrab(g, chain, 0.0);
    // Parents first, so each joint is solved against its parent's new FK frame.
    for (size_t i = 0; i < joints.size(); ++i)
        setWorldRotation(g, joints[i], worlds[i]);
    if (!key)
        return;
    std::vector<std::string> plugs;
    for (NodeId j : joints)
        plugs.push_back(g.node(j)->path() + ".rotate");
    for (const std::string& p : controllerPlugs(g, chain))
        plugs.push_back(p);
    ops::execute("setKeyframe", {{"args", plugs}});
    ops::execute("setKeyframe", {{"args", {g.node(chain)->path() + ".lock"}}, {"inTangentType", "step"}, {"outTangentType", "step"}});
    if (locked(g, chain, g.time()))
        if (NodeId c = controllerOf(g, chain))
        {
            const std::string n = g.node(c)->path();
            const double t = g.time();
            ops::execute("keyTangent", {{"args", {n + ".translate", n + ".rotate"}}, {"time", json::array({t, t})}, {"outTangentType", "step"}});
        }
}

std::vector<NodeId> poseControllers(Graph& g, const std::vector<NodeId>& nodes, const std::function<void()>& edit)
{
    LILA_ZONE();

    std::vector<NodeId> posed;
    for (NodeId n : nodes)
    {
        NodeId c = chainOfController(g, n);
        if (!c && g.node(n) && g.node(n)->type().isA(kIkChainType))
            c = n;
        if (c && std::find(posed.begin(), posed.end(), c) == posed.end())
            posed.push_back(c);
    }
    std::vector<bool> keys;
    for (NodeId c : posed)
    {
        keys.push_back(keysWhenPosed(g, c));
        matchController(g, c);
        setGrab(g, c, 1.0);
    }
    try
    {
        edit();
    }
    catch (...)
    {
        for (NodeId c : posed)
            setGrab(g, c, 0.0);
        throw;
    }
    for (size_t i = 0; i < posed.size(); ++i)
        solveToFk(g, posed[i], keys[i]);
    return posed;
}

void setLock(Graph& g, NodeId chain, double time, bool on)
{
    atTime(g, time, [&] {
        matchController(g, chain);
        const std::string name = g.node(chain)->path();
        ops::execute("setKeyframe", {{"args", {name + ".lock"}}, {"time", time}, {"value", on ? 1.0 : 0.0}, {"inTangentType", "step"},
                                      {"outTangentType", "step"}});
        const NodeId c = controllerOf(g, chain);
        if (!c)
            return;
        const std::string n = g.node(c)->path();
        const json plugs = controllerPlugs(g, chain);
        ops::execute("setKeyframe", {{"args", plugs}, {"time", time}});
        // The controller's key holds until its next key while locked.
        ops::execute("keyTangent", {{"args", {n + ".translate", n + ".rotate"}}, {"time", json::array({time, time})},
                                     {"outTangentType", on ? "step" : "auto"}});
    });
}

void swingPole(Graph& g, NodeId chain, double by, bool turn)
{
    const std::vector<NodeId> joints = chainJoints(g, chain);
    const std::optional<PlugRef> pole = polePlug(g, chain);
    if (joints.size() < 3 || !pole)
        throw std::invalid_argument(g.node(chain)->path() + " has no pole to swing");
    g.evaluate();
    const glm::dvec3 s(worldOf(g, joints.front())[3]);
    glm::dvec3 target(worldOf(g, controllerOf(g, chain))[3]);
    if (NodeId f = footForChain(g, chain))
        target = std::get<glm::dvec3>(g.value(plugAt(*g.node(f), "ankleTarget")));
    const glm::dvec3 before = *bendPoint(g, joints) - s;
    g.setValue(*pole, numOf(g, *pole) + by);
    if (!turn)
        return;
    // The controller turns as far as the knee (elbow) actually swung, around the line from the chain's start.
    const glm::dvec3 axis = target - s;
    const double angle = signedAngle(before, *bendPoint(g, joints) - s, axis);
    if (glm::length(axis) > 1e-9 && std::abs(angle) > 1e-12)
    {
        const NodeId goal = controllerOf(g, chain);
        glm::dmat4 m = glm::translate(glm::dmat4(1.0), target);
        m = glm::rotate(m, radians(angle), glm::normalize(axis));
        m = glm::translate(m, -target);
        setWorldMatrix(g, goal, m * worldOf(g, goal));
    }
}

void snapReverseFoot(Graph& g, NodeId foot, bool align)
{
    g.evaluate();
    Node& fn = *g.node(foot);
    const auto& rf = static_cast<const ReverseFootNode&>(fn);
    Node* ankle = nodeAt(g, fn, "ankle");
    if (!ankle)
        throw std::invalid_argument(fn.path() + " has lost its ankle");
    const glm::dmat4 ankleW = worldOf(g, ankle->id());
    const glm::dmat4 rest = rf.restAnkle();
    FootChannels ch;
    glm::dmat4 control = snapControl(ankleW, rest);
    if (align)
    {
        const FootAlign al = alignControl(ankleW, rest, rf.pivots(), rf.limits());
        control = al.control;
        ch = al.channels;
    }
    setWorldMatrix(g, foot, control);
    g.setValue({foot, ReverseFootNode::at(ReverseFootNode::kRoll), -1}, ch.roll);
    g.setValue({foot, ReverseFootNode::at(ReverseFootNode::kTwist), -1}, ch.twist);
    g.setValue({foot, ReverseFootNode::at(ReverseFootNode::kBank), -1}, ch.bank);
}

void addFootChannels(Graph& g, NodeId foot, std::optional<double> roll, std::optional<double> twist, std::optional<double> bank, bool absolute)
{
    Node& fn = *g.node(foot);
    auto limit = [&](const char* name) { return numOf(g, plugAt(fn, name)); };
    const std::tuple<const char*, std::optional<double>, double, double> channels[] = {
        {"roll", roll, -limit("rollBackMax"), limit("rollForwardMax")},
        {"twist", twist, -limit("twistMax"), limit("twistMax")},
        {"bank", bank, -limit("bankMax"), limit("bankMax")},
    };
    for (const auto& [ch, by, lo, hi] : channels)
    {
        if (!by)
            continue;
        const PlugRef p = plugAt(fn, ch);
        const double v = (absolute ? 0.0 : numOf(g, p)) + *by;
        const double snapped = std::abs(v) < limit("zeroSnap") ? 0.0 : v;
        g.setValue(p, std::clamp(snapped, lo, std::max(lo, hi)));
    }
}

bool isDeform(const Node& n) { return !rig::isJoint(n) || n.as<bool>(Joint::kDeform); }

// Creating --------------------------------------------------------------------------------------------------------

namespace
{
/** The foot's frame and pivots, from its joints and, when given, the world points of its skinned sole. */
struct FootSetup
{
    glm::dmat4 control{1.0};
    FootPivots pivots;
};

FootSetup footSetup(const glm::dvec3& ankle, const glm::dvec3& ball, const glm::dvec3& toe, const std::vector<glm::dvec3>& meshPoints)
{
    glm::dvec3 fwd = toe - ankle;
    fwd.y = 0.0;
    if (glm::length(fwd) < 1e-6)
    {
        fwd = ball - ankle;
        fwd.y = 0.0;
    }
    fwd = glm::length(fwd) < 1e-6 ? glm::dvec3(0.0, 0.0, 1.0) : glm::normalize(fwd);
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const glm::dvec3 side = glm::normalize(glm::cross(up, fwd));
    const double ground = std::min(ball.y, toe.y);
    const glm::dmat3 r(side, up, fwd);
    const glm::dvec3 origin(ankle.x, ground, ankle.z);
    auto local = [&](const glm::dvec3& p) {
        glm::dvec3 l = glm::transpose(r) * (p - origin);
        l.y = 0.0;
        return l;
    };
    FootSetup f;
    f.control = glm::dmat4(r);
    f.control[3] = glm::dvec4(origin, 1.0);
    const glm::dvec3 toeL = local(toe);
    const double length = std::max(glm::length(toeL), 1.0);
    f.pivots.heel = glm::dvec3(0.0, 0.0, -0.25 * length);
    f.pivots.ball = local(ball);
    f.pivots.toe = toeL;
    // The inner edge faces the body's centre line, x = 0.
    const double width = 0.3 * length;
    const double inward = glm::dot(side, glm::dvec3(ankle.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0)) >= 0.0 ? 1.0 : -1.0;
    f.pivots.inner = local(ball) + glm::dvec3(inward * width, 0.0, 0.0);
    f.pivots.outer = local(ball) - glm::dvec3(inward * width, 0.0, 0.0);
    std::vector<glm::dvec3> pts;
    for (const glm::dvec3& p : meshPoints)
        pts.push_back(glm::transpose(r) * (p - origin));
    double sole = 0.0;
    const glm::dvec3 ballL = local(ball);
    if (auto pivots = pivotsFromSole(pts, ballL.x, ballL.z, inward, &sole))
    {
        // The control sits on the sole itself, its pivots at y = 0.
        f.pivots = *pivots;
        for (glm::dvec3* p : {&f.pivots.heel, &f.pivots.ball, &f.pivots.toe, &f.pivots.inner, &f.pivots.outer})
            p->y = 0.0;
        f.control[3] = glm::dvec4(origin + up * sole, 1.0);
    }
    return f;
}

/** World points of the mesh faces the foot joints dominate, on every skinned mesh: the foot's skin. */
std::vector<glm::dvec3> solePoints(Graph& g, const std::vector<NodeId>& joints)
{
    std::vector<glm::dvec3> out;
    if (!ops::find("dominantInfluence") || !ops::find("meshData"))
        return out;
    for (NodeId id : g.nodes())
    {
        Node* m = g.node(id);
        if (!m || !m->type().isA("mesh"))
            continue;
        try
        {
            std::vector<int64_t> faces;
            for (NodeId j : joints)
                for (const json& f : ops::execute("dominantInfluence", {{"args", {m->path()}}, {"joint", g.node(j)->path()}}))
                    faces.push_back(f.get<int64_t>());
            if (faces.empty())
                continue;
            const json data = ops::execute("meshData", {{"args", {m->path()}}, {"worldSpace", true}});
            const json& points = data.at("points");
            const json& polys = data.at("faces");
            for (int64_t f : faces)
                if (f >= 0 && size_t(f) < polys.size())
                    for (const json& i : polys[size_t(f)])
                    {
                        const json& p = points.at(i.get<size_t>());
                        out.push_back({p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>()});
                    }
        }
        catch (const std::exception&)
        {
        }
    }
    return out;
}

void parentUnder(Graph& g, NodeId node, NodeId parent)
{
    if (parent)
        g.setParent(node, parent, false);
}

void connect(Graph& g, const PlugRef& src, const PlugRef& dst) { g.connect(src, dst, true); }

const NodeType& typeNamed(const char* name)
{
    const NodeType* t = types::find(name);
    if (!t)
        throw std::logic_error(std::string("rig elements: type ") + name + " is not registered");
    return *t;
}

NodeId makeAlign(Graph& g, NodeId joint, const std::string& name)
{
    Node* al = g.createNode(typeNamed(kAlignType), name);
    const NodeId id = al->id();
    g.setValue({id, AlignNode::kJointOrient}, vecOf(g, joint, Joint::kJointOrient));
    g.setValue({id, AlignNode::kRotateAxis}, vecOf(g, joint, Transform::kRotateAxis));
    g.setValue({id, AlignNode::kRotateOrder}, std::get<int64_t>(g.value({joint, Transform::kRotateOrder, -1})));
    connect(g, {g.parent(joint), Transform::kWorldMatrix, -1}, {id, AlignNode::kParentMatrix, -1});
    connect(g, {id, AlignNode::kSolvedRotate, -1}, {joint, Joint::kSolvedRotate, -1});
    connect(g, {id, AlignNode::kIkBlend, -1}, {joint, Joint::kIkBlend, -1});
    return id;
}

/** The child joint farthest from id: the next bone of the limb rather than a helper beside it. */
NodeId mainChildJoint(Graph& g, NodeId id, NodeId except)
{
    NodeId best = kNoNode;
    double far = -1.0;
    for (NodeId c : g.children(id))
        if (c != except && rig::isJoint(*g.node(c)))
            if (const double d = glm::length(vecOf(g, c, Transform::kTranslate)); d > far)
                far = d, best = c;
    return best;
}

glm::dmat3 localRotationOf(Graph& g, NodeId id)
{
    return orientOf(g, id) * eulerMatrix(vecOf(g, id, Transform::kRotate), int64_t(rotateOrderOf(g, id))) *
           orientMatrix(vecOf(g, id, Transform::kRotateAxis));
}
} // namespace

NodeId createIkChain(Graph& g, NodeId start, NodeId end, const ChainOptions& opts)
{
    LILA_ZONE();

    if (!rig::isJoint(*g.node(start)) || !rig::isJoint(*g.node(end)))
        throw std::invalid_argument("an IK chain runs between joints");
    if (jointsBetween(g, start, end).size() < 2)
        throw std::invalid_argument(g.node(end)->path() + " is not below " + g.node(start)->path());
    if (chainForEffector(g, end))
        throw std::invalid_argument(g.node(end)->path() + " already ends an IK chain");
    const std::string base = opts.name.empty() ? g.node(end)->name() + "_ik" : opts.name;
    json hcall = {{"startJoint", g.node(start)->path()}, {"endEffector", g.node(end)->path()}, {"name", base + "Handle"}};
    if (!opts.solver.empty())
        hcall["solver"] = opts.solver;
    if (opts.poleVector)
        hcall["poleVector"] = json::array({opts.poleVector->x, opts.poleVector->y, opts.poleVector->z});
    const std::string handle = ops::execute("ikHandle", hcall).get<std::string>();
    Node* c = g.createNode(typeNamed(kIkChainType), base);
    const NodeId id = c->id();
    parentUnder(g, id, opts.parent);
    g.setValue({id, IkChainNode::at(IkChainNode::kStartJoint)}, g.node(start)->path());
    g.setValue({id, IkChainNode::at(IkChainNode::kEndJoint)}, g.node(end)->path());
    g.setValue({id, IkChainNode::at(IkChainNode::kHandle)}, handle);
    // The handle stays at the top: a reverse foot drives its translate with a world position.
    const NodeId hid = g.find(handle)->id();
    connect(g, {id, IkChainNode::at(IkChainNode::kIkBlend), -1}, plugAt(*g.node(hid), "ikBlend"));
    // The end effector turns with the handle, so posing the handle sets the hand's orientation.
    g.evaluate();
    const NodeId al = makeAlign(g, end, g.node(end)->name() + "_ikAlign");
    g.setValue({id, IkChainNode::at(IkChainNode::kAlign)}, g.node(al)->path());
    g.setValue({al, AlignNode::kOffset}, glm::dmat4(glm::transpose(rotationOf(worldOf(g, hid))) * rotationOf(worldOf(g, end))));
    connect(g, {hid, Transform::kWorldMatrix, -1}, {al, AlignNode::kTarget, -1});
    connect(g, {id, IkChainNode::at(IkChainNode::kIkBlend), -1}, {al, AlignNode::kWeight, -1});
    return id;
}

NodeId createReverseFoot(Graph& g, NodeId chain, const FootOptions& opts)
{
    LILA_ZONE();

    if (!g.node(chain) || !g.node(chain)->type().isA(kIkChainType))
        throw std::invalid_argument("a reverse foot goes on an IK chain");
    if (footForChain(g, chain))
        throw std::invalid_argument(g.node(chain)->path() + " already has a reverse foot");
    const std::vector<NodeId> joints = chainJoints(g, chain);
    if (joints.size() < 2)
        throw std::invalid_argument(g.node(chain)->path() + " has lost its joints");
    const NodeId ankleId = joints.back();
    const NodeId ballId = opts.ball ? opts.ball : firstChildJoint(g, ankleId);
    if (!ballId)
        throw std::invalid_argument(g.node(ankleId)->path() + " has no ball joint below it");
    const NodeId toeId = opts.toe ? opts.toe : firstChildJoint(g, ballId);
    const NodeId handleId = handleOf(g, chain);
    if (!handleId)
        throw std::invalid_argument(g.node(chain)->path() + " has no ikHandle");
    const std::string ankleName = g.node(ankleId)->name();

    g.evaluate();
    const glm::dmat4 ankleW = worldOf(g, ankleId), ballW = worldOf(g, ballId);
    const glm::dvec3 toeP = toeId ? glm::dvec3(worldOf(g, toeId)[3]) : glm::dvec3(ballW[3]) + (glm::dvec3(ballW[3]) - glm::dvec3(ankleW[3])) * 0.5;
    std::vector<NodeId> footJoints{ankleId, ballId};
    if (toeId)
        footJoints.push_back(toeId);
    FootSetup setup = footSetup(glm::dvec3(ankleW[3]), glm::dvec3(ballW[3]), toeP, opts.pivots ? std::vector<glm::dvec3>{} : solePoints(g, footJoints));
    if (opts.pivots)
    {
        // Given pivots: the control sits on their sole, level, heading from heel to toe.
        const auto& p = *opts.pivots;
        const double sole = std::min({p[0].y, p[1].y, p[2].y, p[3].y, p[4].y});
        const glm::dmat3 r(setup.control);
        glm::dvec3 origin(setup.control[3]);
        origin.y = sole;
        setup.control[3] = glm::dvec4(origin, 1.0);
        glm::dvec3* out[5] = {&setup.pivots.heel, &setup.pivots.ball, &setup.pivots.toe, &setup.pivots.inner, &setup.pivots.outer};
        for (int i = 0; i < 5; ++i)
        {
            *out[i] = glm::transpose(r) * (p[size_t(i)] - origin);
            out[i]->y = 0.0;
        }
    }

    Node* f = g.createNode(typeNamed(kReverseFootType), opts.name.empty() ? ankleName + "_reverseFoot" : opts.name);
    const NodeId foot = f->id();
    parentUnder(g, foot, opts.parent);
    setWorldMatrix(g, foot, setup.control);
    auto set = [&](int own, PlugValue v) { g.setValue({foot, ReverseFootNode::at(own)}, std::move(v)); };
    set(ReverseFootNode::kAnkle, g.node(ankleId)->path());
    set(ReverseFootNode::kBall, g.node(ballId)->path());
    set(ReverseFootNode::kToe, toeId ? g.node(toeId)->path() : std::string());
    set(ReverseFootNode::kChain, g.node(chain)->path());
    set(ReverseFootNode::kHeelPivot, setup.pivots.heel);
    set(ReverseFootNode::kBallPivot, setup.pivots.ball);
    set(ReverseFootNode::kToePivot, setup.pivots.toe);
    set(ReverseFootNode::kInnerPivot, setup.pivots.inner);
    set(ReverseFootNode::kOuterPivot, setup.pivots.outer);
    g.evaluate();
    const glm::dmat4 control = worldOf(g, foot);
    set(ReverseFootNode::kRestAnkle, glm::inverse(control) * ankleW);
    set(ReverseFootNode::kRestBall, glm::inverse(control) * ballW);
    set(ReverseFootNode::kBallOrient, vecOf(g, ballId, Joint::kJointOrient));
    set(ReverseFootNode::kBallAxis, vecOf(g, ballId, Transform::kRotateAxis));
    set(ReverseFootNode::kBallRotateOrder, std::get<int64_t>(g.value({ballId, Transform::kRotateOrder, -1})));
    set(ReverseFootNode::kPoleAngle, numOf(g, plugAt(*g.node(handleId), "poleAngle")));

    // The foot's align turns the ankle in place of the chain's own.
    if (Node* old = nodeAt(g, *g.node(chain), "align"))
        g.deleteNode(old->id());
    const NodeId al = makeAlign(g, ankleId, ankleName + "_footAlign");
    set(ReverseFootNode::kAlign, g.node(al)->path());
    g.setValue({chain, IkChainNode::at(IkChainNode::kAlign)}, g.node(al)->path());

    const PlugRef ikBlend{chain, IkChainNode::at(IkChainNode::kIkBlend), -1};
    connect(g, ikBlend, {foot, ReverseFootNode::at(ReverseFootNode::kIkWeight), -1});
    connect(g, {foot, ReverseFootNode::at(ReverseFootNode::kAnkleTarget), -1}, {handleId, Transform::kTranslate, -1});
    connect(g, {foot, ReverseFootNode::at(ReverseFootNode::kPoleAngle), -1}, plugAt(*g.node(handleId), "poleAngle"));
    connect(g, {foot, ReverseFootNode::at(ReverseFootNode::kAnkleMatrix), -1}, {al, AlignNode::kTarget, -1});
    connect(g, ikBlend, {al, AlignNode::kWeight, -1});
    connect(g, {foot, ReverseFootNode::at(ReverseFootNode::kBallRotate), -1}, {ballId, Joint::kSolvedRotate, -1});
    connect(g, {foot, ReverseFootNode::at(ReverseFootNode::kBallBlend), -1}, {ballId, Joint::kIkBlend, -1});
    return foot;
}

NodeId createLooker(Graph& g, NodeId j, const LookerOptions& opts)
{
    const NodeId parent = g.parent(j);
    if (!parent)
        throw std::invalid_argument(g.node(j)->path() + " needs a parent to look from");
    const glm::dvec3 aimLocal = rig::axisVector(opts.aimAxis);
    const int64_t aimAxis = axisIndex(opts.aimAxis), upAxis = axisIndex(opts.upAxis);
    g.evaluate();
    const glm::dmat4 w = worldOf(g, j);
    const glm::dvec3 at = glm::dvec3(w[3]) + rotationOf(w) * aimLocal * opts.distance;

    Node* l = g.createNode(typeNamed(kLookerType), opts.name.empty() ? g.node(j)->name() + "_looker" : opts.name);
    const NodeId id = l->id();
    parentUnder(g, id, opts.parent);
    setWorldMatrix(g, id, glm::translate(glm::dmat4(1.0), at));
    auto set = [&](int own, PlugValue v) { g.setValue({id, LookerNode::at(own)}, std::move(v)); };
    set(LookerNode::kJoint, g.node(j)->path());
    set(LookerNode::kAimAxis, aimAxis);
    set(LookerNode::kUpAxis, upAxis);
    set(LookerNode::kUpVector, opts.upVector);
    set(LookerNode::kWeight, opts.weight);
    set(LookerNode::kRestTranslate, vecOf(g, j, Transform::kTranslate));
    set(LookerNode::kJointOrient, vecOf(g, j, Joint::kJointOrient));
    set(LookerNode::kRotateAxis, vecOf(g, j, Transform::kRotateAxis));
    set(LookerNode::kRotateOrder, std::get<int64_t>(g.value({j, Transform::kRotateOrder, -1})));
    connect(g, {parent, Transform::kWorldMatrix, -1}, {id, LookerNode::at(LookerNode::kJointParentMatrix), -1});
    connect(g, {id, LookerNode::at(LookerNode::kSolvedRotate), -1}, {j, Joint::kSolvedRotate, -1});
    connect(g, {id, LookerNode::at(LookerNode::kIkBlend), -1}, {j, Joint::kIkBlend, -1});
    return id;
}

NodeId createTwistDriver(Graph& g, NodeId t, const TwistOptions& opts)
{
    if (!rig::isJoint(*g.node(t)))
        throw std::invalid_argument(g.node(t)->path() + " is not a joint");
    const NodeId bone = g.parent(t);
    if (!bone || !rig::isJoint(*g.node(bone)))
        throw std::invalid_argument(g.node(t)->path() + " needs a parent joint to twist along");
    if (g.source({t, Joint::kSolvedRotate, -1}))
        throw std::invalid_argument(g.node(t)->path() + " is already driven");
    const NodeId source = opts.source ? opts.source : bone;
    const bool counter = source == bone;
    if (!counter && g.parent(source) != bone)
        throw std::invalid_argument(g.node(source)->path() + " is neither " + g.node(t)->path() + "'s parent nor a child of it");
    // The bone's axis in its own frame: toward the next joint along the limb.
    const NodeId along = counter ? mainChildJoint(g, bone, t) : source;
    if (!along)
        throw std::invalid_argument(g.node(bone)->path() + " has no joint to twist toward");
    g.evaluate();
    const glm::dvec3 reach = vecOf(g, along, Transform::kTranslate);
    if (glm::length(reach) < 1e-9)
        throw std::invalid_argument(g.node(along)->path() + " sits on " + g.node(bone)->path());
    const glm::dvec3 boneAxis = glm::normalize(reach);
    const glm::dmat3 restSource = localRotationOf(g, source);
    const glm::dmat3 restLocal = localRotationOf(g, t);
    const glm::dvec3 sourceAxis = counter ? boneAxis : glm::transpose(restSource) * boneAxis;
    const glm::dvec3 drivenAxis = glm::transpose(restLocal) * boneAxis;
    // How far along the bone the twist joint sits sets its default share.
    const double frac = std::clamp(glm::dot(vecOf(g, t, Transform::kTranslate), boneAxis) / glm::length(reach), 0.0, 1.0);
    const double weight = opts.weight ? *opts.weight : counter ? frac - 1.0 : frac;

    Node* n = g.createNode(typeNamed(kTwistType), opts.name.empty() ? g.node(t)->name() + "_twist" : opts.name);
    const NodeId id = n->id();
    g.setValue({id, TwistNode::kJoint}, g.node(t)->path());
    g.setValue({id, TwistNode::kSource}, g.node(source)->path());
    g.setValue({id, TwistNode::kRestSource}, glm::dmat4(restSource));
    g.setValue({id, TwistNode::kSourceAxis}, sourceAxis);
    g.setValue({id, TwistNode::kRestLocal}, glm::dmat4(restLocal));
    g.setValue({id, TwistNode::kDrivenAxis}, drivenAxis);
    g.setValue({id, TwistNode::kWeight}, weight);
    g.setValue({id, TwistNode::kJointOrient}, vecOf(g, t, Joint::kJointOrient));
    g.setValue({id, TwistNode::kRotateAxis}, vecOf(g, t, Transform::kRotateAxis));
    g.setValue({id, TwistNode::kRotateOrder}, std::get<int64_t>(g.value({t, Transform::kRotateOrder, -1})));
    connect(g, {source, Transform::kMatrix, -1}, {id, TwistNode::kSourceMatrix, -1});
    connect(g, {id, TwistNode::kSolvedRotate, -1}, {t, Joint::kSolvedRotate, -1});
    connect(g, {id, TwistNode::kIkBlend, -1}, {t, Joint::kIkBlend, -1});
    return id;
}

// Clean bake ------------------------------------------------------------------------------------------------------

std::vector<NodeId> bakeToDeformSkeleton(Graph& g, const std::vector<NodeId>& roots, const BakeOptions& opts)
{
    LILA_ZONE();

    // Each skeleton once, from its top joint, parents before children.
    std::vector<NodeId> order;
    std::set<NodeId> seen;
    for (NodeId r : roots)
    {
        NodeId top = r;
        while (g.parent(top) && rig::isJoint(*g.node(g.parent(top))))
            top = g.parent(top);
        std::vector<NodeId> stack{top};
        while (!stack.empty())
        {
            const NodeId id = stack.back();
            stack.pop_back();
            if (!rig::isJoint(*g.node(id)) || !seen.insert(id).second)
                continue;
            order.push_back(id);
            const std::vector<NodeId>& kids = g.children(id);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                stack.push_back(*it);
        }
    }
    std::map<NodeId, NodeId> copyOf;
    std::map<NodeId, NodeId> deformParent; // original -> its nearest deform ancestor (original), or none
    std::vector<NodeId> bakes, tops;
    const NodeType& jointType = typeNamed("joint");
    for (NodeId id : order)
    {
        Node& n = *g.node(id);
        if (!isDeform(n))
            continue;
        NodeId up = g.parent(id);
        while (up && !copyOf.contains(up))
            up = g.parent(up);
        Node* c = g.createNode(jointType, n.name() + opts.suffix, up ? copyOf[up] : kNoNode, false);
        const NodeId cid = c->id();
        g.setValue({cid, Transform::kRotateOrder}, n.as<int64_t>(Transform::kRotateOrder));
        g.setValue({cid, Transform::kRotationInterpolation}, int64_t(RotationInterpolation::Euler));
        g.setValue({cid, Joint::kRadius}, n.as<double>(Joint::kRadius));
        g.setValue({cid, Joint::kSide}, n.as<int64_t>(Joint::kSide));
        copyOf[id] = cid;
        deformParent[id] = up;
        bakes.push_back(id);
        if (!up)
            tops.push_back(cid);
    }
    if (bakes.empty())
        return tops;

    const double fps = opts.fps > 0.0 ? opts.fps : 30.0;
    const long long frames = std::max<long long>(0, std::llround((opts.end - opts.start) * fps));
    std::vector<std::array<anim::Curve, 9>> curves(bakes.size());
    std::vector<glm::dvec3> lastRotate(bakes.size(), glm::dvec3(0.0));
    const double was = g.time();
    for (long long f = 0; f <= frames; ++f)
    {
        const double t = opts.start + double(f) / fps;
        g.setTime(t);
        g.evaluate();
        for (size_t i = 0; i < bakes.size(); ++i)
        {
            const NodeId src = bakes[i];
            const NodeId up = deformParent[src];
            const glm::dmat4 local = (up ? glm::inverse(worldOf(g, up)) : glm::dmat4(1.0)) * worldOf(g, src);
            glm::dvec3 scale(glm::length(glm::dvec3(local[0])), glm::length(glm::dvec3(local[1])), glm::length(glm::dvec3(local[2])));
            const glm::dmat3 rot = rotationOf(local);
            if (glm::determinant(rot) < 0.0)
                scale.x = -scale.x;
            const glm::dvec3 r = eulerNear(glm::determinant(rot) < 0.0 ? glm::dmat3(-rot[0], rot[1], rot[2]) : rot,
                                           RotateOrder(g.node(src)->as<int64_t>(Transform::kRotateOrder)), lastRotate[i]);
            lastRotate[i] = r;
            const glm::dvec3 tr(local[3]);
            for (int k = 0; k < 3; ++k)
            {
                curves[i][size_t(k)].keys.push_back({.time = t, .value = tr[k], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
                curves[i][size_t(3 + k)].keys.push_back({.time = t, .value = r[k], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
                curves[i][size_t(6 + k)].keys.push_back({.time = t, .value = scale[k], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
            }
        }
    }
    g.setTime(was);
    g.evaluate();
    // The copy's keys are plain curves, never an active animation layer's.
    std::vector<NodeId> active;
    for (NodeId l : anim::layers(g))
        if (const int a = g.node(l)->type().attrIndex("active"); a >= 0 && g.node(l)->as<bool>(a))
        {
            active.push_back(l);
            g.setValue({l, a, -1}, false);
        }
    const int attrs[3] = {Transform::kTranslate, Transform::kRotate, Transform::kScale};
    for (size_t i = 0; i < bakes.size(); ++i)
        for (int a = 0; a < 3; ++a)
            for (int k = 0; k < 3; ++k)
            {
                const anim::Curve& c = curves[i][size_t(a * 3 + k)];
                const PlugRef plug{copyOf[bakes[i]], attrs[a], k};
                const NodeId curve = anim::setKey(g, plug, c.keys.front().time, c.keys.front().value);
                anim::setCurve(g, curve, c);
            }
    for (NodeId l : active)
        g.setValue({l, g.node(l)->type().attrIndex("active"), -1}, true);
    return tops;
}

void registerTypes()
{
    if (types::find(kIkChainType))
        return;
    registerTypesImpl();
}
} // namespace lila::scene::rigel
