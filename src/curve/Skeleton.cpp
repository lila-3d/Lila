#include "curve/Skeleton.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <queue>

namespace lila::curve
{
namespace
{
constexpr double kInf = std::numeric_limits<double>::infinity();
// Edge directions closer than this (sine of the angle) count as parallel: near-degenerate input acts like the exact case.
constexpr double kParallel = 1e-8;

double angleOf(const D2& d)
{
    double a = std::atan2(d.y, d.x);
    return a < 0.0 ? a + glm::two_pi<double>() : a;
}

/** An input edge's supporting line, moving inward (toward n) at speed w. */
struct EdgeLine
{
    D2 o{0.0}, d{1.0, 0.0}, n{0.0, 1.0};
    double w = 1.0;
};

/** A wavefront vertex: where two edge lines meet, moving with them from where it was made. */
struct Vtx
{
    D2 p0{0.0};
    double t0 = 0.0;
    D2 vel{0.0};
    int in = -1, out = -1;
    int prev = -1, next = -1;
    int node = -1;
    bool active = true;
    bool reflex = false;
    bool spike = false;

    D2 at(double t) const { return p0 + vel * (t - t0); }
};

struct Event
{
    double t = 0.0;
    bool split = false;
    int v = -1, w = -1; // edge event: the edge's start and end vertex; split: the reflex vertex and its candidate index
    int edge = -1;      // split: the edge line it hits

    bool operator>(const Event& o) const { return t > o.t; }
};

class Simulation
{
public:
    StraightSkeleton out;

    Simulation(const std::vector<std::vector<D2>>& loops, const SkeletonOptions& o) : mMaxTime(o.maxTime)
    {
        double extent = 0.0;
        for (const auto& l : loops)
            for (const D2& p : l)
                extent = std::max({extent, std::abs(p.x), std::abs(p.y)});
        for (const auto& l : loops)
            for (size_t i = 0; i < l.size(); ++i)
                extent = std::max(extent, glm::length(l[(i + 1) % l.size()] - l[i]));
        mEps = std::max(extent, 1e-6) * 1e-8;
        build(loops, o);
    }

    void run()
    {
        size_t guard = 64 * mVerts.size() + 4096;
        while (!mQueue.empty() && guard-- > 0)
        {
            Event ev = mQueue.top();
            if (ev.t > mMaxTime + mEps)
                break;
            mQueue.pop();
            if (!ev.split)
            {
                const Vtx& u = mVerts[size_t(ev.v)];
                if (!u.active || !mVerts[size_t(ev.w)].active || u.next != ev.w)
                    continue;
                double t = std::min(std::max(ev.t, mNow), mMaxTime);
                resolve((u.at(t) + mVerts[size_t(ev.w)].at(t)) * 0.5, t, {ev.v, ev.w});
                continue;
            }
            const Vtx& r = mVerts[size_t(ev.v)];
            if (!r.active)
                continue;
            // Events a hair past the stop still happen, at the stop, so the front never keeps a sliver.
            double t = std::min(std::max(ev.t, mNow), mMaxTime);
            D2 x = r.at(t);
            if (!onActiveSegment(ev.edge, x, t, ev.v))
            {
                pushCandidate(ev.v, ev.w + 1);
                continue;
            }
            resolve(x, t, {ev.v});
        }
        finish();
    }

private:
    std::vector<EdgeLine> mLines;
    std::vector<Vtx> mVerts;
    std::vector<int> mActive;
    std::vector<std::vector<int>> mEdgeVerts; // per edge, every vertex that ever had it as out
    std::vector<std::vector<std::pair<double, int>>> mCands;
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> mQueue;
    std::vector<int> mRecentNodes;
    double mRecentTime = -1.0;
    double mNow = 0.0;
    double mMaxTime = kInf;
    double mEps = 1e-9;

    int addNode(const D2& p, double t)
    {
        out.nodes.push_back(p);
        out.times.push_back(t);
        return int(out.nodes.size()) - 1;
    }

    /** The node at p at time t, shared with any made at the same place and time. */
    int nodeAt(const D2& p, double t)
    {
        if (std::abs(t - mRecentTime) > mEps)
        {
            mRecentNodes.clear();
            mRecentTime = t;
        }
        for (int n : mRecentNodes)
            if (glm::length(out.nodes[size_t(n)] - p) <= mEps * 4.0)
                return n;
        int n = addNode(p, t);
        mRecentNodes.push_back(n);
        return n;
    }

    void addArc(int a, int b, int fa, int fb)
    {
        if (a != b)
            out.arcs.push_back({a, b, fa, fb});
    }

    void build(const std::vector<std::vector<D2>>& loops, const SkeletonOptions& o)
    {
        for (size_t l = 0; l < loops.size(); ++l)
        {
            const auto& pts = loops[l];
            size_t n = pts.size();
            int firstEdge = int(mLines.size());
            int firstNode = int(out.nodes.size());
            for (size_t i = 0; i < n; ++i)
            {
                addNode(pts[i], 0.0);
                EdgeLine ln;
                ln.o = pts[i];
                D2 d = pts[(i + 1) % n] - pts[i];
                double len = glm::length(d);
                ln.d = len > 0.0 ? d / len : D2(1.0, 0.0);
                ln.n = D2(-ln.d.y, ln.d.x);
                if (l < o.weights.size() && i < o.weights[l].size())
                    ln.w = std::max(0.0, o.weights[l][i]);
                mLines.push_back(ln);
                out.edges.push_back({firstNode + int(i), firstNode + int((i + 1) % n)});
                out.edgeLoop.push_back(int(l));
            }
            int firstVert = int(mVerts.size());
            for (size_t i = 0; i < n; ++i)
            {
                Vtx v;
                v.p0 = pts[i];
                v.in = firstEdge + int((i + n - 1) % n);
                v.out = firstEdge + int(i);
                v.prev = firstVert + int((i + n - 1) % n);
                v.next = firstVert + int((i + 1) % n);
                v.node = firstNode + int(i);
                setMotion(v);
                mVerts.push_back(v);
            }
        }
        mEdgeVerts.resize(mLines.size());
        mCands.resize(mVerts.size());
        for (size_t v = 0; v < mVerts.size(); ++v)
        {
            mActive.push_back(int(v));
            mEdgeVerts[size_t(mVerts[v].out)].push_back(int(v));
        }
        for (size_t v = 0; v < mVerts.size(); ++v)
        {
            pushEdgeEvent(int(v));
            findCandidates(int(v));
        }
    }

    /** Velocity and reflexness from the vertex's two lines. */
    void setMotion(Vtx& v)
    {
        const EdgeLine& a = mLines[size_t(v.in)];
        const EdgeLine& b = mLines[size_t(v.out)];
        double det = cross2(a.n, b.n);
        v.reflex = cross2(a.d, b.d) < -kParallel;
        v.spike = false;
        if (std::abs(det) > kParallel)
            v.vel = D2((a.w * b.n.y - b.w * a.n.y) / det, (a.n.x * b.w - b.n.x * a.w) / det);
        else if (glm::dot(a.n, b.n) > 0.0)
            v.vel = a.n * (0.5 * (a.w + b.w));
        else
        {
            // Its edges overlap: the pair closes up when the vertices around it arrive.
            v.vel = D2(0.0);
            v.spike = true;
        }
        if (std::abs(det) > 0.1)
        {
            // Sit exactly where the two lines cross now, so error doesn't build up from event to event.
            double ca = glm::dot(a.o, a.n) + a.w * v.t0, cb = glm::dot(b.o, b.n) + b.w * v.t0;
            D2 x((ca * b.n.y - cb * a.n.y) / det, (a.n.x * cb - b.n.x * ca) / det);
            if (glm::length(x - v.p0) < mEps * 16.0)
                v.p0 = x;
        }
    }

    double collapseTime(int ui) const
    {
        const Vtx& u = mVerts[size_t(ui)];
        if (u.next < 0)
            return kInf;
        const Vtx& v = mVerts[size_t(u.next)];
        const D2& d = mLines[size_t(u.out)].d;
        double A = glm::dot(v.p0 - v.vel * v.t0 - u.p0 + u.vel * u.t0, d);
        double B = glm::dot(v.vel - u.vel, d);
        if (B > -1e-12)
            return kInf;
        return -A / B;
    }

    void pushEdgeEvent(int ui)
    {
        if (ui < 0 || mVerts[size_t(ui)].next < 0 || !mVerts[size_t(ui)].active)
            return;
        double t = collapseTime(ui);
        if (t < kInf)
            mQueue.push({std::max(t, mNow), false, ui, mVerts[size_t(ui)].next, -1});
    }

    bool edgeActive(int e) const
    {
        for (int v : mEdgeVerts[size_t(e)])
            if (mVerts[size_t(v)].active)
                return true;
        return false;
    }

    /**
     * Lines the vertex may run into, soonest first: any for a reflex vertex; for others only lines running against one of
     * its own edges, which it meets when the strip between them closes.
     */
    void findCandidates(int ri)
    {
        const Vtx& r = mVerts[size_t(ri)];
        auto& c = mCands[size_t(ri)];
        c.clear();
        if (r.spike)
            return;
        D2 base = r.p0 - r.vel * r.t0;
        const D2& din = mLines[size_t(r.in)].d;
        const D2& dout = mLines[size_t(r.out)].d;
        for (size_t e = 0; e < mLines.size(); ++e)
        {
            if (int(e) == r.in || int(e) == r.out || !edgeActive(int(e)))
                continue;
            const EdgeLine& ln = mLines[e];
            if (!r.reflex)
            {
                bool against = (glm::dot(ln.d, din) < 0.0 && std::abs(cross2(ln.d, din)) < kParallel) ||
                               (glm::dot(ln.d, dout) < 0.0 && std::abs(cross2(ln.d, dout)) < kParallel);
                if (!against)
                    continue;
            }
            double A = glm::dot(base - ln.o, ln.n);
            double B = glm::dot(r.vel, ln.n) - ln.w;
            if (B >= -1e-12 || A + B * mNow <= mEps * 4.0)
                continue;
            double t = -A / B;
            if (t < mNow - mEps)
                continue;
            c.push_back({t, int(e)});
        }
        std::sort(c.begin(), c.end());
        pushCandidate(ri, 0);
    }

    void pushCandidate(int ri, int k)
    {
        const auto& c = mCands[size_t(ri)];
        if (size_t(k) < c.size())
            mQueue.push({std::max(c[size_t(k)].first, mNow), true, ri, k, c[size_t(k)].second});
    }

    /** True when x lies on an active segment of edge e at time t (not one ending at vertex skip). */
    bool onActiveSegment(int e, const D2& x, double t, int skip) const
    {
        const EdgeLine& ln = mLines[size_t(e)];
        for (int a : mEdgeVerts[size_t(e)])
        {
            const Vtx& va = mVerts[size_t(a)];
            if (!va.active || va.out != e || a == skip || va.next == skip)
                continue;
            D2 pa = va.at(t), pb = mVerts[size_t(va.next)].at(t);
            double s = glm::dot(x - pa, ln.d), len = glm::dot(pb - pa, ln.d);
            if (s >= -mEps * 4.0 && s <= len + mEps * 4.0 && std::abs(cross2(ln.d, x - pa)) <= mEps * 64.0 + 1e-9 * std::abs(s))
                return true;
        }
        return false;
    }

    double distToSegment(const D2& x, const D2& a, const D2& b) const
    {
        D2 d = b - a;
        double l2 = glm::dot(d, d);
        double s = l2 > 0.0 ? std::clamp(glm::dot(x - a, d) / l2, 0.0, 1.0) : 0.0;
        return glm::length(a + d * s - x);
    }

    int newVertex(const D2& x, double t, int in, int outEdge, int node)
    {
        Vtx v;
        v.p0 = x;
        v.t0 = t;
        v.in = in;
        v.out = outEdge;
        v.node = node;
        setMotion(v);
        mVerts.push_back(v);
        mCands.emplace_back();
        int id = int(mVerts.size()) - 1;
        mActive.push_back(id);
        mEdgeVerts[size_t(outEdge)].push_back(id);
        return id;
    }

    void link(int a, int b)
    {
        mVerts[size_t(a)].next = b;
        mVerts[size_t(b)].prev = a;
    }

    void kill(int v, int endNode)
    {
        Vtx& x = mVerts[size_t(v)];
        if (!x.active)
            return;
        addArc(x.node, endNode, x.in, x.out);
        x.active = false;
    }

    /** Closes wavefront loops of one or two vertices left around v. True when it did. */
    bool closeSmall(int v, double t)
    {
        Vtx& a = mVerts[size_t(v)];
        if (!a.active || a.next < 0)
            return false;
        if (a.next == v)
        {
            kill(v, nodeAt(a.at(t), t));
            return true;
        }
        int w = a.next;
        if (mVerts[size_t(w)].next != v || !mVerts[size_t(w)].active)
            return false;
        Vtx& b = mVerts[size_t(w)];
        int na = nodeAt(a.at(t), t), nb = nodeAt(b.at(t), t);
        int ea = a.out, eb = b.out;
        kill(v, na);
        kill(w, nb);
        addArc(na, nb, ea, eb);
        return true;
    }

    struct Rec
    {
        int edge;
        int far;
        bool out;
        double ang;
    };

    /** Everything meeting at x at time t ends there; the wavefront around it is reconnected by angle. */
    void resolve(const D2& x, double t, std::vector<int> inv)
    {
        mNow = std::max(mNow, t);
        std::vector<int> passing;
        size_t keep = 0;
        for (int v : mActive)
        {
            if (!mVerts[size_t(v)].active)
                continue;
            mActive[keep++] = v;
            if (glm::length(mVerts[size_t(v)].at(t) - x) <= mEps * 4.0 && std::find(inv.begin(), inv.end(), v) == inv.end())
                inv.push_back(v);
        }
        mActive.resize(keep);
        std::erase_if(inv, [&](int v) { return !mVerts[size_t(v)].active; });
        if (inv.empty())
            return;
        auto involved = [&](int v) { return std::find(inv.begin(), inv.end(), v) != inv.end(); };
        for (int v : mActive)
        {
            const Vtx& a = mVerts[size_t(v)];
            if (involved(v) || involved(a.next))
                continue;
            if (distToSegment(x, a.at(t), mVerts[size_t(a.next)].at(t)) <= mEps * 4.0)
                passing.push_back(v);
        }
        int node = nodeAt(x, t);
        std::vector<Rec> recs;
        for (int v : inv)
        {
            const Vtx& a = mVerts[size_t(v)];
            if (!involved(a.prev))
                recs.push_back({a.in, a.prev, false, angleOf(-mLines[size_t(a.in)].d)});
            if (!involved(a.next))
                recs.push_back({a.out, a.next, true, angleOf(mLines[size_t(a.out)].d)});
        }
        for (int v : passing)
        {
            const Vtx& a = mVerts[size_t(v)];
            recs.push_back({a.out, v, false, angleOf(-mLines[size_t(a.out)].d)});
            recs.push_back({a.out, a.next, true, angleOf(mLines[size_t(a.out)].d)});
        }
        for (int v : inv)
            kill(v, node);
        for (int v : passing)
            mVerts[size_t(v)].next = -1;

        // Counter-clockwise; where an outgoing and an incoming edge leave along the same ray, the outgoing one first, so the
        // zero-width spike between them pairs up and closes.
        constexpr double kTie = 1e-7;
        std::sort(recs.begin(), recs.end(), [](const Rec& p, const Rec& q) { return p.ang < q.ang; });
        for (bool swapped = true; swapped;)
        {
            swapped = false;
            for (size_t k = 0; k + 1 < recs.size(); ++k)
                if (!recs[k].out && recs[k + 1].out && recs[k + 1].ang - recs[k].ang < kTie)
                {
                    std::swap(recs[k], recs[k + 1]);
                    swapped = true;
                }
        }
        if (recs.size() >= 2 && !recs.back().out && recs.front().out && recs.front().ang + glm::two_pi<double>() - recs.back().ang < kTie)
            std::rotate(recs.begin(), recs.end() - 1, recs.end());

        std::vector<std::pair<size_t, size_t>> pairs;
        std::vector<size_t> open, unmatched;
        size_t first = 0;
        while (first < recs.size() && !recs[first].out)
            ++first;
        for (size_t k = 0; k < recs.size(); ++k)
        {
            size_t i = (first + k) % recs.size();
            if (recs[i].out)
                open.push_back(i);
            else if (!open.empty())
            {
                pairs.push_back({open.back(), i});
                open.pop_back();
            }
            else
                unmatched.push_back(i);
        }
        // Only numerical noise leaves any over; pair them in order so the wavefront stays linked.
        for (size_t k = 0; k < std::min(open.size(), unmatched.size()); ++k)
            pairs.push_back({open[k], unmatched[k]});
        std::vector<int> touched, made;
        for (auto [oi, ii] : pairs)
        {
            const Rec& o = recs[oi];
            const Rec& in = recs[ii];
            touched.push_back(in.far);
            touched.push_back(o.far);
            if (o.edge == in.edge)
            {
                link(in.far, o.far);
                continue;
            }
            int nv = newVertex(x, t, in.edge, o.edge, node);
            link(in.far, nv);
            link(nv, o.far);
            made.push_back(nv);
        }
        for (int v : made)
            touched.push_back(v);
        for (int v : touched)
            closeSmall(v, t);
        for (int v : made)
        {
            if (!mVerts[size_t(v)].active)
                continue;
            pushEdgeEvent(mVerts[size_t(v)].prev);
            pushEdgeEvent(v);
            findCandidates(v);
        }
        for (int v : touched)
            if (mVerts[size_t(v)].active)
                pushEdgeEvent(v);
    }

    void finish()
    {
        bool stopped = !mQueue.empty() && mQueue.top().t > mMaxTime + mEps;
        double t = stopped ? mMaxTime : mNow;
        out.endTime = t;
        std::vector<int> left;
        for (int v : mActive)
            if (mVerts[size_t(v)].active)
                left.push_back(v);
        if (!left.empty() && !stopped && mMaxTime == kInf)
            out.error = "the skeleton did not close up (" + std::to_string(left.size()) + " wavefront vertices left)";
        // What remains is the front: each vertex ends where it is now, joined along its outgoing edge.
        std::map<int, int> endNode;
        for (int v : left)
            endNode[v] = nodeAt(mVerts[size_t(v)].at(t), t);
        std::vector<uint8_t> seen(mVerts.size(), 0);
        for (int v : left)
        {
            if (seen[size_t(v)])
                continue;
            std::vector<int> loop;
            for (int w = v; w >= 0 && !seen[size_t(w)] && mVerts[size_t(w)].active; w = mVerts[size_t(w)].next)
            {
                seen[size_t(w)] = 1;
                loop.push_back(w);
            }
            std::vector<int> nodes;
            for (int w : loop)
            {
                int n = endNode[w];
                if (nodes.empty() || nodes.back() != n)
                    nodes.push_back(n);
            }
            while (nodes.size() > 1 && nodes.front() == nodes.back())
                nodes.pop_back();
            for (int w : loop)
                if (endNode.count(mVerts[size_t(w)].next))
                    addArc(endNode[w], endNode[mVerts[size_t(w)].next], mVerts[size_t(w)].out, -1);
            if (nodes.size() >= 3)
                out.front.push_back(std::move(nodes));
        }
        for (int v : left)
            kill(v, endNode[v]);
        traceFaces();
    }

    void traceFaces()
    {
        size_t E = mLines.size();
        std::vector<std::vector<std::array<int, 2>>> segs(E);
        for (size_t e = 0; e < E; ++e)
            segs[e].push_back(out.edges[e]);
        for (const SkeletonArc& a : out.arcs)
        {
            if (a.faceA >= 0)
                segs[size_t(a.faceA)].push_back({a.a, a.b});
            if (a.faceB >= 0 && a.faceB != a.faceA)
                segs[size_t(a.faceB)].push_back({a.a, a.b});
        }
        out.faces.assign(E, {});
        for (size_t e = 0; e < E; ++e)
        {
            const auto& ss = segs[e];
            std::vector<uint8_t> used(ss.size(), 0);
            used[0] = 1;
            std::vector<int> poly = {out.edges[e][0], out.edges[e][1]};
            int prev = out.edges[e][0], cur = out.edges[e][1];
            bool closed = false;
            for (size_t guard = 0; guard < ss.size() + 2; ++guard)
            {
                if (cur == out.edges[e][0])
                {
                    closed = true;
                    break;
                }
                // Face on the left: take the first segment clockwise from the way back.
                D2 c = out.nodes[size_t(cur)];
                double ref = angleOf(out.nodes[size_t(prev)] - c);
                int best = -1;
                double bestTurn = kInf;
                for (size_t k = 0; k < ss.size(); ++k)
                {
                    if (used[k] || (ss[k][0] != cur && ss[k][1] != cur))
                        continue;
                    int other = ss[k][0] == cur ? ss[k][1] : ss[k][0];
                    D2 d = out.nodes[size_t(other)] - c;
                    double turn = glm::length(d) > 0.0 ? ref - angleOf(d) : glm::pi<double>();
                    while (turn <= 1e-12)
                        turn += glm::two_pi<double>();
                    if (turn < bestTurn)
                    {
                        bestTurn = turn;
                        best = int(k);
                    }
                }
                if (best < 0)
                    break;
                used[size_t(best)] = 1;
                int other = ss[size_t(best)][0] == cur ? ss[size_t(best)][1] : ss[size_t(best)][0];
                prev = cur;
                cur = other;
                if (cur != out.edges[e][0])
                    poly.push_back(cur);
            }
            if (closed)
                out.faces[e] = std::move(poly);
            else if (out.error.empty())
                out.error = "could not trace the face of edge " + std::to_string(e);
        }
    }
};
} // namespace

StraightSkeleton straightSkeleton(const std::vector<std::vector<glm::dvec2>>& loops, const SkeletonOptions& o)
{
    LILA_ZONE();

    for (const auto& l : loops)
        if (l.size() < 3)
        {
            StraightSkeleton s;
            s.error = "a loop has fewer than three points";
            return s;
        }
    Simulation sim(loops, o);
    sim.run();
    return std::move(sim.out);
}

std::vector<glm::dvec2> cleanLoop(const std::vector<glm::dvec2>& loop, double tolerance)
{
    std::vector<D2> p;
    for (const D2& q : loop)
        if (p.empty() || glm::length(q - p.back()) > tolerance)
            p.push_back(q);
    while (p.size() > 1 && glm::length(p.front() - p.back()) <= tolerance)
        p.pop_back();
    for (bool changed = true; changed && p.size() >= 3;)
    {
        changed = false;
        for (size_t i = 0; i < p.size() && p.size() >= 3; ++i)
        {
            const D2& a = p[(i + p.size() - 1) % p.size()];
            const D2& b = p[i];
            const D2& c = p[(i + 1) % p.size()];
            D2 u = b - a, v = c - b;
            double lu = glm::length(u), lv = glm::length(v);
            bool spike = lu > 0.0 && lv > 0.0 && glm::dot(u, v) < 0.0 && std::abs(cross2(u, v)) <= kParallel * lu * lv;
            if (spike || glm::length(c - a) <= tolerance)
            {
                p.erase(p.begin() + std::ptrdiff_t(i));
                changed = true;
                break;
            }
        }
    }
    if (p.size() < 3)
        p.clear();
    return p;
}

std::vector<std::vector<glm::dvec2>> skeletonCenterLines(const StraightSkeleton& s)
{
    size_t inputNodes = 0;
    for (const auto& e : s.edges)
        inputNodes = std::max(inputNodes, size_t(std::max(e[0], e[1])) + 1);
    std::map<int, std::vector<int>> adj;
    std::vector<std::array<int, 2>> arcs;
    for (const SkeletonArc& a : s.arcs)
    {
        if (a.faceA < 0 || a.faceB < 0 || size_t(a.a) < inputNodes || size_t(a.b) < inputNodes)
            continue;
        if (glm::length(s.nodes[size_t(a.a)] - s.nodes[size_t(a.b)]) <= 0.0)
            continue;
        adj[a.a].push_back(int(arcs.size()));
        adj[a.b].push_back(int(arcs.size()));
        arcs.push_back({a.a, a.b});
    }
    std::vector<uint8_t> used(arcs.size(), 0);
    std::vector<std::vector<D2>> out;
    auto walk = [&](int startNode, int arc) {
        std::vector<D2> line = {s.nodes[size_t(startNode)]};
        int cur = startNode;
        while (arc >= 0 && !used[size_t(arc)])
        {
            used[size_t(arc)] = 1;
            cur = arcs[size_t(arc)][0] == cur ? arcs[size_t(arc)][1] : arcs[size_t(arc)][0];
            line.push_back(s.nodes[size_t(cur)]);
            const auto& nb = adj[cur];
            arc = -1;
            if (nb.size() == 2)
                for (int k : nb)
                    if (!used[size_t(k)])
                        arc = k;
        }
        out.push_back(std::move(line));
    };
    for (auto& [n, list] : adj)
        if (list.size() != 2)
            for (int k : list)
                if (!used[size_t(k)])
                    walk(n, k);
    for (size_t k = 0; k < arcs.size(); ++k)
        if (!used[k])
            walk(arcs[k][0], int(k));
    return out;
}
} // namespace lila::curve
