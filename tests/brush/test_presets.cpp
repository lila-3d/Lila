#include "brush/Alphas.h"
#include "brush/Presets.h"
#include "core/Paths.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace lila;
namespace fs = std::filesystem;
namespace presets = lila::brush::presets;

namespace
{
void writeBytes(const fs::path& file, std::string_view bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
}

std::string readBytes(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

struct CleanUserDir
{
    CleanUserDir() { fs::remove_all(presets::userDir()); }
    ~CleanUserDir() { fs::remove_all(presets::userDir()); }
};
} // namespace

TEST_CASE("brush presets: save, find, embedded alpha, install, delete")
{
    CleanUserDir clean;
    const fs::path scratch = fs::temp_directory_path() / "lila_brush_preset_test";
    fs::remove_all(scratch);
    const std::string alphaBytes = std::string("not really a png \0 with a zero", 30);
    writeBytes(scratch / "pores.png", alphaBytes);

    presets::Preset p;
    p.label = "Skin: Pores";
    p.brush = "damStandard";
    p.doc = "Fine pores";
    p.settings.strength = 0.35;
    p.settings.falloff = "sharp";
    p.settings.spacing = 0.07;
    p.settings.alphaPath = paths::toUtf8(scratch / "pores.png");

    std::string error;
    const std::string id = presets::save(p, presets::userDir(), false, &error);
    CHECK(error.empty());
    REQUIRE(id == "user/Skin_ Pores");

    std::optional<presets::Preset> back = presets::find(id);
    REQUIRE(back);
    CHECK(back->label == "Skin: Pores");
    CHECK(back->brush == "damStandard");
    CHECK(back->doc == "Fine pores");
    CHECK(back->source == "user");
    CHECK(back->settings.strength == doctest::Approx(0.35));
    CHECK(back->settings.falloff == "sharp");
    CHECK(back->settings.spacing == doctest::Approx(0.07));
    // The alpha came back from inside the file, not from the original path.
    REQUIRE(!back->settings.alphaPath.empty());
    CHECK(fs::path(back->settings.alphaPath) != scratch / "pores.png");
    CHECK(readBytes(back->settings.alphaPath) == alphaBytes);

    SUBCASE("a second save numbers the name; replace overwrites")
    {
        CHECK(presets::save(p, presets::userDir()) == "user/Skin_ Pores 2");
        p.settings.strength = 0.9;
        CHECK(presets::save(p, presets::userDir(), true) == id);
        CHECK(presets::find(id)->settings.strength == doctest::Approx(0.9));
    }

    SUBCASE("exported files install, with the alpha, after the original is gone")
    {
        const fs::path shared = scratch / "shared" / "Pores.lilabrush";
        REQUIRE(presets::write(*back, shared, &error));
        fs::remove(scratch / "pores.png");
        const std::string installed = presets::install(shared, presets::userDir() / "skin", &error);
        CHECK(installed == "user/skin/Pores");
        std::optional<presets::Preset> got = presets::find(installed);
        REQUIRE(got);
        CHECK(got->category == "skin");
        CHECK(readBytes(got->settings.alphaPath) == alphaBytes);
    }

    SUBCASE("library lists them by tool, deletes only your own")
    {
        CHECK(presets::library("sculpt").size() == 1);
        CHECK(presets::library("vertexPaint").empty());
        CHECK(presets::remove(id, &error));
        CHECK(!presets::find(id));
        CHECK(!presets::remove("lila/nothing", &error));
    }

    SUBCASE("files that aren't presets are skipped")
    {
        writeBytes(presets::userDir() / "broken.lilabrush", "{\"format\": \"other\"}");
        CHECK(!presets::load(presets::userDir() / "broken.lilabrush"));
        CHECK(presets::library().size() == 1);
    }

    fs::remove_all(scratch);
}

TEST_CASE("brush presets keep a library alpha as a reference")
{
    CleanUserDir clean;
    const fs::path alphaFile = brush::alphas::userDir() / "preset_ref_test.png";
    writeBytes(alphaFile, "png bytes");
    presets::Preset p;
    p.label = "Ref";
    p.brush = "draw";
    p.settings.alphaPath = "alpha://user/preset_ref_test.png";
    const std::string id = presets::save(p, presets::userDir());
    REQUIRE(!id.empty());
    CHECK(presets::find(id)->settings.alphaPath == "alpha://user/preset_ref_test.png");

    // With the library alpha gone the embedded copy takes over.
    fs::remove(alphaFile);
    const std::string fallback = presets::find(id)->settings.alphaPath;
    CHECK(!fallback.starts_with("alpha://"));
    CHECK(readBytes(fallback) == "png bytes");
}
