#include <doctest/doctest.h>

#include "core/Paths.h"
#include "io/Samples.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace lila;

TEST_CASE("samples: listed from sample.json, installed once the scene exists")
{
    const fs::path root = fs::temp_directory_path() / "lila_samples_test";
    fs::remove_all(root);
    fs::create_directories(root / "Good");
    fs::create_directories(root / "NoScene");
    fs::create_directories(root / "Empty");
    std::ofstream(root / "Good" / "sample.json")
        << R"({"name": "Good Scene", "url": "https://example.com/g.zip", "size": 2500000000, "license": "CC BY 4.0",
              "credit": "By someone", "scene": "good.lila", "camera": "Cam"})";
    std::ofstream(root / "NoScene" / "sample.json") << R"({"name": "No Scene", "url": "https://example.com/n.zip"})";
    fs::create_directories(root / "Local");
    std::ofstream(root / "Local" / "sample.json") << R"({"name": "Local", "scene": "local.lila", "featured": true})";
    std::ofstream(root / "Local" / "local.lila") << "x";

    const std::vector<io::samples::Sample> all = io::samples::list(root);
    REQUIRE(all.size() == 2);
    // Featured first, whatever its folder name.
    CHECK(all[0].id == "Local");
    CHECK(io::samples::bundled(all[0]));
    CHECK(io::samples::installed(all[0]));
    const io::samples::Sample& s = all[1];
    CHECK(s.id == "Good");
    CHECK(s.name == "Good Scene");
    CHECK(s.size == 2500000000ull);
    CHECK(s.camera == "Cam");
    CHECK(io::samples::find(all, "Good Scene") == &s);
    CHECK(io::samples::find(all, "Good") == &s);
    CHECK(io::samples::formatSize(s.size) == "2.5 GB");
    CHECK(io::samples::formatSize(894377473) == "894 MB");

    CHECK(io::samples::installDir(s).parent_path() == paths::userConfigDir() / "samples");
    CHECK_FALSE(io::samples::installed(s));
    fs::create_directories(io::samples::installDir(s));
    std::ofstream(io::samples::scenePath(s)) << "x";
    CHECK(io::samples::installed(s));
    fs::remove_all(io::samples::installDir(s));
    fs::remove_all(root);
}

TEST_CASE("samples: bundled ones ship their scene; downloads have a license, a credit and a setup script")
{
    for (const io::samples::Sample& s : io::samples::list())
    {
        INFO(s.id);
        if (io::samples::bundled(s))
        {
            CHECK(io::samples::installed(s));
            CHECK(io::samples::scenePath(s).parent_path() == s.folder);
            continue;
        }
        CHECK_FALSE(s.license.empty());
        CHECK_FALSE(s.credit.empty());
        CHECK(s.url.starts_with("https://"));
        CHECK(fs::is_regular_file(s.folder / "setup.py"));
    }
}
