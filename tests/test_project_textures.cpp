#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/PrefStore.h"
#include "core/ProjectTextures.h"
#include "core/Workspace.h"
#include "scene/SceneData.h"
#include "scene/TextureFolders.h"

#include <fstream>

using namespace lila;
namespace fs = std::filesystem;
namespace pt = projectTextures;
namespace tf = scene::textureFolders;

namespace
{
/** <temp>/<name> with a handful of images and things that must not be listed. */
struct Project
{
    fs::path root;

    explicit Project(const char* name)
    {
        root = fs::weakly_canonical(fs::temp_directory_path() / name);
        std::error_code ec;
        fs::remove_all(root, ec);
        for (const char* f : {"Textures/Rock_BaseColor.png", "Textures/Rock_Normal.TGA", "Textures/old/Rock_old.png",
                              "Textures/Wood_old.jpg", "Saved/Bakes/Ship_Normal.exr", "hero.psd", "notes.txt",
                              ".lila/thumb.png", ".git/x.png", "Intermediate/cache.png", "Sub/.hidden.png"})
        {
            fs::path p = root / f;
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "x";
        }
    }
    ~Project()
    {
        Workspace::get().close();
        pt::Index::get().setRoot({});
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

json exec(const char* name, const json& call = json::object())
{
    return ops::execute(name, call);
}
} // namespace

TEST_CASE("project textures: globs")
{
    CHECK(pt::globMatch("Textures/**", "Textures/a.png"));
    CHECK(pt::globMatch("Textures/**", "Textures/old/a.png"));
    CHECK(pt::globMatch("textures/**", "Textures/a.png"));
    CHECK_FALSE(pt::globMatch("Textures/**", "Other/Textures.png"));
    CHECK(pt::globMatch("Textures", "Textures/old/a.png"));
    CHECK(pt::globMatch("Textures/", "Textures/a.png"));
    CHECK(pt::globMatch("**/*_old.*", "Textures/Wood_old.jpg"));
    CHECK(pt::globMatch("**/*_old.*", "Wood_old.jpg"));
    CHECK(pt::globMatch("*_old.*", "a/b/Wood_old.jpg"));
    CHECK_FALSE(pt::globMatch("*_old.*", "a/b/Wood_new.jpg"));
    CHECK(pt::globMatch("Saved", "Saved/Bakes/x.exr"));
    CHECK(pt::globMatch("old", "Textures/old/a.png"));
    CHECK_FALSE(pt::globMatch("Textures/*.png", "Textures/old/a.png"));
    CHECK(pt::globMatch("Textures/*.png", "Textures/a.png"));
    CHECK(pt::globMatch("Textures/?.png", "Textures/a.png"));
    CHECK_FALSE(pt::globMatch("Textures/?.png", "Textures/ab.png"));
    CHECK(pt::globMatch("**/Bakes/**", "Saved/Bakes/x.exr"));
    CHECK(pt::globMatch("/Saved/**", "Saved/x.exr"));
    CHECK_FALSE(pt::globMatch("", "a.png"));
}

TEST_CASE("project textures: filter")
{
    pt::Filter f;
    std::vector<std::string> all = {"Textures/a.png", "Textures/a_old.png", "Saved/b.png", "c.png"};
    CHECK(f.apply(all) == all);
    f.include = {"Textures/**"};
    CHECK(f.apply(all) == std::vector<std::string>{"Textures/a.png", "Textures/a_old.png"});
    f.exclude = {"*_old.*"};
    CHECK(f.apply(all) == std::vector<std::string>{"Textures/a.png"});
    f.include.clear();
    f.exclude = {"Saved/**", "*_old.*"};
    CHECK(f.apply(all) == std::vector<std::string>{"Textures/a.png", "c.png"});
}

TEST_CASE("project textures: scan")
{
    Project p("lila_project_textures_scan");
    CHECK(pt::isImageExtension(".PNG"));
    CHECK(pt::isImageExtension("tif"));
    CHECK_FALSE(pt::isImageExtension(".txt"));
    std::vector<std::string> files = pt::scan(p.root);
    CHECK(files == std::vector<std::string>{"hero.psd", "Saved/Bakes/Ship_Normal.exr", "Textures/old/Rock_old.png",
                                            "Textures/Rock_BaseColor.png", "Textures/Rock_Normal.TGA", "Textures/Wood_old.jpg"});

    bool truncated = false;
    pt::ScanLimits tight;
    tight.maxEntries = 3;
    CHECK(pt::scan(p.root, tight, nullptr, &truncated).size() <= 3);
    CHECK(truncated);

    pt::Index::get().setRoot(p.root);
    pt::Index::Snapshot snap = pt::Index::get().snapshot(true);
    CHECK(snap.files == files);
    CHECK_FALSE(snap.scanning);
}

TEST_CASE("project textures: scene rules override workspace rules list by list")
{
    json ws = {{"include", {"Textures/**"}}, {"exclude", {"Saved/**"}}};
    tf::Rules r = tf::resolve(ws, json::object());
    CHECK(r.filter.include == std::vector<std::string>{"Textures/**"});
    CHECK(r.includeFrom == "workspace");
    CHECK(r.excludeFrom == "workspace");

    r = tf::resolve(ws, {{"exclude", {"*_old.*"}}});
    CHECK(r.filter.include == std::vector<std::string>{"Textures/**"});
    CHECK(r.includeFrom == "workspace");
    CHECK(r.filter.exclude == std::vector<std::string>{"*_old.*"});
    CHECK(r.excludeFrom == "scene");

    // An empty scene list is an override too: it clears the workspace's.
    r = tf::resolve(ws, {{"include", json::array()}});
    CHECK(r.filter.include.empty());
    CHECK(r.includeFrom == "scene");

    r = tf::resolve(nullptr, {{"include", "not a list"}});
    CHECK(r.filter.include.empty());
    CHECK(r.includeFrom.empty());
}

TEST_CASE("project textures: commands")
{
    Project p("lila_project_textures_cmds");
    Workspace::get().open(p.root);
    scene::data::clear();
    exec("textureFolders", {{"clear", true}});

    json all = exec("projectTextures");
    CHECK(all.size() == 6);

    exec("textureFolders", {{"include", {"Textures/**", " "}}, {"exclude", {"*_old.*"}}});
    CHECK(PrefStore::get().value(tf::kPrefFile, tf::kIncludeKey) == json{"Textures/**"});
    CHECK(exec("projectTextures") == json{"Textures/Rock_BaseColor.png", "Textures/Rock_Normal.TGA"});
    CHECK(exec("projectTextures", {{"match", "normal"}}) == json{"Textures/Rock_Normal.TGA"});
    CHECK(exec("projectTextures", {{"all", true}}).size() == 6);

    json q = exec("textureFolders", {{"scene", true}, {"include", {"Saved"}}});
    CHECK(q["includeFrom"] == "scene");
    CHECK(q["excludeFrom"] == "workspace");
    CHECK(exec("projectTextures") == json{"Saved/Bakes/Ship_Normal.exr"});
    CHECK(scene::data::get(tf::kSceneKey) == json{{"include", {"Saved"}}});

    exec("textureFolders", {{"scene", true}, {"clear", true}});
    CHECK(scene::data::get(tf::kSceneKey).is_null());
    CHECK(exec("projectTextures").size() == 2);

    json abs = exec("projectTextures", {{"absolute", true}});
    CHECK(fs::exists(fs::path(abs[0].get<std::string>())));

    exec("textureFolders", {{"clear", true}});
    CHECK(exec("projectTextures").size() == 6);

    // Files appearing later show up after a refresh (the watcher does this on its own in the app).
    std::ofstream(p.root / "Textures" / "New.png") << "x";
    CHECK(exec("projectTextures", {{"refresh", true}}).size() == 7);
}
