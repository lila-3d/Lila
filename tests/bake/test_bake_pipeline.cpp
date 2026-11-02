#include <doctest/doctest.h>

#include "bake/Bake.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/BakeWrite.h"
#include "core/ImageIO.h"
#include "core/MainQueue.h"
#include "core/Workspace.h"
#include "geo/Mesh.h"
#include "io/SceneFile.h"
#include "link/TextureSend.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
fs::path tempDir(const char* name)
{
    fs::path d = fs::temp_directory_path() / ("lila_bakef_" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

scene::Graph& graph() { return scene::Graph::current(); }

void newScene()
{
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
}

std::string addMesh(const std::string& name, geo::Mesh m)
{
    scene::Graph& g = graph();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n->path();
}

geo::Mesh plane(int subdiv)
{
    geo::PlaneOptions o;
    o.subdivX = o.subdivZ = subdiv;
    return geo::makePlane(o);
}

geo::Mesh bumpy(int subdiv)
{
    geo::Mesh m = plane(subdiv);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 q = m.position(p);
        q.y = 2.0 * std::cos(q.x * 0.06) * std::cos(q.z * 0.06);
        m.setPosition(p, q);
    }
    return m;
}

/** A low/high pair in one group of project P writing into dir. */
std::string setUp(const fs::path& dir, const std::string& format, int bits = 8, const std::string& name = "{mesh}_{map}")
{
    const std::string low = addMesh("crate_low", plane(1));
    const std::string high = addMesh("crate_high", bumpy(40));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", dir.generic_string()}, {"resolution", 32}, {"supersampling", 1},
                                   {"format", format}, {"bits", bits}, {"name", name}, {"padding", 2}});
    ops::execute("bakeGroup", {{"args", {"crate"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 5.0}, {"rear", 5.0}});
    return p;
}

std::vector<float> ramp(int w, int h)
{
    std::vector<float> v(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float* p = &v[(size_t(y) * size_t(w) + size_t(x)) * 4];
            p[0] = float(x) / float(w - 1);
            p[1] = float(y) / float(h - 1);
            p[2] = 0.25f + 0.0005f * float(x * y);
            p[3] = (x + y) % 2 ? 1.0f : 0.5f;
        }
    return v;
}

void checkClose(const image::Image& img, const std::vector<float>& want, float tol)
{
    REQUIRE(img.rgba.size() == want.size());
    float worst = 0.0f;
    for (size_t i = 0; i < want.size(); ++i)
        worst = std::max(worst, std::abs(img.rgba[i] - want[i]));
    CHECK(worst <= tol);
}
} // namespace

TEST_CASE("formats round trip: exr float, tiff 8/16/32 deflate and raw, psd layers 8/16")
{
    const fs::path dir = tempDir("formats");
    const int W = 37, H = 23;
    const std::vector<float> src = ramp(W, H);
    std::string err;

    std::vector<float> hdr = src;
    hdr[0] = 1234.5678f;
    hdr[5] = -0.000123f;
    image::writeExr(dir / "f.exr", W, H, hdr, 32);
    const image::Image exr = image::readExr(dir / "f.exr", &err);
    INFO(err);
    CHECK(exr.rgba == hdr);

    for (bool deflate : {true, false})
        for (int bits : {8, 16, 32})
        {
            INFO("tiff " << bits << (deflate ? " deflate" : " raw"));
            const fs::path f = dir / ("t" + std::to_string(bits) + (deflate ? "z" : "r") + ".tif");
            image::writeTiff(f, W, H, bits == 32 ? hdr : src, bits, deflate);
            const image::Image t = image::read(f, &err);
            INFO(err);
            if (bits == 32)
                CHECK(t.rgba == hdr);
            else
                checkClose(t, src, bits == 8 ? 0.5f / 255.0f + 1e-6f : 0.5f / 65535.0f + 1e-6f);
        }

    std::vector<float> second(src.size());
    for (size_t i = 0; i < src.size(); ++i)
        second[i] = 1.0f - src[i];
    for (int bits : {8, 16})
    {
        INFO("psd " << bits);
        const fs::path f = dir / ("layers" + std::to_string(bits) + ".psd");
        const image::Layer layers[] = {{"N", src}, {"AO", second}};
        image::writePsd(f, W, H, layers, bits);
        const float tol = bits == 8 ? 0.5f / 255.0f + 1e-6f : 0.5f / 65535.0f + 1e-6f;
        const image::Image flat = image::readPsd(f, &err);
        INFO(err);
        checkClose(flat, src, tol);
        const auto back = image::readPsdLayers(f, &err);
        INFO(err);
        REQUIRE(back.size() == 2);
        // Stored bottom first: the first layer given is the top one.
        CHECK(back[0].first == "AO");
        CHECK(back[1].first == "N");
        checkClose(back[0].second, second, tol);
        checkClose(back[1].second, src, tol);
    }
}

TEST_CASE("triangular dither keeps the mean and leaves exact 8-bit values alone")
{
    double sum = 0.0;
    int changed = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const uint8_t q = image::quantize8(0.3f, x, y, 0, true);
            sum += q;
            changed += q != image::quantize8(0.3f, x, y, 0, false);
            CHECK(image::quantize8(128.0f / 255.0f, x, y, 1, true) == 128);
            CHECK(image::quantize8(0.0f, x, y, 2, true) == 0);
            CHECK(image::quantize8(1.0f, x, y, 3, true) == 255);
        }
    CHECK(sum / 4096.0 == doctest::Approx(0.3 * 255.0).epsilon(0.002));
    CHECK(changed > 500);
    CHECK(image::quantize8(0.3f, 5, 9, 0, true) == image::quantize8(0.3f, 5, 9, 0, true));
}

TEST_CASE("UDIM labels and the metadata sidecar")
{
    CHECK(image::udimLabel(1001, "1001") == "1001");
    CHECK(image::udimLabel(1012, "uv") == "u2_v2");
    CHECK(image::udimLabel(1012, "uv0") == "u1_v1");
    for (const char* style : {"1001", "uv", "uv0"})
        for (int udim : {1001, 1002, 1010, 1011, 1095})
            CHECK(image::udimFromLabel(image::udimLabel(udim, style), style) == udim);
    CHECK(image::udimFromLabel("u0_v0", "uv") == 0);

    bake::NameTokens t;
    t.mesh = "rock";
    t.map = "N";
    t.ext = "png";
    t.udim = 1002;
    t.udimStyle = "uv";
    CHECK(bake::expandName("{mesh}_{map}.{udim}", t) == "rock_N.u2_v1.png");
    CHECK(bake::layeredName("T_{mesh}_{map}", t, false) == "T_rock.psd");
    CHECK(bake::layeredName("{map}-{mesh}", t, false) == "rock.psd");
    CHECK(bake::formatBits("exr", 8) == 16);
    CHECK(bake::formatBits("tif", 32) == 32);
    CHECK(bake::formatBits("psd", 32) == 16);
    CHECK(bake::formatBits("tga", 16) == 8);

    const fs::path dir = tempDir("sidecar");
    image::writeSidecar(dir / "a.png", {{"map", "normal"}});
    CHECK(image::readSidecar(dir / "a.png")["map"] == "normal");
    CHECK(image::readSidecar(dir / "missing.png").is_null());
}

TEST_CASE("bakes write tiff, float exr, layered psd and sidecars")
{
    newScene();
    const fs::path dir = tempDir("bakeformats");
    const std::string p = setUp(dir, "tif", 16);
    ops::execute("setBakeOutput", {{"project", p}, {"sidecar", true}, {"dither", true}});
    json r = ops::execute("bake", {{"project", p}, {"maps", {"normal", "height"}}});
    REQUIRE(r["outputs"].size() == 2);
    for (const json& o : r["outputs"])
    {
        CHECK(o["format"] == "tif");
        CHECK(o["bits"] == 16);
        std::string err;
        CHECK(image::read(o["file"].get<std::string>(), &err).width == 32);
        const json side = image::readSidecar(o["file"].get<std::string>());
        REQUIRE(side.is_object());
        CHECK(side["map"] == o["map"]);
        CHECK(side["colorSpace"] == "linear");
        if (o["map"] == "normal")
        {
            CHECK(side["normalFormat"] == "directx");
            CHECK(o["normalFormat"] == "directx");
        }
    }

    ops::execute("setBakeOutput", {{"project", p}, {"format", "exr"}, {"bits", 32}});
    r = ops::execute("bake", {{"project", p}, {"maps", {"height"}}});
    CHECK(r["outputs"][0]["bits"] == 32);
    CHECK(r["outputs"][0]["file"].get<std::string>().ends_with(".exr"));

    ops::execute("setBakeOutput", {{"project", p}, {"format", "psd"}, {"bits", 8}, {"name", "T_{mesh}_{map}"}});
    r = ops::execute("bake", {{"project", p}, {"maps", {"normal", "height", "id"}}});
    REQUIRE(r["outputs"].size() == 3);
    const std::string psd = r["outputs"][0]["file"].get<std::string>();
    CHECK(fs::path(psd).filename() == "T_crate_low.psd");
    for (const json& o : r["outputs"])
    {
        CHECK(o["file"] == psd);
        CHECK(!o["layer"].get<std::string>().empty());
    }
    std::string err;
    const auto layers = image::readPsdLayers(psd, &err);
    INFO(err);
    REQUIRE(layers.size() == 3);
    CHECK(layers.back().first == r["outputs"][0]["layer"].get<std::string>());

    ops::execute("setBakeOutput", {{"project", p}, {"format", "png"}, {"name", "{mesh}_{map}.{udim}"}, {"udimNaming", "uv0"}});
    r = ops::execute("bake", {{"project", p}, {"maps", {"height"}}, {"dryRun", true}});
    CHECK(fs::path(r["outputs"][0]["file"].get<std::string>()).filename() == "crate_low_height.u0_v0.png");
}

TEST_CASE("input hashes skip unchanged bakes, and saving rebakes changed projects")
{
    newScene();
    const fs::path dir = tempDir("rebake");
    const std::string p = setUp(dir, "png");
    ops::execute("setBakeMap", {{"args", {"ao"}}, {"project", p}, {"enabled", false}});
    scene::Graph& g = graph();
    const scene::NodeId pid = g.find(p)->id();
    const std::string h0 = bake::inputHash(g, pid);
    CHECK(h0.size() == 16);
    CHECK(bake::inputHash(g, pid) == h0);

    json st = ops::execute("bakeUpToDate", {{"project", p}});
    CHECK(st["upToDate"] == false);
    CHECK(st["reason"] == "never baked");

    // Only the save hook's flag; not part of the hash.
    ops::execute("setBakeProject", {{"project", p}, {"rebakeOnSave", true}});
    CHECK(bake::inputHash(g, pid) == h0);

    json saved = bake::onSceneSaved();
    REQUIRE(saved.size() == 1);
    CHECK(saved[0]["baked"] == true);
    st = ops::execute("bakeUpToDate", {{"project", p}});
    CHECK(st["upToDate"] == true);
    CHECK(st["bakedHash"] == h0);
    saved = bake::onSceneSaved();
    CHECK(saved[0]["baked"] == false);
    CHECK(saved[0]["reason"] == "up to date");

    json all = ops::execute("bakeAll", {{"changedOnly", true}});
    REQUIRE(all.size() == 1);
    CHECK(all[0]["baked"] == false);

    // Moving the high changes what the bake sees.
    scene::Node* high = g.find("crate_high");
    g.setValue({high->id(), scene::Transform::kTranslate, -1}, glm::dvec3(0.0, 0.5, 0.0));
    CHECK(bake::inputHash(g, pid) != h0);
    st = ops::execute("bakeUpToDate", {{"project", p}});
    CHECK(st["reason"] == "inputs changed");
    saved = bake::onSceneSaved();
    CHECK(saved[0]["baked"] == true);
    CHECK(ops::execute("bakeUpToDate", {{"project", p}})["upToDate"] == true);

    // A deleted texture needs a bake too.
    const json m = bake::readManifest(*g.find(p));
    fs::remove(m["outputs"][0]["file"].get<std::string>());
    CHECK(ops::execute("bakeUpToDate", {{"project", p}})["reason"].get<std::string>().starts_with("missing"));
    all = ops::execute("bakeAll", {{"changedOnly", true}});
    CHECK(all[0]["baked"] == true);

    ops::execute("setBakeProject", {{"project", p}, {"rebakeOnSave", false}});
    CHECK(bake::onSceneSaved().empty());
}

TEST_CASE("rebake on save runs off the main thread, restarts when a save changes its inputs and assigns once it lands")
{
    newScene();
    mainq::drain();
    const fs::path dir = tempDir("rebakeAsync");
    const std::string p = setUp(dir, "png");
    for (const char* m : {"objectNormal", "curvature", "cavity", "thickness", "ao"})
        ops::execute("setBakeMap", {{"args", {m}}, {"project", p}, {"enabled", false}});
    ops::execute("setBakeProject", {{"project", p}, {"rebakeOnSave", true}, {"autoAssign", true}});
    int started = 0;
    const int listener = bake::addBakeListener([&](const bake::BakeEvent& e) { started += e.kind == bake::BakeEvent::Kind::Started; });
    bake::setRebakeAsync(true);

    // Heavy enough to still be running at the next save.
    ops::execute("setBakeOutput", {{"project", p}, {"resolution", 1024}, {"supersampling", 16}});
    json saved = bake::onSceneSaved();
    REQUIRE(saved.size() == 1);
    CHECK(saved[0]["started"] == true);
    CHECK(bake::rebakesPending());
    CHECK(bake::bakeRunning());

    saved = bake::onSceneSaved();
    CHECK(saved[0]["started"] == false);
    CHECK(saved[0]["reason"] == "already rebaking");

    ops::execute("setBakeOutput", {{"project", p}, {"resolution", 32}, {"supersampling", 1}});
    saved = bake::onSceneSaved();
    CHECK(saved[0]["queued"] == true);
    CHECK(saved[0]["reason"].get<std::string>().ends_with("restarting"));

    const auto t0 = std::chrono::steady_clock::now();
    while (bake::rebakesPending() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        mainq::drain();
    }
    CHECK_FALSE(bake::rebakesPending());
    CHECK(started == 2);
    const json last = ops::execute("bakeStatus", {{"project", p}})["last"];
    CHECK(last["outputs"].size() == 3);
    CHECK(last["outputs"][0]["width"] == 32);
    CHECK(scene::materials::find(graph(), "M_crate_low") != nullptr);

    bake::removeBakeListener(listener);
    bake::setRebakeAsync(false);
}

TEST_CASE("assignTexture wires a file into the material on a face by its name, and undoes")
{
    CHECK(bake::mapFromFileName("T_Rock_N.png") == "normal");
    CHECK(bake::mapFromFileName("M_Outfit_ao.png") == "ao");
    CHECK(bake::mapFromFileName("T_Rock_BaseColor.1001.png") == "baseColor");
    CHECK(bake::mapFromFileName("T_Rock_CRM.png") == "orm");
    CHECK(bake::mapFromFileName("notes.png").empty());

    newScene();
    const std::string mesh = addMesh("rock", plane(1));
    UndoStack::get().clear();
    const json r = ops::execute("assignTexture", {{"args", {"C:/tex/T_Rock_N.png"}}, {"mesh", mesh}, {"face", 0}});
    CHECK(r["created"] == json::array({"M_rock"}));
    REQUIRE(r["assigned"].size() == 1);
    CHECK(r["assigned"][0]["channel"] == "normal");
    scene::Node* mat = scene::materials::find(graph(), "M_rock");
    REQUIRE(mat);
    CHECK(!mat->as<std::string>(scene::material::kNormalMap).empty());

    ops::execute("assignTexture", {{"args", {"C:/tex/rock_ao.png"}}, {"mesh", mesh}});
    CHECK(!mat->as<std::string>(scene::material::kAmbientOcclusionMap).empty());
    UndoStack::get().undo();
    CHECK(mat->as<std::string>(scene::material::kAmbientOcclusionMap).empty());
    CHECK_THROWS(ops::execute("assignTexture", {{"args", {"C:/tex/notes.png"}}, {"mesh", mesh}}));
}

TEST_CASE("bakeAssign wires outputs into the lows' materials and undoes")
{
    newScene();
    const fs::path dir = tempDir("assign");
    const std::string p = setUp(dir, "png");
    ops::execute("createMaterial", {{"args", {"M_Crate"}}});
    ops::execute("assignMaterial", {{"args", {"M_Crate", "crate_low"}}});
    ops::execute("setBakeMap", {{"args", {"normal"}}, {"project", p}, {"options", {{"greenUp", true}}}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"normal", "height"}}});
    REQUIRE(r["outputs"].size() == 2);
    CHECK(r["outputs"][0]["materials"] == json::array({"M_Crate"}));

    UndoStack::get().clear();
    const json a = ops::execute("bakeAssign", {{"project", p}});
    REQUIRE(a["assigned"].size() == 1);
    CHECK(a["assigned"][0]["channel"] == "normal");
    CHECK(a["skipped"].size() == 1);
    scene::Graph& g = graph();
    scene::Node* mat = scene::materials::find(g, "M_Crate");
    REQUIRE(mat);
    const std::string stored = mat->as<std::string>(scene::material::kNormalMap);
    CHECK(fs::path(scene::materials::resolvePath(stored)).filename() == fs::path(r["outputs"][0]["file"].get<std::string>()).filename());
    CHECK(mat->as<int64_t>(scene::material::kNormalFormat) == int64_t(scene::material::NormalFormat::OpenGL));

    UndoStack::get().undo();
    CHECK(mat->as<std::string>(scene::material::kNormalMap).empty());

    // A low with no material gets M_<mesh> on every face.
    newScene();
    const std::string p2 = setUp(tempDir("assign2"), "png");
    ops::execute("bake", {{"project", p2}, {"maps", {"normal"}}});
    const json a2 = ops::execute("bakeAssign", {{"project", p2}});
    CHECK(a2["created"] == json::array({"M_crate_low"}));
    scene::Node* own = scene::materials::find(graph(), "M_crate_low");
    REQUIRE(own);
    CHECK(!own->as<std::string>(scene::material::kNormalMap).empty());
    const json slots = ops::execute("listMaterials", {{"args", {"crate_low"}}});
    CHECK(slots.dump().find("M_crate_low") != std::string::npos);

    CHECK(bake::materialChannel("orm") == "orm");
    CHECK(bake::materialChannel("ao") == "ambientOcclusion");
    CHECK(bake::materialChannel("height").empty());
}

TEST_CASE("relative output folders resolve under the workspace, else the saved scene's folder, else the current folder")
{
    Workspace::get().close();
    io::setCurrentPath({});
    CHECK(bake::resolveFolder("bake") == (fs::current_path() / "bake").lexically_normal());
    CHECK(bake::resolveFolder("") == (fs::current_path() / "bake").lexically_normal());

    const fs::path root = tempDir("folders");
    io::setCurrentPath(root / "scenes" / "ship.lila");
    CHECK(bake::resolveFolder("bake") == (root / "scenes" / "bake").lexically_normal());
    CHECK(bake::resolveFolder("../textures") == (root / "textures").lexically_normal());
    CHECK(bake::resolveFolder((root / "abs").generic_string()) == (root / "abs").lexically_normal());

    const fs::path ws = root / "ws";
    fs::create_directories(ws);
    Workspace::get().open(ws);
    CHECK(bake::resolveFolder("bake") == (ws / "bake").lexically_normal());
    Workspace::get().close();
    io::setCurrentPath({});
}

TEST_CASE("autoAssign: every finished bake, sync or async, assigns once as its own undo step")
{
    newScene();
    mainq::drain();
    const fs::path dir = tempDir("autoAssign");
    const std::string p = setUp(dir, "png");
    scene::Graph& g = graph();
    const scene::NodeId pid = g.find(p)->id();
    auto assigns = [] {
        return std::ranges::count_if(UndoStack::get().steps(), [](const UndoStack::Step& s) { return s.label == "bakeAssign"; });
    };
    auto normalOf = [] {
        scene::Node* m = scene::materials::find(graph(), "M_crate_low");
        return m ? m->as<std::string>(scene::material::kNormalMap) : std::string();
    };

    // Off: nothing is assigned.
    ops::execute("bake", {{"project", p}, {"maps", {"normal"}}});
    CHECK(assigns() == 0);
    CHECK(normalOf().empty());

    ops::execute("setBakeProject", {{"project", p}, {"autoAssign", true}});
    UndoStack::get().clear();
    ops::execute("bake", {{"project", p}, {"maps", {"normal"}}});
    CHECK(assigns() == 1);
    CHECK(UndoStack::get().undoLabel() == "bakeAssign");
    CHECK(!normalOf().empty());
    CHECK(bake::autoAssigned(pid)["assigned"].size() == 1);
    UndoStack::get().undo();
    CHECK(normalOf().empty());

    // A dry run writes nothing, so it assigns nothing.
    UndoStack::get().clear();
    ops::execute("bake", {{"project", p}, {"maps", {"normal"}}, {"dryRun", true}});
    CHECK(assigns() == 0);

    // bakeAll reports the auto-assign rather than running a second one, even with assign=True.
    const json all = ops::execute("bakeAll", {{"assign", true}, {"backend", "cpu"}});
    REQUIRE(all.size() == 1);
    CHECK(all[0]["assign"]["assigned"].size() >= 1);
    CHECK(assigns() == 1);

    // Off the main thread: it lands, then assigns on the main thread. Undone first, so assigning changes something again.
    UndoStack::get().undo();
    CHECK(normalOf().empty());
    UndoStack::get().clear();
    ops::execute("bake", {{"project", p}, {"maps", {"normal"}}, {"async", true}});
    const auto t0 = std::chrono::steady_clock::now();
    while ((bake::bakeRunning() || assigns() == 0) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        mainq::drain();
    }
    mainq::drain();
    CHECK(assigns() == 1);
    CHECK(!normalOf().empty());

    // The sync save hook assigns through the same path, once.
    UndoStack::get().undo();
    CHECK(normalOf().empty());
    ops::execute("setBakeProject", {{"project", p}, {"rebakeOnSave", true}});
    scene::Node* high = g.find("crate_high");
    g.setValue({high->id(), scene::Transform::kTranslate, -1}, glm::dvec3(0.0, 0.25, 0.0));
    UndoStack::get().clear();
    const json saved = bake::onSceneSaved();
    REQUIRE(saved.size() == 1);
    CHECK(saved[0]["baked"] == true);
    CHECK(assigns() == 1);
}

TEST_CASE("Unreal texture import params: per map settings, UDIM sets and material instances")
{
    const json result = {{"outputs",
                          {{{"file", "C:/b/T_rock_N.1001.png"}, {"map", "normal"}, {"udim", 1001}, {"format", "png"}, {"bits", 8},
                            {"colorSpace", "linear"}, {"normalFormat", "opengl"}, {"materials", {"M_Rock"}}, {"meshes", {"rock_low"}}},
                           {{"file", "C:/b/T_rock_N.1002.png"}, {"map", "normal"}, {"udim", 1002}, {"format", "png"}, {"bits", 8},
                            {"colorSpace", "linear"}, {"normalFormat", "opengl"}, {"materials", {"M_Rock"}}, {"meshes", {"rock_low"}}},
                           {{"file", "C:/b/T_rock_ORM.png"}, {"map", "orm"}, {"udim", 1001}, {"format", "png"}, {"bits", 8},
                            {"colorSpace", "linear"}, {"materials", {"M_Rock"}}, {"meshes", {"rock_low"}}},
                           {{"file", "C:/b/T_rock_H.exr"}, {"map", "height"}, {"udim", 1001}, {"format", "exr"}, {"bits", 16},
                            {"colorSpace", "linear"}, {"materials", {"M_Rock"}}, {"meshes", {"rock_low"}}},
                           {{"file", "C:/b/T_rock.psd"}, {"map", "id"}, {"layer", "ID"}, {"udim", 1001}, {"materials", {"M_Rock"}}}}}};
    json warnings = json::array();
    const json p = link::textureImportParams(
        result,
        {{"folder", "/Game/Rock"}, {"materialMode", "instance"}, {"materialParent", "/Game/M_Master"}, {"meshes", {{"rock_low", "/Game/Rock/SM_Rock"}}}},
        &warnings);
    CHECK(warnings.size() == 1);
    REQUIRE(p["textures"].size() == 3);
    const json& n = p["textures"][0];
    CHECK(n["name"] == "T_rock_N");
    CHECK(n["files"].size() == 2);
    CHECK(n["virtualTexture"] == true);
    CHECK(n["flipGreen"] == true);
    CHECK(n["srgb"] == false);
    CHECK(n["compression"] == "normalmap");
    CHECK(n["lodGroup"] == "WorldNormalMap");
    CHECK(p["textures"][1]["compression"] == "masks");
    CHECK(p["textures"][1].value("virtualTexture", false) == false);
    CHECK(p["textures"][2]["compression"] == "hdr");
    REQUIRE(p["materials"].size() == 1);
    const json& mi = p["materials"][0];
    CHECK(mi["name"] == "MI_Rock");
    CHECK(mi["slot"] == "M_Rock");
    CHECK(mi["parameters"]["NormalMap"] == "T_rock_N");
    CHECK(mi["parameters"]["ORMMap"] == "T_rock_ORM");
    CHECK(mi["parameters"]["HeightMap"] == "T_rock_H");
    CHECK(mi["meshes"] == json::array({"/Game/Rock/SM_Rock"}));

    CHECK(link::textureAssetName("C:/b/rock_N.u2_v1.png", 1002) == "rock_N");
    CHECK(link::textureAssetName("C:/b/rock_N_1003.png", 1003) == "rock_N");
    CHECK(link::textureSettings("baseColor", "srgb", "png", 8)["srgb"] == true);
    CHECK(link::textureSettings("id", "srgb", "png", 8)["compression"] == "uncompressed");
}
