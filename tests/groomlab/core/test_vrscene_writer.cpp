// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// The V-Ray scene export (vrscene_writer.h): what VrsceneWriter writes, read back. The packed
// lists (hex and ZIPC: zlib in base64) decoded, the plugins and their values compared with the
// strands; values that change between frames as interpolate() keys, the rest once. That V-Ray
// renders it is tests/hosts/vray/test_vray.py's (ctest -L vray).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "groomlab/export_view.h"
#include "groomlab/vrscene_writer.h"
#include "stb_image_groomlab.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// A plugin's body ("Type name { ... }"), or empty.
std::string pluginBody(const std::string& text, const std::string& type, const std::string& name) {
    const std::string head = type + " " + name + " {\n";
    const std::size_t at = text.find(head);
    if (at == std::string::npos) return {};
    const std::size_t end = text.find("\n}\n", at);
    return text.substr(at + head.size(), end + 1 - at - head.size());  // with the last line's "\n"
}

// A parameter's value text in a plugin body ("" when absent).
std::string param(const std::string& body, const std::string& name) {
    const std::string key = "  " + name + "=";
    const std::size_t at = body.find(key);
    if (at == std::string::npos) return {};
    const std::size_t end = body.find(";\n", at);
    return body.substr(at + key.size(), end - at - key.size());
}

std::vector<std::uint8_t> fromHex(const std::string& hex) {
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
    return out;
}

std::vector<std::uint8_t> fromBase64(const std::string& text) {
    static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        if (c == '=') break;
        bits = (bits << 6) | static_cast<std::uint32_t>(table.find(c));
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>(bits >> count));
        }
    }
    return out;
}

std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }

// A packed list's 32-bit values: List<Kind>Hex("...") plain or ZIPC.
template <class T>
std::vector<T> unpack(const std::string& value) {
    const std::size_t open = value.find("(\""), close = value.rfind("\")");
    EXPECT_NE(open, std::string::npos) << value.substr(0, 40);
    const std::string data = value.substr(open + 2, close - open - 2);
    std::vector<std::uint8_t> bytes;
    if (data.rfind("ZIPC", 0) == 0) {
        const std::vector<std::uint8_t> sizes = fromHex(data.substr(4, 16));
        const std::vector<std::uint8_t> z = fromBase64(data.substr(20));
        EXPECT_EQ(le32(sizes.data() + 4), z.size());
        int length = 0;
        char* raw = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(z.data()), static_cast<int>(z.size()), &length);
        EXPECT_NE(raw, nullptr);
        if (!raw) return {};
        EXPECT_EQ(static_cast<std::uint32_t>(length), le32(sizes.data()));
        bytes.assign(raw, raw + length);
        std::free(raw);
    } else {
        bytes = fromHex(data);
    }
    std::vector<T> out(bytes.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint32_t v = le32(bytes.data() + 4 * i);
        std::memcpy(&out[i], &v, 4);
    }
    return out;
}

// Three strands: two of four and three points, one of a single point (left out).
StrandSet sampleStrands(float shift = 0.0f) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}}, {0.02f, 0.02f, 0.01f, 0.004f});
    s.addStrand(2, {{shift, 5, 0}}, {0.02f});
    s.addStrand(3, {{1 + shift, 0, 0}, {1 + shift, 1, 1}, {1 + shift, 2, 2}}, {0.03f, 0.02f, 0.01f});
    const float rgb[3][3] = {{0.2f, 0.1f, 0.05f}, {0.5f, 0.5f, 0.5f}, {0.6f, 0.3f, 0.1f}};
    for (int k = 0; k < 3; ++k) {
        std::vector<float>& c = s.addStrandChannel(kColorChannels[k]);
        for (int strand = 0; strand < 3; ++strand) c[strand] = rgb[strand][k];
    }
    return s;
}

fs::path tempFile(const std::string& name) { return fs::temp_directory_path() / ("groomlab_vrscene_" + name); }

TEST(VrsceneWriter, OneFrame) {
    for (const bool compress : {true, false}) {
        const fs::path path = tempFile(compress ? "zip.vrscene" : "hex.vrscene");
        VrsceneWriter::Options options;
        options.compress = compress;
        options.comment = "test";
        options.lobes = hairLobes(0.3f, 0.5f, 3.0f);
        {
            VrsceneWriter writer(path.u8string(), "my groom", options);
            const double matrix[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};  // 90 degrees about Z
            writer.writeFrame(1.0, sampleStrands(), nullptr, matrix);
            writer.close();
        }
        const std::string text = readText(path);
        fs::remove(path);
        SCOPED_TRACE(compress ? "ZIPC" : "hex");
        EXPECT_EQ(text.rfind("// GroomLab ", 0), 0u);
        EXPECT_NE(text.find("// 2 strands, 7 points, frames 1\n"), std::string::npos) << text.substr(0, 200);

        const std::string units = pluginBody(text, "SettingsUnitsInfo", "my_groom@units");
        EXPECT_EQ(param(units, "meters_scale"), "0.01");
        EXPECT_EQ(param(units, "scene_upDir"), "Vector(0, 1, 0)");
        EXPECT_EQ(param(units, "frames_scale"), "24");

        const std::string hair = pluginBody(text, "GeomMayaHair", "my_groom@hair");
        ASSERT_FALSE(hair.empty());
        EXPECT_EQ(unpack<std::int32_t>(param(hair, "num_hair_vertices")), (std::vector<std::int32_t>{4, 3}));
        const std::vector<float> xyz = unpack<float>(param(hair, "hair_vertices"));
        ASSERT_EQ(xyz.size(), 21u);
        EXPECT_FLOAT_EQ(xyz[3 * 3 + 1], 3.0f);  // strand 1's tip
        EXPECT_FLOAT_EQ(xyz[3 * 6 + 2], 2.0f);  // strand 3's tip (strand 2 left out)
        // Radii: half the widths.
        EXPECT_EQ(unpack<float>(param(hair, "widths")),
                  (std::vector<float>{0.01f, 0.01f, 0.005f, 0.002f, 0.015f, 0.01f, 0.005f}));
        const std::vector<float> colors = unpack<float>(param(hair, "colors"));
        ASSERT_EQ(colors.size(), 21u);
        const std::array<float, 3> dye1 = vray::vrayDyeColor({0.2f, 0.1f, 0.05f});
        const std::array<float, 3> dye3 = vray::vrayDyeColor({0.6f, 0.3f, 0.1f});
        for (int k = 0; k < 3; ++k) {
            EXPECT_FLOAT_EQ(colors[3 * 3 + k], dye1[k]);
            EXPECT_FLOAT_EQ(colors[3 * 4 + k], dye3[k]);
        }
        EXPECT_GT(colors[0], 0.2f);  // the dye absorbs less than the base color: lighter
        EXPECT_TRUE(param(hair, "strand_uvw").empty());   // no scalp: no root UVs
        EXPECT_TRUE(param(hair, "transparency").empty());  // no opacity channel
        EXPECT_EQ(param(hair, "geom_splines"), "1");

        const std::string brdf = pluginBody(text, "BRDFHair4", "my_groom@hairBrdf");
        EXPECT_EQ(param(brdf, "melanin"), "0");
        EXPECT_EQ(param(brdf, "dye_color"), "my_groom@hairSampler::hair_color");
        const vray::HairBrdf b = vray::vrayHairBrdf(hairLobes(0.3f, 0.5f, 3.0f));
        EXPECT_NEAR(std::stof(param(brdf, "glossiness")), b.glossiness, 1e-6);
        EXPECT_NEAR(b.glossiness, 0.462f, 0.01f);  // Arnold's roughness 0.3
        EXPECT_NEAR(std::stof(param(brdf, "softness")), 0.35f, 1e-5);
        EXPECT_NEAR(std::stof(param(brdf, "ior")), vray::kVRayIor, 1e-6);
        EXPECT_EQ(param(brdf, "trace_depth"), "40");
        EXPECT_NE(text.find("TexHairSampler my_groom@hairSampler {"), std::string::npos);
        EXPECT_EQ(param(pluginBody(text, "MtlSingleBRDF", "my_groom@hairMtl"), "brdf"), "my_groom@hairBrdf");

        const std::string node = pluginBody(text, "Node", "my_groom@node");
        EXPECT_EQ(param(node, "transform"), "Transform(Matrix(Vector(0, 1, 0), Vector(-1, 0, 0), Vector(0, 0, 1)), Vector(5, 6, 7))");
        EXPECT_EQ(param(node, "geometry"), "my_groom@hair");
        EXPECT_EQ(param(node, "material"), "my_groom@hairMtl");
        EXPECT_EQ(param(node, "nsamples"), "");  // V-Ray's default: motion blur's samples (1 keeps the keys still)
    }
}

TEST(VrsceneWriter, ScaleInThePoints) {
    // Blender's V-Ray scene: the centimeter strands in meters, the points and radii scaled (V-Ray
    // does not scale hair widths with a transform), the file's unit the meter.
    const fs::path path = tempFile("scaled.vrscene");
    VrsceneWriter::Options options;
    options.scale = 0.01;
    options.metersPerUnit = 1.0;
    {
        VrsceneWriter writer(path.u8string(), "g", options);
        writer.writeFrame(1.0, sampleStrands());
        writer.close();
    }
    const std::string text = readText(path);
    fs::remove(path);
    EXPECT_EQ(param(pluginBody(text, "SettingsUnitsInfo", "g@units"), "meters_scale"), "1");
    const std::string hair = pluginBody(text, "GeomMayaHair", "g@hair");
    const std::vector<float> xyz = unpack<float>(param(hair, "hair_vertices"));
    ASSERT_EQ(xyz.size(), 21u);
    EXPECT_FLOAT_EQ(xyz[3 * 3 + 1], 0.03f);
    const std::vector<float> widths = unpack<float>(param(hair, "widths"));
    ASSERT_EQ(widths.size(), 7u);
    EXPECT_FLOAT_EQ(widths[0], 0.0001f);

    // A scaled transform: its scale in the points and radii, the Node's rotation and translation
    // alone (V-Ray does not scale hair widths by a transform).
    {
        VrsceneWriter writer(path.u8string(), "g", VrsceneWriter::Options{});
        const double matrix[16] = {0, 3, 0, 0, -3, 0, 0, 0, 0, 0, 3, 0, 5, 6, 7, 1};  // 90 degrees about Z, x3
        writer.writeFrame(1.0, sampleStrands(), nullptr, matrix);
        writer.close();
    }
    const std::string scaled = readText(path);
    fs::remove(path);
    const std::string body = pluginBody(scaled, "GeomMayaHair", "g@hair");
    EXPECT_FLOAT_EQ(unpack<float>(param(body, "hair_vertices"))[3 * 3 + 1], 9.0f);
    EXPECT_FLOAT_EQ(unpack<float>(param(body, "widths"))[0], 0.03f);
    EXPECT_EQ(param(pluginBody(scaled, "Node", "g@node"), "transform"),
              "Transform(Matrix(Vector(0, 1, 0), Vector(-1, 0, 0), Vector(0, 0, 1)), Vector(5, 6, 7))");
}

TEST(VrsceneWriter, FramesAsKeys) {
    const fs::path path = tempFile("anim.vrscene");
    {
        VrsceneWriter writer(path.u8string(), "groom", {});
        writer.writeFrame(1.0, sampleStrands(0.0f));
        writer.writeFrame(1.5, sampleStrands(0.5f));
        writer.writeFrame(2.0, sampleStrands(1.0f));
        EXPECT_THROW(writer.writeFrame(2.0, sampleStrands()), VrsceneError);
        EXPECT_EQ(writer.frameCount(), 3u);
    }  // the destructor closes
    const std::string text = readText(path);
    fs::remove(path);
    EXPECT_FALSE(fs::exists(path.u8string() + ".spool1"));
    const std::string hair = pluginBody(text, "GeomMayaHair", "groom@hair");
    // The points move: one key a frame; the rest written once.
    const std::string points = param(hair, "hair_vertices");
    ASSERT_EQ(points.rfind("interpolate(", 0), 0u) << points.substr(0, 40);
    const std::regex key(R"re(\((1|1\.5|2), ListVectorHex\("([^"]*)"\)\))re");
    std::vector<std::string> times;
    for (auto it = std::sregex_iterator(points.begin(), points.end(), key); it != std::sregex_iterator(); ++it) {
        times.push_back((*it)[1]);
        const std::vector<float> xyz = unpack<float>("ListVectorHex(\"" + (*it)[2].str() + "\")");
        ASSERT_EQ(xyz.size(), 21u);
        EXPECT_FLOAT_EQ(xyz[12], std::stof((*it)[1]));  // strand 3's root x: 1 + the shift, frame - 1
    }
    EXPECT_EQ(times, (std::vector<std::string>{"1", "1.5", "2"}));
    for (const char* still : {"num_hair_vertices", "widths", "colors"}) {
        EXPECT_EQ(param(hair, still).rfind("List", 0), 0u) << still;
    }
    EXPECT_NE(text.find("frames 1 to 2"), std::string::npos);
    EXPECT_EQ(param(pluginBody(text, "Node", "groom@node"), "transform").rfind("Transform(", 0), 0u);  // it holds: written once
}

TEST(VrsceneWriter, OpacityAndWetness) {
    StrandSet s = sampleStrands();
    std::vector<float>& opacity = s.addPointChannel(kOpacityChannel, 1.0f);
    opacity[2] = 0.25f;
    std::vector<float>& wet = s.addStrandChannel(kWetnessChannel);
    wet[2] = 1.0f;  // strand 3 soaked: darker
    const fs::path path = tempFile("look.vrscene");
    {
        VrsceneWriter writer(path.u8string(), "groom", {});
        writer.writeFrame(1.0, s);
    }
    const std::string text = readText(path);
    fs::remove(path);
    const std::string hair = pluginBody(text, "GeomMayaHair", "groom@hair");
    const std::vector<float> t = unpack<float>(param(hair, "transparency"));
    ASSERT_EQ(t.size(), 21u);
    EXPECT_FLOAT_EQ(t[3 * 2], 0.75f);
    EXPECT_FLOAT_EQ(t[0], 0.0f);
    EXPECT_EQ(param(pluginBody(text, "BRDFHair4", "groom@hairBrdf"), "transparency"), "groom@hairSampler::hair_transparency");
    const std::vector<float> colors = unpack<float>(param(hair, "colors"));
    const std::array<float, 3> dry = vray::vrayDyeColor({0.6f, 0.3f, 0.1f});
    for (int k = 0; k < 3; ++k) EXPECT_LT(colors[3 * 4 + k], dry[k]);
}

TEST(VrsceneWriter, PluginNames) {
    EXPECT_EQ(vray::pluginName("hair groom-1"), "hair_groom_1");
    EXPECT_EQ(vray::pluginName("1st"), "_1st");
    EXPECT_EQ(vray::pluginName(""), "groom");
}

}  // namespace
}  // namespace groomlab
