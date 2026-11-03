#include "groom/Settle.h"

#include "core/Profile.h"
#include "groom/Attrs.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lila::groom
{
namespace
{
struct Rod
{
    std::vector<D3> x, prev, rest, start;
    std::vector<double> radius, weight, restLen, restBend;
    // Scalp contact plane under each point for this step (point, normal), valid when near.
    std::vector<D3> hitPoint, hitNormal;
    std::vector<char> near;
    /** Strands of one plait or clump don't push each other apart. */
    int64_t clump = -1;
    /** How much each point collides with other locks: none at the root, where locks overlap as they leave the scalp. */
    std::vector<double> free;
    std::vector<D3> push;
    std::vector<int> pushes;
    /** Each point's follow-the-leader correction this step, taken back out of its parent's velocity (DFTL). */
    std::vector<D3> ftl;
};

std::vector<double> sampleOn(const std::vector<double>& src, size_t from, size_t to, double def)
{
    std::vector<double> out(to, def);
    if (src.empty())
        return out;
    for (size_t i = 0; i < to; ++i)
    {
        const double x = to > 1 ? double(i) * double(std::max<size_t>(from, 1) - 1) / double(to - 1) : 0.0;
        const size_t k = std::min(size_t(x), src.size() - 1);
        const double f = x - double(k);
        out[i] = k + 1 < src.size() ? src[k] + (src[k + 1] - src[k]) * f : src[k];
    }
    return out;
}

/** Each point back at its rest distance from the one before, root first: hair does not stretch. */
void followTheLeader(Rod& r)
{
    for (size_t i = 1; i < r.x.size(); ++i)
    {
        const D3 d = r.x[i] - r.x[i - 1];
        const double l = glm::length(d);
        if (l > 1e-12)
        {
            const D3 to = r.x[i - 1] + d * (r.restLen[i] / l);
            r.ftl[i] += to - r.x[i];
            r.x[i] = to;
        }
    }
}

struct GridKey
{
    int64_t x, y, z;
    bool operator==(const GridKey&) const = default;
};

struct GridHash
{
    size_t operator()(const GridKey& k) const { return size_t(k.x * 73856093 ^ k.y * 19349663 ^ k.z * 83492791); }
};
} // namespace

void settle(Splines& curves, const std::vector<std::vector<double>>& radii, const Surface* scalp, const SettleOptions& o, SettleStats* stats)
{
    LILA_ZONE();

    const size_t nc = curves.size();
    std::vector<Rod> rods(nc);
    // From the typical radius, not the smallest: tapered tips would otherwise sample every lock finely.
    double sumRadius = 0.0;
    size_t radiusCount = 0;
    for (const auto& r : radii)
        for (double v : r)
        {
            sumRadius += v;
            ++radiusCount;
        }
    const double meanRadius = radiusCount ? sumRadius / double(radiusCount) : 0.2;
    const double spacing = o.spacing > 0.0 ? o.spacing : std::clamp(meanRadius * 2.0, 0.3, 3.0);
    std::vector<Spline> work(nc);
    for (size_t c = 0; c < nc; ++c)
    {
        const Spline& s = curves[c];
        if (s.size() < 2)
            continue;
        const int n = std::clamp(int(std::ceil(attr::length(s) / spacing)) + 1, int(s.size()), std::max(96, int(s.size())));
        work[c] = attr::resampled(s, n);
        Rod& r = rods[c];
        r.x = work[c].points;
        r.prev = r.x;
        r.rest = r.x;
        r.start = r.x;
        r.radius = sampleOn(c < radii.size() ? radii[c] : std::vector<double>{}, s.size(), size_t(n), 0.1);
        r.weight = sampleOn(c < o.weights.size() ? o.weights[c] : std::vector<double>{}, s.size(), size_t(n), 1.0);
        r.restLen.assign(size_t(n), 0.0);
        r.restBend.assign(size_t(n), 0.0);
        for (int i = 1; i < n; ++i)
            r.restLen[size_t(i)] = glm::distance(r.x[size_t(i) - 1], r.x[size_t(i)]);
        for (int i = 1; i + 1 < n; ++i)
            r.restBend[size_t(i)] = glm::distance(r.x[size_t(i) - 1], r.x[size_t(i) + 1]);
        r.clump = s.attrs.contains(std::string(attr::kClump)) ? int64_t(attr::get(s, attr::kClump, -1.0)) : -1 - int64_t(c);
        r.free.assign(size_t(n), 1.0);
        for (int i = 0; i < n; ++i)
            r.free[size_t(i)] = glm::smoothstep(0.05, 0.3, double(i) / double(n - 1));
        r.hitPoint.assign(size_t(n), D3(0.0));
        r.hitNormal.assign(size_t(n), D3(0.0));
        r.near.assign(size_t(n), 0);
    }
    const bool haveScalp = scalp && !scalp->empty();
    double maxRadius = 0.0;
    for (const Rod& r : rods)
        for (double v : r.radius)
            maxRadius = std::max(maxRadius, v);
    const double cell = std::max(1e-3, maxRadius * 2.5);
    const double keep = std::clamp(1.0 - o.damping, 0.0, 1.0);
    const double bend = std::clamp(o.bend, 0.0, 1.0) * 0.5, hold = std::clamp(o.holdShape, 0.0, 1.0) * 0.1;
    const double rootK = std::clamp(o.rootStiffness, 0.0, 1.0);
    int contacts = 0;
    double lastMotion = 0.0;
    std::vector<std::pair<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>>> pairs;
    for (int step = 0; step < std::max(0, o.steps); ++step)
    {
        // Predict, then find the scalp plane under each point near it.
        tbb::parallel_for(size_t(0), nc, [&](size_t c) {
            Rod& r = rods[c];
            r.ftl.assign(r.x.size(), D3(0.0));
            for (size_t i = 1; i < r.x.size(); ++i)
            {
                D3 v = (r.x[i] - r.prev[i]) * keep;
                // No point outruns half its segment in a step, so contacts can't tunnel or kick.
                const double vmax = 0.5 * r.restLen[i], speed = glm::length(v);
                if (speed > vmax && speed > 0.0)
                    v *= vmax / speed;
                r.prev[i] = r.x[i];
                r.x[i] += v + o.gravity * (o.dt * o.dt) * r.weight[i];
            }
            if (!haveScalp)
                return;
            for (size_t i = 1; i < r.x.size(); ++i)
            {
                r.near[i] = 0;
                // Unbounded, so a point drawn deep inside the head still finds its way out.
                if (auto h = scalp->closest(r.x[i]))
                {
                    const double reach = r.radius[i] * 3.0 + glm::distance(r.x[i], r.prev[i]) + 0.5;
                    // Inside the mesh the closest point's normal still points out, which is what pushes the point back.
                    if (glm::dot(r.x[i] - h->point, h->normal) < reach)
                    {
                        r.hitPoint[i] = h->point;
                        r.hitNormal[i] = h->normal;
                        r.near[i] = 1;
                    }
                }
            }
        });
        pairs.clear();
        if (o.lockCollisions)
        {
            std::unordered_map<GridKey, std::vector<std::pair<uint32_t, uint32_t>>, GridHash> grid;
            auto keyOf = [&](const D3& p) { return GridKey{int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.y / cell)), int64_t(std::floor(p.z / cell))}; };
            for (uint32_t c = 0; c < nc; ++c)
                for (uint32_t i = 1; i < rods[c].x.size(); ++i)
                    grid[keyOf(rods[c].x[i])].push_back({c, i});
            for (uint32_t c = 0; c < nc; ++c)
                for (uint32_t i = 1; i < rods[c].x.size(); ++i)
                {
                    const GridKey k = keyOf(rods[c].x[i]);
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dz = -1; dz <= 1; ++dz)
                            {
                                auto it = grid.find({k.x + dx, k.y + dy, k.z + dz});
                                if (it == grid.end())
                                    continue;
                                for (auto [c2, i2] : it->second)
                                {
                                    if (c2 <= c || rods[c].free[i] <= 0.0 || rods[c2].free[i2] <= 0.0 || rods[c].clump == rods[c2].clump)
                                        continue;
                                    const double reach = (rods[c].radius[i] + rods[c2].radius[i2]) * 1.5;
                                    if (glm::distance(rods[c].x[i], rods[c2].x[i2]) < reach)
                                        pairs.push_back({{c, i}, {c2, i2}});
                                }
                            }
                }
        }
        for (int it = 0; it < std::max(1, o.iterations); ++it)
        {
            tbb::parallel_for(size_t(0), nc, [&](size_t c) {
                Rod& r = rods[c];
                const size_t n = r.x.size();
                if (n < 2)
                    return;
                r.x[1] += (r.rest[1] - r.x[1]) * rootK;
                for (size_t i = 1; i + 1 < n; ++i)
                {
                    D3 d = r.x[i + 1] - r.x[i - 1];
                    const double l = glm::length(d);
                    if (l < 1e-12)
                        continue;
                    const D3 corr = d * ((l - r.restBend[i]) / l) * bend;
                    if (i - 1 > 0)
                        r.x[i - 1] += corr * 0.5;
                    r.x[i + 1] -= corr * (i - 1 > 0 ? 0.5 : 1.0);
                }
                for (size_t i = 2; i < n; ++i)
                    r.x[i] += (r.rest[i] - r.x[i]) * hold;
                if (haveScalp)
                    for (size_t i = 1; i < n; ++i)
                        if (r.near[i])
                        {
                            const double d = glm::dot(r.x[i] - r.hitPoint[i], r.hitNormal[i]);
                            if (d < r.radius[i])
                                r.x[i] += r.hitNormal[i] * (r.radius[i] - d);
                        }
                followTheLeader(r);
            });
            // Averaged, so a point pressed by several neighbours moves once, not once per neighbour.
            for (Rod& r : rods)
            {
                r.push.assign(r.x.size(), D3(0.0));
                r.pushes.assign(r.x.size(), 0);
            }
            for (const auto& [a, b] : pairs)
            {
                Rod &ra = rods[a.first], &rb = rods[b.first];
                const D3& pa = ra.x[a.second];
                const D3& pb = rb.x[b.second];
                const double k = std::min(ra.free[a.second], rb.free[b.second]);
                if (k <= 0.0)
                    continue;
                const double want = ra.radius[a.second] + rb.radius[b.second];
                D3 d = pb - pa;
                const double l = glm::length(d);
                if (l >= want || l < 1e-9)
                    continue;
                if (haveScalp && ra.near[a.second] && rb.near[b.second])
                {
                    // Lying on the scalp, locks stack rather than shove each other sideways. The later one goes on
                    // top along its whole length, as it was drawn; deciding per point would weave them.
                    const D3 n = glm::normalize(ra.hitNormal[a.second] + rb.hitNormal[b.second] + D3(1e-9));
                    const double ha = glm::dot(pa - ra.hitPoint[a.second], n), hb = glm::dot(pb - rb.hitPoint[b.second], n);
                    const double need = want - (hb - ha);
                    if (need <= 0.0)
                        continue;
                    rb.push[b.second] += n * (need * k * 0.5);
                    ++rb.pushes[b.second];
                    continue;
                }
                // Off the scalp, a gentle shove apart; hanging locks may brush each other.
                const D3 corr = d * ((want - l) / l * 0.5 * k * 0.3);
                ra.push[a.second] -= corr;
                ++ra.pushes[a.second];
                rb.push[b.second] += corr;
                ++rb.pushes[b.second];
            }
            // Spread along each lock before applying, so a contact bends it smoothly instead of kinking one point.
            tbb::parallel_for(size_t(0), nc, [&](size_t c) {
                Rod& r = rods[c];
                const size_t n = r.x.size();
                std::vector<D3> d(n, D3(0.0));
                bool any = false;
                for (size_t i = 1; i < n; ++i)
                    if (r.pushes[i])
                    {
                        d[i] = r.push[i] / double(r.pushes[i]);
                        any = true;
                    }
                if (!any)
                    return;
                for (int pass = 0; pass < 2; ++pass)
                {
                    std::vector<D3> e = d;
                    for (size_t i = 1; i < n; ++i)
                        e[i] = (d[i - 1] + d[i] * 2.0 + d[std::min(i + 1, n - 1)]) * 0.25;
                    d = std::move(e);
                }
                for (size_t i = 1; i < n; ++i)
                    r.x[i] += d[i];
            });
            if (!pairs.empty())
                tbb::parallel_for(size_t(0), nc, [&](size_t c) { followTheLeader(rods[c]); });
        }
        // Contact friction, the brush's mask, and how far things still move.
        double motion = 0.0;
        int touching = 0;
        for (Rod& r : rods)
            for (size_t i = 1; i < r.x.size(); ++i)
            {
                // The parent pays back the correction it caused down the chain, which keeps the strand from gaining energy.
                if (i + 1 < r.x.size())
                    r.prev[i] += r.ftl[i + 1] * 0.9;
                if (haveScalp && r.near[i] && glm::dot(r.x[i] - r.hitPoint[i], r.hitNormal[i]) < r.radius[i] * 1.05)
                {
                    const D3 v = r.x[i] - r.prev[i];
                    const double out = glm::dot(v, r.hitNormal[i]);
                    const D3 tangential = v - r.hitNormal[i] * out;
                    // Friction slows the slide; positions stay where the constraints put them.
                    r.prev[i] += tangential * (1.0 - std::clamp(o.slip, 0.0, 1.0));
                    // A contact pushes out, it doesn't bounce: no outward speed carried into the next step.
                    if (out > 0.0)
                        r.prev[i] += r.hitNormal[i] * out;
                    ++touching;
                }
                if (r.weight[i] < 1.0)
                    r.x[i] = glm::mix(r.start[i], r.x[i], std::clamp(r.weight[i], 0.0, 1.0));
                motion = std::max(motion, glm::distance(r.x[i], r.prev[i]));
            }
        contacts = touching;
        lastMotion = motion;
        if (motion < 1e-4 && step > 8)
            break;
    }
    double penetration = 0.0;
    for (size_t c = 0; c < nc; ++c)
    {
        if (curves[c].size() < 2)
            continue;
        Spline s = work[c];
        s.points = rods[c].x;
        // Points within a thickness of the root sit in the scalp by construction; they don't count.
        double fromRoot = 0.0;
        if (haveScalp)
            for (size_t i = 1; i < s.size(); ++i)
                if ((fromRoot += rods[c].restLen[i]) > rods[c].radius[i] * 2.0)
                    if (auto h = scalp->closest(s.points[i], rods[c].radius[i] * 2.0))
                        penetration = std::max(penetration, rods[c].radius[i] * 0.999 - glm::dot(s.points[i] - h->point, h->normal));
        const Kind kind = curves[c].kind;
        curves[c] = s.size() == curves[c].size() ? s : attr::resampled(s, int(curves[c].size()));
        if (kind != Kind::Poly)
            curves[c] = curve::convert(curves[c], kind, 0.01);
    }
    if (stats)
    {
        stats->curves = int(nc);
        stats->contacts = contacts;
        stats->lastMotion = lastMotion;
        stats->maxPenetration = std::max(0.0, penetration);
    }
}
} // namespace lila::groom
