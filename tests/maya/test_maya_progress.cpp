#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/Progress.h"
#include "io/SceneFile.h"

#include <algorithm>
#include <filesystem>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

std::vector<fs::path> fixtures()
{
    std::vector<fs::path> out;
    for (const auto& e : fs::recursive_directory_iterator(LILA_MAYA_TEST_DATA))
        if (e.path().extension() == ".ma" || e.path().extension() == ".mb")
            out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

std::string utf8(const fs::path& p)
{
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
} // namespace

TEST_CASE("maya progress: converting on workers builds the same scene as inline")
{
    const std::vector<fs::path> files = fixtures();
    REQUIRE(files.size() > 10);
    for (const fs::path& f : files)
    {
        CAPTURE(f.string());
        // Referenced files come in as plain nodes, so nothing is written next to the fixtures.
        const json call = {{"args", {utf8(f)}}, {"references", "import"}, {"force", true}};
        run("mayaOpen", call);
        const json inline_ = io::serialize();

        int pumped = 0;
        progress::setPump([&] { ++pumped; }, 1);
        run("mayaOpen", call);
        progress::setPump(nullptr);
        const json worker = io::serialize();
        CHECK(pumped > 0);
        CHECK(inline_ == worker);
    }
}

TEST_CASE("maya progress: cancelling while the file is parsed leaves the scene alone")
{
    run("newScene");
    run("spaceLocator", {{"name", "Keep"}});
    const json before = io::serialize();
    CHECK(progress::current() == nullptr);
    CHECK(progress::snapshot().empty());

    progress::setPump([] {
        for (const progress::Snapshot& s : progress::snapshot())
            if (s.cancellable)
                progress::cancel(s.id);
    }, 1);
    std::string error;
    try
    {
        run("mayaOpen", {{"args", {utf8(fs::path(LILA_MAYA_TEST_DATA) / "generated" / "rich.mb")}}, {"force", true}});
    }
    catch (const CommandError& e)
    {
        error = e.what();
    }
    progress::setPump(nullptr);
    CHECK(error == "mayaOpen: cancelled");
    CHECK(io::serialize()["nodes"] == before["nodes"]);
}
