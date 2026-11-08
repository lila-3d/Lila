// Settlement streetscape: street and district names.

#include "TestUtil.h"
#include "SettlementCore.h"

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Options styled(const char* name, bool blocks = true)
{
    Options o;
    REQUIRE(builtinProfile(name, &o.profile));
    o.blocks = blocks;
    return o;
}

std::map<uint64_t, std::string> namesById(const Layout& l)
{
    std::map<uint64_t, std::string> out;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
            out[l.graph.edges[e].street] = streetName(l, l.graph.edges[e].street);
    return out;
}
} // namespace

TEST_CASE("settlement streetscape: street and district names are stable by id, overridable, and follow the culture")
{
    // A grid, whose streets keep their ids through unrelated edits, named in Old English.
    Site site{.size = 120000.0};
    site.culture = "oldEnglish";
    const Options o = styled("usTown", false);
    const Layout a = build(site, Ground::plane(), o);
    CHECK(a.culture == "oldEnglish");
    const std::map<uint64_t, std::string> before = namesById(a);
    REQUIRE(before.size() > 10);
    std::set<std::string> distinct;
    for (const auto& [id, name] : before)
    {
        CHECK_FALSE(name.empty());
        distinct.insert(name);
    }
    CHECK(distinct.size() > 5);
    REQUIRE(!a.districts.empty());
    for (const District& d : a.districts)
    {
        CHECK_FALSE(d.name.empty());
        CHECK(d.area > 0.0);
    }
    CHECK(namesById(build(site, Ground::plane(), o)) == before); // every run

    // Another street's profile changed and one street renamed: every other street keeps its name.
    auto it = before.begin();
    const uint64_t changed = it->first, renamed = std::next(it, 3)->first;
    Site edited = site;
    edited.overrides.push_back({.street = changed, .profile = "street"});
    edited.overrides.push_back({.street = renamed, .name = "Pilgrims Way"});
    const Layout b = build(edited, Ground::plane(), o);
    const std::map<uint64_t, std::string> after = namesById(b);
    CHECK(streetName(b, renamed) == "Pilgrims Way");
    int kept = 0, compared = 0;
    for (const auto& [id, name] : before)
    {
        if (!after.contains(id) || name == before.at(renamed))
            continue;
        ++compared;
        kept += after.at(id) == name;
    }
    CHECK(compared > 10);
    CHECK(kept == compared);
    for (size_t k = 0; k < a.districts.size() && k < b.districts.size(); ++k)
    {
        CHECK(b.districts[k].id == a.districts[k].id);
        CHECK(b.districts[k].name == a.districts[k].name);
    }

    // An exclusion in one corner leaves almost every name where it was.
    Site added = site;
    added.exclusions.push_back({1, "", "", {{40000, 40000}, {58000, 40000}, {58000, 58000}, {40000, 58000}}, 0.0});
    const std::map<uint64_t, std::string> withFar = namesById(build(added, Ground::plane(), o));
    int same = 0, shared = 0;
    for (const auto& [id, name] : before)
        if (withFar.contains(id))
        {
            ++shared;
            same += withFar.at(id) == name;
        }
    REQUIRE(shared > 10);
    CHECK(double(same) / shared > 0.9);

    // Cultures: French words first, numbered US grid streets, sector codes; an unknown culture is an error.
    Site french = site;
    french.culture = "french";
    bool frenchWord = true;
    for (const auto& [id, name] : namesById(build(french, Ground::plane(), o)))
        frenchWord &= name.starts_with("Rue") || name.starts_with("Avenue") || name.starts_with("Boulevard") || name.starts_with("Ruelle") ||
                      name.starts_with("Passage") || name.starts_with("Impasse") || name.starts_with("All") || name.starts_with("Sentier") ||
                      name.starts_with("Escalier");
    CHECK(frenchWord);
    const Layout grid = build(Site{.size = 120000.0}, Ground::plane(), o);
    CHECK(grid.culture == "usGrid");
    int numbered = 0;
    for (const auto& [id, name] : namesById(grid))
        numbered += name.find("1st") != std::string::npos || name.find("2nd") != std::string::npos || name.find("3rd") != std::string::npos ||
                    name.find("th Street") != std::string::npos || name.find("th Avenue") != std::string::npos;
    CHECK(numbered > 5);
    Site sector = site;
    sector.culture = "sector";
    for (const auto& [id, name] : namesById(build(sector, Ground::plane(), o)))
        CHECK((name.starts_with("Trunk") || name.starts_with("Corridor") || name.starts_with("Spur") || name.starts_with("Duct") ||
               name.starts_with("Walkway")));
    Site bad = site;
    bad.culture = "klingon";
    CHECK_THROWS(build(bad, Ground::plane(), o));
    CHECK(toJson(a)["streets"][0].contains("name"));
    CHECK(toJson(a)["districts"].size() == a.districts.size());
}

TEST_CASE("settlement streetscape: commands name streets and districts, and undo")
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "usTown"}, {"size", 80000.0}, {"blocks", false}});
    const std::string mesh = made["mesh"].get<std::string>();

    const json names = run("settlementNames", {{"args", {mesh}}});
    CHECK(names["culture"] == "usGrid");
    REQUIRE(names["streets"].size() > 4);
    const json french = run("settlementNames", {{"args", {mesh}}, {"culture", "french"}});
    CHECK(french["culture"] == "french");
    const uint64_t id = french["streets"][0]["id"].get<uint64_t>();
    run("settlementEditStreet", {{"args", {mesh}}, {"id", id}, {"name", "Rue Neuve"}});
    auto nameOf = [&](uint64_t street) {
        for (const json& s : run("settlementStreets", {{"args", {mesh}}}))
            if (s["id"].get<uint64_t>() == street)
                return s["name"].get<std::string>();
        return std::string();
    };
    CHECK(nameOf(id) == "Rue Neuve");
    run("undo");
    CHECK(nameOf(id) == french["streets"][0]["name"].get<std::string>());
    run("undo");
    CHECK(run("settlementNames", {{"args", {mesh}}})["culture"] == "usGrid");
    const uint64_t district = names["districts"][0]["id"].get<uint64_t>();
    const json renamed = run("settlementNames", {{"args", {mesh}}, {"district", district}, {"name", "Old Town"}});
    bool found = false;
    for (const json& d : renamed["districts"])
        found |= d["id"].get<uint64_t>() == district && d["name"] == "Old Town";
    CHECK(found);
    CHECK_THROWS(run("settlementNames", {{"args", {mesh}}, {"culture", "klingon"}}));

    std::random_device rd;
    const auto path = std::filesystem::temp_directory_path() / ("lila_settlement_names_" + std::to_string(rd()) + ".json");
    run("settlementExport", {{"args", {mesh}}, {"path", path.string()}});
    std::ifstream in(path);
    const json exported = json::parse(in);
    in.close();
    CHECK(exported["streets"][0].contains("name"));
    CHECK(exported["districts"].size() == renamed["districts"].size());
    std::error_code ec;
    std::filesystem::remove(path, ec);

}

