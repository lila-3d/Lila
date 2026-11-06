// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Several scalps on one groom (scalp_pieces.h): joining them, cutting each one's own mesh out of the
// joined one (refined too), a Scatter per scalp merged into one output with every root on the joined
// mesh, the groom's own scalp's strands as without the others, groom files, vertex maps painted
// before a scalp was added, and a Scalp node naming a scalp the groom does not have.

#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_history.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/stroke.h"
#include "groomlab/subdivision.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr double kFrameSeconds = 1.0 / 24.0;

// A plane moved to `at` (an ear beside a head), with UVs and a color set when asked.
MeshPtr movedPlane(Vec3f at, int cells = 4, float size = 1.0f, bool uvs = false, const char* colorSet = nullptr) {
    const MeshPtr plane = test::makePlane(cells, size);
    std::vector<Vec3f> points = plane->points();
    for (Vec3f& p : points) p = p + at;
    auto mesh = std::make_shared<ScalpMesh>(std::move(points), plane->triangles());
    if (uvs) {
        std::vector<ScalpMesh::UV> corners;
        for (const ScalpMesh::Triangle& t : mesh->triangles()) {
            for (const std::int32_t v : t) corners.push_back({plane->points()[v].x / size, plane->points()[v].z / size});
        }
        mesh->setCornerUVs(std::move(corners));
    }
    if (colorSet) mesh->setCornerColors(colorSet, std::vector<ScalpMesh::Color>(mesh->triangleCount() * 3, {0.5f, 0.5f, 0.5f, 1.0f}));
    return mesh;
}

// Scatter on the groom's own scalp, and with `ears` a second Scatter on the scalp "ears", merged.
std::string branchesGraph(bool ears, const std::string& subdivision = "off", const std::string& earsName = "ears") {
    std::string nodes = R"({"name": "scalp", "type": "Scalp", "params": {"subdivision": ")" + subdivision + R"("}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 400.0, "length": 0.2}},
    {"name": "output", "type": "Output"})";
    std::string connections = R"({"from": "scalp", "to": "scatter", "input": "scalp"})";
    std::string last = "scatter";
    if (ears) {
        nodes += R"(,
    {"name": "earScalp", "type": "Scalp", "params": {"input": ")" + earsName + R"(", "subdivision": ")" + subdivision + R"("}},
    {"name": "earScatter", "type": "Scatter", "params": {"density": 300.0, "length": 0.1, "seed": 7}},
    {"name": "merge", "type": "Merge"})";
        connections += R"(,
    {"from": "earScalp", "to": "earScatter", "input": "scalp"},
    {"from": "scatter", "to": "merge", "input": "a"},
    {"from": "earScatter", "to": "merge", "input": "b"})";
        last = "merge";
    }
    connections += R"(,
    {"from": ")" + last + R"(", "to": "output", "input": "strands"})";
    return R"({"version": 1, "nodes": [)" + nodes + R"(], "connections": [)" + connections + R"(], "output": "output"})";
}

TEST(ScalpPieces, OneScalpJoinsToItself) {
    const MeshPtr head = test::makeSphere(8, 16);
    EXPECT_EQ(combineScalps(head, {}), head);
    EXPECT_EQ(combineScalps(head, {{"ears", nullptr}}), head);
    EXPECT_EQ(combineScalps(head, {{"scalp", movedPlane({3, 0, 0})}}), head);  // the groom's own name
    EXPECT_EQ(combineScalps(head, {{"1ears", movedPlane({3, 0, 0})}}), head);  // not a name
    EXPECT_TRUE(scalpPieces(head).empty());
}

TEST(ScalpPieces, JoinedMeshKeepsTheOwnScalpFirst) {
    const MeshPtr head = movedPlane({0, 0, 0}, 4, 1.0f, true, "density");
    const MeshPtr ears = movedPlane({3, 0, 0}, 2);
    const MeshPtr joined = combineScalps(head, {{"ears", ears}, {"ears", movedPlane({6, 0, 0})}});
    ASSERT_NE(joined, head);
    ASSERT_EQ(joined->pieces().size(), 2u);  // the second "ears" is left out
    EXPECT_EQ(joined->pointCount(), head->pointCount() + ears->pointCount());
    EXPECT_EQ(joined->triangleCount(), head->triangleCount() + ears->triangleCount());
    const ScalpPiece* own = joined->piece("scalp");
    const ScalpPiece* other = joined->piece("ears");
    ASSERT_TRUE(own && other);
    EXPECT_EQ(own->firstTriangle, 0u);
    EXPECT_EQ(other->firstTriangle, head->triangleCount());
    EXPECT_EQ(other->firstPoint, head->pointCount());
    for (std::size_t t = 0; t < head->triangleCount(); ++t) EXPECT_EQ(joined->triangles()[t], head->triangles()[t]);
    // UVs and color sets where one scalp has them: the other gets zeros (alpha 1).
    ASSERT_TRUE(joined->hasUVs());
    EXPECT_EQ(joined->cornerUVs().size(), joined->triangleCount() * 3);
    ASSERT_TRUE(joined->hasColorSet("density"));
    const StrandRoot onEars{static_cast<std::int32_t>(other->firstTriangle), 0.2f, 0.2f};
    EXPECT_EQ(joined->colorAt("density", onEars)[0], 0.0f);
    EXPECT_EQ(joined->colorAt("density", {0, 0.2f, 0.2f})[0], 0.5f);
    // The pieces cut back out: each its own scalp, roots landing on the joined mesh.
    const auto pieces = scalpPieces(joined);
    ASSERT_EQ(pieces.size(), 2u);
    const MeshPtr earPiece = pieces.at("ears");
    EXPECT_TRUE(earPiece->isPiece());
    EXPECT_EQ(earPiece->whole(), joined);
    EXPECT_EQ(earPiece->triangleCount(), ears->triangleCount());
    EXPECT_EQ(earPiece->points(), ears->points());
    const StrandRoot local{1, 0.3f, 0.3f};
    const StrandRoot global = toWholeRoot(*earPiece, local);
    EXPECT_EQ(global.face, static_cast<std::int32_t>(other->firstTriangle) + 1);
    EXPECT_EQ(joined->position(global), earPiece->position(local));
    EXPECT_EQ(toPieceRoot(*earPiece, global).face, 1);
    EXPECT_EQ(toPieceRoot(*earPiece, StrandRoot{0, 0, 0}).face, -1);  // on the groom's own scalp
    EXPECT_EQ(wholeScalp(earPiece), joined);
    EXPECT_EQ(scalpPieces(joined).at("ears"), earPiece);  // cut once per mesh
}

TEST(ScalpPieces, RefinedPiecesAreTheJoinedMeshCut) {
    const MeshPtr head = test::makeSphere(6, 12);
    const MeshPtr ears = movedPlane({3, 0, 0}, 3);
    const MeshPtr joined = combineScalps(head, {{"ears", ears}});
    for (int levels = 1; levels <= 2; ++levels) {
        const MeshPtr refined = subdivideScalp(joined, levels);
        const auto pieces = scalpPieces(refined);
        ASSERT_EQ(pieces.size(), 2u);
        const MeshPtr earPiece = pieces.at("ears");
        const MeshPtr alone = subdivideScalp(ears, levels);
        ASSERT_EQ(earPiece->triangleCount(), alone->triangleCount());
        EXPECT_EQ(earPiece->subdivisionLevel(), levels);
        ASSERT_TRUE(earPiece->cage());
        EXPECT_TRUE(refinementPlan(*earPiece->cage(), *earPiece));
        // Each triangle where the ears refined alone put it, and where the joined mesh has it.
        for (std::size_t t = 0; t < earPiece->triangleCount(); ++t) {
            const StrandRoot r{static_cast<std::int32_t>(t), 0.25f, 0.25f};
            EXPECT_LT((earPiece->position(r) - alone->position(r)).length(), 1e-5f);
            EXPECT_EQ(refined->position(toWholeRoot(*earPiece, r)), earPiece->position(r));
        }
    }
}

TEST(ScalpPieces, VertexMapPaintedBeforeAScalpWasAdded) {
    const MeshPtr head = movedPlane({0, 0, 0});
    const MeshPtr joined = combineScalps(head, {{"ears", movedPlane({3, 0, 0}, 2)}});
    ScalpMaps maps;
    maps.maps["density"] = {"vertices", std::vector<float>(head->pointCount(), 0.75f), ""};
    std::vector<std::string> errors;
    const MeshPtr mapped = withVertexMaps(joined, maps, "", nullptr, &errors, 1);
    EXPECT_TRUE(errors.empty());
    ASSERT_TRUE(mapped->hasColorSet("density"));
    EXPECT_FLOAT_EQ(mapped->colorAt("density", {0, 0.2f, 0.2f})[0], 0.75f);
    const StrandRoot onEars{static_cast<std::int32_t>(head->triangleCount()), 0.2f, 0.2f};
    EXPECT_FLOAT_EQ(mapped->colorAt("density", onEars)[0], 0.0f);
}

// ---- Scripted strokes on the other scalps ------------------------------------------------------

// ---- Jiggle on the other scalps ----------------------------------------------------------------

// ---- Each scalp at its own subdivision level ---------------------------------------------------

// The ears' Scatter alone on `scalp` (a groom whose own scalp it is), as branchesGraph's ears.
std::string earsAloneGraph(const std::string& subdivision) {
    return R"({"version": 1, "nodes": [
    {"name": "earScalp", "type": "Scalp", "params": {"subdivision": ")" + subdivision + R"("}},
    {"name": "earScatter", "type": "Scatter", "params": {"density": 300.0, "length": 0.1, "seed": 7}},
    {"name": "output", "type": "Output"}],
    "connections": [{"from": "earScalp", "to": "earScatter", "input": "scalp"},
    {"from": "earScatter", "to": "output", "input": "strands"}], "output": "output"})";
}

TEST(ScalpPieces, PlanRefinesEachScalpAtItsOwnLevel) {
    const MeshPtr head = test::makeSphere(6, 12);
    const MeshPtr ears = movedPlane({3, 0, 0}, 3, 1.0f, true);
    const MeshPtr brows = movedPlane({0, 3, 0}, 2);
    const MeshPtr joined = combineScalps(head, {{"ears", ears}, {"brows", brows}});
    // The head as it is, the ears twice, the brows once (the most: 2).
    const MeshPtr levelled = withScalpLevels(joined, {{"scalp", 0}, {"ears", 2}, {"brows", 1}}, 2);
    ASSERT_NE(levelled, joined);
    EXPECT_EQ(levelled->piece("scalp")->subdivision, 0);
    EXPECT_EQ(levelled->piece("ears")->subdivision, -1);  // the whole's
    EXPECT_EQ(levelled->piece("brows")->subdivision, 1);
    EXPECT_EQ(withScalpLevels(levelled, {{"scalp", 0}, {"ears", 2}, {"brows", 1}}, 2), levelled);  // nothing new
    EXPECT_EQ(withScalpLevels(joined, {{"scalp", 1}, {"ears", 1}}, 1), joined);  // one level: one plan, as ever
    EXPECT_EQ(pieceSubdivisionLevels(*levelled, 2), (std::vector<int>{0, 2, 1}));
    EXPECT_TRUE(pieceSubdivisionLevels(*joined, 2).empty());

    const MeshPtr refined = subdivideScalp(levelled, 2);
    EXPECT_EQ(refined->subdivisionLevel(), 2);
    const MeshPtr earsRefined = subdivideScalp(ears, 2), browsRefined = subdivideScalp(brows, 1);
    ASSERT_EQ(refined->triangleCount(), head->triangleCount() + earsRefined->triangleCount() + browsRefined->triangleCount());
    ASSERT_EQ(refined->pointCount(), head->pointCount() + earsRefined->pointCount() + browsRefined->pointCount());
    ASSERT_TRUE(refined->hasUVs());
    const auto plan = refinementPlan(*levelled, *refined);
    ASSERT_TRUE(plan);
    // Each scalp's triangles are its own refinement's, in turn: the head's its own.
    auto expectSame = [&](const ScalpMesh& own, std::size_t at) {
        for (std::size_t t = 0; t < own.triangleCount(); ++t) {
            const StrandRoot r{static_cast<std::int32_t>(t), 0.25f, 0.3f};
            const StrandRoot w{static_cast<std::int32_t>(at + t), 0.25f, 0.3f};
            EXPECT_EQ(refined->position(w), own.position(r)) << t;
        }
    };
    expectSame(*head, 0);
    expectSame(*earsRefined, head->triangleCount());
    expectSame(*browsRefined, head->triangleCount() + earsRefined->triangleCount());
    // A root on the cage lands on its scalp's refinement and back.
    const auto earsPlan = SubdivisionPlan::of(*ears, 2);
    for (const StrandRoot onCage : {StrandRoot{3, 0.2f, 0.3f}, StrandRoot{static_cast<std::int32_t>(head->triangleCount()) + 4, 0.1f, 0.6f},
                                    StrandRoot{static_cast<std::int32_t>(joined->triangleCount()) - 1, 0.3f, 0.3f}}) {
        const StrandRoot on = plan->refinedRoot(onCage);
        ASSERT_TRUE(refined->contains(on));
        const StrandRoot back = plan->cageRoot(on);
        EXPECT_EQ(back.face, onCage.face);
        EXPECT_NEAR(back.u, onCage.u, 1e-5f);
        EXPECT_NEAR(back.v, onCage.v, 1e-5f);
    }
    EXPECT_EQ(plan->refinedRoot({3, 0.2f, 0.3f}).face, 3);  // the head is not refined
    const StrandRoot onEars = plan->refinedRoot({static_cast<std::int32_t>(head->triangleCount()) + 4, 0.1f, 0.6f});
    const StrandRoot alone = earsPlan->refinedRoot({4, 0.1f, 0.6f});
    EXPECT_EQ(onEars.face, alone.face + static_cast<std::int32_t>(head->triangleCount()));
    // The pieces: each at its own level.
    const auto pieces = scalpPieces(refined);
    ASSERT_EQ(pieces.size(), 3u);
    EXPECT_EQ(pieces.at("scalp")->subdivisionLevel(), 0);
    EXPECT_EQ(pieces.at("scalp")->triangleCount(), head->triangleCount());
    EXPECT_EQ(pieces.at("ears")->subdivisionLevel(), 2);
    EXPECT_EQ(pieces.at("ears")->points(), earsRefined->points());
    EXPECT_EQ(pieces.at("brows")->subdivisionLevel(), 1);
    EXPECT_EQ(pieces.at("brows")->triangleCount(), browsRefined->triangleCount());
    // Vertex values refined per scalp.
    std::vector<float> values(levelled->pointCount());
    for (std::size_t v = 0; v < values.size(); ++v) values[v] = levelled->points()[v].x;
    const std::vector<float> fine = plan->refineVertexValues(values, 1);
    ASSERT_EQ(fine.size(), refined->pointCount());
    for (std::size_t v = 0; v < fine.size(); ++v) EXPECT_NEAR(fine[v], refined->points()[v].x, 0.35f);
}

}  // namespace
}  // namespace groomlab
