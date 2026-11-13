// Masonry fit: intact walls stand on their beds, floors fit inside their walls, rubble spreads instead of stacking,
// loose stones keep clear of their neighbours, and masonryCheck says where problems are.

#include "TestUtil.h"
#include "Masonry.h"
#include "MasonryBuilding.h"
#include "MasonryRuin.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <tuple>

using namespace lila;
using namespace masonry;
using namespace masonrytest;
using json = nlohmann::json;

namespace
{
const json kStone = {{"length", 60}, {"height", 30}, {"depth", 35}, {"joint", 1.5}};

json naveWall(bool buttresses)
{
    json w = {{"name", "nave"},
              {"points", pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}})},
              {"closed", true},
              {"height", 900},
              {"thickness", 90},
              {"cornice", 1},
              {"openings",
               {{{"name", "east_window"}, {"at", 2000}, {"width", 120}, {"height", 300}, {"sill", 350}, {"arch", "pointed"}},
                {{"name", "south_window"}, {"at", 1000}, {"width", 100}, {"height", 280}, {"sill", 350}, {"arch", "pointed"}}}}};
    if (buttresses)
        w["buttresses"] = {{{"at", 400}}, {{"at", 800}}, {{"at", 1200}}};
    return w;
}

json towerWall()
{
    return {{"name", "tower"},
            {"points", pts({{-640, 0, 150}, {-140, 0, 150}, {-140, 0, 650}, {-640, 0, 650}})},
            {"closed", true},
            {"height", 1800},
            {"thickness", 100},
            {"bands", {900}},
            {"cornice", 2},
            {"openings",
             {{{"name", "west_door"}, {"at", 1750}, {"width", 160}, {"height", 320}, {"arch", "round"}},
              {{"name", "belfry"}, {"at", 250}, {"width", 90}, {"height", 200}, {"sill", 1450}, {"arch", "round"}}}}};
}

Built building(const json& spec, const RuinParams& r = {}, RuinStats* st = nullptr)
{
    json s = spec;
    s["stone"] = kStone;
    s["bond"] = "ashlar";
    return realizePlan(planBuilding(s, BuildOptions{}), r, AgeParams{}, false, st);
}

int floating(const Built& b) { return countFloating(b.stones, std::max(0.1, b.pitch * 0.15)); }

std::vector<int> standingOf(const Built& b)
{
    std::vector<int> out;
    for (size_t k = 0; k < b.stones.size(); ++k)
        if (!isRubble(b.stones[k]))
            out.push_back(int(k));
    return out;
}

size_t standingOverlaps(const Built& b) { return overlappingPairs(b, standingOf(b), 0.1).size(); }
} // namespace

TEST_CASE("masonry fit: intact walls stand on their beds, over arch crowns and short rough stones too")
{
    SUBCASE("dry stone, straight on flat ground")
    {
        BuildOptions o;
        o.wall.bond = Bond::DryStone;
        o.wall.height = 120;
        o.wall.thickness = 60;
        o.wall.stone = {40, 20, 30, 1.0};
        const Footprint f = line({{0, 0, 0}, {800, 0, 0}});
        const Built b = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), RuinParams{}, AgeParams{}, false);
        CHECK(floating(b) == 0);
        CHECK(countOverlaps(b.stones, 0.1) == 0);
    }
    SUBCASE("a tower with a band, a cornice and a belfry")
    {
        const Built b = building({{"walls", {towerWall()}}});
        CHECK(floating(b) == 0);
        CHECK(standingOverlaps(b) == 0);
    }
    SUBCASE("a nave without buttresses, windows under pointed arches")
    {
        const Built b = building({{"walls", {naveWall(false)}}});
        CHECK(floating(b) == 0);
        CHECK(standingOverlaps(b) == 0);
    }
    SUBCASE("a floor alone on the ground")
    {
        const json floor = {{"name", "paving"}, {"points", pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}})}, {"y", 0}, {"thickness", 10}, {"surface", "flags"}};
        const Built b = building({{"floors", {floor}}});
        CHECK(floating(b) == 0);
    }
}

TEST_CASE("masonry fit: floors fit inside their walls, whole and broken")
{
    const json nave = naveWall(true);
    const json follow = pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}});
    const json inside = pts({{45, 0, 45}, {1555, 0, 45}, {1555, 0, 755}, {45, 0, 755}});
    for (const auto& [tag, outline, y] : {std::tuple{"outline on the walls", follow, 0.0}, std::tuple{"outline on the inside face", inside, 0.0},
                                          std::tuple{"raised floor", follow, 300.0}})
    {
        CAPTURE(tag);
        const json spec = {{"walls", {nave}}, {"floors", {{{"name", "paving"}, {"points", outline}, {"y", y}, {"thickness", y > 0.0 ? 30 : 10}, {"surface", "flags"}}}}};
        const Built whole = building(spec);
        CHECK(standingOverlaps(whole) == 0);
        CHECK(floating(whole) == 0);
        int slabs = 0, paving = 0;
        for (const Stone& s : whole.stones)
        {
            slabs += s.role == Role::Slab ? 1 : 0;
            paving += s.role == Role::Paving ? 1 : 0;
            // Cut back to the walls' inside faces, 45 cm in from the walls' centre line.
            if (s.role == Role::Slab)
                CHECK(s.size.x < 1600.0 - 89.0);
        }
        CHECK(slabs == 1);
        CHECK(paving > 100);

        RuinParams r;
        r.left = 0.5;
        RuinStats st;
        const Built ruined = building(spec, r, &st);
        CHECK(st.slabPieces > 4);
        CHECK(standingOverlaps(ruined) == 0);
    }
}

TEST_CASE("masonry fit: rubble inside a closed tower spreads over its floor instead of stacking")
{
    BuildOptions o;
    o.wall.bond = Bond::Ashlar;
    o.wall.height = 1200;
    o.wall.thickness = 90;
    o.wall.stone = {50, 28, 35, 1.5};
    o.wall.batter = 1;
    o.wall.cornice = 2;
    Footprint f;
    f.closed = true;
    for (int i = 0; i < 16; ++i)
        f.points.push_back({300 * std::cos(i / 16.0 * 2.0 * kPi), 0, 300 * std::sin(i / 16.0 * 2.0 * kPi)});
    Opening door;
    door.at = 150;
    door.width = 120;
    door.height = 240;
    o.wall.openings = {door};
    RuinParams r;
    r.left = 0.5;
    r.collapse = 90;
    r.gaps = 1;
    RuinStats st;
    const Built b = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), r, AgeParams{}, false, &st);
    CHECK(st.rubble > 500);
    double highest = 0.0;
    CHECK(countStacked(b.stones, 0.5 * 1200.0, nullptr, &highest) == 0);
    CHECK(highest < 0.4 * 1200.0);
    int inside = 0;
    for (const Stone& s : b.stones)
        inside += s.role == Role::Rubble && std::hypot(s.center.x, s.center.z) < 240.0 ? 1 : 0;
    CHECK(inside > 100);
    std::vector<Stone> rubble;
    for (const Stone& s : b.stones)
        if (isRubble(s))
            rubble.push_back(s);
    CHECK(countOverlaps(rubble, 1.0) < rubble.size() / 50);
}

TEST_CASE("masonry fit: loose stones of a dry stone wall over a hill keep clear of their neighbours")
{
    BuildOptions o;
    o.wall.bond = Bond::DryStone;
    o.wall.height = 120;
    o.wall.thickness = 60;
    o.wall.stone = {40, 20, 30, 1.0};
    Footprint f;
    const glm::dvec3 corners[] = {{-2500, 0, 4000}, {0, 0, 3500}, {2500, 0, 4200}};
    for (int seg = 0; seg < 2; ++seg)
        for (int i = 0; i < 64; ++i)
        {
            glm::dvec3 p = glm::mix(corners[seg], corners[seg + 1], i / 64.0);
            p.y = 200.0 * std::sin(p.x / 900.0) + 120.0 * std::cos(p.z / 400.0);
            f.points.push_back(p);
        }
    const Built whole = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), RuinParams{}, AgeParams{}, false);
    REQUIRE(standingOverlaps(whole) == 0);
    for (double loose : {0.4, 1.0})
    {
        CAPTURE(loose);
        RuinParams r;
        r.left = 0.75;
        r.loose = loose;
        RuinStats st;
        const Built b = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), r, AgeParams{}, false, &st);
        CHECK(st.loose > 20);
        CHECK(standingOverlaps(b) == 0);
    }
}

TEST_CASE("masonry fit commands: a wall's region name, bounds, and where masonryCheck finds interpenetration")
{
    loadMasonry();
    run("newScene");
    const json w = run("masonryWall", {{"points", pts({{0, 0, 0}, {400, 0, 0}})}, {"region", "garden_wall"}, {"height", 100}, {"name", "Garden"}});
    CHECK(w["regions"].contains("garden_wall"));
    CHECK(!w["regions"].contains("wall_0"));
    REQUIRE(w["bounds"].is_object());
    CHECK(w["bounds"]["min"][0].get<double>() < 1.0);
    CHECK(w["bounds"]["max"][0].get<double>() > 399.0);
    CHECK(w["bounds"]["max"][1].get<double>() > 90.0);

    // Two walls crossing: their stones interpenetrate where they meet, and the check names both.
    const json spec = {{"walls",
                        {{{"name", "east"}, {"points", pts({{0, 0, 0}, {400, 0, 0}})}, {"height", 100}, {"thickness", 40}},
                         {{"name", "north"}, {"points", pts({{200, 0, -200}, {200, 0, 200}})}, {"height", 100}, {"thickness", 40}}}}};
    const json b = run("masonryBuilding", {{"spec", spec}, {"name", "Crossing"}});
    CHECK(b["bounds"].is_object());
    const json c = run("masonryCheck", {{"args", {b["mesh"]}}});
    CHECK(!c["ok"].get<bool>());
    REQUIRE(c["overlaps"].get<int64_t>() > 0);
    CHECK(c["overlapRegions"].contains("east + north"));
    CHECK(c["overlapPairs"].size() > 0);
    CHECK(c["issues"].dump().find("east + north") != std::string::npos);
    CHECK(c["stacked"].get<int64_t>() == 0);
}
