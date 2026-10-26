#include "scene/Repeat.h"

#include "geo/ArrayTransform.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace lila::scene::repeat
{
glm::dmat4 stepMatrix(const Step& s, const glm::dvec3& pivot, const glm::dmat3& frame)
{
    return geo::aboutFrame(geo::pivotFrame(pivot, frame), geo::trsMatrix(s.translate, s.rotate, s.scale));
}

std::vector<glm::dmat4> copyMatrices(const glm::dmat4& base, const glm::dmat4& step, int count) { return geo::repeatMatrices(base, step, count); }

glm::dmat4 delta(const glm::dmat4& from, const glm::dmat4& to) { return to * glm::inverse(from); }

void setWorldMatrix(Graph& g, NodeId id, const glm::dmat4& world, std::optional<glm::dvec3> rotateHint)
{
    Node* n = g.node(id);
    if (!n || !n->type().isA("transform"))
        return;
    const NodeId parent = g.parent(id);
    const glm::dmat4 parentWorld = parent == kNoNode ? glm::dmat4(1.0) : std::get<glm::dmat4>(g.value({parent, Transform::kWorldMatrix, -1}));
    const glm::dmat4 m = glm::inverse(parentWorld) * world;

    // Placing a copy is not a channel edit, so locked channels (copied from its source) take the new value too.
    std::vector<PlugRef> relock;
    for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale})
        for (int c = 0; c < 3; ++c)
            if (g.isLocked({id, attr, c}))
            {
                relock.push_back({id, attr, c});
                g.setLocked(relock.back(), false);
            }

    glm::dvec3 scale, translation, skew;
    glm::dquat orientation;
    glm::dvec4 perspective;
    glm::decompose(m, scale, orientation, translation, skew, perspective);
    const auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
    const auto* t = dynamic_cast<const Transform*>(n);
    const glm::dmat3 pre = t ? glm::dmat3(t->preRotation()) : glm::dmat3(1.0);
    const glm::dmat3 axis = t ? glm::dmat3(t->rotateAxisMatrix()) : glm::dmat3(1.0);
    // Nearest the expected channels, so stepping past 180 keeps counting instead of flipping.
    const glm::dvec3 hint = rotateHint.value_or(n->as<glm::dvec3>(Transform::kRotate));
    const glm::dvec3 euler = xformops::closestEuler(glm::transpose(pre) * glm::mat3_cast(orientation) * glm::transpose(axis), order, hint);
    g.setValue({id, Transform::kRotate, -1}, euler);
    g.setValue({id, Transform::kScale, -1}, scale);

    const glm::dmat4 withoutT = Transform::localMatrix(*n);
    const glm::dvec3 current = n->as<glm::dvec3>(Transform::kTranslate);
    const glm::dvec3 offset = glm::dvec3(withoutT[3]) - current;
    g.setValue({id, Transform::kTranslate, -1}, glm::dvec3(m[3]) - offset);
    for (const PlugRef& p : relock)
        g.setLocked(p, true);
}
} // namespace lila::scene::repeat
