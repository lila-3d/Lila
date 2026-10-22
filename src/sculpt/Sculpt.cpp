#include "sculpt/Sculpt.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace lila::sculpt
{
namespace
{
constexpr std::array<std::string_view, size_t(Brush::Count)> kBrushNames = {
    "draw", "clayStrips", "inflate", "grab", "smooth", "flatten", "pinch", "crease", "scrape", "mask", "relax", "freeze"};
constexpr std::array<std::string_view, size_t(Falloff::Count)> kFalloffNames = {"smooth", "sphere", "root", "sharp", "linear", "constant"};

// Per-dab step sizes as fractions of the radius at full strength; with dabs every tenth of a radius a stroke builds up
// a few tenths of a radius.
constexpr double kDrawStep = 0.03;
constexpr double kPinchStep = 0.15;
constexpr double kClayDepth = 0.12;

glm::dvec3 reflectVector(const geo::SymmetryPlane& plane, const glm::dvec3& v) { return v - 2.0 * glm::dot(v, plane.normal) * plane.normal; }

glm::dvec3 tangential(const glm::dvec3& v, const glm::dvec3& n) { return v - n * glm::dot(n, v); }

/** Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5). */
glm::dvec3 closestOnTriangle(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return a;
    const glm::dvec3 bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        return a + ab * (d1 / (d1 - d3));
    const glm::dvec3 cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

bool movesPoints(Brush b) { return b != Brush::Mask && b != Brush::Freeze; }
} // namespace

std::string_view brushName(Brush b) { return b < Brush::Count ? kBrushNames[size_t(b)] : std::string_view(); }

std::optional<Brush> brushFromName(std::string_view name)
{
    for (size_t i = 0; i < kBrushNames.size(); ++i)
        if (kBrushNames[i] == name)
            return Brush(i);
    return std::nullopt;
}

std::string_view falloffName(Falloff f) { return f < Falloff::Count ? kFalloffNames[size_t(f)] : std::string_view(); }

std::optional<Falloff> falloffFromName(std::string_view name)
{
    for (size_t i = 0; i < kFalloffNames.size(); ++i)
        if (kFalloffNames[i] == name)
            return Falloff(i);
    return std::nullopt;
}

double falloffWeight(Falloff f, double t)
{
    if (t >= 1.0)
        return 0.0;
    t = std::max(t, 0.0);
    switch (f)
    {
    case Falloff::Smooth: return 3.0 * (1.0 - t) * (1.0 - t) - 2.0 * (1.0 - t) * (1.0 - t) * (1.0 - t);
    case Falloff::Sphere: return std::sqrt(1.0 - t * t);
    case Falloff::Root: return std::sqrt(1.0 - t);
    case Falloff::Sharp: return (1.0 - t) * (1.0 - t);
    case Falloff::Linear: return 1.0 - t;
    case Falloff::Constant: return 1.0;
    case Falloff::Count: break;
    }
    return 0.0;
}

SculptMesh::SculptMesh(geo::Mesh mesh) : mMesh(std::move(mesh))
{
    LILA_ZONE();

    const Index n = mMesh.pointCapacity();
    mBvh.build(mMesh);
    // Builds the partial refit's tables now rather than on the first dab.
    mBvh.refitPoints(mMesh, {});
    mMask.assign(n, 0.0f);
    mFrozen.assign(n, 0);
    mNormals.assign(n, glm::vec3(0.0f, 1.0f, 0.0f));
    mStrokeStamp.assign(n, 0);
    mRecordIndex.assign(n, 0);
    mDabStamp.assign(n, 0);
    mGatherStamp.assign(n, 0);
    mDisp.assign(n, glm::dvec3(0.0));
    mMaskDelta.assign(n, 0.0f);
    mFreezeSet.assign(n, 0);

    mAdjOffsets.assign(size_t(n) + 1, 0);
    for (Index e = 0; e < mMesh.edgeCapacity(); ++e)
        if (mMesh.edgeValid(e))
        {
            ++mAdjOffsets[mMesh.edgePoint(e, 0) + 1];
            ++mAdjOffsets[mMesh.edgePoint(e, 1) + 1];
        }
    for (size_t i = 1; i < mAdjOffsets.size(); ++i)
        mAdjOffsets[i] += mAdjOffsets[i - 1];
    mAdj.resize(mAdjOffsets.back());
    std::vector<uint32_t> fill(mAdjOffsets.begin(), mAdjOffsets.end() - 1);
    for (Index e = 0; e < mMesh.edgeCapacity(); ++e)
        if (mMesh.edgeValid(e))
        {
            Index a = mMesh.edgePoint(e, 0), b = mMesh.edgePoint(e, 1);
            mAdj[fill[a]++] = b;
            mAdj[fill[b]++] = a;
        }

    const auto& corners = mBvh.triangles().corners;
    mTriOffsets.assign(size_t(n) + 1, 0);
    for (Index c : corners)
        ++mTriOffsets[mMesh.cornerPoint(c) + 1];
    for (size_t i = 1; i < mTriOffsets.size(); ++i)
        mTriOffsets[i] += mTriOffsets[i - 1];
    mTris.resize(mTriOffsets.back());
    fill.assign(mTriOffsets.begin(), mTriOffsets.end() - 1);
    for (size_t i = 0; i < corners.size(); ++i)
        mTris[fill[mMesh.cornerPoint(corners[i])]++] = Index(i / 3);

    std::vector<Index> all;
    all.reserve(n);
    for (Index p = 0; p < n; ++p)
        if (mMesh.pointValid(p))
            all.push_back(p);
    updateNormals(all);
}

void SculptMesh::setSymmetry(const std::optional<geo::SymmetryPlane>& plane, double tolerance)
{
    LILA_ZONE();

    if (!plane)
    {
        mSymmetry = {};
        return;
    }
    mSymmetry = geo::SymmetryMap::build(mMesh, *plane, tolerance);
    // Exactly mirrored positions keep mirrored strokes exact: reflecting an axis plane through the origin only flips a sign.
    std::vector<Index> moved;
    const geo::SymmetryPlane& pl = mSymmetry.plane();
    for (Index p = 0; p < mMesh.pointCapacity(); ++p)
    {
        if (!mMesh.pointValid(p))
            continue;
        Index q = mSymmetry.mirrorPoint(p);
        if (q == geo::kInvalid || q < p)
            continue;
        glm::dvec3 at = mMesh.position(p);
        if (q == p)
        {
            glm::dvec3 on = pl.project(at);
            if (on != at)
            {
                mMesh.setPosition(p, on);
                moved.push_back(p);
            }
            continue;
        }
        glm::dvec3 avg = (at + pl.reflect(mMesh.position(q))) * 0.5;
        glm::dvec3 mirrored = pl.reflect(avg);
        if (avg != at)
        {
            mMesh.setPosition(p, avg);
            moved.push_back(p);
        }
        if (mirrored != mMesh.position(q))
        {
            mMesh.setPosition(q, mirrored);
            moved.push_back(q);
        }
    }
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
}

bool SculptMesh::raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const
{
    return !mBvh.empty() && mBvh.raycast(origin, dir, hit);
}

glm::dvec3 SculptMesh::normalAt(const geo::BvhHit& hit) const
{
    const Index* c = &mBvh.triangles().corners[size_t(hit.triangle) * 3];
    glm::dvec3 n(0.0);
    for (int k = 0; k < 3; ++k)
        n += glm::dvec3(mNormals[mMesh.cornerPoint(c[k])]) * hit.bary[k];
    double len = glm::length(n);
    return len > 1e-12 ? n / len : glm::dvec3(0.0, 1.0, 0.0);
}

void SculptMesh::beginStroke(const BrushSettings& settings)
{
    mSettings = settings;
    mSettings.strength = std::clamp(settings.strength, 0.0, 1.0);
    mStroking = true;
    mGrab.clear();
    mRecord = {};
    if (++mStroke == 0)
    {
        std::fill(mStrokeStamp.begin(), mStrokeStamp.end(), 0u);
        mStroke = 1;
    }
}

double SculptMesh::pointWeight(Index p, double falloff) const
{
    const double w = falloff * mSettings.strength;
    if (!movesPoints(mSettings.brush))
        return w;
    if (mFrozen[p])
        return 0.0;
    return w * (1.0 - double(mMask[p]));
}

void SculptMesh::gather(Side& s, bool forGrab)
{
    LILA_ZONE();

    s.points.clear();
    s.weights.clear();
    const double r = s.dab.radius;
    if (r <= 0.0)
        return;
    geo::Bounds box;
    box.extend(s.dab.center - glm::dvec3(r));
    box.extend(s.dab.center + glm::dvec3(r));
    if (++mGatherId == 0)
    {
        std::fill(mGatherStamp.begin(), mGatherStamp.end(), 0u);
        mGatherId = 1;
    }
    const auto& corners = mBvh.triangles().corners;
    const bool front = mSettings.frontFacesOnly && glm::length(s.dab.viewDir) > 0.0;
    for (Index t : mBvh.overlapping(box))
        for (int k = 0; k < 3; ++k)
        {
            Index p = mMesh.cornerPoint(corners[size_t(t) * 3 + k]);
            if (mGatherStamp[p] == mGatherId)
                continue;
            mGatherStamp[p] = mGatherId;
            double d = glm::length(mMesh.position(p) - s.dab.center);
            if (d >= r)
                continue;
            if (front && !forGrab && glm::dot(glm::dvec3(mNormals[p]), s.dab.viewDir) > 0.0)
                continue;
            double f = falloffWeight(mSettings.falloff, d / r);
            if (f <= 0.0)
                continue;
            s.points.push_back(p);
            s.weights.push_back(f);
        }
}

void SculptMesh::computeArea(Side& s) const
{
    glm::dvec3 n(0.0), c(0.0);
    double total = 0.0;
    for (size_t i = 0; i < s.points.size(); ++i)
    {
        n += glm::dvec3(mNormals[s.points[i]]) * s.weights[i];
        c += mMesh.position(s.points[i]) * s.weights[i];
        total += s.weights[i];
    }
    double len = glm::length(n);
    s.areaNormal = len > 1e-12 ? n / len : s.dab.normal;
    s.areaCenter = total > 0.0 ? c / total : s.dab.center;
}

void SculptMesh::touch(Index p)
{
    if (mDabStamp[p] == mDabId)
        return;
    mDabStamp[p] = mDabId;
    mChanged.push_back(p);
    mDisp[p] = glm::dvec3(0.0);
    mMaskDelta[p] = 0.0f;
    mFreezeSet[p] = 0;
}

glm::dvec3 SculptMesh::originalPosition(Index p) const
{
    return mStroking && mStrokeStamp[p] == mStroke ? mRecord.before[mRecordIndex[p]] : mMesh.position(p);
}

glm::dvec3 SculptMesh::projectToOriginal(Index p, const glm::dvec3& at) const
{
    const auto& corners = mBvh.triangles().corners;
    glm::dvec3 best = at;
    double bestD = std::numeric_limits<double>::max();
    for (uint32_t k = mTriOffsets[p]; k < mTriOffsets[p + 1]; ++k)
    {
        const Index* c = &corners[size_t(mTris[k]) * 3];
        glm::dvec3 q = closestOnTriangle(at, originalPosition(mMesh.cornerPoint(c[0])), originalPosition(mMesh.cornerPoint(c[1])),
                                         originalPosition(mMesh.cornerPoint(c[2])));
        double d = glm::dot(q - at, q - at);
        if (d < bestD)
        {
            bestD = d;
            best = q;
        }
    }
    return best;
}

void SculptMesh::addDisplacement(const Side& s)
{
    LILA_ZONE();

    const size_t n = s.points.size();
    std::vector<glm::dvec3> out(n, glm::dvec3(0.0));
    const double r = s.dab.radius, sign = mSettings.invert ? -1.0 : 1.0, pressure = s.dab.pressure;
    const glm::dvec3 N = s.areaNormal, C = s.areaCenter;

    // Clay strips: a square footprint in the plane, lined up with the stroke.
    glm::dvec3 ax = tangential(s.dab.direction, N);
    if (glm::length(ax) < 1e-9)
        ax = tangential(std::abs(N.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0), N);
    ax = glm::normalize(ax);
    const glm::dvec3 ay = glm::cross(N, ax);
    const glm::dvec3 clayPlane = C + N * (r * kClayDepth * sign);
    const Brush brush = mSettings.brush;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 1024), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i < range.end(); ++i)
        {
            const Index p = s.points[i];
            const glm::dvec3 P = mMesh.position(p);
            double w = pointWeight(p, s.weights[i]) * pressure;
            if (w <= 0.0)
                continue;
            glm::dvec3 d(0.0);
            switch (brush)
            {
            case Brush::Draw: d = N * (w * r * kDrawStep * sign); break;
            case Brush::Inflate: d = glm::dvec3(mNormals[p]) * (w * r * kDrawStep * sign); break;
            case Brush::Smooth:
            case Brush::Relax:
            {
                auto nb = neighbours(p);
                if (nb.empty())
                    break;
                glm::dvec3 avg(0.0);
                for (Index q : nb)
                    avg += mMesh.position(q);
                avg /= double(nb.size());
                const double k = std::min(w, 1.0);
                if (brush == Brush::Smooth)
                    d = (avg - P) * k;
                else
                    d = projectToOriginal(p, P + tangential(avg - P, glm::dvec3(mNormals[p])) * k) - P;
                break;
            }
            case Brush::Flatten: d = -N * (glm::dot(P - C, N) * std::min(w, 1.0) * sign); break;
            case Brush::Scrape:
            {
                const double dist = glm::dot(P - C, N);
                if (dist * sign > 0.0)
                    d = -N * (dist * std::min(w, 1.0));
                break;
            }
            case Brush::ClayStrips:
            {
                const glm::dvec3 rel = P - C;
                const double t = std::max(std::abs(glm::dot(rel, ax)), std::abs(glm::dot(rel, ay))) / r;
                const double f = falloffWeight(mSettings.falloff, t) / std::max(s.weights[i], 1e-12);
                const double dist = glm::dot(P - clayPlane, N);
                if (dist * sign < 0.0)
                    d = -N * (dist * std::min(w * f, 1.0));
                break;
            }
            case Brush::Pinch: d = tangential(C - P, N) * (w * kPinchStep * sign); break;
            case Brush::Crease: d = tangential(C - P, N) * (w * kPinchStep) - N * (w * r * kDrawStep * sign); break;
            case Brush::Grab:
            case Brush::Mask:
            case Brush::Freeze:
            case Brush::Count: break;
            }
            out[i] = d;
        }
    });
    for (size_t i = 0; i < n; ++i)
        if (out[i] != glm::dvec3(0.0))
        {
            touch(s.points[i]);
            mDisp[s.points[i]] += out[i];
        }
}

void SculptMesh::addMaskChange(const Side& s)
{
    const double sign = mSettings.invert ? -1.0 : 1.0;
    for (size_t i = 0; i < s.points.size(); ++i)
    {
        const Index p = s.points[i];
        touch(p);
        if (mSettings.brush == Brush::Mask)
            mMaskDelta[p] += float(s.weights[i] * mSettings.strength * s.dab.pressure * sign);
        else
            mFreezeSet[p] = mSettings.invert ? -1 : 1;
    }
}

void SculptMesh::symmetrizeChanges()
{
    if (mSymmetry.empty())
        return;
    const size_t count = mChanged.size();
    for (size_t i = 0; i < count; ++i)
        if (Index q = mSymmetry.mirrorPoint(mChanged[i]); q != geo::kInvalid)
            touch(q);
    const geo::SymmetryPlane& pl = mSymmetry.plane();
    for (Index p : mChanged)
    {
        const Index q = mSymmetry.mirrorPoint(p);
        if (q == geo::kInvalid || q < p)
            continue;
        if (q == p)
        {
            mDisp[p] = tangential(mDisp[p], pl.normal);
            continue;
        }
        const glm::dvec3 avg = (mDisp[p] + reflectVector(pl, mDisp[q])) * 0.5;
        mDisp[p] = avg;
        mDisp[q] = reflectVector(pl, avg);
        const float m = (mMaskDelta[p] + mMaskDelta[q]) * 0.5f;
        mMaskDelta[p] = mMaskDelta[q] = m;
        const int8_t f = mFreezeSet[p] ? mFreezeSet[p] : mFreezeSet[q];
        mFreezeSet[p] = mFreezeSet[q] = f;
    }
}

void SculptMesh::commitChanges()
{
    LILA_ZONE();

    std::vector<Index> moved;
    bool maskChanged = false;
    for (Index p : mChanged)
    {
        const bool move = mDisp[p] != glm::dvec3(0.0);
        const float newMask = std::clamp(mMask[p] + mMaskDelta[p], 0.0f, 1.0f);
        const uint8_t newFrozen = mFreezeSet[p] > 0 ? 1 : mFreezeSet[p] < 0 ? 0 : mFrozen[p];
        if (!move && newMask == mMask[p] && newFrozen == mFrozen[p])
            continue;
        if (mStroking && mStrokeStamp[p] != mStroke)
        {
            mStrokeStamp[p] = mStroke;
            mRecordIndex[p] = uint32_t(mRecord.points.size());
            mRecord.points.push_back(p);
            mRecord.before.push_back(mMesh.position(p));
            mRecord.maskBefore.push_back(mMask[p]);
            mRecord.frozenBefore.push_back(mFrozen[p]);
        }
        if (move)
        {
            mMesh.setPosition(p, mMesh.position(p) + mDisp[p]);
            moved.push_back(p);
        }
        maskChanged |= newMask != mMask[p] || newFrozen != mFrozen[p];
        mMask[p] = newMask;
        mFrozen[p] = newFrozen;
    }
    mChanged.clear();
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
    if (maskChanged)
        ++mMaskVersion;
}

void SculptMesh::dab(const Dab& d)
{
    LILA_ZONE();

    if (!mStroking)
        beginStroke(mSettings);
    if (++mDabId == 0)
    {
        std::fill(mDabStamp.begin(), mDabStamp.end(), 0u);
        mDabId = 1;
    }
    mChanged.clear();

    std::vector<Side> sides(1);
    sides[0].dab = d;
    sides[0].dab.normal = glm::length(d.normal) > 0.0 ? glm::normalize(d.normal) : glm::dvec3(0.0, 1.0, 0.0);
    if (!mSymmetry.empty())
    {
        const geo::SymmetryPlane& pl = mSymmetry.plane();
        Side m;
        m.dab = sides[0].dab;
        m.dab.center = pl.reflect(d.center);
        m.dab.normal = reflectVector(pl, sides[0].dab.normal);
        m.dab.viewDir = reflectVector(pl, d.viewDir);
        m.dab.delta = reflectVector(pl, d.delta);
        m.dab.direction = reflectVector(pl, d.direction);
        sides.push_back(std::move(m));
    }

    if (mSettings.brush == Brush::Grab)
    {
        if (mGrab.empty())
        {
            mGrab = sides;
            for (Side& s : mGrab)
                gather(s, true);
        }
        for (size_t k = 0; k < mGrab.size() && k < sides.size(); ++k)
        {
            const Side& g = mGrab[k];
            const glm::dvec3 delta = sides[k].dab.delta;
            for (size_t i = 0; i < g.points.size(); ++i)
            {
                const Index p = g.points[i];
                double w = mFrozen[p] ? 0.0 : g.weights[i] * (1.0 - double(mMask[p])) * d.pressure;
                if (w <= 0.0)
                    continue;
                touch(p);
                mDisp[p] += delta * w;
            }
        }
    }
    else
        for (Side& s : sides)
        {
            gather(s, false);
            if (movesPoints(mSettings.brush))
            {
                computeArea(s);
                addDisplacement(s);
            }
            else
                addMaskChange(s);
        }
    symmetrizeChanges();
    commitChanges();
}

StrokeRecord SculptMesh::endStroke()
{
    mStroking = false;
    mGrab.clear();
    StrokeRecord r = std::move(mRecord);
    mRecord = {};
    r.after.resize(r.points.size());
    r.maskAfter.resize(r.points.size());
    r.frozenAfter.resize(r.points.size());
    for (size_t i = 0; i < r.points.size(); ++i)
    {
        r.after[i] = mMesh.position(r.points[i]);
        r.maskAfter[i] = mMask[r.points[i]];
        r.frozenAfter[i] = mFrozen[r.points[i]];
    }
    if (++mStroke == 0)
    {
        std::fill(mStrokeStamp.begin(), mStrokeStamp.end(), 0u);
        mStroke = 1;
    }
    return r;
}

void SculptMesh::apply(const StrokeRecord& r, bool undo)
{
    LILA_ZONE();

    const auto& pos = undo ? r.before : r.after;
    const auto& mask = undo ? r.maskBefore : r.maskAfter;
    const auto& frozen = undo ? r.frozenBefore : r.frozenAfter;
    std::vector<Index> moved;
    bool maskChanged = false;
    for (size_t i = 0; i < r.points.size(); ++i)
    {
        const Index p = r.points[i];
        if (p >= mMesh.pointCapacity())
            continue;
        if (i < pos.size() && pos[i] != mMesh.position(p))
        {
            mMesh.setPosition(p, pos[i]);
            moved.push_back(p);
        }
        if (i < mask.size() && mask[i] != mMask[p])
        {
            mMask[p] = mask[i];
            maskChanged = true;
        }
        if (i < frozen.size() && frozen[i] != mFrozen[p])
        {
            mFrozen[p] = frozen[i];
            maskChanged = true;
        }
    }
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
    if (maskChanged)
        ++mMaskVersion;
}

StrokeRecord SculptMesh::maskOp(MaskOp op, Layer layer, int iterations)
{
    LILA_ZONE();

    const Index n = mMesh.pointCapacity();
    std::vector<float> next(mMask);
    std::vector<uint8_t> nextFrozen(mFrozen);
    if (layer == Layer::Freeze)
    {
        for (Index p = 0; p < n; ++p)
            switch (op)
            {
            case MaskOp::Clear: nextFrozen[p] = 0; break;
            case MaskOp::Fill: nextFrozen[p] = 1; break;
            case MaskOp::Invert: nextFrozen[p] = mFrozen[p] ? 0 : 1; break;
            case MaskOp::Blur: break;
            }
    }
    else if (op == MaskOp::Blur)
    {
        std::vector<float> cur(mMask);
        for (int it = 0; it < std::max(1, iterations); ++it)
        {
            tbb::parallel_for(Index(0), n, [&](Index p) {
                auto nb = neighbours(p);
                if (nb.empty())
                    return;
                float sum = cur[p];
                for (Index q : nb)
                    sum += cur[q];
                next[p] = sum / float(nb.size() + 1);
            });
            cur = next;
        }
    }
    else
        for (Index p = 0; p < n; ++p)
            next[p] = op == MaskOp::Clear ? 0.0f : op == MaskOp::Fill ? 1.0f : 1.0f - mMask[p];

    StrokeRecord r;
    for (Index p = 0; p < n; ++p)
        if (mMesh.pointValid(p) && (next[p] != mMask[p] || nextFrozen[p] != mFrozen[p]))
        {
            r.points.push_back(p);
            r.maskBefore.push_back(mMask[p]);
            r.maskAfter.push_back(next[p]);
            r.frozenBefore.push_back(mFrozen[p]);
            r.frozenAfter.push_back(nextFrozen[p]);
            mMask[p] = next[p];
            mFrozen[p] = nextFrozen[p];
        }
    if (!r.empty())
        ++mMaskVersion;
    return r;
}

void SculptMesh::setPositions(std::span<const glm::dvec3> positions)
{
    std::vector<Index> moved;
    for (Index p = 0; p < mMesh.pointCapacity() && p < positions.size(); ++p)
        if (mMesh.pointValid(p) && positions[p] != mMesh.position(p))
        {
            mMesh.setPosition(p, positions[p]);
            moved.push_back(p);
        }
    if (!moved.empty())
    {
        refresh(moved);
        ++mVersion;
    }
}

void SculptMesh::setMask(std::span<const float> mask)
{
    for (Index p = 0; p < mMask.size() && p < mask.size(); ++p)
        mMask[p] = std::clamp(mask[p], 0.0f, 1.0f);
    ++mMaskVersion;
}

void SculptMesh::refresh(std::span<const Index> moved)
{
    LILA_ZONE();

    mBvh.refitPoints(mMesh, moved);
    // Normals change one ring out from every moved point.
    if (++mGatherId == 0)
    {
        std::fill(mGatherStamp.begin(), mGatherStamp.end(), 0u);
        mGatherId = 1;
    }
    std::vector<Index> ring;
    ring.reserve(moved.size() * 2);
    for (Index p : moved)
    {
        if (mGatherStamp[p] != mGatherId)
        {
            mGatherStamp[p] = mGatherId;
            ring.push_back(p);
        }
        for (Index q : neighbours(p))
            if (mGatherStamp[q] != mGatherId)
            {
                mGatherStamp[q] = mGatherId;
                ring.push_back(q);
            }
    }
    updateNormals(ring);
    writeVertices(ring);
}

void SculptMesh::updateNormals(std::span<const Index> points)
{
    LILA_ZONE();

    const auto& corners = mBvh.triangles().corners;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size(), 2048), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            const Index p = points[i];
            glm::dvec3 n(0.0);
            for (uint32_t k = mTriOffsets[p]; k < mTriOffsets[p + 1]; ++k)
            {
                const Index* c = &corners[size_t(mTris[k]) * 3];
                const glm::dvec3 a = mMesh.position(mMesh.cornerPoint(c[0]));
                n += glm::cross(mMesh.position(mMesh.cornerPoint(c[1])) - a, mMesh.position(mMesh.cornerPoint(c[2])) - a);
            }
            const double len = glm::length(n);
            if (len > 1e-300)
                mNormals[p] = glm::vec3(n / len);
        }
    });
}

void SculptMesh::writeVertices(std::span<const Index> points)
{
    if (!mRender)
        return;
    render::DynamicVertices& dv = *mRender->dynamic;
    std::scoped_lock lock(dv.mutex);
    for (Index p : points)
    {
        render::MeshVertex v{};
        v.pos = glm::vec3(mMesh.position(p));
        v.normal = mNormals[p];
        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        dv.write(p, v);
    }
}

std::shared_ptr<const render::RenderMesh> SculptMesh::renderMesh()
{
    if (mRender)
        return mRender;
    LILA_ZONE();

    auto rm = std::make_shared<render::RenderMesh>();
    rm->dynamic = std::make_shared<render::DynamicVertices>();
    const Index n = mMesh.pointCapacity();
    auto& verts = rm->dynamic->vertices;
    verts.resize(n);
    tbb::parallel_for(Index(0), n, [&](Index p) {
        render::MeshVertex& v = verts[p];
        v.pos = glm::vec3(mMesh.position(p));
        v.normal = mNormals[p];
        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        v.uv = v.uv1 = glm::vec2(0.0f);
    });
    const auto& corners = mBvh.triangles().corners;
    rm->indices.resize(corners.size());
    tbb::parallel_for(size_t(0), corners.size(), [&](size_t i) { rm->indices[i] = uint32_t(mMesh.cornerPoint(corners[i])); });
    // Strokes may push the surface past its starting bounds, so leave room.
    geo::Bounds b = mMesh.bounds();
    glm::dvec3 pad = b.size() * 0.25 + glm::dvec3(1e-3);
    rm->boundsMin = glm::vec3(b.min - pad);
    rm->boundsMax = glm::vec3(b.max + pad);
    mRender = rm;
    return mRender;
}
} // namespace lila::sculpt
