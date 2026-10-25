#include "curve/BevelPlus.h"

#include "core/Profile.h"
#include "curve/CapFill.h"
#include "curve/CurveBoolean.h"
#include "curve/Skeleton.h"
#include "geo/Attribute.h"
#include "geo/Normals.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace lila::curve
{
namespace
{
using D2 = glm::dvec2;
using D3 = glm::dvec3;
using geo::Index;

/** A bevel vertex: a skeleton node (a == b) or where the arc a-b crosses a level; level -1 for a node between levels. */
using Key = std::tuple<int, int, int>;

enum Part : int32_t
{
    FrontCap = 0,
    FrontBevel = 1,
    Side = 2,
    BackBevel = 3,
    BackCap = 4,
};

/** A bevel's levels: the inset (skeleton time), rise and uv v at each profile point. */
struct Levels
{
    std::vector<double> T, Z, V;

    size_t size() const { return T.size(); }
    double top() const { return T.empty() ? 0.0 : T.back(); }
    /** Rise and v between levels, for a node at time t inside the profile. */
    std::pair<double, double> at(double t) const
    {
        for (size_t k = 0; k + 1 < T.size(); ++k)
            if (t >= T[k] && t <= T[k + 1] && T[k + 1] > T[k])
            {
                double s = (t - T[k]) / (T[k + 1] - T[k]);
                return {Z[k] + (Z[k + 1] - Z[k]) * s, V[k] + (V[k + 1] - V[k]) * s};
            }
        return {Z.empty() ? 0.0 : Z.back(), V.empty() ? 0.0 : V.back()};
    }
};

std::vector<D2> resample(const std::vector<D2>& loop, double maxEdge)
{
    std::vector<D2> out;
    for (size_t i = 0; i < loop.size(); ++i)
    {
        const D2& a = loop[i];
        const D2& b = loop[(i + 1) % loop.size()];
        out.push_back(a);
        double len = glm::length(b - a);
        if (maxEdge > 0.0 && len > maxEdge * 1.5)
        {
            int parts = int(std::ceil(len / maxEdge));
            for (int k = 1; k < parts; ++k)
                out.push_back(a + (b - a) * (double(k) / parts));
        }
    }
    return out;
}

/** Corner uvs of a face around the outline: u unwrapped across the loop's seam. */
void unwrapU(std::vector<glm::vec2>& uv, double loopLen, double scale)
{
    if (uv.empty() || loopLen <= 0.0)
        return;
    float L = float(loopLen / scale);
    float lo = uv[0].x, hi = uv[0].x;
    for (const glm::vec2& c : uv)
    {
        lo = std::min(lo, c.x);
        hi = std::max(hi, c.x);
    }
    if (hi - lo > L * 0.5f)
        for (glm::vec2& c : uv)
            if (c.x < L * 0.5f)
                c.x += L;
}

class Builder
{
public:
    Builder(geo::Mesh& mesh, const PlaneFrame& plane, double uvScale, D2 uvLo, D2 uvHi, std::vector<std::string>& warnings)
        : m(mesh), mPlane(plane), mUvScale(uvScale), mUvLo(uvLo), mUvHi(uvHi), mWarnings(warnings)
    {
    }

    /** One region (outer loop and holes) with its skeleton. */
    void region(const std::vector<std::vector<D2>>& loops, const StraightSkeleton& sk, const BevelPlusOptions& o, const Levels& front,
                const Levels& back, double sideV0)
    {
        mSk = &sk;
        mFrontKeys.clear();
        mBackKeys.clear();
        nodeUs(loops);
        double zb = 0.0, zf = std::max(0.0, o.extrudeDepth);

        bool frontBevel = front.size() > 1 && front.top() > 0.0;
        bool backBevel = back.size() > 1 && back.top() > 0.0;
        if (frontBevel)
            bevel(front, zf, 1.0, mFrontKeys, FrontBevel);
        if (backBevel)
            bevel(back, zb, -1.0, mBackKeys, BackBevel);
        sides(loops, o, front, back, zb, zf, sideV0);
        if (o.frontCap)
            cap(front, frontBevel, zf, 1.0, mFrontKeys, o, FrontCap);
        if (o.backCap)
            cap(back, backBevel, zb, -1.0, mBackKeys, o, BackCap);
    }

    std::vector<std::pair<Index, Index>> seams;

    int failed() const { return mFailed; }

private:
    geo::Mesh& m;
    PlaneFrame mPlane;
    double mUvScale;
    D2 mUvLo, mUvHi;
    std::vector<std::string>& mWarnings;
    const StraightSkeleton* mSk = nullptr;
    std::map<Key, Index> mFrontKeys, mBackKeys;
    std::vector<double> mNodeU;
    std::vector<int> mNodeLoop;
    std::vector<double> mLoopLen;
    int mFailed = 0;

    struct Pt
    {
        Key key;
        D2 p;
        double t;
    };

    /** u along the outline for every skeleton node: the input nodes by arc length, the others averaged over their faces. */
    void nodeUs(const std::vector<std::vector<D2>>& loops)
    {
        const StraightSkeleton& sk = *mSk;
        size_t N = sk.nodes.size();
        mNodeU.assign(N, 0.0);
        mNodeLoop.assign(N, -1);
        mLoopLen.assign(loops.size(), 0.0);
        int node = 0;
        for (size_t l = 0; l < loops.size(); ++l)
        {
            double acc = 0.0;
            for (size_t i = 0; i < loops[l].size(); ++i)
            {
                mNodeU[size_t(node)] = acc;
                mNodeLoop[size_t(node)] = int(l);
                acc += glm::length(loops[l][(i + 1) % loops[l].size()] - loops[l][i]);
                ++node;
            }
            mLoopLen[l] = acc;
        }
        size_t inputNodes = size_t(node);
        std::vector<double> sum(N, 0.0), ref(N, 0.0);
        std::vector<int> count(N, 0);
        for (size_t e = 0; e < sk.faces.size(); ++e)
        {
            int l = sk.edgeLoop[e];
            double L = mLoopLen[size_t(l)];
            int s = sk.edges[e][0], t = sk.edges[e][1];
            D2 ps = sk.nodes[size_t(s)], d = sk.nodes[size_t(t)] - ps;
            double len = glm::length(d);
            if (len <= 0.0)
                continue;
            d /= len;
            for (int n : sk.faces[e])
            {
                if (size_t(n) < inputNodes)
                    continue;
                if (mNodeLoop[size_t(n)] >= 0 && mNodeLoop[size_t(n)] != l)
                    continue;
                double u = mNodeU[size_t(s)] + std::clamp(glm::dot(sk.nodes[size_t(n)] - ps, d), 0.0, len);
                if (count[size_t(n)] == 0)
                {
                    ref[size_t(n)] = u;
                    mNodeLoop[size_t(n)] = l;
                }
                else if (u - ref[size_t(n)] > L * 0.5)
                    u -= L;
                else if (ref[size_t(n)] - u > L * 0.5)
                    u += L;
                sum[size_t(n)] += u;
                ++count[size_t(n)];
            }
        }
        for (size_t n = inputNodes; n < N; ++n)
            if (count[n] > 0)
            {
                double L = mLoopLen[size_t(mNodeLoop[n])];
                double u = sum[n] / count[n];
                mNodeU[n] = L > 0.0 ? std::fmod(std::fmod(u, L) + L, L) : u;
            }
    }

    double loopLenOf(int node) const
    {
        int l = node >= 0 && size_t(node) < mNodeLoop.size() ? mNodeLoop[size_t(node)] : -1;
        return l >= 0 ? mLoopLen[size_t(l)] : 0.0;
    }

    double keyU(const Key& k, const D2& p) const
    {
        auto [a, b, level] = k;
        if (a == b)
            return mNodeU[size_t(a)];
        const StraightSkeleton& sk = *mSk;
        double la = glm::length(sk.nodes[size_t(b)] - sk.nodes[size_t(a)]);
        double s = la > 0.0 ? glm::length(p - sk.nodes[size_t(a)]) / la : 0.0;
        double ua = mNodeU[size_t(a)], ub = mNodeU[size_t(b)];
        double L = loopLenOf(a);
        if (L > 0.0 && std::abs(ub - ua) > L * 0.5)
            ub += ub < ua ? L : -L;
        double u = ua + (ub - ua) * s;
        return L > 0.0 ? std::fmod(std::fmod(u, L) + L, L) : u;
    }

    Index pointFor(std::map<Key, Index>& keys, const Pt& q, const Levels& lv, double zBase, double sign)
    {
        auto it = keys.find(q.key);
        if (it != keys.end())
            return it->second;
        int level = std::get<2>(q.key);
        double z = level >= 0 ? lv.Z[size_t(level)] : lv.at(q.t).first;
        Index id = m.addPoint(mPlane.to3(q.p, zBase + sign * z));
        keys.emplace(q.key, id);
        return id;
    }

    glm::vec2 bevelUv(const Pt& q, const Levels& lv) const
    {
        int level = std::get<2>(q.key);
        double v = level >= 0 ? lv.V[size_t(level)] : lv.at(q.t).second;
        return glm::vec2(float(keyU(q.key, q.p) / mUvScale), float(v / mUvScale));
    }

    Index addFace(std::vector<Index> pts, std::vector<glm::vec2> uvs, bool flip, int part, double loopLen)
    {
        for (size_t i = 0; i < pts.size() && pts.size() >= 3;)
        {
            if (pts[i] == pts[(i + 1) % pts.size()])
            {
                pts.erase(pts.begin() + std::ptrdiff_t(i));
                uvs.erase(uvs.begin() + std::ptrdiff_t(i));
            }
            else
                ++i;
        }
        if (pts.size() < 3)
            return geo::kInvalid;
        if (loopLen > 0.0)
            unwrapU(uvs, loopLen, mUvScale);
        if (flip)
        {
            std::reverse(pts.begin(), pts.end());
            std::reverse(uvs.begin(), uvs.end());
        }
        Index f = m.addFace(pts);
        if (f == geo::kInvalid)
        {
            ++mFailed;
            return f;
        }
        auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        size_t i = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            if (i < uvs.size())
                uv[c] = uvs[i];
            ++i;
        });
        auto pa = m.faceAttrs().getOrAdd<int32_t>("part");
        if (f < pa.size())
            pa[f] = part;
        return f;
    }

    /** Where the boundary segment a-b crosses level L at time T. */
    Pt crossing(int a, int b, double T, int L) const
    {
        const StraightSkeleton& sk = *mSk;
        double ta = sk.times[size_t(a)], tb = sk.times[size_t(b)];
        bool aboveA = ta >= T;
        int up = aboveA ? a : b;
        if (sk.times[size_t(up)] == T)
            return {{up, up, L}, sk.nodes[size_t(up)], T};
        double s = (T - ta) / (tb - ta);
        return {{std::min(a, b), std::max(a, b), L}, sk.nodes[size_t(a)] + (sk.nodes[size_t(b)] - sk.nodes[size_t(a)]) * s, T};
    }

    /** The part of a face between levels k and k + 1 (both included), in boundary order. */
    std::vector<Pt> slab(const std::vector<int>& face, const Levels& lv, size_t k) const
    {
        const StraightSkeleton& sk = *mSk;
        double T0 = lv.T[k], T1 = lv.T[k + 1];
        int k0 = int(k), k1 = int(k) + 1;
        std::vector<Pt> out;
        auto push = [&](const Pt& q) {
            if (out.empty() || out.back().key != q.key)
                out.push_back(q);
        };
        auto nodePt = [&](int a, int level) { return Pt{{a, a, level}, sk.nodes[size_t(a)], sk.times[size_t(a)]}; };
        for (size_t j = 0; j < face.size(); ++j)
        {
            int a = face[j], b = face[(j + 1) % face.size()];
            double ta = sk.times[size_t(a)], tb = sk.times[size_t(b)];
            if (ta >= T0 && ta <= T1)
                push(nodePt(a, ta == T0 ? k0 : ta == T1 ? k1 : -1));
            std::vector<std::pair<double, Pt>> hits;
            if ((ta >= T0) != (tb >= T0))
            {
                int in = ta >= T0 ? a : b;
                double s = (T0 - ta) / (tb - ta);
                hits.push_back({s, sk.times[size_t(in)] == T0 ? nodePt(in, k0)
                                                               : Pt{{std::min(a, b), std::max(a, b), k0},
                                                                    sk.nodes[size_t(a)] + (sk.nodes[size_t(b)] - sk.nodes[size_t(a)]) * s, T0}});
            }
            if ((ta <= T1) != (tb <= T1))
            {
                int in = ta <= T1 ? a : b;
                double s = (T1 - ta) / (tb - ta);
                hits.push_back({s, sk.times[size_t(in)] == T1 ? nodePt(in, k1)
                                                               : Pt{{std::min(a, b), std::max(a, b), k1},
                                                                    sk.nodes[size_t(a)] + (sk.nodes[size_t(b)] - sk.nodes[size_t(a)]) * s, T1}});
            }
            std::sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            for (const auto& h : hits)
                push(h.second);
        }
        while (out.size() > 1 && out.front().key == out.back().key)
            out.pop_back();
        return out;
    }

    /** Loops where the wavefront reached level L, the bevelled region on their left. */
    std::vector<std::vector<Pt>> isoLoops(double T, int L) const
    {
        const StraightSkeleton& sk = *mSk;
        std::vector<std::pair<Pt, Pt>> segs;
        for (const std::vector<int>& face : sk.faces)
        {
            if (face.size() < 3)
                continue;
            // Each run of the boundary at or above the level is closed by the level line, run backwards from where the
            // boundary drops below to where it rose. A run lying exactly on the level is that line itself, node by node.
            size_t n = face.size();
            auto above = [&](size_t j) { return sk.times[size_t(face[j % n])] >= T; };
            for (size_t j = 0; j < n; ++j)
            {
                if (above(j + n - 1) || !above(j))
                    continue;
                size_t end = j;
                while (end + 1 < j + n && above(end + 1))
                    ++end;
                if (end + 1 == j + n && above(end + 1))
                    continue;
                Pt up = crossing(face[(j + n - 1) % n], face[j % n], T, L);
                Pt down = crossing(face[end % n], face[(end + 1) % n], T, L);
                bool onLevel = true;
                for (size_t k = j; k <= end; ++k)
                    onLevel = onLevel && sk.times[size_t(face[k % n])] == T;
                if (onLevel)
                {
                    for (size_t k = end; k > j; --k)
                    {
                        int a = face[k % n], b = face[(k - 1) % n];
                        segs.push_back({{{a, a, L}, sk.nodes[size_t(a)], T}, {{b, b, L}, sk.nodes[size_t(b)], T}});
                    }
                }
                else if (down.key != up.key)
                    segs.push_back({down, up});
            }
        }
        // A segment and its reverse are a zero-width sliver (a ridge exactly at the level): both go.
        std::map<std::pair<Key, Key>, size_t> byEnds;
        std::vector<uint8_t> dead(segs.size(), 0);
        for (size_t s = 0; s < segs.size(); ++s)
        {
            auto rev = byEnds.find({segs[s].second.key, segs[s].first.key});
            if (rev != byEnds.end() && !dead[rev->second])
            {
                dead[s] = dead[rev->second] = 1;
                continue;
            }
            byEnds.emplace(std::pair<Key, Key>{segs[s].first.key, segs[s].second.key}, s);
        }
        std::multimap<Key, size_t> from;
        for (size_t s = 0; s < segs.size(); ++s)
            if (!dead[s])
                from.emplace(segs[s].first.key, s);
        std::vector<uint8_t> used(dead);
        std::vector<std::vector<Pt>> loops;
        for (size_t s0 = 0; s0 < segs.size(); ++s0)
        {
            if (used[s0])
                continue;
            std::vector<Pt> loop;
            size_t s = s0;
            while (!used[s])
            {
                used[s] = 1;
                loop.push_back(segs[s].first);
                // Where loops touch at a point, keep each loop to itself: take the first way out clockwise from the way back.
                const Pt& at = segs[s].second;
                D2 back = segs[s].first.p - at.p;
                double ref = std::atan2(back.y, back.x);
                size_t next = segs.size();
                double best = 1e300;
                auto [lo, hi] = from.equal_range(at.key);
                for (auto it = lo; it != hi; ++it)
                {
                    if (used[it->second] && it->second != s0)
                        continue;
                    D2 d = segs[it->second].second.p - at.p;
                    double turn = ref - std::atan2(d.y, d.x);
                    while (turn <= 1e-12)
                        turn += glm::two_pi<double>();
                    if (turn < best)
                    {
                        best = turn;
                        next = it->second;
                    }
                }
                if (next == segs.size() || next == s0)
                    break;
                s = next;
            }
            if (loop.size() >= 3)
                loops.push_back(std::move(loop));
        }
        return loops;
    }

    void bevel(const Levels& lv, double zBase, double sign, std::map<Key, Index>& keys, int part)
    {
        const StraightSkeleton& sk = *mSk;
        bool flip = sign < 0.0;
        for (size_t e = 0; e < sk.faces.size(); ++e)
        {
            const std::vector<int>& face = sk.faces[e];
            if (face.size() < 3)
                continue;
            double L = mLoopLen[size_t(sk.edgeLoop[e])];
            for (size_t k = 0; k + 1 < lv.size(); ++k)
            {
                if (lv.T[k + 1] <= lv.T[k])
                    continue;
                std::vector<Pt> poly = slab(face, lv, k);
                if (poly.size() < 3)
                    continue;
                std::vector<Index> pts;
                std::vector<glm::vec2> uvs;
                for (const Pt& q : poly)
                {
                    pts.push_back(pointFor(keys, q, lv, zBase, sign));
                    uvs.push_back(bevelUv(q, lv));
                }
                addFace(pts, uvs, flip, part, L);
            }
        }
        // Steps straight up (two levels at one inset) are bands around the level's loops.
        for (size_t k = 0; k + 1 < lv.size(); ++k)
        {
            if (lv.T[k + 1] > lv.T[k])
                continue;
            std::vector<std::vector<Pt>> loops = k == 0 ? outlineLoops(0) : isoLoops(lv.T[k], int(k));
            for (const std::vector<Pt>& loop : loops)
                for (size_t i = 0; i < loop.size(); ++i)
                {
                    Pt a = loop[i], b = loop[(i + 1) % loop.size()];
                    Pt a1 = a, b1 = b;
                    std::get<2>(a1.key) = int(k) + 1;
                    std::get<2>(b1.key) = int(k) + 1;
                    std::vector<Index> pts = {pointFor(keys, a, lv, zBase, sign), pointFor(keys, b, lv, zBase, sign), pointFor(keys, b1, lv, zBase, sign),
                                              pointFor(keys, a1, lv, zBase, sign)};
                    std::vector<glm::vec2> uvs = {bevelUv(a, lv), bevelUv(b, lv), bevelUv(b1, lv), bevelUv(a1, lv)};
                    addFace(pts, uvs, flip, part, loopLenOf(std::get<0>(a.key)));
                }
        }
    }

    /** The input loops as level points (the outline itself). */
    std::vector<std::vector<Pt>> outlineLoops(int level) const
    {
        const StraightSkeleton& sk = *mSk;
        std::vector<std::vector<Pt>> loops;
        for (size_t e = 0; e < sk.edges.size(); ++e)
        {
            if (e == 0 || sk.edgeLoop[e] != sk.edgeLoop[e - 1])
                loops.emplace_back();
            int n = sk.edges[e][0];
            loops.back().push_back({{n, n, level}, sk.nodes[size_t(n)], 0.0});
        }
        return loops;
    }

    void sides(const std::vector<std::vector<D2>>& loops, const BevelPlusOptions& o, const Levels& front, const Levels& back, double zb, double zf,
               double sideV0)
    {
        int rows = std::max(1, o.extrudeDivisions);
        if (zf - zb <= 0.0)
            rows = 0;
        int node = 0;
        for (size_t l = 0; l < loops.size(); ++l)
        {
            size_t n = loops[l].size();
            std::vector<std::vector<Index>> ring(size_t(rows) + 1, std::vector<Index>(n));
            for (size_t i = 0; i < n; ++i)
            {
                int nd = node + int(i);
                Pt q{{nd, nd, 0}, loops[l][i], 0.0};
                ring[0][i] = pointFor(mBackKeys, q, back, zb, -1.0);
                if (rows == 0)
                {
                    mFrontKeys.emplace(q.key, ring[0][i]);
                    continue;
                }
                ring[size_t(rows)][i] = pointFor(mFrontKeys, q, front, zf, 1.0);
                for (int r = 1; r < rows; ++r)
                    ring[size_t(r)][i] = m.addPoint(mPlane.to3(loops[l][i], zb + (zf - zb) * r / rows));
            }
            double L = mLoopLen[l];
            for (int r = 0; r < rows; ++r)
                for (size_t i = 0; i < n; ++i)
                {
                    size_t j = (i + 1) % n;
                    float u0 = float(mNodeU[size_t(node) + i] / mUvScale), u1 = float((j == 0 ? L : mNodeU[size_t(node) + j]) / mUvScale);
                    float v0 = float((sideV0 + (zf - zb) * r / rows) / mUvScale), v1 = float((sideV0 + (zf - zb) * (r + 1) / rows) / mUvScale);
                    addFace({ring[size_t(r)][i], ring[size_t(r)][j], ring[size_t(r) + 1][j], ring[size_t(r) + 1][i]}, {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}},
                            false, Side, 0.0);
                    if (i == 0)
                        seams.push_back({ring[size_t(r)][0], ring[size_t(r) + 1][0]});
                }
            node += int(n);
        }
    }

    void cap(const Levels& lv, bool bevelled, double zBase, double sign, std::map<Key, Index>& keys, const BevelPlusOptions& o, int part)
    {
        std::vector<std::vector<Pt>> loops = bevelled ? isoLoops(lv.top(), int(lv.size()) - 1) : outlineLoops(0);
        if (loops.empty())
            return;
        std::vector<std::vector<D2>> pts2;
        std::vector<Index> boundary;
        for (const std::vector<Pt>& loop : loops)
        {
            pts2.emplace_back();
            for (const Pt& q : loop)
            {
                pts2.back().push_back(q.p);
                boundary.push_back(pointFor(keys, q, lv, zBase, sign));
            }
        }
        CapFillOptions co;
        co.spacing = o.capSpacing;
        co.quads = o.capQuads;
        CapFillResult fill = capFill(pts2, co);
        if (!fill.ok())
        {
            mWarnings.push_back("cap: " + fill.error);
            return;
        }
        double z = zBase + sign * (bevelled ? lv.Z.back() : 0.0);
        std::vector<Index> ids(fill.points.size());
        for (size_t i = 0; i < fill.points.size(); ++i)
            ids[i] = i < boundary.size() ? boundary[i] : m.addPoint(mPlane.to3(fill.points[i], z));
        double S = mUvScale;
        for (const std::vector<int>& f : fill.faces)
        {
            std::vector<Index> pts;
            std::vector<glm::vec2> uvs;
            for (int v : f)
            {
                pts.push_back(ids[size_t(v)]);
                const D2& p = fill.points[size_t(v)];
                float u = float(sign > 0.0 ? (p.x - mUvLo.x) / S : (mUvHi.x - p.x) / S);
                uvs.push_back({u, float((p.y - mUvLo.y) / S)});
            }
            addFace(pts, uvs, sign < 0.0, part, 0.0);
        }
        for (size_t l = 0, base = 0; l < pts2.size(); base += pts2[l].size(), ++l)
            for (size_t i = 0; i < pts2[l].size(); ++i)
                seams.push_back({boundary[base + i], boundary[base + (i + 1) % pts2[l].size()]});
    }
};

Levels levelsOf(const BevelSide& side)
{
    Levels lv;
    if (side.width <= 0.0)
    {
        lv.T.push_back(0.0);
        lv.Z.push_back(0.0);
        return lv;
    }
    for (const D2& p : bevelProfilePoints(side))
    {
        lv.T.push_back(p.x * side.width);
        lv.Z.push_back(p.y * std::max(0.0, side.depth));
    }
    return lv;
}

/** v along the profile from the side (0) out to the cap. */
void profileV(Levels& lv, double start, double dir)
{
    lv.V.assign(lv.size(), start);
    double acc = 0.0;
    for (size_t k = 1; k < lv.size(); ++k)
    {
        acc += std::hypot(lv.T[k] - lv.T[k - 1], lv.Z[k] - lv.Z[k - 1]);
        lv.V[k] = start + dir * acc;
    }
}
} // namespace

const char* bevelProfileName(BevelProfile p)
{
    switch (p)
    {
    case BevelProfile::Straight: return "straight";
    case BevelProfile::Convex: return "convex";
    case BevelProfile::Concave: return "concave";
    case BevelProfile::Step: return "step";
    case BevelProfile::Custom: return "custom";
    }
    return "straight";
}

bool parseBevelProfile(std::string_view name, BevelProfile* out)
{
    for (BevelProfile p : {BevelProfile::Straight, BevelProfile::Convex, BevelProfile::Concave, BevelProfile::Step, BevelProfile::Custom})
        if (name == bevelProfileName(p))
        {
            *out = p;
            return true;
        }
    return false;
}

std::vector<glm::dvec2> bevelProfilePoints(const BevelSide& side)
{
    int n = std::clamp(side.segments, 1, 256);
    std::vector<D2> pts;
    const double h = glm::half_pi<double>();
    switch (side.profile)
    {
    case BevelProfile::Straight:
        for (int k = 0; k <= n; ++k)
            pts.push_back(D2(double(k) / n));
        break;
    case BevelProfile::Convex:
        for (int k = 0; k <= n; ++k)
        {
            double s = double(k) / n;
            pts.push_back(D2(1.0 - std::cos(s * h), std::sin(s * h)));
        }
        break;
    case BevelProfile::Concave:
        for (int k = 0; k <= n; ++k)
        {
            double s = double(k) / n;
            pts.push_back(D2(std::sin(s * h), 1.0 - std::cos(s * h)));
        }
        break;
    case BevelProfile::Step:
        pts.push_back(D2(0.0));
        for (int k = 0; k < n; ++k)
        {
            pts.push_back(D2(double(k) / n, double(k + 1) / n));
            pts.push_back(D2(double(k + 1) / n, double(k + 1) / n));
        }
        break;
    case BevelProfile::Custom:
    {
        if (side.custom.size() < 2)
        {
            pts = {D2(0.0), D2(1.0)};
            break;
        }
        D2 a = side.custom.front(), b = side.custom.back();
        D2 span = b - a;
        for (const D2& p : side.custom)
            pts.push_back(D2(std::abs(span.x) > 1e-12 ? (p.x - a.x) / span.x : 0.0, std::abs(span.y) > 1e-12 ? (p.y - a.y) / span.y : 0.0));
        pts.front() = D2(0.0);
        pts.back() = D2(1.0);
        break;
    }
    }
    // The bevel only ever moves inward: a profile doubling back is held at its furthest inset.
    for (size_t k = 1; k < pts.size(); ++k)
        pts[k].x = std::clamp(std::max(pts[k].x, pts[k - 1].x), 0.0, 1.0);
    pts.front().x = 0.0;
    return pts;
}

BevelPlusResult bevelPlus(const Splines& curves, const BevelPlusOptions& o)
{
    LILA_ZONE();

    BevelPlusResult res;
    double deviation = 0.0;
    PlaneFrame plane = fitPlane(curves, o.normal, &deviation);
    TessOptions tess;
    tess.mode = TessMode::Adaptive;
    tess.chordError = std::max(o.tolerance, 1e-5);
    tess.maxAngle = glm::radians(std::clamp(o.maxAngle, 0.5, 45.0));
    Loops2 loops = loopsOf(curves, plane, tess, &res.warnings);
    if (loops.empty())
    {
        res.warnings.push_back("no closed curves to bevel");
        return res;
    }
    D2 lo(1e300), hi(-1e300);
    for (const Loop2& l : loops)
        for (const D2& p : l.points)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    double extent = std::max(hi.x - lo.x, hi.y - lo.y);
    if (deviation > std::max(0.01, extent * 1e-4))
        res.warnings.push_back("the curves are not planar (up to " + std::to_string(deviation) + " cm off their plane); they are flattened onto it");
    if (loopsCross(loops))
    {
        res.warnings.push_back("the curves cross; their even-odd union is bevelled");
        CurveBooleanOptions bo;
        bo.normal = plane.normal;
        CurveBooleanResult u = curveBoolean({curves}, bo);
        loops = loopsOf(u.curves, plane, tess, nullptr);
        if (loops.empty())
            return res;
    }
    std::vector<Region2> regions = nestLoops(std::move(loops));

    double area = 0.0;
    std::vector<double> lens;
    for (const Region2& r : regions)
    {
        area += std::abs(signedArea(r.outer.points));
        for (const Loop2& h : r.holes)
            area -= std::abs(signedArea(h.points));
        auto addLens = [&](const Loop2& l) {
            for (size_t i = 0; i < l.points.size(); ++i)
                lens.push_back(glm::length(l.points[(i + 1) % l.points.size()] - l.points[i]));
        };
        addLens(r.outer);
        for (const Loop2& h : r.holes)
            addLens(h);
    }
    std::nth_element(lens.begin(), lens.begin() + std::ptrdiff_t(lens.size() / 2), lens.end());
    double spacing = o.capSpacing > 0.0 ? o.capSpacing : std::max(lens[lens.size() / 2], std::sqrt(std::max(area, 0.0)) / 40.0);
    spacing = std::max(spacing, std::sqrt(std::max(area, 1e-12)) / 400.0);

    BevelSide front = o.front, back = o.back;
    front.width = std::max(0.0, front.width);
    back.width = std::max(0.0, back.width);
    Levels lf = levelsOf(front), lb = levelsOf(back);
    double backLen = 0.0;
    for (size_t k = 1; k < lb.size(); ++k)
        backLen += std::hypot(lb.T[k] - lb.T[k - 1], lb.Z[k] - lb.Z[k - 1]);
    profileV(lb, backLen, -1.0);
    double sideV0 = backLen;
    profileV(lf, backLen + std::max(0.0, o.extrudeDepth), 1.0);
    double maxT = std::max(lf.top(), lb.top());

    double uvScale = o.uvScale > 0.0 ? o.uvScale : std::max(extent, 1e-6);
    Builder b(res.mesh, plane, uvScale, lo, hi, res.warnings);
    for (const Region2& r : regions)
    {
        std::vector<std::vector<D2>> polys;
        double tol = std::max(extent * 1e-7, 1e-9);
        std::vector<D2> outer = cleanLoop(resample(r.outer.points, spacing), tol);
        if (outer.size() < 3)
            continue;
        polys.push_back(std::move(outer));
        for (const Loop2& h : r.holes)
        {
            std::vector<D2> hole = cleanLoop(resample(h.points, spacing), tol);
            if (hole.size() >= 3)
                polys.push_back(std::move(hole));
        }
        StraightSkeleton sk;
        if (maxT > 0.0)
        {
            SkeletonOptions so;
            so.maxTime = maxT;
            sk = straightSkeleton(polys, so);
        }
        else
        {
            // No bevel: only the outline is needed.
            int node = 0;
            for (size_t l = 0; l < polys.size(); ++l)
                for (size_t i = 0; i < polys[l].size(); ++i, ++node)
                {
                    sk.nodes.push_back(polys[l][i]);
                    sk.times.push_back(0.0);
                    sk.edges.push_back({node, int(i + 1 == polys[l].size() ? node - int(i) : node + 1)});
                    sk.edgeLoop.push_back(int(l));
                }
            sk.faces.assign(sk.edges.size(), {});
        }
        if (!sk.ok())
            res.warnings.push_back("bevel: " + sk.error);
        b.region(polys, sk, o, lf, lb, sideV0);
    }
    if (b.failed() > 0)
        res.warnings.push_back(std::to_string(b.failed()) + " faces could not be added (overlapping geometry)");
    auto marks = res.mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (auto [p, q] : b.seams)
        if (Index e = res.mesh.findEdge(p, q); e != geo::kInvalid && e < marks.size())
            marks[e] = true;
    geo::setHardEdgesByAngle(res.mesh, std::clamp(o.smoothingAngle, 0.0, 180.0));
    return res;
}
} // namespace lila::curve
