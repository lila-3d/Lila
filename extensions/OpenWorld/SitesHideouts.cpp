// Site kits for run-down hideouts: junkyard, shantytown, unfinished building, fishing camp, derelict warehouse, ranch,
// cockfight arena and impound lot. Each also places gameplay markers: loot points, guard posts and the job vehicle.

#include "SitesKit.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace openworld::sites
{
namespace
{
/**
 * A graded yard x in [-W/2, W/2], z in [0, D]: fenced, its gate in the middle of the front (z = 0) and a track running off
 * toward -z.
 */
void fencedYard(Kit& k, double W, double D, const char* fence, double fenceScale, const char* track)
{
    k.ground("pad", "terrainStampPad",
             {{"footprint", pts(rect(-W * 0.5 - 200.0, -200.0, W * 0.5 + 200.0, D + 200.0))}, {"mode", "flatten"}, {"falloff", 1200.0}, {"masks", {"cityFootprint"}}});
    Part& p = k.part("yard", "yard", rect(-W * 0.5, 0.0, W * 0.5, D), Part::Needs::Land);
    p.maxRelief = 250.0;
    p.groundOp = "pad";
    k.alongRing("fence", fence, rect(-W * 0.5, 0.0, W * 0.5, D), true, 300.0, {{W * 0.5 - 350.0, W * 0.5 + 350.0}}, fenceScale);
    k.slot("gate", "gate.vehicle", {0.0, 0.0}).scale = {700.0 / 600.0, 1.0, 1.0};
    if (track)
        k.ground("access", "terrainAddRoad",
                 {{"kind", track}, {"layer", "Sites"}, {"surface", "dirt"}, {"path", json::array({pt({0.0, 200.0}), pt({0.0, -8000.0})})}});
}

EnvelopeOut& shed(Kit& k, const std::string& path, const Ring& fp, const char* style, const char* program, const char* use, double height,
                  const char* roof, P2 front)
{
    EnvelopeOut& e = k.envelope(path, fp, style, program, use, 1, height, roof);
    e.front = front;
    const P2 c = centroidOf(fp);
    double reach = 0.0;
    for (P2 q : fp)
        reach = std::max(reach, std::abs(glm::dot(q - c, front)));
    e.entrances = {c + front * reach};
    return e;
}

void loot(Kit& k, const std::string& id, P2 at, const char* what) { k.slot("loot/" + id, "marker.loot", at, 0.0, 50.0).data = {{"loot", what}}; }

void guard(Kit& k, const std::string& id, P2 at, P2 faces) { k.slot("guard/" + id, "marker.guard", at, facing(faces)); }

void job(Kit& k, const std::string& id, P2 at, const char* name) { k.slot("job/" + id, "marker.job", at).data = {{"job", name}}; }

bool clearOf(const std::vector<Ring>& taken, const Ring& r)
{
    return std::ranges::none_of(taken, [&](const Ring& o) { return ringsOverlap(o, r); });
}

/** Floodlight masts at the yard's corners, set in from the fence. */
void cornerMasts(Kit& k, double W, double D)
{
    const P2 corners[4] = {{-W * 0.5 + 300.0, 300.0}, {W * 0.5 - 300.0, 300.0}, {W * 0.5 - 300.0, D - 300.0}, {-W * 0.5 + 300.0, D - 300.0}};
    for (int c = 0; c < 4; ++c)
        k.slot("light/" + std::to_string(c), "light.mast", corners[c]);
}

/**
 * Parked vehicles in rows across the yard between z0 and z1, noses to the aisles, skipping anything taken. Each slot's type
 * comes from typeOf(index). Returns how many fitted.
 */
int parkRows(Kit& k, const std::string& prefix, double x0, double x1, double z0, double z1, int count, const std::vector<Ring>& taken,
             const std::function<std::string(int)>& typeOf, int stack = 1)
{
    constexpr double kBay = 260.0, kDepth = 520.0, kAisle = 650.0;
    int n = 0;
    for (int row = 0; n < count; ++row)
    {
        // Rows back to back in pairs, an aisle between pairs.
        const double z = z0 + (row / 2) * (2.0 * kDepth + kAisle) + (row % 2) * kDepth + kDepth * 0.5;
        if (z + kDepth * 0.5 > z1)
            break;
        const P2 nose(0.0, row % 2 ? 1.0 : -1.0);
        for (double x = x0 + kBay * 0.5; x + kBay * 0.5 <= x1 && n < count; x += kBay)
        {
            if (!clearOf(taken, orientedRect({x, z}, kBay * 0.5, kDepth * 0.5, 0.0)))
                continue;
            const int levels = stack > 1 ? k.pick(1, stack, uint64_t(row), uint64_t(int64_t(x))) : 1;
            for (int lv = 0; lv < levels && n < count; ++lv, ++n)
            {
                const std::string id = prefix + "/" + std::to_string(row) + "_" + std::to_string(int(std::lround((x - x0) / kBay))) + (lv ? "_" + std::to_string(lv) : "");
                SlotOut& s = k.slot(id, typeOf(n), {x, z}, facing(nose) + (k.rnd(uint64_t(n), 7) - 0.5) * (stack > 1 ? 0.3 : 0.06), lv * 135.0);
                s.tags = {"row"};
            }
        }
    }
    return n;
}
} // namespace

void buildJunkyard(Kit& k)
{
    const Spec& s = k.spec;
    const double W = s.f("junkWidth"), D = s.f("junkDepth");
    k.L.boundary = rect(-W * 0.5 - 300.0, -1500.0, W * 0.5 + 300.0, D + 300.0);
    fencedYard(k, W, D, "fence.chainlink", 1.0, "track");

    std::vector<Ring> taken;
    const Ring office = rect(W * 0.5 - 1500.0, 500.0, W * 0.5 - 400.0, 1300.0);
    shed(k, "office", office, "ruralShack", "office", "commercial", 260.0, "flat", {-1.0, 0.0}).name = s.name;
    taken.push_back(rect(office[0].x - 300.0, office[0].y - 300.0, office[2].x + 300.0, office[2].y + 300.0));
    const double sw = std::min(2600.0, W * 0.35);
    const Ring chop = rect(-W * 0.5 + 400.0, D - 2600.0, -W * 0.5 + 400.0 + sw, D - 400.0);
    shed(k, "chopShop", chop, "warehouse", "chopShop", "industrial", 480.0, "gable", {1.0, 0.0});
    taken.push_back(rect(chop[0].x, chop[0].y - 300.0, chop[2].x + 500.0, chop[2].y));
    // Keep the gate lane and the tow truck's spot free.
    taken.push_back(rect(-2600.0, 0.0, 600.0, 1800.0));

    const int wrecks = parkRows(k, "wreck", -W * 0.5 + 400.0, W * 0.5 - 400.0, 2000.0, D - 300.0, s.i("wrecks"), taken,
                                [](int) { return std::string("vehicle.wreck"); }, s.i("stackHeight"));
    if (wrecks < s.i("wrecks"))
        k.L.notes.push_back("only " + std::to_string(wrecks) + " of " + std::to_string(s.i("wrecks")) + " wrecks fit: enlarge the yard or stack higher");
    k.L.stats["wrecks"] = wrecks;

    for (int i = 0; i < 4; ++i)
        k.slot("tyres/" + std::to_string(i), "pile.tyres", {W * 0.5 - 600.0, D * 0.35 + i * (D * 0.15)});
    for (int i = 0; i < 3; ++i)
        k.slot("scrap/" + std::to_string(i), "pile.scrap", {-W * 0.5 + 700.0, 900.0 + i * 600.0});
    cornerMasts(k, W, D);

    if (s.b("towTruck"))
    {
        const P2 at(-1500.0, 900.0);
        k.slot("towTruck", "truck.tow", at, facing({0.0, -1.0}));
        k.slot("towTruck/car", "vehicle.car", at + P2(0.0, 150.0), facing({0.0, -1.0}), 120.0);
        job(k, "towTruck", at + P2(250.0, 0.0), "towTruck");
    }
    loot(k, "office", centroidOf(office), "cash");
    loot(k, "chopShop", centroidOf(chop), "stereos");
    loot(k, "yard", {W * 0.25, D * 0.5}, "guns");
    guard(k, "gate", {600.0, 400.0}, {0.0, -1.0});
    guard(k, "aisle", {0.0, D * 0.55}, {1.0, 0.0});
    guard(k, "chopShop", {chop[1].x + 300.0, (chop[0].y + chop[2].y) * 0.5}, {1.0, 0.0});
}

void buildShantytown(Kit& k)
{
    const Spec& s = k.spec;
    const double R = s.f("shantyRadius");
    const int want = s.i("shacks");
    k.L.boundary = circle({0.0, 0.0}, R + 1500.0, 24);
    for (int lane = 0; lane < 2; ++lane)
    {
        const P2 d = lane ? P2(0.0, 1.0) : P2(1.0, 0.0), n(-d.y, d.x);
        const P2 m = n * (k.rnd(3, uint64_t(lane)) - 0.5) * 800.0;
        k.ground("lane/" + std::to_string(lane), "terrainAddRoad",
                 {{"kind", "track"}, {"profile", "track"}, {"layer", "Sites"}, {"surface", "dirt"}, {"path", json::array({pt(-d * (R + 1000.0)), pt(m), pt(d * (R + 1000.0))})}});
        k.street("street:" + std::to_string(lane + 1), 3);
    }
    {
        Part& g = k.part("settlement", "plot", circle({0.0, 0.0}, R, 24), Part::Needs::Land);
        g.maxSlope = 35.0;
    }

    // Shacks on a jittered grid, off the two lanes, densest in the middle.
    constexpr double kCell = 850.0, kLane = 650.0;
    std::vector<P2> spots;
    for (double x = -R; x <= R; x += kCell)
        for (double z = -R; z <= R; z += kCell)
        {
            const P2 c(x + (k.rnd(uint64_t(int64_t(x)), uint64_t(int64_t(z)), 1) - 0.5) * 250.0, z + (k.rnd(uint64_t(int64_t(x)), uint64_t(int64_t(z)), 2) - 0.5) * 250.0);
            if (glm::length(c) > R || std::abs(c.x) < kLane + 300.0 || std::abs(c.y) < kLane + 300.0)
                continue;
            spots.push_back(c);
        }
    std::ranges::sort(spots, [](P2 a, P2 b) { return glm::length(a) < glm::length(b); });
    std::vector<P2> placed;
    for (size_t i = 0; i < spots.size() && int(placed.size()) < want; ++i)
    {
        const P2 c = spots[i];
        const std::string id = "shack/" + std::to_string(placed.size());
        const uint64_t key = lila::names::textKey(id);
        const double w = 380.0 + k.rnd(key, 1) * 280.0, d = 360.0 + k.rnd(key, 2) * 240.0;
        // Face the nearer lane.
        const P2 front = std::abs(c.x) < std::abs(c.y) ? P2(c.x < 0 ? 1.0 : -1.0, 0.0) : P2(0.0, c.y < 0 ? 1.0 : -1.0);
        const double heading = facing(front) + (k.rnd(key, 3) - 0.5) * 0.2;
        const Ring fp = orientedRect(c, w * 0.5, d * 0.5, heading);
        EnvelopeOut& e = k.envelope(id, fp, "ruralShack", "shack", "residential", 1, 230.0 + k.rnd(key, 4) * 50.0, "flat");
        e.front = front;
        e.entrances = {c + front * (d * 0.5)};
        k.ground(id, "terrainStampPad", {{"footprint", pts(orientedRect(c, w * 0.5 + 80.0, d * 0.5 + 80.0, heading))}, {"mode", "flatten"}, {"falloff", 200.0}, {"masks", {"cityFootprint"}}});
        if (k.rnd(key, 5) < 0.35)
            k.slot(id + "/tank", "tank.water", c, 0.0, e.height);
        if (k.rnd(key, 6) < 0.25)
            k.slot(id + "/washing", "clothesline", c + front * (d * 0.5 + 200.0), alongDir(P2(-front.y, front.x)));
        placed.push_back(c);
    }
    if (int(placed.size()) < want)
        k.L.notes.push_back("only " + std::to_string(placed.size()) + " of " + std::to_string(want) + " shacks fit: widen the radius");
    k.L.stats["shacks"] = int(placed.size());

    // Parked cars and mopeds along the lanes, one car a lowrider the side job takes.
    for (int i = 0; i < 6; ++i)
    {
        const double t = -R * 0.8 + i * (R * 0.32);
        const bool alongX = i % 2 == 0;
        const P2 at = alongX ? P2(t, kLane - 150.0) : P2(kLane - 150.0, t);
        k.slot("parked/" + std::to_string(i), i % 3 == 2 ? "vehicle.moped" : "vehicle.car", at, facing(alongX ? P2(1.0, 0.0) : P2(0.0, 1.0)));
    }
    SlotOut& low = k.slot("lowrider", "vehicle.car", {-kLane + 150.0, R * 0.3}, facing({0.0, 1.0}));
    low.data = {{"model", "lowrider"}};
    job(k, "lowrider", {-kLane - 150.0, R * 0.3}, "lowrider");
    for (size_t i = 0; i < placed.size() && i < 9; i += 3)
        loot(k, std::to_string(i / 3), placed[i], i == 0 ? "product" : "cash");
    guard(k, "crossing", {kLane, kLane}, {1.0, 1.0});
    guard(k, "laneE", {R * 0.6, 0.0}, {1.0, 0.0});
    guard(k, "laneW", {-R * 0.6, 0.0}, {-1.0, 0.0});
    guard(k, "laneN", {0.0, R * 0.6}, {0.0, 1.0});
}

void buildUnfinished(Kit& k)
{
    const Spec& s = k.spec;
    const double tw = s.f("towerWidth"), td = s.f("towerDepth");
    const int floors = s.i("towerFloors");
    const double W = tw + 5000.0, D = td + 6000.0, H = floors * 320.0;
    k.L.boundary = rect(-W * 0.5 - 300.0, -1500.0, W * 0.5 + 300.0, D + 300.0);
    fencedYard(k, W, D, "fence.hoarding", 1.0, "track");

    const double tz = 2600.0;
    const Ring tower = rect(-tw * 0.5, tz, tw * 0.5, tz + td);
    EnvelopeOut& e = k.envelope("shell", tower, "resortTower", "unfinishedHotel", "commercial", floors, H, "flat");
    e.front = {0.0, -1.0};
    e.entrances = {{0.0, tz}};
    e.name = s.name;
    e.extra = {{"unfinished", true}};
    const int bays = k.along("scaffold", "scaffold", {-tw * 0.5, tz - 120.0}, {tw * 0.5, tz - 120.0}, 300.0, 0.0, Level::Ground, H / 600.0);
    k.L.stats["scaffoldBays"] = bays;
    if (floors >= 6)
        k.slot("crane", "crane", {tw * 0.5 + 1500.0, tz + td * 0.5});

    k.slot("office", "cabin.site", {-W * 0.5 + 900.0, 900.0}, facing({1.0, 0.0}));
    for (int i = 0; i < 3; ++i)
        k.slot("rebar/" + std::to_string(i), "pile.rebar", {-W * 0.5 + 1200.0, 2400.0 + i * 300.0});
    for (int i = 0; i < 6; ++i)
        k.slot("blocks/" + std::to_string(i), "crate.pallet", {W * 0.5 - 1400.0 + (i % 3) * 170.0, 1400.0 + (i / 3) * 150.0});
    k.slot("mixer", "mixer.cement", {tw * 0.5 + 400.0, tz - 600.0});
    for (int i = 0; i < 2; ++i)
        k.slot("pickup/" + std::to_string(i), "vehicle.pickup", {1200.0 + i * 300.0, 900.0}, facing({0.0, -1.0}));
    if (s.b("generatorTrailer"))
    {
        const P2 at(-tw * 0.5 - 900.0, tz + 400.0);
        k.slot("generator", "generator.trailer", at, facing({0.0, -1.0}));
        job(k, "generator", at + P2(-300.0, 0.0), "generatorTrailer");
    }
    loot(k, "shell", {0.0, tz + td * 0.5}, "product");
    loot(k, "payroll", {-W * 0.5 + 900.0, 900.0}, "cash");
    guard(k, "gate", {500.0, 400.0}, {0.0, -1.0});
    guard(k, "shell", {-tw * 0.25, tz - 400.0}, {0.0, -1.0});
    guard(k, "back", {0.0, D - 500.0}, {0.0, 1.0});
}

void buildFishingCamp(Kit& k)
{
    const Spec& s = k.spec;
    const double L = s.f("campLength"), jetty = s.f("jettyLength");
    k.L.boundary = rect(-L * 0.5, -std::max(jetty, 1500.0) - 1000.0, L * 0.5, 7000.0);
    // The site frame: the shore along x at z = 0, the sea at -z, the beach and shacks inland.
    k.ground("beach", "terrainStampRegion",
             heightFromWater({{"footprint", pts(rect(-L * 0.5, 0.0, L * 0.5, 5500.0))}, {"mode", "flatten"}, {"falloff", 1500.0}, {"masks", {"beach"}}}, 120.0));
    {
        Part& b = k.part("beach", "yard", rect(-L * 0.5, 1000.0, L * 0.5, 5500.0), Part::Needs::Land);
        b.maxRelief = 200.0;
        b.groundOp = "beach";
        k.part("sea", "sea", circle({0.0, -std::max(jetty, 1500.0) - 500.0}, 600.0, 12), Part::Needs::Water);
    }
    k.ground("access", "terrainAddRoad",
             {{"kind", "track"}, {"layer", "Sites"}, {"surface", "dirt"}, {"path", json::array({pt({L * 0.3, 4800.0}), pt({L * 0.3, 16000.0})})}});

    const int nShacks = s.i("campShacks");
    std::vector<Ring> shacks;
    const double step = (L - 1200.0) / std::max(1, nShacks);
    for (int i = 0; i < nShacks; ++i)
    {
        const double x = -L * 0.5 + 600.0 + step * (i + 0.5);
        const double w = std::min(step - 200.0, 500.0 + k.rnd(uint64_t(i), 1) * 300.0);
        if (w < 250.0)
        {
            k.L.notes.push_back("only " + std::to_string(i) + " of " + std::to_string(nShacks) + " shacks fit: lengthen the beach");
            break;
        }
        const double z = 3300.0 + k.rnd(uint64_t(i), 2) * 500.0;
        const Ring fp = rect(x - w * 0.5, z, x + w * 0.5, z + 450.0);
        shed(k, "shack/" + std::to_string(i), fp, "ruralShack", "fishShack", "residential", 240.0, k.rnd(uint64_t(i), 3) < 0.5 ? "gable" : "flat", {0.0, -1.0});
        shacks.push_back(fp);
        if (i % 2 == 0)
            k.slot("nets/" + std::to_string(i), "net.rack", {x, z - 700.0});
    }
    for (int d = 0; d < 8; ++d)
        k.slot("fuel/" + std::to_string(d), "drum.fuel", {-L * 0.5 + 500.0 + (d % 4) * 80.0, 2600.0 + (d / 4) * 80.0});

    if (jetty > 0.0)
    {
        const int deck = k.along("jetty", "dock.pier", {0.0, -50.0}, {0.0, -jetty}, 500.0, 120.0, Level::Water);
        for (size_t n = k.L.slots.size() - size_t(deck); n < k.L.slots.size(); ++n)
            k.L.slots[n].scale.z = 200.0 / 300.0;
    }
    const int pangas = s.i("pangas");
    const int moored = jetty > 0.0 ? std::min(pangas / 2, std::max(0, int(jetty / 900.0)) * 2) : 0;
    for (int b = 0; b < pangas; ++b)
    {
        const std::string id = "panga/" + std::to_string(b);
        if (b < moored)
        {
            const double side = b % 2 ? -1.0 : 1.0;
            k.slot(id, "boat.panga", {side * 230.0, -500.0 - (b / 2) * 900.0}, kPi, 0.0, Level::Water);
        }
        else
        {
            const int beached = b - moored;
            k.slot(id, "boat.panga", {-L * 0.4 + beached * (L * 0.8 / std::max(1, pangas - moored)) + 300.0, 900.0}, facing({0.0, -1.0}) + 0.15);
        }
    }
    if (moored > 0)
        job(k, "panga", {460.0, -500.0}, "panga");
    else if (pangas > 0)
        job(k, "panga", {-L * 0.4 + 300.0, 1500.0}, "panga");
    if (s.b("reeferTruck"))
    {
        const P2 at(L * 0.3, 5600.0);
        k.slot("reefer", "truck.reefer", at, facing({0.0, -1.0}));
        job(k, "reefer", at + P2(300.0, 0.0), "shrimpReefer");
    }
    if (!shacks.empty())
        loot(k, "shack", centroidOf(shacks.front()), "cash");
    if (shacks.size() > 1)
        loot(k, "contraband", centroidOf(shacks.back()), "contraband");
    loot(k, "beach", {0.0, 1600.0}, "guns");
    guard(k, "jetty", {0.0, -100.0}, {0.0, -1.0});
    guard(k, "track", {L * 0.3 - 300.0, 4600.0}, {0.0, 1.0});
    guard(k, "beach", {-L * 0.25, 2000.0}, {0.0, -1.0});
}

void buildWarehouse(Kit& k)
{
    const Spec& s = k.spec;
    const double bl = s.f("bodegaLength"), bd = s.f("bodegaDepth");
    const double W = bl + 6000.0, D = bd + 8000.0;
    k.L.boundary = rect(-W * 0.5 - 300.0, -1500.0, W * 0.5 + 300.0, D + 300.0);
    fencedYard(k, W, D, "fence.chainlink", 1.0, "road");

    const double front = D - bd - 800.0;
    const Ring wh = rect(-bl * 0.5, front, bl * 0.5, D - 800.0);
    EnvelopeOut& e = k.envelope("warehouse", wh, "packingPlant", "warehouse", "industrial", 1, 800.0, "gable");
    e.front = {0.0, -1.0};
    e.name = s.name;
    e.extra = {{"derelict", true}};
    const int docks = s.i("loadingDocks");
    const double pitch = bl / docks;
    for (int d = 0; d < docks; ++d)
    {
        const P2 at(-bl * 0.5 + pitch * (d + 0.5), front);
        k.slot("dock/" + std::to_string(d), "door.dock", at, facing({0.0, -1.0}));
        e.entrances.push_back(at);
    }
    // Trailers backed onto the docks, fronts to the yard.
    const int trailers = std::min(s.i("bodegaTrailers"), docks);
    for (int t = 0; t < trailers; ++t)
        k.slot("trailer/" + std::to_string(t), "truck.trailer", {-bl * 0.5 + pitch * (t + 0.5), front - 700.0}, facing({0.0, -1.0}));
    if (trailers < s.i("bodegaTrailers"))
        k.L.notes.push_back("trailers capped at the dock count");
    for (int i = 0; i < 10; ++i)
        k.slot("pallet/" + std::to_string(i), "crate.pallet", {W * 0.5 - 1500.0 + (i % 5) * 160.0, 1800.0 + (i / 5) * 150.0});
    cornerMasts(k, W, D);
    if (s.b("freightRig"))
    {
        const P2 at(-W * 0.5 + 1500.0, 2200.0);
        k.slot("rig/tractor", "truck.tractor", at, facing({0.0, -1.0}));
        k.slot("rig/trailer", "truck.trailer", at + P2(0.0, 900.0), facing({0.0, -1.0})).data = {{"loaded", true}};
        job(k, "rig", at + P2(300.0, 0.0), "freightTrailer");
    }
    loot(k, "office", {bl * 0.4, front + 400.0}, "cash");
    loot(k, "electronics", {0.0, front + bd * 0.5}, "electronics");
    loot(k, "contraband", {-bl * 0.3, front + bd * 0.6}, "cigarettes");
    guard(k, "gate", {500.0, 400.0}, {0.0, -1.0});
    guard(k, "docks", {0.0, front - 1500.0}, {0.0, -1.0});
    guard(k, "inside", {-bl * 0.2, front + bd * 0.4}, {0.0, -1.0});
    guard(k, "back", {W * 0.5 - 600.0, D - 400.0}, {1.0, 0.0});
}

void buildRanch(Kit& k)
{
    const Spec& s = k.spec;
    const double W = s.f("ranchWidth"), D = s.f("ranchDepth");
    const bool runway = s.b("ranchRunway");
    const double Lr = s.f("runwayLength"), Wr = 1500.0;
    const double rz = D + 4000.0 + Wr * 0.5;
    k.L.boundary = rect(-std::max(W * 0.5, runway ? Lr * 0.5 : 0.0) - 1000.0, -1500.0, std::max(W * 0.5, runway ? Lr * 0.5 : 0.0) + 1000.0,
                        runway ? rz + Wr * 0.5 + 3000.0 : D + 300.0);
    fencedYard(k, W, D, "fence.wood", 1.0, "track");

    const Ring house = rect(-1600.0, D * 0.45, 1600.0, D * 0.45 + 1800.0);
    EnvelopeOut& h = shed(k, "house", house, "adobe", "ranchHouse", "residential", 330.0, "gable", {0.0, -1.0});
    h.name = s.name;
    const Ring barn = rect(W * 0.5 - 3400.0, D - 2800.0, W * 0.5 - 800.0, D - 800.0);
    shed(k, "barn", barn, "warehouse", "barn", "farm", 600.0, "gable", {-1.0, 0.0});
    k.alongRing("corral", "fence.wood", rect(-W * 0.5 + 800.0, D - 3200.0, -W * 0.5 + 3600.0, D - 800.0), true, 300.0, {{200.0, 600.0}});
    k.slot("waterTank", "tank.water", {-W * 0.5 + 1200.0, D * 0.3}).scale = {3.0, 3.0, 3.0};
    for (int i = 0; i < 3; ++i)
        k.slot("suv/" + std::to_string(i), "vehicle.suv", {-1200.0 + i * 600.0, D * 0.45 - 900.0}, facing({0.0, -1.0}));

    if (runway)
    {
        const double ex = Lr * 0.5;
        k.ground("strip", "terrainAddAirstrip", {{"a", pt({-ex, rz})}, {"b", pt({ex, rz})}, {"aircraft", s.s("aircraft")}, {"width", Wr + 1200.0}, {"pad", true}});
        k.ground("runway", "terrainAddRoad",
                 {{"kind", "runway"}, {"profile", "runway"}, {"layer", "Sites"}, {"path", json::array({pt({-ex, rz}), pt({ex, rz})})}, {"smooth", false},
                  {"lanes", 1}, {"laneWidth", Wr}, {"shoulder", 0.0}, {"verge", 200.0}, {"kerbHeight", 0.0}, {"surface", "dirt"}});
        k.ground("taxi", "terrainAddRoad", {{"kind", "track"}, {"layer", "Sites"}, {"surface", "dirt"}, {"path", json::array({pt({0.0, D}), pt({0.0, rz - Wr * 0.5})})}});
        Part& r = k.part("runway", "runway", rect(-ex, rz - Wr * 0.5, ex, rz + Wr * 0.5), Part::Needs::Land);
        r.maxRelief = 400.0;
        r.groundOp = "strip";
        k.slot("windsock", "windsock", {0.0, rz + Wr * 0.5 + 1200.0});
        const P2 plane(800.0, rz + Wr * 0.5 + 1200.0);
        k.slot("plane", "aircraft.light", plane, facing({0.0, -1.0}));
        for (int d = 0; d < 8; ++d)
            k.slot("fuel/" + std::to_string(d), "drum.fuel", {-1200.0 + (d % 4) * 80.0, rz + Wr * 0.5 + 900.0 + (d / 4) * 80.0});
        job(k, "plane", plane + P2(700.0, 0.0), "plane");
        guard(k, "runway", {-600.0, rz + Wr * 0.5 + 700.0}, {0.0, -1.0});
    }
    loot(k, "house", centroidOf(house), "cash");
    loot(k, "safe", centroidOf(house) + P2(800.0, 400.0), "product");
    loot(k, "barn", centroidOf(barn), "guns");
    guard(k, "gate", {500.0, 400.0}, {0.0, -1.0});
    guard(k, "porch", {0.0, D * 0.45 - 300.0}, {0.0, -1.0});
    guard(k, "barn", {barn[0].x - 300.0, D - 1800.0}, {-1.0, 0.0});
    guard(k, "yard", {-W * 0.3, D * 0.2}, {-1.0, 0.0});
}

void buildArena(Kit& k)
{
    const Spec& s = k.spec;
    const double R = s.f("arenaRadius");
    const int trucks = s.i("parking");
    const double lotDepth = 600.0 + std::ceil(trucks / 12.0) * 1100.0;
    const double W = std::max(2.0 * R + 4000.0, 12.0 * 280.0 + 1200.0);
    const double az = lotDepth + 800.0 + R;
    const double D = az + R + 2500.0;
    k.L.boundary = rect(-W * 0.5, -1500.0, W * 0.5, D);
    k.ground("pad", "terrainStampPad", {{"footprint", pts(rect(-W * 0.5, 0.0, W * 0.5, D - 200.0))}, {"mode", "flatten"}, {"falloff", 1200.0}, {"masks", {"cityFootprint"}}});
    {
        Part& p = k.part("grounds", "yard", rect(-W * 0.5 + 200.0, 0.0, W * 0.5 - 200.0, D - 400.0), Part::Needs::Land);
        p.maxRelief = 250.0;
        p.groundOp = "pad";
    }
    k.ground("lot", "terrainStampRegion",
             {{"footprint", pts(rect(-W * 0.5 + 300.0, 200.0, W * 0.5 - 300.0, lotDepth))}, {"mode", "blend"}, {"strength", 0.0}, {"falloff", 0.0}, {"masks", {"paved"}}});
    k.ground("access", "terrainAddRoad", {{"kind", "track"}, {"layer", "Sites"}, {"surface", "dirt"}, {"path", json::array({pt({0.0, 300.0}), pt({0.0, -8000.0})})}});

    EnvelopeOut& arena = k.envelope("arena", circle({0.0, az}, R, 16), "civic", "palenque", "commercial", 1, 650.0, "dome");
    arena.front = {0.0, -1.0};
    arena.entrances = {{0.0, az - R}};
    arena.name = s.name;
    k.slot("pit", "ring.pit", {0.0, az});

    const int parked = parkRows(k, "truck", -W * 0.5 + 600.0, W * 0.5 - 600.0, 400.0, lotDepth, trucks, {},
                                [](int i) { return std::string(i % 4 == 3 ? "vehicle.suv" : "vehicle.pickup"); });
    k.L.stats["trucks"] = parked;
    SlotOut& boss = k.slot("bossTruck", "vehicle.pickup", {R + 900.0, az - R * 0.5}, facing({0.0, -1.0}));
    boss.data = {{"model", "narcoTruck"}};
    job(k, "bossTruck", {R + 1300.0, az - R * 0.5}, "bossTruck");
    for (int i = 0; i < 4; ++i)
        k.slot("stall/" + std::to_string(i), "stall.food", {-R - 1200.0, az - R * 0.6 + i * 450.0}, facing({1.0, 0.0}));
    for (int i = 0; i < 6; ++i)
        k.slot("cages/" + std::to_string(i), "cage.rooster", {-R * 0.6 + i * (R * 0.24), az + R + 700.0}, facing({0.0, -1.0}));
    cornerMasts(k, W, D);
    loot(k, "betting", {0.0, az + R * 0.5}, "cash");
    loot(k, "office", {R * 0.5, az + R * 0.3}, "jewellery");
    guard(k, "door", {300.0, az - R - 300.0}, {0.0, -1.0});
    guard(k, "lot", {-W * 0.3, lotDepth * 0.5}, {0.0, -1.0});
    guard(k, "boss", {R + 600.0, az - R * 0.5 + 400.0}, {1.0, 0.0});
    guard(k, "back", {0.0, az + R + 1300.0}, {0.0, 1.0});
}

void buildImpound(Kit& k)
{
    const Spec& s = k.spec;
    const double W = s.f("impoundWidth"), D = s.f("impoundDepth");
    k.L.boundary = rect(-W * 0.5 - 300.0, -1500.0, W * 0.5 + 300.0, D + 300.0);
    fencedYard(k, W, D, "fence.chainlink", 1.2, "road");
    k.slot("barrier", "gate.boom", {0.0, -300.0});

    std::vector<Ring> taken;
    const Ring booth = rect(500.0, -900.0, 900.0, -400.0);
    shed(k, "booth", booth, "civic", "guardhouse", "civic", 260.0, "flat", {-1.0, 0.0});
    const Ring office = rect(W * 0.5 - 2600.0, 400.0, W * 0.5 - 400.0, 1600.0);
    shed(k, "office", office, "civic", "impoundOffice", "civic", 320.0, "flat", {-1.0, 0.0}).name = s.name;
    taken.push_back(rect(office[0].x - 300.0, 0.0, W * 0.5, office[2].y + 300.0));
    taken.push_back(rect(-2400.0, 0.0, 700.0, 1900.0));

    const int cars = parkRows(k, "seized", -W * 0.5 + 400.0, W * 0.5 - 400.0, 2000.0, D - 300.0, s.i("impounded"), taken,
                              [](int i) { return std::string(i % 7 == 3 ? "vehicle.suv" : i % 5 == 1 ? "vehicle.pickup" : "vehicle.car"); });
    if (cars < s.i("impounded"))
        k.L.notes.push_back("only " + std::to_string(cars) + " of " + std::to_string(s.i("impounded")) + " vehicles fit: enlarge the lot");
    k.L.stats["impounded"] = cars;
    // The best of the seized cars, the one the side job drives out.
    for (size_t i = 0; i < k.L.slots.size(); ++i)
        if (k.L.slots[i].path.starts_with("seized/") && k.L.slots[i].type == "vehicle.car")
        {
            k.L.slots[i].data = {{"model", "seizedLuxury"}};
            const P2 at = k.L.slots[i].at;
            job(k, "seizedCar", at + P2(200.0, 0.0), "seizedCar");
            break;
        }
    k.slot("towTruck", "truck.tow", {-1500.0, 900.0}, facing({0.0, -1.0}));
    cornerMasts(k, W, D);
    loot(k, "evidence", centroidOf(office), "guns");
    loot(k, "cash", centroidOf(office) + P2(500.0, 200.0), "cash");
    guard(k, "booth", {700.0, -1100.0}, {0.0, -1.0});
    guard(k, "gate", {-500.0, 300.0}, {0.0, -1.0});
    guard(k, "office", {office[0].x - 300.0, 1000.0}, {-1.0, 0.0});
    guard(k, "rows", {0.0, D * 0.6}, {1.0, 0.0});
}
} // namespace openworld::sites
