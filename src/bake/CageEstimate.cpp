#include "bake/CageEstimate.h"

#include "bake/AutoDistance.h"
#include "bake/Backend.h"
#include "bake/BakeCommon.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "bake/Maps.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Triangulate.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace lila::bake
{
namespace
{
using detail::edgeFunction;
using detail::meshByName;
using detail::mix3;
using detail::worldOf;

/** The ray direction a bake's auto cage uses at w in triangle t (TexelRaster's), pointing out of the low. */
glm::vec3 cageDirection(const LowInput& low, size_t t, const glm::dvec3& w, float groupSkew)
{
    const size_t c0 = t * 3;
    glm::vec3 n = mix3(&low.cageNormals[c0], w);
    const float l = glm::length(n);
    n = l > 0.0f ? n / l : glm::vec3(0.0f, 0.0f, 1.0f);
    const float skew = std::clamp(groupSkew + (low.cageSkew.size() == low.soup.positions.size() ? mix3(&low.cageSkew[c0], w) : 0.0f), 0.0f, 1.0f);
    if (skew > 0.0f)
    {
        const glm::vec3* p = &low.soup.positions[c0];
        const glm::vec3 g = glm::cross(p[1] - p[0], p[2] - p[0]);
        const float gl = glm::length(g);
        if (gl > 0.0f)
        {
            const glm::vec3 d = glm::mix(n, g / gl, skew);
            if (const float dl = glm::length(d); dl > 1e-6f)
                n = d / dl;
        }
    }
    return n;
}

/** Barycentric weights of a raster sample in its low triangle, from its UV. */
glm::dvec3 sampleBary(const LowInput& low, const TexelSample& s)
{
    const glm::vec2* uv = &low.soup.uvs[size_t(s.lowTriangle) * 3];
    const glm::dvec2 a(uv[0]), b(uv[1]), c(uv[2]), p(s.uv[0], s.uv[1]);
    const double area = edgeFunction(a, b, c);
    if (std::abs(area) < 1e-18)
        return glm::dvec3(1.0 / 3.0);
    glm::dvec3 w(edgeFunction(b, c, p) / area, edgeFunction(c, a, p) / area, edgeFunction(a, b, p) / area);
    w = glm::max(w, glm::dvec3(0.0));
    return w / std::max(1e-12, w.x + w.y + w.z);
}

/** What the walks along one sample's cage line found. */
struct Need
{
    /** Push needed to start outside the high material the low sits in; 0 when it sits outside. */
    float outward = 0.0f;
    /** Distance to the high below the low (for rear), negative when there is none within reach. */
    float inward = -1.0f;
    bool anyHit = false;
};

class Walker
{
public:
    Walker(const Tracer& tracer, size_t group) : mTracer(tracer), mGroup(group) {}

    /** cap bounds the walk: far layers of the same mesh would otherwise read as material the low starts inside. */
    Need at(uint32_t low, const glm::vec3& p, const glm::vec3& dir, float mCap, float mEps) const
    {
        Need n;
        // Exits pair off with the entries before them: an unpaired exit leaves material the walk started in.
        float travelled = 0.0f;
        int entered = 0;
        glm::vec3 o = p;
        for (int k = 0; k < 16 && travelled < mCap; ++k)
        {
            TraceHit h;
            if (!mTracer.closestHit(mGroup, OccluderScope::Group, kNoHit, low, RayKind::Projection, o, dir, mCap - travelled, h))
                break;
            n.anyHit = true;
            travelled += h.distance;
            if (h.backface)
            {
                if (entered > 0)
                    --entered;
                else
                    n.outward = travelled;
            }
            else
                ++entered;
            travelled += mEps;
            o = p + dir * travelled;
        }
        TraceHit h;
        if (mTracer.closestHit(mGroup, OccluderScope::Group, kNoHit, low, RayKind::Projection, p, -dir, mCap, h))
        {
            n.anyHit = true;
            if (!h.backface)
                n.inward = h.distance;
        }
        return n;
    }

private:
    const Tracer& mTracer;
    size_t mGroup;
};

/** Every triangle's interpolated push must reach need. */
struct Constraint
{
    uint32_t tri;
    glm::vec3 w;
    float need;
};

struct LowState
{
    const CageLow* source = nullptr;
    LowInput input;
    /** Point of each soup corner. */
    std::vector<geo::Index> cornerPoint;
    std::vector<Constraint> constraints;
    /** World push per point slot. */
    std::vector<float> push;
    /** Most push per point slot: just short of another low in front of it. */
    std::vector<float> clear;
    /** From this low's own bounds diagonal: the gap kept to the high, the walk's reach, its step past a hit, sample spacing. */
    float margin = 0.0f;
    float cap = 0.0f;
    float eps = 0.0f;
    float spacing = 1.0f;
};

/** The smallest per-point push (from push as it stands) meeting every constraint, by repeated projection. */
void solve(LowState& s, float floor)
{
    LILA_ZONE();

    std::vector<float>& x = s.push;
    for (geo::Index p = 0; p < x.size(); ++p)
        x[p] = std::max(x[p], floor);
    auto pts = [&](const Constraint& c) {
        const geo::Index* cp = &s.cornerPoint[size_t(c.tri) * 3];
        return std::array<geo::Index, 3>{cp[0], cp[1], cp[2]};
    };
    // Seed: each constraint lifts the point it sits closest to.
    for (const Constraint& c : s.constraints)
    {
        const auto p = pts(c);
        const int k = c.w.x >= c.w.y && c.w.x >= c.w.z ? 0 : c.w.y >= c.w.z ? 1 : 2;
        x[p[size_t(k)]] = std::max(x[p[size_t(k)]], c.need);
    }
    for (int pass = 0; pass < 24; ++pass)
    {
        bool moved = false;
        for (const Constraint& c : s.constraints)
        {
            const auto p = pts(c);
            const float cur = x[p[0]] * c.w.x + x[p[1]] * c.w.y + x[p[2]] * c.w.z;
            if (cur >= c.need * 0.9999f)
                continue;
            const float denom = c.w.x * c.w.x + c.w.y * c.w.y + c.w.z * c.w.z;
            const float d = (c.need - cur) / std::max(denom, 1e-6f);
            for (int k = 0; k < 3; ++k)
                x[p[size_t(k)]] += d * c.w[k];
            moved = true;
        }
        if (!moved)
            break;
    }
    // Anything projection left short lifts its whole triangle.
    for (const Constraint& c : s.constraints)
    {
        const auto p = pts(c);
        if (x[p[0]] * c.w.x + x[p[1]] * c.w.y + x[p[2]] * c.w.z < c.need * 0.999f)
            for (geo::Index q : p)
                x[q] = std::max(x[q], c.need);
    }
}

/** How much of the gap to another low in front a cage may cross. */
constexpr float kClearanceShare = 0.9f;

/** Holds every point to its clearance. */
void clampToClearance(LowState& s)
{
    for (size_t p = 0; p < s.push.size(); ++p)
        s.push[p] = std::min(s.push[p], s.clear[p]);
}

/** Raises points below their neighbours' mean halfway toward it: a smoother shell that never pulls under a need. */
void smoothUp(LowState& s)
{
    const geo::Mesh& m = *s.source->mesh;
    std::vector<float> next = s.push;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        double sum = 0.0;
        int n = 0;
        m.forEachPointEdge(p, [&](geo::Index e) {
            sum += s.push[m.edgeOtherPoint(e, p)];
            ++n;
        });
        if (n)
            next[p] = std::max(s.push[p], 0.5f * (s.push[p] + float(sum / n)));
    }
    s.push = std::move(next);
}

float quantile(std::vector<float> v, double q)
{
    if (v.empty())
        return 0.0f;
    const size_t i = std::min(v.size() - 1, size_t(double(v.size() - 1) * q));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(i), v.end());
    return v[i];
}

/** The group's inputs with highs, a prepared CPU backend and its Auto distances measured, as a bake starts. */
struct Prepared
{
    BakeInputs inputs;
    std::unique_ptr<Backend> backend;
};

std::unique_ptr<Prepared> prepare(scene::Graph& g, scene::NodeId group)
{
    scene::Node* grp = g.node(group);
    if (!grp || !isGroup(*grp))
        throw std::runtime_error("not a bake group");
    scene::Node* project = groupProject(g, *grp);
    if (!project)
        throw std::runtime_error(grp->name() + " is in no bake project");
    g.evaluate();
    auto p = std::make_unique<Prepared>();
    p->inputs = buildInputs(g, project->id(), {group}, true);
    if (p->inputs.groups.size() != 1)
        throw std::runtime_error(grp->name() + ": nothing to bake");
    if (p->inputs.groups[0].highs.empty())
        throw std::runtime_error(grp->name() + " has no highs");
    p->backend = createBackend("cpu");
    p->backend->prepare(p->inputs);
    resolveAutoDistances(p->inputs, *p->backend);
    return p;
}
} // namespace

std::vector<geo::Index> soupCornerPoints(const geo::Mesh& mesh, const glm::dmat4& world)
{
    const geo::Triangulation tris = geo::triangulate(mesh);
    const bool mirrored = glm::determinant(glm::dmat3(world)) < 0.0;
    const int order[3] = {0, mirrored ? 2 : 1, mirrored ? 1 : 2};
    std::vector<geo::Index> out(tris.corners.size());
    for (size_t t = 0; t < tris.faces.size(); ++t)
        for (int k = 0; k < 3; ++k)
            out[t * 3 + size_t(k)] = mesh.cornerPoint(tris.corners[t * 3 + size_t(order[k])]);
    return out;
}

json CageCoverage::toJson() const { return {{"samples", samples}, {"misses", misses}, {"backfaces", backfaces}, {"missRate", missRate()}}; }

json CageEstimate::toJson() const
{
    return {{"frontal", frontal}, {"rear", rear}, {"cageOffset", cageOffset}, {"after", after.toJson()}, {"iterations", iterations}, {"unreachable", unreachable}};
}

CageCoverage cageCoverage(const LowInput& low, uint32_t lowIndex, const CageParams& cage, size_t group, const Backend& backend, int resolution,
                          std::vector<TexelSample>* missed)
{
    LILA_ZONE();

    CageCoverage out;
    if (low.soup.uvs.empty())
        return out;
    RasterSettings rs;
    rs.width = rs.height = std::max(8, resolution);
    rs.supersampling = 1;
    rs.bandRows = 32;
    const TexelRaster raster(low, rs);
    std::atomic<uint64_t> samples = 0, misses = 0, backs = 0;
    std::mutex mutex;
    for (int udim : raster.udims())
        tbb::parallel_for(size_t(0), raster.bandCount(), [&](size_t b) {
            std::vector<TexelSample> s;
            raster.band(udim, b, cage, s);
            if (s.empty())
                return;
            for (TexelSample& t : s)
                t.low = lowIndex;
            std::vector<HitRecord> hits(s.size());
            backend.traceProjection(group, s, hits);
            backend.endBand(s);
            uint64_t m = 0, bf = 0;
            std::vector<TexelSample> local;
            for (size_t i = 0; i < s.size(); ++i)
                if (!(hits[i].flags & kHitValid))
                {
                    ++m;
                    bf += (hits[i].flags & kHitBackface) ? 1 : 0;
                    if (missed)
                    {
                        // Carries the backface flag to the caller in the otherwise unused sub index.
                        TexelSample t = s[i];
                        t.sub = (hits[i].flags & kHitBackface) ? 1u : 0u;
                        local.push_back(t);
                    }
                }
            samples += s.size();
            misses += m;
            backs += bf;
            if (missed && !local.empty())
            {
                std::scoped_lock lock(mutex);
                missed->insert(missed->end(), local.begin(), local.end());
            }
        });
    out.samples = samples;
    out.misses = misses;
    out.backfaces = backs;
    return out;
}

CageEstimate estimateCage(std::span<const CageLow> lows, const GroupSettings& settings, const BakeInputs&, size_t group, const Backend& backend,
                          const CageEstimateOptions& options)
{
    LILA_ZONE();

    const Tracer* tracer = backend.tracer();
    if (!tracer)
        throw std::runtime_error("cage estimate: the backend has no tracer");
    CageEstimate est;
    est.cageOffset = settings.cageOffset > 1e-6 ? settings.cageOffset : 1.0;
    const float skew = float(settings.skew);

    std::vector<LowState> states(lows.size());
    float margin = 0.0f;
    for (size_t i = 0; i < lows.size(); ++i)
    {
        LowState& s = states[i];
        s.source = &lows[i];
        s.input = lowFromMesh(*lows[i].mesh, lows[i].world, lows[i].name);
        s.cornerPoint = soupCornerPoints(*lows[i].mesh, lows[i].world);
        s.push.assign(lows[i].mesh->pointCapacity(), 0.0f);
        s.clear.assign(lows[i].mesh->pointCapacity(), std::numeric_limits<float>::max());
        if (!s.input.soup.triangleCount())
            continue;
        // Samples on a barycentric grid about diag / 128 apart (2 to 8 steps per triangle side).
        const float diag = std::max(glm::length(s.input.soup.boundsMax - s.input.soup.boundsMin), 1e-4f);
        s.margin = float(options.margin) * diag;
        s.cap = 0.25f * diag;
        s.eps = 1e-5f * diag;
        s.spacing = diag / 128.0f;
        margin = std::max(margin, s.margin);
    }
    if (std::ranges::none_of(states, [](const LowState& s) { return s.input.soup.triangleCount() > 0; }))
        throw std::runtime_error("cage estimate: the lows have no faces");
    const Walker walk(*tracer, group);

    // The other lows, for clearance: rays go into each one's own space, where t stays the world distance.
    struct Other
    {
        geo::MeshBvh bvh;
        glm::dmat4 toLocal;
    };
    std::vector<std::unique_ptr<Other>> others(lows.size());
    if (options.clearance && lows.size() > 1)
        tbb::parallel_for(size_t(0), lows.size(), [&](size_t i) {
            if (lows[i].mesh && states[i].input.soup.triangleCount())
                others[i] = std::make_unique<Other>(Other{geo::MeshBvh(*lows[i].mesh), glm::inverse(lows[i].world)});
        });
    auto gapAhead = [&](size_t self, const glm::vec3& p, const glm::vec3& dir, float reach) {
        double best = reach;
        for (size_t j = 0; j < others.size(); ++j)
        {
            if (j == self || !others[j])
                continue;
            const glm::dvec3 o(others[j]->toLocal * glm::dvec4(glm::dvec3(p), 1.0));
            const glm::dvec3 d(others[j]->toLocal * glm::dvec4(glm::dvec3(dir), 0.0));
            geo::BvhHit h;
            if (others[j]->bvh.raycast(o, d, h, best))
                best = std::min(best, h.t);
        }
        return float(best);
    };

    std::vector<float> inward;
    std::mutex inMutex;
    for (LowState& s : states)
    {
        const LowInput& li = s.input;
        const uint32_t lowIndex = uint32_t(&s - states.data());
        const size_t tris = li.soup.triangleCount();
        std::vector<std::vector<Constraint>> perTri(tris);
        // Per corner: the most push before the cage reaches halfway to another low.
        std::vector<float> cornerClear(tris * 3, std::numeric_limits<float>::max());
        const bool clearance = options.clearance && others.size() > 1;
        tbb::parallel_for(size_t(0), tris, [&](size_t t) {
            const glm::vec3* p = &li.soup.positions[t * 3];
            const float longest = std::max({glm::length(p[1] - p[0]), glm::length(p[2] - p[1]), glm::length(p[0] - p[2])});
            const int n = std::clamp(int(std::ceil(longest / s.spacing)), 2, 8);
            std::vector<float> in;
            for (int a = 0; a <= n; ++a)
                for (int b = 0; a + b <= n; ++b)
                {
                    const glm::dvec3 w(double(n - a - b) / n, double(a) / n, double(b) / n);
                    const glm::vec3 P = mix3(p, w);
                    const glm::vec3 dir = cageDirection(li, t, w, skew);
                    const Need need = walk.at(lowIndex, P, dir, s.cap, s.eps);
                    perTri[t].push_back({uint32_t(t), glm::vec3(w), need.outward + s.margin});
                    if (clearance)
                    {
                        const int k = w.x >= w.y && w.x >= w.z ? 0 : w.y >= w.z ? 1 : 2;
                        // Meshes that touch or cross can't be kept apart; only a real gap holds the cage back.
                        const float gap = gapAhead(lowIndex, P, dir, s.cap);
                        if (gap < s.cap && kClearanceShare * gap > s.margin)
                            cornerClear[t * 3 + size_t(k)] = std::min(cornerClear[t * 3 + size_t(k)], kClearanceShare * gap);
                    }
                    if (need.outward <= 0.0f && need.inward >= 0.0f)
                        in.push_back(need.inward);
                }
            if (!in.empty())
            {
                std::scoped_lock lock(inMutex);
                inward.insert(inward.end(), in.begin(), in.end());
            }
        });
        for (auto& c : perTri)
            s.constraints.insert(s.constraints.end(), c.begin(), c.end());
        for (size_t c = 0; c < cornerClear.size(); ++c)
            s.clear[s.cornerPoint[c]] = std::min(s.clear[s.cornerPoint[c]], cornerClear[c]);
    }

    // Lone needs far above the rest (a stray high piece) are clamped; coverage passes put back any that were real.
    std::vector<float> all;
    for (const LowState& s : states)
        for (const Constraint& c : s.constraints)
            all.push_back(c.need);
    const float clampAt = 2.0f * quantile(all, 0.995);
    for (LowState& s : states)
    {
        for (Constraint& c : s.constraints)
            c.need = std::min(c.need, std::max(clampAt, s.margin * 2.0f));
        solve(s, s.margin);
        smoothUp(s);
        clampToClearance(s);
    }
    est.rear = std::max(2.0 * margin, double(quantile(inward, 0.999)) * 1.1 + margin);

    // Frontal puts the typical push at offset 1, and the largest within the attribute's 0-10.
    auto fit = [&] {
        std::vector<float> pushes;
        float most = 0.0f;
        for (const LowState& s : states)
            for (geo::Index p = 0; p < s.source->mesh->pointCapacity(); ++p)
                if (s.source->mesh->pointValid(p))
                {
                    pushes.push_back(s.push[p]);
                    most = std::max(most, s.push[p]);
                }
        double f = std::max(double(margin), double(quantile(pushes, 0.9)));
        f = std::max(f, double(most) / 10.0);
        est.frontal = f / est.cageOffset;
        est.offsets.clear();
        for (LowState& s : states)
        {
            std::vector<float> o(s.push.size(), 1.0f);
            for (size_t p = 0; p < o.size(); ++p)
                o[p] = std::clamp(float(double(s.push[p]) / f), 0.0f, 10.0f);
            for (size_t c = 0; c < s.cornerPoint.size(); ++c)
                s.input.cageScale[c] = o[s.cornerPoint[c]];
            est.offsets.push_back(std::move(o));
        }
    };

    auto cover = [&](std::vector<std::vector<TexelSample>>* missed) {
        CageCoverage total;
        const CageParams cp{float(est.frontal), float(est.rear), float(est.cageOffset), skew};
        for (size_t i = 0; i < states.size(); ++i)
        {
            const CageCoverage c = cageCoverage(states[i].input, uint32_t(i), cp, group, backend, options.resolution, missed ? &(*missed)[i] : nullptr);
            total.samples += c.samples;
            total.misses += c.misses;
            total.backfaces += c.backfaces;
        }
        return total;
    };

    fit();
    std::vector<std::vector<TexelSample>> missed(states.size());
    est.after = cover(&missed);
    CageEstimate best = est;
    for (int it = 0; it < options.iterations && est.after.misses > 0; ++it)
    {
        uint64_t unreachable = 0;
        float rear = float(est.rear);
        bool added = false;
        for (size_t i = 0; i < states.size(); ++i)
        {
            LowState& s = states[i];
            const LowInput& li = s.input;
            for (const TexelSample& m : missed[i])
            {
                const size_t t = m.lowTriangle;
                const glm::dvec3 w = sampleBary(li, m);
                const glm::vec3 P = mix3(&li.soup.positions[t * 3], w);
                const Need need = walk.at(uint32_t(i), P, cageDirection(li, t, w, skew), s.cap, s.eps);
                const geo::Index* cp = &s.cornerPoint[t * 3];
                const float push = s.push[cp[0]] * float(w.x) + s.push[cp[1]] * float(w.y) + s.push[cp[2]] * float(w.z);
                if (!need.anyHit)
                {
                    ++unreachable;
                    continue;
                }
                // A ray that met a back face started inside the high: push past it. A plain miss fell short of the high below.
                float want = need.outward + s.margin;
                if (m.sub)
                    want = std::max(want, push * 1.25f + s.margin);
                if (want > push)
                {
                    s.constraints.push_back({uint32_t(t), glm::vec3(w), want});
                    added = true;
                }
                if (!m.sub && need.inward > rear)
                    rear = need.inward * 1.1f + s.margin;
            }
        }
        est.unreachable = unreachable;
        if (!added && rear <= est.rear)
            break;
        est.rear = rear;
        for (LowState& s : states)
        {
            solve(s, s.margin);
            clampToClearance(s);
        }
        fit();
        for (auto& m : missed)
            m.clear();
        est.after = cover(&missed);
        est.iterations = it + 1;
        if (est.after.misses < best.after.misses)
            best = est;
        else
            best.iterations = est.iterations;
        best.unreachable = est.unreachable;
    }
    return best;
}

CageCoverage groupCoverage(scene::Graph& g, scene::NodeId group, int resolution)
{
    LILA_ZONE();

    const std::unique_ptr<Prepared> p = prepare(g, group);
    const GroupInputs& gi = p->inputs.groups[0];
    const CageParams cp{gi.frontal, gi.rear, float(gi.settings.cageOffset), float(gi.settings.skew)};
    CageCoverage total;
    for (const LowInput& low : gi.lows)
    {
        const CageCoverage c = cageCoverage(low, uint32_t(&low - gi.lows.data()), cp, 0, *p->backend, resolution);
        total.samples += c.samples;
        total.misses += c.misses;
        total.backfaces += c.backfaces;
    }
    return total;
}

json estimateGroupCage(scene::Graph& g, scene::NodeId group, const CageEstimateOptions& options, bool apply)
{
    LILA_ZONE();

    const std::unique_ptr<Prepared> p = prepare(g, group);
    scene::Node* grp = g.node(group);
    GroupSettings s = groupSettings(*grp);
    const GroupInputs& gi = p->inputs.groups[0];

    CageCoverage before;
    {
        const CageParams cp{gi.frontal, gi.rear, float(s.cageOffset), float(s.skew)};
        for (const LowInput& low : gi.lows)
        {
            const CageCoverage c = cageCoverage(low, uint32_t(&low - gi.lows.data()), cp, 0, *p->backend, options.resolution);
            before.samples += c.samples;
            before.misses += c.misses;
            before.backfaces += c.backfaces;
        }
    }

    std::vector<CageLow> lows;
    std::vector<scene::NodeId> lowIds;
    for (const std::string& name : s.lows)
    {
        scene::Node* n = meshByName(g, name);
        if (!n)
            throw std::runtime_error(grp->name() + ": low " + name + " is not a mesh in the scene");
        scene::MeshPtr m = scene::modifiers::exportMesh(g, n->id());
        if (!m)
            m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
        const scene::MeshPtr base = std::get<scene::MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, n->id())));
        if (!m || !base)
            throw std::runtime_error(n->name() + " has no mesh");
        if (m->pointCapacity() != base->pointCapacity())
            throw std::runtime_error(n->name() + ": its modifiers change its points, so per-point cage offsets can't be painted on it");
        lows.push_back({m, worldOf(g, n->id()), n->name()});
        lowIds.push_back(n->id());
    }
    const CageEstimate est = estimateCage(lows, s, p->inputs, 0, *p->backend, options);

    if (apply)
    {
        UndoChunk chunk("Estimate Bake Cage");
        for (size_t i = 0; i < lowIds.size(); ++i)
            if (!setPointValues(g, lowIds[i], *paintAttr(kOffsetAttr), est.offsets[i], "Estimate Bake Cage"))
                throw std::runtime_error("couldn't write bakeOffset on " + g.node(lowIds[i])->name());
        s.frontal = est.frontal;
        s.rear = est.rear;
        s.cageOffset = est.cageOffset;
        setGroupSettings(g, group, s);
        if (!s.cage.empty())
            cageFromPaint(g, group);
    }
    json out = est.toJson();
    out["group"] = grp->path();
    out["before"] = before.toJson();
    out["applied"] = apply;
    return out;
}

void registerCageCommands()
{
    if (ops::find("bakeEstimateCage"))
        return;
    using T = ArgType;
    auto groupOf = [](const Args& a, const char* cmd) -> scene::Node& {
        scene::Graph& g = scene::Graph::current();
        const std::vector<std::string> names = a.objects();
        auto find = [&](const std::string& n) -> scene::Node* {
            try
            {
                return g.find(n);
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        };
        for (const std::string& name : names)
            if (scene::Node* n = find(name); n && isGroup(*n))
                return *n;
        for (const std::string& name : names)
            if (scene::Node* n = find(name); n && n->type().isA("mesh"))
                for (scene::NodeId id : g.nodes())
                    if (scene::Node* grp = g.node(id); grp && isGroup(*grp))
                    {
                        const GroupSettings s = groupSettings(*grp);
                        if (std::ranges::find(s.lows, n->path()) != s.lows.end() || s.cage == n->path())
                            return *grp;
                    }
        throw CommandError(std::string(cmd) + ": name a bake group (or select one, or its low)");
    };

    ops::registerCommand({
        .name = "bakeEstimateCage",
        .doc = "Estimate a bake group's cage in one step: paints bakeOffset on its lows so every texel's ray starts just outside "
               "the high and meets it, and sets the group's frontal and rear distances (an editable cage moves to the result). "
               "Checks itself by tracing the lows as a bake would, growing the cage where rays still miss. One undo step. "
               "Returns the miss rates before and after.",
        .positionalDoc = "group (or its low; default the selection)",
        .flags = {{"resolution", "res", T::Int, 512, "Texels per side of the coverage check.", {}, {}, 16.0, 8192.0},
                  {"iterations", "it", T::Int, 4, "Coverage passes that may grow the cage.", {}, {}, 0.0, 32.0},
                  {"margin", "mg", T::Float, 0.004, "Gap kept between cage and high, as a fraction of each low's bounds diagonal.", {}, {}, 0.0, 1.0},
                  {"apply", "ap", T::Bool, true, "Write the result; False only reports it."}},
        .run = [groupOf](const Args& a) -> json {
            scene::Node& grp = groupOf(a, "bakeEstimateCage");
            CageEstimateOptions o;
            o.resolution = std::clamp(int(a.get<int64_t>("resolution", 512)), 16, 8192);
            o.iterations = std::clamp(int(a.get<int64_t>("iterations", 4)), 0, 32);
            o.margin = std::clamp(a.get<double>("margin", 0.004), 0.0, 1.0);
            try
            {
                return estimateGroupCage(scene::Graph::current(), grp.id(), o, a.get<bool>("apply", true));
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("bakeEstimateCage: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "bakeCageCoverage",
        .doc = "How many of a bake group's rays miss its highs as it would bake now (one sample per texel): samples, misses, "
               "backfaces and missRate.",
        .positionalDoc = "group (or its low; default the selection)",
        .flags = {{"resolution", "res", T::Int, 512, "Texels per side.", {}, {}, 16.0, 8192.0}},
        .run = [groupOf](const Args& a) -> json {
            scene::Node& grp = groupOf(a, "bakeCageCoverage");
            try
            {
                json j = groupCoverage(scene::Graph::current(), grp.id(), std::clamp(int(a.get<int64_t>("resolution", 512)), 16, 8192)).toJson();
                j["group"] = grp.path();
                return j;
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("bakeCageCoverage: ") + e.what());
            }
        },
    });
}
} // namespace lila::bake
