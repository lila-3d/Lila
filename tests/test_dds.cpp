#include <doctest/doctest.h>

#include "core/Dds.h"
#include "core/ImageIO.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace lila;

namespace
{
/** A legacy-header DDS of one fourCC format, its levels' blocks given whole. */
fs::path writeDds(const char* name, const char fourCC[4], int w, int h, int mips, const std::vector<uint8_t>& texels)
{
    std::vector<uint8_t> b(128, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); };
    std::memcpy(b.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1007 | (mips > 1 ? 0x20000 : 0));
    put(12, uint32_t(h));
    put(16, uint32_t(w));
    put(28, uint32_t(mips));
    put(76, 32);
    put(80, 0x4);
    std::memcpy(&b[84], fourCC, 4);
    b.insert(b.end(), texels.begin(), texels.end());
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return p;
}

/** A BC1 block of one colour: both endpoints the RGB565 value, every index 0. */
std::vector<uint8_t> bc1Block(uint16_t rgb565)
{
    return {uint8_t(rgb565), uint8_t(rgb565 >> 8), uint8_t(rgb565), uint8_t(rgb565 >> 8), 0, 0, 0, 0};
}
} // namespace

TEST_CASE("dds: BC1 with mips reads its levels and decodes to its colour")
{
    std::vector<uint8_t> texels;
    for (int i = 0; i < 4 + 1 + 1; ++i) // 8x8, 4x4, 2x2: blocks round up
    {
        const std::vector<uint8_t> red = bc1Block(0xF800);
        texels.insert(texels.end(), red.begin(), red.end());
    }
    const fs::path p = writeDds("lila_test_bc1.dds", "DXT1", 8, 8, 3, texels);
    dds::File f;
    std::string err;
    REQUIRE(dds::read(p, f, &err));
    CHECK(f.format == dds::Format::BC1);
    REQUIRE(f.levels.size() == 3);
    CHECK(f.levels[1].width == 4);
    CHECK(f.levels[2].size == 8);
    const std::vector<float> rgba = dds::decodeFloat(f, 0);
    REQUIRE(rgba.size() == 8 * 8 * 4);
    CHECK(rgba[0] == doctest::Approx(1.0f));
    CHECK(rgba[1] == doctest::Approx(0.0f));
    CHECK(rgba[3] == doctest::Approx(1.0f));

    const image::Image img = image::read(p);
    CHECK(img.width == 8);
    CHECK(img.rgba.size() == 8 * 8 * 4);
    fs::remove(p);
}

TEST_CASE("dds: BC5 decodes as a normal map with Z rebuilt")
{
    // Both channels flat at 128: X and Y near 0, so Z is near 1.
    const std::vector<uint8_t> block = {128, 128, 0, 0, 0, 0, 0, 0, 128, 128, 0, 0, 0, 0, 0, 0};
    const fs::path p = writeDds("lila_test_bc5.dds", "ATI2", 4, 4, 1, block);
    dds::File f;
    REQUIRE(dds::read(p, f));
    CHECK(f.format == dds::Format::BC5);
    const std::vector<uint8_t> rgba = dds::decodeRgba8(f.format, 4, 4, f.data.data());
    CHECK(rgba[0] == 128);
    CHECK(rgba[1] == 128);
    CHECK(rgba[2] >= 254);
    fs::remove(p);
}

TEST_CASE("dds: truncated and unsupported files fail with a reason")
{
    const fs::path p = writeDds("lila_test_short.dds", "DXT1", 64, 64, 1, {1, 2, 3});
    dds::File f;
    std::string err;
    CHECK_FALSE(dds::read(p, f, &err));
    CHECK_FALSE(err.empty());
    const fs::path q = writeDds("lila_test_odd.dds", "XYZW", 4, 4, 1, bc1Block(0));
    CHECK_FALSE(dds::read(q, f, &err));
    CHECK(err.find("unsupported") != std::string::npos);
    fs::remove(p);
    fs::remove(q);
}
