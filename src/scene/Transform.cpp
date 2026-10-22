#include "scene/Transform.h"

#include "core/PrefStore.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace lila::scene
{
namespace
{
const char* kOrderNames[] = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"};

// Axis indices in application order.
constexpr int kOrderAxes[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};

glm::dmat4 axisRotation(int axis, double radians)
{
    glm::dvec3 a(0.0);
    a[axis] = 1.0;
    return glm::rotate(glm::dmat4(1.0), radians, a);
}
} // namespace

bool inheritVisibility() { return PrefStore::get().value<bool>("general", kInheritVisibilityPref, true); }

bool effectivelyVisible(const Graph& g, NodeId id, bool inherit)
{
    for (NodeId n = id; n != kNoNode; n = inherit ? g.parent(n) : kNoNode)
    {
        const Node* node = g.node(n);
        if (node && node->type().dag && (!node->as<bool>(Transform::kVisibility) || displayLayers::hiddenByOwnLayer(g, n)))
            return false;
    }
    return true;
}

glm::dmat4 eulerToMatrix(const glm::dvec3& degrees, RotateOrder order)
{
    const int* axes = kOrderAxes[int(order)];
    glm::dvec3 r = glm::radians(degrees);
    // First-applied rotation sits rightmost.
    return axisRotation(axes[2], r[axes[2]]) * axisRotation(axes[1], r[axes[1]]) * axisRotation(axes[0], r[axes[0]]);
}

glm::dvec3 matrixToEuler(const glm::dmat3& rot, RotateOrder order)
{
    glm::dmat4 m(rot);
    double a = 0, b = 0, c = 0;
    glm::dvec3 r;
    // glm's extractEulerAngleABC returns angles for Ra*Rb*Rc, so Maya order xyz (Rz*Ry*Rx) maps to ZYX.
    switch (order)
    {
    case RotateOrder::XYZ: glm::extractEulerAngleZYX(m, a, b, c); r = {c, b, a}; break;
    case RotateOrder::YZX: glm::extractEulerAngleXZY(m, a, b, c); r = {a, c, b}; break;
    case RotateOrder::ZXY: glm::extractEulerAngleYXZ(m, a, b, c); r = {b, a, c}; break;
    case RotateOrder::XZY: glm::extractEulerAngleYZX(m, a, b, c); r = {c, a, b}; break;
    case RotateOrder::YXZ: glm::extractEulerAngleZXY(m, a, b, c); r = {b, c, a}; break;
    case RotateOrder::ZYX: glm::extractEulerAngleXYZ(m, a, b, c); r = {a, b, c}; break;
    }
    return glm::degrees(r);
}

glm::dmat4 Transform::localMatrix(const Node& n)
{
    auto v3 = [&](int a) { return n.as<glm::dvec3>(a); };
    auto tr = [](const glm::dvec3& v) { return glm::translate(glm::dmat4(1.0), v); };
    auto order = RotateOrder(n.as<int64_t>(kRotateOrder));
    glm::dvec3 rp = v3(kRotatePivot);
    glm::dvec3 sp = v3(kScalePivot);
    const auto* t = dynamic_cast<const Transform*>(&n);
    glm::dmat4 pre = t ? t->preRotation() : glm::dmat4(1.0);

    return tr(v3(kTranslate)) * tr(v3(kRotatePivotTranslate)) * tr(rp) * pre * eulerToMatrix(v3(kRotate), order) * tr(-rp) *
           tr(v3(kScalePivotTranslate)) * tr(sp) * glm::scale(glm::dmat4(1.0), v3(kScale)) * tr(-sp);
}

void Transform::compute(const EvalContext&)
{
    glm::dmat4 local = localMatrix(*this);
    setOutput(kMatrix, local);
    setOutput(kWorldMatrix, as<glm::dmat4>(kParentMatrix) * local);
}

void Transform::setLocalMatrix(Graph& g, NodeId id, const glm::dmat4& m)
{
    Node* n = g.node(id);
    glm::dvec3 scale, translation, skew;
    glm::dquat orientation;
    glm::dvec4 perspective;
    glm::decompose(m, scale, orientation, translation, skew, perspective);
    auto order = RotateOrder(n->as<int64_t>(kRotateOrder));
    const auto* t = dynamic_cast<const Transform*>(n);
    glm::dmat3 pre = t ? glm::dmat3(t->preRotation()) : glm::dmat3(1.0);
    glm::dvec3 euler = matrixToEuler(glm::transpose(pre) * glm::mat3_cast(orientation), order);

    g.setValue({id, kRotate}, euler);
    g.setValue({id, kScale}, scale);

    // With pivots, translate is whatever makes the rest of the chain land on m's translation.
    glm::dmat4 withoutT = localMatrix(*n);
    glm::dvec3 current = n->as<glm::dvec3>(kTranslate);
    glm::dvec3 offset = glm::dvec3(withoutT[3]) - current;
    g.setValue({id, kTranslate}, glm::dvec3(m[3]) - offset);
}

NodeType Transform::makeType()
{
    auto vec3 = [](std::string name, std::string shortName, glm::dvec3 def, std::string doc) {
        AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def,
                  .flags = AttrInput | AttrStorable | AttrKeyable, .doc = std::move(doc)};
        for (int i = 0; i < 3; ++i)
        {
            a.childNames[i] = name + char('X' + i);
            a.childShort[i] = shortName + char('x' + i);
        }
        return a;
    };

    auto unkeyable = [](AttrDef a) {
        a.flags &= ~AttrKeyable;
        return a;
    };

    NodeType t;
    t.name = "transform";
    t.icon = "box-multiple";
    t.dag = true;
    t.ownAttrs = {
        vec3("translate", "t", glm::dvec3(0.0), "Position relative to the parent, cm."),
        vec3("rotate", "r", glm::dvec3(0.0), "Euler rotation in degrees."),
        vec3("scale", "s", glm::dvec3(1.0), "Scale."),
        {.name = "rotateOrder", .shortName = "ro", .type = PlugType::Enum, .defaultValue = int64_t(0),
         .enumNames = {kOrderNames, kOrderNames + 6}, .doc = "Euler order, first-applied axis first."},
        unkeyable(vec3("rotatePivot", "rp", glm::dvec3(0.0), "Rotation pivot in local space.")),
        unkeyable(vec3("scalePivot", "sp", glm::dvec3(0.0), "Scale pivot in local space.")),
        unkeyable(vec3("rotatePivotTranslate", "rpt", glm::dvec3(0.0), "Compensates rotatePivot moves so the object stays put.")),
        unkeyable(vec3("scalePivotTranslate", "spt", glm::dvec3(0.0), "Compensates scalePivot moves so the object stays put.")),
        {.name = "visibility", .shortName = "v", .type = PlugType::Bool, .defaultValue = true,
         .flags = AttrInput | AttrStorable | AttrKeyable},
        {.name = "parentMatrix", .shortName = "pm", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
         .flags = AttrHidden, .doc = "World matrix of the parent."},
        {.name = "matrix", .shortName = "m", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
         .flags = AttrOutput, .doc = "Local matrix."},
        {.name = "worldMatrix", .shortName = "wm", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
         .flags = AttrOutput, .doc = "Object-to-world matrix."},
        unkeyable(vec3("pivotOrient", "po", glm::dvec3(0.0),
                       "Orientation of the pivot relative to the object's axes, Euler XYZ degrees: the manipulator's axes in object "
                       "space. Leaves the object where it is.")),
        {.name = "displayLayer", .shortName = "dl", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrHidden,
         .doc = "Display layer membership: connected from a displayLayer's drawInfo."},
    };
    t.create = [] { return std::make_unique<Transform>(); };
    return t;
}

NodeType Locator::makeType(const NodeType& base)
{
    NodeType t;
    t.name = "locator";
    t.icon = "crosshair";
    t.base = &base;
    t.dag = true;
    AttrDef ls{.name = "localScale", .shortName = "lsc", .type = PlugType::Vec3, .defaultValue = glm::dvec3(1.0),
               .flags = AttrInput | AttrStorable, .doc = "Display size."};
    for (int i = 0; i < 3; ++i)
    {
        ls.childNames[i] = std::string("localScale") + char('X' + i);
        ls.childShort[i] = std::string("lsc") + char('x' + i);
    }
    t.ownAttrs = {ls};
    t.create = [] { return std::make_unique<Locator>(); };
    return t;
}
} // namespace lila::scene
