// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Scaling a groom with its scalp (GroomSession::setScaleSource, groomScale): a scalp k times as
// big (its area k^2 times the reference) makes the groom at 1x scaled by k, whatever the stored
// params, guides and keyed controls say: as many strands, their points k times as far from the
// origin the scalp was scaled about. Measured on the rest pose, so a deforming scalp keeps one k;
// a re-meshed scalp of the same surface keeps k at 1.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/presets.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

constexpr double kFps = 24.0;

std::vector<Vec3f> scaled(std::vector<Vec3f> points, float k) {
    for (Vec3f& p : points) p = p * k;
    return points;
}

// The mesh k times as big about the origin (its rest pose too).
MeshPtr scaledMesh(const MeshPtr& mesh, float k) {
    auto out = std::make_shared<ScalpMesh>(scaled(mesh->points(), k), mesh->triangles());
    if (mesh->hasUVs()) out->setCornerUVs(mesh->cornerUVs());
    if (mesh->hasRest()) out->setRestPoints(scaled(mesh->restPoints(), k));
    return out;
}

// Every triangle split in four at its edges' midpoints: another mesh of the same surface.
MeshPtr splitMesh(const MeshPtr& mesh) {
    std::vector<Vec3f> points = mesh->points();
    std::vector<ScalpMesh::Triangle> triangles;
    std::map<std::pair<int, int>, int> mids;
    auto mid = [&](int a, int b) {
        const auto key = std::make_pair(std::min(a, b), std::max(a, b));
        const auto it = mids.find(key);
        if (it != mids.end()) return it->second;
        points.push_back((points[static_cast<std::size_t>(a)] + points[static_cast<std::size_t>(b)]) * 0.5f);
        return mids[key] = static_cast<int>(points.size() - 1);
    };
    for (const auto& t : mesh->triangles()) {
        const int a = static_cast<int>(t[0]), b = static_cast<int>(t[1]), c = static_cast<int>(t[2]);
        const int ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
        triangles.push_back({a, ab, ca});
        triangles.push_back({ab, b, bc});
        triangles.push_back({ca, bc, c});
        triangles.push_back({ab, bc, ca});
    }
    return std::make_shared<ScalpMesh>(points, triangles);
}

// The largest distance of b's points from a's scaled by k, as a share of the groom's size at k;
// the strands' IDs must be the same.
double difference(const StrandSet& a, const StrandSet& b, float k, double size) {
    EXPECT_EQ(a.strandCount(), b.strandCount());
    if (a.ids() != b.ids()) {
        ADD_FAILURE() << "other strands at x" << k;
        return 1.0;
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < a.strandCount(); ++i) {
        if (a.strandPointCount(i) != b.strandPointCount(i)) return 1.0;
        const Vec3f* p = a.strandPoints(i);
        const Vec3f* q = b.strandPoints(i);
        for (std::size_t j = 0; j < a.strandPointCount(i); ++j) worst = std::max(worst, double((q[j] - p[j] * k).length()));
    }
    if (a.widths().size() == b.widths().size()) {
        for (std::size_t i = 0; i < a.widths().size(); ++i) worst = std::max(worst, double(std::abs(b.widths()[i] - a.widths()[i] * k)));
    }
    return worst / (size * k);
}

GroomData strokesGroom(const std::string& graph = {}) {
    GroomData data = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom").data;
    if (!graph.empty()) data.graphJson = graph;
    return data;
}

std::string presetText(const std::string& name) {
    std::ifstream in(std::filesystem::u8path(std::string(GROOMLAB_SHIPPED_PRESETS) + "/" + name + ".json"), std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return presetGraphJson(text.str(), *presetScalpSize(*strokesGroom().scalp));
}

}  // namespace

TEST(ScaleWithScalp, TheUnitTableIsThePresetsOne) {
    EXPECT_DOUBLE_EQ(*unitScaleFactor("length", 2.0), 2.0);
    EXPECT_DOUBLE_EQ(*unitScaleFactor("width", 2.0), 2.0);
    EXPECT_DOUBLE_EQ(*unitScaleFactor("density", 2.0), 0.25);
    EXPECT_DOUBLE_EQ(*unitScaleFactor("perLength", 2.0), 0.5);
    for (const char* unit : {"", "angle", "turns", "fraction", "seconds", "pixels", "color"}) EXPECT_FALSE(unitScaleFactor(unit, 2.0)) << unit;
    // Every unit the operators' params say is one of those (a new one needs a row in the table).
    const std::set<std::string> known = {"", "length", "width", "density", "perLength", "angle", "turns", "fraction", "seconds", "pixels", "color"};
    for (const json& op : json::parse(operatorSpecsJson())) {
        for (const json& p : op.value("params", json::array())) {
            EXPECT_TRUE(known.count(p.value("unit", std::string()))) << op.value("type", std::string()) << "." << p.value("name", std::string())
                                                                     << ": unit " << p.value("unit", std::string());
        }
    }
    // A graph scaled by 2 is a preset fitted to a scalp twice its size; 1 leaves the text.
    const std::string graph = presetText("everyday_sim");
    EXPECT_EQ(scaleGraphJson(graph, 1.0), graph);
    json withSize = json::parse(graph);
    withSize["size"] = 1.0;
    EXPECT_EQ(json::parse(scaleGraphJson(graph, 2.0)), json::parse(fitPresetJson(withSize.dump(), 2.0)));
}

TEST(ScaleWithScalp, APoseStretchingPartOfTheScalpKeepsItsPoseFreeArea) {
    // poseFreeArea: a uniform scale comes through (a rig scaled at its root), a pose that stretches
    // some triangles (a shoulder raised) does not; no rest pose: the plain area.
    GroomData data = strokesGroom();
    auto rested = std::make_shared<ScalpMesh>(*data.scalp);
    rested->setRestPoints(data.scalp->points());
    const double rest = restSurfaceArea(*rested);
    std::vector<Vec3f> doubled = rested->points();
    for (Vec3f& p : doubled) p = p * 2.0f;
    EXPECT_NEAR(poseFreeArea(*rested, doubled), 4.0 * rest, 1e-4 * rest);
    std::vector<Vec3f> posed = rested->points();
    float top = posed.front().y;
    for (const Vec3f& p : posed) top = std::max(top, p.y);
    for (Vec3f& p : posed) {
        if (p.y > top - 0.15f * top) p = {p.x * 1.6f, p.y, p.z * 1.6f};  // the crown stretched
    }
    EXPECT_GT(std::fabs(surfaceArea(posed, rested->triangles()) / rest - 1.0), 0.02) << "the pose stretches";
    EXPECT_NEAR(poseFreeArea(*rested, posed), rest, 1e-3 * rest);
    EXPECT_NEAR(poseFreeArea(*data.scalp, doubled), surfaceArea(doubled, data.scalp->triangles()), 1e-6);
}

TEST(ScaleWithScalp, AGroomFileKeepsTheReferencesAndTheSource) {
    for (const ScaleSource source : {ScaleSource::Transform, ScaleSource::Off}) {
        GroomFile file;
        file.data.scalp = test::makeSphere(6, 12);
        file.data.scaleReference = 12.5;
        file.data.scaleTransformReference = 2.5;
        file.data.scaleSource = source;
        const GroomFile back = groomFileFromJson(groomFileToJson(file));
        EXPECT_DOUBLE_EQ(back.data.scaleReference, 12.5);
        EXPECT_DOUBLE_EQ(back.data.scaleTransformReference, 2.5);
        EXPECT_EQ(back.data.scaleSource, source) << scaleSourceName(source);
    }
    GroomFile plain;
    plain.data.scalp = test::makeSphere(6, 12);
    const json text = json::parse(groomFileToJson(plain));
    EXPECT_FALSE(text.contains("scaleReference")) << "written only when set";
    EXPECT_FALSE(text.contains("scaleTransformReference")) << "written only when set";
    EXPECT_FALSE(text.contains("scaleSource")) << "written only when not area";
    const GroomFile plainBack = groomFileFromJson(text.dump());
    EXPECT_EQ(plainBack.data.scaleSource, ScaleSource::Area);
    EXPECT_EQ(plainBack.data.scaleReference, 0.0);
}

}  // namespace groomlab
