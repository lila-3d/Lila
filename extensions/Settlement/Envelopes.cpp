// Settlement: building envelopes on lots (setbacks by block type, tiers, roofs, entrances, corners, foundations), party
// walls matched between neighbours, greybox massing, and the JSON consumers read.

#include "Parcels.h"

#include <lila/Curves.h>
#include <lila/Profile.h>
#include <lila/Skeleton.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace settlement
{
namespace
{
json xzOf(Vec2 p) { return json::array({p.x, p.y}); }

json ringOf(const Polygon& p)
{
    json a = json::array();
    for (const Vec2& q : p.pts)
        a.push_back(xzOf(q));
    return a;
}

/** Extent of the polygon across the direction d (perpendicular to it) and along it. */
void spans(const Polygon& p, Vec2 d, double& across, double& along)
{
    const Vec2 n = leftNormal(d);
    double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
    for (const Vec2& q : p.pts)
    {
        a0 = std::min(a0, glm::dot(q, n)), a1 = std::max(a1, glm::dot(q, n));
        b0 = std::min(b0, glm::dot(q, d)), b1 = std::max(b1, glm::dot(q, d));
    }
    across = a1 - a0;
    along = b1 - b0;
}

Polygon insetBy(const Polygon& p, double d)
{
    const std::vector<double> ds(p.size(), d);
    return inset(p, ds);
}

/** Ground under the footprint: its corners and a grid inside it no coarser than the ground's cells. */
void groundUnder(const Polygon& fp, const Ground& g, double& lo, double& hi)
{
    lo = std::numeric_limits<double>::max();
    hi = -lo;
    auto take = [&](Vec2 q) {
        const double h = g.height(q);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    };
    for (const Vec2& q : fp.pts)
        take(q);
    take(centroid(fp));
    if (g.isPlane())
        return;
    Vec2 x0(1e300), x1(-1e300);
    for (const Vec2& q : fp.pts)
        x0 = glm::min(x0, q), x1 = glm::max(x1, q);
    const double step = std::clamp(std::min(g.cellSize(), std::max(x1.x - x0.x, x1.y - x0.y) / 6.0), 50.0, 2000.0);
    for (double z = x0.y; z <= x1.y + 1e-6; z += step)
        for (double x = x0.x; x <= x1.x + 1e-6; x += step)
            if (contains(fp.pts, {x, z}))
                take({x, z});
    for (size_t k = 0; k < fp.size(); ++k)
        take((fp.pts[k] + fp.pts[(k + 1) % fp.size()]) * 0.5);
}

/** The corner between two street sides cut off square to its bisector. */
Polygon chamfer(const Polygon& fp, double size)
{
    Polygon out = fp;
    for (size_t k = 0; k < out.size(); ++k)
    {
        const size_t n = out.size(), prev = (k + n - 1) % n;
        if (out.tag[prev] != 0 || out.tag[k] != 0)
            continue;
        const Vec2 a = out.pts[k];
        const Vec2 e1 = out.pts[prev] - a, e2 = out.pts[(k + 1) % n] - a;
        if (glm::length(e1) < 3.0 * size || glm::length(e2) < 3.0 * size)
            continue;
        const Vec2 u1 = glm::normalize(e1), u2 = glm::normalize(e2);
        if (glm::dot(u1, u2) > 0.5 || cross(u2, u1) > 0.0)
            continue;
        const Vec2 b = glm::normalize(u1 + u2);
        Polygon cut = clipHalfPlane(out, a + b * size, b, 0);
        if (cut.size() >= 3)
            return cut;
    }
    return out;
}

const char* sideName2(int tag) { return tag == 0 ? "street" : tag == 1 ? "party" : "open"; }
} // namespace

Envelope parcelEnvelope(const Lot& lot, const Block& block, const Layout& l, const Ground& g, const Options& o, const Site& site)
{
    const Profile& p = o.profile; // an era or district restyle passes its own profile here
    const std::string era = eraOf(p);
    const bool pm = preModern(era);
    const bool landmark = !lot.landmark.empty();
    double front = p.frontSetback, side = 0.0, depth = p.buildDepth;
    switch (block.type)
    {
    case BlockType::Open:
        front = std::max(p.frontSetback, 300.0);
        side = std::max(p.sideSetback, 200.0);
        break;
    case BlockType::Superblock:
        front = std::max(p.frontSetback, 800.0);
        side = std::max(p.sideSetback, 800.0);
        depth = 1.5 * p.buildDepth;
        break;
    case BlockType::Courtyard: depth = std::min(p.buildDepth, p.lotDepth); break;
    default: break;
    }
    if (landmark && (lot.use == Use::Religious || lot.use == Use::Civic))
    {
        front = std::max(front, 300.0);
        side = std::max(side, 300.0);
        depth = 1.6 * p.buildDepth;
    }
    Options local = o;
    local.profile = p;
    local.profile.frontSetback = front;
    local.profile.sideSetback = side;
    local.profile.buildDepth = depth;
    Envelope env = envelopeFor(lot, l, g, local);
    if (env.footprint.pts.empty())
        return env;
    env.key = lot.key;
    env.frontSetback = front;
    env.sideSetback = side;
    env.landmark = lot.landmark;
    env.eraName = era;
    env.wealth = lot.wealth;
    env.route = lot.route;
    for (int& t : env.footprint.tag)
        t = t == 0 ? 0 : 2;

    const Vec2 c0 = centroid(env.footprint);
    const double dist = std::clamp(glm::length(c0 - l.center) / std::max(l.radius, 1.0), 0.0, 1.0);
    if (lot.corner)
        env.corner = era == "early" ? (landmark ? "tower" : "chamfer") : "wrap";
    if (env.corner == "chamfer")
    {
        double across = 0.0, along = 0.0;
        spans(env.footprint, glm::normalize(Vec2(-env.front.y, env.front.x)), across, along);
        env.footprint = chamfer(env.footprint, std::clamp(0.15 * std::min(across, along), 100.0, 400.0));
    }

    int floors = env.floors;
    double storey = p.floorHeight;
    if (!pm && lot.use != Use::Religious)
        floors = std::max(1, int(std::lround(floors * (0.7 + 0.6 * lot.access))));
    switch (lot.use)
    {
    case Use::Religious: floors = std::max(floors, 2), storey *= 1.6; break;
    case Use::Industrial: floors = std::clamp(floors, 1, 3), storey *= 1.4; break;
    case Use::Port: floors = std::clamp(floors, 2, 4); break;
    case Use::Market: floors = std::clamp(floors, 1, 2), storey *= 1.3; break;
    default: break;
    }
    env.floors = floors;
    env.height = floors * storey;

    double lo = 0.0, hi = 0.0;
    groundUnder(env.footprint, g, lo, hi);
    env.foundationSource = "ground";
    if (auto it = site.foundations.find(lot.key); it != site.foundations.end())
    {
        lo = std::min(lo, it->second.min);
        hi = std::max(hi, it->second.max);
        env.foundationSource = "terrainRegionStats";
    }
    env.ground = lo;
    env.base = lo - p.foundation;
    env.foundationDepth = hi - env.base;
    env.top = hi + env.height;

    // Tall buildings step back as they rise (the setback towers of zoning codes).
    Polygon tierFp = env.footprint;
    double bottom = env.base;
    if (!pm && floors > 10)
    {
        double across = 0.0, along = 0.0;
        spans(tierFp, glm::normalize(Vec2(-env.front.y, env.front.x)), across, along);
        const double step = std::clamp(0.1 * std::min(across, along), 200.0, 600.0);
        const double tops[] = {hi + std::max(6, int(std::lround(0.4 * floors))) * storey, hi + std::lround(0.75 * floors) * storey};
        for (double t : tops)
        {
            Polygon next = insetBy(tierFp, step);
            if (t >= env.top || next.size() < 3 || area(next) < 0.3 * area(tierFp))
                break;
            env.tiers.push_back({tierFp, bottom, t});
            bottom = t;
            tierFp = std::move(next);
        }
    }
    env.tiers.push_back({tierFp, bottom, env.top});

    const bool tall = env.tiers.size() > 1;
    // Otherwise the style's own roof stands: an era or district profile names the roof its buildings take.
    if (tall || (lot.use == Use::Industrial && !pm))
        env.roof = "flat";
    else if (lot.use == Use::Religious)
        env.roof = era == "early" && landmark ? "dome" : "gable";
    else if (lot.use == Use::Civic && landmark)
        env.roof = era == "early" ? "dome" : "hip";
    else if (env.roof != "gable" && env.roof != "hip" && env.roof != "flat" && env.roof != "mansard" && env.roof != "dome")
        env.roof = "gable";
    const double pitch = (era == "medieval" ? 50.0 : era == "early" ? 40.0 : era == "industrial" ? 35.0 : 30.0) + (lot.use == Use::Religious ? 5.0 : 0.0);
    const Vec2 along = glm::normalize(Vec2(-env.front.y, env.front.x));
    Vec2 bc{0.0}, bu{1.0, 0.0}, bv{0.0, 1.0};
    double hu = 0.0, hv = 0.0;
    boxOf(tierFp, bc, bu, bv, hu, hv);
    if (landmark || block.type == BlockType::Open || block.type == BlockType::Superblock)
        env.ridge = bu;
    else if (era == "medieval")
        env.ridge = env.front; // gable end to the street
    else
        env.ridge = along;
    double across = 0.0, ridgeLen = 0.0;
    spans(tierFp, env.ridge, across, ridgeLen);
    if (env.roof == "gable" || env.roof == "hip")
        env.roofHeight = 0.5 * across * std::tan(pitch * kPi / 180.0);
    else if (env.roof == "mansard")
        env.roofHeight = 0.9 * p.floorHeight + 0.25 * across * std::tan(20.0 * kPi / 180.0);
    else if (env.roof == "dome")
        env.roofHeight = std::min(hu, hv);

    std::vector<std::pair<double, Vec2>> doors;
    for (size_t k = 0; k < env.footprint.size(); ++k)
        if (env.footprint.tag[k] == 0)
        {
            const Vec2 a = env.footprint.pts[k], b = env.footprint.pts[(k + 1) % env.footprint.size()];
            if (glm::length(b - a) >= 100.0)
                doors.push_back({glm::length(b - a), (a + b) * 0.5});
        }
    if (doors.empty())
    {
        // A street side shorter than a door after setbacks: the door goes on the side that faces the street most.
        double best = -2.0;
        for (size_t k = 0; k < env.footprint.size(); ++k)
        {
            const Vec2 a = env.footprint.pts[k], b = env.footprint.pts[(k + 1) % env.footprint.size()];
            if (glm::length(b - a) < 1e-6)
                continue;
            const Vec2 out(glm::normalize(b - a).y, -glm::normalize(b - a).x);
            if (const double f = glm::dot(out, env.front); f > best)
            {
                best = f;
                doors = {{glm::length(b - a), (a + b) * 0.5}};
            }
        }
    }
    std::stable_sort(doors.begin(), doors.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (const auto& [len, at] : doors)
        env.entrances.push_back(at);

    env.groundUse = lot.use;
    if (lot.use == Use::Market)
        env.groundUse = Use::Commercial;
    else if (lot.use == Use::Residential && lot.access >= 0.6 && dist < p.commercial && lot.frontClass <= StreetClass::Collector)
        env.groundUse = Use::Commercial;
    env.partyWith.assign(env.footprint.size(), -1);
    return env;
}

void frontageFit(const Envelope& env, double& width, double& depth, Vec2& centre)
{
    const Vec2 along(-env.front.y, env.front.x);
    double a0 = 1e300, a1 = -1e300, d0 = 1e300, d1 = -1e300;
    for (const Vec2& q : env.footprint.pts)
    {
        a0 = std::min(a0, glm::dot(q, along)), a1 = std::max(a1, glm::dot(q, along));
        d0 = std::min(d0, glm::dot(q, env.front)), d1 = std::max(d1, glm::dot(q, env.front));
    }
    width = a1 - a0;
    depth = d1 - d0;
    centre = along * (0.5 * (a0 + a1)) + env.front * (0.5 * (d0 + d1));
}

Placement libraryPlacement(const Envelope& env, const LibraryPick& pick)
{
    double w = 0.0, d = 0.0;
    Vec2 c{0.0};
    frontageFit(env, w, d, c);
    const double turn = (std::atan2(env.front.x, env.front.y) * 180.0 / kPi - pick.assetHeading) * kPi / 180.0;
    Placement p;
    p.id = "building/" + std::to_string(env.key);
    p.asset = pick.unreal.empty() ? pick.asset : pick.unreal;
    p.position = {c.x, env.ground, c.y};
    p.rotation = {0.0, std::sin(0.5 * turn), 0.0, std::cos(0.5 * turn)};
    p.tags = {"building", useName(env.use)};
    if (!env.eraName.empty())
        p.tags.push_back(env.eraName);
    if (!env.landmark.empty())
        p.tags.push_back(env.landmark);
    if (pick.unreal.empty())
        p.tags.push_back("noEngineAsset");
    return p;
}

void matchPartyWalls(std::vector<Envelope>& envs)
{
    LILA_ZONE();

    constexpr double kCell = 5000.0;
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> cells;
    std::vector<std::pair<Vec2, Vec2>> boxes(envs.size());
    std::vector<std::vector<double>> shared(envs.size()); // per side, the longest stretch shared so far
    for (size_t i = 0; i < envs.size(); ++i)
    {
        Envelope& e = envs[i];
        e.partyWith.assign(e.footprint.size(), -1);
        for (int& t : e.footprint.tag)
            t = t == 0 ? 0 : 2;
        shared[i].assign(e.footprint.size(), 0.0);
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : e.footprint.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        boxes[i] = {lo - Vec2(5.0), hi + Vec2(5.0)};
        for (int64_t x = int64_t(std::floor(boxes[i].first.x / kCell)); x <= int64_t(std::floor(boxes[i].second.x / kCell)); ++x)
            for (int64_t z = int64_t(std::floor(boxes[i].first.y / kCell)); z <= int64_t(std::floor(boxes[i].second.y / kCell)); ++z)
                cells[{x, z}].push_back(i);
    }
    std::vector<std::pair<size_t, size_t>> pairs;
    for (auto& [key, ids] : cells)
        for (size_t a = 0; a < ids.size(); ++a)
            for (size_t b = a + 1; b < ids.size(); ++b)
                pairs.push_back({std::min(ids[a], ids[b]), std::max(ids[a], ids[b])});
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    for (const auto& [i, j] : pairs)
    {
        if (boxes[i].second.x < boxes[j].first.x || boxes[j].second.x < boxes[i].first.x || boxes[i].second.y < boxes[j].first.y ||
            boxes[j].second.y < boxes[i].first.y)
            continue;
        Polygon& A = envs[i].footprint;
        Polygon& B = envs[j].footprint;
        for (size_t a = 0; a < A.size(); ++a)
        {
            if (A.tag[a] == 0)
                continue;
            const Vec2 pa = A.pts[a], qa = A.pts[(a + 1) % A.size()];
            const double la = glm::length(qa - pa);
            if (la < 1.0)
                continue;
            const Vec2 ua = (qa - pa) / la;
            for (size_t b = 0; b < B.size(); ++b)
            {
                if (B.tag[b] == 0)
                    continue;
                const Vec2 pb = B.pts[b], qb = B.pts[(b + 1) % B.size()];
                const double lb = glm::length(qb - pb);
                if (lb < 1.0 || glm::dot(ua, (qb - pb) / lb) > -0.995 || std::abs(cross(ua, pb - pa)) > 5.0 || std::abs(cross(ua, qb - pa)) > 5.0)
                    continue;
                const double t0 = glm::dot(pb - pa, ua), t1 = glm::dot(qb - pa, ua);
                const double overlap = std::min(la, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1));
                if (overlap < 100.0)
                    continue;
                A.tag[a] = 1;
                B.tag[b] = 1;
                if (overlap > shared[i][a])
                {
                    shared[i][a] = overlap;
                    envs[i].partyWith[a] = envs[j].lot;
                }
                if (overlap > shared[j][b])
                {
                    shared[j][b] = overlap;
                    envs[j].partyWith[b] = envs[i].lot;
                }
            }
        }
    }
}

bool appendMassing(const Envelope& env, const Layout& l, geo::Mesh& into, const std::function<void(int part)>& tagFace)
{
    auto face = [&](const std::vector<geo::Index>& ids, int part) {
        if (into.addFace(std::span<const geo::Index>(ids)) != geo::kInvalid)
            tagFace(part);
    };
    auto ring = [&](const Polygon& fp, double y) {
        std::vector<geo::Index> ids;
        for (const Vec2& q : fp.pts)
            ids.push_back(into.addPoint({q.x, y, q.y}));
        return ids;
    };
    auto cap = [&](const Polygon& fp, double y, int part) {
        std::vector<geo::Index> top = ring(fp, y);
        std::reverse(top.begin(), top.end());
        face(top, part);
    };
    /** Sides between two rings with the same corner count, lo below hi. */
    auto loft = [&](const Polygon& a, double ya, const Polygon& b, double yb, int part) {
        const std::vector<geo::Index> lo = ring(a, ya), hi = ring(b, yb);
        for (size_t k = 0; k < lo.size(); ++k)
        {
            const size_t j = (k + 1) % lo.size();
            face({lo[k], hi[k], hi[j], lo[j]}, part);
        }
    };
    const std::vector<Envelope::Tier>& tiers = env.tiers;
    for (size_t t = 0; t < tiers.size(); ++t)
    {
        const Envelope::Tier& tier = tiers[t];
        loft(tier.footprint, tier.bottom, tier.footprint, tier.top, 0);
        face(ring(tier.footprint, tier.bottom), 2);
        if (t + 1 < tiers.size())
            cap(tier.footprint, tier.top, 1);
    }
    if (tiers.empty())
        return true;
    bool built = true;
    const Polygon& top = tiers.back().footprint;
    const double y = tiers.back().top;
    double across = 0.0, along = 0.0;
    spans(top, env.ridge, across, along);
    auto skeletonRoof = [&](const Polygon& fp, double at, double pitch, bool gables) {
        lila::curve::Spline s;
        s.kind = lila::curve::Kind::Poly;
        s.closed = true;
        for (const Vec2& q : fp.pts)
            s.points.push_back({q.x, at, q.y});
        lila::curve::RoofOptions ro;
        ro.pitch = pitch;
        ro.normal = {0.0, 1.0, 0.0};
        if (gables)
            for (size_t k = 0; k < fp.size(); ++k)
            {
                const Vec2 d = fp.pts[(k + 1) % fp.size()] - fp.pts[k];
                if (glm::length(d) > 1e-6 && std::abs(glm::dot(glm::normalize(d), env.ridge)) < 0.35)
                    ro.gables.push_back(int(k));
            }
        const lila::curve::RoofResult r = lila::curve::roof({s}, ro);
        if (r.mesh.numFaces() == 0)
            return false;
        std::vector<geo::Index> remap(r.mesh.pointCapacity(), geo::kInvalid);
        for (geo::Index pt = 0; pt < r.mesh.pointCapacity(); ++pt)
            if (r.mesh.pointValid(pt))
                remap[pt] = into.addPoint(r.mesh.position(pt));
        for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
            if (r.mesh.faceValid(f))
            {
                std::vector<geo::Index> ids;
                for (geo::Index pt : r.mesh.facePoints(f))
                    ids.push_back(remap[pt]);
                face(ids, 1);
            }
        return true;
    };
    const double pitch = across > 1e-6 ? std::atan(2.0 * env.roofHeight / across) * 180.0 / kPi : 30.0;
    if (env.roof == "gable" || env.roof == "hip")
    {
        if (!skeletonRoof(top, y, pitch, env.roof == "gable"))
        {
            cap(top, y, 1);
            built = false;
        }
    }
    else if (env.roof == "mansard")
    {
        const Polygon upper = insetBy(top, std::clamp(0.2 * across, 60.0, 250.0));
        const double y1 = y + 0.9 * l.profile.floorHeight;
        if (upper.size() == top.size())
        {
            loft(top, y, upper, y1, 1);
            if (!skeletonRoof(upper, y1, 20.0, false))
            {
                cap(upper, y1, 1);
                built = false;
            }
        }
        else
        {
            cap(top, y, 1);
            built = false;
        }
    }
    else if (env.roof == "dome")
    {
        const double r = std::max(env.roofHeight, 1.0);
        Polygon prev = top;
        double prevY = y;
        bool closed = false;
        for (int k = 1; k <= 6; ++k)
        {
            const double a = k * kPi / 12.0;
            const Polygon next = insetBy(top, r * (1.0 - std::cos(a)));
            if (next.size() != prev.size() || area(next) < 1.0)
            {
                cap(prev, prevY, 1);
                closed = true;
                break;
            }
            loft(prev, prevY, next, y + r * std::sin(a), 1);
            prev = next;
            prevY = y + r * std::sin(a);
        }
        if (!closed)
            cap(prev, prevY, 1);
    }
    else
        cap(top, y, 1);
    return built;
}

json blockJson(const Block& b)
{
    json yards = json::array();
    for (const Polygon& y : b.courtyards)
        yards.push_back(ringOf(y));
    return {{"blockId", b.key},
            {"type", blockTypeName(b.type)},
            {"method", lotMethodName(b.method)},
            {"use", useName(b.use)},
            {"areaM2", b.area},
            {"water", b.water},
            {"center", xzOf(centroid(b.outline))},
            {"courtyards", yards},
            {"reasons", b.reasons}};
}

json lotExtras(const Lot& lot)
{
    static const char* kWealth[] = {"low", "middle", "high"};
    return {{"lotId", lot.key},
            {"wealth", kWealth[std::clamp(lot.wealth, 0, 2)]},
            {"access", lot.access},
            {"landmark", lot.landmark},
            {"reasons", lot.reasons},
            {"route", routeName(lot.route)}};
}

json envelopeExtras(const Envelope& env)
{
    json sides = json::array(), tiers = json::array(), doors = json::array();
    for (int t : env.footprint.tag)
        sides.push_back(sideName2(t));
    for (const Envelope::Tier& t : env.tiers)
        tiers.push_back({{"footprint", ringOf(t.footprint)}, {"bottom", t.bottom}, {"top", t.top}});
    for (const Vec2& d : env.entrances)
        doors.push_back(xzOf(d));
    return {{"lotId", env.key},
            {"sides", sides},
            {"partyWith", env.partyWith},
            {"frontSetback", env.frontSetback},
            {"sideSetback", env.sideSetback},
            {"tiers", tiers},
            {"ridge", xzOf(env.ridge)},
            {"roofHeight", env.roofHeight},
            {"entrances", doors},
            {"groundUse", useName(env.groundUse)},
            {"corner", env.corner},
            {"foundationDepth", env.foundationDepth},
            {"foundationSource", env.foundationSource},
            {"landmark", env.landmark},
            {"eraName", env.eraName},
            {"wealth", env.wealth},
            {"route", routeName(env.route)}};
}

json parcelStats(const Layout& l)
{
    json types = json::object(), methods = json::object(), uses = json::object(), wealth = json::object(), roofs = json::object();
    for (const Block& b : l.blocks)
    {
        types[blockTypeName(b.type)] = types.value(blockTypeName(b.type), 0) + 1;
        methods[lotMethodName(b.method)] = methods.value(lotMethodName(b.method), 0) + 1;
    }
    std::vector<double> areas;
    json landmarks = json::array();
    int landlocked = 0;
    for (const Lot& lot : l.lots)
    {
        uses[useName(lot.use)] = uses.value(useName(lot.use), 0) + 1;
        if (lot.use == Use::Residential)
        {
            const char* w = lot.wealth <= 0 ? "low" : lot.wealth == 1 ? "middle" : "high";
            wealth[w] = wealth.value(w, 0) + 1;
        }
        if (lot.frontages == 0)
            ++landlocked;
        if (!lot.landmark.empty())
            landmarks.push_back({{"name", lot.landmark}, {"lot", lot.id}, {"lotId", lot.key}, {"use", useName(lot.use)},
                                 {"center", xzOf(centroid(lot.outline))}, {"reasons", lot.reasons}});
        else if (lot.use != Use::Open && lot.use != Use::Farm)
            areas.push_back(lot.area);
    }
    int party = 0;
    for (const Envelope& env : l.envelopes)
    {
        roofs[env.roof] = roofs.value(env.roof, 0) + 1;
        party += int(std::ranges::count(env.footprint.tag, 1));
    }
    std::sort(areas.begin(), areas.end());
    auto q = [&](double f) { return areas.empty() ? 0.0 : areas[std::min(areas.size() - 1, size_t(f * double(areas.size())))]; };
    const double mean = areas.empty() ? 0.0 : std::accumulate(areas.begin(), areas.end(), 0.0) / double(areas.size());
    return {{"blockTypes", types},
            {"lotMethods", methods},
            {"lotUses", uses},
            {"residentialWealth", wealth},
            {"lotAreaM2", {{"count", areas.size()}, {"mean", mean}, {"p10", q(0.1)}, {"p50", q(0.5)}, {"p90", q(0.9)}, {"max", areas.empty() ? 0.0 : areas.back()}}},
            {"landlockedLots", landlocked},
            {"landmarks", landmarks},
            {"roofs", roofs},
            {"partyWallSides", party},
            {"warnings", l.parcelWarnings}};
}
} // namespace settlement
