#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "io/FileTranslators.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "seq/MeshSequence.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace seqnodes
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

void setupNodes()
{
    static bool once = [] {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        io::translators::registerCommands();
        gn::registerTypes();
        gn::registerCommands();
        seq::registerCommands();
        return true;
    }();
    (void)once;
}

fs::path freshDir(const char* name)
{
    fs::path d = fs::temp_directory_path() / ("lila_seqnodes_" + std::string(name));
    fs::remove_all(d);
    return d;
}

/** Group input -> nodes... -> group output, each linked geometry to geometry. */
void chain(const std::string& g, const std::vector<std::string>& nodes)
{
    run("gnDisconnect", args({g, "GroupOutput.geometry"}));
    std::string from = "GroupInput.geometry";
    for (const std::string& n : nodes)
    {
        run("gnConnect", args({g, from, n + ".geometry"}));
        from = n + ".geometry";
    }
    run("gnConnect", args({g, from, "GroupOutput.geometry"}));
}
} // namespace seqnodes

TEST_CASE("a Vertex Animation Texture node encodes what reaches it over time")
{
    using namespace seqnodes;
    setupNodes();
    run("newScene");
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 4.0 / 30.0}});
    const std::string box = run("polyCube", {{"name", "Box"}})[0];
    run("gnCreateGroup", args({"Anim"}));
    std::string time = run("gnAddNode", args({"Anim", "SceneTime"})).get<std::string>();
    std::string xyz = run("gnAddNode", args({"Anim", "CombineXYZ"})).get<std::string>();
    std::string move = run("gnAddNode", args({"Anim", "TransformGeometry"})).get<std::string>();
    std::string vat = run("gnAddNode", args({"Anim", "VertexAnimationTexture"})).get<std::string>();
    run("gnConnect", args({"Anim", time + ".frame", xyz + ".y"}));
    run("gnConnect", args({"Anim", xyz + ".vector", move + ".translation"}));
    chain("Anim", {move, vat});
    run("gnApply", args({box, "Anim"}));
    fs::path dir = freshDir("vat");
    json r = run("gnEncodeVat", {{"args", {"Anim", vat}}, {"folder", dir.string()}});
    CHECK(fs::exists(r["files"]["positions"].get<std::string>()));
    CHECK(fs::exists(r["files"]["mesh"].get<std::string>()));
    CHECK(r["materialParams"]["FrameCount"] == 4);
    fs::remove_all(dir);
}

TEST_CASE("an Imposter node bakes octahedral atlases and gives a card")
{
    using namespace seqnodes;
    setupNodes();
    run("newScene");
    const std::string ball = run("polySphere", {{"radius", 50.0}, {"name", "Ball"}})[0];
    run("gnCreateGroup", args({"Imp"}));
    std::string imp = run("gnAddNode", args({"Imp", "Imposter"})).get<std::string>();
    run("gnSetValue", args({"Imp", imp + ".frames", 4}));
    run("gnSetValue", args({"Imp", imp + ".resolution", 32}));
    run("gnDisconnect", args({"Imp", "GroupOutput.geometry"}));
    run("gnConnect", args({"Imp", "GroupInput.geometry", imp + ".geometry"}));
    run("gnConnect", args({"Imp", imp + ".card", "GroupOutput.geometry"}));
    run("gnApply", args({ball, "Imp"}));
    json info = run("meshInfo", args({ball}));
    CHECK(info["faces"] == 1);
    CHECK(info["boundsMax"][0].get<double>() == doctest::Approx(50.0).epsilon(0.02));

    fs::path dir = freshDir("imposter");
    json r = run("gnBakeImposter", {{"args", {"Imp", imp}}, {"folder", dir.string()}});
    CHECK(fs::exists(r["files"]["baseColor"].get<std::string>()));
    CHECK(fs::exists(r["files"]["normal"].get<std::string>()));
    CHECK(fs::exists(r["files"]["mesh"].get<std::string>()));
    CHECK(r["materialParams"]["FramesXY"] == 4);
    CHECK(r["materialParams"]["Default Mesh Size"].get<double>() == doctest::Approx(100.0).epsilon(0.02));
    fs::remove_all(dir);
}
