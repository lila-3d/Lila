#include "seq/RigidPieces.h"

#include "core/Profile.h"
#include "fbx/Fbx.h"
#include "math/MathCore.h"
#include "seq/Vat.h"

#include <algorithm>
#include <cmath>

namespace lila::seq
{
using geo::Index;

namespace
{
/** Eigenvalues and unit eigenvectors (columns of vectors) of a symmetric 4x4, by cyclic Jacobi rotations. */
void symmetricEigen4(double a[4][4], double values[4], double vectors[4][4])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            vectors[i][j] = i == j ? 1.0 : 0.0;
    double scale = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            scale += a[i][j] * a[i][j];
    for (int sweep = 0; sweep < 64; ++sweep)
    {
        double off = 0.0;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q)
                off += a[p][q] * a[p][q];
        if (off <= 1e-36 * scale)
            break;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q)
            {
                if (a[p][q] == 0.0)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 4; ++k)
                {
                    const double kp = a[k][p], kq = a[k][q];
                    a[k][p] = c * kp - s * kq;
                    a[k][q] = s * kp + c * kq;
                }
                for (int k = 0; k < 4; ++k)
                {
                    const double pk = a[p][k], qk = a[q][k];
                    a[p][k] = c * pk - s * qk;
                    a[q][k] = s * pk + c * qk;
                }
                for (int k = 0; k < 4; ++k)
                {
                    const double kp = vectors[k][p], kq = vectors[k][q];
                    vectors[k][p] = c * kp - s * kq;
                    vectors[k][q] = s * kp + c * kq;
                }
            }
    }
    for (int i = 0; i < 4; ++i)
        values[i] = a[i][i];
}

/**
 * The rotation (xyzw) taking the centred rest points onto the centred frame points, from their cross covariance
 * s[i][j] = sum rest[i] * frame[j] (Horn 1987: the top eigenvector of a symmetric 4x4). When the top eigenvalue is
 * shared (a piece of one point, or of collinear points) any rotation in that eigenspace fits equally, so the one
 * nearest `prev` is taken.
 */
glm::dvec4 fitRotation(const double s[3][3], const glm::dvec4& prev)
{
    const double xx = s[0][0], xy = s[0][1], xz = s[0][2], yx = s[1][0], yy = s[1][1], yz = s[1][2], zx = s[2][0], zy = s[2][1], zz = s[2][2];
    double n[4][4] = {{xx + yy + zz, yz - zy, zx - xz, xy - yx},
                      {yz - zy, xx - yy - zz, xy + yx, zx + xz},
                      {zx - xz, xy + yx, -xx + yy - zz, yz + zy},
                      {xy - yx, zx + xz, yz + zy, -xx - yy + zz}};
    double values[4], vectors[4][4];
    symmetricEigen4(n, values, vectors);
    int top = 0;
    double magnitude = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        if (values[k] > values[top])
            top = k;
        magnitude = std::max(magnitude, std::abs(values[k]));
    }
    auto column = [&](int k) { return glm::dvec4(vectors[1][k], vectors[2][k], vectors[3][k], vectors[0][k]); };
    glm::dvec4 q(0.0);
    for (int k = 0; k < 4; ++k)
        if (values[k] >= values[top] - 1e-9 * magnitude)
            q += glm::dot(prev, column(k)) * column(k);
    const double len = glm::length(q);
    return len > 1e-6 ? q / len : column(top);
}

} // namespace

RigidPieces findRigidPieces(const std::vector<geo::Mesh>& meshes)
{
    LILA_ZONE();

    RigidPieces out;
    for (const geo::Mesh& m : meshes)
    {
        const Index n = m.numPoints();
        std::vector<Index> local(n, 0);
        Index count = 0;
        std::span<const int32_t> attr = m.pointAttrs().get<int32_t>(kVatPieceAttr);
        if (attr.size() == m.pointCapacity())
        {
            std::vector<int32_t> ids(attr.begin(), attr.begin() + n);
            std::ranges::sort(ids);
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            for (Index p = 0; p < n; ++p)
                local[p] = Index(std::ranges::lower_bound(ids, attr[p]) - ids.begin());
            count = Index(ids.size());
        }
        else
        {
            out.fromAttribute = false;
            std::vector<Index> parent(n);
            for (Index p = 0; p < n; ++p)
                parent[p] = p;
            auto root = [&](Index x) {
                while (parent[x] != x)
                    x = parent[x] = parent[parent[x]];
                return x;
            };
            for (Index e = 0; e < m.edgeCapacity(); ++e)
                if (m.edgeValid(e))
                {
                    const Index a = root(m.edgePoint(e, 0)), b = root(m.edgePoint(e, 1));
                    if (a != b)
                        parent[std::max(a, b)] = std::min(a, b);
                }
            std::vector<Index> label(n, geo::kInvalid);
            for (Index p = 0; p < n; ++p)
            {
                const Index r = root(p);
                if (label[r] == geo::kInvalid)
                    label[r] = count++;
                local[p] = label[r];
            }
        }
        for (Index p = 0; p < n; ++p)
            out.of.push_back(out.count + local[p]);
        out.count += count;
    }
    return out;
}

glm::dvec3 rotateByQuat(const glm::dvec4& q, const glm::dvec3& v)
{
    const glm::dvec3 u(q.x, q.y, q.z);
    const glm::dvec3 t = 2.0 * glm::cross(u, v);
    return v + q.w * t + glm::cross(u, t);
}

RigidFit fitRigidPieces(std::span<const std::span<const glm::dvec3>> frames, std::span<const double> times, std::span<const glm::dvec3> rest,
                        const RigidPieces& pieces, const glm::dmat3& basis)
{
    LILA_ZONE();

    const size_t count = pieces.count;
    std::vector<size_t> start(count + 1, 0);
    for (Index k : pieces.of)
        ++start[k + 1];
    for (size_t k = 0; k < count; ++k)
        start[k + 1] += start[k];
    std::vector<Index> members(pieces.of.size());
    {
        std::vector<size_t> at(start.begin(), start.end() - 1);
        for (Index p = 0; p < Index(pieces.of.size()); ++p)
            members[at[pieces.of[p]]++] = p;
    }

    RigidFit t;
    t.pivots.resize(count);
    t.centres.resize(frames.size() * count);
    t.rotations.resize(frames.size() * count);
    std::vector<glm::dvec3> arm, moved;
    for (size_t k = 0; k < count; ++k)
    {
        const std::span<const Index> idx(members.data() + start[k], start[k + 1] - start[k]);
        glm::dvec3 pivot(0.0);
        arm.clear();
        for (Index p : idx)
            arm.push_back(basis * rest[p]);
        for (const glm::dvec3& a : arm)
            pivot += a;
        pivot /= double(idx.size());
        for (glm::dvec3& a : arm)
            a -= pivot;
        t.pivots[k] = pivot;

        glm::dvec4 prev(0.0, 0.0, 0.0, 1.0);
        for (size_t f = 0; f < frames.size(); ++f)
        {
            moved.clear();
            glm::dvec3 centre(0.0);
            for (Index p : idx)
            {
                moved.push_back(basis * frames[f][p]);
                centre += moved.back();
            }
            centre /= double(idx.size());
            double s[3][3] = {};
            for (size_t i = 0; i < idx.size(); ++i)
            {
                const glm::dvec3 b = moved[i] - centre;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        s[r][c] += arm[i][r] * b[c];
            }
            glm::dvec4 q = fitRotation(s, prev);
            if (glm::dot(q, prev) < 0.0)
                q = -q;
            prev = q;
            t.centres[f * count + k] = centre;
            t.rotations[f * count + k] = q;
            for (size_t i = 0; i < idx.size(); ++i)
            {
                const double err = glm::length(rotateByQuat(q, arm[i]) + centre - moved[i]);
                if (err > t.residual)
                {
                    t.residual = err;
                    t.residualPiece = Index(k);
                    t.residualFrame = f < times.size() ? times[f] : double(f);
                }
            }
        }
    }
    return t;
}
glm::dmat3 unrealBasis(const std::string& profile)
{
    fbx::AxisProfile p = fbx::AxisProfile::Ue5;
    fbx::parseProfile(profile, &p);
    const math::UpAxis up = math::upAxis();
    const glm::dmat3 file = fbx::profileBasis(p, up);
    const bool zUp = p == fbx::AxisProfile::Ue5 || (p == fbx::AxisProfile::Generic && up == math::UpAxis::Z);
    // Unreal reads Z-up files by negating Y and Y-up (ue6) files by negating X.
    const glm::dmat3 mirror = zUp ? glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, -1, 0), glm::dvec3(0, 0, 1))
                                  : glm::dmat3(glm::dvec3(-1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, 1));
    return mirror * file;
}
} // namespace lila::seq
