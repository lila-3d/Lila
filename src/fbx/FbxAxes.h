#pragma once

#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <cmath>

/** Euler and axis-swap helpers shared by the FBX reader and writer. */
namespace lila::fbx
{
/** Euler axis sequences, first-applied axis first, indexed by scene::RotateOrder. */
constexpr int kOrderSeq[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};

inline scene::RotateOrder orderOfSeq(int a, int b, int c)
{
    for (int o = 0; o < 6; ++o)
        if (kOrderSeq[o][0] == a && kOrderSeq[o][1] == b && kOrderSeq[o][2] == c)
            return scene::RotateOrder(o);
    return scene::RotateOrder::XYZ;
}

/** FBX's RotationOrder enum counts XYZ, XZY, YZX, YXZ, ZXY, ZYX. */
inline int fbxOrder(scene::RotateOrder o)
{
    constexpr int kFbx[6] = {0, 2, 4, 1, 3, 5};
    return kFbx[int(o)];
}

/** Lila order for FBX's (and ufbx's) RotationOrder enum; spheric reads as XYZ. */
inline scene::RotateOrder fromFbxOrder(int fbx)
{
    for (int o = 0; o < 6; ++o)
        if (fbxOrder(scene::RotateOrder(o)) == fbx)
            return scene::RotateOrder(o);
    return scene::RotateOrder::XYZ;
}

inline glm::dvec3 unwrapNear(glm::dvec3 e, const glm::dvec3& ref)
{
    for (int i = 0; i < 3; ++i)
        e[i] += 360.0 * std::round((ref[i] - e[i]) / 360.0);
    return e;
}

/** Euler angles of r in `order` closest to ref, so baked frames neither flip nor wrap. */
using scene::eulerNear;

/** Where a signed axis permutation sends each Lila axis: file axis to[i], with sign[i]. */
struct AxisMap
{
    int to[3] = {0, 1, 2};
    double sign[3] = {1, 1, 1};
    double det = 1.0;

    glm::dvec3 translate(const glm::dvec3& v) const
    {
        glm::dvec3 out(0.0);
        for (int i = 0; i < 3; ++i)
            out[to[i]] = sign[i] * v[i];
        return out;
    }
    // Conjugating a rotation about e_i by the basis turns it about e_to[i], negated by a mirror.
    glm::dvec3 rotate(const glm::dvec3& v) const
    {
        glm::dvec3 out(0.0);
        for (int i = 0; i < 3; ++i)
            out[to[i]] = sign[i] * det * v[i];
        return out;
    }
    glm::dvec3 scale(const glm::dvec3& v) const
    {
        glm::dvec3 out(1.0);
        for (int i = 0; i < 3; ++i)
            out[to[i]] = v[i];
        return out;
    }
    scene::RotateOrder order(scene::RotateOrder o) const
    {
        const int* s = kOrderSeq[int(o)];
        return orderOfSeq(to[s[0]], to[s[1]], to[s[2]]);
    }
    /** Factor taking Lila channel `axis` of group g (0 translate, 1 rotate, 2 scale) to its file channel. */
    double factor(int g, int axis) const { return g == 0 ? sign[axis] : g == 1 ? sign[axis] * det : 1.0; }
};

inline AxisMap axisMap(const glm::dmat3& basis)
{
    AxisMap m;
    for (int i = 0; i < 3; ++i)
    {
        const glm::dvec3& col = basis[i];
        int j = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(col[k]) > std::abs(col[j]))
                j = k;
        m.to[i] = j;
        m.sign[i] = col[j] < 0.0 ? -1.0 : 1.0;
    }
    m.det = glm::determinant(basis) < 0.0 ? -1.0 : 1.0;
    return m;
}
} // namespace lila::fbx
