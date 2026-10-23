#include <doctest/doctest.h>

#include "../../extensions/Puppeteer/anim/KeyMath.h"
#include "../../extensions/Puppeteer/anim/PanelModels.h"

#include <cmath>

using namespace puppeteer::anim;
using Approx = doctest::Approx;
using json = nlohmann::json;

TEST_CASE("stacker: editing a gap retimes everything after it")
{
    const std::vector<double> keys = {0, 4, 10, 16};
    const TimeWarp w = retimeGap(keys, 1, 8);
    CHECK(w.apply(keys) == std::vector<double>{0, 4, 12, 18});
    // A key between the two ends of the edited gap stretches with it.
    CHECK(w.map(7) == Approx(8));
    CHECK(w.map(-1) == Approx(-1));

    CHECK(retimeGap(keys, 0, 1).apply(keys) == std::vector<double>{0, 1, 7, 13});
    CHECK_THROWS_AS(retimeGap(keys, 3, 4), std::invalid_argument);
    CHECK_THROWS_AS(retimeGap(keys, 1, 0), std::invalid_argument);
    CHECK(gapAfter(keys, 2) == 6);
    CHECK(gapAfter(keys, 3) == 0);
    CHECK(totalTime(keys) == 16);
}

TEST_CASE("stacker: deleting a key takes its timing with it")
{
    std::vector<double> keys = {0, 4, 10, 16};
    TimeWarp w = deleteWithTiming(keys, 1);
    keys.erase(keys.begin() + 1);
    CHECK(w.apply(keys) == std::vector<double>{0, 4, 10});

    // The last key has no gap to give back.
    std::vector<double> last = {0, 4, 10};
    CHECK(deleteWithTiming(last, 2).knots.empty());
}

TEST_CASE("stacker: a new key after a key opens its gap and uses the last gap")
{
    const std::vector<double> keys = {0, 4, 10};
    CHECK(lastGap(keys, 1, 99) == 4);
    CHECK(lastGap(keys, 0, 99) == 4);
    CHECK(lastGap({5}, 0, 99) == 99);
    CHECK(openAfter(keys, 1, 4).apply(keys) == std::vector<double>{0, 4, 14});
    CHECK(openAfter(keys, 2, 4).apply(keys) == keys);
}

TEST_CASE("stacker: quick select halves skip the loop key")
{
    CHECK(quickSelect(9, true, QuickSelect::All) == std::vector<size_t>{0, 1, 2, 3, 4, 5, 6, 7});
    CHECK(quickSelect(9, true, QuickSelect::FirstHalf) == std::vector<size_t>{0, 1, 2, 3});
    CHECK(quickSelect(9, true, QuickSelect::SecondHalf) == std::vector<size_t>{4, 5, 6, 7});
    CHECK(quickSelect(5, false, QuickSelect::FirstHalf) == std::vector<size_t>{0, 1});
    CHECK(quickSelect(5, false, QuickSelect::SecondHalf) == std::vector<size_t>{2, 3, 4});
}

TEST_CASE("stacker: rows number the keys, flag the loop key and carry marks")
{
    Marks m;
    m.set(4, Mark::Breakdown);
    m.set(10, Mark::Extreme);
    const std::vector<Row> r = rows({0, 4, 10, 16}, 16, m);
    REQUIRE(r.size() == 4);
    CHECK(r[1].mark == Mark::Breakdown);
    CHECK(r[2].mark == Mark::Extreme);
    CHECK(r[2].gap == 6);
    CHECK(!r[2].loop);
    CHECK(r[3].loop);
    CHECK(r[3].gap == 0);
    CHECK(!rows({0, 4}, std::nan(""), m)[1].loop);

    // Marks follow a retime and go with a deleted key.
    m.warp(retimeGap({0, 4, 10, 16}, 1, 8));
    CHECK(m.get(12) == Mark::Extreme);
    CHECK(m.get(4) == Mark::Breakdown);
    m.erase(4);
    CHECK(m.get(4) == Mark::None);
    CHECK_THROWS_AS(markFromName("loud"), std::invalid_argument);
}

TEST_CASE("cycle step: the first half's keys go half a cycle later")
{
    const StepPlan p = cycleStepPlan({0, 4, 8, 16, 20, 32}, 0, 32);
    REQUIRE(p.copies.size() == 3);
    CHECK(p.copies[0].from == 0);
    CHECK(p.copies[0].to == 16);
    CHECK(p.copies[2].from == 8);
    CHECK(p.copies[2].to == 24);
    // Keys already in the second half are replaced; the loop key stays.
    CHECK(p.replaced == std::vector<double>{16, 20});
    CHECK_THROWS_AS(cycleStepPlan({0}, 0, 0), std::invalid_argument);
}

TEST_CASE("cycle side: each key takes the key half the cycle away, and an odd count is refused")
{
    CHECK(cycleSideMap(4) == std::vector<size_t>{2, 3, 0, 1});
    CHECK(cycleSideMap(8)[7] == 3);
    // Applied twice it comes back: the mirror of the mirror is the original timing.
    const std::vector<size_t> m = cycleSideMap(6);
    for (size_t k = 0; k < 6; ++k)
        CHECK(m[m[k]] == k);
    CHECK_THROWS_WITH_AS(cycleSideMap(5), doctest::Contains("even number"), std::invalid_argument);
    CHECK_THROWS_AS(cycleSideMap(0), std::invalid_argument);
}

TEST_CASE("anim bank: duplicate names, folders and the take tree")
{
    CHECK(numberedName("Walk", {"Walk"}) == "Walk_01");
    CHECK(numberedName("Walk_01", {"Walk", "Walk_01"}) == "Walk_02");
    CHECK(numberedName("Walk", {"Walk", "Walk_01", "Walk_02"}) == "Walk_03");
    CHECK(numberedName("Run_2b", {}) == "Run_2b_01");
    CHECK(normalFolder(" locomotion//walks/ ") == "locomotion/walks");
    CHECK(normalFolder("\\a\\b") == "a/b");
    CHECK(folderChain("a/b/c") == std::vector<std::string>{"a", "a/b", "a/b/c"});
    CHECK(folderChain("").empty());

    const json takes = json::array({
        {{"name", "Idle"}, {"root", "root"}, {"folder", ""}, {"active", true}},
        {{"name", "Walk"}, {"root", "root"}, {"folder", "loco/walks"}},
        {{"name", "Run"}, {"root", "root"}, {"folder", "loco"}},
        {{"name", "Jog"}, {"root", "root"}, {"folder", "loco/walks"}},
        {{"name", "Other"}, {"root", "other_root"}},
        {{"name", "Scene"}, {"root", ""}, {"folder", "a"}},
    });
    const Folder all = bankTree(takes);
    CHECK(all.takes.size() == 2);
    REQUIRE(all.folders.size() == 2);
    CHECK(all.folders[0].name == "a");
    CHECK(all.folders[1].name == "loco");
    CHECK(all.folders[1].takes.front().name == "Run");
    REQUIRE(all.folders[1].folders.size() == 1);
    CHECK(all.folders[1].folders[0].path == "loco/walks");
    CHECK(all.folders[1].folders[0].takes.size() == 2);
    CHECK(all.takes.front().active);

    // Filtered to a character: its takes and the whole-scene ones.
    const Folder one = bankTree(takes, "root");
    CHECK(one.takes.size() == 1);
    CHECK(one.folders.size() == 2);
}

TEST_CASE("mixer: the top layer first, the base animation last")
{
    const json layers = json::array({
        {{"name", "BaseAnimation"}, {"base", true}, {"active", false}},
        {{"name", "layer1"}, {"weight", 0.5}, {"mute", true}, {"members", {"a.translate", "a.rotate"}}},
        {{"name", "layer2"}, {"mode", "override"}, {"active", true}, {"lock", true}},
    });
    const std::vector<LayerRow> r = mixerRows(layers);
    REQUIRE(r.size() == 3);
    CHECK(r[0].name == "layer2");
    CHECK(r[0].active);
    CHECK(r[0].lock);
    CHECK(r[0].mode == "override");
    CHECK(r[1].weight == 0.5);
    CHECK(r[1].mute);
    CHECK(r[1].members == 2);
    CHECK(r[2].base);
}
