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
