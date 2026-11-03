#include "deform/Surface.h"

#include "core/Profile.h"
#include "deform/DeformUtil.h"

#include <glm/gtc/quaternion.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>

namespace lila::deform
{
namespace
{
using geo::Index;

/** x along the direction u grows on the triangle, z along its normal. */
glm::dmat3 uvFrame(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::dvec2 uv[3])
{
    glm::dvec3 n = glm::cross(b - a, c - a);
    double ln = glm::length(n);
    if (ln < 1e-15)
        return glm::dmat3(1.0);
    n /= ln;
    glm::dvec3 e1 = b - a, e2 = c - a;
    glm::dvec2 d1 = uv[1] - uv[0], d2 = uv[2] - uv[0];
    double r = d1.x * d2.y - d2.x * d1.y;
    glm::dvec3 t = std::abs(r) > 1e-20 ? (e1 * d2.y - e2 * d1.y) / r : e1;
    t -= n * glm::dot(n, t);
    double lt = glm::length(t);
    if (lt < 1e-15)
        return triangleFrame(a, b, c);
    t /= lt;
    return {t, glm::cross(n, t), n};
}

double triangleArea(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c) { return 0.5 * glm::length(glm::cross(b - a, c - a)); }

glm::dvec3 anyPerpendicular(const glm::dvec3& v)
{
    glm::dvec3 axis = std::abs(v.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    glm::dvec3 p = glm::cross(v, axis);
    double l = glm::length(p);
    return l > 1e-15 ? p / l : glm::dvec3(0, 0, 1);
}

/** Largest-eigenvalue eigenvector of a symmetric 4x4, cyclic Jacobi. */
glm::dvec4 dominantEigenvector(double a[4][4])
{
    double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
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
        if (off <= 1e-34 * scale + 1e-300)
            break;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q)
            {
                if (a[p][q] == 0.0)
                    continue;
                double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 4; ++k)
                {
                    double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 4; ++k)
                {
                    double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 4; ++k)
                {
                    double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (a[i][i] > a[best][best])
            best = i;
    return {v[0][best], v[1][best], v[2][best], v[3][best]};
}

/** Triangles indexed by their uv bounds, for finding the one under a uv. */
class UvLocator
{
public:
    UvLocator(const geo::Mesh& mesh, std::string_view uvSet) : mMesh(mesh)
    {
        mUvs = mesh.cornerAttrs().get<glm::vec2>(uvSet);
        if (mUvs.empty())
            return;
        mTris = geo::triangulate(mesh);
        const size_t n = mTris.faces.size();
        glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (Index c : mTris.corners)
        {
            glm::dvec2 u = mUvs[c];
            lo = glm::min(lo, u);
            hi = glm::max(hi, u);
        }
        if (n == 0)
            return;
        mLo = lo;
        mRes = std::clamp(int(std::sqrt(double(n))), 1, 512);
        mSize = glm::max(hi - lo, glm::dvec2(1e-9)) / double(mRes);
        std::vector<std::vector<uint32_t>> cells(size_t(mRes) * size_t(mRes));
        for (size_t t = 0; t < n; ++t)
        {
            glm::dvec2 a = uvOf(t, 0), b = uvOf(t, 1), c = uvOf(t, 2);
            glm::ivec2 c0 = cell(glm::min(a, glm::min(b, c))), c1 = cell(glm::max(a, glm::max(b, c)));
            for (int y = c0.y; y <= c1.y; ++y)
                for (int x = c0.x; x <= c1.x; ++x)
                    cells[size_t(y) * size_t(mRes) + size_t(x)].push_back(uint32_t(t));
        }
        mStart.push_back(0);
        for (auto& c : cells)
        {
            mIds.insert(mIds.end(), c.begin(), c.end());
            mStart.push_back(uint32_t(mIds.size()));
        }
    }

    bool valid() const { return !mStart.empty(); }

    /** The first triangle (by index) whose uvs cover uv, with its barycentric weights. */
    bool find(const glm::dvec2& uv, uint32_t& tri, glm::dvec3& bary) const
    {
        if (!valid())
            return false;
        glm::ivec2 c = cell(uv);
        uint32_t best = ~0u;
        for (uint32_t k = mStart[size_t(c.y) * size_t(mRes) + size_t(c.x)]; k < mStart[size_t(c.y) * size_t(mRes) + size_t(c.x) + 1]; ++k)
        {
            uint32_t t = mIds[k];
            if (t >= best)
                continue;
            glm::dvec2 a = uvOf(t, 0), b = uvOf(t, 1), cc = uvOf(t, 2);
            double det = (b.x - a.x) * (cc.y - a.y) - (cc.x - a.x) * (b.y - a.y);
            if (std::abs(det) < 1e-20)
                continue;
            double u = ((uv.x - a.x) * (cc.y - a.y) - (cc.x - a.x) * (uv.y - a.y)) / det;
            double v = ((b.x - a.x) * (uv.y - a.y) - (uv.x - a.x) * (b.y - a.y)) / det;
            constexpr double kTol = -1e-9;
            if (u >= kTol && v >= kTol && 1.0 - u - v >= kTol)
            {
                best = t;
                bary = {1.0 - u - v, u, v};
            }
        }
        tri = best;
        return best != ~0u;
    }

    const geo::Triangulation& tris() const { return mTris; }
    glm::dvec2 uvOf(size_t t, int k) const { return glm::dvec2(mUvs[mTris.corners[t * 3 + size_t(k)]]); }
    uint32_t pointOf(size_t t, int k) const { return uint32_t(mMesh.cornerPoint(mTris.corners[t * 3 + size_t(k)])); }

private:
    glm::ivec2 cell(const glm::dvec2& uv) const
    {
        glm::dvec2 f = (uv - mLo) / mSize;
        return glm::clamp(glm::ivec2(int(std::floor(f.x)), int(std::floor(f.y))), glm::ivec2(0), glm::ivec2(mRes - 1));
    }

    const geo::Mesh& mMesh;
    std::span<const glm::vec2> mUvs;
    geo::Triangulation mTris;
    glm::dvec2 mLo{0.0}, mSize{1.0};
    int mRes = 1;
    std::vector<uint32_t> mStart, mIds;
};

std::vector<glm::dvec3> facePositions(const geo::Mesh& mesh, Index f, std::span<const glm::dvec3> points, std::vector<Index>& ids)
{
    ids = mesh.facePoints(f);
    std::vector<glm::dvec3> out(ids.size());
    for (size_t k = 0; k < ids.size(); ++k)
        out[k] = points[ids[k]];
    return out;
}
} // namespace

// ------------------------------------------------------------------------------------------------ rigid fit

RigidFit rigidFit(std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed, std::span<const uint32_t> ids)
{
    RigidFit fit;
    const size_t n = ids.empty() ? std::min(rest.size(), deformed.size()) : ids.size();
    if (n == 0)
        return fit;
    auto at = [&](size_t k) { return ids.empty() ? k : size_t(ids[k]); };
    for (size_t k = 0; k < n; ++k)
    {
        fit.from += rest[at(k)];
        fit.to += deformed[at(k)];
    }
    fit.from /= double(n);
    fit.to /= double(n);
    double s[3][3] = {};
    for (size_t k = 0; k < n; ++k)
    {
        glm::dvec3 a = rest[at(k)] - fit.from, b = deformed[at(k)] - fit.to;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                s[i][j] += a[i] * b[j];
    }
    // Horn 1987: the rotation is the unit quaternion maximising q^T N q.
    double N[4][4] = {
        {s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
        {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
        {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
        {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]},
    };
    glm::dvec4 q = dominantEigenvector(N);
    double l = glm::length(q);
    if (l < 1e-300)
        return fit;
    q /= l;
    fit.rotation = glm::mat3_cast(glm::dquat(q.x, q.y, q.z, q.w));
    return fit;
}

// ------------------------------------------------------------------------------------------------ point grid

void PointGrid::build(std::span<const glm::dvec3> points)
{
    mPoints.assign(points.begin(), points.end());
    mStart.clear();
    mIds.clear();
    if (mPoints.empty())
        return;
    geo::Bounds b;
    for (const glm::dvec3& p : mPoints)
        b.extend(p);
    glm::dvec3 ext = b.size();
    double longest = std::max({ext.x, ext.y, ext.z, 1e-9});
    mCell = std::max(longest / std::cbrt(double(mPoints.size())), longest * 1e-6);
    const double budget = 4.0 * double(mPoints.size()) + 64.0;
    for (;;)
    {
        mDims = glm::ivec3(glm::floor(ext / mCell)) + 1;
        if (double(mDims.x) * mDims.y * mDims.z <= budget)
            break;
        mCell *= 1.5;
    }
    mOrigin = b.min;
    const size_t cells = size_t(mDims.x) * size_t(mDims.y) * size_t(mDims.z);
    std::vector<uint32_t> count(cells + 1, 0);
    std::vector<uint32_t> cellOfPoint(mPoints.size());
    for (size_t i = 0; i < mPoints.size(); ++i)
    {
        glm::ivec3 c = glm::clamp(cellOf(mPoints[i]), glm::ivec3(0), mDims - 1);
        cellOfPoint[i] = uint32_t(c.x + mDims.x * (c.y + mDims.y * c.z));
        ++count[cellOfPoint[i] + 1];
    }
    for (size_t c = 0; c < cells; ++c)
        count[c + 1] += count[c];
    mStart = count;
    mIds.resize(mPoints.size());
    for (size_t i = 0; i < mPoints.size(); ++i)
        mIds[count[cellOfPoint[i]]++] = uint32_t(i);
}

glm::ivec3 PointGrid::cellOf(const glm::dvec3& p) const
{
    glm::dvec3 f = glm::floor((p - mOrigin) / mCell);
    f = glm::clamp(f, glm::dvec3(-1e6), glm::dvec3(1e6));
    return glm::ivec3(f);
}

template <class F> void PointGrid::visitRing(const glm::ivec3& c, int ring, F&& fn) const
{
    auto cellAt = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= mDims.x || y >= mDims.y || z >= mDims.z)
            return;
        size_t id = size_t(x) + size_t(mDims.x) * (size_t(y) + size_t(mDims.y) * size_t(z));
        for (uint32_t k = mStart[id]; k < mStart[id + 1]; ++k)
            fn(mIds[k]);
    };
    for (int dx = -ring; dx <= ring; ++dx)
        for (int dy = -ring; dy <= ring; ++dy)
        {
            if (std::abs(dx) == ring || std::abs(dy) == ring)
                for (int dz = -ring; dz <= ring; ++dz)
                    cellAt(c.x + dx, c.y + dy, c.z + dz);
            else
            {
                cellAt(c.x + dx, c.y + dy, c.z - ring);
                if (ring > 0)
                    cellAt(c.x + dx, c.y + dy, c.z + ring);
            }
        }
}

void PointGrid::nearest(const glm::dvec3& p, int k, std::vector<std::pair<double, uint32_t>>& out) const
{
    out.clear();
    if (mPoints.empty() || k <= 0)
        return;
    glm::ivec3 c = cellOf(p);
    int maxRing = 0;
    for (int a = 0; a < 3; ++a)
        maxRing = std::max({maxRing, std::abs(c[a]), std::abs(mDims[a] - 1 - c[a])});
    auto byDistance = [](const std::pair<double, uint32_t>& x, const std::pair<double, uint32_t>& y) { return x < y; };
    for (int ring = 0; ring <= maxRing; ++ring)
    {
        visitRing(c, ring, [&](uint32_t id) { out.push_back({glm::length(mPoints[id] - p), id}); });
        if (int(out.size()) >= k)
        {
            std::nth_element(out.begin(), out.begin() + (k - 1), out.end(), byDistance);
            // Anything in a farther ring is at least ring cells away.
            if (out[size_t(k - 1)].first < double(ring) * mCell)
                break;
        }
    }
    std::sort(out.begin(), out.end(), byDistance);
    if (int(out.size()) > k)
        out.resize(size_t(k));
}

void PointGrid::within(const glm::dvec3& p, double r, std::vector<uint32_t>& out) const
{
    out.clear();
    if (mPoints.empty() || r < 0.0)
        return;
    glm::ivec3 lo = glm::max(cellOf(p - glm::dvec3(r)), glm::ivec3(0)), hi = glm::min(cellOf(p + glm::dvec3(r)), mDims - 1);
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x)
            {
                size_t id = size_t(x) + size_t(mDims.x) * (size_t(y) + size_t(mDims.y) * size_t(z));
                for (uint32_t k = mStart[id]; k < mStart[id + 1]; ++k)
                    if (glm::length(mPoints[mIds[k]] - p) <= r)
                        out.push_back(mIds[k]);
            }
    std::sort(out.begin(), out.end());
}

// ------------------------------------------------------------------------------------------------ surfaces

std::vector<glm::dvec3> pointNormals(const geo::Mesh& topology, std::span<const glm::dvec3> points)
{
    std::vector<glm::dvec3> n(points.size(), glm::dvec3(0.0));
    std::vector<Index> ids;
    for (Index f = 0; f < topology.faceCapacity(); ++f)
    {
        if (!topology.faceValid(f))
            continue;
        ids = topology.facePoints(f);
        glm::dvec3 area(0.0);
        for (size_t k = 0; k < ids.size(); ++k)
            area += glm::cross(points[ids[k]], points[ids[(k + 1) % ids.size()]]);
        for (Index p : ids)
            n[p] += area;
    }
    for (glm::dvec3& v : n)
    {
        double l = glm::length(v);
        v = l > 1e-300 ? v / l : glm::dvec3(0.0);
    }
    return n;
}

void SurfaceTarget::build(const geo::Mesh& worldMesh)
{
    mMesh = worldMesh;
    mBvh.build(mMesh);
    mNormals = pointNormals(mMesh, mMesh.positions());
    mGrid.build(mMesh.positions());
    mClosed = mMesh.numFaces() > 0;
    for (Index e = 0; e < mMesh.edgeCapacity() && mClosed; ++e)
        if (mMesh.edgeValid(e) && mMesh.edgeFaceCount(e) != 2)
            mClosed = false;
}

uint32_t SurfaceTarget::triPoint(Index triangle, int k) const
{
    return uint32_t(mMesh.cornerPoint(mBvh.triangles().corners[size_t(triangle) * 3 + size_t(k)]));
}

glm::dvec3 SurfaceTarget::faceNormal(Index triangle) const
{
    glm::dvec3 a = mMesh.position(triPoint(triangle, 0)), b = mMesh.position(triPoint(triangle, 1)), c = mMesh.position(triPoint(triangle, 2));
    glm::dvec3 n = glm::cross(b - a, c - a);
    double l = glm::length(n);
    return l > 1e-300 ? n / l : glm::dvec3(0, 0, 1);
}

glm::dvec3 SurfaceTarget::smoothNormal(Index triangle, const glm::dvec3& bary) const
{
    glm::dvec3 n = mNormals[triPoint(triangle, 0)] * bary.x + mNormals[triPoint(triangle, 1)] * bary.y + mNormals[triPoint(triangle, 2)] * bary.z;
    double l = glm::length(n);
    return l > 1e-12 ? n / l : faceNormal(triangle);
}

bool SurfaceTarget::closest(const glm::dvec3& p, Hit& hit, double maxDistance) const
{
    geo::BvhClosest c;
    if (!mBvh.closestPoint(p, c, maxDistance))
        return false;
    hit.point = c.point;
    hit.triangle = c.triangle;
    hit.bary = c.bary;
    hit.distance = c.distance;
    hit.normal = smoothNormal(c.triangle, c.bary);
    return true;
}

bool SurfaceTarget::ray(const glm::dvec3& origin, const glm::dvec3& dir, double limit, bool cullFront, bool cullBack, Hit& hit) const
{
    double l = glm::length(dir);
    if (l < 1e-300 || empty())
        return false;
    glm::dvec3 d = dir / l;
    const double eps = std::max(1e-9, glm::length(mBvh.bounds().size()) * 1e-9);
    glm::dvec3 o = origin;
    double travelled = 0.0;
    for (int guard = 0; guard < 64; ++guard)
    {
        geo::BvhHit h;
        if (!mBvh.raycast(o, d, h, limit - travelled))
            return false;
        if (!((cullFront && h.frontFacing) || (cullBack && !h.frontFacing)))
        {
            hit.point = h.point;
            hit.triangle = h.triangle;
            hit.bary = h.bary;
            hit.distance = travelled + h.t;
            hit.normal = smoothNormal(h.triangle, h.bary);
            return true;
        }
        travelled += h.t + eps;
        o = h.point + d * eps;
        if (travelled >= limit)
            return false;
    }
    return false;
}

bool SurfaceTarget::inside(const glm::dvec3& p) const
{
    if (empty())
        return false;
    if (!mClosed)
    {
        Hit h;
        return closest(p, h) && glm::dot(p - h.point, h.normal) < 0.0;
    }
    // Ray parity along three skewed directions, by majority, so a ray grazing an edge cannot flip the answer.
    static const glm::dvec3 dirs[3] = {glm::normalize(glm::dvec3(0.5773, 0.5779, 0.5767)), glm::normalize(glm::dvec3(-0.6234, 0.2181, 0.7509)),
                                       glm::normalize(glm::dvec3(0.1349, -0.9311, 0.3389))};
    const double eps = std::max(1e-9, glm::length(mBvh.bounds().size()) * 1e-9);
    int votes = 0;
    for (const glm::dvec3& d : dirs)
    {
        int hits = 0;
        glm::dvec3 o = p;
        for (int guard = 0; guard < 512; ++guard)
        {
            geo::BvhHit h;
            if (!mBvh.raycast(o, d, h))
                break;
            ++hits;
            o = h.point + d * eps;
        }
        votes += hits & 1;
    }
    return votes >= 2;
}

// ------------------------------------------------------------------------------------------------ shrinkwrap

std::vector<glm::dvec3> shrinkwrap(const geo::Mesh& topology, const Adjacency& adj, std::span<const glm::dvec3> points, const SurfaceTarget& target,
                                   const ShrinkwrapParams& o)
{
    LILA_ZONE();

    std::vector<glm::dvec3> in(points.begin(), points.end());
    if (target.empty() || in.empty())
        return in;
    const bool smoothFirst = o.smoothIterations > 0;
    std::vector<glm::dvec3> src = in;
    if (smoothFirst)
        smooth(src, adj, o.smoothIterations, std::clamp(o.smoothStep, 0.0, 1.0), false);
    std::vector<glm::dvec3> normals;
    if (o.mode == ShrinkwrapMode::Project && glm::length(o.axis) < 1e-12)
        normals = pointNormals(topology, src);
    const double tiny = std::max(1e-12, glm::length(target.bvh().bounds().size()) * 1e-12);
    const double limit = o.limit > 0.0 ? o.limit : std::numeric_limits<double>::max();
    const bool cullFront = o.cull == CullFaces::Front, cullBack = o.cull == CullFaces::Back;

    auto place = [&](const glm::dvec3& p, const SurfaceTarget::Hit& h) {
        glm::dvec3 d = p - h.point;
        double dist = glm::length(d);
        const glm::dvec3& n = h.normal;
        double side = glm::dot(d, n) >= 0.0 ? 1.0 : -1.0;
        // Away from the surface along the line to the point where there is one, so offsets stay exact on hard edges.
        glm::dvec3 outward = dist > tiny ? d / dist * side : n;
        switch (o.place)
        {
        case ShrinkwrapPlace::OnSurface:
            return o.offset == 0.0 ? h.point : h.point + (dist > tiny ? d / dist : n) * o.offset;
        case ShrinkwrapPlace::Inside:
            return side * dist <= -o.offset ? p : h.point - outward * o.offset;
        case ShrinkwrapPlace::Outside:
            return side * dist >= o.offset ? p : h.point + outward * o.offset;
        case ShrinkwrapPlace::OutsideSurface:
            return h.point + outward * o.offset;
        case ShrinkwrapPlace::AboveSurface:
            return h.point + n * o.offset;
        }
        return p;
    };

    std::vector<glm::dvec3> out = src;
    parallelPoints(src.size(), [&](size_t i) {
        if (!topology.pointValid(Index(i)))
            return;
        const glm::dvec3 p = src[i];
        SurfaceTarget::Hit h;
        bool found = false;
        switch (o.mode)
        {
        case ShrinkwrapMode::NearestSurface:
            found = target.closest(p, h);
            break;
        case ShrinkwrapMode::NearestVertex:
        {
            std::vector<std::pair<double, uint32_t>> near;
            target.points().nearest(p, 4, near);
            for (auto& [d, id] : near)
                if (target.mesh().pointValid(id))
                {
                    h.point = target.mesh().position(id);
                    h.normal = target.normals()[id];
                    h.distance = d;
                    found = true;
                    break;
                }
            break;
        }
        case ShrinkwrapMode::Project:
        {
            glm::dvec3 dir = glm::length(o.axis) >= 1e-12 ? glm::normalize(o.axis) : normals[i];
            if (glm::length(dir) < 1e-12)
                break;
            SurfaceTarget::Hit a, b;
            bool ha = o.positive && target.ray(p, dir, limit, cullFront, cullBack, a);
            bool hb = o.negative && target.ray(p, -dir, limit, cullFront, cullBack, b);
            if (ha && (!hb || a.distance <= b.distance))
                h = a, found = true;
            else if (hb)
                h = b, found = true;
            break;
        }
        case ShrinkwrapMode::TargetNormal:
        {
            found = target.closest(p, h);
            for (int it = 0; found && it < 8; ++it)
            {
                SurfaceTarget::Hit a, b;
                double reach = h.distance * 4.0 + tiny * 1e6;
                bool ha = target.ray(p, -h.normal, reach, false, false, a);
                bool hb = target.ray(p, h.normal, reach, false, false, b);
                if (!ha && !hb)
                    break;
                SurfaceTarget::Hit next = ha && (!hb || a.distance <= b.distance) ? a : b;
                bool settled = glm::length(next.point - h.point) <= tiny * 1e3;
                h = next;
                if (settled)
                    break;
            }
            break;
        }
        }
        if (found)
            out[i] = place(p, h);
    });

    if (smoothFirst && o.keepDetail)
    {
        // The detail smoothing removed, in the smoothed shape's point frames, put back on in the wrapped shape's.
        std::vector<glm::dmat3> before = pointFrames(topology, src, adj), after = pointFrames(topology, out, adj);
        parallelPoints(out.size(), [&](size_t i) { out[i] += after[i] * (glm::transpose(before[i]) * (in[i] - src[i])); });
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ surface deform

glm::dmat3 polygonFrame(std::span<const glm::dvec3> polygon)
{
    if (polygon.size() < 3)
        return glm::dmat3(1.0);
    glm::dvec3 c(0.0), n(0.0);
    for (size_t k = 0; k < polygon.size(); ++k)
    {
        c += polygon[k];
        n += glm::cross(polygon[k], polygon[(k + 1) % polygon.size()]);
    }
    c /= double(polygon.size());
    double ln = glm::length(n);
    if (ln < 1e-300)
        return glm::dmat3(1.0);
    n /= ln;
    glm::dvec3 x = polygon[0] - c;
    x -= n * glm::dot(n, x);
    double lx = glm::length(x);
    x = lx > 1e-300 ? x / lx : anyPerpendicular(n);
    return {x, glm::cross(n, x), n};
}

void polygonCoordinates(std::span<const glm::dvec3> polygon, const glm::dvec3& x, std::span<double> out)
{
    const size_t n = polygon.size();
    std::fill(out.begin(), out.end(), 0.0);
    if (n == 0)
        return;
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : polygon)
        c += p;
    c /= double(n);
    glm::dmat3 ft = glm::transpose(polygonFrame(polygon));
    thread_local std::vector<glm::dvec2> s;
    thread_local std::vector<double> r, tanHalf;
    s.resize(n);
    r.resize(n);
    tanHalf.resize(n);
    glm::dvec3 xl = ft * (x - c);
    double size = 0.0;
    for (size_t j = 0; j < n; ++j)
    {
        glm::dvec3 v = ft * (polygon[j] - c);
        s[j] = glm::dvec2(v) - glm::dvec2(xl);
        r[j] = glm::length(s[j]);
        size = std::max(size, glm::length(glm::dvec2(v)));
    }
    const double eps = std::max(size, 1e-300) * 1e-12;
    for (size_t j = 0; j < n; ++j)
        if (r[j] <= eps)
        {
            out[j] = 1.0;
            return;
        }
    for (size_t j = 0; j < n; ++j)
    {
        size_t k = (j + 1) % n;
        double det = s[j].x * s[k].y - s[j].y * s[k].x, dot = glm::dot(s[j], s[k]);
        if (std::abs(det) <= eps * (r[j] + r[k]))
        {
            if (dot < 0.0)
            {
                // On the edge: linear along it.
                out[j] = r[k] / (r[j] + r[k]);
                out[k] = r[j] / (r[j] + r[k]);
                return;
            }
            tanHalf[j] = 0.0;
            continue;
        }
        tanHalf[j] = (r[j] * r[k] - dot) / det;
    }
    double sum = 0.0;
    for (size_t j = 0; j < n; ++j)
    {
        out[j] = (tanHalf[(j + n - 1) % n] + tanHalf[j]) / r[j];
        sum += out[j];
    }
    if (std::abs(sum) < 1e-300)
    {
        std::fill(out.begin(), out.end(), 1.0 / double(n));
        return;
    }
    for (double& w : out.first(n))
        w /= sum;
}

namespace
{
/** One influence on face f for a target at p whose surface point there is q. */
void addSurfaceInfluence(std::vector<SurfaceBinding::Influence>& list, std::vector<uint32_t>& ids, std::vector<double>& coords,
                         const geo::Mesh& driver, std::span<const glm::dvec3> pos, Index f, const glm::dvec3& p, const glm::dvec3& q, double weight)
{
    std::vector<Index> fp;
    std::vector<glm::dvec3> poly = facePositions(driver, f, pos, fp);
    SurfaceBinding::Influence inf;
    inf.first = uint32_t(ids.size());
    inf.count = uint32_t(fp.size());
    inf.weight = weight;
    std::vector<double> c(fp.size());
    polygonCoordinates(poly, q, c);
    glm::dvec3 anchor(0.0);
    for (size_t k = 0; k < fp.size(); ++k)
    {
        anchor += poly[k] * c[k];
        ids.push_back(uint32_t(fp[k]));
        coords.push_back(c[k]);
    }
    inf.offset = glm::transpose(polygonFrame(poly)) * (p - anchor);
    list.push_back(inf);
}
} // namespace

SurfaceBinding surfaceBind(const geo::Mesh& driver, std::span<const glm::dvec3> targets, int maxFaces, double falloff, std::string_view uvSet)
{
    LILA_ZONE();

    SurfaceBinding b;
    b.driverPoints = driver.pointCapacity();
    geo::MeshBvh bvh(driver);
    if (bvh.empty())
    {
        b.offsets.assign(targets.size() + 1, 0);
        return b;
    }
    const geo::Triangulation& tris = bvh.triangles();
    std::span<const glm::vec2> uvs = driver.cornerAttrs().get<glm::vec2>(uvSet);
    b.hasUv = !uvs.empty();
    std::span<const glm::dvec3> pos = driver.positions();
    const double eps = std::max(1e-9, glm::length(driver.bounds().size()) * 1e-7);
    maxFaces = std::clamp(maxFaces, 1, 16);
    falloff = std::clamp(falloff, 0.0, 16.0);

    struct Local
    {
        std::vector<SurfaceBinding::Influence> infs;
        std::vector<uint32_t> ids;
        std::vector<double> coords;
    };
    std::vector<Local> per(targets.size());
    parallelPoints(targets.size(), [&](size_t i) {
        const glm::dvec3& p = targets[i];
        geo::BvhClosest hit;
        if (!bvh.closestPoint(p, hit))
            return;
        struct Cand
        {
            Index face;
            Index tri;
            double dist;
            glm::dvec3 point, bary;
        };
        std::vector<Cand> cands;
        if (maxFaces > 1)
        {
            double reach = hit.distance * 2.0 + eps * 1e3;
            geo::Bounds box;
            box.extend(p - glm::dvec3(reach));
            box.extend(p + glm::dvec3(reach));
            for (Index t : bvh.overlapping(box))
            {
                glm::dvec3 a = pos[driver.cornerPoint(tris.corners[size_t(t) * 3])], bb = pos[driver.cornerPoint(tris.corners[size_t(t) * 3 + 1])],
                           c = pos[driver.cornerPoint(tris.corners[size_t(t) * 3 + 2])];
                glm::dvec3 bary;
                glm::dvec3 q = closestOnTriangle(p, a, bb, c, bary);
                double d = glm::length(p - q);
                if (d > reach)
                    continue;
                Index f = tris.faces[t];
                auto it = std::find_if(cands.begin(), cands.end(), [&](const Cand& x) { return x.face == f; });
                if (it == cands.end())
                    cands.push_back({f, t, d, q, bary});
                else if (d < it->dist)
                    *it = {f, t, d, q, bary};
            }
        }
        if (cands.empty())
            cands.push_back({hit.face, hit.triangle, hit.distance, hit.point, hit.bary});
        std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.dist != y.dist ? x.dist < y.dist : x.face < y.face; });
        if (int(cands.size()) > maxFaces)
            cands.resize(size_t(maxFaces));
        double total = 0.0;
        std::vector<double> w(cands.size());
        for (size_t k = 0; k < cands.size(); ++k)
            total += w[k] = 1.0 / std::pow(cands[k].dist + eps, falloff);
        Local& L = per[i];
        for (size_t k = 0; k < cands.size(); ++k)
        {
            addSurfaceInfluence(L.infs, L.ids, L.coords, driver, pos, cands[k].face, p, cands[k].point, w[k] / total);
            if (b.hasUv)
            {
                const Index* cr = &tris.corners[size_t(cands[k].tri) * 3];
                glm::dvec2 uv[3] = {glm::dvec2(uvs[cr[0]]), glm::dvec2(uvs[cr[1]]), glm::dvec2(uvs[cr[2]])};
                SurfaceBinding::Influence& inf = L.infs.back();
                inf.uv = uv[0] * cands[k].bary.x + uv[1] * cands[k].bary.y + uv[2] * cands[k].bary.z;
                glm::dmat3 f = uvFrame(pos[driver.cornerPoint(cr[0])], pos[driver.cornerPoint(cr[1])], pos[driver.cornerPoint(cr[2])], uv);
                inf.uvOffset = glm::transpose(f) * (p - cands[k].point);
            }
        }
    });
    for (Local& L : per)
    {
        for (SurfaceBinding::Influence& inf : L.infs)
        {
            inf.first += uint32_t(b.ids.size());
            b.influences.push_back(inf);
        }
        b.ids.insert(b.ids.end(), L.ids.begin(), L.ids.end());
        b.coords.insert(b.coords.end(), L.coords.begin(), L.coords.end());
        b.offsets.push_back(uint32_t(b.influences.size()));
    }
    return b;
}

std::vector<glm::dvec3> surfaceApply(const SurfaceBinding& bind, std::span<const glm::dvec3> driver, std::span<const glm::dvec3> targets)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(targets.begin(), targets.end());
    const size_t n = std::min(targets.size(), bind.offsets.size() - 1);
    parallelPoints(n, [&](size_t i) {
        if (bind.offsets[i] == bind.offsets[i + 1])
            return;
        thread_local std::vector<glm::dvec3> poly;
        glm::dvec3 sum(0.0);
        for (uint32_t k = bind.offsets[i]; k < bind.offsets[i + 1]; ++k)
        {
            const SurfaceBinding::Influence& inf = bind.influences[k];
            poly.resize(inf.count);
            glm::dvec3 anchor(0.0);
            for (uint32_t j = 0; j < inf.count; ++j)
            {
                uint32_t id = bind.ids[inf.first + j];
                if (id >= driver.size())
                    return;
                poly[j] = driver[id];
                anchor += poly[j] * bind.coords[inf.first + j];
            }
            sum += (anchor + polygonFrame(poly) * inf.offset) * inf.weight;
        }
        out[i] = sum;
    });
    return out;
}

SurfaceBinding surfaceRebindByUv(const SurfaceBinding& bind, const geo::Mesh& driver, std::string_view uvSet)
{
    LILA_ZONE();

    SurfaceBinding b;
    b.driverPoints = driver.pointCapacity();
    b.hasUv = bind.hasUv;
    UvLocator loc(driver, uvSet);
    const size_t n = bind.offsets.size() - 1;
    if (!bind.hasUv || !loc.valid())
    {
        b.offsets.assign(n + 1, 0);
        return b;
    }
    std::span<const glm::dvec3> pos = driver.positions();
    struct Local
    {
        std::vector<SurfaceBinding::Influence> infs;
        std::vector<uint32_t> ids;
        std::vector<double> coords;
    };
    std::vector<Local> per(n);
    parallelPoints(n, [&](size_t i) {
        Local& L = per[i];
        double total = 0.0;
        for (uint32_t k = bind.offsets[i]; k < bind.offsets[i + 1]; ++k)
        {
            const SurfaceBinding::Influence& old = bind.influences[k];
            uint32_t t;
            glm::dvec3 bary;
            if (!loc.find(old.uv, t, bary))
                continue;
            glm::dvec3 a = pos[loc.pointOf(t, 0)], bb = pos[loc.pointOf(t, 1)], c = pos[loc.pointOf(t, 2)];
            glm::dvec2 uv[3] = {loc.uvOf(t, 0), loc.uvOf(t, 1), loc.uvOf(t, 2)};
            glm::dvec3 q = a * bary.x + bb * bary.y + c * bary.z;
            glm::dvec3 p = q + uvFrame(a, bb, c, uv) * old.uvOffset;
            addSurfaceInfluence(L.infs, L.ids, L.coords, driver, pos, loc.tris().faces[t], p, q, old.weight);
            L.infs.back().uv = old.uv;
            L.infs.back().uvOffset = old.uvOffset;
            total += old.weight;
        }
        for (SurfaceBinding::Influence& inf : L.infs)
            inf.weight /= total;
    });
    for (Local& L : per)
    {
        for (SurfaceBinding::Influence& inf : L.infs)
        {
            inf.first += uint32_t(b.ids.size());
            b.influences.push_back(inf);
        }
        b.ids.insert(b.ids.end(), L.ids.begin(), L.ids.end());
        b.coords.insert(b.coords.end(), L.coords.begin(), L.coords.end());
        b.offsets.push_back(uint32_t(b.influences.size()));
    }
    return b;
}

PointCapture pointCapture(std::span<const glm::dvec3> driverRest, std::span<const glm::dvec3> targets, int maxPoints, double radius, int neighbours)
{
    LILA_ZONE();

    PointCapture c;
    c.rest.assign(driverRest.begin(), driverRest.end());
    if (driverRest.empty())
    {
        c.offsets.assign(targets.size() + 1, 0);
        return c;
    }
    PointGrid grid(driverRest);
    maxPoints = std::clamp(maxPoints, 1, 64);
    neighbours = std::clamp(neighbours, 2, 64);
    std::vector<std::vector<std::pair<uint32_t, double>>> per(targets.size());
    parallelPoints(targets.size(), [&](size_t i) {
        const glm::dvec3& p = targets[i];
        std::vector<std::pair<double, uint32_t>> near;
        double r = radius;
        if (radius > 0.0)
        {
            std::vector<uint32_t> ids;
            grid.within(p, radius, ids);
            for (uint32_t id : ids)
                near.push_back({glm::length(driverRest[id] - p), id});
            std::sort(near.begin(), near.end());
            if (int(near.size()) > maxPoints)
                near.resize(size_t(maxPoints));
        }
        if (near.empty())
        {
            grid.nearest(p, maxPoints, near);
            r = near.back().first * 1.5;
        }
        r = std::max(r, 1e-12);
        double total = 0.0;
        for (auto& [d, id] : near)
        {
            double t = std::min(d / r, 1.0);
            double w = (1.0 - t * t) * (1.0 - t * t) * (1.0 - t * t);
            if (w > 0.0)
            {
                per[i].push_back({id, w});
                total += w;
            }
        }
        if (total <= 0.0)
        {
            per[i] = {{near.front().second, 1.0}};
            total = 1.0;
        }
        for (auto& [id, w] : per[i])
            w /= total;
    });
    std::vector<uint8_t> used(driverRest.size(), 0);
    for (size_t i = 0; i < per.size(); ++i)
    {
        for (auto& [id, w] : per[i])
        {
            c.ids.push_back(id);
            c.weights.push_back(w);
            used[id] = 1;
        }
        c.offsets.push_back(uint32_t(c.ids.size()));
    }
    std::vector<std::vector<uint32_t>> nb(driverRest.size());
    parallelPoints(driverRest.size(), [&](size_t j) {
        if (!used[j])
            return;
        std::vector<std::pair<double, uint32_t>> near;
        grid.nearest(driverRest[j], neighbours + 1, near);
        for (auto& [d, id] : near)
            nb[j].push_back(id);
    });
    for (auto& list : nb)
    {
        c.nb.insert(c.nb.end(), list.begin(), list.end());
        c.nbOffsets.push_back(uint32_t(c.nb.size()));
    }
    return c;
}

std::vector<glm::dvec3> pointDeform(const PointCapture& c, std::span<const glm::dvec3> driver, std::span<const glm::dvec3> targets)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(targets.begin(), targets.end());
    if (driver.size() != c.rest.size() || c.nbOffsets.size() != c.rest.size() + 1)
        return out;
    std::vector<RigidFit> fits(c.rest.size());
    parallelPoints(c.rest.size(), [&](size_t j) {
        std::span<const uint32_t> ids(c.nb.data() + c.nbOffsets[j], c.nbOffsets[j + 1] - c.nbOffsets[j]);
        if (!ids.empty())
            fits[j] = rigidFit(c.rest, driver, ids);
    });
    const size_t n = std::min(targets.size(), c.offsets.size() - 1);
    parallelPoints(n, [&](size_t i) {
        if (c.offsets[i] == c.offsets[i + 1])
            return;
        glm::dvec3 sum(0.0);
        for (uint32_t k = c.offsets[i]; k < c.offsets[i + 1]; ++k)
            sum += fits[c.ids[k]].apply(targets[i]) * c.weights[k];
        out[i] = sum;
    });
    return out;
}

// ------------------------------------------------------------------------------------------------ proximity wrap

ProximityBinding proximityBind(std::span<const geo::Mesh* const> drivers, std::span<const glm::dvec3> targets, const ProximityParams& o,
                               const Adjacency* targetAdjacency)
{
    LILA_ZONE();

    ProximityBinding b;
    std::vector<geo::MeshBvh> bvhs(drivers.size());
    geo::Bounds all;
    for (size_t d = 0; d < drivers.size(); ++d)
    {
        b.driverPoints.push_back(drivers[d] ? drivers[d]->pointCapacity() : 0);
        if (!drivers[d])
            continue;
        bvhs[d].build(*drivers[d]);
        all.extend(drivers[d]->bounds());
    }
    const double eps = std::max(1e-9, glm::length(all.size()) * 1e-5);
    const int maxDrivers = std::clamp(o.maxDrivers, 1, 32);
    const double falloff = std::max(o.falloff, 1.0);
    using Influence = ProximityBinding::Influence;
    auto triPoint = [&](size_t d, Index t, int k) {
        return uint32_t(drivers[d]->cornerPoint(bvhs[d].triangles().corners[size_t(t) * 3 + size_t(k)]));
    };
    auto makeInfluence = [&](uint32_t d, const uint32_t tri[3], const glm::dvec3& p, double weight) {
        const geo::Mesh& m = *drivers[d];
        Influence inf;
        inf.driver = d;
        std::copy(tri, tri + 3, inf.tri);
        glm::dvec3 a = m.position(tri[0]), bb = m.position(tri[1]), c = m.position(tri[2]);
        glm::dvec3 q = closestOnTriangle(p, a, bb, c, inf.bary);
        inf.offset = glm::transpose(triangleFrame(a, bb, c)) * (p - q);
        inf.weight = weight;
        inf.restArea = triangleArea(a, bb, c);
        inf.anchor = q;
        return inf;
    };

    std::vector<std::vector<Influence>> per(targets.size());
    parallelPoints(targets.size(), [&](size_t i) {
        const glm::dvec3& p = targets[i];
        double nearest = std::numeric_limits<double>::max();
        for (size_t d = 0; d < bvhs.size(); ++d)
        {
            geo::BvhClosest hit;
            if (!bvhs[d].empty() && bvhs[d].closestPoint(p, hit, nearest))
                nearest = std::min(nearest, hit.distance);
        }
        if (nearest == std::numeric_limits<double>::max())
            return;
        double reach = nearest * falloff + eps;
        geo::Bounds box;
        box.extend(p - glm::dvec3(reach));
        box.extend(p + glm::dvec3(reach));
        struct Cand
        {
            uint32_t driver;
            Index tri;
            double dist;
        };
        std::vector<Cand> cands;
        for (size_t d = 0; d < bvhs.size(); ++d)
        {
            if (bvhs[d].empty())
                continue;
            for (Index t : bvhs[d].overlapping(box))
            {
                const geo::Mesh& m = *drivers[d];
                glm::dvec3 bary;
                glm::dvec3 q = closestOnTriangle(p, m.position(triPoint(d, t, 0)), m.position(triPoint(d, t, 1)), m.position(triPoint(d, t, 2)), bary);
                double dist = glm::length(p - q);
                if (dist <= reach)
                    cands.push_back({uint32_t(d), t, dist});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
            return x.dist != y.dist ? x.dist < y.dist : x.driver != y.driver ? x.driver < y.driver : x.tri < y.tri;
        });
        if (int(cands.size()) > maxDrivers)
            cands.resize(size_t(maxDrivers));
        double total = 0.0;
        for (const Cand& c : cands)
        {
            uint32_t tri[3] = {triPoint(c.driver, c.tri, 0), triPoint(c.driver, c.tri, 1), triPoint(c.driver, c.tri, 2)};
            double w = 1.0 / ((c.dist + eps) * (c.dist + eps));
            per[i].push_back(makeInfluence(c.driver, tri, p, w));
            total += w;
        }
        for (Influence& inf : per[i])
            inf.weight /= total;
    });

    if (targetAdjacency && o.smoothIterations > 0 && targetAdjacency->offsets.size() == targets.size() + 1)
    {
        const double amount = std::clamp(o.smoothAmount, 0.0, 1.0);
        using Key = std::array<uint32_t, 4>;
        auto keyOf = [](const Influence& inf) { return Key{inf.driver, inf.tri[0], inf.tri[1], inf.tri[2]}; };
        for (int it = 0; it < o.smoothIterations; ++it)
        {
            std::vector<std::vector<Influence>> next(targets.size());
            parallelPoints(targets.size(), [&](size_t i) {
                const Adjacency& adj = *targetAdjacency;
                uint32_t b0 = adj.offsets[i], b1 = adj.offsets[i + 1];
                if (b0 == b1 || per[i].empty())
                {
                    next[i] = per[i];
                    return;
                }
                std::map<Key, double> w;
                for (const Influence& inf : per[i])
                    w[keyOf(inf)] += (1.0 - amount) * inf.weight;
                for (uint32_t k = b0; k < b1; ++k)
                    for (const Influence& inf : per[adj.neighbors[k]])
                        w[keyOf(inf)] += amount * inf.weight / double(b1 - b0);
                std::vector<std::pair<double, Key>> ranked;
                for (auto& [key, v] : w)
                    ranked.push_back({-v, key});
                std::sort(ranked.begin(), ranked.end());
                if (int(ranked.size()) > maxDrivers)
                    ranked.resize(size_t(maxDrivers));
                double total = 0.0;
                for (auto& [v, key] : ranked)
                    total -= v;
                for (auto& [v, key] : ranked)
                {
                    uint32_t tri[3] = {key[1], key[2], key[3]};
                    next[i].push_back(makeInfluence(key[0], tri, targets[i], -v / total));
                }
            });
            per.swap(next);
        }
    }

    for (auto& list : per)
    {
        b.influences.insert(b.influences.end(), list.begin(), list.end());
        b.offsets.push_back(uint32_t(b.influences.size()));
    }
    return b;
}

std::vector<glm::dvec3> proximityApply(const ProximityBinding& b, std::span<const std::span<const glm::dvec3>> drivers, std::span<const glm::dvec3> targets,
                                       WrapMode mode, double scaleCompensation)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(targets.begin(), targets.end());
    const size_t n = std::min(targets.size(), b.offsets.size() - 1);
    parallelPoints(n, [&](size_t i) {
        if (b.offsets[i] == b.offsets[i + 1])
            return;
        glm::dvec3 sum(0.0);
        for (uint32_t k = b.offsets[i]; k < b.offsets[i + 1]; ++k)
        {
            const ProximityBinding::Influence& inf = b.influences[k];
            if (inf.driver >= drivers.size())
                return;
            std::span<const glm::dvec3> d = drivers[inf.driver];
            if (inf.tri[0] >= d.size() || inf.tri[1] >= d.size() || inf.tri[2] >= d.size())
                return;
            const glm::dvec3 &a = d[inf.tri[0]], &bb = d[inf.tri[1]], &c = d[inf.tri[2]];
            glm::dvec3 q = a * inf.bary.x + bb * inf.bary.y + c * inf.bary.z;
            // Bindings from before wrap 2 carry no rest area or anchor, so they always ride by offset.
            const bool hasRest = inf.restArea > 0.0;
            if (mode == WrapMode::Snap)
                sum += q * inf.weight;
            else if (mode == WrapMode::Surface && hasRest)
                sum += (targets[i] + q - inf.anchor) * inf.weight;
            else
            {
                double scale = 1.0;
                if (hasRest && scaleCompensation != 0.0)
                    scale = 1.0 + scaleCompensation * (std::sqrt(triangleArea(a, bb, c) / inf.restArea) - 1.0);
                sum += (q + triangleFrame(a, bb, c) * inf.offset * scale) * inf.weight;
            }
        }
        out[i] = sum;
    });
    return out;
}

// ------------------------------------------------------------------------------------------------ collision

std::vector<glm::dvec3> collide(const geo::Mesh& topology, std::span<const glm::dvec3> points, std::span<const SurfaceTarget* const> meshes,
                                std::span<const Capsule> capsules, const CollisionParams& o)
{
    LILA_ZONE();

    const double a = std::max(o.offset, 0.0), F = std::max(o.falloff, 0.0);
    // Signed distance s (negative inside) to the distance kept: a hard stop at a, or a C1 ease over [a - F, a + F].
    auto kept = [&](double s, bool soft) {
        if (F <= 0.0 || !soft)
            return std::max(s, a);
        if (s >= a + F)
            return s;
        if (s <= a - F)
            return a;
        double t = s - (a - F);
        return a + t * t / (4.0 * F);
    };
    const double reach = a + F;
    std::vector<geo::Bounds> boxes;
    for (const SurfaceTarget* m : meshes)
    {
        geo::Bounds bx = m && !m->empty() ? m->bvh().bounds() : geo::Bounds();
        if (bx.valid())
        {
            bx.min -= glm::dvec3(reach);
            bx.max += glm::dvec3(reach);
        }
        boxes.push_back(bx);
    }
    const double sign = o.keepInside ? -1.0 : 1.0;

    auto push = [&](glm::dvec3 p, bool soft) {
        for (size_t m = 0; m < meshes.size(); ++m)
        {
            const SurfaceTarget* t = meshes[m];
            if (!t || t->empty())
                continue;
            const geo::Bounds& bx = boxes[m];
            bool outsideBox = p.x < bx.min.x || p.y < bx.min.y || p.z < bx.min.z || p.x > bx.max.x || p.y > bx.max.y || p.z > bx.max.z;
            if (outsideBox && !o.keepInside)
                continue;
            SurfaceTarget::Hit h;
            if (!t->closest(p, h))
                continue;
            bool in = outsideBox ? false : t->inside(p);
            double s = in ? -h.distance : h.distance;
            glm::dvec3 n = h.distance > 1e-12 ? (p - h.point) / h.distance * (in ? -1.0 : 1.0) : h.normal;
            s *= sign;
            n *= sign;
            double k = kept(s, soft);
            if (k != s)
                p = h.point + n * k;
        }
        for (const Capsule& c : capsules)
        {
            glm::dvec3 ab = c.b - c.a;
            double len2 = glm::dot(ab, ab);
            double t = len2 > 1e-300 ? std::clamp(glm::dot(p - c.a, ab) / len2, 0.0, 1.0) : 0.0;
            glm::dvec3 axisPoint = c.a + ab * t;
            glm::dvec3 v = p - axisPoint;
            double d = glm::length(v);
            glm::dvec3 n = d > 1e-12 ? v / d : anyPerpendicular(len2 > 1e-300 ? ab : glm::dvec3(0, 1, 0));
            glm::dvec3 surface = axisPoint + n * c.radius;
            double s = sign * (d - c.radius);
            double k = kept(s, soft);
            if (k != s)
                p = surface + n * (sign * k);
        }
        return p;
    };

    std::vector<glm::dvec3> out(points.begin(), points.end());
    // The second pass only stops points another collider pushed into this one; easing again would push them further.
    parallelPoints(out.size(), [&](size_t i) { out[i] = push(push(out[i], true), false); });

    if (o.volumeKeep > 0.0)
    {
        std::vector<uint32_t> contacts;
        std::vector<glm::dvec3> contactPos;
        double total = 0.0, deepest = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            double m = glm::length(out[i] - points[i]);
            if (m > 1e-12)
            {
                contacts.push_back(uint32_t(i));
                contactPos.push_back(points[i]);
                total += m;
                deepest = std::max(deepest, m);
            }
        }
        if (!contacts.empty())
        {
            const double R = o.bulgeRadius > 0.0 ? o.bulgeRadius : deepest * 4.0;
            PointGrid grid(contactPos);
            std::vector<uint8_t> isContact(out.size(), 0);
            for (uint32_t c : contacts)
                isContact[c] = 1;
            std::vector<double> raw(out.size(), 0.0);
            parallelPoints(out.size(), [&](size_t j) {
                if (isContact[j] || !topology.pointValid(Index(j)))
                    return;
                std::vector<uint32_t> near;
                grid.within(points[j], R, near);
                for (uint32_t k : near)
                {
                    double t = glm::length(contactPos[k] - points[j]) / R;
                    raw[j] += glm::length(out[contacts[k]] - points[contacts[k]]) * (1.0 - t * t) * (1.0 - t * t);
                }
            });
            double rawSum = std::accumulate(raw.begin(), raw.end(), 0.0);
            if (rawSum > 0.0)
            {
                std::vector<glm::dvec3> normals = pointNormals(topology, points);
                const double s = std::clamp(o.volumeKeep, 0.0, 1.0) * total / rawSum;
                parallelPoints(out.size(), [&](size_t j) {
                    if (raw[j] > 0.0)
                        out[j] = push(out[j] + normals[j] * (raw[j] * s), false);
                });
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ rigidify

std::vector<int32_t> connectedRegions(const Adjacency& adj, std::span<const uint8_t> mask)
{
    const size_t n = adj.offsets.size() - 1;
    std::vector<int32_t> region(n, -1);
    int32_t next = 0;
    std::vector<uint32_t> stack;
    for (size_t s = 0; s < n; ++s)
    {
        if (region[s] >= 0 || (!mask.empty() && (s >= mask.size() || !mask[s])))
            continue;
        region[s] = next;
        stack.assign(1, uint32_t(s));
        while (!stack.empty())
        {
            uint32_t p = stack.back();
            stack.pop_back();
            for (uint32_t k = adj.offsets[p]; k < adj.offsets[p + 1]; ++k)
            {
                uint32_t q = adj.neighbors[k];
                if (region[q] >= 0 || (!mask.empty() && (q >= mask.size() || !mask[q])))
                    continue;
                region[q] = next;
                stack.push_back(q);
            }
        }
        ++next;
    }
    return region;
}

std::vector<glm::dvec3> rigidify(const Adjacency& adj, std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed, std::span<const int32_t> region,
                                 int blendRings)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(deformed.begin(), deformed.end());
    const size_t n = std::min({rest.size(), deformed.size(), region.size()});
    int32_t regions = 0;
    for (size_t i = 0; i < n; ++i)
        regions = std::max(regions, region[i] + 1);
    if (regions == 0)
        return out;
    std::vector<std::vector<uint32_t>> members(static_cast<size_t>(regions));
    for (size_t i = 0; i < n; ++i)
        if (region[i] >= 0)
            members[size_t(region[i])].push_back(uint32_t(i));
    std::vector<RigidFit> fits(static_cast<size_t>(regions));
    parallelPoints(size_t(regions), [&](size_t r) { fits[r] = rigidFit(rest, deformed, members[r]); });
    for (size_t i = 0; i < n; ++i)
        if (region[i] >= 0)
            out[i] = fits[size_t(region[i])].apply(rest[i]);

    if (blendRings <= 0 || adj.offsets.size() != n + 1)
        return out;
    // Rings outward from every region at once; a point takes the region that reaches it first, lowest id on ties.
    std::vector<int32_t> owner(n, -1);
    std::vector<int> ring(n, 0);
    std::vector<uint32_t> front;
    for (size_t i = 0; i < n; ++i)
        if (region[i] >= 0)
        {
            owner[i] = region[i];
            front.push_back(uint32_t(i));
        }
    for (int k = 1; k <= blendRings && !front.empty(); ++k)
    {
        std::vector<std::pair<uint32_t, int32_t>> reached;
        for (uint32_t p : front)
            for (uint32_t e = adj.offsets[p]; e < adj.offsets[p + 1]; ++e)
                if (owner[adj.neighbors[e]] < 0)
                    reached.push_back({adj.neighbors[e], owner[p]});
        std::sort(reached.begin(), reached.end());
        front.clear();
        for (auto& [p, r] : reached)
        {
            if (owner[p] >= 0)
                continue;
            owner[p] = r;
            ring[p] = k;
            front.push_back(p);
        }
    }
    for (size_t i = 0; i < n; ++i)
        if (region[i] < 0 && owner[i] >= 0)
        {
            double w = 1.0 - double(ring[i]) / double(blendRings + 1);
            out[i] = glm::mix(deformed[i], fits[size_t(owner[i])].apply(rest[i]), w);
        }
    return out;
}

// ------------------------------------------------------------------------------------------------ skin slide

std::vector<glm::dvec3> skinSlide(const Adjacency& adj, std::span<const glm::dvec3> rest, const SurfaceTarget& restSurface, std::span<const glm::dvec3> deformed,
                                  const SlideParams& o)
{
    LILA_ZONE();

    const size_t n = std::min(rest.size(), deformed.size());
    std::vector<glm::dvec3> out(deformed.begin(), deformed.end());
    if (restSurface.empty() || adj.offsets.size() != n + 1 || restSurface.normals().size() != n)
        return out;
    std::span<const glm::dvec3> rn = restSurface.normals();
    std::vector<glm::dvec3> x(n), xn(n), lift(n, glm::dvec3(0.0));
    auto project = [&](size_t i, const glm::dvec3& p) {
        SurfaceTarget::Hit h;
        if (restSurface.closest(p, h))
        {
            x[i] = h.point;
            xn[i] = h.normal;
        }
        else
        {
            x[i] = p;
            xn[i] = rn[i];
        }
    };
    parallelPoints(n, [&](size_t i) {
        glm::dvec3 d = deformed[i] - rest[i];
        double along = glm::dot(d, rn[i]);
        lift[i] = rn[i] * along;
        project(i, rest[i] + d - lift[i]);
    });
    std::vector<glm::dvec3> moved(n);
    const double step = std::clamp(o.relaxStep, 0.0, 1.0);
    for (int it = 0; it < o.relaxIterations && step > 0.0; ++it)
    {
        parallelPoints(n, [&](size_t i) {
            uint32_t b0 = adj.offsets[i], b1 = adj.offsets[i + 1];
            moved[i] = x[i];
            if (b0 == b1)
                return;
            glm::dvec3 avg(0.0), avgRest(0.0);
            for (uint32_t k = b0; k < b1; ++k)
            {
                avg += x[adj.neighbors[k]];
                avgRest += rest[adj.neighbors[k]];
            }
            double inv = 1.0 / double(b1 - b0);
            // Toward the rest shape's own Laplacian, so the rest pose is a fixed point and only stretching relaxes.
            glm::dvec3 delta = ((avg - avgRest) * inv - (x[i] - rest[i])) * step;
            delta -= xn[i] * glm::dot(delta, xn[i]);
            moved[i] = x[i] + delta;
        });
        parallelPoints(n, [&](size_t i) { project(i, moved[i]); });
    }
    const double keep = o.normalAmount;
    parallelPoints(n, [&](size_t i) { out[i] = x[i] + xn[i] * (glm::dot(lift[i], rn[i]) * keep); });
    return out;
}

// ------------------------------------------------------------------------------------------------ pins

bool locateUv(const geo::Mesh& mesh, std::string_view uvSet, const glm::dvec2& uv, SurfacePoint& out)
{
    UvLocator loc(mesh, uvSet);
    uint32_t t;
    glm::dvec3 bary;
    if (!loc.find(uv, t, bary))
        return false;
    for (int k = 0; k < 3; ++k)
    {
        out.points[k] = loc.pointOf(t, k);
        out.uv[k] = loc.uvOf(t, k);
    }
    out.bary = bary;
    out.hasUv = true;
    return true;
}

bool locateNearest(const geo::Mesh& mesh, const glm::dvec3& p, std::string_view uvSet, SurfacePoint& out)
{
    geo::MeshBvh bvh(mesh);
    geo::BvhClosest hit;
    if (bvh.empty() || !bvh.closestPoint(p, hit))
        return false;
    std::span<const glm::vec2> uvs = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    const Index* c = &bvh.triangles().corners[size_t(hit.triangle) * 3];
    for (int k = 0; k < 3; ++k)
    {
        out.points[k] = uint32_t(mesh.cornerPoint(c[k]));
        out.uv[k] = uvs.empty() ? glm::dvec2(0.0) : glm::dvec2(uvs[c[k]]);
    }
    out.bary = hit.bary;
    out.hasUv = !uvs.empty();
    return true;
}

glm::dmat4 surfaceFrame(std::span<const glm::dvec3> points, const SurfacePoint& sp)
{
    for (uint32_t p : sp.points)
        if (p >= points.size())
            return glm::dmat4(1.0);
    const glm::dvec3 &a = points[sp.points[0]], &b = points[sp.points[1]], &c = points[sp.points[2]];
    glm::dmat3 f = sp.hasUv ? uvFrame(a, b, c, sp.uv) : triangleFrame(a, b, c);
    glm::dmat4 m(f);
    m[3] = glm::dvec4(a * sp.bary.x + b * sp.bary.y + c * sp.bary.z, 1.0);
    return m;
}
} // namespace lila::deform
