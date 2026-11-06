// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Simulate, bake to a cache, render: a groom file's scene steps its Simulate node over the scalp
// cache's frames (its feed), a bake written from the session reads back as the same frames, and a
// groom file naming the bake renders them without simulating (unless the node's settings changed).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

#include "../core/test_helpers.h"
#include "groomlab/alembic_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/sim_cache.h"

namespace groomlab {
namespace {

// A temporary file of the running test's own: ctest runs each test in a process of its own, side by
// side, and several build a Scene (one test removing the cache another is reading).
std::string tempPath(const char* name) {
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    return (std::filesystem::temp_directory_path() / (test + "_" + name)).string();
}

const char* kGraph = R"({"version": 1, "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "sim", "type": "Simulate", "params": {"gravity": 9.8, "collisionOffset": 0.02, "damping": 0.2}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}],
  "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                  {"from": "scatter", "to": "interpolate", "input": "strands"},
                  {"from": "guides", "to": "sim", "input": "guides"},
                  {"from": "sim", "to": "interpolate", "input": "guides"},
                  {"from": "scalp", "to": "interpolate", "input": "scalp"},
                  {"from": "interpolate", "to": "output", "input": "strands"}], "output": "output"})";

// A plane scalp sliding +x over frames 2-8 and still after, cached at 24 fps; guides standing on it.
struct Scene {
    MeshPtr plane = test::makePlane(4);
    std::string cache = tempPath("groomlab_sim_scalp.abc");
    Scene() {
        AlembicMeshWriter writer(cache, "head", 0.0, 1.0 / 24.0);
        for (int f = 0; f <= 30; ++f) {
            std::vector<Vec3f> points = plane->points();
            for (Vec3f& p : points) p.x += 0.1f * static_cast<float>(std::clamp(f - 2, 0, 6));
            writer.writeFrame(plane->withPoints(std::move(points)));
        }
    }
    ~Scene() {
        std::error_code ec;
        std::filesystem::remove(cache, ec);
    }
    GroomFile file() const {
        GuideHierarchy h;
        const GroupId group = h.addGroup("primary");
        for (int i = 0; i < 6; ++i) {
            const Vec3f root{0.15f + 0.14f * static_cast<float>(i), 0.0f, 0.5f};
            h.addGuide(group, test::straightCurve(root, {0.1f, 0.6f, 0.0f}, 8));
        }
        h.evaluate(plane.get());
        GroomFile f;
        f.data.scalp = plane;
        f.data.graphJson = kGraph;
        f.data.hierarchyJson = h.toJson();
        f.scalpCache = {cache, "head"};
        return f;
    }
};

// The worst distance between two sets of strands' points (infinity for other counts).
float worstApart(const StrandSet& a, const StrandSet& b) {
    if (a.pointCount() != b.pointCount()) return std::numeric_limits<float>::infinity();
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.pointCount(); ++i) worst = std::max(worst, (a.positions()[i] - b.positions()[i]).length());
    return worst;
}

// Guides the graph makes (ToGuides of strands grown from the hand guides) into the Simulate node:
// IDs that are not the hierarchy's.
const char* kMadeGuidesGraph = R"({"version": 1, "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "even", "type": "Scatter", "params": {"density": 6.0, "randomness": 0.0, "length": 0.5, "previewAll": true}},
    {"name": "dense", "type": "Interpolate"},
    {"name": "toGuides", "type": "ToGuides"},
    {"name": "sim", "type": "Simulate", "params": {"gravity": 9.8, "collisionOffset": 0.02, "damping": 0.2}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}],
  "connections": [{"from": "scalp", "to": "even", "input": "scalp"},
                  {"from": "even", "to": "dense", "input": "strands"},
                  {"from": "guides", "to": "dense", "input": "guides"},
                  {"from": "scalp", "to": "dense", "input": "scalp"},
                  {"from": "dense", "to": "toGuides", "input": "strands"},
                  {"from": "toGuides", "to": "sim", "input": "guides"},
                  {"from": "scalp", "to": "scatter", "input": "scalp"},
                  {"from": "scatter", "to": "interpolate", "input": "strands"},
                  {"from": "sim", "to": "interpolate", "input": "guides"},
                  {"from": "scalp", "to": "interpolate", "input": "scalp"},
                  {"from": "interpolate", "to": "output", "input": "strands"}], "output": "output"})";

TEST(SimulationCache, GroomFilesKeepTheBakesTheyName) {
    GroomFile file;
    file.data.scalp = test::makePlane(2);
    file.simCaches["hair"] = {"hair_sim/hair.abc", "hair", 0x0123456789ABCDEFull};
    file.simCaches["beard"] = {"beard.abc", "", 7};
    const GroomFile back = groomFileFromJson(groomFileToJson(file), "C:/shows/abc");
    ASSERT_EQ(back.simCaches.size(), 2u);
    EXPECT_EQ(back.simCaches.at("hair").stamp, 0x0123456789ABCDEFull);
    EXPECT_EQ(back.simCaches.at("hair").object, "hair");
    EXPECT_EQ(std::filesystem::path(back.simCaches.at("beard").file), std::filesystem::path("C:/shows/abc/beard.abc"));
    EXPECT_EQ(formatSimulationStamp(7), "0000000000000007");
    EXPECT_EQ(parseSimulationStamp("not hex"), 0u);
}

}  // namespace
}  // namespace groomlab
