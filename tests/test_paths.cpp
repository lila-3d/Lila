#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/Paths.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"

#include <fstream>

using namespace lila;
namespace fs = std::filesystem;

namespace
{
/** <temp>/lila_paths/{Lila (workspace), Ref, scenes}, with Ref/fighter.png on disk. */
struct Tree
{
    fs::path top, ws, ref, scenes, image;

    Tree()
    {
        top = fs::weakly_canonical(fs::temp_directory_path() / "lila_paths");
        fs::remove_all(top);
        ws = top / "Lila";
        ref = top / "Ref";
        scenes = ws / "scenes";
        image = ref / "fighter.png";
        fs::create_directories(ws / ".lila");
        fs::create_directories(ref);
        fs::create_directories(scenes);
        std::ofstream(image) << "png";
    }
    ~Tree()
    {
        Workspace::get().close();
        std::error_code ec;
        fs::remove_all(top, ec);
    }
};

bool same(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}
} // namespace

TEST_CASE("paths: relativeTo")
{
#ifdef _WIN32
    CHECK(paths::relativeTo("X:/Voidtide/Lila/tex/a.png", "X:/Voidtide/Lila") == fs::path("tex/a.png"));
    CHECK(paths::toUtf8(paths::relativeTo("X:/Voidtide/Ref/fighter_02_top.png", "X:/Voidtide/Lila")) == "../Ref/fighter_02_top.png");
    CHECK(paths::toUtf8(paths::relativeTo("x:\\VOIDTIDE\\Lila\\a.png", "X:/Voidtide/Lila/")) == "a.png");
    CHECK(paths::relativeTo("C:/Ref/a.png", "X:/Voidtide/Lila").empty());
    CHECK(paths::relativeTo("tex/a.png", "X:/Voidtide/Lila").empty());
    CHECK(paths::relativeTo("X:/Voidtide/Lila", "X:/Voidtide/Lila") == fs::path("."));
#else
    CHECK(paths::toUtf8(paths::relativeTo("/v/Ref/a.png", "/v/Lila")) == "../Ref/a.png");
#endif
}

TEST_CASE("paths: stored and resolved")
{
    Tree t;

    SUBCASE("empty and URLs pass through")
    {
        CHECK(paths::storedPath(fs::path()) == "");
        CHECK(paths::storedPath("https://example.com/a.png") == "https://example.com/a.png");
        CHECK(paths::resolvePath("https://example.com/a.png") == fs::path("https://example.com/a.png"));
    }

    SUBCASE("workspace: inside the root")
    {
        Workspace::get().open(t.ws);
        fs::path tex = Workspace::get().root() / "tex" / "a.png";
        CHECK(paths::storedPath(tex, t.scenes) == "tex/a.png");
        CHECK(paths::resolvePath("tex/a.png", t.scenes) == tex.lexically_normal());
    }

    SUBCASE("workspace: a sibling of the root via ..")
    {
        Workspace::get().open(t.ws);
        std::string stored = paths::storedPath(t.image, t.scenes);
        CHECK(stored == "../Ref/fighter.png");
        CHECK(same(paths::resolvePath(stored, t.scenes), t.image));
    }

    SUBCASE("no workspace: relative to the scene folder")
    {
        std::string stored = paths::storedPath(t.image, t.scenes);
        CHECK(stored == "../../Ref/fighter.png");
        CHECK(same(paths::resolvePath(stored, t.scenes), t.image));
    }

    SUBCASE("no base: absolute")
    {
        CHECK(paths::storedPath(t.image, {}) == paths::toUtf8(t.image));
    }

#ifdef _WIN32
    SUBCASE("another drive stays absolute")
    {
        Workspace::get().open(t.ws);
        std::string root = paths::toUtf8(Workspace::get().root().root_name());
        std::string other = std::string(1, root[0] == 'Q' || root[0] == 'q' ? 'R' : 'Q') + ":/Ref/a.png";
        CHECK(paths::storedPath(paths::fromUtf8(other), t.scenes) == other);
        CHECK(paths::resolvePath(other, t.scenes) == fs::path(other));
    }
#endif

    SUBCASE("relative paths: workspace root first, then the scene folder")
    {
        Workspace::get().open(t.ws);
        std::ofstream(t.scenes / "local.png") << "png";
        CHECK(same(paths::resolvePath("local.png", t.scenes), t.scenes / "local.png"));
        std::ofstream(t.ws / "local.png") << "png";
        CHECK(same(paths::resolvePath("local.png", t.scenes), t.ws / "local.png"));
    }

    SUBCASE("round trip, stored again for another scene folder")
    {
        fs::path old = paths::currentSceneDir();
        paths::setCurrentSceneDir(t.scenes);
        std::string stored = paths::storedPath(t.image);
        CHECK(stored == "../../Ref/fighter.png");
        CHECK(paths::storedPath(paths::fromUtf8(stored)) == stored);
        CHECK(paths::storedPath(paths::fromUtf8(stored), t.ws) == "../Ref/fighter.png");
        paths::setCurrentSceneDir(old);
    }
}

TEST_CASE("paths: saving makes absolute file attrs relative")
{
    Tree t;
    ops::execute("newScene", json::object());
    Workspace::get().open(t.ws);
    std::string name = ops::execute("imagePlane", {{"fileName", "x.png"}}).get<std::string>();
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    // As an older scene stored it, absolute.
    g.setValue({n->id(), scene::ImagePlane::kImageName, -1}, paths::toUtf8(t.image));
    json doc = io::serialize(t.scenes);
    std::string stored;
    for (const json& node : doc["nodes"])
        if (node["name"] == name)
            stored = node["attrs"]["imageName"].get<std::string>();
    CHECK(stored == "../Ref/fighter.png");
    ops::execute("newScene", json::object());
}
