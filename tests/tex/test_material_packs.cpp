#include "TexTest.h"

#include "core/Paths.h"
#include "ops/Command.h"
#include "tex/Library.h"
#include "tex/Lint.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace textest;
namespace lib = lila::tex::library;

namespace
{
namespace fs = std::filesystem;

/** The shipped .lilamat files: the built-in set and the material packs under extensions/. */
std::vector<fs::path> shippedMaterials()
{
    std::vector<fs::path> dirs = {paths::resourcesDir() / "materials"};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(paths::resourcesDir().parent_path() / "extensions", ec))
        dirs.push_back(e.path() / "resources" / "materials");
    std::vector<fs::path> out;
    for (const fs::path& d : dirs)
        for (const auto& e : fs::recursive_directory_iterator(d, ec))
            if (e.path().extension() == ".lilamat")
                out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

json readJson(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

void checkMeta(const json& doc, const Graph& g)
{
    REQUIRE(doc.contains("meta"));
    const json& m = doc["meta"];
    for (const char* key : {"name", "category", "description", "author", "licence", "version", "physical", "seedParam"})
    {
        CAPTURE(key);
        REQUIRE(m.contains(key));
        REQUIRE(m[key].is_string());
        CHECK_FALSE(m[key].get<std::string>().empty());
    }
    CHECK(m["author"] == "Lila");
    static const std::set<std::string> physical = {"metal", "dielectric", "organic", "emissive", "stylised"};
    CHECK(physical.contains(m["physical"].get<std::string>()));
    REQUIRE(m["tags"].is_array());
    CHECK_FALSE(m["tags"].empty());
    REQUIRE(m["scaleCm"].is_number());
    CHECK(m["scaleCm"].get<double>() > 0.0);
    REQUIRE(m["tileable"].is_boolean());
    REQUIRE(m["variationParams"].is_array());

    // The seed and every variation parameter are exposed inputs of the graph.
    CHECK(g.input(m["seedParam"].get<std::string>()));
    std::set<std::string> vars;
    for (const json& v : m["variationParams"])
    {
        CHECK(g.input(v.get<std::string>()));
        vars.insert(v.get<std::string>());
    }
    for (const char* v : {"wear", "age", "colorVariation"})
    {
        CAPTURE(v);
        CHECK(vars.contains(v));
    }
}
} // namespace

TEST_CASE("material packs: every shipped material loads, evaluates on the CPU and passes materialLint")
{
    reloadLibrary();
    std::vector<fs::path> files = shippedMaterials();
    REQUIRE(files.size() >= 80);
    std::set<std::string> names;
    for (const fs::path& f : files)
    {
        CAPTURE(f.string());
        json doc = readJson(f);
        CHECK(doc["format"] == "lilamat");
        Graph g = Graph::fromJson(doc["graph"]);
        SetSettings s = doc.contains("settings") ? SetSettings::fromJson(doc["settings"]) : SetSettings::makeDefault();
        checkMeta(doc, g);
        CHECK(names.insert(doc["meta"]["name"].get<std::string>()).second);

        PlanRequest req;
        req.width = req.height = 64;
        req.outputs = channelRequests(g, s);
        REQUIRE_FALSE(req.outputs.empty());
        Plan plan = buildPlan(g, req);
        REQUIRE_MESSAGE(plan.error.empty(), plan.error);
        std::string warnings;
        for (const std::string& w : plan.warnings)
            warnings += w + "; ";
        CHECK_MESSAGE(plan.warnings.empty(), warnings);
        EvalResult r = evaluateCpu(plan);
        for (const auto& [name, img] : r.outputs)
        {
            CAPTURE(name);
            REQUIRE(img.width == 64);
            CHECK(std::all_of(img.rgba.begin(), img.rgba.end(), [](float v) { return std::isfinite(v); }));
        }

        lint::Report rep = lint::run(g, s, lib::Meta::fromJson(doc["meta"]), {.size = 64, .backend = "cpu"});
        CHECK_MESSAGE(rep.passed(), rep.toJson().dump());
    }
}

TEST_CASE("material packs: every shipped smart mask applies to a layer and evaluates")
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(paths::resourcesDir().parent_path() / "extensions", ec))
        for (const auto& f : fs::directory_iterator(e.path() / "resources" / "smartmaterials", ec))
            if (f.path().extension() == ".lilasmart")
                files.push_back(f.path());
    REQUIRE(files.size() >= 5);
    for (const fs::path& f : files)
    {
        CAPTURE(f.string());
        ops::execute("newScene", json::object());
        ops::execute("texCreateSet", {{"args", {"Masked"}}, {"resolution", 16}});
        json layer = ops::execute("texAddLayer", {{"args", {"Masked", "fill"}}});
        json r = ops::execute("texApplySmartMaterial", {{"args", {"Masked", f.generic_string()}}, {"layer", layer["id"]}, {"addMaps", false}});
        CHECK_FALSE(r.contains("warnings"));
        CHECK(r["masks"].size() >= 1);
        json out = ops::execute("texEvaluate", {{"args", {"Masked"}}, {"backend", "cpu"}, {"channels", {"baseColor"}},
                                                {"folder", (fs::temp_directory_path() / "lila_material_pack_masks").generic_string()}});
        CHECK(out["files"].size() == 1);
    }
}
