#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/Version.h"
#include "io/References.h"
#include "io/SceneFile.h"
#include "io/SceneFormats.h"
#include "scene/Graph.h"

#include <filesystem>
#include <fstream>

using namespace lila;
using json = nlohmann::json;
using Age = io::SceneOrigin::Age;

namespace
{
json doc(int format, const char* version)
{
    json d = {{"lila", format}, {"nodes", json::array()}};
    if (version)
        d["writer"] = {{"version", version}, {"build", "202611010000-abcdef0123"}};
    return d;
}
} // namespace

TEST_CASE("scene version: the writer and format are compared with the running Lila")
{
    const int f = io::formatVersion();
    CHECK(io::sceneOrigin(doc(f, "0.4.2"), "0.4.2").age == Age::Same);
    CHECK(io::sceneOrigin(doc(f, "0.4.10"), "0.4.9").age == Age::Newer);
    CHECK(io::sceneOrigin(doc(f, "0.10.0"), "0.9.3").age == Age::Newer);
    CHECK(io::sceneOrigin(doc(f, "1.0.0"), "0.9.3+abc").age == Age::Newer);
    CHECK(io::sceneOrigin(doc(f, "0.4.1"), "0.4.2").age == Age::Older);
    CHECK(io::sceneOrigin(doc(f, nullptr), "0.4.2").age == Age::Older);
    // A newer format is newer whatever version string the file carries.
    CHECK(io::sceneOrigin(doc(f + 1, "0.1.0"), "0.4.2").age == Age::Newer);
    const io::SceneOrigin o = io::sceneOrigin(doc(f, "0.4.2"), "0.4.2");
    CHECK(o.version == "0.4.2");
    CHECK(o.build == "202611010000-abcdef0123");
    CHECK(o.format == f);
}

TEST_CASE("scene version: a scene from a newer Lila still opens, and saving makes it this Lila's")
{
    const auto dir = std::filesystem::temp_directory_path() / "lila_tests" / "scene_version";
    std::filesystem::create_directories(dir);
    const auto path = dir / "future.lilaa";

    ops::execute("newScene", json::object());
    ops::execute("polyCube", {{"name", "Crate"}});
    json d = io::serialize();
    d["lila"] = io::formatVersion() + 1;
    d["writer"] = {{"version", "99.0.0"}, {"build", "209901010000-0000000000"}};
    d["nodes"].push_back({{"name", "Gadget"}, {"type", "someFutureNodeType"}});
    {
        std::ofstream out(path, std::ios::binary);
        out << io::encodeAscii(d);
    }
    ops::execute("newScene", json::object());

    ops::execute("openScene", {{"args", {path.generic_string()}}, {"force", true}});
    CHECK(scene::Graph::current().find("Crate"));
    CHECK(io::openedOrigin().age == Age::Newer);
    CHECK(io::openedOrigin().version == "99.0.0");

    ops::execute("saveScene", json::object());
    CHECK(io::openedOrigin().age == Age::Same);
    const json saved = io::readSceneFile(path, nullptr);
    CHECK(saved["lila"] == io::formatVersion());
    CHECK(saved["writer"]["version"] == version::number());

    ops::execute("newScene", json::object());
    CHECK(io::openedOrigin().age == Age::Same);
}
