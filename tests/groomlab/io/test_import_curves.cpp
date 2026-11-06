// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

#include "../core/test_helpers.h"
#include "groomlab/alembic_io.h"
#include "groomlab/alembic_operators.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"

namespace groomlab {
namespace {

std::string tempPath(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

// Guides combed +X, written tip-first (tip at y=1, root at y=0) so rooting must flip them.
StrandSet tipFirstGuides(std::size_t count, float lift) {
    StrandSet s;
    for (std::size_t i = 0; i < count; ++i) {
        const float z = 0.2f + 0.6f * static_cast<float>(i) / static_cast<float>(count);
        s.addStrand(i + 1, {{0.5f + lift, 1.0f, z}, {0.5f, 0.5f, z}, {0.5f, 0.0f, z}});
    }
    return s;
}

void writeGuides(const std::string& path, const std::vector<StrandSet>& frames) {
    AlembicCurvesWriter writer(path, "guides", 1.0, 1.0);  // frames at t = 1, 2, ...
    for (const StrandSet& f : frames) writer.writeFrame(f);
}

struct ImportGraph {
    Graph graph;
    ExternalInputs externals;
    NodeId scalp, import;

    ImportGraph(const std::string& path, bool withScalp) {
        registerAlembicOperators(OperatorRegistry::instance());
        externals.set("scalp", test::makePlane(8), 1);
        scalp = graph.addNode(op::kScalp);
        import = graph.addNode(op::kImportCurves);
        graph.node(import).params().set("path", path);
        if (withScalp) graph.connect(scalp, import, 0);
    }

    GuidesPtr eval(double seconds) {
        EvalContext ctx;
        ctx.time = seconds;
        ctx.externals = &externals;
        return valueAs<GuidesPtr>(graph.evaluate(import, ctx));
    }
};

TEST(ImportCurves, ReadsNearestFrameAndResamples) {
    const std::string path = tempPath("groomlab_guides_anim.abc");
    writeGuides(path, {tipFirstGuides(3, 0.0f), tipFirstGuides(3, 0.25f)});

    ImportGraph g(path, false);
    const GuidesPtr a = g.eval(1.0);
    ASSERT_TRUE(a);
    ASSERT_EQ(a->size(), 3u);
    EXPECT_EQ(a->guides()[0].points.size(), 24u);  // default resampling
    EXPECT_EQ(a->guides()[1].id, 2u);              // strandId survives
    EXPECT_NEAR(a->guides()[0].points.front().x, 0.5f, 1e-5f);

    const GuidesPtr b = g.eval(2.1);  // nearest sample: t = 2
    EXPECT_NEAR(b->guides()[0].points.front().x, 0.75f, 1e-5f);

    g.graph.node(g.import).params().set("samples", std::int64_t{0});
    EXPECT_EQ(g.eval(1.0)->guides()[0].points.size(), 3u);  // kept as read
    std::remove(path.c_str());
}

TEST(ImportCurves, ScalpFlipsAndRootsTipFirstCurves) {
    const std::string path = tempPath("groomlab_guides_root.abc");
    writeGuides(path, {tipFirstGuides(4, 0.3f)});

    ImportGraph g(path, true);
    const GuidesPtr guides = g.eval(1.0);
    ASSERT_EQ(guides->size(), 4u);
    for (const Guide& guide : guides->guides()) {
        EXPECT_GE(guide.root.face, 0);
        EXPECT_NEAR(guide.points.front().y, 0.0f, 1e-5f);  // root end is now on the plane
        EXPECT_GT(guide.points.back().y, 0.9f);
    }
    std::remove(path.c_str());
}

TEST(ImportCurves, RewritingTheFileInvalidatesTheCache) {
    const std::string path = tempPath("groomlab_guides_rewrite.abc");
    writeGuides(path, {tipFirstGuides(2, 0.0f)});
    ImportGraph g(path, false);
    EXPECT_EQ(g.eval(1.0)->size(), 2u);

    writeGuides(path, {tipFirstGuides(5, 0.0f)});
    EXPECT_EQ(g.eval(1.0)->size(), 5u);
    std::remove(path.c_str());
}

TEST(ImportCurves, ObjectFilterAndErrors) {
    const std::string path = tempPath("groomlab_guides_filter.abc");
    writeGuides(path, {tipFirstGuides(2, 0.0f)});
    ImportGraph g(path, false);

    g.graph.node(g.import).params().set("object", std::string("guides"));
    EXPECT_EQ(g.eval(1.0)->size(), 2u);
    g.graph.node(g.import).params().set("object", std::string("elsewhere"));
    EXPECT_EQ(g.eval(1.0)->size(), 0u);

    g.graph.node(g.import).params().set("path", std::string());
    EXPECT_FALSE(g.eval(1.0));  // unset path behaves like no guides

    g.graph.node(g.import).params().set("path", tempPath("groomlab_missing.abc"));
    EXPECT_THROW(g.eval(1.0), GraphError);
    std::remove(path.c_str());
}

}  // namespace
}  // namespace groomlab
