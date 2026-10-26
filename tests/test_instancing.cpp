#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/HudStats.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
namespace inst = scene::instancing;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
scene::Graph& g() { return scene::Graph::current(); }
scene::NodeId id(const std::string& name) { return g().find(name)->id(); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }
std::string kind(const std::string& m) { return run("instanceInfo", {{"args", {m}}})["kind"].get<std::string>(); }

scene::MeshPtr outMesh(const std::string& m)
{
    g().evaluate();
    return g().find(m)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** A point of the mesh as components are edited on it: the cage under the modifiers. */
double pointY(const std::string& m, int p) { return run("meshData", {{"args", {m}}, {"base", true}})["points"][p][1].get<double>(); }

std::string tempPath(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_tests" / "instancing";
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}
} // namespace

TEST_CASE("an instance shares its source's mesh and component edits on any member reach all of them")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Bolt"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    json made = run("instance", {{"args", {cube}}});
    REQUIRE(made.size() == 1);
    std::string a = made[0];
    std::string b = run("instance", {{"args", {a}}})[0];

    CHECK(kind(cube) == "source");
    CHECK(kind(a) == "instance");
    CHECK(kind(b) == "instance");
    // An instance of an instance hangs off the root.
    CHECK(inst::instanceParent(g(), id(b)) == id(cube));
    CHECK(run("instanceInfo", {{"args", {b}}})["source"] == cube);
    CHECK(run("instanceInfo", {{"args", {a}}})["members"].size() == 3);
    CHECK(outMesh(a) == outMesh(cube));
    CHECK(outMesh(b) == outMesh(cube));
    CHECK(faces(a) == 24);

    // Edit points on an instance: the shared base changes, so every member moves.
    run("selectComponents", {{"args", {b}}, {"vertices", {0}}});
    const double before = pointY(cube, 0);
    run("moveComponents", {{"by", {0, 10, 0}}});
    CHECK(pointY(cube, 0) == doctest::Approx(before + 10).epsilon(1e-6));
    CHECK(pointY(a, 0) == doctest::Approx(before + 10).epsilon(1e-6));
    CHECK(kind(b) == "instance");
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 1);

    // Topology edits too.
    run("selectComponents", {{"args", {a}}, {"faces", {0}}});
    run("polyExtrude", {{"thickness", 5}});
    CHECK(faces(cube) == faces(a));
    CHECK(faces(b) == faces(cube));
    CHECK(faces(cube) > 24);

    run("undo");
    CHECK(faces(cube) == 24);
    CHECK(faces(b) == 24);

    // A modifier added on an instance goes on the shared stack.
    run("addModifier", {{"args", {a}}, {"kind", "triangulate"}});
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);
    CHECK(outMesh(a) == outMesh(cube));

    // Selection helpers.
    CHECK(run("selectInstances", {{"args", {a}}}).size() == 3);
    CHECK(run("selectInstanceSource", {{"args", {b}}}) == json::array({cube}));
}

TEST_CASE("a reference shares the base but keeps a modifier stack of its own")
{
    run("newScene");
    std::string panel = run("polyCube", {{"name", "Panel"}})[0];
    run("addModifier", {{"args", {panel}}, {"kind", "bevel"}, {"params", {{"segments", 1}}}});
    std::string ref = run("instance", {{"args", {panel}}, {"reference", true}})[0];
    CHECK(kind(panel) == "source");
    CHECK(kind(ref) == "reference");
    CHECK(run("instanceInfo", {{"args", {ref}}})["source"] == panel);
    // It starts with copies of the source's modifiers, its own.
    json mods = run("modifiers", {{"args", {ref}}});
    REQUIRE(mods.size() == 1);
    CHECK(mods[0] != run("modifiers", {{"args", {panel}}})[0]);
    CHECK(faces(ref) == faces(panel));

    // Its own stack changes without touching the source...
    run("setAttr", {{"args", {mods[0].get<std::string>() + ".segments", 3}}});
    CHECK(faces(ref) > faces(panel));
    // ...but a base edit through either reaches both.
    const int refFaces = faces(ref), panelFaces = faces(panel);
    run("selectComponents", {{"args", {ref}}, {"faces", {0}}});
    run("polyExtrude", {{"thickness", 5}});
    CHECK(faces(ref) > refFaces);
    CHECK(faces(panel) > panelFaces);
    CHECK(kind(ref) == "reference");

    // A reference of a generator-made mesh shares the generator until an edit freezes it for both.
    run("newScene");
    std::string box = run("polyCube", {{"name", "Box"}})[0];
    std::string box2 = run("duplicate", {{"args", {box}}, {"mode", "reference"}})[0];
    CHECK(kind(box2) == "reference");
    run("selectComponents", {{"args", {box2}}, {"vertices", {1}}});
    run("moveComponents", {{"by", {0, 3, 0}}});
    CHECK(pointY(box, 1) == doctest::Approx(pointY(box2, 1)));
    CHECK(kind(box2) == "reference");
}

TEST_CASE("makeUnique breaks sharing for instances, references and sources")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Src"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    std::string a = run("instance", {{"args", {cube}}})[0];
    std::string b = run("instance", {{"args", {cube}}})[0];

    CHECK(run("makeUnique", {{"args", {a}}}) == json::array({a}));
    CHECK(kind(a) == "none");
    // It keeps a live copy of the source's stack.
    CHECK(run("modifiers", {{"args", {a}}}).size() == 1);
    CHECK(faces(a) == faces(cube));
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("polyExtrude", {{"thickness", 5}});
    CHECK(faces(a) != faces(cube));
    CHECK(faces(b) == faces(cube));
    for (int i = 0; i < 3; ++i) // extrude, the face selection, makeUnique
        run("undo");
    CHECK(kind(a) == "instance");

    // The source going unique hands its data to its instance.
    run("makeUnique", {{"args", {cube}}});
    CHECK(kind(cube) == "none");
    CHECK(kind(a) == "source");
    CHECK(kind(b) == "instance");
    CHECK(run("instanceInfo", {{"args", {b}}})["source"] == a);
    CHECK(faces(cube) == faces(a));

    // A reference takes its own base.
    run("newScene");
    std::string p = run("polyCube", {{"name", "P"}})[0];
    std::string r = run("instance", {{"args", {p}}, {"reference", true}})[0];
    run("makeUnique", {{"args", {r}}});
    CHECK(kind(r) == "none");
    CHECK(kind(p) == "none");
    run("selectComponents", {{"args", {p}}, {"vertices", {0}}});
    run("moveComponents", {{"by", {0, 5, 0}}});
    CHECK(pointY(p, 0) != doctest::Approx(pointY(r, 0)));
}

TEST_CASE("deleting a source promotes its first instance, and undo puts it all back")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Src"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    std::string a = run("instance", {{"args", {cube}}})[0];
    std::string b = run("instance", {{"args", {cube}}})[0];
    const int n = faces(cube);

    run("delete", {{"args", {cube}}});
    CHECK(faces(a) == n);
    CHECK(faces(b) == n);
    CHECK(kind(a) == "source");
    CHECK(kind(b) == "instance");
    CHECK(run("instanceInfo", {{"args", {b}}})["source"] == a);
    // The promoted instance owns the stack now.
    CHECK(run("modifiers", {{"args", {a}}}).size() == 1);

    run("undo");
    CHECK(kind(cube) == "source");
    CHECK(kind(a) == "instance");
    CHECK(run("instanceInfo", {{"args", {b}}})["source"] == cube);

    // Deleting a group holding the source and one instance promotes the instance outside it.
    run("group", {{"args", {cube, a}}, {"name", "Grp"}});
    run("delete", {{"args", {"Grp"}}});
    CHECK(faces(b) == n);
    CHECK(kind(b) == "none");

    // A source with a stored base hands that over too.
    run("newScene");
    std::string plain = run("polyCube", {{"name", "Plain"}})[0];
    run("bakeModifierStack", {{"args", {plain}}});
    std::string c = run("instance", {{"args", {plain}}})[0];
    run("delete", {{"args", {plain}}});
    CHECK(faces(c) == 6);
    CHECK(kind(c) == "none");
}

TEST_CASE("convertToInstance replaces identical copies and leaves different meshes alone")
{
    run("newScene");
    std::string a = run("polyCube", {{"name", "Kit"}})[0];
    run("bakeModifierStack", {{"args", {a}}});
    std::string b = run("duplicate", {{"args", {a}}})[0];
    std::string c = run("duplicate", {{"args", {a}}})[0];
    run("move", {{"args", {b}}, {"by", {100, 0, 0}}});
    std::string other = run("polySphere", {{"name", "Ball"}})[0];
    run("selectComponents", {{"args", {c}}, {"vertices", {0}}});
    run("moveComponents", {{"by", {0, 1, 0}}});
    run("select", {{"args", {a}}});

    json converted = run("convertToInstance", {{"args", {a}}});
    CHECK(converted == json::array({b}));
    CHECK(kind(b) == "instance");
    CHECK(kind(c) == "none");
    CHECK(kind(other) == "none");
    // The instance keeps its own transform.
    CHECK(run("getAttr", {{"args", {b + ".tx"}}}).get<double>() == doctest::Approx(100));

    // force converts a different mesh when named.
    CHECK(run("convertToInstance", {{"args", {c, a}}, {"force", true}}) == json::array({c}));
    CHECK(kind(c) == "instance");
    run("undo");
    CHECK(kind(c) == "none");
}

TEST_CASE("instances survive save and open in both formats and through a file reference")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Src"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    std::string a = run("instance", {{"args", {cube}}, {"name", "Inst"}})[0];
    std::string r = run("instance", {{"args", {cube}}, {"reference", true}, {"name", "Ref"}})[0];
    run("move", {{"args", {a}}, {"by", {50, 0, 0}}});
    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = tempPath((std::string("inst") + ext).c_str());
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(kind("Src") == "source");
        CHECK(kind("Inst") == "instance");
        CHECK(kind("Ref") == "reference");
        CHECK(outMesh("Inst") == outMesh("Src"));
        CHECK(run("getAttr", {{"args", {"Inst.tx"}}}).get<double>() == doctest::Approx(50));
    }

    // A local instance of a referenced mesh keeps pointing at it after a reopen.
    run("newScene");
    run("polyCube", {{"name", "Asset"}});
    std::string asset = tempPath("asset.lilaa");
    run("saveScene", {{"args", {asset}}});
    run("newScene");
    run("createReference", {{"args", {asset}}, {"namespace", "kit"}});
    std::string local = run("instance", {{"args", {"kit:Asset"}}, {"name", "Local"}})[0];
    CHECK(kind(local) == "instance");
    std::string shot = tempPath("shot.lila");
    run("saveScene", {{"args", {shot}}});
    run("newScene");
    run("openScene", {{"args", {shot}}});
    CHECK(kind("Local") == "instance");
    CHECK(run("instanceInfo", {{"args", {"Local"}}})["source"] == "kit:Asset");
    CHECK(faces("Local") == 6);
}

TEST_CASE("the HUD counts an instanced mesh's triangles once as unique")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("instance", {{"args", {cube}}});
    run("instance", {{"args", {cube}}});
    run("select", {{"args", json::array()}});
    g().evaluate();
    scene::hud::PolyStats s = scene::hud::polyStats(g());
    CHECK(s.scene.tris == 36);
    CHECK(s.uniqueTris == 12);
}

TEST_CASE("freezeTransforms on an instance makes it unique instead of moving every member")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Src"}})[0];
    std::string a = run("instance", {{"args", {cube}}})[0];
    run("move", {{"args", {a}}, {"by", {0, 100, 0}}});
    const double y = pointY(cube, 0);
    run("freezeTransforms", {{"args", {a}}});
    CHECK(pointY(cube, 0) == doctest::Approx(y));
    CHECK(pointY(a, 0) == doctest::Approx(y + 100));
    CHECK(kind(a) == "none");
}
