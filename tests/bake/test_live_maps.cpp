#include <doctest/doctest.h>

#include "bake/Backend.h"
#include "bake/Bake.h"
#include "core/ImageIO.h"
#include "core/MainQueue.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/Baker.h"
#include "render/MeshMaps.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
bool haveGpu()
{
    static const bool ok = [] {
        // Live maps run on the resident path only, which this switch turns off.
        if (const char* e = std::getenv("LILA_BAKE_GPU_RESIDENT"); e && std::string_view(e) == "0")
            return false;
        try
        {
            render::createGpuBakeBackend(render::GpuTrace::Auto);
            return true;
        }
        catch (const std::exception& e)
        {
            MESSAGE("no GPU for live map tests: " << e.what());
            return false;
        }
    }();
    return ok;
}

scene::Graph& graph() { return scene::Graph::current(); }

void newScene()
{
    render::shutdownLiveMaps();
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
}

void setMesh(const std::string& path, geo::Mesh m)
{
    scene::Graph& g = graph();
    g.setValue({g.find(path)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
}

std::string addMesh(const std::string& name, geo::Mesh m)
{
    scene::Graph& g = graph();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name);
    setMesh(n->path(), std::move(m));
    return n->path();
}

geo::Mesh plane(int subdiv)
{
    geo::PlaneOptions o;
    o.subdivX = o.subdivZ = subdiv;
    return geo::makePlane(o);
}

template <class F> geo::Mesh displaced(int subdiv, F f)
{
    geo::Mesh m = plane(subdiv);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 q = m.position(p);
        q.y = f(q.x, q.z);
        m.setPosition(p, q);
    }
    return m;
}

/** A bumpy high over a flat low, in a project writing 32-bit EXRs. */
struct Scene
{
    fs::path dir;
    std::string project;
    std::string low;
    scene::NodeId projectId = scene::kNoNode;
};

Scene bumpScene(const char* name, int lowSubdiv, int res, int ss)
{
    newScene();
    Scene s;
    s.dir = fs::temp_directory_path() / ("lila_live_maps_" + std::string(name));
    fs::remove_all(s.dir);
    fs::create_directories(s.dir);
    const double A = 3.0, k = glm::two_pi<double>() / 100.0;
    s.low = addMesh("bump_low", plane(lowSubdiv));
    const std::string high = addMesh("bump_high", displaced(160, [&](double x, double z) { return A * std::cos(k * x) + A * std::cos(k * z); }));
    s.project = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", s.project},
                                   {"folder", s.dir.generic_string()},
                                   {"resolution", res},
                                   {"supersampling", ss},
                                   {"format", "exr"},
                                   {"bits", 32},
                                   {"name", "{group}_{map}"},
                                   {"padding", 4}});
    ops::execute("bakeGroup", {{"args", {"bump"}}, {"project", s.project}, {"lows", {s.low}}, {"highs", {high}}, {"frontal", 10.0}, {"rear", 10.0}});
    s.projectId = graph().find(s.project)->id();
    return s;
}

image::Image readMap(const render::MeshMapInfo& m)
{
    const std::vector<uint8_t> bytes = render::readGpuImage(m.image);
    image::Image img{m.width, m.height, {}};
    render::decodeTexels(m.image->format(), false, bytes, img.rgba);
    return img;
}

/** The largest per-channel difference over the first channels channels, and how many texels exceed tol. */
std::pair<float, size_t> difference(const image::Image& a, const image::Image& b, int channels, float tol)
{
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    float worst = 0.0f;
    size_t over = 0;
    for (size_t i = 0; i < size_t(a.width) * size_t(a.height); ++i)
    {
        float d = 0.0f;
        for (int c = 0; c < channels; ++c)
            d = std::max(d, std::abs(a.rgba[i * 4 + size_t(c)] - b.rgba[i * 4 + size_t(c)]));
        worst = std::max(worst, d);
        over += d > tol ? 1 : 0;
    }
    return {worst, over};
}

json live(const json& flags) { return ops::execute("bakeLiveMaps", flags); }
} // namespace

TEST_CASE("Live mesh maps land in GPU images that match the file-output bake")
{
    if (!haveGpu())
        return;
    const Scene s = bumpScene("match", 1, 64, 4);
    const std::vector<std::string> maps{"normal", "height", "position", "ao"};
    const json baked = ops::execute("bake", {{"project", s.project}, {"maps", maps}, {"backend", "gpu"}});
    const json status = live({{"project", s.project},
                              {"enable", true},
                              {"maps", maps},
                              {"resolution", 64},
                              {"supersampling", 4},
                              {"padding", 4}});
    INFO(status.dump());
    CHECK(status["enabled"] == true);
    CHECK(status["updates"] == 1);
    CHECK(status["last"]["full"] == true);
    for (const json& o : baked["outputs"])
    {
        const std::string map = o["map"].get<std::string>();
        INFO("map " << map);
        const auto m = render::findMeshMap(s.projectId, map, 1001);
        REQUIRE(m);
        CHECK(m->version == 1);
        CHECK(m->low == s.low);
        CHECK(m->image->format() == (map == "height" || map == "ao" ? render::TextureFormat::R16F : render::TextureFormat::RGBA16F));
        std::string err;
        const image::Image file = image::read(fs::path(o["file"].get<std::string>()), &err);
        REQUIRE(file.width == 64);
        const auto [worst, over] = difference(file, readMap(*m), map == "height" || map == "ao" ? 1 : 3, 2e-3f);
        INFO("worst " << worst << ", " << over << " texels over");
        // Half floats: values match to their precision.
        CHECK(over == 0);
        if (map == "height")
            CHECK(m->meta["heightRange"].get<double>() == doctest::Approx(o["meta"]["heightRange"].get<double>()).epsilon(1e-4));
    }
    // By name too, as texture sets ask.
    CHECK(render::findMeshMap(std::string_view("P"), "normal", 1001).has_value());
    CHECK(!render::findMeshMap(s.projectId, "normal", 1002).has_value());
    const json info = ops::execute("bakeMapInfo", {{"project", s.project}, {"map", "height"}});
    REQUIRE(info["images"].size() == 1);
    CHECK(info["images"][0]["format"] == "r16f");
    CHECK(info["images"][0]["version"] == 1);
    CHECK(info["images"][0]["width"] == 64);
    live({{"project", s.project}, {"disable", true}});
    CHECK(!render::findMeshMap(s.projectId, "normal", 1001));
    newScene();
}

TEST_CASE("Live mesh maps: a low edit re-traces only the tiles it touches and bumps versions")
{
    if (!haveGpu())
        return;
    // 32 x 32 quads at 256 texels: a quad is 8 texels, a texel tile (32) covers 4 x 4 quads.
    Scene s = bumpScene("incremental", 32, 256, 1);
    // A second low in its own group that the edit must leave alone.
    geo::Mesh other = plane(4);
    for (geo::Index p = 0; p < other.pointCapacity(); ++p)
        other.setPosition(p, other.position(p) + glm::dvec3(300.0, 0.0, 0.0));
    const std::string otherLow = addMesh("other_low", other);
    const std::string otherHigh = addMesh("other_high", displaced(16, [](double, double) { return 1.0; }));
    ops::execute("bakeGroup", {{"args", {"other"}}, {"project", s.project}, {"lows", {otherLow}}, {"highs", {otherHigh}}, {"frontal", 10.0}, {"rear", 10.0}});

    std::vector<int> changes;
    const int id = render::addMeshMapSetListener([&](const render::MeshMapChange& c) { changes.push_back(int(c.regions.size())); });
    const std::vector<std::string> maps{"normal", "position", "ao"};
    live({{"project", s.project}, {"enable", true}, {"maps", maps}, {"resolution", 256}, {"supersampling", 1}, {"padding", 4}});
    const auto before = render::findMeshMap(s.projectId, "normal", 1001, s.low);
    const auto otherBefore = render::findMeshMap(s.projectId, "normal", 1001, otherLow);
    REQUIRE(before);
    REQUIRE(otherBefore);
    CHECK(before->version == 1);
    changes.clear();

    // Nudge one point near the middle of the low.
    geo::Mesh edited = plane(32);
    geo::Index pick = 0;
    double best = 1e30;
    for (geo::Index p = 0; p < edited.pointCapacity(); ++p)
        if (const double d = glm::length(edited.position(p) - glm::dvec3(10.0, 0.0, 10.0)); d < best)
        {
            best = d;
            pick = p;
        }
    edited.setPosition(pick, edited.position(pick) + glm::dvec3(0.0, 1.5, 0.0));
    setMesh(s.low, edited);

    const json r = live({{"project", s.project}, {"update", true}});
    INFO(r.dump());
    const json& u = r["update"];
    MESSAGE("incremental update: " << u["tilesTraced"] << " of " << (256 / 32) * (256 / 32) << " tiles traced in " << u["seconds"] << " s");
    CHECK(u["full"] == false);
    const int tiles = (256 / 32) * (256 / 32);
    CHECK(u["tilesTraced"].get<int>() > 0);
    CHECK(u["tilesTraced"].get<int>() <= 9);
    CHECK(u["tilesDirty"].get<int>() < tiles / 4);
    CHECK(u["images"] == maps.size());
    const auto after = render::findMeshMap(s.projectId, "normal", 1001, s.low);
    const auto otherAfter = render::findMeshMap(s.projectId, "normal", 1001, otherLow);
    CHECK(after->version == 2);
    CHECK(otherAfter->version == 1);
    CHECK(render::meshMapVersion(s.projectId, "ao", 1001, s.low) == 2);
    REQUIRE(changes.size() == maps.size());
    for (int regions : changes)
        CHECK(regions > 0);

    // The incremental result equals a bake from scratch of the edited scene.
    std::map<std::string, image::Image> incremental;
    for (const std::string& map : maps)
        incremental[map] = readMap(*render::findMeshMap(s.projectId, map, 1001, s.low));
    const json again = live({{"project", s.project}, {"enable", true}, {"maps", maps}, {"resolution", 256}, {"supersampling", 1}, {"padding", 4}});
    CHECK(again["last"]["full"] == true);
    for (const std::string& map : maps)
    {
        INFO("map " << map);
        const auto [worst, over] = difference(incremental[map], readMap(*render::findMeshMap(s.projectId, map, 1001, s.low)), 3, 1e-3f);
        INFO("worst " << worst);
        CHECK(over == 0);
    }

    // Nothing changed: nothing re-traced, no version moves.
    const json idle = live({{"project", s.project}, {"update", true}});
    CHECK(idle["update"]["tilesTraced"] == 0);
    CHECK(idle["update"]["images"] == 0);

    // A cage stroke in progress rebakes just as incrementally.
    std::vector<float> offsets(edited.pointCapacity(), 1.0f);
    offsets[pick] = 1.6f;
    render::setLiveCageOffsets(s.projectId, "", s.low, offsets);
    const json stroke = live({{"project", s.project}, {"update", true}});
    CHECK(stroke["update"]["full"] == false);
    CHECK(stroke["update"]["tilesTraced"].get<int>() > 0);
    CHECK(stroke["update"]["tilesTraced"].get<int>() <= 9);
    render::removeMeshMapSetListener(id);
    newScene();
}

TEST_CASE("Live mesh maps rebake off the main thread after an edit, debounced")
{
    if (!haveGpu())
        return;
    Scene s = bumpScene("async", 8, 64, 1);
    render::setLiveMapsAsync(true);
    live({{"project", s.project}, {"enable", true}, {"maps", {"normal"}}, {"resolution", 64}, {"debounce", 20}});
    auto waitIdle = [] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!render::liveMapsIdle() && std::chrono::steady_clock::now() < until)
        {
            mainq::drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        mainq::drain();
    };
    waitIdle();
    const auto first = render::findMeshMap(s.projectId, "normal", 1001);
    REQUIRE(first);
    CHECK(first->version == 1);
    geo::Mesh edited = plane(8);
    edited.setPosition(0, edited.position(0) + glm::dvec3(0.0, 2.0, 0.0));
    setMesh(s.low, edited);
    waitIdle();
    CHECK(render::meshMapVersion(s.projectId, "normal", 1001) == 2);
    const json st = live({{"project", s.project}});
    CHECK(st["updates"] == 2);
    CHECK(st["last"]["full"] == false);
    render::setLiveMapsAsync(false);
    newScene();
}
