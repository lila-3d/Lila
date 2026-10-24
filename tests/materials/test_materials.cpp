#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "io/SceneFile.h"
#include "scene/Graph.h"
#include "scene/Components.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"

#include <filesystem>
#include <map>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

/** Face count per slot name of an object's evaluated mesh, checking every face id points at a named slot. */
std::map<std::string, int> slots(const std::string& object)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(object);
    REQUIRE(n);
    const scene::MeshPtr& m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    std::span<const int32_t> ids = m->faceAttrs().get<int32_t>(geo::attr::kMaterial);
    std::map<std::string, int> out;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
    {
        if (!m->faceValid(f))
            continue;
        int32_t id = ids.empty() ? 0 : ids[f];
        CHECK(id >= 0);
        CHECK((m->materialNames().empty() ? id == 0 : size_t(id) < m->materialNames().size()));
        ++out[scene::materials::slotName(*m, id)];
    }
    return out;
}

std::filesystem::path tempFile(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_material_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}
} // namespace

TEST_CASE("create, assign and undo")
{
    run("newScene");
    std::string red = run("createMaterial", {{"args", {"M_Red"}}, {"baseColor", {0.8, 0.05, 0.05}}, {"roughness", 0.3}});
    CHECK(red == "M_Red");
    json info = run("materialInfo", {{"args", {red}}});
    CHECK(info["baseColor"][0].get<double>() == doctest::Approx(0.8));
    CHECK(info["roughness"].get<double>() == doctest::Approx(0.3));
    CHECK(info["blendMode"] == "opaque");
    CHECK(info["normalFormat"] == "directx");
    CHECK(run("listMaterials") == json::array({"M_Red"}));

    std::string cube = run("polyCube")[0];
    run("assignMaterial", {{"args", {red, cube}}});
    CHECK(slots(cube) == std::map<std::string, int>{{"M_Red", 6}});
    CHECK(run("materialInfo", {{"args", {red}}})["usedBy"][0]["faces"] == 6);
    run("undo");
    CHECK(slots(cube) == std::map<std::string, int>{{"Default", 6}});
    run("redo");
    CHECK(slots(cube) == std::map<std::string, int>{{"M_Red", 6}});

    // Face assignment through the face selection, then back to one slot.
    std::string blue = run("createMaterial", {{"args", {"M_Blue"}}});
    run("selectComponents", {{"args", {cube}}, {"faces", {0, 1}}});
    run("assignMaterial", {{"args", {blue}}});
    CHECK(slots(cube) == std::map<std::string, int>{{"M_Blue", 2}, {"M_Red", 4}});
    run("assignMaterial", {{"args", {red, cube}}, {"faces", {0}}});
    CHECK(slots(cube) == std::map<std::string, int>{{"M_Blue", 1}, {"M_Red", 5}});
    run("assignMaterial", {{"args", {red, cube}}, {"faces", {1}}});
    // The unused slot is dropped.
    CHECK(slots(cube) == std::map<std::string, int>{{"M_Red", 6}});
    json listed = run("listMaterials", {{"args", {cube}}});
    CHECK(listed[0]["slots"].size() == 1);

    run("setMaterialTexture", {{"args", {red}}, {"channel", "normal"}, {"path", "textures\\T_Brick_N.png"}});
    CHECK(run("materialInfo", {{"args", {red}}})["normalMap"] == "textures/T_Brick_N.png");
    CHECK_THROWS_AS(run("setMaterialTexture", {{"args", {red}}, {"channel", "bogus"}, {"path", "x.png"}}), CommandError);
    CHECK_THROWS_AS(run("assignMaterial", {{"args", {"NoSuchMaterial", cube}}}), CommandError);

    std::string renamed = run("renameMaterial", {{"args", {red, "M_Brick"}}});
    CHECK(renamed == "M_Brick");
    CHECK(slots(cube) == std::map<std::string, int>{{"M_Brick", 6}});
    run("undo");
    CHECK(slots(cube) == std::map<std::string, int>{{"M_Red", 6}});
}

TEST_CASE("slots survive modeling ops")
{
    run("newScene");
    std::string red = run("createMaterial", {{"args", {"M_Red"}}});
    std::string cube = run("polyCube")[0];
    run("assignMaterial", {{"args", {red, cube}}, {"faces", {0}}});
    CHECK(slots(cube) == std::map<std::string, int>{{"Default", 5}, {"M_Red", 1}});

    // Extruded walls take the material of the face they grow from.
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("polyExtrude", {{"thickness", 20}});
    auto s = slots(cube);
    CHECK(s["Default"] == 5);
    CHECK(s["M_Red"] == 5);

    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {cube}}, {"edges", {0, 1, 2}}});
    run("polyBevel", {{"offset", 2}});
    s = slots(cube);
    CHECK(s.size() == 2);
    CHECK(s["M_Red"] >= 5);
    run("selectMode", {{"args", {"object"}}});

    // Combine keeps both slots and maps them by name; separate splits them back.
    std::string blue = run("createMaterial", {{"args", {"M_Blue"}}});
    std::string other = run("polyCube", {{"position", {300, 0, 0}}})[0];
    run("assignMaterial", {{"args", {blue, other}}});
    std::map<std::string, int> before = slots(cube);
    std::string both = run("polyCombine", {{"args", {cube, other}}, {"name", "Both"}});
    s = slots(both);
    CHECK(s["M_Blue"] == 6);
    CHECK(s["M_Red"] == before["M_Red"]);
    CHECK(s["Default"] == before["Default"]);

    json parts = run("polySeparate", {{"args", {both}}});
    REQUIRE(parts.size() == 2);
    int blueParts = 0;
    for (const json& p : parts)
    {
        auto ps = slots(p.get<std::string>());
        if (ps.contains("M_Blue") && ps["M_Blue"] == 6)
            ++blueParts;
    }
    CHECK(blueParts == 1);
}

TEST_CASE("materials round trip through scene files")
{
    for (const char* name : {"materials.lila", "materials.lilaa"})
    {
        run("newScene");
        run("createMaterial", {{"args", {"M_Glass"}}, {"blendMode", "translucent"}, {"opacity", 0.25}, {"twoSided", true},
                               {"baseColorMap", "T_Glass_D.png"}, {"emissive", {0.0, 1.0, 0.0}}});
        std::string cube = run("polyCube")[0];
        run("assignMaterial", {{"args", {"M_Glass", cube}}, {"faces", {2, 3}}});
        auto path = tempFile(name);
        io::save(path);
        run("newScene");
        CHECK(run("listMaterials").empty());
        io::load(path);
        json info = run("materialInfo", {{"args", {"M_Glass"}}});
        CHECK(info["blendMode"] == "translucent");
        CHECK(info["opacity"].get<double>() == doctest::Approx(0.25));
        CHECK(info["twoSided"] == true);
        CHECK(info["baseColorMap"] == "T_Glass_D.png");
        CHECK(info["emissiveColor"][1].get<double>() == doctest::Approx(1.0));
        CHECK(slots(cube) == std::map<std::string, int>{{"Default", 4}, {"M_Glass", 2}});
    }
}

TEST_CASE("FBX keeps slot names and creates materials")
{
    run("newScene");
    run("createMaterial", {{"args", {"M_Wood"}}, {"baseColor", {0.4, 0.2, 0.1}}});
    run("createMaterial", {{"args", {"M_Metal"}}, {"metallic", 1.0}});
    std::string cube = run("polyCube")[0];
    run("assignMaterial", {{"args", {"M_Wood", cube}}});
    run("assignMaterial", {{"args", {"M_Metal", cube}}, {"faces", {0, 1, 2}}});
    auto path = tempFile("materials.fbx");
    run("exportFbx", {{"args", {path.string(), cube}}, {"triangulate", false}});

    // Into an empty scene: the slots come back by name and material nodes are made for them.
    run("newScene");
    json imported = run("importFbx", {{"args", {path.string()}}});
    (void)imported;
    std::string mesh;
    for (const json& n : run("ls", {{"type", "mesh"}}))
        mesh = n.get<std::string>();
    REQUIRE(!mesh.empty());
    CHECK(slots(mesh) == std::map<std::string, int>{{"M_Metal", 3}, {"M_Wood", 3}});
    json mats = run("listMaterials");
    CHECK(std::find(mats.begin(), mats.end(), "M_Wood") != mats.end());
    CHECK(std::find(mats.begin(), mats.end(), "M_Metal") != mats.end());

    // Into a scene that has them: existing materials are reused, not duplicated.
    run("setAttr", {{"args", {"M_Wood.roughness", 0.9}}});
    run("importFbx", {{"args", {path.string()}}});
    CHECK(run("listMaterials").size() == 2);
    CHECK(run("materialInfo", {{"args", {"M_Wood"}}})["roughness"].get<double>() == doctest::Approx(0.9));
}

TEST_CASE("roughness default and packed layout survive scene files")
{
    using namespace scene;
    run("newScene");
    run("createMaterial", {{"args", {"M_New"}}, {"ormMap", "T_Mro.png"}, {"packedLayout", "mro"}});
    CHECK(run("materialInfo", {{"args", {"M_New"}}})["roughness"].get<double>() == doctest::Approx(0.4));

    // Files leave defaults out, so a material saved before the default changed must come back with the old one.
    io::deserialize({{"lila", 1}, {"nodes", {{{"name", "M_Old"}, {"type", "material"}}}}});
    CHECK(run("materialInfo", {{"args", {"M_Old"}}})["roughness"].get<double>() == doctest::Approx(0.5));
    CHECK(run("materialInfo", {{"args", {"M_Old"}}})["packedLayout"] == "orm");

    run("createMaterial", {{"args", {"M_New"}}, {"ormMap", "T_Mro.png"}, {"packedLayout", "mro"}});
    for (const char* name : {"defaults.lila", "defaults.lilaa"})
    {
        auto path = tempFile(name);
        io::save(path);
        run("newScene");
        io::load(path);
        json fresh = run("materialInfo", {{"args", {"M_New"}}});
        CHECK(fresh["roughness"].get<double>() == doctest::Approx(0.4));
        CHECK(fresh["packedLayout"] == "mro");
        CHECK(run("materialInfo", {{"args", {"M_Old"}}})["roughness"].get<double>() == doctest::Approx(0.5));
    }

    CHECK(materials::packedChannels(material::PackedLayout::ORM) == std::array<int, 3>{0, 1, 2});
    CHECK(materials::packedChannels(material::PackedLayout::MRO) == std::array<int, 3>{2, 1, 0});
}

TEST_CASE("flip green maps to the OpenGL normal format and survives scene files")
{
    run("newScene");
    run("createMaterial", {{"args", {"M_Gl"}}, {"normalMap", "T_N.png"}, {"flipGreen", true}});
    run("createMaterial", {{"args", {"M_Dx"}}, {"normalMap", "T_N.png"}});
    CHECK(run("materialInfo", {{"args", {"M_Gl"}}})["normalFormat"] == "opengl");
    CHECK(run("materialInfo", {{"args", {"M_Gl"}}})["flipGreen"] == true);
    CHECK(run("materialInfo", {{"args", {"M_Dx"}}})["flipGreen"] == false);

    run("setAttr", {{"args", {"M_Dx.normalFormat", 1}}});
    CHECK(run("materialInfo", {{"args", {"M_Dx"}}})["flipGreen"] == true);
    run("undo");
    CHECK(run("materialInfo", {{"args", {"M_Dx"}}})["flipGreen"] == false);

    for (const char* name : {"flip.lila", "flip.lilaa"})
    {
        auto path = tempFile(name);
        io::save(path);
        run("newScene");
        io::load(path);
        CHECK(run("materialInfo", {{"args", {"M_Gl"}}})["flipGreen"] == true);
        CHECK(run("materialInfo", {{"args", {"M_Dx"}}})["flipGreen"] == false);
    }
}

TEST_CASE("assigning the default slot clears a material")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("assignMaterial", {{"args", {run("createMaterial", {{"args", {"M_A"}}}), cube}}});
    run("select", {{"args", {cube}}});
    run("assignMaterial", {{"args", {scene::materials::kDefaultSlot}}});
    CHECK(slots(cube) == std::map<std::string, int>{{"Default", 6}});
    CHECK(run("materialInfo", {{"args", {"M_A"}}})["usedBy"].empty());
}

TEST_CASE("selectByMaterial selects the objects or faces a material is assigned to")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0], c = run("polyCube")[0];
    run("createMaterial", {{"args", {"M_Blue"}}});
    run("assignMaterial", {{"args", {"M_Blue", a}}});
    run("assignMaterial", {{"args", {"M_Blue", b}}, {"faces", {1, 3}}});
    CHECK(run("selectByMaterial", {{"args", {"M_Blue"}}}) == json::array({a, b}));
    CHECK(run("ls", {{"selection", true}}) == json::array({a, b}));
    run("selectByMaterial", {{"args", {"M_Blue"}}, {"faces", true}});
    namespace comps = scene::components;
    scene::Graph& g = scene::Graph::current();
    CHECK(comps::selected(scene::ComponentMode::Face).size() == 2);
    CHECK(comps::selected(g.find(b)->id(), scene::ComponentMode::Face) == comps::Ids{1, 3});
    CHECK(comps::selected(g.find(c)->id(), scene::ComponentMode::Face).empty());
}

TEST_CASE("shading models and their inputs round trip, undo and key")
{
    run("newScene");
    run("createMaterial", {{"args", {"M_Paint"}}, {"shadingModel", "painterly"}, {"strokeScale", 35.0}, {"strokeMapping", "screen"},
                           {"paletteCount", 4}, {"paletteShadow", {0.1, 0.2, 0.6}}, {"boil", true}, {"boilFps", 8.0}, {"strokeMap", "strokes_dabs"}});
    run("createMaterial", {{"args", {"M_Toon"}}, {"shadingModel", "toon"}, {"toonSoftness", 0.02}, {"rimColor", {1.0, 0.5, 0.0}}});
    run("createMaterial", {{"args", {"M_Leaf"}}, {"shadingModel", "foliage"}, {"foliageWrap", 0.9}});
    run("createMaterial", {{"args", {"M_Plain"}}});
    CHECK(run("materialInfo", {{"args", {"M_Plain"}}})["shadingModel"] == "defaultLit");

    run("setAttr", {{"args", {"M_Toon.toonThreshold", 0.3}}});
    CHECK(run("materialInfo", {{"args", {"M_Toon"}}})["toonThreshold"].get<double>() == doctest::Approx(0.3));
    run("undo");
    CHECK(run("materialInfo", {{"args", {"M_Toon"}}})["toonThreshold"].get<double>() == doctest::Approx(0.0));
    run("setKeyframe", {{"args", {"M_Paint.painterlyStrength"}}});

    for (const char* name : {"styles.lila", "styles.lilaa"})
    {
        auto path = tempFile(name);
        io::save(path);
        run("newScene");
        io::load(path);
        json paint = run("materialInfo", {{"args", {"M_Paint"}}});
        CHECK(paint["shadingModel"] == "painterly");
        CHECK(paint["strokeScale"].get<double>() == doctest::Approx(35.0));
        CHECK(paint["strokeMapping"] == "screen");
        CHECK(paint["paletteCount"] == 4);
        CHECK(paint["paletteShadow"][2].get<double>() == doctest::Approx(0.6));
        CHECK(paint["boil"] == true);
        CHECK(paint["boilFps"].get<double>() == doctest::Approx(8.0));
        CHECK(paint["strokeMap"] == "strokes_dabs");
        json toon = run("materialInfo", {{"args", {"M_Toon"}}});
        CHECK(toon["shadingModel"] == "toon");
        CHECK(toon["toonSoftness"].get<double>() == doctest::Approx(0.02));
        CHECK(toon["rimColor"][1].get<double>() == doctest::Approx(0.5));
        CHECK(run("materialInfo", {{"args", {"M_Leaf"}}})["foliageWrap"].get<double>() == doctest::Approx(0.9));
        CHECK(run("materialInfo", {{"args", {"M_Plain"}}})["shadingModel"] == "defaultLit");
    }

    // Built-in sheets resolve by name, an empty strokeMap is the default sheet.
    CHECK(std::filesystem::exists(scene::materials::strokeSheetPath("")));
    CHECK(std::filesystem::exists(scene::materials::strokeSheetPath("strokes_dabs")));
    CHECK(scene::materials::builtinStrokeSheets().size() >= 3);
}

TEST_CASE("soft foliage normals point a mesh's normals out of a dome, mixed with its own")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    // The lowest normal Y: -1 while the cube's bottom face points straight down.
    auto lowestY = [&] {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        const scene::MeshPtr& m = g.find(cube)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        REQUIRE(m);
        auto stored = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
        std::vector<glm::vec3> n = stored.empty() ? geo::cornerNormals(*m) : std::vector<glm::vec3>(stored.begin(), stored.end());
        float lo = 1.0f;
        for (geo::Index c = 0; c < m->cornerCapacity(); ++c)
            if (m->cornerValid(c))
            {
                CHECK(glm::length(n[c]) == doctest::Approx(1.0).epsilon(1e-3));
                lo = std::min(lo, n[c].y);
            }
        return lo;
    };
    CHECK(lowestY() < -0.99f);
    run("softFoliageNormals", {{"args", {cube}}, {"mix", 1.0}});
    const float full = lowestY();
    CHECK(full > -0.3f);
    run("undo");
    CHECK(lowestY() < -0.99f);
    run("softFoliageNormals", {{"args", {cube}}, {"mix", 0.0}});
    CHECK(lowestY() < -0.99f);
    run("undo");
    run("softFoliageNormals", {{"args", {cube}}});
    const float mixed = lowestY();
    CHECK(mixed < full);
    CHECK(mixed > -0.99f);
}
