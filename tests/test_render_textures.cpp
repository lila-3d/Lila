#include <doctest/doctest.h>

#include "core/ImageIO.h"
#include "render/Compute.h"
#include "render/GpuImage.h"
#include "render/RenderMesh.h"
#include "render/TextureFormat.h"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace lila;
using render::TextureFormat;

TEST_CASE("Texture formats: sizes, names and Vulkan formats")
{
    using enum TextureFormat;
    CHECK(render::texelBytes(R8) == 1);
    CHECK(render::texelBytes(RG8) == 2);
    CHECK(render::texelBytes(RGBA8) == 4);
    CHECK(render::texelBytes(R16F) == 2);
    CHECK(render::texelBytes(RGBA16F) == 8);
    CHECK(render::texelBytes(R32F) == 4);
    CHECK(render::channelCount(RG8) == 2);
    CHECK(render::isFloat(R16F));
    CHECK_FALSE(render::isFloat(RGBA8));

    for (TextureFormat f : {R8, RG8, RGBA8, R16F, RGBA16F, R32F})
    {
        TextureFormat back = RGBA8;
        CHECK(render::parseFormat(render::formatName(f), back));
        CHECK(back == f);
    }
    TextureFormat unchanged = R8;
    CHECK_FALSE(render::parseFormat("bc7", unchanged));
    CHECK(unchanged == R8);

    // sRGB only ever applies to RGBA8 colour.
    CHECK(render::vkFormat(RGBA8, true) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(render::vkFormat(RGBA8, false) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(render::vkFormat(R8, true) == VK_FORMAT_R8_UNORM);
    CHECK(render::vkFormat(RGBA16F, true) == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(render::vkFormat(R32F, false) == VK_FORMAT_R32_SFLOAT);
    CHECK(render::sampleSwizzle(R16F).g == VK_COMPONENT_SWIZZLE_R);
    CHECK(render::sampleSwizzle(RG8).a == VK_COMPONENT_SWIZZLE_G);
    CHECK(render::sampleSwizzle(RGBA8).r == VK_COMPONENT_SWIZZLE_IDENTITY);

    // Existing callers build RGBA8 sRGB textures without naming a format.
    render::TextureImage t;
    CHECK(t.format == RGBA8);
    CHECK(t.srgb);
    CHECK_FALSE(t.isUdimSet());
}

TEST_CASE("Texture formats: encode and decode texels")
{
    const std::vector<float> rgba = {0.0f, 0.5f, 1.0f, 0.25f, 2.0f, -1.0f, 0.2f, 1.0f};
    std::vector<uint8_t> bytes;
    std::vector<float> back;

    render::encodeTexels(TextureFormat::RGBA8, false, rgba, bytes);
    CHECK(bytes == std::vector<uint8_t>{0, 128, 255, 64, 255, 0, 51, 255});

    render::encodeTexels(TextureFormat::R8, false, rgba, bytes);
    CHECK(bytes == std::vector<uint8_t>{0, 255});
    render::decodeTexels(TextureFormat::R8, false, bytes, back);
    CHECK(back == std::vector<float>{0, 0, 0, 1, 1, 0, 0, 1});

    render::encodeTexels(TextureFormat::RG8, false, rgba, bytes);
    CHECK(bytes == std::vector<uint8_t>{0, 128, 255, 0});

    // Float formats keep values outside 0..1.
    render::encodeTexels(TextureFormat::RGBA16F, false, rgba, bytes);
    CHECK(bytes.size() == 16);
    render::decodeTexels(TextureFormat::RGBA16F, false, bytes, back);
    for (size_t i = 0; i < rgba.size(); ++i)
        CHECK(back[i] == doctest::Approx(rgba[i]).epsilon(1e-3));

    render::encodeTexels(TextureFormat::R32F, false, rgba, bytes);
    render::decodeTexels(TextureFormat::R32F, false, bytes, back);
    CHECK(back[0] == 0.0f);
    CHECK(back[4] == 2.0f);

    // sRGB RGBA8 stores encoded colour and leaves alpha linear.
    render::encodeTexels(TextureFormat::RGBA8, true, std::vector<float>{0.214f, 0.0f, 1.0f, 0.5f}, bytes);
    CHECK(int(bytes[0]) == doctest::Approx(128).epsilon(0.01));
    CHECK(bytes[3] == 128);
    render::decodeTexels(TextureFormat::RGBA8, true, bytes, back);
    CHECK(back[0] == doctest::Approx(0.214f).epsilon(0.02));
    CHECK(back[3] == doctest::Approx(128.0f / 255.0f));
}

TEST_CASE("UDIM paths: tokens and tile numbers")
{
    auto p = image::udimPattern("C:/tex/rock_BaseColor.<UDIM>.png", false);
    REQUIRE(p);
    CHECK(p->prefix == "C:/tex/rock_BaseColor.");
    CHECK(p->suffix == ".png");
    CHECK(image::udimTilePath(*p, 1012) == "C:/tex/rock_BaseColor.1012.png");

    p = image::udimPattern("tex/rock.<udim>.exr", false);
    REQUIRE(p);
    CHECK(image::udimTilePath(*p, 1001) == "tex/rock.1001.exr");

    p = image::udimPattern("rock_<UVTILE>.tif", false);
    REQUIRE(p);
    CHECK(p->style == "uv");
    CHECK(image::udimTilePath(*p, 1012) == "rock_u2_v2.tif");

    // Tile numbers only count where asked for: image sequences use the same names for frames.
    CHECK_FALSE(image::udimPattern("shot.1001.png", false));
    p = image::udimPattern("D:\\maps\\shot.1001.png", true);
    REQUIRE(p);
    CHECK(p->prefix == "D:\\maps\\shot.");
    CHECK(image::udimTilePath(*p, 1002) == "D:\\maps\\shot.1002.png");
    p = image::udimPattern("maps/rock_1013.png", true);
    REQUIRE(p);
    CHECK(p->prefix == "maps/rock_");

    CHECK_FALSE(image::udimPattern("rock.1000.png", true));
    CHECK_FALSE(image::udimPattern("rock.0001.png", true));
    CHECK_FALSE(image::udimPattern("rock1001.png", true));
    CHECK_FALSE(image::udimPattern("maps.1001/rock.png", true));
    CHECK_FALSE(image::udimPattern("1001.png", true));
    CHECK_FALSE(image::udimPattern("rock.png", true));
}

TEST_CASE("UDIM paths: finding the tiles on disk")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lila_udim_tiles";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const char* name : {"rock.1001.png", "rock.1002.png", "rock.1011.png", "rock.1001.exr", "rock.notes.png", "rocky.1003.png"})
        std::ofstream(dir / name) << "x";

    const std::string base = (dir / "rock.").generic_string();
    auto p = image::udimPattern(base + "<UDIM>.png", false);
    REQUIRE(p);
    const auto tiles = image::findUdimTiles(*p);
    REQUIRE(tiles.size() == 3);
    CHECK(tiles[0].first == 1001);
    CHECK(tiles[1].first == 1002);
    CHECK(tiles[2].first == 1011);
    CHECK(tiles[2].second == base + "1011.png");

    p = image::udimPattern(base + "1002.png", true);
    REQUIRE(p);
    CHECK(image::findUdimTiles(*p).size() == 3);

    p = image::udimPattern(base + "<UDIM>.tga", false);
    REQUIRE(p);
    CHECK(image::findUdimTiles(*p).empty());
    fs::remove_all(dir);
}

TEST_CASE("Compute helpers: SPIR-V hashes, layout keys and slicing")
{
    const std::vector<uint32_t> a = {0x07230203, 1, 2, 3}, b = {0x07230203, 1, 2, 4};
    CHECK(render::compute::spirvHash(a) == render::compute::spirvHash(a));
    CHECK(render::compute::spirvHash(a) != render::compute::spirvHash(b));
    CHECK(render::compute::spirvHash(std::vector<uint32_t>{0}) != render::compute::spirvHash(std::vector<uint32_t>{0, 0}));

    using render::compute::layoutKey;
    CHECK(layoutKey(128, VK_SHADER_STAGE_COMPUTE_BIT) == layoutKey(128, VK_SHADER_STAGE_COMPUTE_BIT));
    CHECK(layoutKey(128, VK_SHADER_STAGE_COMPUTE_BIT) != layoutKey(96, VK_SHADER_STAGE_COMPUTE_BIT));
    CHECK(layoutKey(128, VK_SHADER_STAGE_COMPUTE_BIT) != layoutKey(128, VK_SHADER_STAGE_COMPUTE_BIT, "tlas"));

    using render::compute::sliceFit;
    // Everything fits.
    CHECK(sliceFit(1000, 0.01, 0.0, 40.0) == 1000);
    // An empty slice takes what fits, but never fewer than the minimum.
    CHECK(sliceFit(100000, 0.001, 0.0, 40.0) == 40000);
    CHECK(sliceFit(100000, 1.0, 0.0, 40.0) == 4096);
    CHECK(sliceFit(1000, 1.0, 0.0, 40.0) == 1000);
    // A slice already holding work stops rather than take a sliver.
    CHECK(sliceFit(100000, 1.0, 30.0, 40.0) == 0);
    CHECK(sliceFit(100000, 0.001, 30.0, 40.0) == 10000);
    CHECK(sliceFit(100000, 0.001, 50.0, 40.0) == 0);

    using render::compute::adaptRate;
    CHECK(adaptRate(100.0, 10.0, 10.0) == doctest::Approx(100.0));
    CHECK(adaptRate(100.0, 10.0, 20.0) == doctest::Approx(75.0));
    // Moves at most 2.5x either way per slice.
    CHECK(adaptRate(100.0, 100.0, 1.0) == doctest::Approx(250.0));
    CHECK(adaptRate(100.0, 1.0, 100.0) == doctest::Approx(62.5));
}
