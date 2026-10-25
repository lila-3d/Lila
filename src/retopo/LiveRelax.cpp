#include "retopo/LiveRelax.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

namespace lila::retopo
{
namespace
{
/** A border point turning more sharply than this is a corner and stays put when borders slide. */
constexpr double kCornerCos = 0.766; // 40 degrees
/** How many border steps away a sliding point may still find a segment to land on. */
constexpr int kSlideReach = 4;
/** Halvings of a point's step before a fold makes it stay put. */
constexpr int kHalvings = 4;
/** How far a target's pull reaches the points around it, in target lengths. */
constexpr double kDragReach = 3.0;
/** The most a target pulls any point in one step, in target lengths. */
constexpr double kDragStep = 0.5;

/** weight of the way from `from` to `to`, at most kDragStep target lengths. */
glm::dvec3 targetPull(const glm::dvec3& from, const glm::dvec3& to, double weight, double length)
{
    glm::dvec3 pull = (to - from) * weight;
    const double pl = glm::length(pull);
    return pl > kDragStep * length ? pull * (kDragStep * length / pl) : pull;
}

glm::dvec3 closestOnSegments(const glm::dvec3& x, std::span<const std::array<glm::dvec3, 2>> segs)
{
    glm::dvec3 best = x;
    double bestD = std::numeric_limits<double>::max();
    for (const auto& [a, b] : segs)
    {
        const glm::dvec3 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(x - a, ab) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec3 q = a + ab * t;
        const double d = glm::dot(x - q, x - q);
        if (d < bestD)
        {
            bestD = d;
            best = q;
        }
    }
    return best;
}

/** Newell area vector of a run of points: twice the area along the normal. */
glm::dvec3 areaVector(std::span<const Index> pts, const std::vector<glm::dvec3>& x)
{
    glm::dvec3 a(0.0);
    const glm::dvec3 o = x[pts[0]];
    for (size_t i = 1; i + 1 < pts.size(); ++i)
        a += glm::cross(x[pts[i]] - o, x[pts[i + 1]] - o);
    return a;
}

/** Unit vector perpendicular to n, along hint where it can be. */
glm::dvec3 tangent(const glm::dvec3& n, const glm::dvec3& hint)
{
    glm::dvec3 t = hint - n * glm::dot(n, hint);
    double len = glm::length(t);
    if (len <= 1e-300)
    {
        t = glm::cross(n, std::abs(n.x) < 0.9 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0));
        len = glm::length(t);
    }
    return t / len;
}

struct CellKey
{
    int64_t x, y, z;
    bool operator==(const CellKey&) const = default;
};

struct CellHash
{
    size_t operator()(const CellKey& k) const
    {
        return size_t(k.x * 73856093) ^ size_t(k.y * 19349663) ^ size_t(k.z * 83492791);
    }
};
} // namespace

LiveRelax::LiveRelax(const geo::Mesh& mesh, const geo::MeshBvh* surface, const LiveRelaxOptions& opts)
    : mSurface(surface && !surface->empty() ? surface : nullptr), mOpts(opts)
{
    LILA_ZONE();

    const Index n = mesh.pointCapacity();
    mX.assign(mesh.positions().begin(), mesh.positions().end());
    mValid.assign(n, 0);
    for (Index p = 0; p < n; ++p)
        mValid[p] = mesh.pointValid(p);
    mBoundary.assign(n, 0);
    mPinned.assign(n, 0);
    mOnPlane.assign(n, 0);
    mMirror.assign(n, geo::kInvalid);
    mTarget.assign(n, glm::dvec3(0.0));
    mTargetW.assign(n, 0.0);
    mRaw.assign(n, glm::dvec3(0.0));
    mNext.assign(n, glm::dvec3(0.0));
    mCut.assign(n, 1.0);
    mDrag.assign(n, glm::dvec3(0.0));

    mFaceStart.push_back(0);
    std::vector<Index> count(n, 0);
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f) || mesh.faceSize(f) < 3)
            continue;
        const Index face = Index(mFaceStart.size() - 1);
        mesh.forEachFaceCorner(f, [&](Index c) {
            mFacePts.push_back(mesh.cornerPoint(c));
            mSlotFace.push_back(face);
            ++count[mesh.cornerPoint(c)];
        });
        mFaceStart.push_back(Index(mFacePts.size()));
    }
    mGoal.assign(mFacePts.size(), glm::dvec3(0.0));
    mArea.assign(mFaceStart.size() - 1, glm::dvec3(0.0));
    mPtStart.assign(n + 1, 0);
    for (Index p = 0; p < n; ++p)
        mPtStart[p + 1] = mPtStart[p] + count[p];
    mPtSlot.resize(mFacePts.size());
    std::fill(count.begin(), count.end(), 0);
    for (Index s = 0; s < Index(mFacePts.size()); ++s)
    {
        const Index p = mFacePts[s];
        mPtSlot[mPtStart[p] + count[p]++] = s;
    }

    std::vector<std::vector<Index>> nb(n), border(n);
    std::vector<uint8_t> nonManifold(n, 0);
    double lengthSum = 0.0;
    Index edges = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e))
            continue;
        const Index a = mesh.edgePoint(e, 0), b = mesh.edgePoint(e, 1);
        nb[a].push_back(b);
        nb[b].push_back(a);
        lengthSum += glm::length(mX[a] - mX[b]);
        ++edges;
        const Index faces = mesh.edgeFaceCount(e);
        if (faces != 2)
            mBoundary[a] = mBoundary[b] = 1;
        if (faces == 1)
        {
            border[a].push_back(b);
            border[b].push_back(a);
        }
        else if (faces > 2)
            nonManifold[a] = nonManifold[b] = 1;
    }
    mMeanLength = edges ? lengthSum / double(edges) : 1.0;
    mLength = mOpts.targetLength > 0.0 ? mOpts.targetLength : mMeanLength;
    mNbStart.assign(n + 1, 0);
    for (Index p = 0; p < n; ++p)
    {
        mNb.insert(mNb.end(), nb[p].begin(), nb[p].end());
        mNbStart[p + 1] = Index(mNb.size());
    }

    mSegStart.assign(n + 1, 0);
    for (Index p = 0; p < n; ++p)
    {
        bool line = border[p].size() == 2 && !nonManifold[p];
        if (line)
        {
            const glm::dvec3 a = mX[p] - mX[border[p][0]], b = mX[border[p][1]] - mX[p];
            const double la = glm::length(a), lb = glm::length(b);
            line = la > 0.0 && lb > 0.0 && glm::dot(a, b) >= kCornerCos * la * lb;
        }
        if (line)
        {
            std::vector<Index> near = {p};
            std::vector<int> depth = {0};
            for (size_t k = 0; k < near.size(); ++k)
                if (depth[k] < kSlideReach)
                    for (Index q : border[near[k]])
                        if (std::find(near.begin(), near.end(), q) == near.end())
                        {
                            near.push_back(q);
                            depth.push_back(depth[k] + 1);
                        }
            for (Index a : near)
                for (Index b : border[a])
                    if (a < b)
                        mSegs.push_back({mX[a], mX[b]});
        }
        mSegStart[p + 1] = Index(mSegs.size());
    }
    buildSymmetry();
}

void LiveRelax::buildSymmetry()
{
    LILA_ZONE();

    const Index n = Index(mX.size());
    std::fill(mOnPlane.begin(), mOnPlane.end(), 0);
    std::fill(mMirror.begin(), mMirror.end(), geo::kInvalid);
    if (!mOpts.symmetry)
        return;
    const geo::SymmetryPlane& plane = *mOpts.symmetry;
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (Index p = 0; p < n; ++p)
        if (mValid[p])
        {
            lo = glm::min(lo, mX[p]);
            hi = glm::max(hi, mX[p]);
        }
    if (lo.x > hi.x)
        return;
    const double tol = std::max(1e-4 * glm::length(hi - lo), 1e-9);
    auto cell = [&](const glm::dvec3& x) {
        const glm::dvec3 c = glm::floor((x - lo) / tol);
        return CellKey{int64_t(c.x), int64_t(c.y), int64_t(c.z)};
    };
    std::unordered_map<CellKey, std::vector<Index>, CellHash> grid;
    for (Index p = 0; p < n; ++p)
        if (mValid[p])
            grid[cell(mX[p])].push_back(p);
    for (Index p = 0; p < n; ++p)
    {
        if (!mValid[p])
            continue;
        if (std::abs(plane.distance(mX[p])) <= tol)
        {
            mOnPlane[p] = 1;
            continue;
        }
        const glm::dvec3 r = plane.reflect(mX[p]);
        const CellKey c = cell(r);
        Index best = geo::kInvalid;
        double bestD = tol * tol;
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                {
                    auto it = grid.find({c.x + dx, c.y + dy, c.z + dz});
                    if (it == grid.end())
                        continue;
                    for (Index q : it->second)
                    {
                        const double d = glm::dot(mX[q] - r, mX[q] - r);
                        if (q != p && (d < bestD || (d == bestD && q < best)))
                        {
                            bestD = d;
                            best = q;
                        }
                    }
                }
        mMirror[p] = best;
    }
    // Only mutual matches pair up, so every partner points back.
    for (Index p = 0; p < n; ++p)
        if (mMirror[p] != geo::kInvalid && mMirror[mMirror[p]] != p)
            mMirror[p] = geo::kInvalid;
}

void LiveRelax::setOptions(const LiveRelaxOptions& opts)
{
    const bool symmetryChanged = opts.symmetry.has_value() != mOpts.symmetry.has_value()
                              || (opts.symmetry && (opts.symmetry->normal != mOpts.symmetry->normal
                                                    || opts.symmetry->offset != mOpts.symmetry->offset));
    mOpts = opts;
    mLength = mOpts.targetLength > 0.0 ? mOpts.targetLength : mMeanLength;
    if (symmetryChanged)
        buildSymmetry();
    mSettled = false;
}

void LiveRelax::setPosition(Index p, const glm::dvec3& at)
{
    if (p >= mX.size() || !mValid[p])
        return;
    mX[p] = at;
    mSettled = false;
}

void LiveRelax::setPinned(Index p, bool on)
{
    if (p < mPinned.size())
        mPinned[p] = on;
    mSettled = false;
}

void LiveRelax::setMobility(std::vector<double> mobility)
{
    mMobility = std::move(mobility);
    mSettled = false;
}

void LiveRelax::setTarget(Index p, const glm::dvec3& at, double weight)
{
    if (p >= mX.size())
        return;
    mTarget[p] = at;
    mTargetW[p] = std::clamp(weight, 0.0, 1.0);
    auto it = std::find_if(mDrags.begin(), mDrags.end(), [&](const Drag& d) { return d.point == p; });
    if (mTargetW[p] <= 0.0 && it != mDrags.end())
        mDrags.erase(it);
    else if (mTargetW[p] > 0.0 && it == mDrags.end() && mValid[p] && !mPinned[p])
    {
        if (mDrags.empty())
            mStartX = mX;
        mDrags.push_back({p});
    }
    mSettled = false;
}

void LiveRelax::clearTargets()
{
    std::fill(mTargetW.begin(), mTargetW.end(), 0.0);
    mDrags.clear();
    mSettled = false;
}

bool LiveRelax::slides(Index p) const
{
    return !mOpts.pinBoundary && mSegStart[p + 1] > mSegStart[p];
}

double LiveRelax::mobility(Index p) const
{
    if (mPinned[p] || (mBoundary[p] && !slides(p)))
        return 0.0;
    return mMobility.empty() ? 1.0 : p < mMobility.size() ? std::clamp(mMobility[p], 0.0, 1.0) : 0.0;
}

glm::dvec3 LiveRelax::place(Index p, const glm::dvec3& at) const
{
    glm::dvec3 x = at;
    if (slides(p))
        x = closestOnSegments(x, std::span(mSegs).subspan(mSegStart[p], mSegStart[p + 1] - mSegStart[p]));
    geo::BvhClosest c;
    if (mOpts.project && mSurface && mSurface->closestPoint(x, c))
        x = c.point;
    return x;
}

void LiveRelax::symmetrize(Index p, std::vector<glm::dvec3>& x) const
{
    if (!mOpts.symmetry || mPinned[p])
        return;
    const geo::SymmetryPlane& plane = *mOpts.symmetry;
    if (mOnPlane[p])
    {
        x[p] = plane.project(x[p]);
        return;
    }
    const Index q = mMirror[p];
    if (q == geo::kInvalid)
        return;
    // The partner that is held (pinned, then dragged) drives; otherwise the one on the positive side drives the average.
    auto rank = [&](Index i) { return mPinned[i] ? 2 : mTargetW[i] > 0.0 ? 1 : 0; };
    const int rp = rank(p), rq = rank(q);
    if (rq > rp)
        x[p] = plane.reflect(x[q]);
    else if (rp > rq)
        x[q] = plane.reflect(x[p]);
    else
    {
        const Index a = plane.distance(mX[p]) >= 0.0 ? p : q, b = a == p ? q : p;
        x[a] = 0.5 * (x[a] + plane.reflect(x[b]));
        x[b] = plane.reflect(x[a]);
    }
}

bool LiveRelax::folded(Index f, const std::vector<glm::dvec3>& x) const
{
    const std::span<const Index> pts(mFacePts.data() + mFaceStart[f], mFaceStart[f + 1] - mFaceStart[f]);
    const glm::dvec3& before = mArea[f];
    const double len2 = glm::dot(before, before);
    if (len2 <= 0.0)
        return false;
    if (glm::dot(areaVector(pts, x), before) < 0.05 * len2)
        return true;
    if (pts.size() == 3)
        return false;
    const size_t k = pts.size();
    for (size_t i = 0; i < k; ++i)
    {
        const Index a = pts[(i + k - 1) % k], b = pts[i], c = pts[(i + 1) % k];
        const double was = glm::dot(glm::cross(mX[c] - mX[b], mX[a] - mX[b]), before);
        if (was > 0.0 && glm::dot(glm::cross(x[c] - x[b], x[a] - x[b]), before) <= 0.0)
            return true;
    }
    return false;
}

glm::dvec3 LiveRelax::spacingGoal(Index p, double evenSize) const
{
    const Index i0 = mNbStart[p], i1 = mNbStart[p + 1];
    if (i0 == i1)
        return mX[p];
    double own = 0.0;
    for (Index i = i0; i < i1; ++i)
        own += glm::length(mX[mNb[i]] - mX[p]);
    const double rest = (1.0 - evenSize) * own / double(i1 - i0) + evenSize * mLength;
    glm::dvec3 sum(0.0);
    for (Index i = i0; i < i1; ++i)
    {
        const glm::dvec3 q = mX[mNb[i]], v = mX[p] - q;
        const double l = glm::length(v);
        sum += l > 1e-12 * mLength ? q + v * (rest / l) : q;
    }
    return sum / double(i1 - i0);
}

void LiveRelax::spreadDrag()
{
    LILA_ZONE();

    for (Index p : mDragged)
        mDrag[p] = glm::dvec3(0.0);
    mDragged.clear();
    if (mDrags.empty())
        return;
    if (mDist.size() != mX.size())
        mDist.assign(mX.size(), std::numeric_limits<double>::max());
    std::vector<Index> reached;
    using Entry = std::pair<double, Index>;
    for (const Drag& drag : mDrags)
    {
        const double tw = mTargetW[drag.point];
        const glm::dvec3 delta = mTarget[drag.point] - mStartX[drag.point];
        // Wide enough that the falloff never carries a point past one farther in.
        const double reach = std::max(kDragReach * mLength, 2.5 * glm::length(delta));
        // Dijkstra over the edges as they were when the drag began; ties pop by index so the spread is deterministic.
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
        reached.assign(1, drag.point);
        mDist[drag.point] = 0.0;
        open.push({0.0, drag.point});
        while (!open.empty())
        {
            const auto [d, q] = open.top();
            open.pop();
            if (mDist[q] < d)
                continue;
            for (Index i = mNbStart[q]; i < mNbStart[q + 1]; ++i)
            {
                const Index r = mNb[i];
                const double dr = d + glm::length(mStartX[r] - mStartX[q]);
                if (dr >= reach || dr >= mDist[r])
                    continue;
                if (mDist[r] == std::numeric_limits<double>::max())
                    reached.push_back(r);
                mDist[r] = dr;
                open.push({dr, r});
            }
        }
        for (Index q : reached)
        {
            const double t = mDist[q] / reach;
            mDist[q] = std::numeric_limits<double>::max();
            if (q == drag.point)
                continue;
            const double w = (1.0 - t * t) * (1.0 - t * t);
            mDrag[q] += targetPull(mX[q], mStartX[q] + delta * w, tw * w, mLength);
            mDragged.push_back(q);
        }
    }
    std::sort(mDragged.begin(), mDragged.end());
    mDragged.erase(std::unique(mDragged.begin(), mDragged.end()), mDragged.end());
    if (!mOpts.symmetry)
        return;
    // The mirror side gets the reflected pull, so neither side is held back by the other's average.
    const glm::dvec3 nn = mOpts.symmetry->normal;
    const size_t touched = mDragged.size();
    for (size_t i = 0; i < touched; ++i)
    {
        const Index q = mDragged[i], m = mMirror[q];
        if (m == geo::kInvalid || std::binary_search(mDragged.begin(), mDragged.begin() + touched, m))
            continue;
        mDrag[m] = mDrag[q] - 2.0 * glm::dot(mDrag[q], nn) * nn;
        mDragged.push_back(m);
    }
}

bool LiveRelax::shrinks(Index f, Index slot) const
{
    const Index s0 = mFaceStart[f], k = mFaceStart[f + 1] - s0, i = slot - s0;
    const Index a = mFacePts[s0 + (i + k - 1) % k], b = mFacePts[slot], c = mFacePts[s0 + (i + 1) % k];
    const glm::dvec3 was = glm::cross(mX[c] - mX[b], mX[a] - mX[b]);
    const glm::dvec3 now = glm::cross(mX[c] - mNext[b], mX[a] - mNext[b]);
    return glm::dot(now - was, mArea[f]) < 0.0;
}

double LiveRelax::step()
{
    LILA_ZONE();

    const Index n = Index(mX.size());
    const Index faces = Index(mFaceStart.size() - 1);
    if (n == 0)
    {
        mSettled = true;
        return 0.0;
    }
    const double evenSize = std::clamp(mOpts.evenSize, 0.0, 1.0);
    const double square = std::clamp(mOpts.squareness, 0.0, 1.0);
    const double stiffness = std::clamp(mOpts.stiffness, 0.0, 1.0);

    tbb::parallel_for(Index(0), faces, [&](Index f) {
        const Index s0 = mFaceStart[f];
        const Index k = mFaceStart[f + 1] - s0;
        const std::span<const Index> pts(mFacePts.data() + s0, k);
        const glm::dvec3 area = areaVector(pts, mX);
        mArea[f] = area;
        glm::dvec3 c(0.0);
        double perimeter = 0.0;
        for (Index i = 0; i < k; ++i)
        {
            c += mX[pts[i]];
            perimeter += glm::length(mX[pts[(i + 1) % k]] - mX[pts[i]]);
        }
        c /= double(k);
        const double alen = glm::length(area);
        if (alen <= 1e-300 || perimeter <= 0.0)
        {
            for (Index i = 0; i < k; ++i)
                mGoal[s0 + i] = mX[pts[i]];
            return;
        }
        const glm::dvec3 nn = area / alen;
        const glm::dvec3 u = tangent(nn, mX[pts[1]] - mX[pts[0]]);
        const glm::dvec3 v = glm::cross(nn, u);
        const double step = glm::two_pi<double>() / double(k);
        double dotSum = 0.0, crossSum = 0.0;
        for (Index i = 0; i < k; ++i)
        {
            const glm::dvec3 d = mX[pts[i]] - c;
            const double px = glm::dot(d, u), py = glm::dot(d, v);
            const double tx = std::cos(step * double(i)), ty = std::sin(step * double(i));
            dotSum += tx * px + ty * py;
            crossSum += tx * py - ty * px;
        }
        const double rl = std::hypot(dotSum, crossSum);
        const double cs = rl > 0.0 ? dotSum / rl : 1.0, sn = rl > 0.0 ? crossSum / rl : 0.0;
        const double side = (1.0 - evenSize) * perimeter / double(k) + evenSize * mLength;
        const double radius = side / (2.0 * std::sin(glm::pi<double>() / double(k)));
        for (Index i = 0; i < k; ++i)
        {
            const double tx = std::cos(step * double(i)), ty = std::sin(step * double(i));
            mGoal[s0 + i] = c + radius * ((cs * tx - sn * ty) * u + (sn * tx + cs * ty) * v);
        }
    });

    spreadDrag();
    tbb::parallel_for(Index(0), n, [&](Index p) {
        mCut[p] = 1.0;
        mRaw[p] = mX[p];
        const Index s0 = mPtStart[p], s1 = mPtStart[p + 1];
        if (!mValid[p] || mPinned[p] || s0 == s1)
            return;
        const double mob = mobility(p);
        const double tw = mTargetW[p];
        if (mob <= 0.0 && tw <= 0.0)
            return;
        const glm::dvec3 x = mX[p];
        glm::dvec3 shape(0.0), normal(0.0);
        for (Index s = s0; s < s1; ++s)
        {
            shape += mGoal[mPtSlot[s]];
            const glm::dvec3& a = mArea[mSlotFace[mPtSlot[s]]];
            const double la = glm::length(a);
            if (la > 0.0)
                normal += a / la;
        }
        shape /= double(s1 - s0);
        glm::dvec3 d = (1.0 - square) * spacingGoal(p, evenSize) + square * shape - x;
        const double nl = glm::length(normal);
        if (nl > 0.0)
        {
            const glm::dvec3 nn = normal / nl;
            d -= nn * glm::dot(nn, d);
        }
        d *= stiffness * mob;
        const double dl = glm::length(d);
        if (dl > mLength)
            d *= mLength / dl;
        glm::dvec3 at = x + d + mDrag[p] * mob;
        if (tw > 0.0)
            at += targetPull(at, mTarget[p], tw, mLength);
        mRaw[p] = place(p, at);
    });

    mNext = mRaw;
    for (Index p = 0; p < n; ++p)
        if (mValid[p])
            symmetrize(p, mNext);

    std::vector<uint8_t> bad(faces, 0);
    std::vector<Index> cut;
    std::vector<double> cutTo;
    bool clean = false;
    for (int pass = 0; pass < 64 && !clean; ++pass)
    {
        tbb::parallel_for(Index(0), faces, [&](Index f) { bad[f] = folded(f, mNext); });
        cut.clear();
        for (Index f = 0; f < faces; ++f)
        {
            if (!bad[f])
                continue;
            // Shorten the points whose own move shrinks the face; the others are making way and keep going.
            const size_t first = cut.size();
            for (int pickAll = 0; pickAll < 2 && cut.size() == first; ++pickAll)
                for (Index s = mFaceStart[f]; s < mFaceStart[f + 1]; ++s)
                {
                    const Index p = mFacePts[s];
                    if (mCut[p] > 0.0 && mNext[p] != mX[p] && (pickAll || shrinks(f, s)))
                    {
                        cut.push_back(p);
                        if (mOpts.symmetry && mMirror[p] != geo::kInvalid)
                            cut.push_back(mMirror[p]);
                    }
                }
        }
        clean = cut.empty();
        std::sort(cut.begin(), cut.end());
        cut.erase(std::unique(cut.begin(), cut.end()), cut.end());
        // Partners shorten together so the pair stays mirrored.
        cutTo.resize(cut.size());
        for (size_t i = 0; i < cut.size(); ++i)
        {
            const Index p = cut[i], m = mOpts.symmetry ? mMirror[p] : geo::kInvalid;
            const double t = m != geo::kInvalid ? std::min(mCut[p], mCut[m]) : mCut[p];
            cutTo[i] = pass < kHalvings ? 0.5 * t : 0.0;
        }
        for (size_t i = 0; i < cut.size(); ++i)
        {
            const Index p = cut[i];
            mCut[p] = cutTo[i];
            mNext[p] = mCut[p] > 0.0 ? place(p, mX[p] + (mRaw[p] - mX[p]) * mCut[p]) : mX[p];
        }
        for (Index p : cut)
            if (mCut[p] > 0.0)
                symmetrize(p, mNext);
    }

    if (!clean)
        mNext = mX;

    double worst = 0.0;
    for (Index p = 0; p < n; ++p)
    {
        if (!mValid[p])
            continue;
        worst = std::max(worst, glm::length(mNext[p] - mX[p]));
        mX[p] = mNext[p];
    }
    worst /= std::max(mLength, 1e-300);
    mSettled = worst < mOpts.tolerance;
    return worst;
}

int LiveRelax::solve(int maxSteps)
{
    LILA_ZONE();

    int steps = 0;
    while (steps < maxSteps)
    {
        step();
        ++steps;
        if (mSettled)
            break;
    }
    return steps;
}

void settleMesh(geo::Mesh& mesh, const geo::MeshBvh* surface, const LiveRelaxOptions& opts, int maxSteps,
                std::span<const uint8_t> active)
{
    LILA_ZONE();

    LiveRelax relax(mesh, surface, opts);
    if (!active.empty())
        for (Index p = 0; p < mesh.pointCapacity(); ++p)
            if (p >= active.size() || !active[p])
                relax.setPinned(p, true);
    relax.solve(maxSteps);
    std::span<const glm::dvec3> x = relax.positions();
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && mesh.position(p) != x[p])
            mesh.setPosition(p, x[p]);
}
} // namespace lila::retopo
